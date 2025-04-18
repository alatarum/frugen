/** @file
 *  @brief FRU generator utility header file
 *
 *  @copyright
 *  Copyright (C) 2016-2024 Alexander Amelkin <alexander@amelkin.msk.ru>
 *
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

struct frugen_config_s {
	frugen_format_t format;
	frugen_format_t outformat;
	fru_flags_t flags;
};

extern volatile int debug_level;
extern const struct area_names_s {
	const char * json;
	const char * human;
} area_names[FRU_TOTAL_AREAS];

void fru_perror(FILE *fp, const char *fmt, ...);

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

#define fru_fatal(fmt, args...) do { \
	fru_perror(stderr, fmt, ##args); \
	exit(1); \
} while(0)

#define fru_warn(fmt, args...) do { \
	fru_perror(stderr,fmt, ##args); \
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
	fru_field_enc_t type;
	fru_area_type_t area;
	union {
#ifdef DEBUG
		// The named enums are just aliases for debug convenience only
		fru_chassis_field_t chassis;
		fru_board_field_t board;
		fru_prod_field_t product;
#endif
		int index;
	} field;
	char *value;
	int custom_index;
} fieldopt_t;

#define DATEBUF_SZ 20 ///< Date string buffer length (must fit "DD/MM/YYYY HH:MM:SS")
/**
 * Convert local date/time string to UTC time in seconds for FRU
 */
bool datestr_to_tv(struct timeval *tv, const char *datestr);

/**
 * Convert FRU time (in UTC) to a local date/time string
 */
void tv_to_datestr(char *datestr, const struct timeval *tv);

/**
 * Split a `--set` command line option argument string
 * into fields.
 *
 * The argument format is expected to be:
 *
 * `[<encoding>:]<area>.<field>=<value>`
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
fru_mr_mgmt_type_t frugen_mr_mgmt_type_by_name(const char *name);

/**
 * Get Multirecord Area Record name by its type
 *
 * Reverse of frugen_mr_mgmt_type_by_name()
 */
const char * frugen_mr_mgmt_name_by_type(fru_mr_mgmt_type_t type);

/**
 * Find a field encoding type by its short name
 *
 * Takes a short name of the encoding type and returns the ID as per Section 13
 *
 * @param[in] name   The short name of the encoding type
 * @returns The encoding type as per Section 13 of IPMI FRU Spec
 * @retval 1..4 The encoding type
 * @retval FIELD_TYPE_UNKNOWN Type name is not recognized
 */
fru_field_enc_t frugen_enc_by_name(const char *name);
/**
 * The opposite of frugen_enc_by_name()
 */
const char * frugen_enc_name_by_val(fru_field_enc_t type);
