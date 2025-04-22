/** @file
 *  @brief Implementation of fru_setfield()
 *
 *  @copyright
 *  Copyright (C) 2016-2024 Alexander Amelkin <alexander@amelkin.msk.ru>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later OR Apache-2.0
 */

#include <ctype.h>
#include <errno.h>
#include <string.h>

//#define DEBUG
#include "fru-private.h"
#include "../fru_errno.h"

/** Convert a hex string to a binary byte array, report resulting size
 *
 * Requires an even count of hex digits. Will silently ignore an odd trailing digit.
 * Ignores separators: space, tab, dot, colon, dash.
 *
 * Separators may be placed only between bytes, not between nibbles. That is:
 * "12-45" is ok, "1-2-4-5" is not.
 *
 * When called with out==NULL, calculates the size of the required resulting binary buffer.
 */
bool fru__hexstr2bin(void * out,
                     size_t * outsize,
                     fru__hex_mode_t hex_mode,
                     const char * s)
{
	const char * ptr = s;
	size_t size = 0;

	DEBUG("Converting hex string at %p to bin @ %p", s, out);
	DEBUG("Size limit specified is %zu bytes (up to %p)", *outsize, out + *outsize);

	/* If *outsize value is specified, then treat it
	 * as the size limit for the output buffer */
	while (ptr[0] && ptr[1]
	       && (0 == *outsize || size < *outsize))
	{
		switch (ptr[0]) {
			case ' ':
			case '\t':
			case ':':
			case '-':
			case '.':
				// Skip separators if allowed by hex_mode
				if (hex_mode == FRU__HEX_STRICT)
					break; // fru_hex2byte will fail
				ptr++;
				continue;
			default:
				break;
		}
		int c = fru_hex2byte(ptr);
		if (c < 0)
			return false;
		if (out) {
			DEBUG("Writing a byte 0x%02hhX to %p", (char)c, out + size);
			((char *)(out + size))[0] = (char)c;
		}
		size++;
		ptr += 2;
	}
	DEBUG("Done converting at %p", out + *outsize);
	if (*outsize && size == *outsize && ptr[0]) {
		// Indicate truncation, don't bail out
		fru__seterr(FE2BIG, FERR_LOC_GENERAL, -1);
	}
	else if (ptr[0]) {
		fru__seterr(FENOTEVEN, FERR_LOC_GENERAL, -1);
		return false;
	}
	*outsize = size;
	return true;
}

/** Validate and encode the input hex string into the output buffer as binary.
 *
 * If \a out is not NULL, puts the encoded data into that buffer along with
 * the leading type/length byte.
 *
 * If the input string was truncated to fit, then \ref fru_errno is set
 * to \ref FE2BIG. Set \ref fru_errno to 0 before this call to avoid
 * false positive checks for input truncation.
 *
 * If \a out is NULL, then the function just validates the input string.
 *
 * @param[out] out The encoded field pointer, can be NULL
 * @param[in] hex_mode Specify whether or not to ignore delimiters
 * @param[in] s Input string
 *
 * @returns Success status
 * @retval true Encoded/validated successfully
 * @retval false There was an error, fru_errno is set accordingly
 */
static
bool encode_binary(fru__file_field_t * out,
                   fru__hex_mode_t hex_mode,
                   const char * s)
{
	uint8_t data[FRU__FIELDMAXLEN];
	size_t outsize = sizeof(data);

	if (!fru__hexstr2bin(data, &outsize, hex_mode, s)) {
		return false;
	}

	if (out) {
		out->typelen = FRU__TYPELEN(BINARY, outsize);
		memcpy(out->data, data, outsize);
	}

	return true;
}

/** Validate and encode the input string into the output buffer as 6-bit ASCII
 *
 * If \a out is not NULL, puts the encoded data into that buffer along with
 * the leading type/length byte.
 *
 * If the input string was truncated to fit, then \ref fru_errno is set
 * to \ref FE2BIG. Set \ref fru_errno to 0 before this call to avoid
 * false positive checks for input truncation.
 *
 * If \a out is NULL, then the function just validates the input string.
 *
 * @param[out] out The encoded field pointer, can be NULL
 * @param[in] s Input string
 *
 * @returns Success status
 * @retval true Encoded/validated successfully
 * @retval false There was an error, fru_errno is set accordingly
 */
