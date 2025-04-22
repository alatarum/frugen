/** @file
 *  @brief Implementation of binary FRU saving functions
 *
 *  @copyright
 *  Copyright (C) 2016-2024 Alexander Amelkin <alexander@amelkin.msk.ru>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later OR Apache-2.0
 */
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
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

/*
 * Save blob data into an MR record, update record header.
 *
 * Doesn't checksum the header, the caller must do it after setting
 * the EOL flag if needed.
 */
static
bool mr_blob2rec(fru__file_mr_rec_t * rec,
                 size_t * size,
                 const void * blob,
                 size_t len,
                 fru_mr_type_t type)
{
	uint8_t mr_blob[sizeof(fru__file_mr_rec_t) + FRU__FILE_MRR_MAXDATA] = {};
	fru__file_mr_rec_t * local_rec = (fru__file_mr_rec_t *)mr_blob;

	if (len > FRU__FILE_MRR_MAXDATA) {
		fru__seterr(FE2BIG, FERR_LOC_MR, -1);
		return false;
	}

	local_rec->hdr.type_id = type;
	local_rec->hdr.eol_ver = FRU__MR_VER;
	local_rec->hdr.len = len;

	memcpy(local_rec->data, blob, FRU_MIN(FRU__FILE_MRR_MAXDATA, len));

	// Checksum the data
	int cksum = fru__calc_checksum(local_rec->data, len);
	if (cksum < 0) {
		fru_errno.src = (fru_error_source_t)FERR_LOC_MR;
		return false;
	}
	local_rec->hdr.rec_checksum = (uint8_t)cksum;

	*size = sizeof(*local_rec) + local_rec->hdr.len;
	if (rec) {
		memcpy(rec, local_rec, *size);
	}

	return true;
}

/*
 * Save blob data into an MR Management Access record, update record header
 */
static
bool mgmt_blob2rec(fru__file_mr_rec_t * rec,
                   size_t * size,
                   const void * blob,
                   size_t len,
                   fru_mr_mgmt_type_t subtype)
{
	size_t min, max;
	uint8_t mgmt_blob[sizeof(fru__file_mr_mgmt_rec_t) + FRU__FILE_MR_MGMT_MAXDATA] = {};
	fru__file_mr_mgmt_rec_t * local_rec = (fru__file_mr_mgmt_rec_t *)mgmt_blob;

	min = fru__mr_mgmt_minlen[FRU_MR_MGMT_SUBTYPE_TO_IDX(subtype)];
	max = fru__mr_mgmt_maxlen[FRU_MR_MGMT_SUBTYPE_TO_IDX(subtype)];

	if (min > len || len > max || len > FRU__FILE_MR_MGMT_MAXDATA) {
		fru__seterr(FESIZE, FERR_LOC_MR, -1);
		return false;
	}

	/* Prepend the raw data blob with management subtype */
	local_rec->subtype = subtype;
	memcpy(local_rec->data, blob, len);

	/* Now pack it into a generic MR record starting with the subtype byte */
	return mr_blob2rec(rec, size, &local_rec->subtype, len + 1, FRU_MR_MGMT_ACCESS);
}



/**
 * Take an input string, check that it looks like UUID, and pack it into
 * an "exploded" multirecord area record in binary form.
 *
 * @returns An errno-like negative error code
 * @retval 0        Success
 * @retval -EINVAL  Invalid UUID string (wrong length, wrong symbols)
 * @retval -EFAULT  Invalid pointer
 * @retval >0       any errno that calloc() is allowed to set
 */
