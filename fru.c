/** @file
 *  @brief FRU information encoding functions
 *
 *  @copyright
 *  Copyright (C) 2016-2024 Alexander Amelkin <alexander@amelkin.msk.ru>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later OR Apache-2.0
 */

#include "fru-private.h"


#if 0 // Old code

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

#include "fru-errno.h"
#include "smbios.h"

#define _BSD_SOURCE
#include <endian.h>

#ifdef __STANDALONE__
#include <stdio.h>
#endif

const char * fru_enc_name_by_type(field_type_t type)
{
	static const char * fru_enc_type_names[TOTAL_FIELD_TYPES] = {
		[FIELD_TYPE_AUTO] = "auto",
		[FIELD_TYPE_BINARY] = "binary", /* For input data that is hex string [0-9A-Fa-f] */
		[FIELD_TYPE_BCDPLUS] = "bcdplus",
		[FIELD_TYPE_6BITASCII] = "6bitascii",
		[FIELD_TYPE_TEXT] = "text"
	};

	if (type < FIELD_TYPE_AUTO || type >= TOTAL_FIELD_TYPES) {
		return "undefined";
	}

	return fru_enc_type_names[type];
}
#else /* DEBUG */
#define DEBUG(f, args...)
#endif /* DEBUG */

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
void cut_tail(char *s)
{
	int i;
	for(i = strlen((char *)s) - 1; i >= 0 && ' ' == s[i]; i--) s[i] = 0;
}

/** Copy a FRU area field to a buffer and return the field's size */
static inline
uint8_t fru_field_copy(void *dest, const fru__file_field_t *fieldp)
{
	memcpy(dest, (void *)fieldp, FRU__FIELDSIZE(fieldp->typelen));
	DEBUG("copied field of type %02x, size %zd (%zd) into %p\n",
	      FRU__TYPE(fieldp->typelen),
	      FRU__FIELDLEN(fieldp->typelen), FRU__FIELDSIZE(fieldp->typelen),
	      dest
	      );
	return FRU__FIELDSIZE(fieldp->typelen);
}

static inline
uint8_t nibble2hex(char n)
{
	return (n > 9 ? n - 10 + 'A': n + '0');
}

/**
 * Convert 2 bytes of hex string into a binary byte
 */
static
int16_t hex2byte(const char *hex) {
/* TODO: Get rid of this fast and elegant code for the sake of portability, rework using if */
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

	if (hi < 0 || lo < 0) {
		fru_errno = FEGENERIC;
		errno = EINVAL;
		return -1;
	}

	return ((hi << 4) | lo);
}


/**
 * Detect the most suitable encoding for the given string
 *
 * @returns A FRU field type if everything was ok, or an error code.
 * @retval FIELD_TYPE_EMPTY The \a data argument was NULL or the length
 *         of the data string was zero
 * @retval FIELD_TYPE_TOOLONG The data exceeded the maximum length (63 bytes)
 * @retval FIELD_TYPE_NONPRINTABLE The data contains non-printable characters
 *
 */
