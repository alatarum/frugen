/**
 *  @file Header for FRU error codes
 *
 *  Copyright (C) 2016-2021 Alexander Amelkin <alexander@amelkin.msk.ru>
 *  SPDX-License-Identifier: LGPL-2.0-or-later OR Apache-2.0
 */
#pragma once

/**
 * Defines the errors specific to libfru.
 * Number range is supposed to include standard errno codes.
 */
typedef enum {
    FEBASE = 10000, /* Ofsset to safely include errno range */
    FELONGINPUT = FEBASE,  /* Input string is too long */
    FENONPRINT,            /* Field data contains non-printable bytes */
    FENONHEX,              /* Input string contains non-hex characters */
    FERANGE,               /* Field data exceeds range for the requested encoding */
    FENOTEVEN,             /* Input string must contain an even number of nibbles for binary encoding */
    FEBADENC,              /* Invalid encoding for a field */
    FETOOSMALL,            /* FRU file is too small */
    FEHDRVER,              /* FRU header has bad version */
    FEHDRBADPTR,           /* FRU header points to an area beyond the end of file */
    FENOSUCHAREA,          /* Area not found in the header */
    FEAREANOTSUP,          /* Area type not supported */
    FEAREABADTYPE,         /* Invalid area type */
    FEAREAVER,             /* Invalid area version */
    FEAREACKSUM,           /* Invalid area checksum */
    FEHEXLEN,              /* Input hex string must contain even number of bytes */
    FEINVCHAS,             /* Invalid chassis type (not per SMBIOS spec) */
    FEMRNODATA,            /* MR Record is empty */
    FEMRVER,               /* MR Record has bad version */
    FEMRHCKSUM,            /* MR Record has wrong header checksum */
    FEMRDCKSUM,            /* MR Record has wrong data checksum */
    FEMRMGMTRANGE,         /* MR Management Record type is out of range */
    FEMRMGMTSIZE,          /* MR Management Record has wrong size */
    FEMRMGMTTYPE,          /* MR Management Record is of wrong type */
    FETOTALCOUNT
} fru_errno_t;

/**
 * @brief Numeric code of an error
 *
 * Similar to errno, this variable is modified by most functons
 * in libfru in the event of an error.
 *
 * Set this to 0 before invocation of any libfru function.
 * Check for non-zero values after the invocation.
 *
 * Non-zero values below 10000 represent the standard errno codes
 * and can be decoded with strerr().
 *
 * Both the standard errno and extended fru-specific codes can be
 * decoded with fru_strerr()
 */
extern __thread fru_errno_t fru_errno;

/**
 * @brief Get a description of the given fru_errno value.
 * @returns A pointer to a constant string with the description
 * @retval NULL No description found for the given value.
 */
const char * fru_strerr(fru_errno_t);
