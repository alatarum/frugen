/** @file
 *  @brief FRU error codes implementation
 *
 *  Copyright (C) 2016-2021 Alexander Amelkin <alexander@amelkin.msk.ru>
 *  SPDX-License-Identifier: LGPL-2.0-or-later OR Apache-2.0
 */
#include <string.h>
#include "fru-errno.h"

__thread fru_errno_t fru_errno;

static const char * fru_errno_string[FETOTALCOUNT] = {
    [FENONE]                = "No libfru error",
    [FEGENERIC]             = "Generic error, check errno",
    [FELONGINPUT]           = "Input string is too long",
    [FENONPRINT]            = "Field data contains non-printable bytes ",
    [FENONHEX]              = "Input string contains non-hex characters ",
    [FERANGE]               = "Field data exceeds range for the requested encoding ",
    [FENOTEVEN]             = "Input string must contain an even number of nibbles for binary encoding ",
    [FEBADENC]              = "Invalid encoding for a field ",
    [FETOOSMALL]            = "FRU file is too small ",
    [FETOOBIG],             = "FRU file is too big",
    [FEHDRVER]              = "FRU header has bad version ",
    [FEHDRCKSUM]            = "FRU header has wrong checksum",
    [FEHDRBADPTR]           = "FRU header points to an area beyond the end of file ",
    [FENOSUCHAREA]          = "Area not found in the header ",
    [FEAREANOTSUP]          = "Area type not supported ",
    [FEAREABADTYPE]         = "Invalid area type ",
    [FEAREAVER]             = "Invalid area version ",
    [FEAREACKSUM]           = "Invalid area checksum ",
    [FEAREANOEOF]           = "Area doesn't have an end-of-fields marker",
    [FEINVCHAS]             = "Invalid chassis type (not per SMBIOS spec) ",
    [FENOFIELD]             = "No such custom field",
    [FEMRNOREC]             = "No such record in MR area",
    [FEMRNODATA]            = "MR Record is empty ",
    [FEMRVER]               = "MR Record has bad version ",
    [FEMRHCKSUM]            = "MR Record has wrong header checksum ",
    [FEMRDCKSUM]            = "MR Record has wrong data checksum ",
    [FEMRMGMTRANGE]         = "MR Management Record type is out of range ",
    [FEMRMGMTSIZE]          = "MR Management Record has wrong size ",
    [FEMRMGMTTYPE]          = "MR Management Record is of wrong type ",
    [FEMRNOTSUP]            = "MR Record type is not supported",
    [FEMREND]               = "End of MR records",
};

const char * fru_strerr(fru_errno_t ferr)
{
	if (ferr < FENONE || ferr >= FETOTALCOUNT) {
		return "Undefined libfru error";
	}

	if (ferr == FEGENERIC)
		return strerror(ferr);

	return fru_errno_string[ferr];
}
