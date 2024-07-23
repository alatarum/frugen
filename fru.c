/** @file
 *  @brief FRU information encoding functions
 *
 *  Copyright (C) 2016-2023 Alexander Amelkin <alexander@amelkin.msk.ru>
 *  SPDX-License-Identifier: LGPL-2.0-or-later OR Apache-2.0
 */

#include "fru.h"
#include "smbios.h"

#include <ctype.h>
#include <stdio.h>
#include <stdint.h>
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <inttypes.h>

#define _BSD_SOURCE
#include <endian.h>

#ifdef __STANDALONE__
#include <stdio.h>
#endif

#ifdef DEBUG
#undef DEBUG
#include <stdio.h>
#define DEBUG(f, args...) do { \
	typeof(errno) err = errno; \
	printf("%s:%d: ", __func__, __LINE__); \
	errno = err; /* Allow 'f' to use "%m" */ \
	printf(f,##args); \
	errno = err; \
} while(0)
#else
#define DEBUG(f, args...)
#endif

static bool autodetect = true;

const char* enc_names[TOTAL_FIELD_TYPES] = {
	[FIELD_TYPE_AUTO] = "auto",
	[FIELD_TYPE_BINARY] = "binary",
	[FIELD_TYPE_BCDPLUS] = "bcdplus",
	[FIELD_TYPE_SIXBITASCII] = "6bitascii",
	[FIELD_TYPE_TEXT] = "text"
};

/*
 * Minimum and maximum lengths of values as per
 * Table 18-6, Management Access Record
 */
static const size_t fru_mr_mgmt_minlen[FRU_MR_MGMT_MAX] = {
	[MGMT_TYPENAME_ID(SYS_URL)] = 16,
	[MGMT_TYPENAME_ID(SYS_NAME)] = 8,
	[MGMT_TYPENAME_ID(SYS_PING)] = 8,
	[MGMT_TYPENAME_ID(COMPONENT_URL)] = 16,
	[MGMT_TYPENAME_ID(COMPONENT_NAME)] = 8,
	[MGMT_TYPENAME_ID(COMPONENT_PING)] = 8,
	[MGMT_TYPENAME_ID(SYS_UUID)] = 16
};

static const size_t fru_mr_mgmt_maxlen[FRU_MR_MGMT_MAX] = {
	[MGMT_TYPENAME_ID(SYS_URL)] = 256,
	[MGMT_TYPENAME_ID(SYS_NAME)] = 64,
	[MGMT_TYPENAME_ID(SYS_PING)] = 64,
	[MGMT_TYPENAME_ID(COMPONENT_URL)] = 256,
	[MGMT_TYPENAME_ID(COMPONENT_NAME)] = 256,
	[MGMT_TYPENAME_ID(COMPONENT_PING)] = 64,
	[MGMT_TYPENAME_ID(SYS_UUID)] = 16
};

void fru_set_autodetect(bool enable)
{
	autodetect = enable;
}

static inline
size_t strulen(const uint8_t *data)
{
	return strlen((const char *)data);
}

/**
 * Get the FRU date/time base in seconds since UNIX Epoch
 *
 * According to IPMI FRU Information Storage Definition v1.0, rev 1.3,
 * the date/time encoded as zero designates "0:00 hrs 1/1/96",
 * see Table 11-1 "BOARD INFO AREA"
 *
 * @returns The number of seconds from UNIX Epoch to the FRU date/time base
 */
static
time_t fru_datetime_base() {
	struct tm tm_1996 = {
		.tm_year = 96,
		.tm_mon = 0,
		.tm_mday = 1
	};
	// The argument to mktime is zoneless
	return mktime(&tm_1996);
}


/**
 * Strip trailing spaces
 */
static inline
void cut_tail(uint8_t *s)
{
	int i;
	for(i = strlen((char *)s) - 1; i >= 0 && ' ' == s[i]; i--) s[i] = 0;
}

/** Copy a FRU area field to a buffer and return the field's size */
static inline
uint8_t fru_field_copy(void *dest, const fru_field_t *fieldp)
{
	memcpy(dest, (void *)fieldp, FRU_FIELDSIZE(fieldp->typelen));
	return FRU_FIELDSIZE(fieldp->typelen);
}

/**
 * Detect the most suitable encoding for the string and calculate
 * the length as well
 *
 * @returns A FRU field type/length byte, as per IPMI FRU Storage Definition,
 *          if everything was ok, or an error code.
 * @retval FRU_FIELD_EMPTY The \a data argument was NULL
 * @retval FRU_FIELD_TERMINATOR The data exceeded the maximum length (63 bytes)
 *
 */
static
uint8_t fru_get_typelen(int len, /**< [in] Length of the data, LEN_AUTO for pure
                                           text zero-terminated data or one of
                                           LEN_BCDPLUS, LEN_6BITASCII, LEN_TEXT
                                           for explicit text type */
                        const uint8_t *data) /**< [in] The input data */
{
	uint8_t typelen = len;
	int i;

	if (!data)
		return FRU_FIELD_EMPTY;

	if (len < 0) {
		DEBUG("Forcing string '%s' to ...\n", (char *)data);
		// Explicit text type
		switch (len) {
		case LEN_BCDPLUS:
			DEBUG("BCDPLUS type\n");
			return FRU_TYPELEN(BCDPLUS, (strulen(data) + 1) / 2);
		case LEN_6BITASCII:
			DEBUG("6BIT ASCII type\n");
			return FRU_TYPELEN(ASCII_6BIT, FRU_6BIT_LENGTH(strulen(data)));
		case LEN_TEXT:
			DEBUG("ASCII type\n");
			return FRU_TYPELEN(TEXT, strulen(data));
		default:
			DEBUG("Nothing... Unknown text type\n");
			return FRU_FIELD_TERMINATOR;
		}
	}

	if (!len) {
		len = strulen(data);
		if (!len) {
			return FRU_FIELD_EMPTY;
		}
	}

	// If the data exceeds the maximum length, return a terminator
	if ((size_t)len > FRU_FIELDDATALEN(len)) {
		DEBUG("Data exceeds maximum length\n");
		return FRU_FIELD_TERMINATOR;
	}

	if (typelen) {
		// They gave us a non-zero length. The data must be binary.
		// Trust 'em and don't try to optimize.
		DEBUG("Binary data due to non-zero length\n");
		return FRU_TYPELEN(BINARY, len);
	}

	DEBUG("Guessing type of string '%s'...\n", (char *)data);

	// As we reach this point, we know the data must be text.
	// We will try to find the encoding that suits best.
	if (autodetect) {
		// By default - the most range-restricted text type
		typelen = FRU_TYPELEN(BCDPLUS, (len + 1) / 2);

		DEBUG("Assuming BCD plus data...\n");
	}
	else {
		DEBUG("Assuming ASCII data...\n");
		typelen = FRU_TYPELEN(TEXT, len);
	}

	// Go through the data and expand charset as needed
	for (i = 0; i < len; i++) {
		if (data[i] < ' '
		    && data[i] != '\t'
		    && data[i] != '\r'
		    && data[i] != '\n')
		{
			// They lied! The data is binary!
			// That's the widest range type.
			// There is no use in checking any further.
			DEBUG("[%#02x] Binary data!\n", data[i]);
			typelen = FRU_TYPELEN(BINARY, len);
			break;
		}

		if (autodetect) {
			if (typelen < FRU_MAKETYPE(TEXT) // Do not reduce the range
			    && (data[i] > '_' || data[i] < ' '))
			{
				// The data doesn't fit into 6-bit ASCII, expand to simple text.
				DEBUG("[%c] Data is simple text!\n", data[i]);
				typelen = FRU_TYPELEN(TEXT, len);
				continue;
			}

			if (typelen < FRU_MAKETYPE(ASCII_6BIT) // Do not reduce the range
			    && !isdigit(data[i])
			    && data[i] != ' '
			    && data[i] != '-'
			    && data[i] != '.')
			{
				// The data doesn't fit into BCD plus, expand to
				DEBUG("[%c] Data is 6-bit ASCII!\n", data[i]);
				typelen = FRU_TYPELEN(ASCII_6BIT, FRU_6BIT_LENGTH(len));
			}
		} /* autodetect */
	}

	return typelen;
}

