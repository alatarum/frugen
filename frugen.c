/** @file
 *  @brief FRU generator utility
 * 
 *  @copyright
 *  Copyright (C) 2016-2024 Alexander Amelkin <alexander@amelkin.msk.ru>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later OR Apache-2.0
 */
#ifndef VERSION
// If VERSION is not defined, that means something is wrong with CMake
#define VERSION "BROKEN"
#endif

#define COPYRIGHT_YEARS "2016-2025"

#define _GNU_SOURCE
#include <getopt.h>

#include <assert.h>
#include <ctype.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <sys/types.h>

#include "fru_errno.h"
#include "frugen.h"
#include "smbios.h"

#ifdef __HAS_JSON__
#include "frugen-json.h"
#endif

volatile int debug_level = 0;

const frugen_name_t chassis_fields[FRU_CHASSIS_FIELD_COUNT] = {
	[FRU_CHASSIS_PARTNO] = { "pn", "Part Number" },
	[FRU_CHASSIS_SERIAL] = { "serial", "Serial Number" },
};

const frugen_name_t board_fields[FRU_BOARD_FIELD_COUNT] = {
	[FRU_BOARD_MFG] = { "mfg", "Manufacturer" },
	[FRU_BOARD_PRODNAME] = { "pname", "Product Name" },
	[FRU_BOARD_SERIAL] = { "serial", "Serial Number" },
	[FRU_BOARD_PARTNO] = { "pn", "Part Number" },
	[FRU_BOARD_FILE] = { "file", "FRU File ID" },
};
const frugen_name_t product_fields[FRU_PROD_FIELD_COUNT] = {
	[FRU_PROD_MFG] = { "mfg", "Manufacturer" },
	[FRU_PROD_NAME] = { "pname", "Product Name" },
	[FRU_PROD_MODELPN] = { "pn", "Part/Model Number" },
	[FRU_PROD_VERSION] = { "version", "Version" },
	[FRU_PROD_SERIAL] = { "serial", "Serial Number" },
	[FRU_PROD_ASSET] = { "atag", "Asset Tag" },
	[FRU_PROD_FILE] = { "file", "FRU File ID" },
};

const size_t field_max[FRU_TOTAL_AREAS] = {
	[FRU_CHASSIS_INFO] = FRU_CHASSIS_FIELD_COUNT,
	[FRU_BOARD_INFO] = FRU_BOARD_FIELD_COUNT,
	[FRU_PRODUCT_INFO] = FRU_PROD_FIELD_COUNT,
};
const frugen_name_t * const field_name[FRU_TOTAL_AREAS] = {
	[FRU_CHASSIS_INFO] = chassis_fields,
	[FRU_BOARD_INFO] = board_fields,
	[FRU_PRODUCT_INFO] = product_fields,
};

/* List only the encodings that can be legally saved in
 * a fru_field_t. That is all real encodings plus 'auto' and 'empty'.
 * FRU_FE_PRESERVE can only be used as a parameter to fru_setfield()
 * and is never saved to a variable.
 */
static
const char * const frugen_enc_names[FRU_FE_TOTALCOUNT] = {
	[FRU_FE_EMPTY] = "empty",
	[FRU_FE_AUTO] = "auto",
	[FRU_FE_BINARY] = "binary", /* For input data that is hex string [0-9A-Fa-f] */
	[FRU_FE_BCDPLUS] = "bcdplus",
	[FRU_FE_6BITASCII] = "6bitascii",
	[FRU_FE_TEXT] = "text",
};

const char * frugen_enc_name_by_val(fru_field_enc_t enc)
{

	if (0 > enc || enc >= FRU_FE_TOTALCOUNT) {
		return "undefined";
	}

	if (!frugen_enc_names[enc]) {
		return "invalid";
	}

	return frugen_enc_names[enc];
}

fru_field_enc_t frugen_enc_by_name(const char * name)
{
	debug(4, "Looking for encoding '%s'", name);
	for (fru_field_enc_t i = 0; i <= FRU_FE_AUTO; i++) {
		if (!strcmp(name, frugen_enc_names[i])) {
			debug(4, "Encoding '%s' is definitely %d", name, i);
			return i;
		}
		debug(4, "Encoding '%s' is not %d", name, i);
	}
	return FRU_FE_UNKNOWN;
}

#define MGMT_TYPENAME_ID(name) FRU_MR_MGMT_SUBTYPE_TO_IDX(FRU_MR_MGMT_##name)

const frugen_name_t frugen_mr_mgmt_name[FRU_MR_MGMT_INDEX_COUNT] = {
	[MGMT_TYPENAME_ID(SYS_URL)] = { "surl", "System URL" },
	[MGMT_TYPENAME_ID(SYS_NAME)] = { "sname", "System Name" },
	[MGMT_TYPENAME_ID(SYS_PING)] = { "spingaddr", "System Ping Address" },
	[MGMT_TYPENAME_ID(COMPONENT_URL)] = { "curl", "Component URL" },
	[MGMT_TYPENAME_ID(COMPONENT_NAME)] = { "cname", "Component Name" },
	[MGMT_TYPENAME_ID(COMPONENT_PING)] = { "cpingaddr", "Component Ping Address" },
	[MGMT_TYPENAME_ID(SYS_UUID)] = { "uuid", "System Unique ID" },
};

fru_mr_mgmt_type_t frugen_mr_mgmt_type_by_name(const char * name)
{
	off_t i;
	fru_mr_mgmt_type_t subtype = FRU_MR_MGMT_INVALID;

	if (!name) {
		warn("FRU MR Management Record type not provided");
		goto out;
	}

	for (i = MGMT_TYPENAME_ID(MIN); i <= MGMT_TYPENAME_ID(MAX); i++) {
		if (!strcmp(frugen_mr_mgmt_name[i].json, name)) {
			subtype = FRU_MR_MGMT_IDX_TO_SUBTYPE(i);
			goto out;
		}
	}
	warn("Invalid FRU MR Management Record type '%s'", name);
out:
	return subtype;
}