static
bool uuid2rec(fru__file_mr_rec_t * rec, size_t * size, const char * str)
{
	size_t len;
	fru__uuid_t uuid;

	len = strlen(str);

	if(FRU__UUID_STRLEN_DASHED != len && FRU__UUID_STRLEN_NONDASHED != len) {
		DEBUG("Invalid UUID string length\n");
		fru__seterr(FESIZE, FERR_LOC_MR, -1);
		return false;
	}

	/*
	 * Fill in uuid.raw with bytes encoded from the input string
	 * as they come from left to right. The result is Big Endian.
	 *
	 * Checking the source string length is not sufficient as it
	 * may contain hex digits instead of dashes, which would result
	 * in overflow of uuid buffer. Hence, limit the output size.
	 */
	len = sizeof(uuid);
	if(!fru__hexstr2bin(&uuid.raw, &len, FRU__HEX_RELAXED, str)) {
		return false;
	}

	/*
	 * Per SMBIOS specification, the first three fields of UUID must be Little-Endian,
	 * see DSP0134 Section 7.2.1.
	 */
	uuid.time_low = htole32(be32toh(uuid.time_low));
	uuid.time_mid = htole16(be16toh(uuid.time_mid));
	uuid.time_hi_and_version = htole16(be16toh(uuid.time_hi_and_version));

	return mgmt_blob2rec(rec, size, &uuid.raw, sizeof(uuid),
	                     FRU_MR_MGMT_SYS_UUID);
}

static
bool encode_mr_mgmt_record(void * outbuf, size_t * size, fru_mr_rec_t * rec)
{
	assert(rec);

	fru_mr_mgmt_type_t subtype = rec->mgmt.subtype;
	if (!FRU_MR_MGMT_IS_SUBTYPE_VALID(subtype)) {
		fru__seterr(FEMRMGMTBAD, FERR_LOC_MR, -1);
		return false;
	}

	/* System GUID (UUID) record needs special treatment */
	if (FRU_MR_MGMT_SYS_UUID == subtype) {
		return uuid2rec(outbuf, size, rec->mgmt.data);
	}

	/* All other records are just plain text */
	return mgmt_blob2rec(outbuf, size, rec->mgmt.data,
	                     strlen(rec->mgmt.data), subtype);
}

static
bool encode_mr_raw_record(void * outbuf, size_t * size, fru_mr_rec_t * rec)
{
	size_t bytes = 0;
	fru__file_mr_rec_t * file_rec = outbuf;
	if (FRU_MR_RAW != rec->type) {
		return false;
	}

	if (file_rec) {
		file_rec->hdr.eol_ver = FRU__MR_VER;
		file_rec->hdr.type_id = rec->raw.type;
	}
	if (FRU_FE_TEXT == rec->raw.enc) {
		size_t len = strlen(rec->raw.data);
		memcpy(file_rec->data, rec->raw.data, len);
		if (file_rec)
			// Length can never be exceeded due to definition of fru_mr_rec_t
			file_rec->hdr.len = (uint8_t)len;
	}
	else {
		DEBUG("Calling hexstr2bin(%p, %p = %zu, ...)", outbuf ? file_rec->data : NULL, size, *size);
		if (!fru__hexstr2bin(outbuf ? file_rec->data : NULL,
		                     &bytes, FRU__HEX_RELAXED, rec->raw.data))
		{
			return false;
		}
		if (file_rec) {
			// Length can never be exceeded due to definition of fru_mr_rec_t
			file_rec->hdr.len = (uint8_t)bytes;
		}
	}

	if (file_rec) {
		file_rec->hdr.rec_checksum = fru__calc_checksum(file_rec->data, file_rec->hdr.len);
		file_rec->hdr.hdr_checksum = fru__calc_checksum(&file_rec->hdr, FRU__FILE_MR_HDR_CHECKED_SIZE);
	}

	*size = bytes + sizeof(*file_rec);

	return true;
}

