/** @file
 *  @brief Implementation internal use area manipulation functions
 *
 *  @copyright
 *  Copyright (C) 2016-2024 Alexander Amelkin <alexander@amelkin.msk.ru>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later OR Apache-2.0
 */

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>

//#define DEBUG
#include "fru-private.h"
#include "../fru_errno.h"

/**
 * Copy a hex string without delimiters
 *
 * Copies the source \a hexstr into the destination pointed to by \a outhexstr,
 * performing sanity check and skipping delimiters. Delimiters include:
 * space, dot, colon, and dash.
 *
 * \b Example:
 * ```
 * "11 22 33 44 55"
 * "DE-AD-C0-DE"
 * "13:37:C0:DE:BA:BE"
 * "45.67.89.AB"
 * ```
 *
 * The source string must contain only hex digits and delimiters, nothing more.
 * If anything else is found, the function aborts and no modifications to
 * \a outhexstring are performed.
 *
 * Allocates and reallocates (resizes) \a outhexstrg as necessary.
 *
 * @returns Success status
 * @retval true Data copied successfully
 * @retval false An error occured, \a outhexstr unmodified, see \ref fru_errno
 */
static
bool hexcopy(char ** outhexstr, const char * hexstr)
{
	size_t i;
	size_t len = 0;
	char *newhexstr;
	if (!outhexstr || !hexstr) {
		fru_errno = FEGENERIC;
		errno = EFAULT;
		return false;
	}
	newhexstr = *outhexstr;

	// Check input string sanity, skip delimiters
	for (i = 0; hexstr[i]; i++) {
		if (!isxdigit(hexstr[i])) {
			switch (hexstr[i]) {
			case ' ':
			case '-':
			case ':':
			case '.':
				continue;
			default:
				fru_errno = FENONHEX;
				return false;
			}
		}
		else {
			len++;
		}
	}

	if (len % 2) {
		fru_errno = FENOTEVEN;
	}

	newhexstr = realloc(newhexstr, len + 1);
	if (!newhexstr) {
		fru_errno = FEGENERIC;
		return false;
	}

	*outhexstr = newhexstr;
	for (i = 0, len = 0; hexstr[i]; i++)
		switch (hexstr[i]) {
		case ' ':
		case '-':
		case ':':
		case '.':
			continue;
		default:
			newhexstr[len++] = hexstr[i];
		}

	return true;
}

// See fru.h
bool fru_set_internal_binary(fru_t * fru,
                             const void * buffer,
                             size_t size)
{
	char *hexstring;
	/* The output is two hex digits per byte,
	 * plus an extra byte for the string terminator. */
	size_t out_len = size * 2 + 1;
	bool rc = false;

	if (!fru || !buffer) {
		fru_errno = FEGENERIC;
		errno = EFAULT;
		goto err;
	}

	hexstring = realloc(fru->internal, out_len);
	if (!hexstring) {
		fru_errno = FEGENERIC;
		goto err;
	}

	if(!fru__decode_raw_binary(buffer, size,
	                           hexstring, out_len))
	{
		zfree(hexstring);
		errno = ENOBUFS;
		fru_errno = FEGENERIC;
		goto err;
	}

	fru->internal = hexstring;
	fru_enable_area(fru, FRU_INTERNAL_USE, FRU_APOS_AUTO);
	rc = true;
err:
	return rc;
}

// See fru.h
bool fru_set_internal_hexstring(fru_t * fru, const void * hexstr)
{
	if (!fru || !hexstr) {
		fru_errno = FEGENERIC;
		errno = EFAULT;
		return false;
	}

	/* Don't touch presence flag if copying fails,
	 * old data are preserved */
	if(!hexcopy(&fru->internal, hexstr))
		return false;

	fru_enable_area(fru, FRU_INTERNAL_USE, FRU_APOS_AUTO);
	return true;
}

bool fru_delete_internal(fru_t * fru)
{
	if (!fru) {
		fru_errno = FEGENERIC;
		errno = EFAULT;
		return false;
	}

	if (!fru->present[FRU_INTERNAL_USE]) {
		fru_errno = FEADISABLED;
		return false;
	}

	fru->present[FRU_INTERNAL_USE] = false;
	zfree(fru->internal);

	return true;
}