const frugen_name_t * frugen_mr_mgmt_name_by_type(fru_mr_mgmt_type_t type)
{
	if (!FRU_MR_MGMT_IS_SUBTYPE_VALID(type)) {
		fatal("FRU MR Management Record type %d is out of range", type);
	}
	off_t i = FRU_MR_MGMT_SUBTYPE_TO_IDX(type);
	return &frugen_mr_mgmt_name[i];
}

static inline
bool isdelim(char c)
{
	return ((c == ' ') || (c == '.') || (c == '-') || (c ==':'));
}

static char * const mr_type_names[FRU_MR_TYPE_COUNT] = {
	[FRU_MR_PSU_INFO] = "PSU Information",
	[FRU_MR_DC_OUT] = "DC Output",
	[FRU_MR_DC_LOAD] = "DC Load",
	[FRU_MR_MGMT_ACCESS] = "Management Access Record",
	[FRU_MR_BCR] = "Base Compatibility Record",
	[FRU_MR_ECR] = "Extended Compatibility Record",

	[FRU_MR_ASF_FIXED_SMBUS] = "ASF Fixed SMBus Addresses",
	[FRU_MR_ASF_LEGACY_ALERTS] = "ASF Lecacy-Device Alerts",
	[FRU_MR_ASF_REMOTE_CTRL] = "ASF Remote Control",

	[FRU_MR_EXT_DC_OUT] = "Extended DC Output",
	[FRU_MR_EXT_DC_LOAD] = "Extended DC Load",

	[FRU_MR_NVME] = "NVMe Information",
	[FRU_MR_NVME_PCIE_PORT] = "NVMe PCIe Port",
	[FRU_MR_NVME_TOPOLOGY] = "NVMe Topolgy",
	[FRU_MR_NVME_RSVD_E] = "NVMe Reserved",
	[FRU_MR_NVME_RSVD_F] = "NVMe Reserved",

	[FRU_MR_OEM_START + 0] = "OEM", /* 0xC0 */
	[FRU_MR_OEM_START + 1] = "OEM", /* 0xC1 */
	[FRU_MR_OEM_START + 2] = "OEM", /* 0xC2 */
	[FRU_MR_OEM_START + 3] = "OEM", /* 0xC3 */
	[FRU_MR_OEM_START + 4] = "OEM", /* 0xC4 */
	[FRU_MR_OEM_START + 5] = "OEM", /* 0xC5 */
	[FRU_MR_OEM_START + 6] = "OEM", /* 0xC6 */
	[FRU_MR_OEM_START + 7] = "OEM", /* 0xC7 */
	[FRU_MR_OEM_START + 8] = "OEM", /* 0xC8 */
	[FRU_MR_OEM_START + 9] = "OEM", /* 0xC9 */
	[FRU_MR_OEM_START + 10] = "OEM", /* 0xCA */
	[FRU_MR_OEM_START + 11] = "OEM", /* 0xCB */
	[FRU_MR_OEM_START + 12] = "OEM", /* 0xCC */
	[FRU_MR_OEM_START + 13] = "OEM", /* 0xCD */
	[FRU_MR_OEM_START + 14] = "OEM", /* 0xCE */
	[FRU_MR_OEM_START + 15] = "OEM", /* 0xCF */
	[FRU_MR_OEM_START + 16] = "OEM", /* 0xD0 */
	[FRU_MR_OEM_START + 17] = "OEM", /* 0xD1 */
	[FRU_MR_OEM_START + 18] = "OEM", /* 0xD2 */
	[FRU_MR_OEM_START + 19] = "OEM", /* 0xD3 */
	[FRU_MR_OEM_START + 20] = "OEM", /* 0xD4 */
	[FRU_MR_OEM_START + 21] = "OEM", /* 0xD5 */
	[FRU_MR_OEM_START + 22] = "OEM", /* 0xD6 */
	[FRU_MR_OEM_START + 23] = "OEM", /* 0xD7 */
	[FRU_MR_OEM_START + 24] = "OEM", /* 0xD8 */
	[FRU_MR_OEM_START + 25] = "OEM", /* 0xD9 */
	[FRU_MR_OEM_START + 26] = "OEM", /* 0xDA */
	[FRU_MR_OEM_START + 27] = "OEM", /* 0xDB */
	[FRU_MR_OEM_START + 28] = "OEM", /* 0xDC */
	[FRU_MR_OEM_START + 29] = "OEM", /* 0xDD */
	[FRU_MR_OEM_START + 30] = "OEM", /* 0xDE */
	[FRU_MR_OEM_START + 31] = "OEM", /* 0xDF */
	[FRU_MR_OEM_START + 32] = "OEM", /* 0xE0 */
	[FRU_MR_OEM_START + 33] = "OEM", /* 0xE1 */
	[FRU_MR_OEM_START + 34] = "OEM", /* 0xE2 */
	[FRU_MR_OEM_START + 35] = "OEM", /* 0xE3 */
	[FRU_MR_OEM_START + 36] = "OEM", /* 0xE4 */
	[FRU_MR_OEM_START + 37] = "OEM", /* 0xE5 */
	[FRU_MR_OEM_START + 38] = "OEM", /* 0xE6 */
	[FRU_MR_OEM_START + 39] = "OEM", /* 0xE7 */
	[FRU_MR_OEM_START + 40] = "OEM", /* 0xE8 */
	[FRU_MR_OEM_START + 41] = "OEM", /* 0xE9 */
	[FRU_MR_OEM_START + 42] = "OEM", /* 0xEA */
	[FRU_MR_OEM_START + 43] = "OEM", /* 0xEB */
	[FRU_MR_OEM_START + 44] = "OEM", /* 0xEC */
	[FRU_MR_OEM_START + 45] = "OEM", /* 0xED */
	[FRU_MR_OEM_START + 46] = "OEM", /* 0xEE */
	[FRU_MR_OEM_START + 47] = "OEM", /* 0xEF */
	[FRU_MR_OEM_START + 48] = "OEM", /* 0xF0 */
	[FRU_MR_OEM_START + 49] = "OEM", /* 0xF1 */
	[FRU_MR_OEM_START + 50] = "OEM", /* 0xF2 */
	[FRU_MR_OEM_START + 51] = "OEM", /* 0xF3 */
	[FRU_MR_OEM_START + 52] = "OEM", /* 0xF4 */
	[FRU_MR_OEM_START + 53] = "OEM", /* 0xF5 */
	[FRU_MR_OEM_START + 54] = "OEM", /* 0xF6 */
	[FRU_MR_OEM_START + 55] = "OEM", /* 0xF7 */
	[FRU_MR_OEM_START + 56] = "OEM", /* 0xF8 */
	[FRU_MR_OEM_START + 57] = "OEM", /* 0xF9 */
	[FRU_MR_OEM_START + 58] = "OEM", /* 0xFA */
	[FRU_MR_OEM_START + 59] = "OEM", /* 0xFB */
	[FRU_MR_OEM_START + 60] = "OEM", /* 0xFC */
	[FRU_MR_OEM_START + 61] = "OEM", /* 0xFD */
	[FRU_MR_OEM_START + 62] = "OEM", /* 0xFE */
	[FRU_MR_OEM_END] = "OEM",        /* 0xFF */
	
	[FRU_MR_RAW] = "Unsupported (raw)"
};

