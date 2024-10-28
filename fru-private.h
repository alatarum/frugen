/** @file
 *  @brief Header for private libfru functions and definitions
 *
 *  Some definitions are not needed for the library users, so they
 *  are put here, away from the main \p fru.h header
 *
 *  @copyright
 *  Copyright (C) 2016-2024 Alexander Amelkin <alexander@amelkin.msk.ru>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later OR Apache-2.0
 */

/** @cond PRIVATE */
#pragma once

#include "fru.h"

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

#define FRU_VER_1    1
#define FRU_MINIMUM_AREA_HEADER \
	uint8_t ver;  /**< Area format version, only lower 4 bits */

#define FRU_INFO_AREA_HEADER \
	FRU_MINIMUM_AREA_HEADER; \
	uint8_t blocks;         /**< Size in 8-byte blocks */ \
	uint8_t langtype        /**< Area language code or chassis type (from smbios.h) */

typedef struct { // The generic info area structure
	FRU_INFO_AREA_HEADER;
	char data[];
} __attribute__((packed)) fru__file_area_t;

#define FRU_INFO_AREA_HEADER_SZ sizeof(fru__file_area_t)

typedef struct {
	FRU_MINIMUM_AREA_HEADER;
	char data[];
} __attribute__((packed)) fru__file_internal_t;
#define FRU__INTERNAL_HDR_LEN sizeof(file_internal_t);

typedef fru__file_area_t fru__file_chassis_t;

typedef struct {
	FRU_INFO_AREA_HEADER;
	uint8_t mfgdate[3]; ///< Manufacturing date/time in minutes since 1996/1/1 0:00
	char data[];     ///< Variable size (multiple of 8 bytes) data with tail padding and checksum
} __attribute__((packed)) fru__file_board_t;

typedef fru__file_area_t fru__file_product_t;

typedef struct {
	uint8_t type_id; ///< Record Type ID
	uint8_t eol_ver;
#define FRU_MR_EOL 0x80 // End-of-list flag
#define FRU_MR_VER_MASK 0x07
#define FRU_MR_VER 0x02 // Version is always 2
	uint8_t len;           ///< Length of the raw `data` array
	uint8_t rec_checksum;
	uint8_t hdr_checksum;
} __attribute__((packed)) fru__file_mr_header_t;

typedef struct {
	fru__file_mr_header_t hdr;
#define IS_FRU_MR_END(rec) ((rec)->hdr.eol_ver & FRU_MR_EOL)
#define IS_FRU_MR_VALID_VER(rec) \
            (((rec)->hdr.eol_ver & FRU_MR_VER_MASK) == FRU_MR_VER)
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
static const size_t fru__mr_mgmt_minlen[FRU_MR_MGMT_MAX] = {
	[MGMT_TYPENAME_ID(SYS_URL)] = 16,
	[MGMT_TYPENAME_ID(SYS_NAME)] = 8,
	[MGMT_TYPENAME_ID(SYS_PING)] = 8,
	[MGMT_TYPENAME_ID(COMPONENT_URL)] = 16,
	[MGMT_TYPENAME_ID(COMPONENT_NAME)] = 8,
	[MGMT_TYPENAME_ID(COMPONENT_PING)] = 8,
	[MGMT_TYPENAME_ID(SYS_UUID)] = 16
};

static const size_t fru__mr_mgmt_maxlen[FRU_MR_MGMT_MAX] = {
	[MGMT_TYPENAME_ID(SYS_URL)] = 256,
	[MGMT_TYPENAME_ID(SYS_NAME)] = 64,
	[MGMT_TYPENAME_ID(SYS_PING)] = 64,
	[MGMT_TYPENAME_ID(COMPONENT_URL)] = 256,
	[MGMT_TYPENAME_ID(COMPONENT_NAME)] = 256,
	[MGMT_TYPENAME_ID(COMPONENT_PING)] = 64,
	[MGMT_TYPENAME_ID(SYS_UUID)] = 16
};



/**
 * Indices of mandatory fields in Chassis Info Area
 */
typedef enum {
	FRU_CHASSIS_PARTNO,
	FRU_CHASSIS_SERIAL,
	FRU_CHASSIS_FIELD_COUNT
} fru_chassis_field_t;

/**
 * Indices of mandatory fields in Board Info Area
 */
typedef enum {
	FRU_BOARD_MFG,
	FRU_BOARD_PRODNAME,
	FRU_BOARD_SERIAL,
	FRU_BOARD_PARTNO,
	FRU_BOARD_FILE,
	FRU_BOARD_FIELD_COUNT
} fru_board_field_t;

/**
 * Indices of mandatory fields in Product Info Area
 */
typedef enum {
	FRU_PROD_MFG,
	FRU_PROD_NAME,
	FRU_PROD_MODELPN,
	FRU_PROD_VERSION,
	FRU_PROD_SERIAL,
	FRU_PROD_ASSET,
	FRU_PROD_FILE,
	FRU_PROD_FIELD_COUNT
} fru_prod_field_t;

/** Maximum field cound among all info areas */
#define FRU_MAX_FIELD_COUNT FRU_PROD_FIELD_COUNT

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

#pragma pack(pop)
/*
 * End of binary FRU file related definitions
 */

#define zfree(buf) do { \
	int err = errno; /* Prevent errno corruption */ \
	free(buf); \
	(buf) = NULL; \
	errno = err; \
} while(0)

