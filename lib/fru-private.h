/** @file
 *  @brief Header for private libfru functions and definitions
 *
 *  Some definitions are not needed for the library users, so they
 *  are put here, away from the main \p fru.h header
 *
 *  @copyright
 *  Copyright (C) 2016-2024 Alexander Amelkin <alexander@amelkin.msk.ru>
 *
 *_  SPDX-License-Identifier: GPL-2.0-or-later OR Apache-2.0
 */

/** @cond PRIVATE */
#pragma once

#include "fru.h"

#if defined(__APPLE__)

#include <libkern/OSByteOrder.h>

#define htobe16(x) OSSwapHostToBigInt16(x)
#define htole16(x) OSSwapHostToLittleInt16(x)
#define be16toh(x) OSSwapBigToHostInt16(x)
#define le16toh(x) OSSwapLittleToHostInt16(x)

#define htobe32(x) OSSwapHostToBigInt32(x)
#define htole32(x) OSSwapHostToLittleInt32(x)
#define be32toh(x) OSSwapBigToHostInt32(x)
#define le32toh(x) OSSwapLittleToHostInt32(x)

#else

#define _BSD_SOURCE
#include <endian.h>

#endif

#ifdef DEBUG
#undef DEBUG
#include <stdio.h>
#include <errno.h>
#define DEBUG(f, args...) do { \
	typeof(errno) err = errno; \
	printf("%s:%d: ", __func__, __LINE__); \
	errno = err; /* Allow 'f' to use "%m" */ \
	printf(f,##args); \
	printf("\n"); \
	errno = err; \
} while(0)
#else
#define DEBUG(f, args...)
#endif

#define fru__seterr(err, where, idx) do { \
	fru_errno.code = err; \
	fru_errno.src = (fru_error_source_t)where; \
	fru_errno.index = idx; \
} while(0)

/*
 * Binary FRU file related definitions
 */

/**
 * Maximum supported binary FRU file size
 * 
 * Theoretically, the last area in FRU file can not start beyond
 * byte 2040 (8 * 255), and generally no info area can be longer
 * than the same 2040 bytes due to a single-byte size designator
 * in the area header. However, if the last area is 'internal use',
 * which doesn't have a header containing its own size, or a
 * 'multirecord' that comprises an unlimited number of records,
 * then such last area can span beyond the 4K limit.
 * 
 * Nonetheless, we don't want to allow for arbitrary huge internal
 * use or multirecord areas, so we impose some reasonable limit
 * on the size of the whole FRU file.
 * 
 * If you want to read a huge file where FRU is just a part of the
 * file content, you may manually load it into memory and then use
 * fru_loadbuffer() on it.
 */
#define FRU__MAX_FILE_SIZE (64L * 1024L)

/**
 * FRU file layout as per IPMI FRU Specification
 *
 * Can be used for type-punning byte arrays
 */
#pragma pack(push, 1)
typedef struct {
	uint8_t ver:4, rsvd:4;
	uint8_t internal;
	uint8_t chassis;
	uint8_t board;
	uint8_t product;
	uint8_t multirec;
	uint8_t pad;
	uint8_t hchecksum; ///< Header checksum
	uint8_t data[];
} __attribute__((packed)) fru__file_t;

/**
 * A generic FRU area encoded field header structure.
 *
 * Every field in chassis, board and product information areas has such a header.
 */
typedef struct {
	uint8_t typelen;   /**< Type/length of the field */
	uint8_t data[];    /**< The field data */
} fru__file_field_t;

#define FRU__FILE_FIELD_MAXDATA ((1 << 8) - 1)
#define FRU__FILE_FIELD_MAXSIZE (sizeof(fru__file_field_t) + FRU__FILE_FIELD_MAXDATA)

#define FRU__TYPE_BITS_SHIFT     6

/** Make FRU field type byte. Any of BINARY, BCDPLUS, ASCII_6BIT or TEXT. */
#define FRU__MAKETYPE(x)         (FRU__TYPE_##x << FRU__TYPE_BITS_SHIFT)
#define FRU__TYPELEN(t, l)      (FRU__MAKETYPE(t) | FRU__FIELDLEN(l))
#define FRU__TYPE(t)            (((t) & FRU__TYPE_BITS_MASK) >> FRU__TYPE_BITS_SHIFT)
#define FRU__FIELDSIZE(typelen) (FRU__FIELDLEN(typelen) + sizeof(fru__file_field_t))
#define FRU__FIELD_ENC_T(typelen) (FRU_FE_FROMREAL(FRU__TYPE(typelen)))

#define FRU__VER 1 /* Always 1 for the only existing specification */
#define FRU__MINIMUM_AREA_HEADER \
	uint8_t ver;  /**< Area format version, only lower 4 bits */

