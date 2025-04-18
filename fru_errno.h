/** @file
 *  @brief Header for FRU error codes
 *
 *  Copyright (C) 2016-2021 Alexander Amelkin <alexander@amelkin.msk.ru>
 *  SPDX-License-Identifier: LGPL-2.0-or-later OR Apache-2.0
 */
#pragma once

/**
 * @addtogroup common
 * @brief Common definitions for the library
 *
 * @{
 */

/**
 * Defines the errors specific to \a libfru.
 * These values are applicable to \ref fru_errno.code
 */
typedef enum {
    FENONE,          /**< No libfru error */
    FEGENERIC,       /**< Generic error, check errno */
    FEINIT,          /**< Uninitialized FRU structure */
    FENONPRINT,      /**< Field data contains non-printable bytes */
    FENONHEX,        /**< Input string contains non-hex characters */
    FERANGE,         /**< Field data exceeds range for the requested encoding */
    FENOTEVEN,       /**< Not an even number of nibbles */
    FEAUTOENC,       /**< Unable to auto-detect encoding */
    FEBADENC,        /**< Invalid encoding for a field */
    FE2SMALL,        /**< File is too small */
    FE2BIG,          /**< Data or file is too big */
    FESIZE,          /**< Data size mismatch */
    FEHDRVER,        /**< Bad header version */
    FEHDRCKSUM,      /**< Bad header checksum */
    FEHDRBADPTR,     /**< Area pointer beyond the end of file/buffer */
    FEDATACKSUM,     /**< Bad data checksum */
    FEAREADUP,       /**< Duplicate area in area order */
    FEAREANOTSUP,    /**< Unsupported area type (For a particular operation) */
    FEAREABADTYPE,   /**< Bad area type (A completely wrong value) */
    FENOTERM,        /**< Unterminated area */
    FEBDATE,         /**< Board manufacturing date is out of range */
    FENOFIELD,       /**< No such field */
    FENOREC,         /**< No such record */
    FEBADDATA,       /**< Malformed data */
    FENODATA,        /**< No data */
    FEMRMGMTBAD,     /**< Bad management record subtype */
    FEMRNOTSUP,      /**< Unsupported record type */
    FEMREND,         /**< End of MR records (not an error) */
    FEAPOS,          /**< Invalid area position */
    FENOTEMPTY,      /**< List is not empty */
    FEAENABLED,      /**< Area is (already) enabled */
    FEADISABLED,     /**< Area is (already) disabled */
    FELIB,           /**< Internal library error (bug) */
    FETOTALCOUNT,    /**< The total count of possible libfru error codes */
} fru_error_code_t;

/**
 * @brief Describes an offending area, see \ref fru_errno_t
 *
 * For area-related values this enum matches fru_area_type_t
 */
typedef enum {
	FERR_LOC_INTERNAL, /**< The error is in the internal use area */
	FERR_LOC_CHASSIS, /**< The error is in the chassis information area */
	FERR_LOC_BOARD, /**< The error is in the board information area */
	FERR_LOC_PRODUCT, /**< The error is in the product information area */
	FERR_LOC_MR, /**< The error is in the multirecord area */
	FERR_LOC_GENERAL, /**< The error is about FRU file or structure in general,
	                   *   not about any specific area. This is also used when
	                   *   a non-area specific call, such as \ref fru_setfield(),
	                   *   encounters an error.
	                   */
	FERR_LOC_CALLER, /**< The error is in the calling code (bad arguments?) */
	FERR_LOC_COUNT /**< The total count of possible error locations */
} fru_error_source_t;

typedef struct {
	fru_error_code_t code;
	fru_error_source_t src; /**< The source of error */
	int index; /**< Index of the offending entity within the source, that is, an
	            *   index of a field (for FRU_*_INFO) or a record (for FRU_MR).
	            *
	            *   This is (-1) when not applicable. That is, the area indicated by
	            *   \a src doesn't have any fields/records, or the error is in the
	            *   area as a whole, not in some particular field/record, or the error
	            *   is not in a FRU area at all (e.g., src == \ref FERR_LOC_CALLER).
	            *
	            *   For info areas this is less than the respective `FRU_*_FIELD_COUNT`
	            *   for the standard/mandatory fields, or is `FRU_*_FIELD_COUNT + X`, where
	            *   `X` is the index of the offending custom field.
	            *
	            *   For \ref FEAREANOTSUP and \ref FEAREABADTYPE this field contains
	            *   the bad area type (see \ref fru_area_type_t).
	            */
} fru_errno_t;

/**
 * @brief Numeric code of an error
 *
 * Similar to `errno`, this thread-local variable is modified
 * by most functons in \a libfru in the event of an error.
 *
 * Call \ref fru_clearerr() before invocation of any \a libfru function,
 * check for non-zero values of the `code` field after the invocation.
 *
 * Unlike `errno`, this variable contains the information about not just the
 * error, but also about the part of FRU where it was detected.
 * If the `fru_errno.code` is \ref FEGENERIC, then you
 * need to check `errno` for the actual error.
 *
 * Please also see \ref fru_strerr()
 */
extern __thread fru_errno_t fru_errno;

/**
 * @brief Get a description of the given \p fru_errno value.
 *
 * Converts a numeric error represented by \ref fru_errno into a string
 * description of the error. If \ref fru_errno is \ref FEGENERIC, this
 * function will call \p strerror() to obtain the description of the
 * generic libc error.
 * 
 * @returns A pointer to a constant string with the description of the error
 * @retval NULL No description found for the given value.
 */
const char * fru_strerr(fru_errno_t);

/**
 * @brief Clear the error status of the library
 *
 * Use this before library calls that may set \ref fru_errno without
 * returning an error status to ensure that the value of \ref fru_errno
 * that you get has indeed been set by the function you've called.
 */
void fru_clearerr(void);

/* @} */
