/** @file
 *  @brief FRU generator utility
 *
 *  Copyright (C) 2016-2023 Alexander Amelkin <alexander@amelkin.msk.ru>
 *  SPDX-License-Identifier: GPL-2.0-or-later OR Apache-2.0
 */
#ifndef VERSION
#define VERSION "v1.4-dirty-orphan"
#endif

#define COPYRIGHT_YEARS "2016-2023"
#define MAX_FILE_SIZE 1L * 1024L * 1024L

#define _GNU_SOURCE
#include <getopt.h>

#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <limits.h>
#include <ctype.h>
#include "fru.h"
#include "frugen.h"
#include "smbios.h"

#ifdef __HAS_JSON__
#include "frugen-json.h"
#endif

volatile int debug_level = 0;

static const char* fru_mr_mgmt_name[FRU_MR_MGMT_MAX] = {
	[MGMT_TYPENAME_ID(SYS_URL)] = "surl",
	[MGMT_TYPENAME_ID(SYS_NAME)] = "sname",
	[MGMT_TYPENAME_ID(SYS_PING)] = "spingaddr",
	[MGMT_TYPENAME_ID(COMPONENT_URL)] = "curl",
	[MGMT_TYPENAME_ID(COMPONENT_NAME)] = "cname",
	[MGMT_TYPENAME_ID(COMPONENT_PING)] = "cpingaddr",
	[MGMT_TYPENAME_ID(SYS_UUID)] = "uuid"
};

fru_mr_mgmt_type_t fru_mr_mgmt_type_by_name(const char *name)
{
	off_t i;

	if (!name)
		fatal("FRU MR Management Record type not provided");

	for (i = MGMT_TYPENAME_ID(MIN); i <= MGMT_TYPENAME_ID(MAX); i++) {
		if (!strcmp(fru_mr_mgmt_name[i], name))
			return i + FRU_MR_MGMT_MIN;
	}
	fatal("Invalid FRU MR Management Record type '%s'", name);
}

const char * fru_mr_mgmt_name_by_type(fru_mr_mgmt_type_t type)
{
	if (type < FRU_MR_MGMT_MIN || type > FRU_MR_MGMT_MAX) {
		fatal("FRU MR Management Record type %d is out of range", type);
	}
	return fru_mr_mgmt_name[MGMT_TYPE_ID(type)];
}

static
void fhexdump(FILE *fp, const char *prefix, const void *data, size_t len)
{
	size_t i;
	const unsigned char *buf = data;
	const size_t perline = 16;
	char printable[perline + 1];

	for (i = 0; i < len; ++i) {
		if (0 == (i % perline)) {
			memset(printable, 0, sizeof(printable));
			fprintf(fp, "%s%04x: ", prefix, (unsigned int)i);
		}

		fprintf(fp, "%02X ", buf[i]);
		printable[i % perline] = isprint(buf[i]) ? buf[i] : '.';

		if (perline - 1  == (i % perline)) {
			fprintf(fp, "| %s\n", printable);
		}
	}

	if (i % 16) {
		const size_t spaces_per_byte = 3; // Size of result of "%02X " above
		const size_t remains_bytes = perline - (i % perline);
		const size_t remains_spaces = remains_bytes * spaces_per_byte;
		fprintf(fp, "%*c| %s\n", (int)remains_spaces, ' ', printable);
	}
}

#define debug_dump(level, data, len, fmt, args...) do { \
	debug(level, fmt, ##args); \
	if (level <= debug_level) fhexdump(stderr, "DEBUG: ", data, len); \
} while(0)


bool datestr_to_tv(const char *datestr, struct timeval *tv)
{
	struct tm tm = {0};
	time_t time;
	char *ret;

#if __WIN32__ || __WIN64__
	/* There is no strptime() in Windows C libraries */
	int mday, mon, year, hour, min, sec;

	if(6 != sscanf(datestr, "%d/%d/%d %d:%d:%d", &mday, &mon, &year, &hour, &min, &sec)) {
		return false;
	}

	tm.tm_mday = mday;
	tm.tm_mon = mon - 1;
	tm.tm_year = year - 1900;
	tm.tm_hour = hour;
	tm.tm_min = min;
	tm.tm_sec = sec;
#else
	ret = strptime(datestr, "%d/%m/%Y%t%T", &tm);
	if (!ret || *ret != 0)
		return false;
#endif
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
	.no_curr_date = false,
};

