/** @file
 *  @brief Implementation of binary FRU field decoders
 *
 *  @copyright
 *  Copyright (C) 2016-2025 Alexander Amelkin <alexander@amelkin.msk.ru>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later OR Apache-2.0
 */

#include <assert.h>
#include <errno.h>
#include <string.h>

#include "fru-private.h"
#include "../fru_errno.h"

/**
 * Strip trailing spaces
 */
static inline
void cut_tail(char *s)
{
	int i;
	for(i = strlen((char *)s) - 1; i >= 0 && ' ' == s[i]; i--) s[i] = 0;
}

/**
 * @brief Get a hex string representation of the supplied binary field.
 *
 * @param[in] field Field to decode.
 * @param[out] out Pointer to the decoded field structure
 * @retval true Success.
 * @retval false Failure.
 */
static
void decode_binary(fru_field_t *out,
                   const fru__file_field_t *field)
{
	assert(field);
	assert(out);

	fru__decode_raw_binary(field->data,
	                       FRU__FIELDLEN(field->typelen),
	                       out->val,
	                       sizeof(out->val));
}

/**
 * @brief Decode BCDPLUS string.
 *
 * @param[out] out Pointer to the decoded field structure
 * @param[in] field Field to decode.
 * @retval true Success.
 * @retval false Failure.
 */
static
void decode_bcdplus(fru_field_t *out,
                    const fru__file_field_t *field)
{
	size_t i;
	uint8_t c;
	size_t len;

	assert(field);
	assert(out);

	len = 2 * FRU__FIELDLEN(field->typelen);
	/* Need space for nul-byte */
	/* This can never actually happen as fru_field_t.val is fixed size */
	assert(sizeof(out->val) >= len + 1);

	/* Copy the data and pack it as BCD */
	for (i = 0; i < len; i++) {
		c = (field->data[i / 2] >> ((i % 2) ? 0 : 4)) & 0x0F;
		switch (c) {
		case 0xA:
			out->val[i] = ' ';
			break;
		case 0xB:
			out->val[i] = '-';
			break;
		case 0xC:
			out->val[i] = '.';
			break;
		case 0xD:
		case 0xE:
		case 0xF:
			out->val[i] = '?';
			break;
		default: // Digits
			out->val[i] = c + '0';
		}
	}
	out->val[len] = 0; // Terminate the string
	// Strip trailing spaces that may have emerged when a string of odd
	// length was BCD-encoded.
	cut_tail(out->val);
	DEBUG("BCD+ string of length %zd decoded: '%s'", len, out->val);
}

/**
 * @brief Decode a 6-bit ASCII string.
 *
 * @param[out] out Buffer to decode into.
 * @param[in] field Field to decode.
 * @retval true Success.
 * @retval false Failure.
 */
static
void decode_6bit(fru_field_t *out,
                 const fru__file_field_t *field)
{
	const uint8_t *s6;
	size_t len, len6bit;
	size_t i, i6;

	assert(field);
	assert(out);

	len6bit = FRU__FIELDLEN(field->typelen);
	s6 = field->data;

	len = FRU__6BIT_FULLLENGTH(len6bit);
	/* Need space for nul-byte */
	/* This can never actually happen as fru_field_t.val is fixed size */
	assert(sizeof(out->val) >= len + 1);

	for(i = 0, i6 = 0; i6 <= len6bit && i < len && s6[i6]; i++) {
		int byte = i % 4;

		switch(byte) {
			case 0:
				out->val[i] = s6[i6] & FRU__6BIT_MAXVALUE;
				break;
			case 1:
				out->val[i] = (s6[i6] >> 6) | (s6[i6 + 1] << 2);
				++i6;
				break;
			case 2:
				out->val[i] = (s6[i6] >> 4) | (s6[i6 + 1] << 4);
				++i6;
				break;
			case 3:
				out->val[i] = s6[i6++] >> 2;
				break;
		}
		out->val[i] &= FRU__6BIT_MAXVALUE;
		out->val[i] += FRU__6BIT_BASE;
	}

	// Strip trailing spaces that could emerge when decoding a
	// string that was a byte shorter than a multiple of 4.
	cut_tail(out->val);
	DEBUG("6bit ASCII string of length %zd decoded: '%s'", len, out->val);
}

/**
 * @brief Copy data from encoded fru__file_field_t into a plain text buffer
 *
 * @param[out] out Pointer to the decoded field structure
 * @param[in] field Field to decode.
 * @retval true Success.
 * @retval false Failure (buffer too short)
 */
static
void decode_text(fru_field_t *out,
                 const fru__file_field_t *field)
{
	assert(field);
	assert(out);

	size_t len = FRU__FIELDLEN(field->typelen);
	/* Need space for nul-byte */
	/* This can never actually happen as fru_field_t.val is fixed size */
	assert(sizeof(out->val) >= len + 1);

	if (len) {
		// TODO: Potentially the input fru data may contain something
		//       non-printable. It would be nice to detect and report
		//       that on the library level. A flag to ignore the error
		//       would also be needed in that case.
		memcpy(out->val, field->data, len);
	}
	out->val[len] = 0; /* Terminate the string */
	DEBUG("text string of length %zd decoded: '%s'", len, out->val);
}

/**
 * Decode data from a buffer into another buffer.
 *
 * For binary data use FRU__FIELDLEN(field->typelen) to find
 * out the size of valid bytes in the returned buffer.
 *
 * @param[out] out Decoded field.
 * @param[in] field Encoded data field.
 * @retval true Success.
 * @retval false Failure.
 */
bool fru__decode_field(fru_field_t *out,
                  const fru__file_field_t *field)
{
	fru_field_enc_t enc;
	void (*decode[FRU_FE_REALCOUNT])(fru_field_t *,
	                                  const fru__file_field_t *) =
	{
		[FRU_REAL_FE(FRU_FE_BINARY)] = decode_binary,
		[FRU_REAL_FE(FRU_FE_BCDPLUS)] = decode_bcdplus,
		[FRU_REAL_FE(FRU_FE_6BITASCII)] = decode_6bit,
		[FRU_REAL_FE(FRU_FE_TEXT)] = decode_text,
	};

	if (!field) {
		// Area and item will be adjusted by the caller
		fru__seterr(FEGENERIC, FERR_LOC_GENERAL, -1);
		errno = EFAULT;
		return false;
	}

	enc = FRU__FIELD_ENC_T(field->typelen);

	if (!FRU_FE_IS_REAL(enc)) {
		DEBUG("ERROR: Field encoding type is invalid (%d)", enc);
		// Area and item will be adjusted by the caller
		fru__seterr(FEBADENC, FERR_LOC_GENERAL, -1);
		return false;
	}

	if (field->typelen != FRU__FIELD_TERMINATOR) {
		DEBUG("The encoded field is marked as type %d, length is %zi (typelen %02x)",
		      enc, FRU__FIELDLEN(field->typelen), field->typelen);
	}
	else {
		DEBUG("End-of-fields reached");
	}

	memset(out, 0, sizeof(*out));
	out->enc = enc;
	decode[FRU_REAL_FE(enc)](out, field);
	return true;
}

