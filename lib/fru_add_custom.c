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

	if (!fru) {
		fru__seterr(FEGENERIC, FERR_LOC_GENERAL, -1);
		errno = EFAULT;
		goto out;
	}

	if (!FRU_IS_VALID_AREA(atype)) {
		fru__seterr(FEAREABADTYPE, FERR_LOC_GENERAL, atype);
		goto out;
	}

	if (!FRU_IS_INFO_AREA(atype)) {
		fru__seterr(FEAREANOTSUP, FERR_LOC_GENERAL, atype);
		goto out;
	}

	fru__reclist_t ** cust = fru__get_customlist(fru, atype);

	/* Before allocating any list entries check if the supplied
	 * string is encodable with the requested encoding */
	if (encoding != FRU_FE_EMPTY && !fru_setfield(&field, encoding, string)) {
		fru_errno.src = (fru_error_source_t)atype;
		fru_errno.index = fru__fieldcount[atype] + index;
		goto out;
	}

	fru__reclist_t * custom_list = *cust;
	fru__reclist_t * custom_entry = fru__add_reclist_entry(&custom_list, index);
	if (!custom_entry) {
		fru__seterr(FEGENERIC, atype, fru__fieldcount[atype] + index);
		DEBUG("Failed to allocate reclist entry: %s\n", fru_strerr(fru_errno));
		goto out;
	}

	custom_entry->rec = calloc(1, sizeof(fru_field_t));
	if (!custom_entry->rec) {
		DEBUG("Failed to allocate custom record: %s\n", fru_strerr(fru_errno));
		fru__seterr(FEGENERIC, atype, fru__fieldcount[atype] + index);
		goto out;
	}

	/* Now as everything seeems ok, copy the field into the list entry */
	memcpy(custom_entry->rec, &field, sizeof(fru_field_t));
	ret = custom_entry->rec;
	*cust = custom_list;

	// Ignore the error: the area is anyway enabled, either now or before
	fru_enable_area(fru, atype, FRU_APOS_AUTO);

out:
	return ret;
}