/*
 * Exploded/decoded FRU related definitions
 */

/**
 * FRU area description structure.
 *
 * Contains information about a single arbitrary area and points
 * to the encoded area data.
 */
typedef struct {
	fru_area_type_t atype; /**< FRU area type */
	uint8_t blocks; /**< Size of the data field in 8-byte blocks */
	void * data; /**< Pointer to the actual FRU area data */
} fru_area_t;

#define FRU_AREA_NOT_PRESENT (-1)
#define FRU_IS_ATYPE_VALID(t) ((t) >= FRU_AREA_NOT_PRESENT && (t) < FRU_MAX_AREAS)

/**
 * A linked list of multirecord area records
 */
typedef struct fru_mr_reclist_s {
	fru__file_mr_rec_t *rec;
	struct fru_mr_reclist_s *next;
} fru_mr_reclist_t;

typedef fru__file_mr_rec_t fru__file_mr_t; /// Intended for use as a pointer only

#define FRU_AREA_HAS_DATE(t) (FRU_BOARD_INFO == (t))
#define FRU_DATE_AREA_HEADER_SZ sizeof(fru__file_board_t)
#define FRU_DATE_UNSPECIFIED 0

#define FRU_AREA_HAS_SIZE(t) (FRU_INTERNAL_USE < (t) && (t) < FRU_MULTIRECORD)
#define FRU_AREA_HAS_HEADER(t) (FRU_MULTIRECORD != (t))
#define FRU_AREA_IS_GENERIC(t) FRU_AREA_HAS_SIZE(t)


#define LEN_AUTO              0
#define LEN_BCDPLUS           -1
#define LEN_6BITASCII         -2
#define LEN_TEXT              -3

#define FRU__6BIT_LENGTH(len)    (((size_t)(len) * 3 + 3) / 4)
#define FRU__6BIT_FULLLENGTH(l6) (((size_t)(l6) * 4) / 3)
#define FRU__6BIT_MAXFULLLENGTH ((1 << 6) - 1)
#define FRU__FIELD_EMPTY       FRU__TYPELEN(TEXT, 0)
#define FRU__FIELD_TERMINATOR  FRU__TYPELEN(TEXT, 1)

#define FRU_BLOCK_SZ 8
#define FRU_BYTES(blocks) ((blocks) * FRU_BLOCK_SZ)
#define FRU_BLOCKS(bytes)  (((bytes) + FRU_BLOCK_SZ - 1) / FRU_BLOCK_SZ)

#ifdef DEBUG
const char * fru_enc_name_by_type(field_type_t type);
field_type_t fru_enc_type_by_name(const char *name);
#endif

/**
 * Calculate zero checksum for command header and FRU areas
 */
int fru__calc_checksum(void *blk, size_t blk_bytes);

/**
 * Calculate an area checksum
 *
 * Calculation includes the checksum byte itself.
 * For freshly prepared area this method returns a checksum to be stored in
 * the last byte. For a pre-existing area this method returns zero if checksum
 * is ok or non-zero otherwise.
 *
 */
int fru__area_checksum(fru__file_area_t *area);

