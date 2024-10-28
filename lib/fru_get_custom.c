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

	if (!FRU_IS_VALID_AREA(atype)) {
		fru_errno = FEAREABADTYPE;
		goto out;
	}

	if (!FRU_IS_INFO_AREA(atype)) {
		fru_errno = FEAREANOTSUP;
		goto out;
	}

	if (!fru->present[atype]) {
		fru_errno = FEADISABLED;
		goto out;
	}

	fru__reclist_t ** cust = fru__get_customlist(fru, atype);

	if (!cust) {
		DEBUG("Custom list is not available for area type %d\n", atype);
		goto out;
	}

	fru__reclist_t * entry;
	entry = fru__find_reclist_entry(cust, NULL, index);
	if (entry == NULL) {
		DEBUG("Failed to find reclist entry: %s\n", fru_strerr(fru_errno));
		fru_errno = FENOFIELD;
		goto out;
	}

	field = entry->rec;

out:
	return field;
}