void load_from_binary_file(const char *fname,
                           const struct frugen_config_s *config,
                           struct frugen_fruinfo_s *info)
{
	assert(fname);
	size_t mr_size;
	int fd = open(fname, O_RDONLY);
	debug(2, "Data format is BINARY");
	if (fd < 0) {
		fatal("Failed to open file: %m");
	}

	struct stat statbuf = {0};
	if (fstat(fd, &statbuf)) {
		fatal("Failed to get file properties: %m");
	}
	if (statbuf.st_size > MAX_FILE_SIZE) {
		fatal("File too large");
	}

	uint8_t *buffer = calloc(1, statbuf.st_size);
	if (buffer == NULL) {
		fatal("Cannot allocate buffer");
	}

	debug(2, "Reading the template file of size %lu...", statbuf.st_size);
	if (read(fd, buffer, statbuf.st_size) != statbuf.st_size) {
		fatal("Cannot read file");
	}
	close(fd);

	errno = 0;
	size_t iu_size;
	fru_internal_use_area_t *internal_area =
		find_fru_internal_use_area(buffer, &iu_size, statbuf.st_size,
								   config->flags);
	if (internal_area) {
		debug(2, "Found an internal use area of size %zd", iu_size);

		if (!fru_decode_internal_use_area(internal_area, iu_size,
										  &info->fru.internal_use,
										  config->flags))
		{
			fatal("Failed to decode interal use area");
		}

		debug(2, "Internal use area: %s", info->fru.internal_use);
		info->has_internal = true;
	}
	else {
		debug(2, "No internal use area found: %m");
	}

	errno = 0;
	fru_chassis_area_t *chassis_area =
		find_fru_chassis_area(buffer, statbuf.st_size, config->flags);
	if (chassis_area) {
		debug(2, "Found a chassis area");
		if (!fru_decode_chassis_info(chassis_area, &info->fru.chassis))
			fatal("Failed to decode chassis");
		info->has_chassis = true;
	}
	else {
		debug(2, "No chassis area found: %m");
	}

	errno = 0;
	fru_board_area_t *board_area =
		find_fru_board_area(buffer, statbuf.st_size, config->flags);
	if (board_area) {
		debug(2, "Found a board area");
		if (!fru_decode_board_info(board_area, &info->fru.board))
			fatal("Failed to decode board");
		info->has_board = true;
	}
	else {
		debug(2, "No board area found");
	}

	errno = 0;
	fru_product_area_t *product_area =
		find_fru_product_area(buffer, statbuf.st_size, config->flags);
	if (product_area) {
		debug(2, "Found a product area");
		if (!fru_decode_product_info(product_area, &info->fru.product))
			fatal("Failed to decode product");
		info->has_product = true;
	}
	else {
		debug(2, "No product area found");
	}

	errno = 0;
	mr_size = 0;
	fru_mr_area_t *mr_area =
		find_fru_mr_area(buffer, &mr_size, statbuf.st_size, config->flags);
	if (mr_area) {
		int rec_cnt;
		debug(2, "Found a multirecord area of size %zd", mr_size);
		rec_cnt = fru_decode_mr_area(mr_area, &info->fru.mr_reclist,
									 mr_size, config->flags);
		if (0 > rec_cnt)
			fatal("Failed to decode multirecord area");
		debug(2, "Loaded %d records from the multirecord area", rec_cnt);
		info->has_multirec = true;
	}
	else {
		debug(2, "No multirecord area found: %m");
	}

	free(buffer);
}

void load_fromfile(const char *fname,
                   const struct frugen_config_s *config,
                   struct frugen_fruinfo_s *info)
{
	assert(fname);

	switch(config->format) {
#ifdef __HAS_JSON__
	case FRUGEN_FMT_JSON:
		load_from_json_file(fname, info);
		break;
#endif /* __HAS_JSON__ */
	case FRUGEN_FMT_BINARY:
		load_from_binary_file(fname, config, info);
		break;
	default:
		fatal("Please specify the input file format");
		break;
	}
}

/**
 * Save the decoded FRU from \a info into a text file specified
 * by \a *fp or \a fname.
 *
 * @param[in,out] fp     Pointer to the file pointer to use for output.
 *                       If \a *fp is NULL, \a fname will be opened, and
 *                       the pointer to it will be stored in \a *fp.
 * @param[in]     fname  Filename to open when \a *fp is NULL, may be NULL otherwise
 * @param[in]     info   The FRU information structure to get the FRU data from
 * @param[in]     config Various frugen configuration settings structure
 */