static
field_type_t fru_detect_type(const char *data) /**< [in] The input data */
{
	size_t len;
	field_type_t type = FIELD_TYPE_AUTO;

	if (!data)
		return FIELD_TYPE_EMPTY;

	len = strlen(data);
	if (!len) {
		DEBUG("Data string is empty\n");
		return FIELD_TYPE_EMPTY;
	}

	// If the data exceeds the maximum length, return a terminator
	// TODO: This is wrong, max length depends on the encoding
	if (len > FRU_FIELDMAXLEN) {
		DEBUG("Data exceeds maximum length\n");
		fru_errno = FELONGINPUT;
		return FIELD_TYPE_UNKNOWN;
	}

	DEBUG("Guessing type of string '%s'...\n", (char *)data);

	// By default - the most range-restricted text type
	// On input we treat 'BINARY' as simple BCD (hex string)
	type = FIELD_TYPE_BINARY;
	DEBUG("Assuming binary (hex string) data...\n");

	// Go through the data and expand charset as needed
	for (size_t i = 0; i < len; i++) {
		if (data[i] < ' '
		    && data[i] != '\t'
		    && data[i] != '\r'
		    && data[i] != '\n')
		{
			// They cheated! The input data is non-printable (binary)! We don't support that!
			DEBUG("[%#02x] Binary data!\n", data[i]);
			fru_errno = FENONPRINT;
			return FIELD_TYPE_UNKNOWN;
		}

		if (type < FIELD_TYPE_BCDPLUS
			&& !isxdigit(data[i]))
		{
			// The data doesn't fit into BINARY (hex string), expand to
			DEBUG("[%c] Data is at least BCD+!\n", data[i]);
			type = FIELD_TYPE_BCDPLUS;
		}

		if (type < FIELD_TYPE_6BITASCII // Do not reduce the range
			&& !isdigit(data[i])
			&& data[i] != ' '
			&& data[i] != '-'
			&& data[i] != '.')
		{
			// The data doesn't fit into BCD plus, expand to
			DEBUG("[%c] Data is at least 6-bit ASCII!\n", data[i]);
			type = FIELD_TYPE_6BITASCII;
		}

		if (type < FIELD_TYPE_TEXT
			&& (data[i] > '_' || data[i] < ' '))
		{
			// The data doesn't fit into 6-bit ASCII, expand to simple text.
			DEBUG("[%c] Data is simple text!\n", data[i]);
			type = FIELD_TYPE_TEXT;
			continue;
		}
	}

	return type;
}

/**
 * Allocate a buffer and encode the input string into it as 6-bit ASCII
 *
 * @returns pointer to the newly allocated field buffer if allocation and
 *          encoding were successful
 * @returns NULL if there was an error, sets errno accordingly (man malloc)
 */
static
fru__file_field_t *fru_encode_6bit(const char *s /**< [in] Input string */)
{
	size_t len = strlen(s);
	size_t len6bit = FRU_6BIT_LENGTH(len);
	size_t i, i6;
	fru__file_field_t *out = NULL;
	size_t outlen = sizeof(fru__file_field_t) + len6bit;

	fru_errno = FERANGE;
	if (len6bit > FRU__FIELDLEN(len6bit) || !(out = calloc(1, outlen)))
	{
		if (!out)
			fru_errno = errno;
		return out;
	}

	out->typelen = FRU__TYPELEN(ASCII_6BIT, len6bit);

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
 * Allocate a buffer and encode the input string into it as BCD+
 *
 * @returns pointer to the newly allocated field buffer if allocation and
 *          encoding were successful
 * @returns NULL if there was an error, sets errno accordingly (man malloc)
 *          May set errno to EMSGSIZE if the \a s string exceeds the
 *          maximum length
 */
static
fru__file_field_t *fru_encode_bcdplus(const char *s /**< [in] Input string */)
{
	size_t len = strlen(s);
	size_t lenbcd = (len + 1) / 2; /* Need an extra byte for a lone trailing nibble */
	fru__file_field_t *out = NULL;
	size_t outlen = sizeof(fru__file_field_t) + lenbcd;
	uint8_t c[2] = { 0 };

	fru_errno = FERANGE;
	if (lenbcd > FRU__FIELDLEN(lenbcd) || !(out = calloc(1, outlen)))
	{
		if (!out)
			fru_errno = errno;
		return out;
	}

	out->typelen = FRU__TYPELEN(BCDPLUS, lenbcd);

	/* Copy the data and pack it as BCD */
	for (size_t i = 0; i < len; i++) {
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
				c[i % 2] = s[i] - '0';
		}
		out->data[i / 2] = c[0] << 4 | c[1];
	}

	return out;
}

/**
 * Allocate a buffer and encode the input string into it as plain text
 *
 * @returns pointer to the newly allocated field buffer if allocation and
 *          encoding were successful
 * @returns NULL if there was an error, sets errno accordingly (man malloc)
 *          May set errno to EMSGSIZE if the \a s string exceeds the
 *          maximum length
 */
