/*
 *  @brief Header for FRU information helper functions
 *
 *  Copyright (C) 2016-2024 Alexander Amelkin <alexander@amelkin.msk.ru>
 *  SPDX-License-Identifier: LGPL-2.0-or-later OR Apache-2.0
 */

#pragma once

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#define ARRAY_SZ(a) (sizeof(a) / sizeof((a)[0]))
#define FRU_BIT(x) (1 << (x))

/** Flags to FRU decoding functions */
typedef enum {
	FRU_NOFLAGS = 0,
	FRU_IGNFVER = FRU_BIT(0), /// Ignore FRU version (in FRU header)
	FRU_IGNFHCKSUM = FRU_BIT(1), /// Ignore FRU header checksum
	FRU_IGNFDCKSUM = FRU_BIT(2), /// Ignore FRU data checksum
	FRU_IGNAVER = FRU_BIT(3), /// Ignore area version
	FRU_IGNRVER = FRU_BIT(4), /// Ignore record version (for multirecord area)
	FRU_IGNACKSUM = FRU_BIT(5), /// Ignore area checksum
	FRU_IGNRHCKSUM = FRU_BIT(6), /// Ignore record header checksum (for multirecord area)
	FRU_IGNRDCKSUM = FRU_BIT(7), /// Ignore record data checksum (for multirecord area)
	FRU_IGNRNOEOL = FRU_BIT(8), /// Ignore no EOL-flagged record, use any previous valid records
} fru_flags_t;

typedef struct fru_s {
	uint8_t ver:4, rsvd:4;
	uint8_t internal;
	uint8_t chassis;
	uint8_t board;
	uint8_t product;
	uint8_t multirec;
	uint8_t pad;
	uint8_t hchecksum; ///< Header checksum
	uint8_t data[];
} fru_t;

typedef enum fru_area_type_e {
	FRU_AREA_NOT_PRESENT = -1,
	FRU_INTERNAL_USE,
	FRU_CHASSIS_INFO,
	FRU_BOARD_INFO,
	FRU_PRODUCT_INFO,
	FRU_MULTIRECORD,
	FRU_MAX_AREAS
} fru_area_type_t;

typedef enum {
	FRU_CHASSIS_PARTNO,
	FRU_CHASSIS_SERIAL,
	FRU_CHASSIS_FIELD_COUNT
} fru_chassis_field_t;

typedef enum {
	FRU_BOARD_MFG,
	FRU_BOARD_PRODNAME,
	FRU_BOARD_SERIAL,
	FRU_BOARD_PARTNO,
	FRU_BOARD_FILE,
	FRU_BOARD_FIELD_COUNT
} fru_board_field_t;

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

/// Table 16-2, MultiRecord Area Record Types
typedef enum {
	FRU_MR_MIN = 0x00,
	FRU_MR_PSU_INFO = 0x00,
	FRU_MR_DC_OUT = 0x01,
	FRU_MR_DC_LOAD = 0x02,
	FRU_MR_MGMT_ACCESS = 0x03,
	FRU_MR_BASE_COMPAT = 0x04,
	FRU_MR_EXT_COMPAT = 0x05,
	FRU_MR_ASF_FIXED_SMBUS = 0x06,
	FRU_MR_ASF_LEGACY_ALERTS = 0x07,
	FRU_MR_ASF_REMOTE_CTRL = 0x08,
	FRU_MR_EXT_DC_OUT = 0x09,
	FRU_MR_EXT_DC_LOAD = 0x0A,
	FRU_MR_NVME_B = 0x0B,
	FRU_MR_NVME_C = 0x0C,
	FRU_MR_NVME_D = 0x0D,
	FRU_MR_NVME_E = 0x0E,
	FRU_MR_NVME_F = 0x0F,
	FRU_MR_OEM_START = 0xC0,
	FRU_MR_OEM_END = 0xFF,
	FRU_MR_MAX = 0xFF
} fru_mr_type_t;

/// Table 18-1, Power Supply Information
#pragma pack(push, 1)
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
} __attribute__((packed)) fru_mr_psu_t;
#pragma pack(pop)

/// Table 18-6, Management Access Record
typedef enum {
	FRU_MR_MGMT_INVALID = 0x00,
	FRU_MR_MGMT_MIN = 0x01,
	FRU_MR_MGMT_SYS_URL = 0x01,
	FRU_MR_MGMT_SYS_NAME = 0x02,
	FRU_MR_MGMT_SYS_PING = 0x03,
	FRU_MR_MGMT_COMPONENT_URL = 0x04,
	FRU_MR_MGMT_COMPONENT_NAME = 0x05,
	FRU_MR_MGMT_COMPONENT_PING = 0x06,
	FRU_MR_MGMT_SYS_UUID = 0x07,
	FRU_MR_MGMT_MAX = 0x07,
} fru_mr_mgmt_type_t;
#define MGMT_TYPE_ID(type) ((type) - FRU_MR_MGMT_MIN)
#define MGMT_TYPENAME_ID(name) MGMT_TYPE_ID(FRU_MR_MGMT_##name)