static
bool encode_6bit(fru__file_field_t * out,
                 fru__hex_mode_t hex_mode __attribute__((__unused__)),
                 const char * s)
{
	size_t len = strlen(s);
	size_t len6bit = FRU__6BIT_LENGTH(len);
	size_t i, i6;

	if (len6bit > FRU__FIELDLEN(len6bit)) {
		// Indicate truncation, don't bail out
		fru__seterr(FE2BIG, FERR_LOC_GENERAL, -1);
		len6bit = FRU__FIELDLEN(len6bit); // Truncate to fit
	}

	for (i = 0, i6 = 0; i < len && i6 < len6bit; i++) {
		// Four original bytes get encoded into three 6-bit-packed ones
		int byte = i % 4;
		char c = (s[i] - FRU__6BIT_BASE);

		if (c > FRU__6BIT_MAXVALUE) {
			fru__seterr(FERANGE, FERR_LOC_GENERAL, -1);
			return false;
		}

		c &= FRU__6BIT_MAXVALUE;

		switch(byte) {
			case 0:
				out->data[i6] = c; // The whole 6-bit char goes low into byte 0
				break;
			case 1:
				out->data[i6] |= (c & 0x03) << 6; // Lower 2 bits go high into byte 0
				out->data[++i6] = c >> 2; // Higher (4) bits go low into byte 1
				break;
			case 2:
				out->data[i6++] |= c << 4; // Lower 4 bits go high into byte 1
				out->data[i6] = c >> 4; // Higher 2 bits go low into byte 2
				break;
			case 3:
				out->data[i6++] |= c << 2; // The whole 6-bit char goes high into byte 3
				break;
		}
	}

	if (out)
		out->typelen = FRU__TYPELEN(ASCII_6BIT, len6bit);

	return true;
}

/** Validate and encode the input string into the output buffer as BCD+
 *
 * If \a out is not NULL, puts the encoded data into that buffer along with
 * the leading type/length byte.
 *
 * If the input string was truncated to fit, then \ref fru_errno is set
 * to \ref FE2BIG. Set \ref fru_errno to 0 before this call to avoid
 * false positive checks for input truncation.
 *
 * If \a out is NULL, then the function just validates the input string.
 *
 * @param[out] out The encoded field pointer, can be NULL
 * @param[in] s Input string
 *
 * @returns Success status
 * @retval true Encoded/validated successfully
 * @retval false There was an error, fru_errno is set accordingly
 */
static
bool encode_bcdplus(fru__file_field_t * out,
                    fru__hex_mode_t hex_mode __attribute__((__unused__)),
                    const char * s)
{
	size_t len = strlen(s);
	size_t lenbcd = (len + 1) / 2; /* Need an extra byte for a lone trailing nibble */
	uint8_t c[2] = { 0 };
	size_t i = 0;

	if (lenbcd > FRU__FIELDLEN(lenbcd))
	{
		// Indicate truncation, don't bail out
		fru__seterr(FE2BIG, FERR_LOC_GENERAL, -1);
		lenbcd = FRU__FIELDLEN(lenbcd); // Truncate to fit
	}

	/* Copy the data and pack it as BCD */
	for (; i < len; i++) {
		switch(s[i]) {
			case 0:
				// The null-terminator encountered earlier than
				// the end of the BCD field, encode as space
			case ' ':
				c[i % 2] = 0xA;
				break;
			case '-':
				c[i % 2] = 0xB;
				break;
			case '.':
				c[i % 2] = 0xC;
				break;
			default: // Digits
				if (!isdigit(s[i])) {
					fru__seterr(FERANGE, FERR_LOC_GENERAL, -1);
					return false;
			    }
				c[i % 2] = s[i] - '0';
		}
		if (out)
			out->data[i / 2] = c[0] << 4 | c[1];
	}

	if (out)
		out->typelen = FRU__TYPELEN(BCDPLUS, lenbcd);

	return true;
}

/** Validate and encode the input string into the output buffer as plain text
 *
 * If \a out is not NULL, puts the encoded data into that buffer along with
 * the leading type/length byte.
 *
 * If the input string was truncated to fit, then \ref fru_errno is set
 * to \ref FE2BIG. Set \ref fru_errno to 0 before this call to avoid
 * false positive checks for input truncation.
 *
 * If \a out is NULL, then the function just validates the input string.
 *
 * @param[out] out The encoded field pointer, can be NULL
 * @param[in] s Input string
 *
 * @returns Success status
 * @retval true Encoded/validated successfully
 * @retval false There was an error, fru_errno is set accordingly
 */
static
bool encode_text(fru__file_field_t * out,
                 fru__hex_mode_t hex_mode __attribute__((__unused__)),
                 const char * s)
{
	size_t len = strlen(s);

	fru_clearerr();
	if (len > FRU__FIELDLEN(len)) {
		// Indicate truncation, don't bail out
		fru__seterr(FE2BIG, FERR_LOC_GENERAL, -1);
		len = FRU__FIELDLEN(len); // Truncate to fit
	}

	if (out) {
		out->typelen = FRU__TYPELEN(TEXT, len);
		// For TEXT encoding length 1 means "end-of-fields",
		// so we increment the length and put the nul-byte there
		if (out->typelen == FRU__FIELD_TERMINATOR)
			out->typelen++;

		// We don't want the nul-byte in the destination
		// unless it's a single-byte string
		for (size_t i = 0; i < FRU__FIELDLEN(out->typelen); i++) {
			if (!isprint(s[i])) {
				fru__seterr(FENONPRINT, FERR_LOC_GENERAL, -1);
				return false;
			}
			out->data[i] = s[i];
		}
	}

	return true;
}