static
fru__file_field_t *fru_encode_text(const char *s /**< [in] Input string */)
{
	size_t len = strlen(s);
	fru__file_field_t *out = NULL;
	// For TEXT encoding length 1 means "end-of-fields"
	size_t outlen = ((len == 1) ? 2 : len);

	fru_errno = FERANGE;
	if (len > FRU__FIELDLEN(len)
	    || !(out = calloc(1, sizeof(fru__file_field_t) + outlen)))
	{
		if (!out)
			fru_errno = errno;
		return out;
	}

	out->typelen = FRU__TYPELEN(TEXT, len);
	memcpy(out->data, s, len); // We don't want the nul-byte in the destination
	fru_errno = 0;
	return out;
}


/*
 * A helper to endcode a hex string into a byte array
 */
static
uint8_t * fru_encode_binary_string(size_t *len, const char *hexstr)
{
	size_t i;
	uint8_t *buf;

	if (!len) {
		DEBUG("Storage for hex string length is not provided\n");
		fru_errno = FEGENERIC;
		errno = EFAULT;
		return NULL;
	}

	*len = strlen(hexstr);
	if (*len % 2) {
		DEBUG("Must provide even number of nibbles for binary data\n");
		fru_errno = FENOTEVEN;
		return NULL;
	}
	*len /= 2;
	buf = malloc(*len);
	if (!buf) {
		DEBUG("Failed to allocate a buffer for binary data\n");
		fru_errno = errno;
		return NULL; /* errno from malloc */
	}
	for (i = 0; i < *len; i++) {
		int16_t byte = hex2byte(hexstr + 2 * i);
		DEBUG("[%zd] %c %c => 0x%02" PRIX16 "\n",
		      i, hexstr[2 * i], hexstr[2 * i + 1], byte);
		if (byte < 0) {
			DEBUG("Invalid hex data provided for binary attribute\n");
			fru_errno = FENONHEX;
			return NULL;
		}
		buf[i] = byte;
	}
	return buf;
}

/**
 * Allocate a buffer and encode the input string into it as plain text
 *
 * @returns pointer to the newly allocated field buffer if allocation and
 *          encoding were successful
 * @returns NULL if there was an error, sets errno accordingly (man malloc)
 *          May set fru_errno to FELONGINPUT if the \a s string exceeds the
 *          maximum length
 */
static
fru__file_field_t * fru_encode_binary(const char *hexstr)
{
	size_t len;
	fru__file_field_t *out = NULL;
	uint8_t *buf = fru_encode_binary_string(&len, hexstr);
	if (!buf) {
		return NULL;
	}

	if (len > FRU__FIELDMAXLEN)
	{
		DEBUG("Binary data length (%zd) exceeds max (%" PRIu8 ")\n",
		      len, FRU__FIELDMAXLEN);
		free(buf);
		fru_errno = FELONGINPUT;
		return NULL;
	}

	out = calloc(1, sizeof(fru__file_field_t) + len);
	if (!out) {
		fru_errno = errno;
		free(buf);
		return NULL;
	}

	out->typelen = FRU__TYPELEN(BINARY, len);
	memcpy(out->data, buf, len);
	free(buf);

	return out;
}

/**
 * @brief Allocate a buffer and encode that data as per FRU specification
 *
 * @param[in] field Pointer to the input (decoded) field structure
 * @retval NULL Failure, errno is set to the reason
 * @return Encoded field.
 */
