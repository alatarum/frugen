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
	FRUGEN_FMT_FIRST,
	FRUGEN_FMT_JSON = FRUGEN_FMT_FIRST,
	FRUGEN_FMT_BINARY,
	FRUGEN_FMT_TEXTOUT, /* Output format only */
	FRUGEN_FMT_LAST = FRUGEN_FMT_TEXTOUT
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

#define warn(fmt, args...) do {   \
	typeof(errno) e = errno;      \
	fprintf(stderr, "WARNING: "); \
	errno = e;                    \
	fprintf(stderr, fmt, ##args); \
	fprintf(stderr, "\n");        \
	errno = e;                    \
} while(0)

#define debug(level, fmt, args...) do { \
	typeof(errno) e = errno;            \
	if(level <= debug_level) {          \
		printf("DEBUG: ");              \
		errno = e;                      \
		printf(fmt, ##args);            \
		printf("\n");                   \
		errno = e;                      \
	}                                   \
} while(0)

#define FRU_FIELD_CUSTOM (-1) // Applicable to any area
typedef struct {
	field_type_t type;
	fru_area_type_t area;
	union {
		// The named enums are just aliases for debug convenience only
		fru_chassis_field_t chassis;
		fru_board_field_t board;
		fru_prod_field_t product;
		int index;
	} field;
	char *value;
	int custom_index;
} fieldopt_t;

bool datestr_to_tv(const char *datestr, struct timeval *tv);

/*
 * Split a `--set` command line option argument string
 * into fields.
 *
 * The argument format is expected to be:
 *
 * [<encoding>:]<area>.<field>=<value>
 *
 * Works for string fields only (i.e. not chassis.type or board.date)
 *
 * Examples:
 *   6bitascii:product.pn=ABCDEF123  // Force 6-bit ASCII if possible
 *   text:procuct.name=SOMEPRODUCT   // Force plain text, prevent 6-bit
 *   board.serial=Whatever           // Autodetect encoding
 *   product.custom=Sometext         // Autodetect, request addition
 *   product.custom.1=Sometext       // Autodetect, request replacement of field 1
 *
 * Returns field_opt_t structure.
 * Terminates the program on parsing failure.
 *
 * WARNING: Modifies the input string.
 */
fieldopt_t arg_to_fieldopt(char *arg);

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
