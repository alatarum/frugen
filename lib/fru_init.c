/** @file
 *  @brief Implementation of fru_init()
 *
 *  @copyright
 *  Copyright (C) 2016-2024 Alexander Amelkin <alexander@amelkin.msk.ru>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later OR Apache-2.0
 */

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "fru-private.h"
#include "../fru_errno.h"

#define FRU__DEFAULT_CHASSIS_TYPE 0x17 // Rack-mount, see SMBIOS specification

fru_t * fru_init(fru_t * fru)
{
	if (!fru) {
		fru = malloc(sizeof(fru_t));
	}

	if (!fru) {
		fru_errno = FEGENERIC;
		return NULL;
	}

	/* Set all strings and lists empty, all areas non-present */
	memset(fru, 0, sizeof(fru_t));

	/* Set board manufacturing date to be automatically set on save */
	fru->board.tv_auto = true;

	/* Set chassis type */
	fru->chassis.type = FRU__DEFAULT_CHASSIS_TYPE;

	/* Set default area order */
	fru_area_type_t atype;
	FRU_FOREACH_AREA(atype) {
		fru->order[atype] = atype;
	}

	return fru;
}