static
fru__file_field_t * fru_encode_data(fru_field_t *field)
{
	size_t len = strlen(field->val);
	field_type_t realtype;
	fru__file_field_t *out;
	fru__file_field_t * (*encode[TOTAL_FIELD_TYPES])(const char *) = {
		[FIELD_TYPE_BINARY] = fru_encode_binary,
		[FIELD_TYPE_BCDPLUS] = fru_encode_bcdplus,
		[FIELD_TYPE_6BITASCII] = fru_encode_6bit,
		[FIELD_TYPE_TEXT] = fru_encode_text,
	};

	if (field->type < FIELD_TYPE_AUTO || field->type > FIELD_TYPE_MAX) {
		DEBUG("ERROR: Field encoding type is invalid (%d)\n", field->type);
		fru_errno = FEBADENC;
		return NULL;
	}

	DEBUG("The field is marked as '%s', length is %zi\n",
	      fru_enc_name_by_type(field->type),
	      len);

	realtype = fru_detect_type(field->val);
	if (realtype == FIELD_TYPE_UNKNWOWN) {
		switch (fru_errno) {
		case FELONGINPUT:
			DEBUG("ERROR: Data in field is too long\n");
			break;
		case FENONPRINT;
			DEBUG("ERROR: Data in field contains non-printable characters\n");
			break;
		default:
			DEBUG("BUG: Type of data in field can't be detected!\n");
			break;
		}
		return NULL;
	}

	if (FIELD_TYPE_AUTO == field->type) {
		field->type = realtype;
	}
	else if (field->type != FIELD_TYPE_BINARY && realtype > field->type) {
		DEBUG("ERROR: Data in field exceeds the specified type's range\n");
		/* TODO: Add command line option to allow automatic type expansion */
		fru_errno = FERANGE;
		return NULL;
	}

	if (!len || field->type == FIELD_TYPE_EMPTY) {
		out = calloc(1, sizeof(fru__file_field_t));
		if (!out) {
			fru_errno = errno;
			return NULL;
		}
		out->typelen = FRU_FIELD_EMPTY;
		return out;
	}

	DEBUG("The field will be encoded as '%s'\n",
	      fru_enc_name_by_type(field->type));

	return encode[field->type](field->val);
}

