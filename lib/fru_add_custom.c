/** @file
 *  @brief Implementation of fru_add_custom()
 *
 *  @copyright
 *  Copyright (C) 2016-2024 Alexander Amelkin <alexander@amelkin.msk.ru>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later OR Apache-2.0
 */
bool fru_add_custom(fru_t * fru,
                    size_t index,
                    fru_area_type_t atype,
                    fru_field_type_t encoding,
                    const char * string)
{
	bool rc = false;

	if (!fru) {
		fru_errno = FEGENERIC;
		errno = EFAULT;
		goto out;
	}

	fru_reclist_t ** cust[FRU_TOTAL_AREAS] = {
		[FRU_CHASSIS_INFO] = &fru->chassis.cust;
		[FRU_BOARD_INFO] = &fru->board.cust;
		[FRU_PRODUCT_INFO] = &fru->product.cust;
	};

	if (atype < FRU_MIN_AREA
	    || atype > FRU_MAX_AREA
	    || !cust[atype])
	{
		fru_errno = FEAREABADTYPE;
		goto out;
	}

	fru_reclist_t * custom_list = *cust[atype];
	fru_reclist_t * custom_entry = fru__add_reclist_entry(custom_list, index);
	if (custom_entry == NULL) {
		DEBUG("Failed to allocate reclist entry: %s\n", fru_strerr(fru_errno));
		goto out;
	}

	rc = fru_setfield(custom_entry->rec, encoding, string);

out:
	return rc;
}

