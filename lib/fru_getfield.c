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

	if (!fru) {
		fru__seterr(FEGENERIC, FERR_LOC_CALLER, -1);
		errno = EFAULT;
		goto out;
	}

	if (!FRU_IS_VALID_AREA(atype)) {
		fru__seterr(FEAREABADTYPE, FERR_LOC_CALLER, atype);
		goto out;
	}

	if (!FRU_IS_INFO_AREA(atype)) {
		fru__seterr(FEAREANOTSUP, FERR_LOC_CALLER, atype);
		goto out;
	}

	if (!fru->present[atype]) {
		fru__seterr(FEADISABLED, atype, -1);
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

	if (index >= fru__fieldcount[atype]) {
		fru__seterr(FENOFIELD, atype, index);
		goto out;
	}

	off_t infoidx = FRU_ATYPE_TO_INFOIDX(atype);
	field = fields[infoidx][index];
out:
	return (fru_field_t *)field;
}