#define FRU_IS_ATYPE_VALID(t) ((t) >= FRU_AREA_NOT_PRESENT && (t) < FRU_MAX_AREAS)

/**
 * FRU area description structure.
 *
 * Contains information about a single arbitrary area.
 */
typedef struct fru_area_s {
	fru_area_type_t atype; /**< FRU area type */
	uint8_t blocks; /**< Size of the data field in 8-byte blocks */
	void * data; /**< Pointer to the actual FRU area data */
} fru_area_t;

#define FRU_VER_1    1
#define FRU_MINIMUM_AREA_HEADER \
	uint8_t ver;  /**< Area format version, only lower 4 bits */

#define FRU_INFO_AREA_HEADER \
	FRU_MINIMUM_AREA_HEADER; \
	uint8_t blocks;         /**< Size in 8-byte blocks */ \
	uint8_t langtype        /**< Area language code or chassis type (from smbios.h) */

#define LANG_DEFAULT 0
#define LANG_ENGLISH 25

typedef struct fru_info_area_s { // The generic info area structure
	FRU_INFO_AREA_HEADER;
	char data[];
} fru_info_area_t;

#define FRU_INFO_AREA_HEADER_SZ sizeof(fru_info_area_t)

typedef struct fru_internal_use_area_s {
	FRU_MINIMUM_AREA_HEADER;
	char data[];
} fru_internal_use_area_t;

typedef fru_info_area_t fru_chassis_area_t;

typedef struct fru_board_area_s {
	FRU_INFO_AREA_HEADER;
	uint8_t mfgdate[3]; ///< Manufacturing date/time in minutes since 1996/1/1 0:00
	char data[];     ///< Variable size (multiple of 8 bytes) data with tail padding and checksum
} fru_board_area_t;

typedef fru_info_area_t fru_product_area_t;

#pragma pack(push, 1)
typedef struct {
	uint8_t type_id; ///< Record Type ID
	uint8_t eol_ver;
#define FRU_MR_EOL 0x80 // End-of-list flag
#define FRU_MR_VER_MASK 0x07
#define FRU_MR_VER 0x02 // Version is always 2
	uint8_t len;           ///< Length of the raw `data` array
	uint8_t rec_checksum;
	uint8_t hdr_checksum;
} __attribute__((packed)) fru_mr_header_t;

typedef struct {
	fru_mr_header_t hdr;
#define IS_FRU_MR_END(rec) ((rec)->hdr.eol_ver & FRU_MR_EOL)
#define IS_FRU_MR_VALID_VER(rec) \
            (((rec)->hdr.eol_ver & FRU_MR_VER_MASK) == FRU_MR_VER)
	uint8_t data[];        ///< Raw data of size `len`
} __attribute__((packed)) fru_mr_rec_t;
#define FRU_MR_REC_SZ(rec) (sizeof(fru_mr_rec_t) + (rec)->hdr.len)

typedef struct {
	fru_mr_header_t hdr;
	uint8_t subtype;
	uint8_t data[];
} __attribute__((packed)) fru_mr_mgmt_rec_t;

typedef struct fru_mr_reclist_s {
	fru_mr_rec_t *rec;
	struct fru_mr_reclist_s *next;
} fru_mr_reclist_t;

typedef fru_mr_rec_t fru_mr_area_t; /// Intended for use as a pointer only
#pragma pack(pop)

#define FRU_AREA_HAS_DATE(t) (FRU_BOARD_INFO == (t))
#define FRU_DATE_AREA_HEADER_SZ sizeof(fru_board_area_t)
#define FRU_DATE_UNSPECIFIED 0

#define FRU_AREA_HAS_SIZE(t) (FRU_INTERNAL_USE < (t) && (t) < FRU_MULTIRECORD)
#define FRU_AREA_HAS_HEADER(t) (FRU_MULTIRECORD != (t))
#define FRU_AREA_IS_GENERIC(t) FRU_AREA_HAS_SIZE(t)

#define __TYPE_BITS_SHIFT     6
#define __TYPE_BITS_MASK      0xC0
#define __TYPE_BINARY         0x00
#define __TYPE_BCDPLUS        0x01
#define __TYPE_ASCII_6BIT     0x02
#define __TYPE_TEXT           0x03