#define FRU__INFO_AREA_HEADER \
	FRU__MINIMUM_AREA_HEADER; \
	uint8_t blocks;         /**< Size in 8-byte blocks */ \
	uint8_t langtype        /**< Area language code or chassis type (from smbios.h) */

typedef struct { // The generic info area structure
	FRU__INFO_AREA_HEADER;
	char data[];
} __attribute__((packed)) fru__file_area_t;

#define FRU__INFO_AREA_HEADER_SZ sizeof(fru__file_area_t)

typedef struct {
	FRU__MINIMUM_AREA_HEADER;
	char data[];
} __attribute__((packed)) fru__file_internal_t;
#define FRU__INTERNAL_HDR_LEN sizeof(fru__file_internal_t);

typedef fru__file_area_t fru__file_chassis_t;

typedef struct {
	FRU__INFO_AREA_HEADER;
	uint8_t mfgdate[3]; ///< Manufacturing date/time in minutes since 1996/1/1 0:00
	char data[];     ///< Variable size (multiple of 8 bytes) data with tail padding and checksum
} __attribute__((packed)) fru__file_board_t;

typedef fru__file_area_t fru__file_product_t;

typedef struct {
	uint8_t type_id; ///< Record Type ID
	uint8_t eol_ver;
#define FRU__MR_EOL 0x80 // End-of-list flag
#define FRU__MR_VER_MASK 0x07
#define FRU__MR_VER 0x02 // Version is always 2
	uint8_t len;           ///< Length of the raw `data` array
	uint8_t rec_checksum;
	uint8_t hdr_checksum;
} __attribute__((packed)) fru__file_mr_header_t;
// The checksum byte itself is not checksummed
#define FRU__FILE_MR_HDR_CHECKED_SIZE (sizeof(fru__file_mr_header_t) - sizeof(uint8_t))

typedef struct {
	fru__file_mr_header_t hdr;
#define FRU__IS_MR_END(rec) ((rec)->hdr.eol_ver & FRU__MR_EOL)
#define FRU__IS_MR_VALID_VER(rec) \
            (((rec)->hdr.eol_ver & FRU__MR_VER_MASK) == FRU__MR_VER)
	uint8_t data[];        ///< Raw data of size `len`
} __attribute__((packed)) fru__file_mr_rec_t;
#define FRU__MR_REC_SZ(rec) (sizeof(fru__file_mr_rec_t) + (rec)->hdr.len)

typedef struct {
	fru__file_mr_header_t hdr;
	uint8_t subtype;
	uint8_t data[];
} __attribute__((packed)) fru__file_mr_mgmt_rec_t;

/*
 * Minimum and maximum lengths of values as per
 * Table 18-6, Management Access Record
 */
extern const size_t fru__mr_mgmt_minlen[FRU_MR_MGMT_MAX];
extern const size_t fru__mr_mgmt_maxlen[FRU_MR_MGMT_MAX];
/* A convenience macro to make indices into the above arrays from subtype NAMES */
#define FRU__MGMT_TYPENAME_ID(name) FRU_MR_MGMT_SUBTYPE_TO_IDX(FRU_MR_MGMT_##name)

/*
 * Convert management access record data size into a pure MR record data size.
 * We convert this way and not the other way round to avoid getting (size_t)(-1)
 * when mr_size is 0
 */
#define FRU__MGMT_MR_DATASIZE(mgmt_size) \
	(sizeof(((fru__file_mr_mgmt_rec_t *)NULL)->subtype) + (mgmt_size))