void save_to_text_file(FILE **fp, const char *fname,
                       const struct frugen_fruinfo_s *info,
                       const struct frugen_config_s *config)
{
	(void)config; /* Silence the compiler, maybe use later */

	if (!*fp) {
		*fp = fopen(fname, "w");
	}

	if (!*fp) {
		fatal("Failed to open file '%s' for writing: %m", fname);
	}

	if (info->has_internal) {
		fru_internal_use_area_t *internal;
		uint8_t blocklen = info->areas[FRU_INTERNAL_USE].blocks;
		fputs("Internal use area\n", *fp);
		internal = fru_encode_internal_use_area(info->fru.internal_use, &blocklen);
		if (!internal)
			fatal("Failed to encode internal use area: %m\n"
				  "Check that the value is a hex string of even bytes length");
		fhexdump(*fp, "\t", internal->data, FRU_BYTES(blocklen));
	}

	if (info->has_chassis) {
		fputs("Chassis\n", *fp);
		fprintf(*fp, "\ttype: %u\n", info->fru.chassis.type);
		fprintf(*fp, "\tpn(%s): %s\n",
		             fru_enc_name_by_type(info->fru.chassis.pn.type),
		             info->fru.chassis.pn.val);
		fprintf(*fp, "\tserial(%s): %s\n",
		             fru_enc_name_by_type(info->fru.chassis.serial.type),
		             info->fru.chassis.serial.val);
		fru_reclist_t *next = info->fru.chassis.cust;
		while (next != NULL) {
			fprintf(*fp, "\tcustom(%s): %s\n",
			             fru_enc_name_by_type(next->rec->type),
			             next->rec->val);
			next = next->next;
		}
	}

	if (info->has_product) {
		fputs("Product\n", *fp);
		fprintf(*fp, "\tlang: %u\n", info->fru.product.lang);
		fprintf(*fp, "\tmfg(%s): %s\n",
		             fru_enc_name_by_type(info->fru.product.mfg.type),
		             info->fru.product.mfg.val);
		fprintf(*fp, "\tpname(%s): %s\n",
		             fru_enc_name_by_type(info->fru.product.pname.type),
		             info->fru.product.pname.val);
		fprintf(*fp, "\tserial(%s): %s\n",
		             fru_enc_name_by_type(info->fru.product.serial.type),
		             info->fru.product.serial.val);
		fprintf(*fp, "\tpn(%s): %s\n",
		             fru_enc_name_by_type(info->fru.product.pn.type),
		             info->fru.product.pn.val);
		fprintf(*fp, "\tver(%s): %s\n",
		             fru_enc_name_by_type(info->fru.product.ver.type),
		             info->fru.product.ver.val);
		fprintf(*fp, "\tatag(%s): %s\n",
		             fru_enc_name_by_type(info->fru.product.atag.type),
		             info->fru.product.atag.val);
		fprintf(*fp, "\tfile(%s): %s\n",
		             fru_enc_name_by_type(info->fru.product.file.type),
		             info->fru.product.file.val);
		fru_reclist_t *next = info->fru.product.cust;
		while (next != NULL) {
			fprintf(*fp, "\tcustom(%s): %s\n",
			             fru_enc_name_by_type(next->rec->type),
			             next->rec->val);
			next = next->next;
		}
	}

	if (info->has_board) {
		char timebuf[20] = {0};
		struct tm* bdtime = gmtime(&info->fru.board.tv.tv_sec);
		strftime(timebuf, 20, "%d/%m/%Y %H:%M:%S", bdtime);

		fputs("Board\n", *fp);
		fprintf(*fp, "\tlang: %u\n", info->fru.board.lang);
		fprintf(*fp, "\tdate: %s\n", timebuf);
		fprintf(*fp, "\tmfg(%s): %s\n",
		             fru_enc_name_by_type(info->fru.board.mfg.type),
		             info->fru.board.mfg.val);
		fprintf(*fp, "\tpname(%s): %s\n",
		             fru_enc_name_by_type(info->fru.board.pname.type),
		             info->fru.board.pname.val);
		fprintf(*fp, "\tserial(%s): %s\n",
		             fru_enc_name_by_type(info->fru.board.serial.type),
		             info->fru.board.serial.val);
		fprintf(*fp, "\tpn(%s): %s\n",
		             fru_enc_name_by_type(info->fru.board.pn.type),
		             info->fru.board.pn.val);
		fprintf(*fp, "\tfile(%s): %s\n",
		             fru_enc_name_by_type(info->fru.board.file.type),
		             info->fru.board.file.val);
		fru_reclist_t *next = info->fru.board.cust;
		while (next != NULL) {
			fprintf(*fp, "\tcustom(%s): %s\n",
			             fru_enc_name_by_type(next->rec->type),
			             next->rec->val);
			next = next->next;
		}
	}

	if (info->has_multirec) {
		fru_mr_reclist_t *entry = info->fru.mr_reclist;
		size_t count = 0;
		fputs("Multirecord\n", *fp);
		fputs("\tNOTE: Data decoding is only available in JSON mode\n", *fp);

		while (entry) {
			fprintf(*fp, "\trecord %03zd:\n", count++);
			fhexdump(*fp, "\t\t", entry->rec, FRU_MR_REC_SZ(entry->rec));
			if (IS_FRU_MR_END(entry->rec))
				break;
			entry = entry->next;
		}
	}
}