/**
 * Allocate a buffer and encode the input string into it as 6-bit ASCII
 *
 * @returns pointer to the newly allocated field buffer if allocation and
 *          encoding were successful
 * @returns NULL if there was an error, sets errno accordingly (man malloc)
 */
static
fru_field_t *fru_encode_6bit(const unsigned char *s /**< [in] Input string */)
{
	size_t len = strulen(s);
	size_t len6bit = FRU_6BIT_LENGTH(len);
	size_t i, i6;
	fru_field_t *out = NULL;
	size_t outlen = sizeof(fru_field_t) + len6bit + 1; // 1 extra for null-byte

	if (len6bit > FRU_FIELDDATALEN(len6bit) || !(out = calloc(1, outlen)))
	{
		return out;
	}

	out->typelen = FRU_TYPELEN(ASCII_6BIT, len6bit);

	for (i = 0, i6 = 0; i < len && i6 < len6bit; i++) {
		// Four original bytes get encoded into three 6-bit-packed ones
		int byte = i % 4;
		// Space is zero, maximum is 0x3F (6 significant bits)
		char c = (s[i] - ' ') & 0x3F;

		DEBUG("%d:%zu = %c -> %02hhX\n", byte, i6, s[i], c);
		switch(byte) {
			case 0:
				out->data[i6] = c;
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

	return out;
}

/**
 * @brief Decode a 6-bit ASCII string.
 *
 * @param[in] field Field to decode.
 * @param[out] out Buffer to decode into.
 * @param[in] out_len Length of output buffer.
 * @retval true Success.
 * @retval false Failure.
 */
static
bool fru_decode_6bit(const fru_field_t *field,
		     uint8_t *out,
		     size_t out_len)
{
	const unsigned char *s6;
	size_t len, len6bit;
	size_t i, i6;

	if (!field) return false;

	len6bit = FRU_FIELDDATALEN(field->typelen);
	s6 = field->data;

	len = FRU_6BIT_FULLLENGTH(len6bit);
	if (out_len < (len + 1)) {
		return false;
	}

	for(i = 0, i6 = 0; i6 <= len6bit && i < len && s6[i6]; i++) {
		int byte = i % 4;

		DEBUG("%d:%zu = ", byte, i6);

		switch(byte) {
			case 0:
				DEBUG("%02" PRIX8, s6[i6]);
				out[i] = s6[i6] & 0x3F;
				break;
			case 1:
				DEBUG("%02" PRIX8 " %02" PRIX8, s6[i6], s6[i6 + 1]);
				out[i] = (s6[i6] >> 6) | (s6[i6 + 1] << 2);
				++i6;
				break;
			case 2:
				DEBUG("%02" PRIX8 " %02" PRIX8, s6[i6], s6[i6 + 1]);
				out[i] = (s6[i6] >> 4) | (s6[i6 + 1] << 4);
				++i6;
				break;
			case 3:
				DEBUG("%02" PRIX8, s6[i6]);
				out[i] = s6[i6++] >> 2;
				break;
		}
		out[i] &= 0x3F;
		out[i] += ' ';
		DEBUG("-> %02hhx %c\n", out[i], out[i]);
	}

	// Strip trailing spaces that could emerge when decoding a
	// string that was a byte shorter than a multiple of 4.
	cut_tail(out);

	return true;
}

/**
 * @brief Decode BCDPLUS string.
 *
 * @param[in] field Field to decode.
 * @param[out] out Buffer to decode into.
 * @param[in] out_len Length of output buffer.
 * @retval true Success.
 * @retval false Failure.
 */
static
bool fru_decode_bcdplus(const fru_field_t *field,
                        uint8_t *out,
                        size_t out_len)
{
	size_t i;
	uint8_t c;
	if (out_len < 2 * FRU_FIELDDATALEN(field->typelen) + 1)
		return false;
	/* Copy the data and pack it as BCD */
	for (i = 0; i < 2 * FRU_FIELDDATALEN(field->typelen); i++) {
		c = (field->data[i / 2] >> ((i % 2) ? 0 : 4)) & 0x0F;
		switch (c) {
		case 0xA:
			out[i] = ' ';
			break;
		case 0xB:
			out[i] = '-';
			break;
		case 0xC:
			out[i] = '.';
			break;
		case 0xD:
		case 0xE:
		case 0xF:
			out[i] = '?';
			break;
		default: // Digits
			out[i] = c + '0';
		}
	}
	out[2 * FRU_FIELDDATALEN(field->typelen)] = 0; // Terminate the string
	// Strip trailing spaces that may have emerged when a string of odd
	// length was BCD-encoded.
	cut_tail(out);

	return true;
}

static inline
uint8_t nibble2hex(uint8_t n)
{
	return (n > 9 ? n - 10 + 'A': n + '0');
}

static
void byte2hex(void *buf, uint8_t byte)
{
	uint8_t *str = buf;
	if (!str) return;

	str[0] = nibble2hex((byte & 0xf0) >> 4);
	str[1] = nibble2hex(byte & 0x0f);
	str[2] = 0;
}

/**
 * @brief Get a hex string representation of the supplied raw binary buffer.
 *
 * Also performs the check to see if the result will fit into the
 * output buffer. The output buffer must be twice as big as the input
 * plus one byte for string termination.
 *
 * @param[in] in The binary buffer to decode.
 * @param[in] in_len Length of the input data.
 * @param[out] out Buffer to decode into.
 * @param[in] out_len Length of output buffer.
 * @retval true Success.
 * @retval false Failure.
 */
static
bool fru_decode_raw_binary(const void *in,
                           size_t in_len,
                           uint8_t *out,
                           size_t out_len)
{
	size_t i;
	const uint8_t *buffer = in;

	if (in_len * 2 + 1 > out_len)
		return false;

	/* byte2hex() automatically terminates the string */
	for (i = 0; i < in_len; i++) {
		byte2hex(out + 2 * i, buffer[i]);
	}

	return true;
}

/**
 * @brief Get a hex string representation of the supplied binary field.
 *
 * @param[in] field Field to decode.
 * @param[out] out Buffer to decode into.
 * @param[in] out_len Length of output buffer.
 * @retval true Success.
 * @retval false Failure.
 */
static
bool fru_decode_binary(const fru_field_t *field,
                       uint8_t *out,
                       size_t out_len)
{
	return fru_decode_raw_binary(field->data,
	                             FRU_FIELDDATALEN(field->typelen),
	                             out,
	                             out_len);
}

/**
 * @brief Allocate a buffer and encode that data as per FRU specification
 *
 * @param[in] len Buffer length.
 * @param[in] data Binary buffer.
 * @param[in] out_len Length of output buffer.
 * @retval NULL Failure.
 * @return Encoded field.
 */
fru_field_t * fru_encode_data(int len, const uint8_t *data)
{
	int typelen;
	fru_field_t *out;

	typelen = fru_get_typelen(len, data);
	if (FRU_FIELD_TERMINATOR == typelen)
		return NULL; // Can't encode this data

	if (FRU_ISTYPE(typelen, ASCII_6BIT)) {
		out = fru_encode_6bit(data);
	}
	else {
		if (!(out = malloc(FRU_FIELDSIZE(typelen) + 1))) // Extra byte for NUL
			return NULL;

		out->typelen = typelen;
		if (FRU_ISTYPE(typelen, BCDPLUS)) {
			size_t i;
			uint8_t c[2] = {0};

			/* Copy the data and pack it as BCD */
			for (i = 0; i < 2 * FRU_FIELDDATALEN(typelen); i++) {
				switch(data[i]) {
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
						c[i % 2] = data[i] - '0';
				}
				out->data[i / 2] = c[0] << 4 | c[1];
			}
		}
		else {
			memcpy(out->data, data, FRU_FIELDDATALEN(typelen));
		}
		out->data[FRU_FIELDDATALEN(typelen)] = 0; // Terminate for safety
	}

	return out;
}

bool fru_decode_data(fru_field_t *field,
                     decoded_field_t *out,
                     size_t out_len)
{
	if (!field) return false;

	if (FRU_ISTYPE(field->typelen, ASCII_6BIT)) {
		out->type = FIELD_TYPE_SIXBITASCII;
		return fru_decode_6bit(field, out->val, out_len);
	} else {
		if (out_len < (FRU_FIELDDATALEN(field->typelen) + 1))
			return false;

		if (FRU_ISTYPE(field->typelen, BCDPLUS)) {
			out->type = FIELD_TYPE_BCDPLUS;
			return fru_decode_bcdplus(field, out->val, out_len);
		} else {
			out->type = FIELD_TYPE_TEXT;
			memcpy(out->val, field->data, FRU_FIELDDATALEN(field->typelen));
			out->val[FRU_FIELDDATALEN(field->typelen)] = 0; // Terminate the string
			return true;
		}
	}
}

/**
 * Calculate zero checksum for command header and FRU areas
 */
static
int calc_checksum(void *blk, size_t blk_bytes)
{
	if (!blk || blk_bytes == 0) {
		printf("Null pointer or zero buffer length\n");
		return -1;
	}

	uint8_t *data = (uint8_t *)blk;
	uint8_t checksum = 0;

	for(size_t i = 0; i < blk_bytes; i++) {
		checksum += data[i];
	}

	return (int)(uint8_t)(-(int8_t)checksum);
}

/**
 * Calculate an area checksum
 *
 * Calculation includes the checksum byte itself.
 * For freshly prepared area this method returns a checksum to be stored in
 * the last byte. For a pre-existing area this method returns zero if checksum
 * is ok or non-zero otherwise.
 *
 */
int fru_area_checksum(fru_info_area_t *area)
{
	return calc_checksum(area, (area->blocks * FRU_BLOCK_SZ));
}

/**
 * Allocate and build a FRU Information Area block of any type.
 *
 * The function will allocate a buffer of size that is a muliple of 8 bytes
 * and is big enough to accomodate the standard area header corresponding to the
 * requested area type, as well as all the supplied data fields, the require
 * padding, and a checksum byte.
 *
 * The data fields will be taken as is and should be supplied pre-encoded in
 * the standard FRU field format.
 *
 * It is safe to free (deallocate) the fields supplied to this function
 * immediately after the call as all the data is copied to the new buffer.
 *
 * Don't forget to free() the returned buffer when you don't need it anymore.
 *
 * @param[in] atype    Area type (FRU_[CHASSIS|BOARD|PRODUCT]_INFO)
 * @param[in] langtype Language code for areas that use it (board, product) or
 *                     Chassis Type for chassis info area
 * @param[in] tv       Manufacturing time since the Epoch
 *                     (1970/01/01 00:00:00 +0000 UTC)
 *                     for areas that use it (board).
 *                     You may safely use NULL for areas that don't require
 *                     the manufacturing time (non-board areas).
 * @param[in] fields   Single-linked list of data fields
 * @param[in] nstrings Number of strings for mandatory fields
 * @param[in] strings  Array of typed strings for mandatory fields
 * @returns fru_info_area_t *area A newly allocated buffer containing the
 *                                created area
 *
 */
static
fru_info_area_t *fru_create_info_area(fru_area_type_t atype,
                                      uint8_t langtype,
                                      const struct timeval *tv,
                                      fru_reclist_t *fields,
                                      size_t nstrings,
                                      const decoded_field_t strings[])
{
	int field_count;
	size_t padding_size;
	fru_board_area_t header = { // Allocate the biggest possible header
		.ver = FRU_VER_1,
	};
	int headerlen = FRU_INFO_AREA_HEADER_SZ; // Assume a smallest possible
	                                         // header for a generic info area
	void *out = NULL;
	uint8_t *outp;
	fru_reclist_t *field = fields;
	int totalsize = 2; // A generic info area has a custom fields terminator
	                   // and a checksum

	if (!FRU_AREA_IS_GENERIC(atype)) {
		// This function doesn't support multirecord or internal use areas
		errno = EINVAL;
		goto err;
	}

	header.langtype = langtype;

	if (FRU_AREA_HAS_DATE(atype)) {
		uint32_t fru_time;
		const struct timeval tv_unspecified = { 0 };

		if (!tv) {
			errno = EFAULT;
			goto err;
		}

		/*
		 * It's assumed here that UNIX time 0 (Jan 1st of 1970)
		 * can never actually happen in a FRU file in 2018 or later.
		 */
		if (!memcmp(&tv_unspecified, tv, sizeof(*tv))) {
			printf("Using FRU_DATE_UNSPECIFIED\n");
			fru_time = FRU_DATE_UNSPECIFIED;
		} else {
			// FRU time is in minutes and we don't care about microseconds
			fru_time = (tv->tv_sec - fru_datetime_base()) / 60;
		}
		header.mfgdate[0] = 0xFF & fru_time;
		header.mfgdate[1] = 0xFF & (fru_time >> 8);
		header.mfgdate[2] = 0xFF & (fru_time >> 16);
		headerlen = FRU_DATE_AREA_HEADER_SZ; // Expand the header size
	}

	DEBUG("headerlen is %d\n", headerlen);

	totalsize += headerlen;

	/* Find uninitialized mandatory fields, allocate and initialize them with
	 * provided strings */
	for (field_count = 0, field = fields;
	     field && !field->rec && (size_t)field_count < nstrings;
	     field = field->next, field_count++)
	{
		int len = LEN_AUTO;
		switch (strings[field_count].type) {
			case FIELD_TYPE_BINARY: {
				DEBUG("binary format not yet implemented\n");
				errno = EINVAL;
				goto err;
			}
			case FIELD_TYPE_BCDPLUS: {
				len = LEN_BCDPLUS;
				break;
			}
			case FIELD_TYPE_SIXBITASCII: {
				len = LEN_6BITASCII;
				break;
			}
			case FIELD_TYPE_TEXT: {
				len = LEN_TEXT;
				break;
			}
			default:
				len = LEN_AUTO;
				break;
		}
		field->rec = fru_encode_data(len, strings[field_count].val);
		if (!field->rec) goto err;
	}

	/* Now calculate the total size of all initialized
	 * (mandatory and custom) fields */
	for (field = &fields[0]; field && field->rec; field = field->next) {
		totalsize += FRU_FIELDSIZE(field->rec->typelen);
	}

	header.blocks = FRU_BLOCKS(totalsize); // Round up to multiple of 8 bytes
	padding_size = header.blocks * FRU_BLOCK_SZ - totalsize;

	// This will be returned, to be freed by the caller
	out = calloc(1, FRU_BYTES(header.blocks));
	outp = out;

	if (!out) goto err;

	// Now fill the output buffer. First copy the header.
	memcpy(outp, &header, headerlen);
	outp += headerlen;

	DEBUG("area size is %d (%d) bytes\n", totalsize, FRU_BYTES(header.blocks));
	DEBUG("area size in header is (%d) bytes\n", FRU_BYTES(((fru_info_area_t *)out)->blocks));

	// Add the data fields
	for (field = fields; field && field->rec; field = field->next) {
		outp += fru_field_copy(outp, field->rec);
	}

	// Terminate the data fields, add padding and checksum
	*outp = FRU_FIELD_TERMINATOR;
	outp += 1 + padding_size;
	*outp = fru_area_checksum(out);

	DEBUG("area size is %d (%d) bytes\n", totalsize, FRU_BYTES(header.blocks));
	DEBUG("area size in header is (%d) bytes\n", FRU_BYTES(((fru_info_area_t *)out)->blocks));

err:
	/*
	 * Free the allocated mandatory fields. Either an error has occured or the
	 * fields have already been copied into the output buffer. Anyway, they
	 * aren't needed anymore
	 */
	for (--field_count; field_count >= 0; field_count--) {
		free(fields[field_count].rec);
	}
	return out;
}

static
bool fru_decode_custom_fields(const uint8_t *data,
                              int bytes,
                              fru_reclist_t **reclist)
{
	fru_field_t *field = NULL;

	while (bytes > 0) {
		field = (fru_field_t*)data;

		// end of fields
		if (field->typelen == FRU_TYPE_EOF)
			break;

		fru_reclist_t *custom_field = add_reclist(reclist);
		if (custom_field == NULL)
			return false;

		size_t length = FRU_FIELDDATALEN(field->typelen);
		custom_field->rec = calloc(1, FRU_FIELDMAXARRAY);
		custom_field->rec->typelen = field->typelen;
		switch (FRU_TYPE(field->typelen)) {
			case __TYPE_BINARY:
				fru_decode_binary(field, custom_field->rec->data, FRU_FIELDMAXLEN);
				break;
			case __TYPE_ASCII_6BIT:
				fru_decode_6bit(field, custom_field->rec->data, FRU_FIELDMAXLEN);
				break;
			case __TYPE_BCDPLUS:
				fru_decode_bcdplus(field, custom_field->rec->data, FRU_FIELDMAXLEN);
				break;
			default:
				memcpy(custom_field->rec->data, field->data, length);
				custom_field->rec->data[length] = 0; // Terminate the string
				break;
		}
		data += length + 1;
		bytes -= length + 1;
	}

	if (bytes <= 0) {
		DEBUG("area doesn't contain an end-of-fields byte\n");
		return false;
	}

	return true;
}

/**
 * Convert 2 bytes of hex string into a binary byte
 */
static
int16_t hex2byte(const char *hex) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverride-init"
	// First initialize the array with all FFs,
	// then override the values for the bytes that
	// are valid hexadecimal digits
	static const int8_t hextable[256] = {
		[0 ... 255] = -1,
		['0'] = 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,
		['A'] = 10, 11, 12, 13, 14, 15,
		['a'] = 10, 11, 12, 13, 14, 15
	};
#pragma GCC diagnostic pop

	if (!hex) return -1;

	int16_t hi = hextable[(off_t)hex[0]];
	int16_t lo = hextable[(off_t)hex[1]];

	if (hi < 0 || lo < 0)
		return -1;

	return ((hi << 4) | lo);
}

fru_internal_use_area_t *fru_encode_internal_use_area(const void *data, uint8_t *blocks)
{
	size_t i;
	size_t len;
	fru_internal_use_area_t *area = NULL;

	if (!data) {
		errno = EFAULT;
		goto err;
	}

	len = strlen(data);

	/* Must provide even number of nibbles for binary data */
	if (!len || (len % 2)) {
		errno = EINVAL;
		goto err;
	}

	len /= 2;
	*blocks = FRU_BLOCKS(len + sizeof(*area));
	area = calloc(1, FRU_BYTES(*blocks));
	if (!area)
		goto err;

	area->ver = FRU_VER_1;
	for (i = 0; i < len; ++i) {
		int16_t byte = hex2byte(data + 2 * i);
		if (byte < 0) {
			free(area);
			errno = EINVAL;
			goto err;
		}
		area->data[i] = byte;
	}
	goto out;
err:
	area = NULL;
	*blocks = 0;
out:
	return area;
}

/**
 * Allocate and build a Chassis Information Area block.
 *
 * The function will allocate a buffer of size that is a muliple of 8 bytes
 * and is big enough to accomodate the standard area header, all the mandatory
 * fields, all the supplied custom fields, the required padding and a checksum
 * byte.
 *
 * The mandatory fields will be encoded as fits best.  The custom fields will
 * be used as is (pre-encoded).
 *
 * It is safe to free (deallocate) any arguments supplied to this function
 * immediately after the call as all the data is copied to the new buffer.
 *
 * Don't forget to free() the returned buffer when you don't need it anymore.
 *
 * @param[in] chassis Exploded chassis info area
 * @returns fru_info_area_t *area A newly allocated buffer containing the
 * created area
 */
fru_chassis_area_t * fru_encode_chassis_info(const fru_exploded_chassis_t *chassis)
{
	if(!chassis) {
		errno = EFAULT;
		return NULL;
	}

	// List of fields. Mandatory fields are unallocated yet.
	fru_reclist_t fields[] = {
		[FRU_CHASSIS_PARTNO] = { NULL, &fields[FRU_CHASSIS_SERIAL] },
		[FRU_CHASSIS_SERIAL] = { NULL, chassis->cust },
	};

	const decoded_field_t strings[] = { chassis->pn, chassis->serial };
	fru_chassis_area_t *out = NULL;

	if (!SMBIOS_CHASSIS_IS_VALID(chassis->type)) {
		errno = EINVAL;
		return NULL;
	}

	out = fru_create_info_area(FRU_CHASSIS_INFO,
	                           chassis->type, NULL, fields,
	                           ARRAY_SZ(strings), strings);

	return out;
}

bool fru_decode_chassis_info(const fru_chassis_area_t *area,
                             fru_exploded_chassis_t *chassis_out)
{
	chassis_out->type = area->langtype;
	const uint8_t *data = area->data;
	int bytes_left = area->blocks * FRU_BLOCK_SZ;
	fru_field_t *field = (fru_field_t*)data;

	if(!fru_decode_data(field, &chassis_out->pn,
	                    sizeof(chassis_out->pn.val)))
	{
		return false;
	}

	data += FRU_FIELDSIZE(field->typelen);
	bytes_left -= FRU_FIELDSIZE(field->typelen);
	field = (fru_field_t*)data;
	if (!fru_decode_data(field, &chassis_out->serial,
	                     sizeof(chassis_out->serial.val)))
	{
		return false;
	}
	data += FRU_FIELDSIZE(field->typelen);
	bytes_left -= FRU_FIELDSIZE(field->typelen);

	fru_decode_custom_fields(data, bytes_left, &chassis_out->cust);

	return true;
}

/**
 * Allocate and build a Board Information Area block.
 *
 * The function will allocate a buffer of size that is a muliple of 8 bytes
 * and is big enough to accomodate the standard area header, all the mandatory
 * fields, all the supplied custom fields, the required padding and a checksum
 * byte.
 *
 * The mandatory fields will be encoded as fits best.  The custom fields will
 * be used as is (pre-encoded).
 *
 * It is safe to free (deallocate) any arguments supplied to this function
 * immediately after the call as all the data is copied to the new buffer.
 *
 * Don't forget to free() the returned buffer when you don't need it anymore.
 *
 * @param[in] board Exploded board information area
 * @returns fru_info_area_t *area A newly allocated buffer containing the
 * created area
 */
fru_board_area_t * fru_encode_board_info(const fru_exploded_board_t *board)
{
	if(!board) {
		errno = EFAULT;
		return NULL;
	}

	// List of fields. Mandatory fields are unallocated yet.
	fru_reclist_t fields[] = {
		[FRU_BOARD_MFG]      = { NULL, &fields[FRU_BOARD_PRODNAME] },
		[FRU_BOARD_PRODNAME] = { NULL, &fields[FRU_BOARD_SERIAL] },
		[FRU_BOARD_SERIAL]   = { NULL, &fields[FRU_BOARD_PARTNO] },
		[FRU_BOARD_PARTNO]   = { NULL, &fields[FRU_BOARD_FILE] },
		[FRU_BOARD_FILE]     = { NULL, board->cust },
	};

	const decoded_field_t strings[] = {
		board->mfg,
		board->pname,
		board->serial,
		board->pn,
		board->file
	};
	fru_board_area_t *out = NULL;

	out = (fru_board_area_t *)fru_create_info_area(FRU_BOARD_INFO,
	                                               board->lang,
	                                               &board->tv,
	                                               fields,
	                                               ARRAY_SZ(strings),
	                                               strings);

	return out;
}

bool fru_decode_board_info(const fru_board_area_t *area,
                           fru_exploded_board_t *board_out)
{
	fru_field_t *field;
	const uint8_t *data = area->data;
	int bytes_left = area->blocks * FRU_BLOCK_SZ;

	board_out->lang = area->langtype;

	// NOTE: host is not always little endian! //
	union {
		uint32_t val;
		uint8_t arr[4];
	} min_since_1996_big_endian = { 0 };
	min_since_1996_big_endian.arr[1] = area->mfgdate[2];
	min_since_1996_big_endian.arr[2] = area->mfgdate[1];
	min_since_1996_big_endian.arr[3] = area->mfgdate[0];
	uint32_t min_since_1996 = be32toh(min_since_1996_big_endian.val);
	// The argument to mktime is zoneless
	board_out->tv.tv_sec = fru_datetime_base() + 60 * min_since_1996;

	field = (fru_field_t*)data;
	if (!fru_decode_data(field, &board_out->mfg,
	                     sizeof(board_out->mfg.val)))
		return false;
	data += FRU_FIELDSIZE(field->typelen);
	bytes_left -= FRU_FIELDSIZE(field->typelen);

	field = (fru_field_t*)data;
	if (!fru_decode_data(field, &board_out->pname,
	                     sizeof(board_out->pname.val)))
		return false;
	data += FRU_FIELDSIZE(field->typelen);
	bytes_left -= FRU_FIELDSIZE(field->typelen);

	field = (fru_field_t*)data;
	if (!fru_decode_data(field, &board_out->serial,
	                     sizeof(board_out->serial.val)))
		return false;
	data += FRU_FIELDSIZE(field->typelen);
	bytes_left -= FRU_FIELDSIZE(field->typelen);

	field = (fru_field_t*)data;
	if (!fru_decode_data(field, &board_out->pn,
	                     sizeof(board_out->pn.val)))
		return false;
	data += FRU_FIELDSIZE(field->typelen);
	bytes_left -= FRU_FIELDSIZE(field->typelen);

	field = (fru_field_t*)data;
	if (!fru_decode_data(field, &board_out->file,
	                     sizeof(board_out->file.val)))
		return false;
	data += FRU_FIELDSIZE(field->typelen);
	bytes_left -= FRU_FIELDSIZE(field->typelen);

	fru_decode_custom_fields(data, bytes_left, &board_out->cust);

	return true;
}

/**
 * Allocate and build a Product Information Area block.
 *
 * The function will allocate a buffer of size that is a muliple of 8 bytes
 * and is big enough to accomodate the standard area header, all the mandatory
 * fields, all the supplied custom fields, the required padding and a checksum
 * byte.
 *
 * The mandatory fields will be encoded as fits best.
 * The custom fields will be used as is (pre-encoded).
 *
 * It is safe to free (deallocate) any arguments supplied to this function
 * immediately after the call as all the data is copied to the new buffer.
 *
 * Don't forget to free() the returned buffer when you don't need it anymore.
 *
 * @param[in] product Exploded product information area
 * @returns fru_info_area_t *area A newly allocated buffer containing the
 *                                created area
 *
 */
fru_product_area_t * fru_encode_product_info(const fru_exploded_product_t *product)
{
	if(!product) {
		errno = EFAULT;
		return NULL;
	}

	// List of fields. Mandatory fields are unallocated yet.
	fru_reclist_t fields[] = {
		[FRU_PROD_MFG]     = { NULL, &fields[FRU_PROD_NAME] },
		[FRU_PROD_NAME]    = { NULL, &fields[FRU_PROD_MODELPN] },
		[FRU_PROD_MODELPN] = { NULL, &fields[FRU_PROD_VERSION] },
		[FRU_PROD_VERSION] = { NULL, &fields[FRU_PROD_SERIAL] },
		[FRU_PROD_SERIAL]  = { NULL, &fields[FRU_PROD_ASSET] },
		[FRU_PROD_ASSET]   = { NULL, &fields[FRU_PROD_FILE] },
		[FRU_PROD_FILE]    = { NULL, product->cust },
	};

	const decoded_field_t strings[] = {
		product->mfg, product->pname,
		product->pn, product->ver,
		product->serial, product->atag,
		product->file };
	fru_product_area_t *out = NULL;

	out = fru_create_info_area(FRU_PRODUCT_INFO,
	                           product->lang, NULL, fields,
	                           ARRAY_SZ(strings), strings);

	return out;
}

/* These are used for uuid2rec and rec2uuid */
#define UUID_SIZE 16
#define UUID_STRLEN_NONDASHED (UUID_SIZE * 2) // 2 hex digits for byte
#define UUID_STRLEN_DASHED (UUID_STRLEN_NONDASHED + 4)

#pragma pack(push,1)
typedef union __attribute__((packed)) {
	uint8_t raw[UUID_SIZE];
	// The structure is according to DMTF SMBIOS 3.2 Specification
	struct __attribute__((packed)) {
		// All words and dwords here must be Little-Endian for SMBIOS
		uint32_t time_low;
		uint16_t time_mid;
		uint16_t time_hi_and_version;
		uint8_t clock_seq_hi_and_reserved;
		uint8_t clock_seq_low;
		uint8_t node[6];
	};
} uuid_t;
#pragma pack(pop)

static bool is_mr_rec_valid(fru_mr_rec_t *rec, size_t limit, fru_flags_t flags)
{
	int cksum;

	/* The record must have some data to be valid */
	if (!rec || limit <= sizeof(fru_mr_rec_t)) {
		errno = EFAULT;
		return false;
	}

	/*
	 * Each record that is not EOL must have a valid
	 * version, as well as valid checksums
	 */
	if (!IS_FRU_MR_VALID_VER(rec)) {
		errno = EPROTO;
		if (!(flags & FRU_IGNRVER))
			return false;
	}

	/* Check the header checksum, checksum byte included into header */
	cksum = calc_checksum(rec, sizeof(fru_mr_header_t));
	if (cksum) {
		errno = EPROTO;
		if (!(flags & FRU_IGNRHCKSUM))
			return false;
	}

	if (FRU_MR_REC_SZ(rec) > limit) {
		errno = ENOBUFS;
		return false;
	}

	/* Check the data checksum, checksum byte not included into data */
	cksum = calc_checksum(rec->data, rec->hdr.len);
	if (cksum != (int)rec->hdr.rec_checksum) {
		errno = EPROTO;
		if (!(flags & FRU_IGNRDCKSUM))
			return false;
	}

	return true;
}

static int fru_mr_mgmt_blob2rec(fru_mr_rec_t **rec,
                                const void *blob,
                                size_t len,
                                fru_mr_mgmt_type_t type)
{
	size_t min, max;
	int cksum;
	fru_mr_mgmt_rec_t *mgmt = NULL;

	// Need a valid non-allocated record pointer and a blob
	if (!rec || *rec) return -EFAULT;
	if (!blob) return -EFAULT;

	if (type < FRU_MR_MGMT_MIN || type > FRU_MR_MGMT_MAX)
		return -EINVAL;

	min = fru_mr_mgmt_minlen[MGMT_TYPE_ID(type)];
	max = fru_mr_mgmt_maxlen[MGMT_TYPE_ID(type)];

	if (min > len || len > max)
		return -EINVAL;

	/* At this point we believe the input value is sane */

	mgmt = calloc(1, sizeof(fru_mr_mgmt_rec_t) + len);
	if (!mgmt) return errno;

	mgmt->hdr.type_id = FRU_MR_MGMT_ACCESS;
	mgmt->hdr.eol_ver = FRU_MR_VER;
	mgmt->hdr.len = len + 1; // Include the subtype byte
	mgmt->subtype = type;
	memcpy(mgmt->data, blob, len);

	// Checksum the data
	cksum = calc_checksum(((fru_mr_rec_t *)mgmt)->data,
	                      mgmt->hdr.len);
	if (cksum < 0) {
		free(mgmt);
		return -ERANGE;
	}
	mgmt->hdr.rec_checksum = (uint8_t)cksum;

	// Checksum the header, don't include the checksum byte itself
	cksum = calc_checksum(&mgmt->hdr,
	                      sizeof(fru_mr_header_t) - 1);
	if (cksum < 0) {
		free(mgmt);
		return -ERANGE;
	}
	mgmt->hdr.hdr_checksum = (uint8_t)cksum;

	*rec = (fru_mr_rec_t *)mgmt;

	return 0;
}

int fru_mr_mgmt_str2rec(fru_mr_rec_t **rec,
                        const unsigned char *str,
                        fru_mr_mgmt_type_t type)
{
	size_t len;

	if (!str) return -EFAULT;

	len = strulen(str);

	return fru_mr_mgmt_blob2rec(rec, str, len, type);
}

int fru_mr_uuid2rec(fru_mr_rec_t **rec, const unsigned char *str)
{
	size_t len;
	uuid_t uuid;

	if (!str) return -EFAULT;

	len = strulen(str);

	if(UUID_STRLEN_DASHED != len && UUID_STRLEN_NONDASHED != len) {
		return -EINVAL;
	}

	/*
	 * Fill in uuid.raw with bytes encoded from the input string
	 * as they come from left to right. The result is Big Endian.
	 */
	while(*str) {
		static size_t i = 0;
		int val;

		// Skip dashes
		if ('-' == *str) {
			++str;
			continue;
		}

		if (!isxdigit(*str)) {
			return -EINVAL;
		}

		val = toupper(*str);
		if (val < 'A')
			val = val - '0';
		else
			val = val - 'A' + 0xA;

		if (0 == i % 2)
			uuid.raw[i / 2] = val << 4;
		else
			uuid.raw[i / 2] |= val;

		++i;
		++str;
	}

	// Ensure Little-Endian encoding for SMBIOS specification compatibility
	uuid.time_low = htole32(be32toh(uuid.time_low));
	uuid.time_mid = htole16(be16toh(uuid.time_mid));
	uuid.time_hi_and_version = htole16(be16toh(uuid.time_hi_and_version));

	return fru_mr_mgmt_blob2rec(rec, &uuid.raw, sizeof(uuid),
	                            FRU_MR_MGMT_SYS_UUID);
}

int fru_mr_rec2uuid(char **str, fru_mr_mgmt_rec_t *mgmt, fru_flags_t flags)
{
	size_t i;
	uuid_t uuid;

	if (!mgmt || !str) {
		return -EFAULT;
	}

	/* Is this really a Management System UUID record? */
	if (FRU_MR_MGMT_ACCESS != mgmt->hdr.type_id
	    || FRU_MR_VER != (mgmt->hdr.eol_ver & FRU_MR_VER_MASK)
	    || (UUID_SIZE + 1) != mgmt->hdr.len
	    || FRU_MR_MGMT_SYS_UUID != mgmt->subtype)
	{
		return -EINVAL;
	}

	errno = 0;
	if (!is_mr_rec_valid((fru_mr_rec_t *)mgmt, SIZE_MAX, flags)) {
		return -errno;
	}

	*str = calloc(1, UUID_STRLEN_NONDASHED + 1);
	if (!str) return -errno;

	/* This is the reversed operation of uuid2rec, SMBIOS-compatible
	 * Little-Endian encoding in the input record is assumed.
	 * The resulting raw data will be Big Endian */
	memcpy(uuid.raw, mgmt->data, UUID_SIZE);
	uuid.time_hi_and_version = htobe16(le16toh(uuid.time_hi_and_version));
	uuid.time_low = htobe32(le32toh(uuid.time_low));
	uuid.time_mid = htobe16(le16toh(uuid.time_mid));

	/* Now convert the Big Endian byte array into a non-dashed UUID string.
	 * byte2hex() automatically terminates the string.
	 */
	for (i = 0; i < sizeof(uuid.raw); ++i) {
		byte2hex(*str + i * 2, uuid.raw[i]);
	}

	return 0;
}

int fru_mr_mgmt_rec2str(char **str, fru_mr_mgmt_rec_t *mgmt,
                        fru_flags_t flags)
{
	size_t minsize, maxsize;

	if (!mgmt || !str) {
		return -EFAULT;
	}

	/* Is this really a Management Access record and not a UUID subtype? */
	if (FRU_MR_MGMT_ACCESS != mgmt->hdr.type_id
	    || FRU_MR_VER != (mgmt->hdr.eol_ver & FRU_MR_VER_MASK)
	    || (FRU_MR_MGMT_MIN > mgmt->subtype)
	    || (FRU_MR_MGMT_SYS_UUID <= mgmt->subtype))
	{
		return -EINVAL;
	}

	/* Is the management record data size valid? */
	minsize = fru_mr_mgmt_minlen[MGMT_TYPE_ID(mgmt->subtype)];
	maxsize = fru_mr_mgmt_maxlen[MGMT_TYPE_ID(mgmt->subtype)];
	if (minsize > mgmt->hdr.len || mgmt->hdr.len > maxsize) {
		return -EOVERFLOW;
	}

	errno = 0;
	if (!is_mr_rec_valid((fru_mr_rec_t *)mgmt, SIZE_MAX, flags)) {
		return -errno;
	}

	*str = calloc(1, mgmt->hdr.len + 1);
	if (!*str) return -errno;

	memcpy(*str, mgmt->data, mgmt->hdr.len);

	return 0;
}

/**
 * Allocate a new multirecord reclist entry and add it to \a reclist,
 * set \a reclist to point to the newly allocated entry if
 * \a reclist was NULL.
 *
 * @returns Pointer to the added entry
 */
fru_mr_reclist_t *add_mr_reclist(fru_mr_reclist_t **reclist)
{
	fru_mr_reclist_t *rec;
	fru_mr_reclist_t *reclist_ptr = *reclist;
	rec = calloc(1, sizeof(*rec));
	if(!rec) return NULL;

	// If the reclist is empty, update it
	if(!reclist_ptr) {
		*reclist = rec;
	} else {
		// If the reclist is not empty, find the last entry,
		// and append the new one as next
		while(reclist_ptr->next)
			reclist_ptr = reclist_ptr->next;

		reclist_ptr->next = rec;
	}

	return rec;
}

fru_mr_area_t *fru_encode_mr_area(fru_mr_reclist_t *reclist, size_t *total)
{
	fru_mr_area_t *area = NULL;
	fru_mr_rec_t *rec;
	fru_mr_reclist_t *listitem = reclist;

	// Calculate the cumulative size of all records
	while (listitem && listitem->rec && listitem->rec->hdr.len) {
		*total += sizeof(fru_mr_header_t);
		*total += listitem->rec->hdr.len;
		listitem = listitem->next;
	}

	area = calloc(1, *total);
	if (!area) {
		*total = 0;
		return NULL;
	}

	// Walk the input records and pack them into an MR area
	listitem = reclist;
	rec = area;
	while (listitem && listitem->rec && listitem->rec->hdr.len) {
		size_t rec_sz = sizeof(fru_mr_header_t) + listitem->rec->hdr.len;
		memcpy(rec, listitem->rec, rec_sz);
		if (!listitem->next
		    || !listitem->next->rec
		    || !listitem->next->rec->hdr.len)
		{
			// Update the header and its checksum. Don't include the
			// checksum byte itself.
			size_t checksum_span = sizeof(fru_mr_header_t) - 1;
			rec->hdr.eol_ver |= FRU_MR_EOL;
			int cksum = calc_checksum(rec, checksum_span);
			if (cksum < 0) {
				free(area);
				return NULL;
			}
			rec->hdr.hdr_checksum = (uint8_t)cksum;
		}
		rec = (void *)rec + rec_sz;
		listitem = listitem->next;
	}

	return area;
}

bool fru_decode_product_info(const fru_product_area_t *area,
                             fru_exploded_product_t *product_out)
{
	fru_field_t *field;
	const uint8_t *data = area->data;
	int bytes_left = area->blocks * FRU_BLOCK_SZ;

	product_out->lang = area->langtype;

	field = (fru_field_t*)data;
	if (!fru_decode_data(field, &product_out->mfg,
	                     sizeof(product_out->mfg.val)))
		return false;
	data += FRU_FIELDSIZE(field->typelen);
	bytes_left -= FRU_FIELDSIZE(field->typelen);

	field = (fru_field_t*)data;
	if (!fru_decode_data(field, &product_out->pname,
	                     sizeof(product_out->pname.val)))
		return false;
	data += FRU_FIELDSIZE(field->typelen);
	bytes_left -= FRU_FIELDSIZE(field->typelen);

	field = (fru_field_t*)data;
	if (!fru_decode_data(field, &product_out->pn,
	                     sizeof(product_out->pn.val)))
		return false;
	data += FRU_FIELDSIZE(field->typelen);
	bytes_left -= FRU_FIELDSIZE(field->typelen);

	field = (fru_field_t*)data;
	if (!fru_decode_data(field, &product_out->ver,
	                     sizeof(product_out->ver.val)))
		return false;
	data += FRU_FIELDSIZE(field->typelen);
	bytes_left -= FRU_FIELDSIZE(field->typelen);

	field = (fru_field_t*)data;
	if (!fru_decode_data(field, &product_out->serial,
	                     sizeof(product_out->serial.val)))
		return false;
	data += FRU_FIELDSIZE(field->typelen);
	bytes_left -= FRU_FIELDSIZE(field->typelen);

	field = (fru_field_t*)data;
	if (!fru_decode_data(field, &product_out->atag,
	                     sizeof(product_out->atag.val)))
		return false;
	data += FRU_FIELDSIZE(field->typelen);
	bytes_left -= FRU_FIELDSIZE(field->typelen);

	field = (fru_field_t*)data;
	if (!fru_decode_data(field, &product_out->file,
	                     sizeof(product_out->file.val)))
		return false;
	data += FRU_FIELDSIZE(field->typelen);
	bytes_left -= FRU_FIELDSIZE(field->typelen);

	fru_decode_custom_fields(data, bytes_left, &product_out->cust);

	return true;
}

/**
 * Create a FRU information file.
 *
 * @param[in] area  The array of 5 areas, each may be NULL.
 *                  Areas must be given in the FRU order, which is:
 *                  internal use, chassis, board, product, multirecord
 * @param[out] size On success, the size of the newly created FRU information
 *                  buffer, in 8-byte blocks
 *
 * @returns fru_t * buffer, a newly allocated buffer containing the created
 *                          FRU information file
 */
fru_t * fru_create(fru_area_t area[FRU_MAX_AREAS], size_t *size)
{
	fru_t fruhdr = { .ver = FRU_VER_1 };
	int totalblocks = FRU_BLOCKS(sizeof(fru_t)); // Start with just the header
	int area_offsets[FRU_MAX_AREAS] = {
		// Indices must match values of fru_area_type_t
		offsetof(fru_t, internal),
		offsetof(fru_t, chassis),
		offsetof(fru_t, board),
		offsetof(fru_t, product),
		offsetof(fru_t, multirec)
	};
	fru_t *out = NULL;
	int i;
	int cksum;

	// First calculate the total size of the FRU information storage file to
	// be allocated.
	for(i = 0; i < FRU_MAX_AREAS; i++) {
		fru_area_type_t atype = area[i].atype;
		uint8_t blocks = area[i].blocks;
		fru_info_area_t *data = area[i].data;

		// Area type must be valid and match the index
		if (!FRU_IS_ATYPE_VALID(atype) || atype != i) {
			errno = EINVAL;
			return NULL;
		}

		int area_offset_index = area_offsets[atype];
		uint8_t *offset = (uint8_t *)&fruhdr + area_offset_index;

		if(!data // No data is provided
		   || (!FRU_AREA_HAS_SIZE(atype) && !blocks) // or no size is given
		                                             // for a non-sized area
		   || (FRU_AREA_HAS_SIZE(atype) // or a sized area
		      && !((fru_info_area_t *)data)->blocks)) // contains a zero size
		{
			// Mark the area as not present
			*offset = 0;
			continue;
		}

		if(!blocks) {
			blocks = data->blocks;
			area[i].blocks = blocks;
		}

		*offset = totalblocks;
		totalblocks += blocks;
	}

	// Calcute header checksum
	cksum = calc_checksum(&fruhdr, sizeof(fruhdr));
	if (cksum < 0) return NULL;
	fruhdr.hchecksum = (uint8_t)cksum;
	out = calloc(1, FRU_BYTES(totalblocks));

	DEBUG("allocated a buffer at %p\n", out);
	if (!out) return NULL;

	memcpy(out, (uint8_t *)&fruhdr, sizeof(fruhdr));

	// Now go through the areas again and copy them into the allocated buffer.
	// We have all the offsets and sizes set in the previous loop.
	for(i = 0; i < FRU_MAX_AREAS; i++) {
		uint8_t atype = area[i].atype;
		uint8_t blocks = area[i].blocks;
		uint8_t *data = area[i].data;
		int area_offset_index = area_offsets[atype];
		uint8_t *offset = (uint8_t *)&fruhdr + area_offset_index;
		uint8_t *dst = (void *)out + FRU_BYTES(*offset);

		if (!blocks) continue;

		DEBUG("copying %d bytes of area of type %d to offset 0x%03X (%p)\n",
		      FRU_BYTES(blocks), atype, FRU_BYTES(*offset), dst);
		memcpy(dst, data, FRU_BYTES(blocks));
	}

	*size = totalblocks;
	return out;
}

fru_t *find_fru_header(uint8_t *buffer, size_t size, fru_flags_t flags) {
	int cksum;
	if (size < 8) {
		errno = ENOBUFS;
		return NULL;
	}
	fru_t *header = (fru_t *) buffer;
	if ((header->ver != FRU_VER_1)
	    || (header->rsvd != 0)
	    || (header->pad != 0))
	{
		errno = EPROTO;
		if (!(flags & FRU_IGNFVER))
			return NULL;
	}
	cksum = calc_checksum(header, sizeof(fru_t) - 1);
	if (cksum < 0 || header->hchecksum != (uint8_t)cksum) {
		errno = EPROTO;
		if (!(flags & FRU_IGNFHCKSUM))
			return NULL;
	}
	return header;
}

#define AREA(NAME) \
fru_##NAME##_area_t *find_fru_##NAME##_area(uint8_t *buffer,\
                                            size_t size,\
                                            fru_flags_t flags\
                                            )\
{ \
	fru_t *header = find_fru_header(buffer, size, flags); \
	int cksum; \
	if ((header == NULL) || (header->NAME == 0)) { \
		return NULL; \
	} \
	if (FRU_BYTES(header->NAME) + FRU_INFO_AREA_HEADER_SZ > size) { \
		errno = ENOBUFS; \
		return NULL; \
	} \
	fru_##NAME##_area_t *area = \
	    (fru_##NAME##_area_t *)(buffer + FRU_BYTES(header->NAME)); \
	if (area->ver != FRU_VER_1) { \
		errno = EPROTO; \
		if (!(flags & FRU_IGNAVER)) \
			return NULL; \
	} \
	if (FRU_BYTES(header->NAME) + FRU_BYTES(area->blocks) > size) { \
		errno = ENOBUFS; \
		return NULL; \
	} \
	cksum = calc_checksum(((uint8_t *)area), FRU_BYTES(area->blocks) - 1); \
	if (cksum < 0 || *(((uint8_t *)area) + FRU_BYTES(area->blocks) - 1) != \
	                 (uint8_t)cksum) { \
		errno = EPROTO; \
		if (!(flags & FRU_IGNACKSUM)) \
			return NULL; \
	} \
	return area; \
}
AREA(chassis);
AREA(board);
AREA(product);


static
fru_mr_rec_t* fru_mr_next_rec(fru_mr_rec_t* rec, size_t limit,
                              fru_flags_t flags)
{
	if (!is_mr_rec_valid(rec, limit, flags))
		return NULL;

	if (IS_FRU_MR_END(rec))
		return NULL;

	return (void *)rec + FRU_MR_REC_SZ(rec);
}

static
size_t fru_mr_area_size(fru_mr_area_t *area, size_t limit, fru_flags_t flags)
{
	size_t size = 0;
	fru_mr_rec_t *rec = (fru_mr_rec_t *)area;

	/*
	 * The multirecord area does not have a separate area header,
	 * it is just a bunch of records, each with a separate header,
	 * following each another until there are no more valid records.
	 * So walk through and calculate the total size.
	 */
	while (rec) {
		if (!is_mr_rec_valid(rec, limit - size, flags)) {
			if (flags & FRU_IGNRNOEOL)
				return size;
			return 0;
		}
		size += FRU_MR_REC_SZ(rec);

		if (IS_FRU_MR_END(rec))
			break;

		rec = (void *)area + size;
	}

	if (!size) {
		errno = ENODATA;
	}

	return size;
}

fru_internal_use_area_t *find_fru_internal_use_area(
	uint8_t *buffer, size_t *iu_size, size_t size,
	fru_flags_t flags)
{
	off_t next_nearest_area = 0;
	fru_t *header;

	if (!iu_size) {
		errno = EFAULT;
		return NULL;
	}
	*iu_size = 0;

	header = find_fru_header(buffer, size, flags);
	if (!header || !header->internal) {
		return NULL;
	}

	/* Now we need to find the area closest by offset to the
	 * Internal Use one as the IU area doesn't contain its
	 * size in the header so we need to calculate
	 */
	next_nearest_area = size; // Assume there is no next area, and the IU is the last one
	for (fru_area_type_t idx = 0; idx < FRU_MAX_AREAS; ++idx) {
		uint8_t *area_offset_ptr = (uint8_t *)header + offsetof(fru_t, internal) + idx;
		if (*area_offset_ptr > header->internal
		    && FRU_BYTES(*area_offset_ptr) < next_nearest_area)
		{
			next_nearest_area = FRU_BYTES(*area_offset_ptr);
		}
	}
	fru_internal_use_area_t *area =
	    (fru_internal_use_area_t *)(buffer + FRU_BYTES(header->internal));

	if (area->ver != FRU_VER_1 && !(flags & FRU_IGNRVER)) {
		return NULL;
	}

	*iu_size = next_nearest_area - FRU_BYTES(header->internal);
	return area;
}

fru_mr_area_t *find_fru_mr_area(uint8_t *buffer, size_t *mr_size, size_t size,
                                fru_flags_t flags)
{
	size_t limit;
	fru_t *header;

	if (!mr_size) {
		errno = EFAULT;
		return NULL;
	}
	header = find_fru_header(buffer, size, flags);
	if (!header || !header->multirec) {
		return NULL;
	}
	if (FRU_BYTES(header->multirec) + sizeof(fru_mr_rec_t) > size) {
		errno = ENOBUFS;
		return NULL;
	}
	fru_mr_area_t *area =
	    (fru_mr_area_t *)(buffer + FRU_BYTES(header->multirec));

	limit = size - ((uint8_t *)header - buffer);
	errno = 0;
	*mr_size = fru_mr_area_size(area, limit, flags);
	if (!*mr_size)
	{
		if (!errno)
			errno = ENODATA;
		return NULL;
	}

	return area;
}

int fru_decode_mr_area(const fru_mr_area_t *area,
                       fru_mr_reclist_t **reclist,
                       size_t mr_size,
                       fru_flags_t flags)
{
	fru_mr_rec_t *srec = (fru_mr_rec_t *)area;
	fru_mr_rec_t *rec;
	fru_mr_reclist_t *mr_reclist_tail = NULL;
	size_t total = 0;
	int count = -1;
	int err = 0;

	if (!reclist) {
		err = EFAULT;
		goto out;
	}

	if (*reclist) {
		/* The code below expects an empty reclist */
		err = EINVAL;
		goto out;
	}

	while (srec) {
		size_t rec_sz = FRU_MR_REC_SZ(srec);
		if (!is_mr_rec_valid(srec, mr_size - total, flags)) {
			count = -1;
			break;
		}
		rec = calloc(1, rec_sz);
		if (!rec) {
			count = -1;
			err = errno;
			break;
		}
		memmove(rec, srec, rec_sz);

		mr_reclist_tail = add_mr_reclist(reclist);
		if (!mr_reclist_tail) {
			count = -1;
			break;
		}
		mr_reclist_tail->rec = rec;

		srec = fru_mr_next_rec(srec, mr_size - total, flags);
		total += rec_sz;
		count = (count < 0) ? 1 : count + 1;
	}

out:
	if (count < 0) {
		if (*reclist) {
			free_reclist(*reclist);
			*reclist = NULL;
		}
		errno = err;
	}

	return count;
}

bool fru_decode_internal_use_area(const fru_internal_use_area_t *area,
                                  size_t area_len,
                                  uint8_t **out,
                                  fru_flags_t flags __attribute__((unused)))
{
	uint8_t *hexstring;
	/* Internal Use Area size includes the version byte header that
	 * we don't want to decode, so we subtract the header size. */
	size_t data_len = area_len - sizeof(*area);
	size_t out_len = data_len * 2 + 1;

	if (!out) {
		errno = EFAULT;
		return false;
	}

	/* The output is two hex digits per byte,
	 * plus an extra byte for the string terminator. */
	hexstring = calloc(1, out_len);

	if (!hexstring)
		return false;

	if(!fru_decode_raw_binary(area->data, data_len,
	                          hexstring, out_len))
	{
		free(hexstring);
		errno = ENOBUFS;
		return false;
	}

	*out = hexstring;
	return true;
}

#ifdef __STANDALONE__

void dump(int len, const unsigned char *data)
{
	int i;
	printf("Data Dump:");
	for (i = 0; i < len; i++) {
		if (!(i % 16)) printf("\n%04X:  ", i);
		printf("%02X ", data[i]);
	}
	printf("\n");
}

void test_encodings(void)
{
	int i, len;
	uint8_t typelen;
	unsigned char *test_strings[] = {
		/* 6-bit ASCII */
		"IPMI", "OK!",
		/* BCD plus */
		"1234-56-7.89 01",
		/* Simple text */
		"This is a simple text, with punctuation & other stuff",
		/* Binary */
		"\x00\x01\x02\x03\x04\x05 BINARY TEST"
	};
	unsigned char *test_types[] = {
		"6-bit", "6-bit",
		"BCPplus",
		"Simple text",
		"Binary"
	};
	int test_lengths[] = { LEN_AUTO, LEN_AUTO, LEN_AUTO, LEN_AUTO, 18 };

	for(i = 0; i < ARRAY_SZ(test_strings); i++) {
		fru_field_t *field;
		const unsigned char out[FRU_FIELDMAXARRAY];

		printf("Data set %d.\n", i);
		printf("Original data ");
		if (test_lengths[i]) dump(test_lengths[i], test_strings[i]);
		else printf(": [%s]\n", test_strings[i]);

		printf("Original type: %s\n", test_types[i]);
		printf("Encoding... ");
		field = fru_encode_data(test_lengths[i], test_strings[i]);
		if (FRU_FIELD_TERMINATOR == field->typelen) {
			printf("FAIL!\n\n");
			continue;
		}

		printf("OK\n");
		printf("Encoded type is: ");
		switch((field->typelen & __TYPE_BITS_MASK) >> __TYPE_BITS_SHIFT) {
			case __TYPE_TEXT:
				printf("Simple text\n");
				break;
			case __TYPE_ASCII_6BIT:
				printf("6-bit\n");
				break;
			case __TYPE_BCDPLUS:
				printf("BCDplus\n");
				break;
			default:
				printf("Binary\n");
				break;
		}

		printf("Encoded data ");
		dump(FRU_FIELDSIZE(field->typelen), (uint8_t *)field);
		printf("Decoding... ");

		if (!fru_decode_data(field->&typelen, &field->data,
		                     FRU_FIELDMAXARRAY))
		{
			printf("FAIL!");
			goto next;
		}

		printf("Decoded data ");
		if (FRU_ISTYPE(field->typelen, BINARY)) {
			dump(FRU_FIELDDATALEN(field->typelen), out);
		}
		else {
			printf(": [%s]\n", out);
		}

		printf("Comparing... ");
		if (test_lengths[i] && !memcmp(test_strings[i], out, test_lengths[i]) ||
		    !strcmp(test_strings[i], out))
		{
			printf("OK!");
		}
		else {
			printf("FAIL!");
		}

next:
		free((void *)field);
		printf("\n\n");
	}
}

int main(int argc, char *argv[])
{
	test_encodings();
	exit(1);
}
#endif