static
bool encode_mr_record(void * outbuf, size_t * size, fru_mr_rec_t * rec, bool last)
{
	size_t bytes = 0;
	bool (* encode_rec[FRU_MR_TYPE_COUNT])(void *,
	                                       size_t *,
	                                       fru_mr_rec_t *) =
	{
		[FRU_MR_MGMT_ACCESS] = encode_mr_mgmt_record,
		// TODO: Implement encoders for other MR types, add them all here
		[FRU_MR_RAW] = encode_mr_raw_record,
	};

	if (!encode_rec[rec->type]) {
		DEBUG("MR Record type 0x%02X is not supported yet\n", rec->type);
		fru__seterr(FEMRNOTSUP, FERR_LOC_MR, -1);
		return false;
	}

	bool rc = encode_rec[rec->type](outbuf, &bytes, rec);
	/* Update the header checksum and set the EOL flag if needed */
	if (outbuf && rc) {
		fru__file_mr_header_t * hdr = outbuf;
		int cksum;


		if (last) {
			hdr->eol_ver |= FRU__MR_EOL;
		}

		/* Checksum the header, don't include the checksum byte itself */
		cksum = fru__calc_checksum(hdr, sizeof(*hdr) - 1); // Can't fail here
		hdr->hdr_checksum = (uint8_t)cksum;
	}

	*size = bytes;
	return rc;
}


/*
 * The following encode_*_area functions define area encoders for all types of
 * areas. Presence flag for the corresponding area must be checked by the
 * caller. These functions simply encode the source data into the destination
 * binary buffer and report the raw binary area size in bytes.
 *
 * When called with area_out==NULL, these functions will just calculate the size
 * needed to encode the area.
 */
static
bool encode_iu_area(void * area_out, size_t * size,
                    fru_area_type_t atype __attribute__((__unused__)),
                    const fru_t * fru)
{
	fru__file_internal_t * internal = area_out;
	size_t bytesize = 0;

	if (internal) {
		if (!fru__hexstr2bin(internal->data, &bytesize, FRU__HEX_RELAXED, fru->internal)) {
			fru_errno.src = (fru_error_source_t)FERR_LOC_INTERNAL;
			return false;
		}
		internal->ver = FRU__VER;
	}
	else {
		/* Just calculate the size */
		if(!fru__hexstr2bin(NULL, &bytesize, FRU__HEX_RELAXED, fru->internal)) {
			fru_errno.src = (fru_error_source_t)FERR_LOC_INTERNAL;
			return false;
		}
	}
	*size = FRU__BLOCK_ALIGN(bytesize + sizeof(internal->ver));
	if (internal) {
		// Ensure the unused tail of the area is not some garbage
		memset(internal->data + bytesize, 0, *size - bytesize);
	}
	return true;
}

static
bool add_field_to_area(void * area_out,
                       size_t * offset,
                       const fru_field_t * in_field)
{
	uint8_t buf[FRU__FILE_FIELD_MAXSIZE];
	fru__file_field_t * local_outfield = (fru__file_field_t *)buf;

	fru_clearerr();
	if (!fru__encode_field(local_outfield,
	                       in_field->enc,
	                       in_field->val))
	{
		return false;
	}

	// Copy the data to the output buffer if any
	if (area_out) {
		memcpy(area_out + *offset, local_outfield,
			   FRU__FIELDSIZE(local_outfield->typelen));
	}

	// Update offset if pointer given, even if data wasn't copied.
	// This is for evaluating the output size.
	if (offset)
		*offset += FRU__FIELDSIZE(local_outfield->typelen);

	return true;
}


