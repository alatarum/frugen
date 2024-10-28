/** @file
 *  @brief Implementation of fru_add_custom()
 *
 *  @copyright
 *  Copyright (C) 2016-2024 Alexander Amelkin <alexander@amelkin.msk.ru>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later OR Apache-2.0
 */
#include "fru-private.h"
#include "../fru_errno.h"
#include <errno.h>
#include <stddef.h>
#include <string.h>

fru_field_t * fru_add_custom(fru_t * fru,
                             fru_area_type_t atype,
                             size_t index,
                             fru_field_enc_t encoding,
                             const char * string)
{
	fru_field_t * ret = NULL;
	fru_field_t field = {};

	fru__reclist_t ** cust = fru__get_customlist(fru, atype);

	if (!cust) {
		DEBUG("Custom list is not available for area type %d\n", atype);
		goto out;
	}

	/* Before allocating any list entries check if the supplied
	 * string is encodable with the requested encoding */
	if (encoding != FRU_FE_EMPTY && !fru_setfield(&field, encoding, string)) {
		goto out;
	}

	fru__reclist_t * custom_list = *cust;
	fru__reclist_t * custom_entry = fru__add_reclist_entry(&custom_list, index);
	if (!custom_entry) {
		DEBUG("Failed to allocate reclist entry: %s\n", fru_strerr(fru_errno));
		goto out;
	}

	custom_entry->rec = calloc(1, sizeof(fru_field_t));
	if (!custom_entry->rec) {
		DEBUG("Failed to allocate custom record: %s\n", fru_strerr(fru_errno));
		fru_errno = FEGENERIC;
		goto out;
	}

	/* Now as everything seeems ok, copy the field into the list entry */
	memcpy(custom_entry->rec, &field, sizeof(fru_field_t));
	ret = custom_entry->rec;
	*cust = custom_list;
	fru->present[atype] = true;

out:
	return ret;
}
