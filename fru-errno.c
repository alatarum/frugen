/**
 *  @file FRU error codes implementation
 *
 *  Copyright (C) 2016-2021 Alexander Amelkin <alexander@amelkin.msk.ru>
 *  SPDX-License-Identifier: LGPL-2.0-or-later OR Apache-2.0
 */
#include <string.h>
#include "fru-errno.h"

__thread fru_errno_t fru_errno;

static const char * fru_errno_string[FETOTALCOUNT - FEBASE] = {
    [FELONGINPUT - FEBASE]           = "Input string is too long",
    [FENONPRINT - FEBASE]            = "Field data contains non-printable bytes ",
    [FENONHEX - FEBASE]              = "Input string contains non-hex characters ",
    [FERANGE - FEBASE]               = "Field data exceeds range for the requested encoding ",
    [FENOTEVEN - FEBASE]             = "Input string must contain an even number of nibbles for binary encoding ",
    [FEBADENC - FEBASE]              = "Invalid encoding for a field ",
    [FETOOSMALL - FEBASE]            = "FRU file is too small ",
    [FEHDRVER - FEBASE]              = "FRU header has bad version ",
    [FEHDRBADPTR - FEBASE]           = "FRU header points to an area beyond the end of file ",
    [FENOSUCHAREA - FEBASE]          = "Area not found in the header ",
    [FEAREANOTSUP - FEBASE]          = "Area type not supported ",
    [FEAREABADTYPE - FEBASE]         = "Invalid area type ",
    [FEAREAVER - FEBASE]             = "Invalid area version ",
    [FEAREACKSUM - FEBASE]           = "Invalid area checksum ",
    [FEHEXLEN - FEBASE]              = "Input hex string must contain even number of bytes ",
    [FEINVCHAS - FEBASE]             = "Invalid chassis type (not per SMBIOS spec) ",
    [FEMRNODATA - FEBASE]            = "MR Record is empty ",
    [FEMRVER - FEBASE]               = "MR Record has bad version ",
    [FEMRHCKSUM - FEBASE]            = "MR Record has wrong header checksum ",
    [FEMRDCKSUM - FEBASE]            = "MR Record has wrong data checksum ",
    [FEMRMGMTRANGE - FEBASE]         = "MR Management Record type is out of range ",
    [FEMRMGMTSIZE - FEBASE]          = "MR Management Record has wrong size ",
    [FEMRMGMTTYPE - FEBASE]          = "MR Management Record is of wrong type ",
};

const char * fru_strerr(fru_errno_t ferr)
{
	if (ferr < FEBASE)
		return strerror(ferr);

	if (ferr >= FETOTALCOUNT) {
		return "Undefined FRU error";
	}

	return fru_errno_string[ferr - FEBASE];
}