/// Table 18-1, Power Supply Information
typedef struct {
	uint16_t overall_cap;
#define FRU_MR_PSU_OCAP_MAX ((1 << 12) - 1)
#define FRU_MR_PSU_OCAP_MASK FRU_MR_PSU_OCAP_MAX
	uint16_t peak_va;
#define FRU_MR_PSU_PEAK_VA_UNSPEC 0xFFFF
	uint8_t inrush_amp;
#define FRU_MR_PSU_INRUSH_AMP_UNSPEC 0xFF
	uint8_t inrush_ms;
#define FRU_MR_PSU_INRUSH_MS_UNSPEC 0
	int16_t lo_vin1;
	int16_t hi_vin1;
	int16_t lo_vin2;
	int16_t hi_vin2;
#define FRU_MR_PSU_VIN_SINGLE_RANGE 0
#define FRU_MR_PSU_VIN_RANGE_STEP_mV 10
	uint8_t lo_freq;
#define FRU_MR_PSU_LFREQ_ACCEPTS_DC 0
	uint8_t hi_freq;
#define FRU_MR_PSU_HFREQ_DC_ONLY 0
	uint8_t dropout_tolerance_ms;
	uint8_t flags;
#define FRU_MR_PSU_FLAGS_PREFAIL_SUPPORT (1 << 0)
#define FRU_MR_PSU_FLAGS_PF_CORRECTION (1 << 1)
#define FRU_MR_PSU_FLAGS_AUTOSWITCH (1 << 2)
#define FRU_MR_PSU_FLAGS_HOTSWAP (1 << 3)
#define FRU_MR_PSU_FLAGS_TACH_PPR (1 << 4)
#define   FRU_MR_PSU_TACH_PPR(x) (((x)->flags & FRU_MR_PSU_FLAGS_TACH_PPR) ? 2 : 1)
#define FRU_MR_PSU_FLAGS_PREFAIL_POL FRU_MR_PSU_FLAGS_TACH_PPR
	uint16_t peak_watts_holdup;
#define FRU_MR_PSU_HOLDUP_SHIFT 12
#define FRU_MR_PSU_HOLDUP(x) ((x)->peak_watts_holdup >> FRU_MR_PSU_HOLDUP_SHIFT)
#define FRU_MR_PSU_PEAK_WATTS_MASK ((1 << FRU_MR_PSU_HOLDUP_SHIFT) - 1)
#define FRU_MR_PSU_PEAK_WATTS(x) ((x)->peak_watts_holdup & FRU_MR_PSU_PEAK_WATTS_MASK)
#define FRU_MR_PSU_PEAK_WATTS_UNSPEC FRU_MR_PSU_PEAK_WATTS_MASK
	struct {
		uint8_t per_range;
#define FRU_MR_PSU_WATTS_RANGE1_SHIFT 4
#define FRU_MR_PSU_WATTS_RANGE2_SHIFT 0
#define FRU_MR_PSU_WATTS_MASK ((1 << FRU_MR_PSU_WATTS_RANGE1_SHIFT) - 1)
#define FRU_MR_PSU_WATTS_RANGE(x, r) ( \
        	( \
        		((x)->combined_watts.per_range) >> FRU_MR_PSU_WATTS_RANGE##r##_SHIFT \
        	) & FRU_MR_PSU_WATTS_MASK \
        )
		uint16_t total;
	} __attribute__((packed)) combined_watts;
	uint8_t prefail_tach_rps;
#define FRU_MR_PSU_PREFAIL_BINARY 0x00
#define FRU_MR_PSU_PREFAIL_RPM(x) (((x)->prefail_tach_rps) * 60) // RPS to RPM conversion
} __attribute__((packed)) fru__file_mr_psu_t;

/**
 * Generic FRU info area description structure.
 *
 * All of the above area structures share the same first byte.
 */
typedef struct {
	uint8_t langtype; /* All fru__*_t area types have this */
	uint8_t data[];
} fru__info_area_t;

/* These are used for uuid2rec and rec2uuid */
#define FRU__UUID_SIZE 16
#define FRU__UUID_STRLEN_NONDASHED (FRU__UUID_SIZE * 2) // 2 hex digits for byte
#define FRU__UUID_STRLEN_DASHED (FRU__UUID_STRLEN_NONDASHED + 4)

/**
 * Binary UUID representation structure
 */
typedef union __attribute__((packed)) {
	uint8_t raw[FRU__UUID_SIZE];
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
} fru__uuid_t;

#pragma pack(pop)

/*
 * End of binary FRU file related definitions
 */

#define FRU__AREA_HAS_DATE(t) (FRU_BOARD_INFO == (t))
#define FRU__DATE_AREA_HEADER_SZ sizeof(fru__file_board_t)
#define FRU__DATE_UNSPECIFIED 0

// 6-bit encoding allows for 4 characters in 3 bytes
#define FRU__6BIT_LENGTH(len)    (((size_t)(len) * 3 + 3) / 4)
#define FRU__6BIT_FULLLENGTH(l6) (((size_t)(l6) * 4) / 3)
#define FRU__6BIT_CHARS          (1 << 6)
#define FRU__6BIT_MAXVALUE       (FRU__6BIT_CHARS - 1)
#define FRU__6BIT_BASE           0x20 // Space is encoded as 0
#define FRU__FIELD_EMPTY         FRU__TYPELEN(TEXT, 0)
#define FRU__FIELD_TERMINATOR    FRU__TYPELEN(TEXT, 1)

#define FRU__BLOCK_SZ 8
#define FRU__BYTES(blocks) ((size_t)((blocks) * FRU__BLOCK_SZ))
#define FRU__BLOCKS(bytes)  (((bytes) + FRU__BLOCK_SZ - 1) / FRU__BLOCK_SZ)
#define FRU__BLOCK_ALIGN(size) FRU__BYTES(FRU__BLOCKS(size))