/// Extract FRU field type as fru_field_type_t
#define FRU__FIELD_TYPE_T(t) (FRU_TYPE(t) + BASE_FIELD_TYPE)

/**
 * Store a string and encoding type in a fru_field_t structure.
 * No checks are performed except for the string length.
 *
 * @param[in,out] field  A pointer to a decoded field structure to fill with data
 * @param[in]     s      The source data string
 * @param[in]     enc    The desired encoding, use FILED_TYPE_PRESERVE to
 *                       preserve whatever value is already in \a field.
 *
 */
void fru_loadfield(fru_field_t *field, const char *s, field_type_t enc);

/*
 * Field types per Section 13 of IPMI FRU Specification,
 * in their binary/encoded representation
 */
#define FRU__TYPE_BINARY         0x00
#define FRU__TYPE_BCDPLUS        0x01
#define FRU__TYPE_ASCII_6BIT     0x02
#define FRU__TYPE_TEXT           0x03

#define FRU__TYPE_BITS_SHIFT     6

/** Make FRU field type byte. Any of BINARY, BCDPLUS, ASCII_6BIT or TEXT. */
#define FRU_MAKETYPE(x)       (FRU__TYPE_##x << FRU__TYPE_BITS_SHIFT)

#define FRU__TYPELEN(t, l)     (FRU_MAKETYPE(t) | FRU__FIELDLEN(l))
#define FRU__TYPE(t)           (((t) & FRU__TYPE_BITS_MASK) >> FRU__TYPE_BITS_SHIFT)
#define FRU__ISTYPE(t, type)   (FRU_TYPE(t) == FRU__TYPE_##type)
#define FRU__FIELDSIZE(typelen) (FRU__FIELDLEN(typelen) + sizeof(fru__file_field_t))

#define FRU__MR_HDR_LEN          5

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


/**
 * A generic FRU area encoded field header structure.
 *
 * Every field in chassis, board and product information areas has such a header.
 */
typedef struct {
	uint8_t typelen;   /**< Type/length of the field */
	uint8_t data[];    /**< The field data */
} fru__file_field_t;


/**
 * Allocate a new reclist entry and add it to reclist, pointed
 * to by \a head_ptr. Set \a head_ptr to point to the newly
 * allocated entry if \a head_ptr was NULL.
 *
 * @note Doesn't allocate/add the actual data to the entry
 *
 * @returns Pointer to the added entry
 */
void *fru__add_reclist_entry(void *head_ptr, int index);

/**
 * Find an \a n'th record in a list.
 *
 * Works both with fru_reclist_t and fru_mr_reclist_t.
 *
 * @param[in] reclist A pointer to any record list
 * @param[in] n       The index of the record to find, 1-based
 * @returns A pointer to the found record or NULL
 */
static inline void *find_rec(void *reclist, size_t n)
{
	fru_reclist_t *rl = reclist;

	for (size_t i = 1; i != n && rl; rl = rl->next, i++);

	return rl;
}

/// Works for any list (fru_generic_reclist_t*, fru_reclist_t*, fru_mr_reclist_t*)
#define free_reclist(recp) while(recp) { \
	typeof((recp)->next) next = (recp)->next; \
	free((recp)->rec); \
	free(recp); \
	recp = next; \
}

fru__file_chassis_t * fru_chassis_info(const fru_chassis_t *chassis);
fru__file_board_t * fru_board_info(const fru_board_t *board);
fru__file_product_t * fru_product_info(const fru_product_t *product);

/**
 * Take an input string and pack it into an "exploded" multirecord
 * area "management access" record in binary form, using the
 * provided subtype.
 *
 * As per IPMI FRU specifiation section 18.4, no validity checks
 * are performed to restrict the values. Only the length is checked
 * according to the subtype restrictions.
 *
 * @returns An errno-like negative error code
 * @retval 0        Success
 * @retval -EINVAL  Invalid string (wrong length, wrong symbols)
 * @retval -EFAULT  Invalid pointer
 * @retval >0       any errno that calloc() is allowed to set
 */
int fru_mr_mgmt_str2rec(fru__file_mr_rec_t **rec,
                        const char *str,
                        fru_mr_mgmt_type_t type);

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
int fru_mr_uuid2rec(fru__file_mr_rec_t **rec, const char *str);

