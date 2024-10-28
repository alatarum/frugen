/** @file
 *  @brief Implementation of binary FRU loading functions
 *
 *  @copyright
 *  Copyright (C) 2016-2024 Alexander Amelkin <alexander@amelkin.msk.ru>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later OR Apache-2.0
 */

//#define DEBUG
#include "fru-private.h"

#include <assert.h>
#include <ctype.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * =========================================================================
 * Private functions for use in this file only
 * =========================================================================
 */

/** @cond PRIVATE */

/**
 * @brief Find and validate FRU header in the byte buffer.
 *
 * @param[in] buffer Byte buffer.
 * @param[in] size Byte buffer size.
 * @return Pointer to the FRU header in the buffer.
 * @retval NULL FRU header not found. \p errno is set accordingly.
 */
static
fru__file_t *find_fru_header(void *buffer, size_t size, fru_flags_t flags) {
	int cksum;
	if (size < FRU_BLOCK_SZ) {
		fru_errno = FETOOSMALL;
		return NULL;
	}
	fru__file_t *header = (fru__file_t *) buffer;
	if ((header->ver != FRU_VER_1)
	    || (header->rsvd != 0)
	    || (header->pad != 0))
	{
		fru_errno = FEHDRVER;
		if (!(flags & FRU_IGNFVER))
			return NULL;
	}
	/* Don't include the checksum byte into calculation */
	cksum = fru__calc_checksum(header, sizeof(fru__file_t) - 1);
	if (cksum < 0 || header->hchecksum != (uint8_t)cksum) {
		if (cksum >= 0) // Keep fru_errno if there was an error
			fru_errno = FEHDRCKSUM;
		if (!(flags & FRU_IGNFHCKSUM))
			return NULL;
	}
	return header;
}

/*
 * Get area size limit.
 *
 * Areas are not required by the specification to be distributed
 * across the FRU file in the same order as pointers to them in
 * the file header. The areas may as well be placed in random order.
 * Sometimes the next area may be placed much farther than the current
 * area ends, and there is extra space at the end of the current area.
 *
 * This function calculates the distnce from an area starting location
 * to the starting location of the next area in the file, or to the end
 * of the file itself. So to say, the size limit of the current area.
 * 
 * For some areas, such as Internal Use, that limit is the area's size.
 */
static
size_t get_area_limit(void *fru_file, size_t size, fru_area_type_t type, fru_flags_t flags)
{
	const off_t area_ptr_offset = offsetof(fru__file_t, internal) + type;
	const uint8_t * area_ptr = (void *)fru_file + area_ptr_offset;
	off_t area_offset = FRU_BYTES(*area_ptr);
	off_t next_area_offset = size; // Assume the 'type' area is the last one
	fru_file_t *header = (fru_file_t *)fru_file;

	if (!fru_fle) {
		fru_errno = FEGENERIC;
		errno = EFAULT;
		return 0;
	}

	/*
	 * Now we need to find the area closest by offset to the
	 * area of the given type
	 */
	for (fru_area_type_t idx = 0; idx < FRU_MAX_AREAS; ++idx) {
		const off_t next_area_ptr_offset = offsetof(fru__file_t, internal) + idx;
		const void * next_area_ptr = fru_file + next_area_ptr_offset;
		// Is this really the next area in the file?
		if (*next_area_ptr > *area_ptr
		    && area_offset < next_area_offset)
		{
			next_area_offset = FRU_BYTES(*next_area_ptr);
		}
	}

	/* An area must at least 1 byte long */
	if (area_offset + 1 > size) {
		fru_errno = FETOOSMALL;
		return 0;
	}

	return next_area_offset - area_offset;
}

static
fru_file_mr_area_t *find_mr_area(uint8_t *buffer, size_t *mr_size, size_t size,
                                 fru_flags_t flags)
{
	size_t limit;
	fru_file_t *header;

	if (!mr_size) {
		fru_errno = EFAULT;
		return NULL;
	}
	header = find_fru_header(buffer, size, flags);
	if (!header || !header->multirec) {
		return NULL;
	}
	if (FRU_BYTES(header->multirec) + sizeof(fru__file_mr_rec_t) > size) {
		fru_errno = ENOBUFS;
		return NULL;
	}
	fru_file_mr_area_t *area =
	    (fru_file_mr_area_t *)(buffer + FRU_BYTES(header->multirec));

	limit = size - ((uint8_t *)header - buffer);
	fru_errno = 0;
	*mr_size = fru_mr_area_size(area, limit, flags);
	if (!*mr_size)
	{
		if (!fru_errno)
			fru_errno = ENODATA;
		return NULL;
	}

	return area;
}

