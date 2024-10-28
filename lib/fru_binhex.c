/** @file
 *  @brief Implementation of bin to hex conversion functions
 *
 *  @copyright
 *  Copyright (C) 2016-2025 Alexander Amelkin <alexander@amelkin.msk.ru>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later OR Apache-2.0
 */
#include "fru-private.h"

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
bool fru__decode_raw_binary(const void *in,
                            size_t in_len,
                            char *out,
                            size_t out_len)
{
	size_t i;
	const char *buffer = in;

	if (in_len * 2 + 1 > out_len) {
		return false;
	}

	/* byte2hex() automatically terminates the string */
	for (i = 0; i < in_len; i++) {
		fru__byte2hex(out + 2 * i, buffer[i]);
	}

	return true;
}

