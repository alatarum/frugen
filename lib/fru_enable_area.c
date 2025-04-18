/** @file
 *  @brief Implementation of FRU area enabler/disabler
 *
 *  @copyright
 *  Copyright (C) 2016-2025 Alexander Amelkin <alexander@amelkin.msk.ru>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later OR Apache-2.0
 */

#include <errno.h>

#include "fru-private.h"
#include "../fru_errno.h"

bool fru_enable_area(fru_t * fru, fru_area_type_t atype, fru_area_position_t after)
{
	if (!fru) {
		fru__seterr(FEGENERIC, FERR_LOC_CALLER, -1);
		errno = EFAULT;
		return false;
	}

	if (!FRU_IS_VALID_AREA(atype)) {
		fru__seterr(FEAREABADTYPE, FERR_LOC_CALLER, atype);
		return false;
	}

	if (!FRU_IS_APOS_VALID(after)) {
		fru__seterr(FEAPOS, FERR_LOC_CALLER, -1);
		return false;
	}

	if (fru->present[atype]) {
		/*
		 * The function is expected to set up the requested order
		 * of areas. If already enabled, that can't be done. So fail.
		 */
		fru__seterr(FEAENABLED, atype, -1);
		return false;
	}

	fru_area_type_t after_atype = (fru_area_type_t)after;
	fru_area_position_t old_pos = FRU_APOS_AUTO;
	fru_area_position_t new_pos;
	fru_area_position_t i;

	/* If the area wasn't present, mark it present and put it in
	 * in the order array as if it was there initially in the natural
	 * order of areas given in the FRU specification. */

	/* First find the areas old position in the order array */
	for (i = FRU_APOS_MIN; i <= FRU_APOS_MAX; i++) {
		if (fru->order[i] == atype) {
			old_pos = i;
			break;
		}
	}

	if (FRU_APOS_AUTO == old_pos) {
		/*
		 * This can only happen if fru_t structure wasn't initialized with fru_init(),
		 * or if it was later manually tampered with. No library API can result in this.
		 */
		fru__seterr(FEINIT, FERR_LOC_GENERAL, -1);
		return false;
	}

	/* The order array has non-present areas at the beginning,
	 * so we start searching for a place to insert the new area
	 * from the end, and then will shift everything between the
	 * old and new positions one slot to the left in the array.
	 * 
	 * That's unless we are asked for an exact position.
	 */
	if (after == FRU_APOS_LAST) {
		new_pos = FRU_APOS_MAX;
	}
	else {
		new_pos = FRU_APOS_MAX;
		fru_area_position_t auto_new_pos = FRU_APOS_AUTO;
		fru_area_type_t last_pos_atype = FRU_MIN_AREA;
		for (i = FRU_APOS_MAX; i >= FRU_APOS_MIN; i--) {
			fru_area_type_t pos_atype = fru->order[i];

			/*
			 * If they've asked for the first position, then
			 * the very first unoccupied slot is what we want
			 */
			if (after == FRU_APOS_FIRST && !fru->present[pos_atype])
				break;

			/*
			 * We only respect the value of `after` if that area is
			 * actually present.
			 */
			if (fru->present[pos_atype] && pos_atype == after_atype)
				break;

			if (!fru->present[pos_atype] || pos_atype <= atype) {
				/*
				 * Remember the position of the very first non-present
				 * area or adjust the auto position through present
				 * areas as they are found
				 */
				bool present = fru->present[pos_atype];
				if ((present && last_pos_atype < pos_atype)
					|| (!present && auto_new_pos == FRU_APOS_AUTO))
				{
					auto_new_pos = new_pos;
					last_pos_atype = pos_atype;
				}
			}

			new_pos = i - 1;
		}
		/*
		 * The requested `after` area is not present (isn't enabled),
		 * behave as if auto positioning was requested
		 */
		if (after == FRU_APOS_AUTO && auto_new_pos != FRU_APOS_AUTO)
			new_pos = auto_new_pos;
	}


	for (i = old_pos; i < new_pos; i++) {
		fru->order[i] = fru->order[i + 1];
	}

	fru->order[new_pos] = atype;
	fru->present[atype] = true;

	return true;
}

bool fru_disable_area(fru_t * fru, fru_area_type_t atype)
{
	fru_area_position_t old_pos = FRU_APOS_AUTO;
	fru_area_position_t new_pos;
	fru_area_position_t i;

	if (!fru) {
		fru__seterr(FEGENERIC, FERR_LOC_CALLER, -1);
		errno = EFAULT;
		return false;
	}

	if (!FRU_IS_VALID_AREA(atype)) {
		fru__seterr(FEAREABADTYPE, FERR_LOC_CALLER, atype);
		return false;
	}

	if (!fru->present[atype]) {
		/*
		 * No extra actions are expected, so report success but still
		 * register the error condition
		 */
		fru__seterr(FEAENABLED, atype, -1);
		return true;
	}

	/*
	 * If the area was present, mark it non-present and put it at the
	 * beginning of the `order` array.
	 */

	/* First find the area's old position in the order array */
	for (i = FRU_APOS_MIN; i <= FRU_APOS_MAX; i++) {
		if (fru->order[i] == atype) {
			old_pos = i;
			break;
		}
	}

	if (FRU_APOS_AUTO == old_pos) {
		/*
		 * This can only happen if fru_t structure wasn't initialized with fru_init(),
		 * or if it was later manually tampered with. No library API can result in this.
		 */
		fru__seterr(FEINIT, FERR_LOC_CALLER, -1);
		return false;
	}

	/*
	 * The order array has non-present areas at the beginning,
	 * so we start searching for a place to insert the new area
	 * from the end
	 */
	new_pos = FRU_APOS_MIN;
	for (size_t i = FRU_APOS_MIN; i <= FRU_APOS_MAX; i++) {
		fru_area_type_t pos_atype = fru->order[i];

		if (fru->present[pos_atype] || pos_atype > atype)
			break;

		new_pos = i + 1;
	}

	/*
	 * Now shift everything between the new position (inclusive)
	 * and the old one one slot to the right in the order array
	 */
	for (i = old_pos; i > new_pos; i--) {
		fru->order[i] = fru->order[i - 1];
	}

	fru->order[new_pos] = atype;
	fru->present[atype] = false;

	return true;
}

bool fru_move_area(fru_t * fru, fru_area_type_t area, fru_area_position_t after)
{
	fru_clearerr();
	if (!fru_disable_area(fru, area) || fru_errno.code)
		return false;

	return fru_enable_area(fru, area, after);
}