/**
 * Convert a binary byte into 2 bytes of hex string
 */
static
void byte2hex(void *buf, char byte)
{
	uint8_t *str = buf;
	if (!str) return;

	str[0] = nibble2hex(((byte & 0xf0) >> 4) & 0x0f);
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
bool decode_raw_binary(const void *in,
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
		byte2hex(out + 2 * i, buffer[i]);
	}

	return true;
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
bool decode_binary(const fru__file_field_t *field,
                   fru_field_t *out)
{
	return decode_raw_binary(field->data,
	                         FRU__FIELDLEN(field->typelen),
	                         out->val,
	                         sizeof(out->val));
}

/**
 * A helper function to decoding of custom fields in a
 * generic info area (chassis, board, product).
 *
 * @param[in] data Pointer to the first custom field in the FRU file area
 * @param[in] bytes The area type
 * @param[in, out] reclist Pointer to a linked list head pointer for decoded fields
 */
static
bool decode_custom_fields(fru_t * fru,
                          const char *data,
                          int limit,
                          fru_flags_t flags)
{
	fru__file_field_t *field = NULL; /* A pointer to an _encoded_ field */

	DEBUG("Decoding custom fields in %d bytes\n", limit);
	while (limit > 0) {
		fru_field_t outfield = {};
		field = (fru__file_field_t*)data;

		// end of fields
		if (field->typelen == FRU_FIELD_TERMINATOR) {
			DEBUG("End of fields found (bytes left %d)\n", limit);
			break;
		}


		if (!decode_field(outfield->val, field)) {
			DEBUG("Failed to decode custom field: %s\n", fru_strerr(fru_errno));
			free_reclist(*reclist);
			return false;
		}
		fru_add_custom(fru, atype, FRU__TYPE(field->typelen), outfield.val);

		size_t length = FRU__FIELDLEN(field->typelen)
		                + sizeof(fru__file_field_t);
		data += length;
		limit -= length;
	}

	if (bytes <= 0 && !(flags & FRU_IGNAEOF)) {
		DEBUG("Area doesn't contain an end-of-fields byte\n");
		fru_errno = FEAREANOEOF;
		return false;
	}

	DEBUG("Done decoding custom fields\n");
	return true;
}

/**
 * A helper function to decode an internal use area.
 * This function just copies the data as a hex string.

 * @param[in,out] fru Pointer to the FRU information structure to fill in
 * @param[in] atype The area type
 * @param[in] data_in Pointer to the encoded (source) data buffer
 */
static
bool decode_iu_area(fru_t * fru,
                    fru_area_type_t atype,
                    const void *data_in,
                    size_t data_size,
                    fru_flags_t flags)
{
	fru_internal_use_area_t * iu_area = data_in;

	data_size -= sizeof(iu_area->ver); // Only the data counts

	if (iu_area->ver != FRU_VER_1) {
		fru_errno = FEAREAVER;
		if(!(flags & FRU_IGNRVER))
			return false;
	}

	if (!fru_set_internal_binary(fru, iu_area->data, data_size))
		return false;

	fru->present[atype] = true;
	return true;
}

/**
 * A helper function to perform the actual decoding/explosion of any
 * generic info area (chassis, board, product).

 * They all have more or less similar layouts that can be accounted
 * for inside this function given the area type.
 *
 * @param[in,out] fru Pointer to the FRU information structure to fill in
 * @param[in] atype The area type
 * @param[in] data_in Pointer to the encoded (source) data buffer
 */
static
bool decode_info_area(fru_t * fru,
                      fru_area_type_t atype,
                      const void *data_in,
                      size_t data_size,
                      fru_flags_t flags)
{
	const fru__file_area_t * file_area = data_in;
	int bytes_left = FRU_BYTES(file_area->blocks); /* All generic areas have this */
	fru__file_field_t * field = NULL;
	int info_atype = atype - FRU_FIRST_INFO_AREA;

	if (!fru || !data_in) {
		errno = EFAULT;
		fru_errno = FEGENERIC;
		return false;
	}

	/* An area must at least contain a header */
	if (area_size < FRU_INFO_AREA_HEADER_SZ) {
		fru_errno = FETOOSMALL;
		return false;
	}

	/* Now check if there is there is enough data for what's
	 * specified in the area header */
	if (area_size < FRU_BYTES(file_area->blocks)) {
		fru_errno = FEHDRBADPTR;
		return false;
	}
	area_size = FRU_BYTES(file_area->blocks);

	/* Verify checksum, don't include the checksum byte itself */
	cksum = fru__calc_checksum((uint8_t *)file_area, area_size);
	if (cksum < 0) {
		if (fru_errno == FEGENERIC)
			return false;
	}
	else if (cksum) {
		fru_errno = FEAREACKSUM;
		if (!(flags & FRU_IGNACKSUM))
			return false;
	}

	struct info_area_s {
		uint8_t langtype; /* All fru__*_t area types have this */
		uint8_t data[];
	} * area_out[FRU_INFO_AREAS] = {
		(info_area_s *)&fru->chassis,
		(info_area_s *)&fru->board,
		(info_area_s *)&fru->product,
	};

	fru_field_t * out_field[FRU_MAX_AREAS][FRU_MAX_FIELD_COUNT] = {
		[FRU_CHASSIS_INFO] = {
			&fru->chassis->pn,
			&fru->chassis->serial,
		},
		[FRU_BOARD_INFO] = {
			&fru->board->mfg,
			&fru->board->pname,
			&fru->board->serial,
			&fru->board->pn,
			&fru->board->file,
		},
		[FRU_PRODUCT_INFO] = {
			&fru->product->mfg,
			&fru->product->pname,
			&fru->product->pn,
			&fru->product->ver,
			&fru->product->serial,
			&fru->product->atag,
			&fru->product->file,
		}
	};
	size_t field_count[FRU_MAX_AREAS] = {
		[FRU_CHASSIS_INFO] = FRU_CHASSIS_FIELD_COUNT,
		[FRU_BOARD_INFO] = FRU_BOARD_FIELD_COUNT,
		[FRU_PRODUCT_INFO] = FRU_PROD_FIELD_COUNT,
	};

	DEBUG("Decoding area type %d\n", atype);

	area_out[info_atype]->langtype = file_area->langtype;

	switch (atype) {
		case FRU_CHASSIS_INFO:
			field = (fru__file_field_t *)file_area->data;
			break;
		case FRU_PRODUCT_INFO:
			field = (fru__file_field_t *)file_area->data;
			break;
		case FRU_BOARD_INFO:
			/* Board area has a slightly different layout than the other
			 * generic areas, account for its specifics here */
			const fru__file_board_t *board = data_in;
			union {
				uint32_t val;
				uint8_t arr[4];
			} min_since_1996_big_endian = { 0 }; // BE is per FRU spec
			min_since_1996_big_endian.arr[1] = board->mfgdate[2];
			min_since_1996_big_endian.arr[2] = board->mfgdate[1];
			min_since_1996_big_endian.arr[3] = board->mfgdate[0];
			uint32_t min_since_1996 = be32toh(min_since_1996_big_endian.val);
			// The argument to mktime is zoneless
			board_out->tv.tv_sec = fru_datetime_base() + 60 * min_since_1996;
			field = (fru__file_field_t *)board->data;
			break;
		default:
			fru_errno FEAREANOTSUP;
			DEBUG("Attempt to decode unsupported area type (%d)\n", atype);
			return false;
	}

	for (size_t i = 0; i < field_count[atype]; i++) {
		if (!decode_field(field, out_field[atype][i]))
			return false;
		field = (void *)field += FRU__FIELDSIZE(field->typelen);
		bytes_left -= FRU__FIELDSIZE(field->typelen);
	}

	DEBUG("Decoded mandatory fields of area type %d\n", atype);
	return decode_custom_fields((void *)field, bytes_left, cust, flags);
}

/**
 * Check if FRU file MR record looks valid
 */
static
bool is_mr_rec_valid(fru__file_mr_rec_t * rec, size_t limit, fru_flags_t flags)
{
	int cksum;

	/* The record must have some data to be valid */
	if (!rec || limit <= sizeof(fru__file_mr_rec_t)) {
		fru_errno = FEMRNODATA;
		return false;
	}

	/*
	 * Each record that is not EOL must have a valid
	 * version, as well as valid checksums
	 */
	if (!IS_FRU_MR_VALID_VER(rec)) {
		fru_errno = FEMRVER;
		if (!(flags & FRU_IGNRVER))
			return false;
	}

	/* Check the header checksum, checksum byte included into header */
	cksum = fru__calc_checksum(rec, sizeof(fru_file_mr_header_t));
	if (cksum) {
		fru_errno = FEMRHCKSUM;
		if (!(flags & FRU_IGNRHCKSUM))
			return false;
	}

	if (FRU_MR_REC_SZ(rec) > limit) {
		fru_errno = FEGENERIC;
		errno = ENOBUFS;
		return false;
	}

	/* Check the data checksum, checksum byte not included into data */
	cksum = fru__calc_checksum(rec->data, rec->hdr.len);
	if (cksum != (int)rec->hdr.rec_checksum) {
		fru_errno = FEMRDCKSUM;
		if (!(flags & FRU_IGNRDCKSUM))
			return false;
	}

	return true;
}

/**
 * Get the address of the next MR record in MR area
 * starting with \a rec
 *
 * TODO: Is this really needed?
 */
static
fru__file_mr_rec_t * fru_mr_next_rec(fru__file_mr_rec_t * rec,
                                     size_t limit,
                                     fru_flags_t flags)
{
	if (!is_mr_rec_valid(rec, limit, flags))
		return NULL;

	if (IS_FRU_MR_END(rec))
		return NULL;

	return (void *)rec + FRU_MR_REC_SZ(rec);
}

/**
 * Convert a FRU file MR UUID Management record into an UUID user record
 */
static
bool decode_mr_mgmt_uuid(fru_mr_rec_t * rec,
                         fru__file_mr_mgmt_rec_t *file_rec,
                         fru_flags_t flags)
{
	size_t i;
	uuid_t uuid;

	if (!file_rec || !str) {
		fru_errno = FEGENERIC;
		errno = EFAULT;
		return false;
	}

	/* Is this really a Management System UUID record? */
	if (FRU_MR_MGMT_ACCESS != file_rec->hdr.type_id
	    || FRU_MR_VER != (file_rec->hdr.eol_ver & FRU_MR_VER_MASK)
	    || (UUID_SIZE + 1) != file_rec->hdr.len
	    || FRU_MR_MGMT_SYS_UUID != file_rec->subtype)
	{
		fru_errno = FEGENERIC;
		errno = EINVAL;
		return false;
	}

	*str = calloc(1, UUID_STRLEN_NONDASHED + 1);
	if (!str) {
		fru_errno = FEGENERIC;
		return false;
	}

	/* This is the reversed operation of uuid2rec, SMBIOS-compatible
	 * Little-Endian encoding in the input record is assumed.
	 * The resulting raw data will be Big Endian */
	memcpy(uuid.raw, file_rec->data, UUID_SIZE);
	uuid.time_hi_and_version = htobe16(le16toh(uuid.time_hi_and_version));
	uuid.time_low = htobe32(le32toh(uuid.time_low));
	uuid.time_mid = htobe16(le16toh(uuid.time_mid));

	/* Now convert the Big Endian byte array into a non-dashed UUID string.
	 * byte2hex() automatically terminates the string.
	 */
	for (i = 0; i < sizeof(uuid.raw); ++i) {
		byte2hex(rec->mgmt.data + i * 2, uuid.raw[i]);
	}

	return true;
}

static
bool decode_mr_mgmt(fru_mr_rec_t * rec,
                    void * data,
                    fru_flags_t flags)
{
	size_t minsize, maxsize;
	fru__file_mr_mgmt_rec_t *file_rec = data;

	if (!file_rec || !str) {
		fru_errno = FEGENERIC;
		errno = EFAULT;
		return -1;
	}

	/* Is this really a Management Access record and not a UUID subtype? */
	if (FRU_MR_MGMT_ACCESS != file_rec->hdr.type_id
	    || FRU_MR_VER != (file_rec->hdr.eol_ver & FRU_MR_VER_MASK)
	    || (FRU_MR_MGMT_MIN > file_rec->subtype)
	    || (FRU_MR_MGMT_MAX < file_rec->subtype))
	{
		return -FEMRMGMTTYPE;
	}

	/* Is the management record data size valid? */
	minsize = fru__mr_mgmt_minlen[MGMT_TYPE_ID(file_rec->subtype)];
	maxsize = fru__mr_mgmt_maxlen[MGMT_TYPE_ID(file_rec->subtype)];
	if (minsize > file_rec->hdr.len || file_rec->hdr.len > maxsize) {
		return -FEMRMGMTSIZE;
	}

	/* System GUID (UUID) record needs special treatment */
	if (FRU_MR_MGMT_SYS_UUID == file_rec->subtype) {
		return decode_mr_mgmt_uuid(rec, file_rec, flags);
	}

	/* All other records are just plain text */
	memcpy(rec->mgmt.data, file_rec->data, file_rec->hdr.len);

	return 0;
}

static
bool decode_mr_record(fru_mr_rec_t * rec,
                      fru__file_mr_rec_t * srec,
                      fru_flags_t flags)
{
	bool rc = false;

	fru_errno = FEMRNOTSUP;
	if (!rec) {
		fru_errno = FEGENERIC;
		goto out;
	}

	bool (* decode_rec[FRU_MR_TYPE_COUNT])(fru_mr_rec_t *,
	                                       void *,
	                                       fru_flags_t) =
	{
		[FRU_MR_MGMT_ACCESS] = decode_mr_mgmt,
		// TODO: Implement other decoders, add them all here
	};

	if (!decode_rec[srec->type]) {
		fru_errno = FRUMRNOTSUP;
		goto out;
	}

	rc = decode_rec[srec->type](rec, srec, flags);

out:
	return rc;
}

static
int decode_mr_area(fru_t * fru,
                   fru_area_type_t atype,
                   const void *data,
                   size_t limit, // FRU file size
                   fru_flags_t flags)
{
	const fru__file_mr_area_t * area = data;
	const fru__file_mr_rec_t * srec = (fru__file_mr_rec_t *)area;
	fru_mr_rec_t *rec;
	fru_mr_reclist_t ** reclist = NULL;
	size_t total = 0;
	int count = -1;
	int err = 0;
	bool rc = false;

	if (!fru) {
		err = EFAULT;
		goto out;
	}

	reclist = &fru->mr_reclist;
	if (*reclist) {
		/* The code below expects an empty reclist */
		err = EINVAL;
		goto out;
	}

	while (srec) {
		size_t rec_sz = FRU__MR_REC_SZ(srec);
		if (!is_mr_rec_valid(srec, limit - total, flags)) {
			if (!(flags & FRU_IGNRNOEOL))
				count = -1;
			break;
		}

		/* Alocate a new empty record and add it to the list */
		rec = fru_add_mr(fru, FRU_LIST_TAIL, NULL);
		if (!rec) { 
			count = -1;
			break;
		}

		if (!decode_mr_record(rec, srec, flags)) {
			count = -1;
			break;
		}

		count = (count < 0) ? 1 : count + 1;

		if (IS_FRU_MR_END(srec))
			break;

		srec = fru_mr_next_rec(srec, mr_size - total, flags);
		total += rec_sz;
	}

	rc = true;
	fru->present[atype] = true;

out:
	if (count < 0) {
		if (*reclist) {
			free_reclist(*reclist);
			*reclist = NULL;
		}
		fru->present[atype] = false;
		fru_errno = FEGENERIC;
		errno = err;
	}

	return rc;
}

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
bool strhexcpy(char ** outhexstr, const char * hexstr)
{
	size_t i;
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

	newhextr = realloc(newhexstr, 1, len + 1);
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

/**
 * @brief Decode a 6-bit ASCII string.
 *
 * @param[out] out Buffer to decode into.
 * @param[in] field Field to decode.
 * @retval true Success.
 * @retval false Failure.
 */
static
bool decode_6bit(fru_field_t *out,
                 const fru__file_field_t *field)
{
	const uint8_t *s6;
	size_t len, len6bit;
	size_t i, i6;

	if (!field) return false;

	len6bit = FRU__FIELDLEN(field->typelen);
	s6 = field->data;

	len = FRU_6BIT_FULLLENGTH(len6bit);
	/* Need space for nul-byte */
	/* This can never actually happen as fru_field_t.val is fixed size */
	assert(sizeof(out->val) < len + 1);

	for(i = 0, i6 = 0; i6 <= len6bit && i < len && s6[i6]; i++) {
		int byte = i % 4;

		switch(byte) {
			case 0:
				out->val[i] = s6[i6] & 0x3F;
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
		out->val[i] &= 0x3F;
		out->val[i] += ' ';
	}

	// Strip trailing spaces that could emerge when decoding a
	// string that was a byte shorter than a multiple of 4.
	cut_tail(out->val);
	DEBUG("6bit ASCII string of length %zd decoded: '%s'\n", len, out->val);

	return true;
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
bool decode_bcdplus(fru_field_t *out,
                    const fru__file_field_t *field)
{
	size_t i;
	uint8_t c;
	size_t len;

	len = 2 * FRU__FIELDLEN(field->typelen);
	/* Need space for nul-byte */
	/* This can never actually happen as fru_field_t.val is fixed size */
	assert(sizeof(out->val) < len + 1);

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
	DEBUG("BCD+ string of length %zd decoded: '%s'\n", len, out->val);

	return true;
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
bool decode_text(fru_field_t *out,
                 const fru__file_field_t *field)
{
	size_t len = FRU__FIELDLEN(field->typelen);
	/* Need space for nul-byte */
	/* This can never actually happen as fru_field_t.val is fixed size */
	assert(sizeof(out->val) < len + 1);

	if (len) {
		memcpy(out->val, field->data, len);
	}
	out->val[len] = 0; /* Terminate the string */
	DEBUG("text string of length %zd decoded: '%s'\n", len, out->val);

	return true;
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
static
bool decode_field(fru_field_t *out,
                  const fru__file_field_t *field)
{
	field_type_t type;
	bool (*decode[TOTAL_FIELD_TYPES])(const fru__file_field_t *,
	                                  fru_field_t *) =
	{
		[FIELD_TYPE_BINARY] = decode_binary,
		[FIELD_TYPE_BCDPLUS] = decode_bcdplus,
		[FIELD_TYPE_6BITASCII] = decode_6bit,
		[FIELD_TYPE_TEXT] = decode_text,
	};

	if (!field) {
		errno = EFAULT;
		fru_errno = FEGENERIC;
		return false;
	}

	type = FRU__FIELD_TYPE_T(field->typelen);

	if (type < FIELD_TYPE_MIN || type > FIELD_TYPE_MAX) {
		DEBUG("ERROR: Field encoding type is invalid (%d)\n", type);
		fru_errno = FEBADENC;
		return false;
	}

	if (field->typelen != FRU_FIELD_TERMINATOR) {
		DEBUG("The encoded field is marked as type %d, length is %zi (typelen %02x)\n",
		      type, FRU__FIELDLEN(field->typelen), field->typelen);
	}
	else {
		DEBUG("End-of-fields reached");
	}

	out->type = type;
	return decode[type](field, out);
}

/** @endcond */

/*
 * =========================================================================
 * Public API Functions
 * =========================================================================
 */

// See fru.h
fru_t * fru_loadbuffer(fru_t * init_fru,
                       const void * buf,
                       size_t size,
                       fru_flags_t flags)
{
	fru_t * fru = init_fru;
	fru__file_t * fru_file = (fru__file_t *)buf;
	fru_area_type type;
	bool (*decode_area[FRU_TOTAL_AREAS])(fru_t *, fru_area_type_t,
                                         const void *, size_t data,
                                         fru_flags_t) =
	{
		[FRU_INTERNAL_USE] = decode_iu_area,
		[FRU_CHASSIS_AREA] = decode_info_area,
		[FRU_BOARD_AREA] = decode_info_area,
		[FRU_PRODUCT_AREA] = decode_info_area,
		[FRU_MR_AREA] = decode_mr_area
	};

	if (!buf) {
		fru_errno = EFAULT;
		goto out;
	}

	fru_file = find_fru_header(buf, size, flags);
	if (!fru_file) {
		goto out;
	}

	if (!fru)
		fru = calloc(1, sizeof(fru_t));

	if (!fru) {
		fru_errno = errno;
		goto out;
	}
	/* After this point all exiting must be done via `goto err` */

	/* For each area type check its presence and mark fru.meta,
	 * then find the actual area and parse it into fru.data */
	for (type = FRU_MIN_AREA; type <= FRU_MAX_AREA; type++) {
		const off_t area_ptr_offset = offsetof(fru__file_t, internal) + type;
		const uint8_t * area_ptr = (void *)fru_file + area_ptr_offset;
		off_t area_offset = FRU_BYTES(*area_ptr);

		/* The header indicates absense of this specific area */
		if (!area_offset)
			continue;

		size_t area_limit = get_area_limit(fru_file, size, type, flags);
		if (!area_limit)
			goto err;

		const void * raw_area = buf + area_offset;
		if (!decode_area[type](fru, type, raw_area, area_limit, flags))
			goto err;
	}

	goto out;

err:
	// Don't free the supplied init_fru in case
	// it was staticaly allocated
	if (!init_fru)
		zfree(fru);
out:
	return fru;
}

// See fru.h
fru_t * fru_loadfile(fru_t * init_fru,
                     const char *filename,
                     fru_flags_t flags)
{
	size_t mr_size;
	int fd;
	fru_t * fru = NULL;

	if (!filename) {
		fru_errno = FEGENERIC;
		errno = EFAULT;
		goto out;
	}

	fd = open(fname, O_RDONLY);
	if (fd < 0) {
		fru_errno = FEGENERIC;
		goto out;
	}

	struct stat statbuf = {0};
	if (fstat(fd, &statbuf)) {
		fru_errno = FEGENERIC;
		goto err;
	}
	if (statbuf.st_size > FRU__MAX_FILE_SIZE && !(flags & FRU_IGNBIG)) {
		fru_errno = FETOOBIG;
		goto err;
	}

	buffer = mmap(NULL, statbuf.st_size, PROT_READ, 0, fd, 0);
	if (buffer == NULL) {
		close(fd);
		fru_errno = FEGENERIC;
		goto err;
	}
	fru = fru_loadbuffer(init_fru, buffer, statbuf.st_size, flags);
	munmap(buffer, statbuf.st_size);

err:
	close(fd);

out:
	return fru;
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

	hexstring = realloc(fru->internal, 1, out_len);
	if (!hexstring) {
		fru_errno = FEGENERIC;
		goto err;
	}

	if(!decode_raw_binary(buffer, size,
	                      hexstring, out_len))
	{
		zfree(hexstring);
		errno = ENOBUFS;
		fru_errno = FEGENERIC;
		goto err;
	}

	fru->internal = hexstring;
	fru->present[FRU_INTERNAL_USE] = true;
	rc = true;
err:
	return rc;
}

// See fru.h
bool fru_set_internal_hexstring(fru_t * fru, const void * hexstr)
{
	size_t len = 0;
	if (!fru || !hexstr) {
		fru_errno = FEGENERIC;
		errno = EFAULT;
		return false;
	}

	/* Don't touch presence flag of copy fails, old data is preserved */
	if(!strhexcpy(&fru->internal, hexstr))
		return false;

	fru->present[FRU_INTERNAL_USE] = true;
}

// See fru.h
void fru_free(fru_t * fru)
{
	if (!fru) return;

	zfree(fru->internal);
	free_reclist(fru->chassis.cust);
	free_reclist(fru->board.cust);
	free_reclist(fru->product.cust);
	free_reclist(fru->mr_reclist);
}
