/** @file
 *  @brief Implementation of fru_delete_custom()
 *
 *  @copyright
 *  Copyright (C) 2016-2024 Alexander Amelkin <alexander@amelkin.msk.ru>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later OR Apache-2.0
 */
#include <assert.h>
#include <errno.h>
#include <stddef.h>

#include "fru-private.h"
#include "../fru_errno.h"

/**
 * Delete an \a n'th record in a list.
 *
 * Could work both with fru__reclist_t and fru__mr_reclist_t,
 * but is only used for the former as for the latter deletion
 * is handled by mr_operation() in fru_mr_ops.c along with
 * search by MR type, which is impossible here.
 *
 * @param[in] reclist A pointer to any record list
 * @param[in] index   The index of the record to find, 1-based
 * @returns A success status
 */
static
bool delete_reclist_entry(void * head_ptr, int index)
{
	assert(head_ptr);

	fru__genlist_t * rec,
	               * prev_rec,
	               ** first_rec = (fru__genlist_t **)head_ptr;

	rec = fru__find_reclist_entry(head_ptr, (void *)&prev_rec, index);
	if (!rec)
		return false;

	if (rec == *first_rec) { // Deleting the head
		*first_rec = rec->next;
	}
	else if(rec) { // Deleting any non-head entry
		prev_rec->next = rec->next;
	}

	// `rec` is always the record we delete, free it
	rec->next = NULL;
	fru__free_reclist(&rec);
	return true;
}

bool fru_delete_custom(fru_t * fru,
                       fru_area_type_t atype,
                       size_t index)
{
	bool rc = false;

	if (!fru) {
		fru__seterr(FEGENERIC, FERR_LOC_CALLER, -1);
		errno = EFAULT;
		goto out;
	}

	if (atype < FRU_MIN_AREA || atype > FRU_MAX_AREA) {
		fru__seterr(FEAREABADTYPE, FERR_LOC_CALLER, atype);
		goto out;
	}

	if (!FRU_IS_INFO_AREA(atype)) {
		fru__seterr(FEAREANOTSUP, FERR_LOC_CALLER, atype);
		goto out;
	}

	fru__reclist_t ** cust = fru__get_customlist(fru, atype);

	if (!delete_reclist_entry(cust, index)) {
		DEBUG("Failed to delete reclist entry: %s\n", fru_strerr(ru_errno));
		// Custom fields start at FRU_<atype>_FIELD_COUNT index
		fru__seterr(FENOFIELD, atype, fru__fieldcount[atype] + index);
		goto out;
	}

	rc = true;
out:
	return rc;
}
