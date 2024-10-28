/** @file
 *  @brief Implementation of common library functions
 *
 *  @copyright
 *  Copyright (C) 2016-2024 Alexander Amelkin <alexander@amelkin.msk.ru>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later OR Apache-2.0
 */

//#define DEBUG
#include "fru-private.h"

/** @cond PRIVATE */

int fru__calc_checksum(void *buf, size_t size)
{
	if (!buf || size == 0) {
		DEBUG("Null pointer or zero buffer length\n");
		fru_errno = FEGENERIC;
		errno = EFAULT;
		return -1;
	}

	uint8_t *data = (uint8_t *)buf;
	uint8_t checksum = 0;

	for(size_t i = 0; i < size; i++) {
		checksum += data[i];
	}

	return (int)(uint8_t)(-(int8_t)checksum);
}

int fru__area_checksum(fru__file_area_t *area)
{
	return calc_checksum(area, FRU_BYTES(area->blocks));
}

/**
 * A generic single-linked list abstraction.
 * This is used as a substitute for all other list types in the library.
 */
struct genlist_s {
	void *data; /* A pointer to the actual data or NULL if not initialized */
	void *next; /* The next record in the list or NULL if last */
}

void * find_reclist_entry(void *head_ptr, void *prev, int index)
{
	struct genlist_s *rec, **prev_rec = prev, **reclist = head_ptr;
	size_t counter = 0;

	if (!head_ptr) return NULL;

	rec = *reclist;
	*prev_rec = NULL;
	while (rec && counter < index) {
		*prev_rec = rec;
		rec = rec->next;
		counter++;
	}

	return rec;
}

// See header
void * fru__add_reclist_entry(void *head_ptr, size_t index)
{
	struct genlist_s *rec,
	                 *oldrec,
	                 *prevrec,
	                 **reclist = head_ptr,
	                 *reclist_ptr = *reclist;

	if (!head_ptr) {
		fru_errno = FEGENERIC;
		errno = EFAULT;
		return NULL;
	}

	rec = calloc(1, sizeof(struct genlist_s));
	if(!rec) {
		fru_errno = FEGENERIC;
		return NULL;
	}

	// If the reclist is empty, update it
	if(!(*reclist)) {
		*reclist = rec;
	} else {
		// If the reclist is not empty, find the last entry
		// and append the new one as next, or find the entry
		// at the given index and insert the new one before it.
		oldrec = find_reclist_entry(head_ptr, &prevrec, index);
		if (prevrec) {
			// Update the previous entry (or the last one)
			prevrec->next = rec;
		}
		if (olrec) {
			// Don't lose the old entry that was at this position
			rec->next = oldrec;
		}
	}

	return rec;
}

/** @endcond */
