/** @file
 *  @brief Implementation of binary FRU loading functions
 *
 *  @copyright
 *  Copyright (C) 2016-2025 Alexander Amelkin <alexander@amelkin.msk.ru>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later OR Apache-2.0
 */

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

//#define DEBUG
#include "fru-private.h"
#include "../fru_errno.h"

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
fru__file_t * find_fru_header(const void * buffer, size_t size, fru_flags_t flags) {
	int cksum;
	if (size < FRU__BLOCK_SZ) {
		fru__seterr(FE2SMALL, FERR_LOC_GENERAL, -1);
		return NULL;
	}
	fru__file_t *header = (fru__file_t *) buffer;
	if ((header->ver != FRU__VER)
	    || (header->rsvd != 0)
	    || (header->pad != 0))
	{
		fru__seterr(FEHDRVER, FERR_LOC_GENERAL, -1);
		if (!(flags & FRU_IGNFVER))
			return NULL;
	}
	/* Don't include the checksum byte into calculation */
	cksum = fru__calc_checksum(header, sizeof(fru__file_t) - 1);
	if (cksum < 0 || header->hchecksum != (uint8_t)cksum) {
		if (cksum >= 0) // Keep fru_errno if there was an error
			fru__seterr(FEHDRCKSUM, FERR_LOC_GENERAL, -1);
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
size_t get_area_limit(void *fru_file, size_t size, fru_area_type_t type)
{
	const off_t area_ptr_offset = offsetof(fru__file_t, internal) + type;
	const uint8_t * area_ptr = (void *)fru_file + area_ptr_offset;
	size_t area_offset = FRU__BYTES(*area_ptr);
	size_t next_area_offset = size; // Assume the 'type' area is the last one

	DEBUG("Detecting area limit for area %d within %zu bytes of FRU", type, size);

	if (!fru_file) {
		fru__seterr(FEGENERIC, type, -1);
		errno = EFAULT;
		return 0;
	}

	/*
	 * Now we need to find the area closest by offset to the
	 * area of the given type
	 */
	size_t last_area_offset = 0xFF;
	fru_area_type_t atype;
	FRU_FOREACH_AREA(atype) {
		if (atype == type) // Self-check is useless, skip
			continue;

		const off_t next_area_ptr_offset = offsetof(fru__file_t, internal) + atype;
		const uint8_t * next_area_ptr = fru_file + next_area_ptr_offset;

		DEBUG("Area %d is at %d", atype, *next_area_ptr);
		// Is this really the next area in the file?
		if (*next_area_ptr > *area_ptr
		    && area_offset < next_area_offset
		    && FRU__BYTES(*next_area_ptr) < last_area_offset)
		{
			next_area_offset = FRU__BYTES(*next_area_ptr);
			last_area_offset = next_area_offset;
			DEBUG("That's the actual next area");
		}
	}

	/* An area must be at least 1 byte long */
	if (area_offset + 1 > size) {
		fru__seterr(FE2SMALL, type, -1);
		return 0;
	}

	DEBUG("Our limit appears to be %zu bytes", next_area_offset - area_offset);
	return next_area_offset - area_offset;
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
                          fru_area_type_t atype,
                          const char *data,
                          int limit,
                          fru_flags_t flags)
{
	fru__file_field_t *field = NULL; /* A pointer to an _encoded_ field */
	size_t index = 0;

	DEBUG("Decoding custom fields in %d bytes", limit);
	while (limit > 0) {
		fru_field_t outfield = {};
		field = (fru__file_field_t*)data;

		// end of fields
		if (field->typelen == FRU__FIELD_TERMINATOR) {
			break;
		}
		if (!fru__decode_field(&outfield, field)) {
			fru_errno_t err = fru_errno;
			err.src = (fru_error_source_t)atype;
			err.index = index;
			DEBUG("Failed to decode custom field: %s",
			      fru_strerr(fru_errno));
			fru__reclist_t **cust = fru__get_customlist(fru, atype);
			if (cust)
				fru__free_reclist(cust);
			fru_errno = err;
			return false;
		}
		fru_add_custom(fru, atype, FRU_LIST_TAIL,
			       outfield.enc, outfield.val);

		size_t length = FRU__FIELDLEN(field->typelen)
		                + sizeof(fru__file_field_t);

		data += length;
		limit -= length;
		index++;
	}

	if (limit <= 0) {
		DEBUG("Area doesn't contain an end-of-fields byte");
		fru__seterr(FENOTERM, atype, -1);
		if(!(flags & FRU_IGNAEOF))
			return false;
	}

	DEBUG("Done decoding custom fields");
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
                    fru_area_type_t atype __attribute__((unused)),
                    const void *data_in,
                    size_t data_size,
                    fru_flags_t flags)
{
	const fru__file_internal_t * iu_area = data_in;

	DEBUG("Decoding %zu bytes of iu area @ %p", data_size, iu_area);

	data_size -= sizeof(iu_area->ver); // Only the data counts

	if (iu_area->ver != FRU__VER) {
		fru__seterr(FEHDRVER, FERR_LOC_INTERNAL, -1);
		if(!(flags & FRU_IGNAVER))
			return false;
	}

	if (!fru_set_internal_binary(fru, iu_area->data, data_size))
		return false;

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
	int bytes_left = FRU__BYTES(file_area->blocks); /* All generic areas have this */
	fru__file_field_t * field = NULL;
	int infoidx = FRU_ATYPE_TO_INFOIDX(atype);
	int cksum;

	DEBUG("Decoding %zu bytes of info area type %d @ %p", data_size, atype, file_area);

	if (!fru || !data_in) {
		errno = EFAULT;
		fru__seterr(FEGENERIC, atype, -1);
		return false;
	}

	/* An area must at least contain a header */
	if (data_size < FRU__INFO_AREA_HEADER_SZ) {
		fru__seterr(FE2SMALL, atype, -1);
		return false;
	}

	/* Now check if there is there is enough data for what's
	 * specified in the area header */
	if (data_size < (size_t)bytes_left) {
		fru__seterr(FEHDRBADPTR, atype, -1);
		return false;
	}

	/* Verify checksum, don't include the checksum byte itself */
	cksum = fru__calc_checksum((uint8_t *)file_area, bytes_left);
	if (cksum < 0) {
		fru_errno.src = (fru_error_source_t)atype;
		return false;
	}
	else if (cksum) {
		fru__seterr(FEDATACKSUM, atype, -1);
		if (!(flags & FRU_IGNACKSUM))
			return false;
	}

	fru__info_area_t * area_out[FRU_INFO_AREAS] = {
		(fru__info_area_t *)&fru->chassis,
		(fru__info_area_t *)&fru->board,
		(fru__info_area_t *)&fru->product,
	};

	fru_field_t * out_field[FRU_TOTAL_AREAS][FRU_MAX_FIELD_COUNT] = {
		[FRU_CHASSIS_INFO] = {
			&fru->chassis.pn,
			&fru->chassis.serial,
		},
		[FRU_BOARD_INFO] = {
			&fru->board.mfg,
			&fru->board.pname,
			&fru->board.serial,
			&fru->board.pn,
			&fru->board.file,
		},
		[FRU_PRODUCT_INFO] = {
			&fru->product.mfg,
			&fru->product.pname,
			&fru->product.pn,
			&fru->product.ver,
			&fru->product.serial,
			&fru->product.atag,
			&fru->product.file,
		}
	};
	size_t field_count[FRU_TOTAL_AREAS] = {
		[FRU_CHASSIS_INFO] = FRU_CHASSIS_FIELD_COUNT,
		[FRU_BOARD_INFO] = FRU_BOARD_FIELD_COUNT,
		[FRU_PRODUCT_INFO] = FRU_PROD_FIELD_COUNT,
	};

	DEBUG("Decoding area type %d", atype);

	if (file_area->ver != FRU__VER) {
		DEBUG("Wrong area version %d for area %d", (int)file_area->ver, atype);
		fru__seterr(FEHDRVER, atype, -1);
		if (!(flags & FRU_IGNAVER))
			return false;
	}

	area_out[infoidx]->langtype = file_area->langtype;
	bytes_left -= FRU__INFO_AREA_HEADER_SZ; // Language/type byte is a part of the header

	switch (atype) {
		case FRU_CHASSIS_INFO:
			field = (fru__file_field_t *)file_area->data;
			break;
		case FRU_PRODUCT_INFO:
			field = (fru__file_field_t *)file_area->data;
			break;
		case FRU_BOARD_INFO: {
			/* Board area has a slightly different layout than the other
			 * generic areas, account for its specifics here */
			const fru__file_board_t *board = data_in;
			fru_board_t * board_out = (fru_board_t *)area_out[infoidx];
			const struct timeval tv_unspecified = { 0 };
			union {
				uint32_t val;
				uint8_t arr[4];
			} fru_time_le = { 0 }; // little endian per FRU spec
			// Copy 24-bit source to 32-bit storage (both little-endian):
			// Source: X 0 1 2
			// Dest  : X 0 1 2
			fru_time_le.arr[0] = board->mfgdate[0];
			fru_time_le.arr[1] = board->mfgdate[1];
			fru_time_le.arr[2] = board->mfgdate[2];
			uint32_t fru_time = le32toh(fru_time_le.val);

			// The argument to mktime is zoneless
			board_out->tv.tv_sec = fru__datetime_base() + 60 * fru_time;

			// If some specific manufacturing date/time is given in the file,
			// disable automatic generation of timestamp on save
			if (memcmp(&tv_unspecified, &board_out->tv, sizeof(struct timeval)))
				board_out->tv_auto = false;

			bytes_left -= sizeof(board->mfgdate);

			field = (fru__file_field_t *)board->data;
			}
			break;
		default:
			fru__seterr(FEAREANOTSUP, FERR_LOC_GENERAL, atype);
			DEBUG("Attempt to decode unsupported area type (%d)", atype);
			return false;
	}

	for (size_t i = 0; i < field_count[atype]; i++) {
		if (!fru__decode_field(out_field[atype][i], field)) {
			fru_errno.src = (fru_error_source_t)atype;
			fru_errno.index = i;
			return false;
		}

		bytes_left -= FRU__FIELDSIZE(field->typelen);
		field = (void *)field + FRU__FIELDSIZE(field->typelen);
	}

	DEBUG("Decoded mandatory fields of area type %d", atype);
	return decode_custom_fields(fru, atype, (void *)field, bytes_left, flags);
}

/**
 * Check if FRU file MR record looks valid
 */
static
bool is_mr_rec_valid(const fru__file_mr_rec_t * rec, size_t limit, fru_flags_t flags)
{
	int cksum;

	/* The record must have some data to be valid */
	if (!rec || limit <= sizeof(fru__file_mr_rec_t)) {
		fru__seterr(FENODATA, FERR_LOC_MR, -1);
		return false;
	}

	/*
	 * Each record that is not EOL must have a valid
	 * version, as well as valid checksums
	 */
	if (!FRU__IS_MR_VALID_VER(rec)) {
		fru__seterr(FEHDRVER, FERR_LOC_MR, -1);
		if (!(flags & FRU_IGNRVER))
			return false;
	}

	/* Check the header checksum, checksum byte included into header */
	cksum = fru__calc_checksum(rec, sizeof(fru__file_mr_header_t));
	if (cksum) {
		fru__seterr(FEHDRCKSUM, FERR_LOC_MR, -1);
		if (!(flags & FRU_IGNRHCKSUM))
			return false;
	}

	if (FRU__MR_REC_SZ(rec) > limit) {
		fru__seterr(FEGENERIC, FERR_LOC_MR, -1);
		errno = ENOBUFS;
		return false;
	}

	/* Check the data checksum, checksum byte not included into data */
	cksum = fru__calc_checksum(rec->data, rec->hdr.len);
	if (cksum != (int)rec->hdr.rec_checksum) {
		fru__seterr(FEDATACKSUM, FERR_LOC_MR, -1);
		if (!(flags & FRU_IGNRDCKSUM))
			return false;
	}

	return true;
}

/**
 * Convert a FRU file MR UUID Management record into an UUID user record
 */
static
bool decode_mr_mgmt_uuid(fru_mr_rec_t * rec,
                         const fru__file_mr_mgmt_rec_t *file_rec)
{
	size_t i;
	fru__uuid_t uuid;

	/* Is this really a Management System UUID record? */
	if (FRU__MGMT_MR_DATASIZE(FRU__UUID_SIZE) != file_rec->hdr.len)
	{
		fru__seterr(FEBADDATA, FERR_LOC_MR, -1);
		return false;
	}

	/* This is the reversed operation of uuid2rec, SMBIOS-compatible
	 * Little-Endian encoding in the input record is assumed.
	 * The resulting raw data will be Big Endian */
	memcpy(uuid.raw, file_rec->data, FRU__UUID_SIZE);
	uuid.time_hi_and_version = htobe16(le16toh(uuid.time_hi_and_version));
	uuid.time_low = htobe32(le32toh(uuid.time_low));
	uuid.time_mid = htobe16(le16toh(uuid.time_mid));

	/* Now convert the Big Endian byte array into a non-dashed UUID string.
	 * fru__byte2hex() automatically terminates the string.
	 */
	for (i = 0; i < sizeof(uuid.raw); ++i) {
		fru__byte2hex(rec->mgmt.data + i * 2, uuid.raw[i]);
	}

	rec->mgmt.subtype = file_rec->subtype;

	return true;
}

static
bool decode_mr_mgmt(fru_mr_rec_t * rec,
                    const void * data,
                    fru_flags_t flags)
{
	size_t minsize, maxsize;
	const fru__file_mr_mgmt_rec_t *file_rec = data;

	if (!FRU_MR_MGMT_IS_SUBTYPE_VALID(file_rec->subtype)) {
		fru__seterr(FEMRMGMTBAD, FERR_LOC_MR, -1);
		return false;
	}

	/* Check if the management record data size is valid */
	minsize = fru__mr_mgmt_minlen[FRU_MR_MGMT_SUBTYPE_TO_IDX(file_rec->subtype)];
	minsize = FRU__MGMT_MR_DATASIZE(minsize);

	maxsize = fru__mr_mgmt_maxlen[FRU_MR_MGMT_SUBTYPE_TO_IDX(file_rec->subtype)];
	maxsize = FRU__MGMT_MR_DATASIZE(maxsize);

	if (minsize > file_rec->hdr.len || file_rec->hdr.len > maxsize) {
		fru__seterr(FESIZE, FERR_LOC_MR, -1);
		if (!(flags & FRU_IGNMRDATALEN))
			return false;
	}

	/* System GUID (UUID) record needs special treatment */
	if (FRU_MR_MGMT_SYS_UUID == file_rec->subtype) {
		return decode_mr_mgmt_uuid(rec, file_rec);
	}

	/* All other records are just plain text */
	memcpy(rec->mgmt.data, file_rec->data, file_rec->hdr.len);
	rec->mgmt.subtype = file_rec->subtype;

	return true;
}

/*
 * Decode any yet unsupported type of MR record
 * as a `raw` type
 */
static
bool decode_mr_raw(fru_mr_rec_t * rec,
                   const void * data,
                   fru_flags_t flags __attribute__((__unused__)))
{
	const fru__file_mr_rec_t *file_rec = data;

	rec->type = FRU_MR_RAW;
	rec->raw.type = file_rec->hdr.type_id;
	rec->raw.enc = FRU_FE_TEXT; // Assume text mode initially
	for (size_t i = 0; i < file_rec->hdr.len; i++) {
		if (!isprint(file_rec->data[i])) {
			rec->raw.enc = FRU_FE_BINARY;
			break;
		}
	}

	size_t maxsize = FRU_MIN(file_rec->hdr.len, FRU_MRR_RAW_MAXDATA - 1);
	if (FRU_FE_TEXT == rec->raw.enc) {
		memcpy(rec->raw.data, file_rec->data, maxsize);
		rec->raw.data[maxsize] = 0; // Terminate the string
	}
	else {
		fru__decode_raw_binary(file_rec->data, file_rec->hdr.len,
		                       rec->raw.data, FRU_MRR_RAW_MAXDATA - 1);
	}

	return true;
}

static
bool decode_mr_record(fru_mr_rec_t * rec,
                      const fru__file_mr_rec_t * srec,
                      fru_flags_t flags)
{
	bool rc = false;
	fru__seterr(FEMRNOTSUP, FERR_LOC_MR, -1);

	size_t type_id = srec->hdr.type_id;
	bool (* decode_rec[FRU_MR_TYPE_COUNT])(fru_mr_rec_t *,
	                                       const void *,
	                                       fru_flags_t) =
	{
		[FRU_MR_MGMT_ACCESS] = decode_mr_mgmt,
		// TODO: Implement other decoders, add them all here
	};

	if (type_id >= FRU_ARRAY_SZ(decode_rec)) {
		goto out;
	}

	if (decode_rec[type_id]) {
		rc = decode_rec[type_id](rec, srec, flags);
		if (rc) {
			rec->type = type_id;
		}
	}
	else {
		// Decode all unsupported types as `raw`
		rc = decode_mr_raw(rec, srec, flags);
	}

out:
	return rc;
}

static
bool decode_mr_area(fru_t * fru,
                    fru_area_type_t atype __attribute__((unused)),
                    const void *data,
                    size_t limit, // FRU file size
                    fru_flags_t flags)
{
	const fru__file_mr_rec_t * srec = (fru__file_mr_rec_t *)data;
	fru_mr_rec_t *rec;
	fru__mr_reclist_t ** reclist = NULL;
	size_t total = 0;
	int count = -1;
	bool rc = false;

	fru_clearerr();

	reclist = (fru__mr_reclist_t **)&fru->mr;
	if (*reclist) {
		/* The code below expects an empty reclist */
		fru__seterr(FENOTEMPTY, FERR_LOC_MR, -1);
		goto out;
	}

	while (srec) {
		size_t rec_sz = FRU__MR_REC_SZ(srec);
		if (!is_mr_rec_valid(srec, limit - total, flags)) {
			fru_errno.index = count;
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
			fru_errno.index = count;
			count = -1;
			break;
		}

		count = (count < 0) ? 1 : count + 1;
		total += rec_sz;

		if (FRU__IS_MR_END(srec))
			break;

		srec = (void *)srec + rec_sz;
	}

	if (count >= 0) {
		rc = true;
	}

out:
	if (count < 0) {
		if (*reclist) {
			fru__free_reclist(reclist);
			*reclist = NULL;
		}
	}

	return rc;
}

struct area_order_s {
	fru_area_type_t type;
	off_t offset;
};

/* Sort the area_order array to reflect the actual order
 * of the areas in the file */
static void sort_areas_by_offset(struct area_order_s *area_order)
{
	for (int pos = FRU_MIN_AREA + 1; pos <= FRU_MAX_AREA; pos++) {
		int pos2, newpos;
		struct area_order_s this_ao = area_order[pos];
		for (pos2 = pos - 1, newpos = pos; pos2 >= FRU_MIN_AREA; pos2--) {
			if (this_ao.offset < area_order[pos2].offset) {
				/* Move all from pos2 to newpos-1 one position to the right */
				for (int old = newpos; old > pos2; --old) {
					area_order[old] = area_order[old - 1];
				}
				area_order[pos2] = this_ao;
				newpos = pos2;
			}
		}
	}
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
	fru_t * fru = NULL;
	fru__file_t * fru_file = (fru__file_t *)buf;
	fru_area_type_t atype;
	bool (*decode_area[FRU_TOTAL_AREAS])(fru_t *, fru_area_type_t,
                                         const void *, size_t,
                                         fru_flags_t) =
	{
		[FRU_INTERNAL_USE] = decode_iu_area,
		[FRU_CHASSIS_INFO] = decode_info_area,
		[FRU_BOARD_INFO] = decode_info_area,
		[FRU_PRODUCT_INFO] = decode_info_area,
		[FRU_MR] = decode_mr_area
	};

	if (!buf) {
		fru__seterr(FEGENERIC, FERR_LOC_GENERAL, -1);
		errno = EFAULT;
		goto out;
	}

	fru_file = find_fru_header(buf, size, flags);
	if (!fru_file) {
		goto out;
	}

	if (!init_fru) {
		fru = calloc(1, sizeof(fru_t));
		if (!fru) {
			fru__seterr(FEGENERIC, FERR_LOC_GENERAL, -1);
			goto out;
		}
	}
	else {
		fru = init_fru;
	}

	/* After this point all exiting must be done via `goto err` */

	struct area_order_s area_order[FRU_TOTAL_AREAS] = { 0 };

	/* For each area type check its presence and mark fru.meta,
	 * then find the actual area and parse it into fru.data */
	FRU_FOREACH_AREA(atype) {
		const off_t area_ptr_offset = offsetof(fru__file_t, internal) + atype;
		const uint8_t * area_ptr = (void *)fru_file + area_ptr_offset;
		off_t area_offset = FRU__BYTES(*area_ptr);

		/* The indices to this array are actually just sequential
		 * numbers that just happen to be initially equal to `atype`.
		 * We are going to sort this array by offset later when it
		 * is fully filled */
		area_order[atype].type = atype;
		area_order[atype].offset = area_offset;

		/* The header indicates absense of this specific area */
		if (!area_offset)
			continue;

		size_t area_limit = get_area_limit(fru_file, size, atype);
		if (!area_limit)
			goto err;

		const void * raw_area = buf + area_offset;
		if (!decode_area[atype](fru, atype, raw_area, area_limit, flags))
			goto err;

		/*
		 * Don't use fru_enable_area() here to save on sorting that
		 * will be redone later anyway according to area offsets
		 * read from the header
		 */
		fru->present[atype] = true;
	}

	sort_areas_by_offset(area_order);

	/* Save the result into fru->order */
	int pos;
	FRU_FOREACH_AREA(pos) {
		fru->order[pos] = area_order[pos].type;
	}

	goto out;

err:
	// Don't free the supplied init_fru in case
	// it was staticaly allocated
	if (!init_fru)
		zfree(fru);
	fru = NULL;
out:
	return fru;
}

// See fru.h
fru_t * fru_loadfile(fru_t * init_fru,
                     const char *filename,
                     fru_flags_t flags)
{
	int fd;
	fru_t * fru = NULL;
	void * buffer;

	if (!filename) {
		fru__seterr(FEGENERIC, FERR_LOC_GENERAL, -1);
		errno = EFAULT;
		goto out;
	}

	fd = open(filename, O_RDONLY);
	DEBUG("open() == %d", fd);
	if (fd < 0) {
		fru__seterr(FEGENERIC, FERR_LOC_GENERAL, -1);
		goto out;
	}

	struct stat statbuf = {0};
	if (fstat(fd, &statbuf)) {
		fru__seterr(FEGENERIC, FERR_LOC_GENERAL, -1);
		goto err;
	}
	DEBUG("st_size == %zd", statbuf.st_size);
	if (statbuf.st_size > FRU__MAX_FILE_SIZE && !(flags & FRU_IGNBIG)) {
		fru__seterr(FE2BIG, FERR_LOC_GENERAL, -1);
		goto err;
	}

	buffer = mmap(NULL, statbuf.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	if (buffer == MAP_FAILED) {
		fru__seterr(FEGENERIC, FERR_LOC_GENERAL, -1);
		goto err;
	}
	DEBUG("loading into buffer @ %p", buffer);
	fru = fru_loadbuffer(init_fru, buffer, statbuf.st_size, flags);
	munmap(buffer, statbuf.st_size);

err:
	int err = errno; // Preserve
	close(fd);
	errno = err;

out:
	return fru;
}