/** FRU field type. Any of BINARY, BCDPLUS, ASCII_6BIT or TEXT. */
#define FRU_MAKETYPE(x)       (__TYPE_##x << __TYPE_BITS_SHIFT)
#define FRU_FIELDDATALEN(x)   (size_t)(((x) & ~__TYPE_BITS_MASK))
#define FRU_FIELDMAXLEN       FRU_FIELDDATALEN(UINT8_MAX) /// For FRU fields
#define FRU_FIELDMAXARRAY     ((2 * FRU_FIELDMAXLEN) + 1) /// For C array allocation, BCD+ decoded data is twice the length of encoded
#define FRU_FIELDSIZE(typelen) (FRU_FIELDDATALEN(typelen) + sizeof(fru_field_t))
#define FRU_TYPELEN(t, l)     (FRU_MAKETYPE(t) | FRU_FIELDDATALEN(l))
#define FRU_TYPE(t)           (((t) & __TYPE_BITS_MASK) >> __TYPE_BITS_SHIFT)
#define FRU_ISTYPE(t, type)   (FRU_TYPE(t) == __TYPE_##type)

#define LEN_AUTO              0
#define LEN_BCDPLUS           -1
#define LEN_6BITASCII         -2
#define LEN_TEXT              -3

#define FRU_6BIT_LENGTH(len)    (((size_t)(len) * 3 + 3) / 4)
#define FRU_6BIT_FULLLENGTH(l6) (((size_t)(l6) * 4) / 3)
#define FRU_FIELD_EMPTY       FRU_TYPELEN(TEXT, 0)
#define FRU_FIELD_TERMINATOR  FRU_TYPELEN(TEXT, 1)

#define FRU_BLOCK_SZ 8
#define FRU_BYTES(blocks) ((blocks) * FRU_BLOCK_SZ)
#define FRU_BLOCKS(bytes)  (((bytes) + FRU_BLOCK_SZ - 1) / FRU_BLOCK_SZ)

typedef enum {
    FIELD_TYPE_TERMINATOR = -4,
    FIELD_TYPE_UNKNOWN = -3,
    FIELD_TYPE_NONPRINTABLE = -2,
    FIELD_TYPE_TOOLONG = -1,
    FIELD_TYPE_AUTO = 0,
    FIELD_TYPE_EMPTY = FIELD_TYPE_AUTO,
    FIELD_TYPE_BINARY = (__TYPE_BINARY + 1),
    BASE_FIELD_TYPE = FIELD_TYPE_BINARY,
    FIELD_TYPE_BCDPLUS = (__TYPE_BCDPLUS + 1),
    FIELD_TYPE_6BITASCII = (__TYPE_ASCII_6BIT + 1),
    FIELD_TYPE_TEXT = (__TYPE_TEXT + 1),
    TOTAL_FIELD_TYPES
} field_type_t;

const char * fru_enc_name_by_type(field_type_t type);
field_type_t fru_enc_type_by_name(const char *name);

/// Extract FRU field type as field_type_t
#define FIELD_TYPE_T(t) (FRU_TYPE(t) + BASE_FIELD_TYPE)

/**
 * A generic input (decoded) field structure containing
 * the field encoding type and the input data as a string.
 */
typedef struct {
	field_type_t type;
	char val[FRU_FIELDMAXARRAY];
} decoded_field_t;

/**
 * A generic FRU area encoded field header structure.
 *
 * Every field in chassis, board and product information areas has such a header.
 */
typedef struct fru_field_s {
	uint8_t typelen;   /**< Type/length of the field */
	uint8_t data[];    /**< The field data */
} fru_field_t;

/**
 * A single-linked list of decoded FRU area fields.
 *
 * This is used to describe any length chains of fields.
 * Mandatory fields are linked first in the order they appear
 * in the information area (as per Specification), then custom
 * fields are linked.
 */
typedef struct fru_reclist_s {
	decoded_field_t *rec; /**< A pointer to the field or NULL if not initialized */
	struct fru_reclist_s *next; /**< The next record in the list or NULL if last */
} fru_reclist_t;

/**
 * Allocate a new reclist entry and add it to \a reclist,
 * set \a reclist to point to the newly allocated entry if
 * \a reclist was NULL.
 *
 * @returns Pointer to the added entry
 */