/**
 * Save the encoded FRU from \a info into a binary file specified
 * by \a fname.
 *
 * @param[in]     fname  Filename to open when \a *fp is NULL, may be NULL otherwise
 * @param[in]     info   The FRU information structure to get the FRU data from
 * @param[in]     config Various frugen configuration settings structure
 */
void save_to_binary_file(const char *fname,
                         struct frugen_fruinfo_s *info,
                         const struct frugen_config_s *config)
{
	fru_t *fru;
	size_t size;
	int fd;

	if (info->has_internal) {
		fru_internal_use_area_t *internal;
		/* .blocks is used later by fru_create() */
		uint8_t *blocklen = &info->areas[FRU_INTERNAL_USE].blocks;
		debug(1, "FRU file will have an internal use area");
		internal = fru_encode_internal_use_area(info->fru.internal_use, blocklen);
		if (!internal)
			fatal("Failed to encode internal use area: %m\n"
				  "Check that the value is a hex string of even bytes length");
		free(info->fru.internal_use);
		info->fru.internal_use = NULL;
		info->areas[FRU_INTERNAL_USE].data = internal;
	}

	if (info->has_chassis) {
		int e;
		fru_chassis_area_t *ci = NULL;
		debug(1, "FRU file will have a chassis information area");
		debug(3, "Chassis information area's custom field list is %p", info->fru.chassis.cust);
		ci = fru_encode_chassis_info(&info->fru.chassis);
		e = errno;
		free_reclist(info->fru.chassis.cust);

		if (ci)
			info->areas[FRU_CHASSIS_INFO].data = ci;
		else {
			errno = e;
			fatal("Error allocating a chassis info area: %m");
		}
	}

	if (info->has_board) {
		int e;
		fru_board_area_t *bi = NULL;
		debug(1, "FRU file will have a board information area");
		debug(3, "Board information area's custom field list is %p", info->fru.board.cust);
		debug(3, "Board date is specified? = %d", info->has_bdate);
		debug(3, "Board date use unspec? = %d", config->no_curr_date);
		if (!info->has_bdate && config->no_curr_date) {
			debug(1, "Using 'unspecified' board mfg. date");
			info->fru.board.tv = (struct timeval){0};
		}

		bi = fru_encode_board_info(&info->fru.board);
		e = errno;
		free_reclist(info->fru.board.cust);

		if (bi)
			info->areas[FRU_BOARD_INFO].data = bi;
		else {
			errno = e;
			fatal("Error allocating a board info area: %m");
		}
	}

	if (info->has_product) {
		int e;
		fru_product_area_t *pi = NULL;
		debug(1, "FRU file will have a product information area");
		debug(3, "Product information area's custom field list is %p", info->fru.product.cust);
		pi = fru_encode_product_info(&info->fru.product);

		e = errno;
		free_reclist(info->fru.product.cust);

		if (pi)
			info->areas[FRU_PRODUCT_INFO].data = pi;
		else {
			errno = e;
			fatal("Error allocating a product info area: %m");
		}
	}

	if (info->has_multirec) {
		int e;
		fru_mr_area_t *mr = NULL;
		size_t totalbytes = 0;
		debug(1, "FRU file will have a multirecord area");
		debug(3, "Multirecord area record list is %p", info->fru.mr_reclist);
		mr = fru_encode_mr_area(info->fru.mr_reclist, &totalbytes);

		e = errno;
		free_reclist(info->fru.mr_reclist);

		if (mr) {
			info->areas[FRU_MULTIRECORD].data = mr;
			info->areas[FRU_MULTIRECORD].blocks = FRU_BLOCKS(totalbytes);

			debug_dump(3, mr, totalbytes, "Multirecord data:");
		}
		else {
			errno = e;
			fatal("Error allocating a multirecord area: %m");
		}
	}

	fru = fru_create(info->areas, &size);
	if (!fru) {
		fatal("Error allocating a FRU file buffer: %m");
	}

	debug(1, "Writing %lu bytes of FRU data", (long unsigned int)FRU_BYTES(size));

	fd = open(fname,
#if __WIN32__ || __WIN64__
			  O_CREAT | O_TRUNC | O_WRONLY | O_BINARY,
#else
			  O_CREAT | O_TRUNC | O_WRONLY,
#endif
			  0644);

	if (fd < 0)
		fatal("Couldn't create file %s: %m", fname);

	if (0 > write(fd, fru, FRU_BYTES(size)))
		fatal("Couldn't write to %s: %m", fname);

	free(fru);
	close(fd);

}