/**
 * Take an "exploded" multirecord area record in binary form and convert
 * it into a canonical UUID string if it's a Management UUID record
 * or return an error otherwise.
 *
 * @param[in,out] str  Pointer to a string to be allocated and filled in with
 *                     the decoded UUID
 * @param[in]     rec  Pointer to the input Management UUID record to decode
 * @returns An errno-like negative error code
 * @retval 0        Success
 * @retval -EINVAL  Not a UUID record
 * @retval -EFAULT  Invalid pointer
 * @rerval -ERANGE  Checksum error on record header or data
 * @retval >0       any errno that calloc() is allowed to set
 */
int fru_mr_rec2uuid(char **str, fru__file_mr_mgmt_rec_t *rec, fru_flags_t flags);

/**
 * Take an "exploded" multirecord area record in binary form and convert
 * it into a canonical C string if it's a Management Access record
 * (except for Management Access System UUID record), or return an error
 * otherwise.
 *
 * @param[in,out] str  Pointer to a string to be allocated and filled in with
 *                     the string value of the management record
 * @param[in]     rec  Pointer to the input Management Access record
 * @returns An errno-like negative error code
 * @retval 0        Success
 * @retval -EINVAL  Not a valid Management Access record or a UUID record
 * @retval -EFAULT  Invalid pointer
 * @rerval -ERANGE  Checksum error on record header or data
 * @retval >0       any errno that calloc() is allowed to set
 */
int fru_mr_mgmt_rec2str(char **str, fru__file_mr_mgmt_rec_t *mgmt,
                        fru_flags_t flags);

/**
 * Allocate and build an Internal Use Area block.
 *
 * The function will allocate a buffer of size that is a muliple of 8 bytes
 * and is big enough to accomodate the standard area header and the area data.
 *
 * It is safe to free (deallocate) any arguments supplied to this function
 * immediately after the call as all the data is copied to the new buffer.
 *
 * Don't forget to free() the returned buffer when you don't need it anymore.
 *
 * @param[in] data The internal use area data as a hex string (e.g. "DEADBEEF")
 * @param[out] blocks The size of the resulting area in FRU blocks
 * @returns fru__file_internal_t *area A newly allocated buffer containing
 *                                        the created area
 */
fru__file_internal_t *fru_encode_internal_use_area(const void *data, uint8_t *blocks);

/**
 * @brief Encode chassis info into binary buffer.
 *
 * Binary buffer needs to be freed after use.
 *
 * @param[in] chassis Area info.
 * @return Encoded area buffer.
 * @retval NULL Encoding failed. \p errno is set accordingly.
 */
fru__file_chassis_t * fru_encode_chassis_info(fru_chassis_t *chassis);

/**
 * @brief Encode board info into binary buffer.
 *
 * Binary buffer needs to be freed after use.
 *
 * @param[in] board Area info.
 * @return Encoded area buffer.
 * @retval NULL Encoding failed. \p errno is set accordingly.
 */
fru__file_board_t * fru_encode_board_info(fru_board_t *board);

/**
 * @brief Encode product info into binary buffer.
 *
 * Binary buffer needs to be freed after use.
 *
 * @param[in] product Area info.
 * @return Encoded area buffer.
 * @retval NULL Encoding failed. \p errno is set accordingly.
 */
fru__file_product_t * fru_encode_product_info(fru_product_t *product);

/**
 * Allocate and build a MultiRecord area block.
 *
 * The function will allocate a buffer of size that is required to store all
 * the provided data and accompanying record headers. It will calculate data
 * and header checksums automatically.
 *
 * All data will be copied as-is, without any additional encoding, the name
 * `encode` is given for consistency with the functions that build
 * other areas.
 *
 * It is safe to free (deallocate) any arguments supplied to this function
 * immediately after the call as all the data is copied to the new buffer.
 *
 * Don't forget to free() the returned buffer when you don't need it anymore.
 *
 * @returns fru__file_mr_t *area  A newly allocated buffer containing the
 *                               created area
 *
 */
fru__file_mr_t * fru_encode_mr_area(fru_mr_reclist_t *reclist, size_t *total);

fru_t * fru__file_create(fru_t area[FRU_MAX_AREAS], size_t *size);

fru_t *find_fru_header(uint8_t *buffer, size_t size, fru_flags_t flags);