static inline void *add_generic_reclist(void *genlist_ptr, size_t rec_size)
{
	/**
	 * A generic single-linked list abstraction.
	 * This is used as a substitute for all other list types in the library.
	 */
	struct genlist_s {
		void *rec; /* A pointer to the field or NULL if not initialized */
		void *next; /* The next record in the list or NULL if last */
	} *rec, **reclist = genlist_ptr, *reclist_ptr = *reclist;

	if (!genlist_ptr) {
		errno = EFAULT;
		return NULL;
	}

	rec = calloc(1, rec_size);
	if(!rec) return NULL;

	// If the reclist is empty, update it
	if(!(*reclist)) {
		*reclist = rec;
	} else {
		// If the reclist is not empty, find the last entry and append the new one as next
		while(reclist_ptr->next)
			reclist_ptr = reclist_ptr->next;

		reclist_ptr->next = rec;
	}

	return rec;
}

/// A wrapper around add_generic_reclist() to work with fru_reclist_t** and fru_mr_reclist_t**
#define add_reclist(reclist) \
	add_generic_reclist((reclist), sizeof(*(*reclist)->rec))

/// Works for any list (fru_generic_reclist_t*, fru_reclist_t*, fru_mr_reclist_t*)
#define free_reclist(recp) while(recp) { \
	typeof((recp)->next) next = (recp)->next; \
	free((recp)->rec); \
	free(recp); \
	recp = next; \
}

typedef struct {
	uint8_t type;
	decoded_field_t pn;
	decoded_field_t serial;
	fru_reclist_t *cust;
} fru_exploded_chassis_t;

typedef struct {
	uint8_t lang;
	struct timeval tv;
	decoded_field_t mfg;
	decoded_field_t pname;
	decoded_field_t serial;
	decoded_field_t pn;
	decoded_field_t file;
	fru_reclist_t *cust;
} fru_exploded_board_t;

typedef struct {
	uint8_t lang;
	decoded_field_t mfg;
	decoded_field_t pname;
	decoded_field_t pn;
	decoded_field_t ver;
	decoded_field_t serial;
	decoded_field_t atag;
	decoded_field_t file;
	fru_reclist_t *cust;
} fru_exploded_product_t;

typedef struct {
	char *internal_use; /// In exploded view this is a hex string
	fru_exploded_chassis_t chassis;
	fru_exploded_board_t board;
	fru_exploded_product_t product;
	fru_mr_reclist_t *mr_reclist;
} fru_exploded_t;

#define fru_loadfield(eafield, value) strncpy((char *)eafield, value, FRU_FIELDMAXLEN)

void fru_set_autodetect(bool enable);