static
bool encode_info_area(void * area_out, size_t * size,
                      fru_area_type_t atype, const fru_t * fru)
{
	fru__file_area_t * file_area = area_out;
	int info_atype = atype - FRU_FIRST_INFO_AREA;
	fru__file_board_t * board = (fru__file_board_t *)area_out;
	size_t bytes = 0; // Counter for the output area size in bytes,
	                  // don't spoil *size until everything is
	                  // known to be success

	fru__info_area_t * info[FRU_INFO_AREAS] = {
		(fru__info_area_t *)&fru->chassis,
		(fru__info_area_t *)&fru->board,
		(fru__info_area_t *)&fru->product,
	};

	const fru_field_t * in_field[FRU_TOTAL_AREAS][FRU_MAX_FIELD_COUNT] = {
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

	fru__reclist_t * cust_head[FRU_INFO_AREAS] = {
		fru->chassis.cust,
		fru->board.cust,
		fru->product.cust
	};

	bytes = FRU__INFO_AREA_HEADER_SZ;
	if (file_area) {
		file_area->ver = FRU__VER;
		// Intentionally do not restrict the range for chassis type or language code here.
		// SMBIOS specis get updated quite often, I don't want to keep track of that.
		// Some language codes may still use the printable ASCII+Latin1 range, I don't
		// feel like delving into those depths. Proper support for languages would imply
		// using wchars, which I don't want to do now. Most FRUs use ASCII+Latin1 anyway.
		file_area->langtype = info[info_atype]->langtype;
	}

	if (FRU__AREA_HAS_DATE(atype)) {
		const struct timeval tv_unspecified = { 0 };
		struct timeval tv_toset = fru->board.tv;
		time_t fru_time_base = fru__datetime_base();
		uint32_t fru_time;
		union {
			uint32_t val;
			uint8_t arr[4];
		} fru_time_le = { 0 }; // little-endian per FRU spec

		if (fru->board.tv_auto) {
			tzset();
			gettimeofday(&tv_toset, NULL);
			tv_toset.tv_sec += timezone;
		} else if (tv_toset.tv_sec < fru_time_base) {
			fru__seterr(FEBDATE, FERR_LOC_BOARD, -1);
			return false;
		}

		/* Yes, there is an upper limit and it's soon */
		if (tv_toset.tv_sec > FRU_DATETIME_MAX) {
			fru__seterr(FEBDATE, FERR_LOC_BOARD, -1);
			return false;
		}

		/*
		 * UNIX time 0 (Jan 1st of 1970) can never actually happen in a system,
		 * provided that this code is written in 2024.
		 */
		if (!memcmp(&tv_unspecified, &tv_toset, sizeof(struct timeval))) {
			DEBUG("Using FRU__DATE_UNSPECIFIED\n");
			fru_time = FRU__DATE_UNSPECIFIED;
		}
		else {
			// FRU time is in minutes and we don't care about microseconds
			fru_time = (tv_toset.tv_sec - fru__datetime_base()) / 60;
		}
		fru_time_le.val = htole32(fru_time);
		// Save 32-bit LE integer into 24-bit LE integer, loose the MSB
		// Source: 0 1 2 3 // MSB at offset 3
		// Dest  : 0 1 2 X // MSB at offset 2
		if (board) {
			board->mfgdate[0] = fru_time_le.arr[0];
			board->mfgdate[1] = fru_time_le.arr[1];
			board->mfgdate[2] = fru_time_le.arr[2];
		}
		bytes = FRU__DATE_AREA_HEADER_SZ; // Expand the header size
	}

	/* Encode mandatory fields */
	size_t i;
	for (i = 0; i < fru__fieldcount[atype]; i++) {
		if (!add_field_to_area(area_out, &bytes, in_field[atype][i])) {
			fru_errno.src = (fru_error_source_t)atype;
			fru_errno.index = i;
			return false;
		}
	}

	/* Now process cusom fields if any */
	fru__reclist_t * cust = cust_head[info_atype];

	i = 0;
	while (cust) {
		fru_field_t * field = cust->rec;

		if (!add_field_to_area(area_out, &bytes, field)) {
			fru_errno.src = (fru_error_source_t)atype;
			fru_errno.index = fru__fieldcount[atype] + i;
			return false;
		}

		cust = cust->next;
	}

// We don't yet increase bytes to account for the terminator
// to make the below calculations a bit easier
#define TERMINATOR_PTR (area_out + bytes)
#define TERMINATED_SIZE (bytes + 1) // Account for the terminator bytes
#define CKSUMMED_SIZE (TERMINATED_SIZE + 1) // Account for the checksum byte
#define PAD_OUTPTR (area_out + TERMINATED_SIZE) // Padding starts after the terminator
	size_t padding_size;
	padding_size = FRU__BLOCK_ALIGN(CKSUMMED_SIZE) - TERMINATED_SIZE;

	/* Add the custom field list terminator, then add padding and checksum */
	if (area_out) {
		fru__file_field_t * out_field = TERMINATOR_PTR;
		out_field->typelen = FRU__FIELD_TERMINATOR;
		file_area->blocks = FRU__BLOCKS(CKSUMMED_SIZE); // Round up to multiple of 8 bytes
		memset(PAD_OUTPTR, 0, padding_size);
		int cksum = fru__calc_checksum(area_out, TERMINATED_SIZE + padding_size);
		if (cksum < 0)
			return false;

		// Last byte of padding is the checksum
		uint8_t * area_out_cksum = PAD_OUTPTR + padding_size - 1;
		*area_out_cksum = cksum;
	}

	/* Now report the actual size to the caller */
	*size = bytes + padding_size + 1;
#undef CUR_OUTPTR
#undef TERMINATED_SIZE
#undef TERMINATOR_PTR

	return true;
}

static
bool encode_mr_area(void * area_out, size_t * size,
                    fru_area_type_t atype __attribute__((__unused__)),
                    const fru_t * fru)
{
	size_t mr_size = 0;
	void * outptr = NULL;

	if (!fru->mr) {
		/*
		 * There is no correct way to save an empty MR area in a binary file
		 * because the area by definition consists of records and doesn't have
		 * any separate header.  There is even no separate end-of-records
		 * marker, it's just a single bit in the header of the last record. If
		 * there are no records, we have no way to encode that.  Hence, empty
		 * MR area must not be enabled/present, and this is a bug on the
		 * caller's side, so we bail out with an error.
		 */
		*size = 0;
		fru__seterr(FENOREC, FERR_LOC_MR, 0); // No record 0 in MR
		return false;
	}

	size_t index = 0;
	fru__mr_reclist_t * entry = (fru__mr_reclist_t *)fru->mr;
	for (; entry; entry = entry->next) {
		size_t entry_size = 0;
		if (area_out) {
			outptr = area_out + mr_size;
		}
		if (!encode_mr_record(outptr, &entry_size, entry->rec, entry->next == NULL)) {
			fru_errno.src = FERR_LOC_MR;
			fru_errno.index = index;
			return false;
		}
		mr_size += entry_size;
		index++;
	}

	/* The returned size is expected to be block-aligned */
	*size = FRU__BYTES(FRU__BLOCKS(mr_size));
	return true;
}

/*
 * Create a binary fru in the provided file buffer
 * and/or calculate the output size in bytes (block-aligned)
 */
static bool create_frufile(fru__file_t * frufile, size_t * size, const fru_t * fru)
{
	void * area_out = frufile ? (void *)frufile->data : NULL;
	void * outbuf = (void *)frufile;
	size_t totalsize = sizeof(fru__file_t); // There is at least the header

	bool (* encode_area[FRU_TOTAL_AREAS])(void *, size_t *,
	                                      fru_area_type_t,
	                                      const fru_t *) =
	{
		[FRU_INTERNAL_USE] = encode_iu_area,
		[FRU_CHASSIS_INFO] = encode_info_area,
		[FRU_BOARD_INFO]   = encode_info_area,
		[FRU_PRODUCT_INFO] = encode_info_area,
		[FRU_MR]           = encode_mr_area
	};

	bool processed[FRU_TOTAL_AREAS] = { false };

	// Put areas into buffer in the order set by fru->order
	size_t index;
	FRU_FOREACH_AREA(index) {
		fru_area_type_t type = fru->order[index];
		if (processed[type]) {
			// This is a sign that fru wasn't properly initialized
			DEBUG("Area type %d have already been processed\n", type);
			fru__seterr(FEAREADUP, type, -1);
			return false;
		}
		processed[type] = true;
		if (!fru->present[type])
			continue;

		// Encode the area and get back its encoded size in bytes (block-aligned)
		size_t area_size;
		if (!encode_area[type](area_out, &area_size, type, fru))
			return false;

		if (frufile) {
			// Save the current encoded area offset into the fru file header
			uint8_t * frufile_hdr_offset = &frufile->internal + type;
			*frufile_hdr_offset = FRU__BLOCKS(area_out - outbuf);
			// Adjust the output address for the next area (already block-aligned)
			area_out += area_size;
		}

		totalsize += area_size;
	}

	if (frufile) {
		frufile->ver = FRU__VER;
		int cksum = fru__calc_checksum(frufile, sizeof(*frufile));
		if (cksum < 0) {
			return false;
		}
		frufile->hchecksum = (uint8_t)(cksum & 0xFF);
	}

	if (size) {
		// Report the size of the resulting FRU file if the caller wants it
		*size = totalsize;
	}

	return true;
}

/** @endcond */

/*
 * =========================================================================
 * Public API Functions
 * =========================================================================
 */

// See fru.h
bool fru_savebuffer(void ** bufptr, size_t * size, const fru_t * fru)
{
	size_t realsize = 0;
	bool allocated = false;

	if (!fru || !bufptr || !size) {
		fru__seterr(FEGENERIC, FERR_LOC_GENERAL, -1);
		errno = EFAULT;
		goto err;
	}

	/*
	 * First calculate how much space will the encoded fru file take.
	 * Yes, it's a double job, but without it we can't guarantee the
	 * provided data will fit into the provided buffer and we don't
	 * know beforehand how much to allocate for a new buffer.
	 */
	if (!create_frufile(NULL, &realsize, fru)) {
		goto err;
	}

	if (!*bufptr) {
		DEBUG("Allocating %zu bytes for FRU file buffer", realsize);
		*bufptr = calloc(1, realsize);
		if (!*bufptr) {
			fru__seterr(FEGENERIC, FERR_LOC_GENERAL, -1);
			goto err;
		}
		DEBUG("Got a buffer from %p to %p", *bufptr, (void *)*bufptr + realsize - 1);
		allocated = true;
	}
	else {
		if (*size < realsize) {
			fru__seterr(FE2SMALL, FERR_LOC_GENERAL, -1);
			goto err;
		}
		memset(*bufptr, 0, realsize);
	}

	/* Do the encoding again, now with the actual destination buffer */
	if(!create_frufile(*bufptr, size, fru))
		goto err;

	return true;

err:
	if (allocated) {
		zfree(*bufptr);
		*size = 0;
	}
	return false;
}

bool fru_savefile(const char * fname, const fru_t * fru)
{
	fru__file_t * frufile = NULL;
	size_t frufile_size = 0;

	if (!fname || !fru) {
		fru__seterr(FEGENERIC, FERR_LOC_GENERAL, -1);
		errno = EFAULT;
	}

	if (!fru_savebuffer((void **)&frufile, &frufile_size, fru)) {
		return false;
	}

	int fd = open(fname,
#if __WIN32__ || __WIN64__
			  O_CREAT | O_TRUNC | O_WRONLY | O_BINARY,
#else
			  O_CREAT | O_TRUNC | O_WRONLY,
#endif
			  0644);

	if (fd < 0) {
		DEBUG("Couldn't create file %s: %m", fname);
		fru__seterr(FEGENERIC, FERR_LOC_GENERAL, -1);
		return false;
	}

	size_t written = 0;
	while (written < frufile_size) {
		int rc = write(fd, frufile, frufile_size);
		if (0 > rc) {
			if(EINTR == errno)
				continue;
			fru__seterr(FEGENERIC, FERR_LOC_GENERAL, -1);
			DEBUG("Couldn't write to %s: %m", fname);
			return false;
		}
		written += rc;
	}

	free(frufile);
	close(fd);
	return true;
}