int main(int argc, char *argv[])
{
	size_t i;
	FILE *fp = NULL;
	int opt;
	int lindex;

	struct frugen_fruinfo_s fruinfo = {
		.fru = {
			.internal_use = NULL,
			.chassis      = { .type = SMBIOS_CHASSIS_UNKNOWN },
			.board        = { .lang = LANG_ENGLISH },
			.product      = { .lang = LANG_ENGLISH },
			.mr_reclist   = NULL,
		},
		.areas = {
			{ .atype = FRU_INTERNAL_USE },
			{ .atype = FRU_CHASSIS_INFO },
			{ .atype = FRU_BOARD_INFO, },
			{ .atype = FRU_PRODUCT_INFO, },
			{ .atype = FRU_MULTIRECORD }
		},
		.has_chassis  = false,
		.has_board    = false,
		.has_bdate    = false,
		.has_product  = false,
		.has_internal = false,
		.has_multirec = false,
	};

	bool cust_binary = false; // Flag: treat the following custom attribute as binary

	const char *fname = NULL;

	tzset();
	gettimeofday(&fruinfo.fru.board.tv, NULL);
	fruinfo.fru.board.tv.tv_sec += timezone;

	struct option options[] = {
		/* Display usage help */
		{ .name = "help",          .val = 'h', .has_arg = false },

		/* Increase verbosity */
		{ .name = "verbose",       .val = 'v', .has_arg = false },

		/* Set debug flags */
		{ .name = "debug",         .val = 'g', .has_arg = true },

		/* Mark the following '*-custom' data as binary */
		{ .name = "binary",        .val = 'b', .has_arg = false },

		/* Disable autodetection, force ASCII encoding on standard fields,
		 * Detection of binary (out of ASCII range) stays in place.
		 */
		{ .name = "ascii",         .val = 'I', .has_arg = false },

#ifdef __HAS_JSON__
		/* Set input file format to JSON */
		{ .name = "json",          .val = 'j', .has_arg = false },
#endif

		/* Set input file format to raw binary */
		{ .name = "raw",          .val = 'r', .has_arg = false },

		/* Set file to load the data from */
		{ .name = "from",          .val = 'z', .has_arg = true },

		/* Set the output data format */
		{ .name = "out-format",    .val = 'o', .has_arg = true },

		/* Chassis info area related options */
		{ .name = "chassis-type",  .val = 't', .has_arg = true },
		{ .name = "chassis-pn",    .val = 'a', .has_arg = true },
		{ .name = "chassis-serial",.val = 'c', .has_arg = true },
		{ .name = "chassis-custom",.val = 'C', .has_arg = true },
		/* Board info area related options */
		{ .name = "board-pname",   .val = 'n', .has_arg = true },
		{ .name = "board-mfg",     .val = 'm', .has_arg = true },
		{ .name = "board-date",    .val = 'd', .has_arg = true },
		{ .name = "board-date-unspec", .val = 'u', .has_arg = false },
		{ .name = "board-pn",      .val = 'p', .has_arg = true },
		{ .name = "board-serial",  .val = 's', .has_arg = true },
		{ .name = "board-file",    .val = 'f', .has_arg = true },
		{ .name = "board-custom",  .val = 'B', .has_arg = true },
		/* Product info area related options */
		{ .name = "prod-name",     .val = 'N', .has_arg = true },
		{ .name = "prod-mfg",      .val = 'G', .has_arg = true },
		{ .name = "prod-modelpn",  .val = 'M', .has_arg = true },
		{ .name = "prod-version",  .val = 'V', .has_arg = true },
		{ .name = "prod-serial",   .val = 'S', .has_arg = true },
		{ .name = "prod-file",     .val = 'F', .has_arg = true },
		{ .name = "prod-atag",     .val = 'A', .has_arg = true },
		{ .name = "prod-custom",   .val = 'P', .has_arg = true },
		/* MultiRecord area related options */
		{ .name = "mr-uuid",       .val = 'U', .has_arg = true },
	};

	const char *option_help[] = {
		['h'] = "Display this help",
		['v'] = "Increase program verbosity (debug) level",
		['g'] = "Set debug flag (use multiple times for multiple flags):\n\t\t"
		        "\tfver  - Ignore wrong version in FRU header\n\t\t"
			    "\taver  - Ignore wrong version in area headers\n\t\t"
			    "\trver  - Ignore wrong verison in multirecord area record version\n\t\t"
			    "\tasum  - Ignore wrong area checksum (for standard areas)\n\t\t"
			    "\trhsum - Ignore wrong record header checksum (for multirecord)\n\t\t"
			    "\trdsum - Ignore wrong data checksum (for multirecord)\n\t\t"
			    "\trend  - Ignore missing EOL record, use any found records",
		['b'] = "Mark the next --*-custom option's argument as binary.\n\t\t"
			    "Use hex string representation for the next custom argument.\n"
			    "\n\t\t"
			    "Example: frugen --binary --board-custom 0012DEADBEAF\n"
			    "\n\t\t"
			    "There must be an even number of characters in a 'binary' argument",
		['I'] = "Disable auto-encoding on all fields, force ASCII.\n\t\t"
			    "Out of ASCII range data will still result in binary encoding",
		['j'] = "Set input file format to JSON. Specify before '--from'",
		['r'] = "Set input file format to raw binary. Specify before '--from'",
		['z'] = "Load FRU information from a file, use '-' for stdout",
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
		/* Chassis info area related options */
		['t'] = "Set chassis type (hex). Defaults to 0x02 ('Unknown')",
		['a'] = "Set chassis part number",
		['c'] = "Set chassis serial number",
		['C'] = "Add a custom chassis information field, may be used multiple times.\n\t\t"
		        "NOTE: This does NOT replace the data specified in the template",
		/* Board info area related options */
		['n'] = "Set board product name",
		['m'] = "Set board manufacturer name",
		['d'] = "Set board manufacturing date/time, use \"DD/MM/YYYY HH:MM:SS\" format.\n\t\t"
		        "By default the current system date/time is used unless -u is specified",
		['u'] = "Don't use current system date/time for board mfg. date, use 'Unspecified'",
		['p'] = "Set board part number",
		['s'] = "Set board serial number",
		['f'] = "Set board FRU file ID",
		['B'] = "Add a custom board information field, may be used multiple times.\n\t\t"
		        "NOTE: This does NOT replace the data specified in the template",
		/* Product info area related options */
		['N'] = "Set product name",
		['G'] = "Set product manufacturer name",
		['M'] = "Set product model / part number",
		['V'] = "Set product version",
		['S'] = "Set product serial number",
		['F'] = "Set product FRU file ID",
		['A'] = "Set product Asset Tag",
		['P'] = "Add a custom product information field, may be used multiple times\n\t\t"
		        "NOTE: This does NOT replace the data specified in the template",
		/* MultiRecord area related options */
		['U'] = "Set System Unique ID (UUID/GUID)\n\t\t"
		        "NOTE: This does NOT replace the data specified in the template",
	};

	char optstring[ARRAY_SZ(options) * 2 + 1] = {0};

	for (i = 0; i < ARRAY_SZ(options); ++i) {
		static int k = 0;
		optstring[k++] = options[i].val;
		if (options[i].has_arg)
			optstring[k++] = ':';
	}

	/* Process command line options */
	do {
		fru_reclist_t **custom = NULL;
		bool is_mr_record = false; // The current option is an MR area record
		lindex = -1;
		opt = getopt_long(argc, argv, optstring, options, &lindex);
		switch (opt) {
			case 'b': // binary
				debug(2, "Next custom field will be considered binary");
				cust_binary = true;
				break;
			case 'I': // ASCII
				fru_set_autodetect(false);
				break;
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
					{ "rend", FRU_IGNRNOEOL },
				};
				debug(2, "Checking debug flag %s", optarg);
				for (size_t i = 0; i < ARRAY_SZ(all_flags); i++) {
					if (strcmp(all_flags[i].name, optarg))
						continue;
					config.flags |= all_flags[i].value;
					debug(2, "Debug flag accepted: %s", optarg);
					break;
				}
				break;
			}
			case 'h': // help
				printf("FRU Generator %s (C) %s, "
					   "Alexander Amelkin <alexander@amelkin.msk.ru>\n",
					   VERSION, COPYRIGHT_YEARS);
				printf("\n"
					   "Usage: frugen [options] <filename>\n"
					   "\n"
					   "Options:\n\n");
				for (i = 0; i < ARRAY_SZ(options); i++) {
					printf("\t-%c, --%s%s\n" /* "\t-%c%s\n" */,
					       options[i].val,
					       options[i].name,
					       options[i].has_arg ? " <argument>" : "");
					printf("\t\t%s.\n\n", option_help[options[i].val]);
				}
				printf("Example (encode):\n"
				       "\tfrugen --board-mfg \"Biggest International Corp.\" \\\n"
				       "\t       --board-pname \"Some Cool Product\" \\\n"
				       "\t       --board-pn \"BRD-PN-123\" \\\n"
				       "\t       --board-date \"10/1/2017 12:58:00\" \\\n"
				       "\t       --board-serial \"01171234\" \\\n"
				       "\t       --board-file \"Command Line\" \\\n"
				       "\t       --binary --board-custom \"01020304FEAD1E\" \\\n"
				       "\t       fru.bin\n"
				       "\n");
				printf("Example (decode):\n"
				       "\tfrugen --raw --from fru.bin -\n");
				exit(0);
				break;

#ifdef __HAS_JSON__
			case 'j': // json
				config.format = FRUGEN_FMT_JSON;
				debug(1, "Using JSON input format");
				break;
#endif

			case 'r': // binary
				config.format = FRUGEN_FMT_BINARY;
				debug(1, "Using RAW binary input format");
				break;

			case 'z': // from
				debug(2, "Will load FRU information from file %s", optarg);
				load_fromfile(optarg, &config, &fruinfo);

				break;

			case 'o': // out-format
#ifdef __HAS_JSON__
				if (!strcmp(optarg, "json")) {
					config.outformat = FRUGEN_FMT_JSON;
				}
				else
#endif
				if (!strcmp(optarg, "text")) {
					config.outformat = FRUGEN_FMT_TEXTOUT;
				}
				else {
					debug(1, "Using default output format");
				}
				break;

			case 't': // chassis-type
				fruinfo.fru.chassis.type = strtol(optarg, NULL, 16);
				debug(2, "Chassis type will be set to 0x%02X from [%s]", fruinfo.fru.chassis.type, optarg);
				fruinfo.has_chassis = true;
				break;
			case 'a': // chassis-pn
				fru_loadfield(fruinfo.fru.chassis.pn.val, optarg);
				fruinfo.has_chassis = true;
				break;
			case 'c': // chassis-serial
				fru_loadfield(fruinfo.fru.chassis.serial.val, optarg);
				fruinfo.has_chassis = true;
				break;
			case 'C': // chassis-custom
				debug(2, "Custom chassis field [%s]", optarg);
				fruinfo.has_chassis = true;
				custom = &fruinfo.fru.chassis.cust;
				break;
			case 'n': // board-pname
				fru_loadfield(fruinfo.fru.board.pname.val, optarg);
				fruinfo.has_board = true;
				break;
			case 'm': // board-mfg
				fru_loadfield(fruinfo.fru.board.mfg.val, optarg);
				fruinfo.has_board = true;
				break;
			case 'd': // board-date
				debug(2, "Board manufacturing date will be set from [%s]", optarg);
				if (!datestr_to_tv(optarg, &fruinfo.fru.board.tv))
					fatal("Invalid date/time format, use \"DD/MM/YYYY HH:MM:SS\"");
				fruinfo.has_board = true;
				break;
			case 'u': // board-date-unspec
				config.no_curr_date = true;
				break;
			case 'p': // board-pn
				fru_loadfield(fruinfo.fru.board.pn.val, optarg);
				fruinfo.has_board = true;
				break;
			case 's': // board-sn
				fru_loadfield(fruinfo.fru.board.serial.val, optarg);
				fruinfo.has_board = true;
				break;
			case 'f': // board-file
				fru_loadfield(fruinfo.fru.board.file.val, optarg);
				fruinfo.has_board = true;
				break;
			case 'B': // board-custom
				debug(2, "Custom board field [%s]", optarg);
				fruinfo.has_board = true;
				custom = &fruinfo.fru.board.cust;
				break;
			case 'N': // prod-name
				fru_loadfield(fruinfo.fru.product.pname.val, optarg);
				fruinfo.has_product = true;
				break;
			case 'G': // prod-mfg
				fru_loadfield(fruinfo.fru.product.mfg.val, optarg);
				fruinfo.has_product = true;
				break;
			case 'M': // prod-modelpn
				fru_loadfield(fruinfo.fru.product.pn.val, optarg);
				fruinfo.has_product = true;
				break;
			case 'V': // prod-version
				fru_loadfield(fruinfo.fru.product.ver.val, optarg);
				fruinfo.has_product = true;
				break;
			case 'S': // prod-serial
				fru_loadfield(fruinfo.fru.product.serial.val, optarg);
				fruinfo.has_product = true;
				break;
			case 'F': // prod-file
				fru_loadfield(fruinfo.fru.product.file.val, optarg);
				fruinfo.has_product = true;
				break;
			case 'A': // prod-atag
				fru_loadfield(fruinfo.fru.product.atag.val, optarg);
				fruinfo.has_product = true;
				break;
			case 'P': // prod-custom
				debug(2, "Custom product field [%s]", optarg);
				fruinfo.has_product = true;
				custom = &fruinfo.fru.product.cust;
				break;
			case 'U': // All multi-record options must be listed here
			          // and processed later in a separate switch
				is_mr_record = true;
				break;

			case '?':
				exit(1);
			default:
				break;
		}

		if (is_mr_record) {
			fru_mr_reclist_t *mr_reclist_tail = add_reclist(&fruinfo.fru.mr_reclist);
			if (!mr_reclist_tail) {
				fatal("Failed to allocate multirecord area list");
			}
			fruinfo.has_multirec = true;

			switch(opt) {
				case 'U': // UUID
					errno = fru_mr_uuid2rec(&mr_reclist_tail->rec, optarg);
					if (errno) {
						fatal("Failed to convert UUID: %m");
					}
					break;
				default:
					fatal("Unknown multirecord option: %c", opt);
			}
		}

		if (custom) {
			fru_reclist_t *custptr;
			decoded_field_t *field = NULL;
			debug(3, "Adding a custom field from argument [%s]", optarg);
			custptr = add_reclist(custom);

			if (!custptr)
				fatal("Failed to allocate a custom record list entry");

			field = calloc(1, sizeof(*field));
			if (!field) {
				fatal("Failed to process custom field. Memory allocation or field length problem.");
			}
			field->type = FIELD_TYPE_AUTO;
			/* TODO: Allow all types in command line and use the same code
			 *       here as for JSON, ditch cust_binary. Use the same function. */
			if (cust_binary) {
				field->type = FIELD_TYPE_BINARY;
			}
			else {
				debug(3, "The custom field will be auto-typed");
			}
			strncpy(field->val, optarg, FRU_FIELDMAXLEN);
			custptr->rec = field;
			cust_binary = false;
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
		save_to_json_file(&fp, fname, &fruinfo, &config);
		free(fruinfo.fru.internal_use);
		fruinfo.fru.internal_use = NULL;
		break;
#endif
	case FRUGEN_FMT_TEXTOUT:
		save_to_text_file(&fp, fname, &fruinfo, &config);
		free(fruinfo.fru.internal_use);
		fruinfo.fru.internal_use = NULL;
		break;

	default:
	case FRUGEN_FMT_BINARY:
		save_to_binary_file(fname, &fruinfo, &config);

	}
}
