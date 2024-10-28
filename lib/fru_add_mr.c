/** @file
 *  @brief Implementation of fru_mr_add()
 *
 *  @copyright
 *  Copyright (C) 2016-2024 Alexander Amelkin <alexander@amelkin.msk.ru>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later OR Apache-2.0
 */
#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "fru-private.h"
#include "../fru_errno.h"


// See fru.h
fru_mr_rec_t * fru_add_mr(fru_t * fru, size_t index, fru_mr_rec_t * rec)
{
	if (!fru) {
		fru_errno = FEGENERIC;
		errno = EFAULT;
		return NULL;
	}

	fru__mr_reclist_t *mr_reclist_tail = NULL;
	fru__mr_reclist_t ** mr_reclist_head = (fru__mr_reclist_t **)&fru->mr;

	mr_reclist_tail = fru__add_reclist_entry(mr_reclist_head, index);
	if (!mr_reclist_tail) {
		return NULL;
	}

	/*
	 * In order for fru__free_reclist() to work properly later, we must
	 * ensure that the new record in reclist is dynamically allocated.
	 * That's why we don't take the user-supplied pointer, but instead
	 * allocate a new record and copy the data.
	 */
	fru_mr_rec_t *newrec = calloc(1, sizeof(fru_mr_rec_t));
	if (!newrec) {
		fru_errno = FEGENERIC;
		errno = EFAULT;
	}
	newrec->type = FRU_MR_EMPTY;

	if (rec) {
		memcpy(newrec, rec, sizeof(fru_mr_rec_t));
	}

	mr_reclist_tail->rec = newrec;
	fru->present[FRU_MR] = true;
	return newrec;
}