/*
 * Break hex-string into lines of 16 octets each,
 * skip the delimiters (-,:,., )
 */
static
void fhexstrdump(FILE * fp, const char * prefix, const char * s)
{
	const size_t perline = 16;
	size_t i = 0; // Position into string
	size_t totalcount = 0; // Hex octet count

	while (s && s[i]) {
		size_t count;
		fprintf(fp, "%s%04zX:", prefix, totalcount);
		int16_t c = 0;
		char printable[perline + 1];
		memset(printable, 0, perline + 1);

		for (count = 0; count < perline && s[i]; i++) {
			if (isdelim(s[i]))
				continue;

			if (!isxdigit(s[i])) {
				fatal("\nNeither a hex digit nor a delimiter at offset 0x%04zX ('%c')", i, s[i]);
			}

			if (i % 2 == 1) {
				c = fru_hex2byte(&s[i - 1]);
				printable[count] = isprint(c) ? (uint8_t)(c & 0xff) : 0xFE;
				fprintf(fp, " %02hhX", c);
				count++;
			}
		}
		const size_t spaces_per_byte = 3; // Size of result of "%02X " above
		const size_t remains_bytes = perline - (count % (perline + 1));
		const size_t remains_spaces = 1 + remains_bytes * spaces_per_byte;
		fprintf(fp, "%*c| %s\n", (int)remains_spaces, ' ', printable);
		totalcount += count;
	}
//	if (totalcount % perline)
//		fprintf(fp, "\n");
}

#if 0
#define debug_dump(level, data, len, fmt, args...) do { \
	debug(level, fmt, ##args); \
	if (level <= debug_level) fhexdump(stderr, "DEBUG: ", data, len); \
} while(0)
#endif

/* Dump a raw MR record */
void mr_raw_dump(FILE * fp, fru_mr_rec_t * mr_rec, char * prefix)
{
	if (FRU_FE_TEXT != mr_rec->raw.enc)
		fhexstrdump(fp, prefix, mr_rec->raw.data);
	else {
		fprintf(fp, "%sPrintable data found:\n", prefix);
		// The printable data is always shorter than the data storage
		// in mr_rec, and is nul-terminated by fru_load*() functions,
		// so it is safe to just print it
		fprintf(fp, "%s[%s]\n", prefix, mr_rec->raw.data);
	}
}

bool datestr_to_tv(struct timeval * tv, const char * datestr)
{
	struct tm tm = {0};
	time_t time;

	int mday, mon, year, hour, min;

	/* We don't use strptime() to use the same format for all locales,
	 * and also because that function is not available on Windows */
	if(6 != sscanf(datestr, "%d/%d/%d %d:%d", &mday, &mon, &year, &hour, &min)) {
		return false;
	}

	tm.tm_mday = mday;
	tm.tm_mon = mon - 1;
	tm.tm_year = year - 1900;
	tm.tm_hour = hour;
	tm.tm_min = min;

	tzset(); // Set up local timezone
	tm.tm_isdst = -1; // Use local timezone data in mktime
	time = mktime(&tm); // Here we have local time since local Epoch
	tv->tv_sec = time + timezone; // Convert to UTC
	tv->tv_usec = 0;
	return true;
}

static struct frugen_config_s config = {
	.format = FRUGEN_FMT_UNSET,
	.outformat = FRUGEN_FMT_BINARY, /* Default binary output */
	.flags = FRU_NOFLAGS,
};

void tv_to_datestr(char * datestr, const struct timeval * tv)
{
		const struct timeval tv_unspecified = { 0 };

		if (!memcmp(&tv_unspecified, tv, sizeof(*tv))) {
			datestr[0] = 0; // Empty string for 'unspecified'
		}

		tzset(); // Set up local timezone
		struct tm bdtime;
		// Time in FRU is in UTC, convert to local
		time_t seconds = tv->tv_sec - timezone;
		localtime_r(&seconds, &bdtime);
		strftime(datestr, DATEBUF_SZ, "%d/%m/%Y %H:%M %Z", &bdtime);
}

const frugen_name_t area_names[FRU_TOTAL_AREAS] = {
	[ FRU_INTERNAL_USE ] = { "internal", "Internal Use" },
	[ FRU_CHASSIS_INFO ] = { "chassis", "Chassis Information" },
	[ FRU_BOARD_INFO ] = { "board", "Board Information" },
	[ FRU_PRODUCT_INFO ] = { "product", "Product Information" },
	[ FRU_MR ] = { "multirecord", "Multirecord" }
};

fru_errno_t get_fru_errno(void)
{
	return fru_errno;
}

