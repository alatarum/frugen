/** @file
 *  @brief Implementation of fru_get_custom()
 *
 *  @copyright
 *  Copyright (C) 2016-2024 Alexander Amelkin <alexander@amelkin.msk.ru>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later OR Apache-2.0
 */
#include <stddef.h>
#include <errno.h>

#include "fru-private.h"
#include "../fru_errno.h"


fru_field_t * fru_get_custom(const fru_t * fru,
                             fru_area_type_t atype,
                             size_t index)
{
	fru_field_t *field = NULL;

	if (!fru) {
		fru__seterr(FEGENERIC, FERR_LOC_CALLER, -1);
		errno = EFAULT;
		goto out;
	}

	if (!FRU_IS_VALID_AREA(atype)) {
		fru__seterr(FEAREABADTYPE, FERR_LOC_CALLER, atype);
		goto out;
	}

	if (!FRU_IS_INFO_AREA(atype)) {
		fru__seterr(FEAREANOTSUP, FERR_LOC_CALLER, atype);
		goto out;
	}

	if (!fru->present[atype]) {
		fru__seterr(FEADISABLED, atype, -1);
		goto out;
	}

	fru__reclist_t ** cust = fru__get_customlist(fru, atype);

	fru__reclist_t * entry;
	entry = fru__find_reclist_entry(cust, NULL, index);
	if (entry == NULL) {
		DEBUG("Failed to find reclist entry: %s\n", fru_strerr(fru_errno));
		// Custom fields start at FRU_<atype>_FIELD_COUNT index
		fru__seterr(FENOFIELD, atype, fru__fieldcount[atype] + index);
		goto out;
	}

	field = entry->rec;

out:
	return field;
}