bool fru__encode_field(fru__file_field_t * out_field,
                       fru_field_enc_t encoding,
                       const char * s)
{
	bool rc = false;
	uint8_t buf[FRU__FIELDMAXLEN + 1]; // Type/Length byte included
	fru__file_field_t * local_outfield = (fru__file_field_t *)buf;
	fru_field_enc_t auto_encs[FRU_FE_REALCOUNT] = {
		// List of real encodings in the order they will be tried.
		// Try 6-bit ASCII first as it's the most compressed format,
		// them try other formats in ascending character range size order
		FRU_FE_6BITASCII, // Uppercase ASCII, digits, punctuation
		FRU_FE_BCDPLUS, // [0-9, \-]
		FRU_FE_BINARY, // [0-9,a-f,A-F]
		FRU_FE_TEXT // Any ASCII text
	};
	bool (* encode[FRU_FE_REALCOUNT])(fru__file_field_t *,
	                                  fru__hex_mode_t,
	                                  const char *) =
	{
		[FRU_REAL_FE(FRU_FE_BINARY)] = encode_binary,
		[FRU_REAL_FE(FRU_FE_BCDPLUS)] = encode_bcdplus,
		[FRU_REAL_FE(FRU_FE_6BITASCII)] = encode_6bit,
		[FRU_REAL_FE(FRU_FE_TEXT)] = encode_text,
	};

	if (!s) {
		fru__seterr(FEGENERIC, FERR_LOC_GENERAL, -1);
		errno = EFAULT;
		goto out;
	}

	if (FRU_FE_EMPTY == encoding) {
		s = "";
		encoding = FRU_FE_TEXT;
	}
	else if (FRU_FE_AUTO != encoding && !FRU_FE_IS_REAL(encoding)) {
		fru__seterr(FEBADENC, FERR_LOC_GENERAL, 0);
		goto out;
	}
	DEBUG("Going to start with encoding %d", encoding);

	if (!strlen(s)) {
		// Empty field, don't attempt to encode
		local_outfield->typelen = FRU__FIELD_EMPTY;
	}
	else {
		// Can't use `encoding` in any way as an index into `auto_encs`
		// as the encodings there are out of standard order.
		// Hence, we can't use the same loop below both for a specific
		// endoding and for automatic, so we have to repeat the
		// call to encode[]():
		if (FRU_FE_IS_REAL(encoding)) {
			// For specific encodings use relaxed hex mode
			if (!encode[FRU_REAL_FE(encoding)](local_outfield, FRU__HEX_RELAXED, s))
				goto out;
		}
		// Try to encode using most to least restrictive encodings within
		// the selected range
		else {
			size_t i = 0;
			for (; i < FRU_FE_REALCOUNT; i++) {
				// For automatic selection of encdong we must use strict hex
				// mode to prevent possible delimiters from affecting the detection
				if (encode[FRU_REAL_FE(auto_encs[i])](local_outfield, FRU__HEX_STRICT, s))
					break;
			}

			if (i >= FRU_FE_REALCOUNT) {
				fru__seterr(FEAUTOENC, FERR_LOC_GENERAL, 0);
				goto out;
			}
		}

	}

	if (out_field) {
		memcpy(out_field, local_outfield,
		       FRU__FIELDSIZE(local_outfield->typelen));
	}

	rc = true;
out:
	return rc;
}

// See fru.h
bool fru_setfield(fru_field_t * field,
                  fru_field_enc_t encoding,
                  const char * s)
{
	bool rc = false;
	uint8_t buf[FRU__FIELDMAXLEN + 1]; // Type/Length byte included
	fru__file_field_t * local_outfield = (fru__file_field_t *)buf;

	DEBUG("Setting field at %p", field);

	if (encoding == FRU_FE_PRESERVE) {
		if (!FRU_FIELD_IS_REAL_ENC(field)) {
			// Can't preserve encoding if there is none or it is a meta-type,
			// so use automatic detection
			field->enc = FRU_FE_AUTO;
		}
		encoding = field->enc;
	}

	if (!fru__encode_field(local_outfield, encoding, s)) {
		goto out;
	}

	/* The string appears to be encodable, copy it to the field structure */
	if (field) {
		fru__decode_field(field, local_outfield);
		DEBUG("New value is \"%s\"", field->val);
	}

	rc = true;
out:
	return rc;
}
