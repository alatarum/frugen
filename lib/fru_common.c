/** @file
 *  @brief Implementation of common library functions
 *
 *  This code will always be linked because one either uses load_* functions
 *  or save_* functions, or both. Both those groups of functions use what
 *  is defined here. That's why it is called common.
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
#include <time.h>

//#define DEBUG
#include "fru-private.h"
#include "../fru_errno.h"

/** @cond PRIVATE */

/*
 * Minimum and maximum lengths of values as per
 * Table 18-6, Management Access Record
 */
const size_t fru__mr_mgmt_minlen[FRU_MR_MGMT_INDEX_COUNT] = {
	[FRU__MGMT_TYPENAME_ID(SYS_URL)] = 16,
	[FRU__MGMT_TYPENAME_ID(SYS_NAME)] = 8,
	[FRU__MGMT_TYPENAME_ID(SYS_PING)] = 8,
	[FRU__MGMT_TYPENAME_ID(COMPONENT_URL)] = 16,
	[FRU__MGMT_TYPENAME_ID(COMPONENT_NAME)] = 8,
	[FRU__MGMT_TYPENAME_ID(COMPONENT_PING)] = 8,
	[FRU__MGMT_TYPENAME_ID(SYS_UUID)] = 16
};

const size_t fru__mr_mgmt_maxlen[FRU_MR_MGMT_INDEX_COUNT] = {
	[FRU__MGMT_TYPENAME_ID(SYS_URL)] = 256,
	[FRU__MGMT_TYPENAME_ID(SYS_NAME)] = 64,
	[FRU__MGMT_TYPENAME_ID(SYS_PING)] = 64,
	[FRU__MGMT_TYPENAME_ID(COMPONENT_URL)] = 256,
	[FRU__MGMT_TYPENAME_ID(COMPONENT_NAME)] = 256,
	[FRU__MGMT_TYPENAME_ID(COMPONENT_PING)] = 64,
	[FRU__MGMT_TYPENAME_ID(SYS_UUID)] = 16
};

int fru__calc_checksum(const void * buf, size_t size)
{
	if (!buf || size == 0) {
		DEBUG("Null pointer or zero buffer length\n");
		fru_errno = FEGENERIC;
		errno = EFAULT;
		return -1;
	}

	uint8_t * data = (uint8_t *)buf;
	uint8_t checksum = 0;

	for(size_t i = 0; i < size; i++) {
		checksum += data[i];
	}

	// Negated checksum is returned as an unsigned LSB of a signed bigger integer
	return (int)(uint8_t)(-(int8_t)checksum);
}

fru__reclist_t ** fru__get_customlist(const fru_t * fru, fru_area_type_t atype)
{
	int err;
	fru__reclist_t ** ret = NULL;

	fru_errno = FENONE;

	if (!fru) {
		fru_errno = FEGENERIC;
		err = EFAULT;
		goto out;
	}

	if (atype < FRU_MIN_AREA || atype > FRU_MAX_AREA)
	{
		fru_errno = FEAREABADTYPE;
		goto out;
	}

	fru__reclist_t ** cust[FRU_TOTAL_AREAS] = {
		[FRU_CHASSIS_INFO] = (fru__reclist_t **)&fru->chassis.cust,
		[FRU_BOARD_INFO] = (fru__reclist_t **)&fru->board.cust,
		[FRU_PRODUCT_INFO] = (fru__reclist_t **)&fru->product.cust,
	};

	ret = cust[atype];

out:
	if (fru_errno == FEGENERIC)
		errno = err;

	return ret;
}

void * fru__find_reclist_entry(void * head_ptr, void * prev, size_t index)
{
	fru__genlist_t * rec,
	               ** prev_rec = NULL,
	               ** reclist = (fru__genlist_t **)head_ptr;
	size_t counter = 0;

	if (!head_ptr) {
		fru_errno = FEGENERIC;
		errno = EFAULT;
		return NULL;
	}

	rec = *reclist;
	if (prev) {
		prev_rec = (fru__genlist_t **)prev,
		*prev_rec = NULL;
	}
	while (rec && counter < index) {
		if (prev_rec)
			*prev_rec = rec;
		rec = rec->next;
		counter++;
	}

	if (!rec) {
		fru_errno = FENOENTRY;
	}
	return rec;
}

// See header
void * fru__add_reclist_entry(void * head_ptr, size_t index)
{
	fru__genlist_t * rec,
	               * oldrec,
	               * prevrec = NULL,
	               **reclist = (fru__genlist_t **)head_ptr;

	if (!head_ptr) {
		fru_errno = FEGENERIC;
		errno = EFAULT;
		return NULL;
	}

	rec = (fru__genlist_t *)calloc(1, sizeof(fru__genlist_t));
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
		oldrec = fru__find_reclist_entry(head_ptr, &prevrec, index);
		if (prevrec) {
			// Update the previous entry (or the last one)
			prevrec->next = rec;
		}
		if (oldrec) {
			// Don't lose the old entry that was at this position
			rec->next = oldrec;
		}
	}

	return rec;
}

/*
 * Free all the record list entries starting with the
 * one pointed to by listptr and up to the end of the list.
 *
 * Takes a pointer to any fru__genlist_t compatible list.
 * That is either fru__reclist_t ** or fru__mr_reclist_t **.
 */
bool fru__free_reclist(void * listptr)
{
	fru__genlist_t ** genlist = listptr;
	fru__genlist_t * entry;

	if (!listptr)
		return false;

	entry = *genlist;

	while (entry) {
		fru__genlist_t * next = entry->next;
		zfree(entry->data);
		zfree(entry);
		entry = next;
	}

	return true;
}

// See fru-private.h
time_t fru__datetime_base(void) {
	struct tm tm_1996 = {
		.tm_year = 96,
		.tm_mon = 0,
		.tm_mday = 1
	};
	// The argument to mktime is zoneless
	return mktime(&tm_1996);
}

/** @endcond */

// See fru.h
void fru_wipe(fru_t * fru)
{
	if (!fru) return;

	zfree(fru->internal);
	fru__free_reclist(&fru->chassis.cust);
	fru__free_reclist(&fru->board.cust);
	fru__free_reclist(&fru->product.cust);
	fru__free_reclist(&fru->mr);
	memset(fru, 0, sizeof(fru_t));
}

/** See fru.h */
#define FRU__NIBBLES_IN_BYTE 2
#define FRU__NIBBLE_SIZE 4
int16_t fru_hex2byte(const char * hex)
{
	if (!hex) {
		fru_errno = FEGENERIC;
		errno = EFAULT;
		return -1;
	}

	/* We could use sscanf, but that's about 6 times slower for single-byte conversions */
	uint8_t byte = 0;
	for (size_t i = 0; i < FRU__NIBBLES_IN_BYTE; i++) { // 2 nibbles in a byte
		char c = hex[i];
		int nibble = -1;

		if (c >= 'A' && c <= 'F') {
			c = c - 'A' + 'a';
		}

		if (c >= '0' && c <= '9') {
			nibble = c - '0';
		}
		else if (c >= 'a' && c <= 'f') {
				nibble = c - 'a' + 10;
		}
		else {
				fru_errno = FENONHEX;
				return -1;
		}

		if (nibble > 0) {
			// First char goes to high nibble.
			byte |= nibble << (FRU__NIBBLE_SIZE - FRU__NIBBLE_SIZE * i);
		}
	}
	return byte;
}

