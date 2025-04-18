/** @file
 *  @brief FRU error codes implementation
 *
 *  Copyright (C) 2016-2021 Alexander Amelkin <alexander@amelkin.msk.ru>
 *  SPDX-License-Identifier: LGPL-2.0-or-later OR Apache-2.0
 */
#include <string.h>
#include "../fru_errno.h"

__thread fru_errno_t fru_errno = { FENONE, FERR_LOC_GENERAL, -1 };

static const char * const fru_errno_string[FETOTALCOUNT] = {
    [FENONE]                = "No libfru error",
    [FEGENERIC]             = "Generic error, check errno",
    [FEINIT]                = "Uninitialized FRU structure",
    [FENONPRINT]            = "Field data contains non-printable bytes",
    [FENONHEX]              = "Input string contains non-hex characters",
    [FERANGE]               = "Field data exceeds range for the requested encoding",
    [FENOTEVEN]             = "Not an even number of nibbles",
    [FEAUTOENC]             = "Unable to auto-detect encoding",
    [FEBADENC]              = "Invalid encoding for a field",
    [FE2SMALL]              = "File or buffer is too small",
    [FE2BIG]                = "Data, file, or buffer is too big",
    [FESIZE]                = "Data size mismatch",
    [FEHDRVER]              = "Bad header version",
    [FEHDRCKSUM]            = "Bad header checksum",
    [FEHDRBADPTR]           = "Area pointer beyond the end of file/buffer",
    [FEDATACKSUM]           = "Bad data checksum",
    [FEAREADUP]             = "Duplicate area in area order",
    [FEAREANOTSUP]          = "Unsupported area type", /* For a particular operation */
    [FEAREABADTYPE]         = "Bad area type", /* A completely wrong value */
    [FENOTERM]              = "Unterminated area",
    [FEBDATE]               = "Board manufacturing date is out of range",
    [FENOFIELD]             = "No such field",
    [FENOREC]               = "No such record",
    [FEBADDATA]             = "Malformed data",
    [FENODATA]              = "No data",
    [FEMRMGMTBAD]           = "Bad management record subtype",
    [FEMRNOTSUP]            = "Unsupported record type",
    [FEMREND]               = "End of MR records (not an error)",
    [FEAPOS]                = "Invalid area position",
    [FENOTEMPTY]            = "List is not empty",
    [FEAENABLED]            = "Area is enabled",
    [FEADISABLED]           = "Areas is disabled",
    [FELIB]                 = "Internal library error (bug?)",
};

const char * fru_strerr(fru_errno_t ferr)
{
	if (ferr.code < FENONE || ferr.code >= FETOTALCOUNT) {
		return "Undefined libfru error";
	}

	if (ferr.code == FEGENERIC)
		return strerror(ferr.code);

	return fru_errno_string[ferr.code];
}

void fru_clearerr(void)
{
	fru_errno.code = FENONE;
	fru_errno.src = FERR_LOC_GENERAL;
	fru_errno.index = -1;
}
