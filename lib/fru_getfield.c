/** @file
 *  @brief Implementation of fru_get_field()
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


fru_field_t * fru_getfield(const fru_t * fru,
                           fru_area_type_t atype,
                           size_t index)
{
	const fru_field_t *field = NULL;

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

	const fru_field_t * fields[FRU_INFO_AREAS][FRU_MAX_FIELD_COUNT] = {
		[FRU_INFOIDX(CHASSIS)] = {
			&fru->chassis.pn,
			&fru->chassis.serial,
		},
		[FRU_INFOIDX(BOARD)] = {
			&fru->board.mfg,
			&fru->board.pname,
			&fru->board.serial,
			&fru->board.pn,
			&fru->board.file,
		},
		[FRU_INFOIDX(PRODUCT)] = {
			&fru->product.mfg,
			&fru->product.pname,
			&fru->product.pn,
			&fru->product.ver,
			&fru->product.serial,
			&fru->product.atag,
			&fru->product.file,
		}
	};

	size_t field_count[FRU_INFO_AREAS] = {
		[FRU_INFOIDX(CHASSIS)] = FRU_CHASSIS_FIELD_COUNT,
		[FRU_INFOIDX(BOARD)] = FRU_BOARD_FIELD_COUNT,
		[FRU_INFOIDX(PRODUCT)] = FRU_PROD_FIELD_COUNT,
	};

	off_t infoidx = FRU_ATYPE_TO_INFOIDX(atype);
	if (index >= field_count[infoidx]) {
		fru_errno = FENOFIELD;
		goto out;
	}

	field = fields[infoidx][index];
out:
	return (fru_field_t *)field;
}