void fru_perror(FILE *fp, const char *fmt, ...)
{
	const char * const sources[FERR_LOC_COUNT] = {
		area_names[FRU_INTERNAL_USE].human,
		area_names[FRU_CHASSIS_INFO].human,
		area_names[FRU_BOARD_INFO].human,
		area_names[FRU_PRODUCT_INFO].human,
		area_names[FRU_MR].human,
		"FRU",
		"frugen",
	};
	va_list args;
	va_start(args, fmt);
	vfprintf(fp, fmt, args);
	va_end(args);
	fprintf(fp, ": %s in %s ",
	        fru_strerr(fru_errno),
	        sources[fru_errno.src]);

	if (fru_errno.src != FERR_LOC_GENERAL && fru_errno.src != FERR_LOC_CALLER)
		fprintf(fp, "Area ");

	if (fru_errno.index >= 0) {
		if (fru_errno.code == FEAREANOTSUP || fru_errno.code == FEAREABADTYPE) {
			fprintf(fp, "(%d)", fru_errno.index);
		}
		else if (FRU_IS_INFO_AREA((fru_area_type_t)fru_errno.src)) {
			if (fru_errno.index < (int)field_max[fru_errno.src])
				fprintf(fp, "(field '%s')",
				        field_name[fru_errno.src][fru_errno.index].json);
			else
				fprintf(fp, "(field 'custom.%d')",
				        fru_errno.index - (int)field_max[fru_errno.src]);
		}
		else if (fru_errno.src == FERR_LOC_MR) {
			fprintf(fp, "(record %d)", fru_errno.index);
		}
	}
	fputc('\n', fp);
}

fieldopt_t arg_to_fieldopt(char * arg)
{
	fieldopt_t opt = { .type = FRU_FE_PRESERVE };
	char * p;

	/* Check if there is an encoding specifier */
	p = strchr(arg, ':');
	if (p) {
		*p = 0;
		debug(3, "Encoding specifier found");
		opt.type = FRU_FE_AUTO;
		if (p != arg) {
			opt.type = frugen_enc_by_name(arg);
			debug(2, "Encoding requested is '%s'", arg);
			debug(2, "Encoding parsed is '%s'", frugen_enc_name_by_val(opt.type));
			if (FRU_FE_UNKNOWN == opt.type) {
				fatal("Field encoding type '%s' is not supported", arg);
			}
		}
		arg = p+1;
	}
	else {
		debug(2, "Preserving original encoding (if any)");
	}

	/* Now check if the area is specified */
	p = strchr(arg, '.');
	if (!p || p == arg) {
		fatal("Area name must be specified");
	}
	*p = 0;

	FRU_FOREACH_AREA(opt.area) {
		if (!FRU_IS_INFO_AREA(opt.area))
			continue;
		if (!strcmp(arg, area_names[opt.area].json))
			break;
	}
	if (opt.area > FRU_MAX_AREA) {
		fatal("Bad area name '%s'", arg);
	}
	arg = p + 1;

	/* Now check if there is value */
	p = strchr(arg, '=');
	if ((p && arg == p) || (!p && !strlen(arg))) {
		fatal("Must specify field name for %s area", area_names[opt.area].human);
	}
	if (!p) {
		fatal("Must specify value for '%s.%s'",
		      area_names[opt.area].json, arg);
	}
	*p = 0;

#define FRU_FIELD_NOT_PRESENT (-1)
	if (!field_max[opt.area]) {
		fatal("No fields are settable for area '%s'",
			  area_names[opt.area].json);
	}
	for (opt.field.index = field_max[opt.area] - 1;
		 opt.field.index > FRU_FIELD_NOT_PRESENT; opt.field.index--)
	{
		if (!strcmp(arg, field_name[opt.area][opt.field.index].json))
			break;
	}
	if (opt.field.index == FRU_FIELD_NOT_PRESENT) {
		/* No standard field found, but it still can be a custom
		 * field specifier in form 'custom.<N>'
		 */
		if (!strncmp(arg, "custom", 6)) { /* It IS a custome field! */
			char * p2;
			opt.field.index = FRU_FIELD_CUSTOM;
			p2 = strchr(arg, '.');
			if (p2)
				opt.custom_index = atoi(p2 + 1);
			if (opt.custom_index < 0)
				fatal("Custom field index must be positive or zero");
		}
		else {
			fatal("Field '%s' doesn't exist in area '%s'",
			      arg, area_names[opt.area].json);
		}
	}
	opt.value = p + 1;
	debug(2, "Field '%s' is being set in '%s' to '%s'",
	         opt.field.index == FRU_FIELD_CUSTOM
	                            ? "custom"
	                            : field_name[opt.area][opt.field.index].json,
	         area_names[opt.area].json,
	         opt.value);


	return opt;
}

void load_fromfile(const char * fname,
                   const struct frugen_config_s * config,
                   fru_t * fru)
{
	assert(fname);

	switch(config->format) {
#ifdef __HAS_JSON__
	case FRUGEN_FMT_JSON:
		// This call exits on failures
		frugen_loadfile_json(fru, fname);
		break;
#endif /* __HAS_JSON__ */
	case FRUGEN_FMT_BINARY:
		fru = fru_loadfile(fru, fname, config->flags);
		if (!fru) {
			fru_fatal("Couldn't load FRU file");
		}
		break;
	default:
		fatal("Please specify the input file format");
		break;
	}
}

