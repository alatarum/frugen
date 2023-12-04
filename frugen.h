/** @file
 *  @brief FRU generator utility header file
 *
 *  Copyright (C) 2016-2023 Alexander Amelkin <alexander@amelkin.msk.ru>
 *  SPDX-License-Identifier: GPL-2.0-or-later OR Apache-2.0
 */
#pragma once

#include <errno.h>
#include <stdio.h>
#include <unistd.h>

#include "fru.h"

typedef enum {
	FRUGEN_FMT_UNSET,
	FRUGEN_FMT_JSON,
	FRUGEN_FMT_BINARY,
	FRUGEN_FMT_TEXTOUT, /* Output format only */
} frugen_format_t;

struct frugen_fruinfo_s {
	fru_exploded_t fru;
	fru_area_t areas[FRU_MAX_AREAS];
	bool has_chassis;
	bool has_board;
	bool has_bdate;
	bool has_product;
	bool has_internal;
	bool has_multirec;
};

struct frugen_config_s {
	frugen_format_t format;
	frugen_format_t outformat;
	fru_flags_t flags;
	bool no_curr_date; // Don't use current timestamp if no 'date' is specified
};

extern volatile int debug_level;

#define fatal(fmt, args...) do {  \
	fprintf(stderr, fmt, ##args); \
	fprintf(stderr, "\n");        \
	exit(1);                      \
} while(0)

#define debug(level, fmt, args...) do { \
	int e = errno;                      \
	if(level <= debug_level) {          \
		printf("DEBUG: ");              \
		errno = e;                      \
		printf(fmt, ##args);            \
		printf("\n");                   \
		errno = e;                      \
	}                                   \
} while(0)

static inline
int typelen2ind(uint8_t field) {
	if (FIELD_TYPE_T(field) < TOTAL_FIELD_TYPES)
		return FIELD_TYPE_T(field);
	else
		return FIELD_TYPE_AUTO;
}

fru_field_t * fru_encode_custom_binary_field(const char *hexstr);
bool datestr_to_tv(const char *datestr, struct timeval *tv);

/**
 * Find a Management Access record subtype by its short name
 *
 * Takes a short name of the subtype from the following list
 * and returs the ID as per Table 18-6.
 *
 * Terminates the program on failure.
 *
 * @param[in] name   The short name of the type:
 *                   surl = System URL
 *                   sname = System Name
 *                   spingaddr = System ping address
 *                   curl = Component URL
 *                   cname = Component name
 *                   cpingaddr = Component ping address
 *                   uuid = System UUID
 * @returns The ID of the subtype as per Table 18-6 of IPMI FRU Spec
 * @retval 1..7 The subtype ID
 */
fru_mr_mgmt_type_t fru_mr_mgmt_type_by_name(const char *name);

/**
 * Get Multirecord Area Record name by its type
 *
 * Reverse of fru_mr_mgmt_type_by_name()
 */
const char * fru_mr_mgmt_name_by_type(fru_mr_mgmt_type_t type);