bool fru_setfield(fru_field_t *field, field_type_t enc, const char *s)
{
	if (!field || !s) {
		fru_errno = FEGENERIC;
		errno = EFAULT;
		return false;
	}

	if (enc != FIELD_TYPE_PRESERVE
	    && (enc < FIELD_TYPE_AUTO
	        || enc > FIELD_TYPE_MAX))
	{
		fru_errno = FEBADENC;
		return false;
	}

	if (enc != FIELD_TYPE_PRESERVE)
		field->type = enc;
	strncpy(field->val, s, sizeof(field->val));
	field->val[sizeof(field->val)] = 0;
	return true;
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
 * @param[in] fields   Single-linked list of data fields (first mandatory, then custom)
 *
 * @returns fru__file_area_t *area A newly allocated buffer containing the
 *                                created area
 *
 */
static
fru__file_area_t *fru_create_info_area(fru_area_type_t atype,
                                      uint8_t langtype,
                                      const struct timeval *tv,
                                      const fru_reclist_t *fields)
{
	int field_count;
	size_t padding_size;
	fru__file_board_t header = { // Allocate the biggest possible header
		.ver = FRU_VER_1,
	};
	int headerlen = FRU_INFO_AREA_HEADER_SZ; // Assume a smallest possible
	                                         // header for a generic info area
	void *out = NULL;
	uint8_t *outp;
	const fru_reclist_t *field = fields;

	/*
	 * A single-linked list of encoded FRU area fields.
	 *
	 * This structure is similar to fru_generic_reclist_t so
	 * the standard fru__add_reclist_entry() and free_reclist() can be used
	 * on it.
	 */
	struct fru_encoded_reclist_s {
		fru__file_field_t *rec; /* A pointer to the field or NULL if not initialized */
		struct fru_encoded_reclist_s *next; /* The next record in the list or NULL if last */
	} *encoded_fields = NULL;
	int totalsize = 0;

	if (!FRU_AREA_IS_GENERIC(atype)) {
		// This function doesn't support multirecord or internal use areas
		fru_errno = FEAREANOTSUP;
		goto err;
	}

	header.langtype = langtype;

	if (FRU_AREA_HAS_DATE(atype)) {
		uint32_t fru_time;
		const struct timeval tv_unspecified = { 0 };

		if (!tv) {
			fru_errno = FEGENERIC;
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

	/* Encode the input fields */
	struct fru_encoded_reclist_s *encoded_recptr;
	for (field_count = 0, field = fields; field; field = field->next, field_count++) {
		/* Allocate a new entry for the list of encoded fields */
		encoded_recptr = fru__add_reclist_entry(&encoded_fields, RECLIST_TAIL);
		if (!encoded_recptr) {
			goto err_fields;
		}

		encoded_recptr->rec = fru_encode_data(field->rec);
		if (!encoded_recptr->rec) {
			goto err_fields;
		}
		/* Update the total size of all encoded (mandatory and custom) fields */
		totalsize += FRU__FIELDSIZE(encoded_recptr->rec->typelen);
	}
	totalsize += sizeof(fru__file_field_t); /* Reserve space for a terminator field */
	totalsize++; /* Reserve space for the checksum */

	header.blocks = FRU_BLOCKS(totalsize); // Round up to multiple of 8 bytes
	padding_size = FRU_BYTES(header.blocks) - totalsize;
	DEBUG("padding size is %zd (%d - %d) bytes\n", padding_size, FRU_BYTES(header.blocks), totalsize);

	// This will be returned, to be freed by the caller
	out = calloc(1, FRU_BYTES(header.blocks));
	outp = out;

	if (!out) {
		fru_errno = errno;
		goto err_fields;
	}

	// Now fill the output buffer. First copy the header.
	memcpy(outp, &header, headerlen);
	outp += headerlen;

	DEBUG("area size is %d (%d) bytes, header is %zd bytes long\n",
	      totalsize, FRU_BYTES(header.blocks), (void *)outp - out);
	DEBUG("area size in header is (%d) bytes\n", FRU_BYTES(((fru__file_info_t *)out)->blocks));

	// Copy the data from the encoded field list into the linear output buffer
	for (struct fru_encoded_reclist_s *encoded_recptr = encoded_fields;
	     encoded_recptr && encoded_recptr->rec;
	     encoded_recptr = encoded_recptr->next)
	{
		outp += fru_field_copy(outp, encoded_recptr->rec);
	}

	// Terminate the data fields, add padding and checksum
	*outp = FRU_FIELD_TERMINATOR;
	DEBUG("terminator (c1) written at offset %zd\n", (void *)outp - out);
	outp += 1 + padding_size;
	*outp = fru_area_checksum(out);
	DEBUG("checksum (%02x) written at offset %zd\n", *outp, (void *)outp - out);

err_fields:
	free_reclist(encoded_fields);
err:
	return out;
}

fru_internal_use_area_t *fru_encode_internal_use_area(const void *data, uint8_t *blocks)
{
	size_t i;
	size_t len;
	fru_internal_use_area_t *area = NULL;

	if (!data) {
		fru_errno = FEGENERIC;
		errno = EFAULT;
		goto err;
	}

	len = strlen(data);

	/* Must provide even number of nibbles for binary data */
	if (!len || (len % 2)) {
		fru_errno = FEHEXLEN;
		goto err;
	}

	len /= 2;
	*blocks = FRU_BLOCKS(len + sizeof(*area));
	area = calloc(1, FRU_BYTES(*blocks));
	if (!area) {
		fru_errno = errno;
		goto err;
	}

	area->ver = FRU_VER_1;
	for (i = 0; i < len; ++i) {
		int16_t byte = hex2byte(data + 2 * i);
		if (byte < 0) {
			free(area);
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
 * @returns fru_info_area_T *area A newly allocated buffer containing the
 * created area
 */
fru__file_chassis_t * fru_encode_chassis_info(fru_chassis_t *chassis)
{
	if(!chassis) {
		fru_errno = FEGENERIC;
		errno = EFAULT;
		return NULL;
	}

	/*
	 * List of decoded fields. First mandatory, then custom.
	 */
	fru_reclist_t fields[] = {
		[FRU_CHASSIS_PARTNO] = { &chassis->pn, &fields[FRU_CHASSIS_SERIAL] },
		[FRU_CHASSIS_SERIAL] = { &chassis->serial, chassis->cust },
	};

	fru__file_chassis_t *out = NULL;

	if (!SMBIOS_CHASSIS_IS_VALID(chassis->type)) {
		fru_errno = FEINVCHAS;
		return NULL;
	}

	out = fru_create_info_area(FRU_CHASSIS_INFO,
	                           chassis->type, NULL, fields);

	return out;
}

bool fru_decode_chassis_info(const fru__file_chassis_t *area,
                             fru_chassis_t *chassis_out)
{
	return fru_decode_area_info(FRU_CHASSIS_INFO, area, chassis_out);
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
 * @returns fru__file_area_t *area A newly allocated buffer containing the
 * created area
 */
static
fru__file_board_t * fru_encode_board_info(fru_board_t *board)
{
	if(!board) {
		fru_errno = FEGENERIC;
		errno = EFAULT;
		return NULL;
	}

	/*
	 * List of decoded fields. First mandatory, then custom.
	 */
	fru_reclist_t fields[] = {
		[FRU_BOARD_MFG]      = { &board->mfg, &fields[FRU_BOARD_PRODNAME] },
		[FRU_BOARD_PRODNAME] = { &board->pname, &fields[FRU_BOARD_SERIAL] },
		[FRU_BOARD_SERIAL]   = { &board->serial, &fields[FRU_BOARD_PARTNO] },
		[FRU_BOARD_PARTNO]   = { &board->pn, &fields[FRU_BOARD_FILE] },
		[FRU_BOARD_FILE]     = { &board->file, board->cust },
	};

	fru__file_board_t *out = NULL;

	out = (fru__file_board_t *)fru_create_info_area(FRU_BOARD_INFO,
	                                               board->lang,
	                                               &board->tv,
	                                               fields);

	return out;
}

bool fru_decode_board_info(const fru__file_board_t *area,
                           fru_board_t *board_out)
{
	return fru_decode_area_info(FRU_BOARD_INFO, area, board_out);
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
 * @returns fru__file_area_t *area A newly allocated buffer containing the
 *                                created area
 *
 */
fru__file_product_t * fru_encode_product_info(fru_product_t *product)
{
	if(!product) {
		fru_errno = FEGENERIC;
		errno = EFAULT;
		return NULL;
	}

	/*
	 * List of decoded fields. First mandatory, then custom.
	 */
	fru_reclist_t fields[] = {
		[FRU_PROD_MFG]     = { &product->mfg, &fields[FRU_PROD_NAME] },
		[FRU_PROD_NAME]    = { &product->pname, &fields[FRU_PROD_MODELPN] },
		[FRU_PROD_MODELPN] = { &product->pn, &fields[FRU_PROD_VERSION] },
		[FRU_PROD_VERSION] = { &product->ver, &fields[FRU_PROD_SERIAL] },
		[FRU_PROD_SERIAL]  = { &product->serial, &fields[FRU_PROD_ASSET] },
		[FRU_PROD_ASSET]   = { &product->atag, &fields[FRU_PROD_FILE] },
		[FRU_PROD_FILE]    = { &product->file, product->cust },
	};

	fru__file_product_t *out = NULL;

	out = fru_create_info_area(FRU_PRODUCT_INFO,
	                           product->lang, NULL, fields);

	return out;
}

static int fru_mr_mgmt_blob2rec(fru_file_mr_rec_t **rec,
                                const void *blob,
                                size_t len,
                                fru_mr_mgmt_type_t type)
{
	size_t min, max;
	int cksum;
	fru_file_mr_mgmt_rec_t *mgmt = NULL;

	// Need a valid non-allocated record pointer and a blob
	if (!rec || *rec || !blob) {
		fru_errno = FEGENERIC;
		errno = EFAULT;
		return -1;
	}

	if (type < FRU_MR_MGMT_MIN || type > FRU_MR_MGMT_MAX)
		return -FEMRMGMTRANGE;

	min = fru_mr_mgmt_minlen[MGMT_TYPE_ID(type)];
	max = fru_mr_mgmt_maxlen[MGMT_TYPE_ID(type)];

	if (min > len || len > max)
		return -FEMRMGMTSIZE;

	/* At this point we believe the input value is sane */

	mgmt = calloc(1, sizeof(fru_file_mr_mgmt_rec_t) + len);
	if (!mgmt) {
		fru_errno = errno;
		return -fru_errno;
	}

	mgmt->hdr.type_id = FRU_MR_MGMT_ACCESS;
	mgmt->hdr.eol_ver = FRU_MR_VER;
	mgmt->hdr.len = len + 1; // Include the subtype byte
	mgmt->subtype = type;
	memcpy(mgmt->data, blob, len);

	// Checksum the data
	cksum = calc_checksum(((fru_file_mr_rec_t *)mgmt)->data,
	                      mgmt->hdr.len);
	if (cksum < 0) {
		free(mgmt);
		return -FEMRDCKSUM;
	}
	mgmt->hdr.rec_checksum = (uint8_t)cksum;

	// Checksum the header, don't include the checksum byte itself
	cksum = calc_checksum(&mgmt->hdr,
	                      sizeof(fru_file_mr_header_t) - 1);
	if (cksum < 0) {
		free(mgmt);
		return -FEMRHCKSUM;
	}
	mgmt->hdr.hdr_checksum = (uint8_t)cksum;

	*rec = (fru_file_mr_rec_t *)mgmt;

	return 0;
}

int fru_mr_mgmt_str2rec(fru_file_mr_rec_t **rec,
                        const char *str,
                        fru_mr_mgmt_type_t type)
{
	size_t len;

	if (!str) {
		fru_errno = FEGENERIC;
		errno = EFAULT;
		return -1;
	}

	len = strlen(str);

	return fru_mr_mgmt_blob2rec(rec, str, len, type);
}

int fru_mr_uuid2rec(fru_file_mr_rec_t **rec, const char *str)
{
	size_t len;
	uuid_t uuid;

	if (!str) {
		fru_errno = FEGENERIC;
		errno = EFAULT;
		return -1;
	}

	len = strlen(str);

	if(UUID_STRLEN_DASHED != len && UUID_STRLEN_NONDASHED != len) {
		fru_errno = FEGENERIC;
		errno = EINVAL;
		return -1;
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
			fru_errno = FEGENERIC;
			errno = EINVAL;
			return -1;
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


fru_file_mr_area_t *fru_encode_mr_area(fru_mr_reclist_t *reclist, size_t *total)
{
	fru_file_mr_area_t *area = NULL;
	fru_file_mr_rec_t *rec;
	fru_mr_reclist_t *listitem = reclist;

	// Calculate the cumulative size of all records
	while (listitem && listitem->rec && listitem->rec->hdr.len) {
		*total += sizeof(fru_file_mr_header_t);
		*total += listitem->rec->hdr.len;
		listitem = listitem->next;
	}

	area = calloc(1, *total);
	if (!area) {
		fru_errno = errno;
		*total = 0;
		return NULL;
	}

	// Walk the input records and pack them into an MR area
	listitem = reclist;
	rec = area;
	while (listitem && listitem->rec && listitem->rec->hdr.len) {
		size_t rec_sz = sizeof(fru_file_mr_header_t) + listitem->rec->hdr.len;
		memcpy(rec, listitem->rec, rec_sz);
		if (!listitem->next
		    || !listitem->next->rec
		    || !listitem->next->rec->hdr.len)
		{
			// Update the header and its checksum. Don't include the
			// checksum byte itself.
			size_t checksum_span = sizeof(fru_file_mr_header_t) - 1;
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

bool fru_decode_product_info(const fru__file_product_t *area,
                             fru_product_t *product_out)
{
	return fru_decode_area_info(FRU_PRODUCT_INFO, area, product_out);
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
 * @returns fru_file_t * buffer, a newly allocated buffer containing the created
 *                          FRU information file
 */
fru_file_t * fru_create(fru_area_t area[FRU_MAX_AREAS], size_t *size)
{
	fru_file_t fruhdr = { .ver = FRU_VER_1 };
	int totalblocks = FRU_BLOCKS(sizeof(fru_t)); // Start with just the header
	int area_offsets[FRU_MAX_AREAS] = {
		// Indices must match values of fru_area_type_t
		offsetof(fru_t, internal),
		offsetof(fru_t, chassis),
		offsetof(fru_t, board),
		offsetof(fru_t, product),
		offsetof(fru_t, multirec)
	};
	fru_file_t *out = NULL;
	int i;
	int cksum;

	// First calculate the total size of the FRU information storage file to
	// be allocated.
	for(i = 0; i < FRU_MAX_AREAS; i++) {
		fru_area_type_t atype = area[i].atype;
		uint8_t blocks = area[i].blocks;
		fru__file_area_t *data = area[i].data;

		// Area type must be valid and match the index
		if (!FRU_IS_ATYPE_VALID(atype) || atype != i) {
			fru_errno = FEAREABADTYPE;
			return NULL;
		}

		int area_offset_index = area_offsets[atype];
		uint8_t *offset = (uint8_t *)&fruhdr + area_offset_index;

		if(!data // No data is provided
		   || (!FRU_AREA_HAS_SIZE(atype) && !blocks) // or no size is given
		                                             // for a non-sized area
		   || (FRU_AREA_HAS_SIZE(atype) // or a sized area
		      && !((fru__file_area_t *)data)->blocks)) // contains a zero size
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
	if (!out) {
		fru_errno = errno;
		return NULL;
	}

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





#ifdef __STANDALONE__

void dump(int len, const char *data)
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
	fru_field_t test_strings[] = {
		/* 6-bit ASCII */
		{ FIELD_TYPE_AUTO, "IPMI" },
		{ FIELD_TYPE_AUTO, "OK!" },
		/* BCD plus */
		{ FIELD_TYPE_AUTO, "1234-56-7.89 01" },
		/* Simple text */
		{ FIELD_TYPE_AUTO, "This is a simple text, with punctuation & other stuff" },
		/* Binary (hex string) */
		{ FIELD_TYPE_BINARY, "010203040506070809" },
		/* Binary (true binary) */
		{ FIELD_TYPE_AUTO, "\x01\x02\x03\x04\x05 BINARY TEST"
	};
	char *test_types[] = {
		"6-bit", "6-bit",
		"BCPplus",
		"Simple text",
		"Binary hex string",
		"True Binary"
	};

	for(i = 0; i < FRU__ARRAY_SZ(test_strings); i++) {
		fru__file_field_t *field;
		const char out[FRU_FIELDMAXARRAY];

		printf("Data set %d.\n", i);
		printf("Original data ");
		if (test_lengths[i]) dump(test_lengths[i], test_strings[i]);
		else printf(": [%s]\n", test_strings[i]);

		printf("Original type: %s\n", test_types[i]);
		printf("Encoding... ");
		field = fru_encode_data(test_strings[i]);
		if (FRU_FIELD_TERMINATOR == field->typelen) {
			printf("FAIL!\n\n");
			continue;
		}

		printf("OK\n");
		printf("Encoded type is: ");
		switch((field->typelen & __TYPE_BITS_MASK) >> FRU__TYPE_BITS_SHIFT) {
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
		dump(FRU__FIELDSIZE(field->typelen), (uint8_t *)field);
		printf("Decoding... ");

		if (!fru_decode_data(field->&typelen, &field->data,
		                     FRU_FIELDMAXARRAY))
		{
			printf("FAIL!");
			goto next;
		}

		printf("Decoded data ");
		if (FRU__ISTYPE(field->typelen, BINARY)) {
			dump(FRU__FIELDLEN(field->typelen), out);
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
#endif // Old code