void print_info_area(FILE ** fp, const fru_t * fru, fru_area_type_t atype)
{
	const char * const aname = area_names[atype].human;

	/* First print area-specific non-string fields */
	if (FRU_CHASSIS_INFO == atype) {
		fprintf(*fp, "   %25s: %11s %d\n",
		        "Chassis Type", "", fru->chassis.type);
	}
	else {
		uint8_t lang = (FRU_BOARD_INFO == atype)
			? fru->board.lang
			: fru->product.lang;
		fprintf(*fp, "   %25s: %11s %d\n", "Language Code", "", lang);
	}

	if (FRU_BOARD_INFO == atype) {
		char datebuf[DATEBUF_SZ] = {};
		struct timeval tv_unspec = {};

		if (!memcmp(&fru->board.tv, &tv_unspec, sizeof(tv_unspec))) {
			sprintf(datebuf, "Unspecified %s",
			        fru->board.tv_auto
			        ? "(auto)"
			        : "");
		}
		else {
			tv_to_datestr(datebuf, &fru->board.tv);
		}

		fprintf(*fp, "   %25s: %11s %s\n", "Manufacturing date/time", "", datebuf);
	}

	/* Then print out the mandatory fields */
	for (size_t i = 0; i < field_max[atype]; i++) {
		const char * const name = field_name[atype][i].human;
		const fru_field_t * field = fru_getfield(fru, atype, i);
		if (!field)
			fru_fatal("Failed to get standard field '%s' from '%s'", name, aname);

		const char * encoding = frugen_enc_name_by_val(field->enc);
		fprintf(*fp, "   %25s: [%9s] \"%s\"\n",
		        name, encoding,
		        field->val);
	}

	if (!fru_get_custom(fru, atype, FRU_LIST_HEAD)) {
		printf("\n");
		return;
	}

	off_t idx = FRU_LIST_HEAD;
	fru_field_t * field = NULL;
	while ((field = fru_get_custom(fru, atype, idx))) {
		const char * encoding = frugen_enc_name_by_val(field->enc);
		fprintf(*fp, "   %22s %02jd: [%9s] \"%s\"\n",
		        "Custom", (intmax_t)idx, encoding, field->val);
		idx++;
	}
	if (fru_errno.code != FENOFIELD) {
		fru_perror(*fp, "   Error getting custom fields");
	}

	printf("\n");
}

void print_mr_area(FILE ** fp, size_t mr_index, fru_mr_rec_t * mr_rec)
{
	fru_mr_mgmt_type_t subtype = mr_rec->mgmt.subtype;
	bool valid = FRU_MR_MGMT_IS_SUBTYPE_VALID(subtype);
	fru_mr_type_t mr_type = mr_rec->type;
	if (mr_type == FRU_MR_RAW)
		mr_type = mr_rec->raw.type;

	if (!FRU_MR_IS_VALID_TYPE(mr_type)) {
		fprintf(*fp,
		        "   #%zu: INVALID RECORD (%d) (bug in libfru?)\n",
		        mr_index, mr_type
		);
		return;
	}
	fprintf(*fp,
	        "   #%zu: %s (0x%02hhX)%s\n", mr_index,
	        mr_type_names[mr_type], (uint8_t)mr_type,
	        (mr_rec->type == FRU_MR_RAW)
	        ? " - Decoding unsupported yet:"
	        : ""
	);

	switch (mr_rec->type) {
	case FRU_MR_RAW:
		mr_raw_dump(*fp, mr_rec, "       ");
		break;
	case FRU_MR_MGMT_ACCESS:
		fprintf(*fp,
		        "       Subtype %d: %s (%s)\n",
		        subtype,
		        valid
		        ? frugen_mr_mgmt_name_by_type(subtype)->human
		        : "INVALID",
		        valid
		        ? frugen_mr_mgmt_name_by_type(subtype)->json
		        : "-"
		);
		fprintf(*fp, "       Data     : %s\n", mr_rec->mgmt.data);
		break;
	default:
		fprintf(*fp, "       Decoding to text is not yet supported\n");
		break;
	}
	fprintf(*fp, "\n");
}

void print_area(FILE ** fp, const fru_t * fru, fru_area_type_t atype)
{
	fru_mr_rec_t * mr_rec = NULL;
	size_t mr_index = 0;
	const char * const aname = area_names[atype].human;

	fprintf(*fp, "=== %s Area ===\n\n", aname);

	switch(atype) {
	case FRU_INTERNAL_USE:
		fhexstrdump(*fp, "   ", fru->internal);
		printf("\n");
		break;

	case FRU_CHASSIS_INFO:
	case FRU_BOARD_INFO:
	case FRU_PRODUCT_INFO:
		print_info_area(fp, fru, atype);
		break;

	case FRU_MR:
		fru_clearerr();
		while((mr_rec = fru_get_mr(fru, mr_index))) {
			print_mr_area(fp, mr_index, mr_rec);
			mr_index++;
		}
		if (!mr_index) {
			if (fru_errno.code == FENONE) {
				fprintf(*fp, "   %25s\n", "The area is empty");
			}
			else {
				fru_perror(*fp, "   Probably a frugen BUG");
			}
		}
		break;
	default:
		fatal("BUG!!! Area %d should never be processed\n", atype);
	}
}

/**
 * Save the decoded FRU from \a info into a text file specified
 * by \a *fp or \a fname.
 *
 * @param[in,out] fp     Pointer to the file pointer to use for output.
 *                       If \a *fp is NULL, \a fname will be opened, and
 *                       the pointer to the file stream will be stored in \a *fp.
 * @param[in]     fname  Filename to open when \a *fp is NULL, may be NULL otherwise
 * @param[in]     info   The FRU information structure to get the FRU data from
 */
void save_to_text_file(FILE ** fp, const char * fname,
                       const fru_t * fru)
{
	if (!*fp) {
		*fp = fopen(fname, "w");
	}

	if (!*fp) {
		fatal("Failed to open file '%s' for writing: %m", fname);
	}

	fru_area_type_t atype;
	FRU_FOREACH_AREA(atype) {
		debug(3, "%s is %spresent",
		      area_names[atype].human,
		      fru->present[atype]
		      ? ""
		      : "not ");
		if (fru->present[atype]) {
			print_area(fp, fru, atype);
		}
	}
}

void frugen_update_uuid(fru_t * fru, const char * s)
{
	fru_mr_rec_t mr = {
		.type = FRU_MR_MGMT_ACCESS,
		.mgmt.subtype = FRU_MR_MGMT_SYS_UUID
	};

	// Blindly copy the data, sanity will be checked
	// later by libfru during record update/addition
	strncpy(mr.mgmt.data, s, FRU_MR_MGMT_MAXDATA);

	fru_mr_rec_t * old_mr;
	size_t index = FRU_LIST_HEAD;
	while ((old_mr = fru_find_mr(fru, FRU_MR_MGMT_ACCESS, &index))) {
		if (old_mr->mgmt.subtype == FRU_MR_MGMT_SYS_UUID)
			break;
		index++;
	}

	if (!old_mr) {
		/* No UUID yet, add one */
		if (!fru_add_mr(fru, FRU_LIST_TAIL, &mr)) {
			fru_fatal("Couln't add UUID");
		}
	}
	else {
		/* An UUID record is already present, update it */
		if (!fru_replace_mr(fru, index, &mr)) {
			fru_fatal("Couln't replace UUID");
		}
	}
}