/** Numbers of standard string fields per info area */
extern const size_t fru__fieldcount[FRU_TOTAL_AREAS];

/**
 * Calculate checksum for an arbitrary block of bytes
 */
int fru__calc_checksum(const void * blk, size_t blk_bytes);

/** Encode an info area field.
 *
 * @param[in,out] field  A pointer to a decoded field structure to fill with data
 * @param[in]  encoding  The desired encoding, use FILED_TYPE_PRESERVE to
 *                       preserve whatever value is already in \p field.
 * @param[in]  s         The source data string
 */
bool fru__encode_field(fru__file_field_t * out_field,
                       fru_field_enc_t encoding,
                       const char * s);

/**
 * @brief A single-linked list of decoded FRU area fields.
 *
 * This is used to describe any length chains of fields.
 * Mandatory fields are linked first in the order they appear
 * in the information area (as per Specification), then custom
 * fields are linked.
 */
typedef struct fru__reclist_s {
	fru_field_t * rec; ///< A pointer to a field or NULL if not initialized
	struct fru__reclist_s * next; ///< The next record in the list or NULL if last
} fru__reclist_t;

/**
 * @brief A single-linked list of decoded FRU MR area records.
 *
 * This is used to describe any length chains of MR records.
 */
typedef struct fru__mr_reclist_s {
	fru_mr_rec_t * rec; ///< A pointer to a record or NULL if not initialized
	struct fru__mr_reclist_s * next; ///< The next record in the list or NULL if last
} fru__mr_reclist_t;

/**
 * A generic single-linked list abstraction.
 * This is used as a substitute for all other list types in the library.
 */
typedef struct {
	void * data; /* A pointer to the actual data or NULL if not initialized */
	void * next; /* The next record in the list or NULL if last */
} fru__genlist_t;

/**
 * Get a pointer to the custom records list in \a fru structure
 * according to the provided \a atype.
 */
fru__reclist_t ** fru__get_customlist(const fru_t * fru, fru_area_type_t atype);

/**
 * Allocate a new reclist entry and add it to reclist, pointed
 * to by \a head_ptr. Set \a head_ptr to point to the newly
 * allocated entry if \a head_ptr was NULL.
 *
 * @note Doesn't allocate/add the actual data to the entry
 *
 * @returns Pointer to the added entry
 */
void * fru__add_reclist_entry(void * head_ptr, size_t index);

/**
 * Find an \a n'th record in a list.
 *
 * Works both with fru__reclist_t and fru__mr_reclist_t.
 *
 * @param[in] reclist A pointer to any record list
 * @param[in] prev    A pointer to a record preceding the matching one,
 *                    can be NULL
 * @param[in] n       The index of the record to find, 1-based
 * @returns A pointer to the found record or NULL
 */
void * fru__find_reclist_entry(void * head_ptr, void * prev, size_t index);

/*
 * Free all the record list entries starting with the
 * one pointed to by listptr and up to the end of the list.
 *
 * Takes a pointer to any fru__genlist_t compatible list.
 * That is either fru__reclist_t ** or fru__mr_reclist_t **.
 */
bool fru__free_reclist(void * listptr);

typedef enum {
	FRU__HEX_RELAXED, // Allow delimiters in the input hex string
	FRU__HEX_STRICT   // Only allow hex digits in the input hex string
} fru__hex_mode_t;

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
bool fru__hexstr2bin(void * out, size_t * outsize, fru__hex_mode_t enc_mode, const char * s);

/**
 * Get the FRU date/time base in seconds since UNIX Epoch
 *
 * According to IPMI FRU Information Storage Definition v1.0, rev 1.3,
 * the date/time encoded as zero designates "0:00 hrs 1/1/96",
 * see Table 11-1 "BOARD INFO AREA"
 *
 * @returns The number of seconds from UNIX Epoch to the FRU date/time base
 */
time_t fru__datetime_base(void);

/* FRU has 3 bytes for time in minutes since the base */
#define FRU_DATETIME_MAX ((time_t)(0xFFFFFFLL * 60 + fru__datetime_base()))

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
 */
void fru__decode_raw_binary(const void * in, size_t in_len, char * out, size_t out_len);

/**
 * Decode data from an encoded buffer into a decoded buffer.
 *
 * For binary data use FRU__FIELDLEN(field->typelen) to find
 * out the size of valid bytes in the returned buffer.
 *
 * @param[out] out Decoded field.
 * @param[in] field Encoded data field.
 * @retval true Success.
 * @retval false Failure.
 */
bool fru__decode_field(fru_field_t *out, const fru__file_field_t *field);

/**
 * Convert a binary byte into 2 bytes of hex string
 */
void fru__byte2hex(void * buf, char byte);

/** @endcond */