/**
 * @brief Find and validate FRU chassis area in the byte buffer.
 *
 * @param[in] buffer Byte buffer.
 * @param[in] size Byte buffer size.
 * @return Pointer to the FRU chassis area in the buffer.
 * @retval NULL FRU chassis area not found. \p errno is set accordingly.
 */
fru__file_chassis_t *find_fru_chassis_area(uint8_t *buffer, size_t size, fru_flags_t flags);

/**
 * @brief Find and validate FRU board area in the byte buffer.
 *
 * @param[in] buffer Byte buffer.
 * @param[in] size Byte buffer size.
 * @return Pointer to the FRU board area in the buffer.
 * @retval NULL FRU board area not found. \p errno is set accordingly.
 */
fru__file_board_t *find_fru_board_area(uint8_t *buffer, size_t size, fru_flags_t flags);

/**
 * @brief Find and validate FRU product area in the byte buffer.
 *
 * @param[in] buffer Byte buffer.
 * @param[in] size Byte buffer size.
 * @return Pointer to the FRU product area in the buffer.
 * @retval NULL FRU product area not found. \p errno is set accordingly.
 */
fru__file_product_t *find_fru_product_area(uint8_t *buffer, size_t size, fru_flags_t flags);

/**
 * Find an Internal Use area in the supplied FRU \a buffer of the
 * given \a size.
 *
 * @param[in]  buffer   Pointer to the FRU data
 * @param[in]  size     Size of the FRU \a buffer
 * @param[out] iu_size  Detected size of the found Internal Use area
 * @param[in]  flags    Debug flags to skip certain checks
 *
 * @returns A pointer to the internal use area within the \a buffer, detected
 *          size of the area is stored in \a iu_size.
 * @retval NULL An error has occured, errno indicates the problem, \a iu_size equals 0
 * @retval non-NULL A pointer to the found multi-record area start
 */
fru__file_internal_t *find_fru_internal_use_area(
	uint8_t *buffer, size_t *ia_size, size_t size, fru_flags_t flags);

/**
 * Find a multirecord area in the supplied FRU \a buffer of the
 * given \a size.
 *
 * @param[in]  buffer   Pointer to the FRU data
 * @param[in]  size     Size of the FRU \a buffer
 * @param[out] mr_size  Detected size of the found Multirecord area
 * @param[in]  flags    Debug flags to skip certain checks
 *
 * @returns A pointer to the multi-record area within the \a buffer
 * @retval NULL An error has occured, errno indicates the problem
 * @retval non-NULL A pointer to the found multi-record area start
 */
fru__file_mr_t *find_fru_mr_area(uint8_t *buffer, size_t *mr_size, size_t size, fru_flags_t flags);

/**
 * @brief Decode chassis area into \p fru_chassis_t.
 *
 * @param[in] area Encoded area.
 * @param[out] chassis_out Decoded structure.
 * @retval true Success.
 * @retval false Failure.
 */
static bool fru_decode_chassis_info(const fru__file_chassis_t *area, fru_chassis_t *chassis_out);

/**
 * @brief Decode board area into \p fru_board_t.
 *
 * @param[in] area Encoded area.
 * @param[out] chassis_out Decoded structure.
 * @retval true Success.
 * @retval false Failure.
 */
static bool fru_decode_board_info(const fru__file_board_t *area, fru_board_t *board_out);

/**
 * @brief Decode product area into \p fru_product_board_t.
 *
 * @param[in] area Encoded area.
 * @param[out] chassis_out Decoded structure.
 * @retval true Success.
 * @retval false Failure.
 */
static bool fru_decode_product_info(const fru__file_product_t *area, fru_product_t *product_out);

/**
 * @brief Decode multirecord area from \p fru__file_mr_t into a record list
 *
 * @param[in] area Encoded area.
 * @param[out] mr_reclist Pointer to the record list head
 * @returns The number of records decoded from the multirecord are into the list
 * @retval -1 - Failure, errno is set accordingly.
 * @retval >= 0 - The number of records added to \a mr_reclist
 */
static int fru_decode_mr_area(const fru__file_mr_t *area,
                              fru_mr_reclist_t **reclist,
                              size_t mr_size,
                              fru_flags_t flags);

/** @endcond */
