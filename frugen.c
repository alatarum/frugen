/** @file
 *  @brief FRU generator utility
 *
 *  Copyright (C) 2016-2024 Alexander Amelkin <alexander@amelkin.msk.ru>
 *  SPDX-License-Identifier: GPL-2.0-or-later OR Apache-2.0
 */
#ifndef VERSION
// If VERSION is not defined, that means something is wrong with CMake
#define VERSION "BROKEN"
#endif

#define COPYRIGHT_YEARS "2016-2025"
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
#include "fru-errno.h"
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

char * fru_mr_mgmt_name_by_type(fru_mr_mgmt_type_t type)
{
	char *str = NULL;
	if (type < FRU_MR_MGMT_MIN || type > FRU_MR_MGMT_MAX) {
		fatal("FRU MR Management Record type %d is out of range", type);
	}
	sscanf(fru_mr_mgmt_name[MGMT_TYPE_ID(type)], "%ms", &str);
	return str;
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

#define DATEBUF_SZ 20
void tv_to_datestr(char *datestr, const struct timeval *tv)
{
		tzset(); // Set up local timezone
		struct tm bdtime;
		// Time in FRU is in UTC, convert to local
		time_t seconds = tv->tv_sec - timezone;
		localtime_r(&seconds, &bdtime);
		strftime(datestr, 20, "%d/%m/%Y %H:%M:%S", &bdtime);
}

fieldopt_t arg_to_fieldopt(char *arg)
{
	fieldopt_t opt = { .type = FIELD_TYPE_PRESERVE };

	const char *area_type[FRU_MAX_AREAS] = {
		// [ FRU_INTERNAL_USE ] = "internal", // Not supported
		[ FRU_CHASSIS_INFO ] = "chassis",
		[ FRU_BOARD_INFO ] = "board",
		[ FRU_PRODUCT_INFO ] = "product",
		// [ FRU_MULTIRECORD ] = "multirecord" // Not supported
	};
	const char *chassis_fields[FRU_CHASSIS_FIELD_COUNT] = {
		[FRU_CHASSIS_PARTNO] = "pn",
		[FRU_CHASSIS_SERIAL] = "serial",
	};
	const char *board_fields[FRU_BOARD_FIELD_COUNT] = {
		[FRU_BOARD_MFG] = "mfg",
		[FRU_BOARD_PRODNAME] = "pname",
		[FRU_BOARD_SERIAL] = "serial",
		[FRU_BOARD_PARTNO] = "pn",
		[FRU_BOARD_FILE] = "file",
	};
	const char *product_fields[FRU_PROD_FIELD_COUNT] = {
		[FRU_PROD_MFG] = "mfg",
		[FRU_PROD_NAME] = "pname",
		[FRU_PROD_MODELPN] = "pn",
		[FRU_PROD_VERSION] = "version",
		[FRU_PROD_SERIAL] = "serial",
		[FRU_PROD_ASSET] = "atag",
		[FRU_PROD_FILE] = "file",
	};
	char *p;
	int field_max[FRU_MAX_AREAS] = {
		[FRU_CHASSIS_INFO] = FRU_CHASSIS_FIELD_COUNT,
		[FRU_BOARD_INFO] = FRU_BOARD_FIELD_COUNT,
		[FRU_PRODUCT_INFO] = FRU_PROD_FIELD_COUNT,
	};
	const char **fields[FRU_MAX_AREAS] = {
		[FRU_CHASSIS_INFO] = chassis_fields,
		[FRU_BOARD_INFO] = board_fields,
		[FRU_PRODUCT_INFO] = product_fields,
	};

	/* Check if there is an encoding specifier */
	p = strchr(arg, ':');
	if (p) {
		*p = 0;
		debug(3, "Encoding specifier found");
		opt.type = FIELD_TYPE_AUTO;
		if (p != arg) {
			opt.type = fru_enc_type_by_name(arg);
			debug(2, "Encoding requested is '%s'", arg);
			debug(2, "Encoding parsed is '%s'", fru_enc_name_by_type(opt.type));
			if (FIELD_TYPE_UNKNOWN == opt.type) {
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

	for (opt.area = FRU_MAX_AREAS - 1; opt.area > FRU_AREA_NOT_PRESENT; opt.area--) {
		if (!area_type[opt.area]) continue;
		if (!strcmp(arg, area_type[opt.area]))
			break;
	}
	if (opt.area == FRU_AREA_NOT_PRESENT) {
		fatal("Area name '%s' is not valid", arg);
	}
	arg = p + 1;

	/* Now check if there is value */
	p = strchr(arg, '=');
	if ((p && arg == p) || (!p && !strlen(arg))) {
		fatal("Must specify field name for area '%s'", area_type[opt.area]);
	}
	if (!p) {
		fatal("Must specify value for '%s.%s'",
		      area_type[opt.area], arg);
	}
	*p = 0;

#define FRU_FIELD_NOT_PRESENT (-1)
	if (!field_max[opt.area]) {
		fatal("No fields are settable for area '%s'",
			  area_type[opt.area]);
	}
	for (opt.field.index = field_max[opt.area] - 1;
		 opt.field.index > FRU_FIELD_NOT_PRESENT; opt.field.index--)
	{
		if (!strcmp(arg, fields[opt.area][opt.field.index]))
			break;
	}
	if (opt.field.index == FRU_FIELD_NOT_PRESENT) {
		/* No standard field found, but it still can be a custom
		 * field specifier in form 'custom.<N>'
		 */
		if (!strncmp(arg, "custom", 6)) { /* It IS a custome field! */
			char *p2;
			opt.field.index = FRU_FIELD_CUSTOM;
			p2 = strchr(arg, '.');
			if (p2)
				opt.custom_index = atoi(p2 + 1);
			if (opt.custom_index < 0)
				fatal("Custom field index must be positive or zero");
		}
		else {
			fatal("Field '%s' doesn't exist in area '%s'",
			      arg, area_type[opt.area]);
		}
	}
	opt.value = p + 1;
	debug(2, "Field '%s' is being set in '%s' to '%s'",
	         opt.field.index == FRU_FIELD_CUSTOM
	                            ? "custom"
	                            : fields[opt.area][opt.field.index],
	         area_type[opt.area],
	         opt.value);


	return opt;
}

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

	debug(2, "Reading the template file of size %jd...", (intmax_t)statbuf.st_size);
	off_t total_read = 0;
	off_t bytes_read;
	do
	{
		bytes_read = read(fd, buffer + total_read, statbuf.st_size - total_read);
		if (bytes_read < 0) {
			fatal("Error (%d) reading template file: %s", errno, strerror(errno));
		}
		else {
			debug(3, "Read %jd bytes from file (%jd/%jd)",
			      (intmax_t)bytes_read,
			      (intmax_t)total_read,
			      (intmax_t)statbuf.st_size);
		}
		total_read += bytes_read;
	} while(total_read < statbuf.st_size && bytes_read);

	if(total_read < statbuf.st_size) {
		warn("File was shorter than expected (%jd / %jd)",
		     (intmax_t)total_read,
		     (intmax_t)statbuf.st_size);
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
		debug(2, "No internal use area found: %s", fru_strerr(fru_errno));
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
		debug(2, "No chassis area found: %s", fru_strerr(fru_errno));
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
		debug(2, "No multirecord area found: %s", fru_strerr(fru_errno));
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
			fatal("Failed to encode internal use area: %s\n",
				  fru_strerr(fru_errno));
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
		tv_to_datestr(timebuf, &info->fru.board.tv);

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
			fatal("Failed to encode internal use area: %s\n",
				  fru_strerr(fru_errno));
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
			fatal("Error allocating a chassis info area: %s",
			      fru_strerr(fru_errno));
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
			fatal("Error allocating a board info area: %s",
			      fru_strerr(fru_errno));
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
			fatal("Error allocating a product info area: %s",
			      fru_strerr(fru_errno));
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
			fatal("Error allocating a multirecord area: %s",
			      fru_strerr(fru_errno));
		}
	}

	fru = fru_create(info->areas, &size);
	if (!fru) {
		fatal("Error allocating a FRU file buffer: %s",
		      fru_strerr(fru_errno));
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
	bool single_option_help = false;
	fieldopt_t fieldopt = {};

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

	const char *fname = NULL;

	tzset();
	gettimeofday(&fruinfo.fru.board.tv, NULL);
	fruinfo.fru.board.tv.tv_sec += timezone;

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
	const char *option_help[] = {
		['d'] = "Set board manufacturing date/time, use \"DD/MM/YYYY HH:MM:SS\" format.\n\t\t"
		        "By default the current system date/time is used unless -u is specified",
		['g'] = "Set debug flag (use multiple times for multiple flags):\n\t\t"
		        "\tfver  - Ignore wrong version in FRU header\n\t\t"
			    "\taver  - Ignore wrong version in area headers\n\t\t"
			    "\trver  - Ignore wrong verison in multirecord area record version\n\t\t"
			    "\tasum  - Ignore wrong area checksum (for standard areas)\n\t\t"
			    "\trhsum - Ignore wrong record header checksum (for multirecord)\n\t\t"
			    "\trdsum - Ignore wrong data checksum (for multirecord)\n\t\t"
			    "\trend  - Ignore missing EOL record, use any found records",
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
		['U'] = "Set System Unique ID (UUID/GUID)\n\t\t"
		        "NOTE: This does NOT replace the data specified in the template",
		['v'] = "Increase program verbosity (debug) level",
	};

	char optstring[ARRAY_SZ(options) * 2 + 1] = {0};

	for (i = 0; i < ARRAY_SZ(options); ++i) {
		static int k = 0;
		optstring[k++] = options[i].val;
		if (options[i].has_arg)
			optstring[k++] = ':';
		if (options[i].has_arg == optional_argument)
			optstring[k++] = ':';
	}

	/* Process command line options */
	do {
		bool is_mr_record = false; // The current option is an MR area record
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
				printf("FRU Generator v%s (C) %s, "
					   "Alexander Amelkin <alexander@amelkin.msk.ru>\n",
					   VERSION, COPYRIGHT_YEARS);
				printf("\n"
					   "Usage: frugen [options] <filename>\n"
					   "\n"
					   "Options:\n\n");
				for (i = 0; i < ARRAY_SZ(options); i++) {
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
				if (single_option_help && i == ARRAY_SZ(options)) {
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
				debug(2, "Will load FRU information from file %s", optarg);
				load_fromfile(optarg, &config, &fruinfo);
				break;
#endif

			case 'r': // raw binary
				config.format = FRUGEN_FMT_BINARY;
				debug(1, "Using RAW binary input format");
				debug(2, "Will load FRU information from file %s", optarg);
				load_fromfile(optarg, &config, &fruinfo);
				break;

			case 'o': { // out-format
				const char *outfmt[] = {
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
				decoded_field_t * const field[FRU_MAX_AREAS][FRU_MAX_FIELD_COUNT] = {
					[FRU_CHASSIS_INFO] = {
						[FRU_CHASSIS_PARTNO] = &fruinfo.fru.chassis.pn,
						[FRU_CHASSIS_SERIAL] = &fruinfo.fru.chassis.serial,
					},
					[FRU_BOARD_INFO] = {
						[FRU_BOARD_MFG] = &fruinfo.fru.board.mfg,
						[FRU_BOARD_PRODNAME] = &fruinfo.fru.board.pname,
						[FRU_BOARD_SERIAL] = &fruinfo.fru.board.serial,
						[FRU_BOARD_PARTNO] = &fruinfo.fru.board.pn,
						[FRU_BOARD_FILE] = &fruinfo.fru.board.file,
					},
					[FRU_PRODUCT_INFO] = {
						[FRU_PROD_MFG] = &fruinfo.fru.product.mfg,
						[FRU_PROD_NAME] = &fruinfo.fru.product.pname,
						[FRU_PROD_MODELPN] = &fruinfo.fru.product.pn,
						[FRU_PROD_VERSION] = &fruinfo.fru.product.ver,
						[FRU_PROD_SERIAL] = &fruinfo.fru.product.serial,
						[FRU_PROD_ASSET] = &fruinfo.fru.product.atag,
						[FRU_PROD_FILE] = &fruinfo.fru.product.file,
					},
				};
				fru_reclist_t ** const custom[FRU_MAX_AREAS] = {
					[FRU_CHASSIS_INFO] = &fruinfo.fru.chassis.cust,
					[FRU_BOARD_INFO] = &fruinfo.fru.board.cust,
					[FRU_PRODUCT_INFO] = &fruinfo.fru.product.cust,
				};
				bool * const fru_has[FRU_MAX_AREAS] = {
					[FRU_CHASSIS_INFO] = &fruinfo.has_chassis,
					[FRU_BOARD_INFO] = &fruinfo.has_board,
					[FRU_PRODUCT_INFO] = &fruinfo.has_product,
				};

				/* Now do the actual job and set data in the appropriate locations */
				fieldopt = arg_to_fieldopt(optarg);
				if (fieldopt.field.index != FRU_FIELD_CUSTOM) {
					fru_loadfield(field[fieldopt.area][fieldopt.field.index],
					              fieldopt.value, fieldopt.type);
					*fru_has[fieldopt.area] = true;
				}
				else {
					fru_reclist_t *cust_rec;
					if (fieldopt.custom_index) {
						cust_rec = find_rec(*custom[fieldopt.area],
						                    fieldopt.custom_index);
						if (!cust_rec) {
							fatal("Custom field %d not found in specified area\n",
							      fieldopt.custom_index);
						}
						debug(3, "Modifying custom field %d. New value is [%s]",
						         fieldopt.custom_index, fieldopt.value);
					}
					else {
						decoded_field_t *field;
						debug(3, "Adding a custom field from argument [%s]", optarg);
						cust_rec = add_reclist(custom[fieldopt.area]);
						if (!cust_rec) {
							fatal("Failed to allocate a custom record list entry");
						}
						field = calloc(1, sizeof(*field));
						if (!field) {
							fatal("Failed to process custom field. Memory allocation or field length problem.");
						}
						cust_rec->rec = field;
					}
					fru_loadfield(cust_rec->rec, fieldopt.value, fieldopt.type);
					*fru_has[fieldopt.area] = true;
				}
				break;
			}

			case 't': // chassis-type
				fruinfo.fru.chassis.type = strtol(optarg, NULL, 16);
				debug(2, "Chassis type will be set to 0x%02X from [%s]", fruinfo.fru.chassis.type, optarg);
				fruinfo.has_chassis = true;
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
						fatal("Failed to convert UUID: %s",
						      fru_strerr(fru_errno));
					}
					break;
				default:
					fatal("Unknown multirecord option: %c", opt);
			}
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