int main(int argc, char * argv[])
{
	size_t i;
	FILE * fp = NULL;
	int opt;
	int lindex;
	bool single_option_help = false;
	fieldopt_t fieldopt = {};

	// Prevent intermixing of stderr and stdout outputs
	setbuf(stdout, NULL);

	/*
	 * Allocate a new decoded FRU file structure instance,
	 * set some defaults that are not zeroes.
	 *
	 * Its contents are to be filled further by command line options
	 * or overwritten by an input template file.
	 */
	fru_t * fru = fru_init(NULL);
	fru->chassis.type = SMBIOS_CHASSIS_UNKNOWN;
	fru->board.lang = FRU_LANG_ENGLISH;
	fru->board.tv_auto = true;
	fru->product.lang = FRU_LANG_ENGLISH;

	const char * fname = NULL;

	/* Options are sorted by .val */
	struct option options[] = {
		/* Set board date */
		{ .name = "board-date",    .val = 'd', .has_arg = required_argument },

		/* Set debug flags */
		{ .name = "debug",         .val = 'g', .has_arg = required_argument },

		/* Display usage help */
		{ .name = "help",          .val = 'h', .has_arg = optional_argument },

#ifdef __HAS_JSON__
		/* Set input file format to JSON */
		{ .name = "json",          .val = 'j', .has_arg = required_argument },
#endif

		/* Set the output data format */
		{ .name = "out-format",    .val = 'o', .has_arg = required_argument },

		/* Set input file format to raw binary */
		{ .name = "raw",          .val = 'r', .has_arg = required_argument },

		/* Set data and optionally type of encoding for a FRU field */
		{ .name = "set",          .val = 's', .has_arg = required_argument },

		/* Non-string fields for areas */
		{ .name = "chassis-type",  .val = 't', .has_arg = required_argument },
		{ .name = "board-date-unspec", .val = 'u', .has_arg = no_argument },

		/* MultiRecord area options */
		{ .name = "mr-uuid",       .val = 'U', .has_arg = required_argument },

		/* Increase verbosity */
		{ .name = "verbose",       .val = 'v', .has_arg = no_argument },
	};

	/* Sorted by index */
	const char * option_help[] = {
		['d'] = "Set board manufacturing date/time, use \"DD/MM/YYYY HH:MM\" format.\n\t\t"
		        "By default the current system date/time is used unless -u is specified",
		['g'] = "Set debug flag (use multiple times for multiple flags):\n\t\t"
		        "\tfver  - Ignore wrong version in FRU header\n\t\t"
			    "\taver  - Ignore wrong version in area headers\n\t\t"
			    "\trver  - Ignore wrong record version in multirecord area\n\t\t"
			    "\tasum  - Ignore wrong area checksum (for standard areas)\n\t\t"
			    "\trhsum - Ignore wrong record header checksum (for multirecord)\n\t\t"
			    "\trdsum - Ignore wrong record data checksum (for multirecord)\n\t\t"
			    "\trdlen - Ignore wrong record data size (for multirecord)\n\t\t"
			    "\taeof  - Ignore missing end-of-field in info areas, try to decode till the end\n\t\t"
			    "\treol  - Ignore missing EOL record, use any found records",
		['h'] = "Display this help. Use any option name as an argument to show\n\t\t"
		        "help for a single option.\n"
				"\n\t\t"
				"Examples:\n\t\t"
				"\tfrugen -h     # Show full program help\n\t\t"
				"\tfrugen -hhelp # Help for long option '--help'\n\t\t"
				"\tfrugen -hh    # Help for short option '-h'",
		['j'] = "Load FRU information from a JSON file, use '-' for stdin",
		['o'] = "Output format, one of:\n"
		        "\t\tbinary - Default format when writing to a file.\n"
		        "\t\t         For stdout, the following will be used, even\n"
		        "\t\t         if 'binary' is explicitly specified:\n"
#ifdef __HAS_JSON__
		        "\t\tjson   - Default when writing to stdout.\n"
#endif
		        "\t\ttext   - Plain text format, no decoding of MR area records"
#ifndef __HAS_JSON__
		        ".\n\t\t         Default format when writing to stdout"
#endif
		        ,
		['r'] = "Load FRU information from a raw binary file, use '-' for stdin",
		['s'] = "Set a text field in an area to the given value, use given encoding\n\t\t"
		        "Requires an argument in form [<encoding>:]<area>.<field>=<value>\n\t\t"
				"If an encoding is not specified at all, frugen will attempt to\n\t\t"
				"preserve the encoding specified in the template or will use 'auto'\n\t\t"
				"if none is set there. To force 'auto' encoding you may either\n\t\t"
				"specify it explicitly or use a bare ':' without any preceding text.\n"
			    "\n\t\t"
		        "Supported encodings:\n\t\t"
				"\tauto      - Autodetect encoding based on the used characters.\n\t\t"
				"\t            This will attempt to use the most compact encoding\n\t\t"
				"\t            among the following.\n\t\t"
		        "\t6bitascii - 6-bit ASCII, available characters:\n\t\t"
				"\t             !\"#$%^&'()*+,-./\n\t\t"
				"\t            1234567890:;<=>?\n\t\t"
				"\t            @ABCDEFGHIJKLMNO\n\t\t"
				"\t            PQRSTUVWXYZ[\\]^_\n\t\t"
				"\tbcdplus   - BCD+, available characters:\n\t\t"
				"\t            01234567890 -.\n\t\t"
				"\ttext      - Plain text (Latin alphabet only).\n\t\t"
				"\t            Characters: Any printable 8-bit ASCII byte.\n\t\t"
				"\tbinary    - Binary data represented as a hex string.\n\t\t"
				"\t            Characters: 0123456789ABCDEFabcdef\n"
				"\n\t\t"
				"For area and field names, please refer to example.json\n"
				"\n\t\t"
				"You may specify field name 'custom' to add a new custom field.\n\t\t"
				"Alternatively, you may specify field name 'custom.<N>' to\n\t\t"
				"replace the value of the custom field number N given in the\n\t\t"
				"input template file.\n"
				"\n\t\t"
				"Examples:\n"
				"\n\t\t"
				"\tfrugen -r fru-template.bin -s text:board.pname=\"MY BOARD\" out.fru\n\t\t"
				"\t\t# (encode board.pname as text)\n\t\t"
				"\tfrugen -r fru-template.bin -s board.pname=\"MY BOARD\" out.fru\n\t\t"
				"\t\t# (preserve original encoding type if possible)\n\t\t"
				"\tfrugen -r fru-template.bin -s :board.pname=\"MY BOARD\" out.fru\n\t\t"
				"\t\t# (auto-encode board.pname as 6-bit ASCII)\n\t\t"
				"\tfrugen -j fru-template.json -s binary:board.custom=0102DEADBEEF out.fru\n\t\t"
				"\t\t# (add a new binary-encoded custom field to board)\n\t\t"
				"\tfrugen -j fru-template.json -s binary:board.custom.2=0102DEADBEEF out.fru\n\t\t"
				"\t\t# (replace custom field 2 in board with new value)",
		/* Chassis info area related options */
		['t'] = "Set chassis type (hex). Defaults to 0x02 ('Unknown')",
		['u'] = "Don't use current system date/time for board mfg. date, use 'Unspecified'",
		/* MultiRecord area related options */
		['U'] = "Add/update a System Unique ID (UUID/GUID) record in MR area",
		['v'] = "Increase program verbosity (debug) level",
	};

	char optstring[FRU_ARRAY_SZ(options) * 2 + 1] = {0};

	for (i = 0; i < FRU_ARRAY_SZ(options); ++i) {
		static int k = 0;
		optstring[k++] = options[i].val;
		if (options[i].has_arg)
			optstring[k++] = ':';
		if (options[i].has_arg == optional_argument)
			optstring[k++] = ':';
	}

	/* Process command line options */
	do {
		lindex = -1;
		opt = getopt_long(argc, argv, optstring, options, &lindex);
		switch (opt) {
			case 'v': // verbose
				debug_level++;
				debug(debug_level, "Verbosity level set to %d", debug_level);
				break;
			case 'g': { // debug flag
				struct {
					const char * name;
					int value;
				} all_flags[] = {
					{ "fver", FRU_IGNFVER },
					{ "aver", FRU_IGNAVER },
					{ "rver", FRU_IGNRVER },
					{ "fhsum", FRU_IGNFHCKSUM },
					{ "fdsum", FRU_IGNFDCKSUM },
					{ "asum", FRU_IGNACKSUM },
					{ "rhsum", FRU_IGNRHCKSUM },
					{ "rdsum", FRU_IGNRDCKSUM },
					{ "rdlen", FRU_IGNMRDATALEN },
					{ "aeof", FRU_IGNAEOF },
					{ "reol", FRU_IGNRNOEOL },
					{ "big", FRU_IGNBIG },
				};
				debug(2, "Checking debug flag %s", optarg);
				for (size_t i = 0; i < FRU_ARRAY_SZ(all_flags); i++) {
					if (strcmp(all_flags[i].name, optarg))
						continue;
					config.flags |= all_flags[i].value;
					debug(2, "Debug flag accepted: %s", optarg);
					break;
				}
				break;
			}
			case 'h': // help
				printf("FRU Generator v%s (C) %s, "
					   "Alexander Amelkin <alexander@amelkin.msk.ru>\n",
					   VERSION, COPYRIGHT_YEARS);
				printf("\n"
					   "Usage: frugen [options] <filename>\n"
					   "\n"
					   "Options:\n\n");
				for (i = 0; i < FRU_ARRAY_SZ(options); i++) {
					if (optarg) {
						single_option_help = true;
						if ((optarg[1] || optarg[0] != options[i].val)
							&& strcmp(optarg, options[i].name))
						{
							// Only show help for the option given in optarg
							continue;
						}
					}
					printf("\t-%c%s, --%s%s\n" /* "\t-%c%s\n" */,
						   options[i].val,
						   options[i].has_arg
						   ? (options[i].has_arg == optional_argument)
						     ? "[<argument>]"
						     : " <argument>"
						   : "",
						   options[i].name,
						   options[i].has_arg
						   ? (options[i].has_arg == optional_argument)
						     ? "[=<argument>]"
						     : " <argument>"
						   : "");
					printf("\t\t%s.\n\n", option_help[options[i].val]);
					if (single_option_help)
						exit(0);
				}
				if (single_option_help && i == FRU_ARRAY_SZ(options)) {
					fatal("No such option '%s'\n", optarg);
				}
				printf("Example (encode from scratch):\n"
					   "\tfrugen -s board.mfg=\"Biggest International Corp.\" \\\n"
					   "\t       --set board.pname=\"Some Cool Product\" \\\n"
					   "\t       --set text:board.pn=\"BRD-PN-123\" \\\n"
					   "\t       --board-date \"10/1/2017 12:58:00\" \\\n"
					   "\t       --set board.serial=\"01171234\" \\\n"
					   "\t       --set board.file=\"Command Line\" \\\n"
					   "\t       --set binary:board.custom=\"01020304FEAD1E\" \\\n"
					   "\t       fru.bin\n"
					   "\n");
				printf("Example (decode to json, output to stdout):\n"
					   "\tfrugen --raw fru.bin -o json -\n"
					   "\n");
				printf("Example (modify binary file):\n"
					   "\tfrugen --raw fru.bin \\\n"
					   "\t       --set text:board.serial=123456789 \\\n"
					   "\t       --set text:board.custom.1=\"My custom field\" \\\n"
					   "\t       fru.bin\n");
				exit(0);
				break;

#ifdef __HAS_JSON__
			case 'j': // json
				config.format = FRUGEN_FMT_JSON;
				debug(1, "Using JSON input format");
				load_fromfile(optarg, &config, fru);
				break;
#endif

			case 'r': // raw binary
				config.format = FRUGEN_FMT_BINARY;
				debug(1, "Using RAW binary input format");
				debug(2, "Will load FRU information from file %s", optarg);
				load_fromfile(optarg, &config, fru);
				break;

			case 'o': { // out-format
				const char * const outfmt[] = {
#ifdef __HAS_JSON__
					[FRUGEN_FMT_JSON] = "json",
#endif
					[FRUGEN_FMT_BINARY] = "binary",
					[FRUGEN_FMT_TEXTOUT] = "text",
				};

				frugen_format_t i;
				for (i = FRUGEN_FMT_FIRST; i <= FRUGEN_FMT_LAST; i++) {
					if (outfmt[i] && !strcmp(optarg, outfmt[i])) {
						config.outformat = i;
						break;
					}
				}
				if (i != config.outformat) {
					warn("Output format '%s' not supported, using default.",
					     optarg);
					debug(1, "Using default output format");
				}
				}
				break;

			case 's': { // set field
				/* We intentionally waste some memory on these sparse arrays
				 * for the sake of data/code separation */
				fru_field_t * const fields[FRU_TOTAL_AREAS][FRU_MAX_FIELD_COUNT] = {
					[FRU_CHASSIS_INFO] = {
						[FRU_CHASSIS_PARTNO] = &fru->chassis.pn,
						[FRU_CHASSIS_SERIAL] = &fru->chassis.serial,
					},
					[FRU_BOARD_INFO] = {
						[FRU_BOARD_MFG] = &fru->board.mfg,
						[FRU_BOARD_PRODNAME] = &fru->board.pname,
						[FRU_BOARD_SERIAL] = &fru->board.serial,
						[FRU_BOARD_PARTNO] = &fru->board.pn,
						[FRU_BOARD_FILE] = &fru->board.file,
					},
					[FRU_PRODUCT_INFO] = {
						[FRU_PROD_MFG] = &fru->product.mfg,
						[FRU_PROD_NAME] = &fru->product.pname,
						[FRU_PROD_MODELPN] = &fru->product.pn,
						[FRU_PROD_VERSION] = &fru->product.ver,
						[FRU_PROD_SERIAL] = &fru->product.serial,
						[FRU_PROD_ASSET] = &fru->product.atag,
						[FRU_PROD_FILE] = &fru->product.file,
					},
				};

				/* Now do the actual job and set data in the appropriate locations */
				fieldopt = arg_to_fieldopt(optarg);
				fru_field_t * field = NULL;
				if (fieldopt.field.index != FRU_FIELD_CUSTOM)
					field = fields[fieldopt.area][fieldopt.field.index];
				else {
					if (fieldopt.custom_index) {
						field = fru_get_custom(fru, fieldopt.area, fieldopt.custom_index);
						if (!field) {
							fru_fatal("Custom field %d not found in specified area",
							          fieldopt.custom_index);
						}
						debug(3, "Modifying custom field %d. New value is [%s]",
						         fieldopt.custom_index, fieldopt.value);
					}
					else {
						field = fru_add_custom(fru, fieldopt.area, FRU_LIST_TAIL, FRU_FE_EMPTY, NULL);
						if (!field) {
							fru_fatal("Failed to add a custom field");
						}

						debug(3, "Adding a custom field from argument [%s]", optarg);
					}
				}
				if(!fru_setfield(field, fieldopt.type, fieldopt.value)) {
					fru_fatal("Failed to add custom field value '%s'", fieldopt.value);
				}
				// Don't care about errors. The area is either enabled now or was enabled before.
				fru_enable_area(fru, fieldopt.area, FRU_APOS_AUTO);
				break;
			}

			case 't': // chassis-type
				fru->chassis.type = strtol(optarg, NULL, 16);
				debug(2, "Chassis type will be set to 0x%02X from [%s]", fru->chassis.type, optarg);
				// Don't care about errors. The area is either enabled now or was enabled before.
				fru_enable_area(fru, FRU_CHASSIS_INFO, FRU_APOS_AUTO);
				break;

			case 'd': // board-date
				debug(2, "Board manufacturing date will be set from [%s]", optarg);
				if (!datestr_to_tv(&fru->board.tv, optarg))
					fatal("Invalid date/time format, use \"DD/MM/YYYY HH:MM:SS\"");
				// Don't care about errors. The area is either enabled now or was enabled before.
				fru_enable_area(fru, FRU_BOARD_INFO, FRU_APOS_AUTO);
				break;
			case 'u': // board-date-unspec
				fru->board.tv_auto = false;
				break;
			case 'U': {
				frugen_update_uuid(fru, optarg);
				break;
			}
			case '?':
				exit(1);
			default:
				break;
		}
	} while (opt != -1);

	/* Generate the output */
	if (optind >= argc)
		fatal("Filename must be specified");

	fname = argv[optind];

	if (!strcmp("-", fname)) {
		if (config.outformat == FRUGEN_FMT_BINARY)
#ifdef __HAS_JSON__
			config.outformat = FRUGEN_FMT_JSON;
#else
			config.outformat = FRUGEN_FMT_TEXTOUT;
#endif

		fp = stdout;
		debug(1, "FRU info data will be output to stdout");
	}
	else
		debug(1, "FRU info data will be stored in %s", fname);

	switch (config.outformat) {
#ifdef __HAS_JSON__
	case FRUGEN_FMT_JSON:
		save_to_json_file(&fp, fname, fru);
		break;
#endif
	case FRUGEN_FMT_TEXTOUT:
		save_to_text_file(&fp, fname, fru);
		break;

	default:
	case FRUGEN_FMT_BINARY:
		if (!fru_savefile(fname, fru))
			fru_fatal("Couldn't save binary FRU as %s", fname);
	}

	fru_free(fru);
}
