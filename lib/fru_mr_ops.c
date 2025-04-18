/** @file
 *  @brief Implementation of fru_get_mr() function
 *
 *  @copyright
 *  Copyright (C) 2016-2024 Alexander Amelkin <alexander@amelkin.msk.ru>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later OR Apache-2.0
 */

#include <errno.h>
#include <stddef.h>
#include <string.h>

//#define DEBUG
#include "fru-private.h"
#include "../fru_errno.h"

typedef enum {
	MR_OP_FIND,
	MR_OP_REPLACE
} fru__mr_op_t;

// A helper function that does all the job for all
// the public interfaces in this file.
static
fru_mr_rec_t * mr_operation(fru_t * fru,
                            fru__mr_op_t op,
                            fru_mr_rec_t * rec,
                            fru_mr_type_t type,
                            size_t * index)
{
	if (!fru) {
		fru__seterr(FEGENERIC, FERR_LOC_CALLER, -1);
		errno = EFAULT;
		return NULL;
	}

	if (!fru->present[FRU_MR]) {
		fru__seterr(FEADISABLED, FERR_LOC_CALLER, -1);
		return NULL;
	}

	// This is wider than FRU_MR_IS_VALID_TYPE()
	if (!FRU_MR_IS_VALID_TYPE(type)
	    && FRU_MR_RAW != type
	    && FRU_MR_ANY != type)
	{
		fru__seterr(FEMRNOTSUP, FERR_LOC_CALLER, *index);
		return NULL;
	}

	fru__mr_reclist_t * entry = fru->mr;
	fru__mr_reclist_t * prev_entry = NULL;
	size_t count = 0;
	size_t start_index = index ? *index : 0;

	while (entry) {
		/* If 'ANY' type is requested, then we search for the record at the given index */
		if ((type == FRU_MR_ANY && count == *index)
			|| (count >= start_index && type == entry->rec->type))
		{
			/* The found entry is last, indicate that to the caller */
			if (!entry->next)
				fru__seterr(FEMREND, FERR_LOC_MR, count);

			/* We've found the record, now see what to do with it */
			switch (op) {
			default:
			case MR_OP_FIND:
				*index = count;
				return entry->rec;
			case MR_OP_REPLACE:
				if (rec) {
					memcpy(entry->rec, rec, sizeof(fru_mr_rec_t));
					return entry->rec;
				}
				else {
					/* Empty rec means they want to delete the record */
					fru__mr_reclist_t ** mr_head = (fru__mr_reclist_t **)&fru->mr;
					fru__mr_reclist_t ** prevptr = mr_head;
					zfree(entry->rec);

					if (prev_entry)
						prevptr = &prev_entry->next;

					(*prevptr) = entry->next;

					zfree(entry);

					/* If there are no more entries in MR list, this means the area
					 * is emtpy, mark it as not present */
					if (! *mr_head)
						fru->present[FRU_MR] = false;

					/* On delete the caller doesn't actually care about the
					 * returned pointer, it only checks for NULL or non-NULL */
					return (fru_mr_rec_t *)true;
				}
				break;
			}
		}

		prev_entry = entry;
		entry = entry->next;
		count++;
	}

	fru__seterr(FENOREC, FERR_LOC_MR, *index);
	return NULL;
}

// See fru.h
bool fru_delete_mr(fru_t * fru, size_t index)
{
	return (NULL != mr_operation(fru, MR_OP_REPLACE, NULL, FRU_MR_ANY, &index));
}

// See fru.h
bool fru_replace_mr(fru_t * fru,
                    size_t index,
                    fru_mr_rec_t * rec)
{
	return mr_operation(fru, MR_OP_REPLACE, rec, FRU_MR_ANY, &index);
}

// See fru.h
fru_mr_rec_t * fru_find_mr(const fru_t * fru,
                           fru_mr_type_t type,
                           size_t * index)
{
	return mr_operation((fru_t *)fru, MR_OP_FIND, NULL, type, index);
}

// See fru.h
fru_mr_rec_t * fru_get_mr(const fru_t * fru, size_t index)
{
	return fru_find_mr(fru, FRU_MR_ANY, &index);
}

