/** @file
 *  @brief Implementation of fru_setfield_binary()
 *
 *  @copyright
 *  Copyright (C) 2016-2024 Alexander Amelkin <alexander@amelkin.msk.ru>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later OR Apache-2.0
 */

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <string.h>

//#define DEBUG
#include "fru-private.h"
#include "../fru_errno.h"

/**
 * Convert 4 binary bits (a nibble) into a hex digit character
 */
static inline
uint8_t nibble2hex(char n)
{
	return (n > 9 ? n - 10 + 'A': n + '0');
}

// See fru-private.h
void fru__byte2hex(void *buf, char byte)
{
	uint8_t *str = buf;
	if (!str) return;

	str[0] = nibble2hex(((byte & 0xf0) >> 4) & 0x0f);
	str[1] = nibble2hex(byte & 0x0f);
	str[2] = 0;
}

// See fru-private.h
void fru__decode_raw_binary(const void *in,
                            size_t in_len,
                            char *out,
                            size_t out_len)
{
	size_t i;
	const char *buffer = in;

	assert(in_len * 2 + 1 <= out_len);

	/* byte2hex() automatically terminates the string */
	for (i = 0; i < in_len; i++) {
		fru__byte2hex(out + 2 * i, buffer[i]);
	}
}


// See fru.h
bool fru_setfield_binary(fru_field_t * field,
                         const void * buf,
                         size_t size)
{
	bool rc = false;

	if (!field || !buf) {
		fru__seterr(FEGENERIC, FERR_LOC_CALLER, -1);
		errno = EFAULT;
		goto out;
	}

	if (!size) {
		fru__seterr(FEGENERIC, FERR_LOC_CALLER, -1);
		errno = EINVAL;
		goto out;
	}

	// Truncate input to fit
	size_t insize = FRU_MIN(size, FRU__FIELDMAXLEN);
	// Allocate a buffer for the resulting hex string.
	// Each input byte turns into two, plus the NUL terminator byte
	uint8_t * hexstr = calloc(1, insize * 2 + 1);
	if (!hexstr) {
		fru__seterr(FEGENERIC, FERR_LOC_GENERAL, -1);
		errno = EFAULT;
		goto out;
	}

	fru__decode_raw_binary(buf, size, field->val, insize * 2 + 1);
	field->enc = FRU_FE_BINARY;
	rc = true;
	if (insize < size)
		fru_errno.code = FE2BIG;
out:
	return rc;
}
