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
 * These values are applicable to \ref fru_errno.
 */
typedef enum {
    FENONE,                /**< No libfru error */
    FEGENERIC,             /**< Generic error, see fru_errno.err */
    FEINIT,                /**< FRU structure was not properly initialized */
    FELONGINPUT,           /**< Input string is too long */
    FENONPRINT,            /**< Field data contains non-printable bytes */
    FENONHEX,              /**< Input string contains non-hex characters */
    FERANGE,               /**< Field data exceeds range for the requested encoding */
    FENOTEVEN,             /**< Input string must contain an even number of nibbles for binary encoding */
    FEAUTOENC,             /**< Unable to auto-detect encoding */
    FEBADENC,              /**< Invalid encoding for a field */
    FETOOSMALL,            /**< FRU file is too small */
    FETOOBIG,              /**< FRU file is too big */
    FEHDRVER,              /**< FRU header has bad version */
    FEHDRCKSUM,            /**< FRU header has wrong checksum */
    FEHDRBADPTR,           /**< FRU header points to an area beyond the end of file */
    FEAREADUP,             /**< Area has already been processed (invalid order) */
    FEAREANOTSUP,          /**< Area type not supported */
    FEAREABADTYPE,         /**< Invalid area type */
    FEAREAVER,             /**< Invalid area version */
    FEAREACKSUM,           /**< Invalid area checksum */
    FEAREANOEOF,           /**< Area doesn't have an end-of-fields marker */
    FEBDATE,               /**< Board manufacturing date is out of range */
    FENOFIELD,             /**< No such field */
    FEMRNOREC,             /**< No such record in MultiRecord area */
    FEMRNODATA,            /**< MR Record is empty */
    FEMRNOTEMPTY,          /**< MR Area is not empty */
    FEMRVER,               /**< MR Record has bad version */
    FEMRHCKSUM,            /**< MR Record has wrong header checksum */
    FEMRDCKSUM,            /**< MR Record has wrong data checksum */
    FEMRDSIZE,             /**< Too many data to fit into an MR Record */
    FEMRMGMTRANGE,         /**< MR Management Record type is out of range */
    FEMRMGMTSIZE,          /**< MR Management Record has wrong size */
    FEMRMGMTTYPE,          /**< MR Management Record is of wrong type */
    FEMRNOTSUP,            /**< MR Record type is not supported */
    FENOENTRY,             /**< No such list entry */
    FEMREND,               /**< End of MR records (not an error) */
    FEAPOS,                /**< Invalid area position */
    FEAENABLED,            /**< Area already enabled */
    FEADISABLED,           /**< Area already disabled */
    FETOTALCOUNT           /**< Total count of FRU-specific codes */
} fru_errno_t;

/**
 * @brief Numeric code of an error
 *
 * Similar to \p errno, this thread-local variable is modified
 * by most functons in \a libfru in the event of an error.
 *
 * Set this to \ref FENONE before invocation of any \a libfru function,
 * check for non-zero values after the invocation.
 *
 * If this is \ref FEGENERIC, then please check \p errno for the actual
 * error code.
 *
 * Please also see fru_strerr().
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

/* @} */