fru_chassis_area_t * fru_chassis_info(const fru_exploded_chassis_t *chassis);
fru_board_area_t * fru_board_info(const fru_exploded_board_t *board);
fru_product_area_t * fru_product_info(const fru_exploded_product_t *product);

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
int fru_mr_mgmt_str2rec(fru_mr_rec_t **rec,
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
int fru_mr_uuid2rec(fru_mr_rec_t **rec, const char *str);

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
int fru_mr_rec2uuid(char **str, fru_mr_mgmt_rec_t *rec, fru_flags_t flags);

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
int fru_mr_mgmt_rec2str(char **str, fru_mr_mgmt_rec_t *mgmt,
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
 * @returns fru_internal_use_area_t *area A newly allocated buffer containing
 *                                        the created area
 */
fru_internal_use_area_t *fru_encode_internal_use_area(const void *data, uint8_t *blocks);

/**
 * @brief Encode chassis info into binary buffer.
 *
 * Binary buffer needs to be freed after use.
 *
 * @param[in] chassis Area info.
 * @return Encoded area buffer.
 * @retval NULL Encoding failed. \p errno is set accordingly.
 */
fru_chassis_area_t * fru_encode_chassis_info(fru_exploded_chassis_t *chassis);

/**
 * @brief Encode board info into binary buffer.
 *
 * Binary buffer needs to be freed after use.
 *
 * @param[in] board Area info.
 * @return Encoded area buffer.
 * @retval NULL Encoding failed. \p errno is set accordingly.
 */
fru_board_area_t * fru_encode_board_info(fru_exploded_board_t *board);

/**
 * @brief Encode product info into binary buffer.
 *
 * Binary buffer needs to be freed after use.
 *
 * @param[in] product Area info.
 * @return Encoded area buffer.
 * @retval NULL Encoding failed. \p errno is set accordingly.
 */
fru_product_area_t * fru_encode_product_info(fru_exploded_product_t *product);

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
 * @returns fru_mr_area_t *area  A newly allocated buffer containing the
 *                               created area
 *
 */
fru_mr_area_t * fru_encode_mr_area(fru_mr_reclist_t *reclist, size_t *total);

fru_t * fru_create(fru_area_t area[FRU_MAX_AREAS], size_t *size);

/**
 * @brief Find and validate FRU header in the byte buffer.
 *
 * @param[in] buffer Byte buffer.
 * @param[in] size Byte buffer size.
 * @return Pointer to the FRU header in the buffer.
 * @retval NULL FRU header not found. \p errno is set accordingly.
 */
fru_t *find_fru_header(uint8_t *buffer, size_t size, fru_flags_t flags);

/**
 * @brief Find and validate FRU chassis area in the byte buffer.
 *
 * @param[in] buffer Byte buffer.
 * @param[in] size Byte buffer size.
 * @return Pointer to the FRU chassis area in the buffer.
 * @retval NULL FRU chassis area not found. \p errno is set accordingly.
 */
fru_chassis_area_t *find_fru_chassis_area(uint8_t *buffer, size_t size, fru_flags_t flags);

/**
 * @brief Find and validate FRU board area in the byte buffer.
 *
 * @param[in] buffer Byte buffer.
 * @param[in] size Byte buffer size.
 * @return Pointer to the FRU board area in the buffer.
 * @retval NULL FRU board area not found. \p errno is set accordingly.
 */
fru_board_area_t *find_fru_board_area(uint8_t *buffer, size_t size, fru_flags_t flags);

/**
 * @brief Find and validate FRU product area in the byte buffer.
 *
 * @param[in] buffer Byte buffer.
 * @param[in] size Byte buffer size.
 * @return Pointer to the FRU product area in the buffer.
 * @retval NULL FRU product area not found. \p errno is set accordingly.
 */
fru_product_area_t *find_fru_product_area(uint8_t *buffer, size_t size, fru_flags_t flags);

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
fru_internal_use_area_t *find_fru_internal_use_area(
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
fru_mr_area_t *find_fru_mr_area(uint8_t *buffer, size_t *mr_size, size_t size, fru_flags_t flags);

/**
 * @brief Decode chassis area into \p fru_exploded_chassis_t.
 *
 * @param[in] area Encoded area.
 * @param[out] chassis_out Decoded structure.
 * @retval true Success.
 * @retval false Failure.
 */
bool fru_decode_chassis_info(const fru_chassis_area_t *area, fru_exploded_chassis_t *chassis_out);

/**
 * @brief Decode board area into \p fru_exploded_board_t.
 *
 * @param[in] area Encoded area.
 * @param[out] chassis_out Decoded structure.
 * @retval true Success.
 * @retval false Failure.
 */
bool fru_decode_board_info(const fru_board_area_t *area, fru_exploded_board_t *board_out);

/**
 * @brief Decode product area into \p fru_product_board_t.
 *
 * @param[in] area Encoded area.
 * @param[out] chassis_out Decoded structure.
 * @retval true Success.
 * @retval false Failure.
 */
bool fru_decode_product_info(const fru_product_area_t *area, fru_exploded_product_t *product_out);

/**
 * @brief Decode multirecord area from \p fru_mr_area_t into a record list
 *
 * @param[in] area Encoded area.
 * @param[out] mr_reclist Pointer to the record list head
 * @returns The number of records decoded from the multirecord are into the list
 * @retval -1 - Failure, errno is set accordingly.
 * @retval >= 0 - The number of records added to \a mr_reclist
 */
int fru_decode_mr_area(const fru_mr_area_t *area,
                       fru_mr_reclist_t **reclist,
                       size_t mr_size,
                       fru_flags_t flags);

/**
 * @brief Decode internal use area \a area into a hex string
 *
 * The function allocates a string buffer that a caller must free()
 * when not needed anymore.
 *
 * @param[in]  area     Encoded area.
 * @param[in]  area_len The full area length as calculated by \p find_fru_internal_use_area()
 * @param[out] out      Pointer to a string buffer to be allocated and filled in with the data
 * @param[in]  flags    The debug flags (unused here)
 * @returns The boolean success indicator
 * @retval true  - Success
 * @retval false - An error occured, errno is set accordingly
 */
bool fru_decode_internal_use_area(const fru_internal_use_area_t *area,
                                  size_t area_len,
                                  char **out,
                                  fru_flags_t flags __attribute__((unused)));


/**
 * Decode data from a buffer into another buffer.
 *
 * For binary data use FRU_FIELDDATALEN(field->typelen) to find
 * out the size of valid bytes in the returned buffer.
 *
 * @param[in] field Encoded data field.
 * @param[out] out Decoded field.
 * @retval true Success.
 * @retval false Failure.
 */
bool fru_decode_data(fru_field_t *field, decoded_field_t *out);
