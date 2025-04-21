/** @file
 *  @brief FRU generator utility JSON support code
 *
 *  @copyright
 *  Copyright (C) 2016-2024 Alexander Amelkin <alexander@amelkin.msk.ru>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later OR Apache-2.0
 */


#include <assert.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>

#include <json-c/json.h>

#include "fru_errno.h"
#include "frugen-json.h"

#if (JSON_C_MAJOR_VERSION == 0 && JSON_C_MINOR_VERSION < 13)
#include <string.h>
static // Don't export the local definition
int
json_object_to_fd(int fd, struct json_object *obj, int flags)
{
	// implementation is copied from json-c v0.13 with minor refactoring
	int ret;
	const char *json_str;
	size_t pos, size;

	if (!(json_str = json_object_to_json_string_ext(obj, flags))) {
		return -1;
	}

	size = strlen(json_str);
	pos = 0;
	while(pos < size) {
		if((ret = write(fd, json_str + pos, size - pos)) < 0) {
			return -1;
		}

		/* because of the above check for ret < 0, we can safely add */
		pos += ret;
	}

	return 0;
}
#endif

static
bool load_single_field(fru_field_t * field, json_object * jsfield)
{
	bool rc = false;

	// jsfield is either an object or a string.
	// First assume it's an object with 'type' and 'data' fields.
	json_object *typefield, *valfield;
	const char * val;
	fru_field_enc_t encoding = FRU_FE_UNKNOWN;
	if (json_object_object_get_ex(jsfield, "type", &typefield) &&
	    json_object_object_get_ex(jsfield, "data", &valfield))
	{
		// expected subfields are type and val
		const char * type = json_object_get_string(typefield);
		val = json_object_get_string(valfield);
		encoding = frugen_enc_by_name(type);
		if (FRU_FE_UNKNOWN == encoding) {
			warn("Unknown encoding type '%s', using 'auto'", type);
			encoding = FRU_FE_AUTO;
		}
	} else {
		// Apparently, jsfield is not an object.
		// It must be a string then.
		encoding = FRU_FE_AUTO;
		val = json_object_get_string(jsfield);
		if (!val) {
			warn("Field is neither an object, nor a string");
			goto out;
		}
	}

	if (!fru_setfield(field, encoding, val)) {
		warn("Couldn't add field: %s", fru_strerr(fru_errno));
		goto out;
	}

	rc = true;
out:
	return rc;
}

static
bool load_info_fields(fru_t * fru, fru_area_type_t atype,
                      json_object *jso)
{
	bool rc = false;
	json_object *jsfield;
	const char * const jsnames[FRU_INFO_AREAS][FRU_MAX_FIELD_COUNT] = {
		[FRU_INFOIDX(CHASSIS)] = {
			"pn",
			"serial"
		},
		[FRU_INFOIDX(BOARD)] = {
			"mfg",
			"pname",
			"serial",
			"pn",
			"file"
		},
		[FRU_INFOIDX(PRODUCT)] = {
			"mfg",
			"pname",
			"pn",
			"ver",
			"serial",
			"atag",
			"file"
		}
	};

	/* First load mandatory fields */

	size_t field_idx = FRU_LIST_HEAD;
	fru_field_t * field;
	int infoidx = FRU_ATYPE_TO_INFOIDX(atype);
	while ((field = fru_getfield(fru, atype, field_idx))) {
		const char * jsname = jsnames[infoidx][field_idx];
		if (!json_object_object_get_ex(jso, jsname, &jsfield)) {
			debug(2, "Field '%s' not found for area '%s', skipping",
			      jsname, area_names[atype].json);
			goto nextloop;
		}

		if (!load_single_field(field, jsfield)) {
			warn("Failed to parse or add field '%s'", jsname);
			goto out;
		}

	nextloop:
		field_idx++;
		debug(2, "Field '%s' = '%s' (%s) loaded from JSON",
		      jsname, field->val, frugen_enc_name_by_val(field->enc));
	}

	/* Now load custom fields */

	json_object_object_get_ex(jso, "custom", &jsfield);
	if (!jsfield) {
		debug(2, "No custom field list provided");
		rc = true;
		goto out;
	}

	if (json_object_get_type(jsfield) != json_type_array) {
		warn("Field 'custom' is not a list object");
		return false;
	}

	size_t alen = json_object_array_length(jsfield);
	if (!alen)
		debug(1, "Custom list is present but empty");

	for (size_t i = 0; i < alen; i++) {
		fru_field_t field;
		json_object *item;

		item = json_object_array_get_idx(jsfield, i);
		if (!item)
			continue;

		if (!load_single_field(&field, item)) {
			warn("Failed to load custom field %zu", i);
			goto out;
		}

		if (!fru_add_custom(fru, atype, FRU_LIST_TAIL, field.enc, field.val)) {
			fru_warn("Failed to add custom field %zu", i);
			goto out;
		}

		debug(2, "Custom field %zu has been loaded from JSON", i);
	}

	rc = true;

out:
	return rc;
}

static
bool load_mr_mgmt_record(fru_t * fru,
                         size_t i,
                         json_object * item)
{
	bool rc = false;
	const char *subtype = NULL;
	fru_mr_mgmt_type_t subtype_id;
	json_object * ifield;

	json_object_object_get_ex(item, "subtype", &ifield);
	if (!ifield || !(subtype = json_object_get_string(ifield))) {
		warn("Each management record must have a subtype");
		goto out;
	}

	debug(3, "Management record %zu subtype is '%s'", i, subtype);
	subtype_id = frugen_mr_mgmt_type_by_name(subtype);
	if (!FRU_MR_MGMT_IS_SUBTYPE_VALID(subtype_id))
		goto out;

	debug(3, "Management record %zu subtype ID is '%d'", i, subtype_id);

	fru_mr_rec_t mr_rec = {};

	mr_rec.type = FRU_MR_MGMT_ACCESS;
	mr_rec.mgmt.subtype = subtype_id;

	json_object_object_get_ex(item, subtype, &ifield);
	if (!ifield) {
		warn("Field '%s' not found for record %zu data", subtype, i);
		goto out;
	}

	const char * field_data = json_object_get_string(ifield);
	if (!field_data) {
		warn("Field '%s' is not a string for MR record %zu", subtype, i);
		goto out;
	}
	strncpy(mr_rec.mgmt.data,
	        field_data,
	        FRU_MIN(sizeof(mr_rec.mgmt.data) - 1,
	                strlen(field_data)
	        )
	);

	/* Always add to the tail, one by one, sparse addition is not supported */
	if (!fru_add_mr(fru, FRU_LIST_TAIL, &mr_rec)) {
		fru_warn("Failed to add MR management record %zu", i);
		goto out;
	}

	rc = true;

out:
	return rc;
}

static
bool load_mr_record(fru_t * fru, size_t i, json_object * item)
{
	bool rc = false;

	const char *type = NULL;
	json_object *ifield;

	json_object_object_get_ex(item, "type", &ifield);
	if (!ifield || !(type = json_object_get_string(ifield))) {
		warn("Each multirecord area record must have a type specifier");
		goto out;
	}

	debug(3, "Record is of type '%s'", type);
	if (!strcmp(type, "management")) {
		if (!load_mr_mgmt_record(fru, i, item)) {
			goto out;
		}
	}
	else if (!strcmp(type, "custom")) {
		debug(1, "Found a custom MR record");
		int32_t custom_type = FRU_MR_EMPTY;
		json_object_object_get_ex(item, "custom_type", &ifield);
		if (!ifield) {
			warn("Each custom MR record must have "
			     "a 'custom_type' (0...255)");
			goto out;
		}

		custom_type = json_object_get_int(ifield);
		if (!FRU_MR_IS_VALID_TYPE(custom_type)) {
			warn("Custom type %" PRIi32 " for record %zu "
			     "is out of range (0...255)",
			     custom_type, i);
			goto out;
		}

		const char *hexstr = NULL;
		json_object_object_get_ex(item, "data", &ifield);
		hexstr = json_object_get_string(ifield);
		if (!ifield || !hexstr) {
			warn("A custom MR record %zu must have 'data' "
			     "field with a hex string", i);
		}

		fru_mr_rec_t mr_rec = {
			.type = FRU_MR_RAW,
			.raw.type = custom_type,
		};

		strncpy(mr_rec.raw.data,
			hexstr,
			FRU_MIN(sizeof(mr_rec.raw.data) - 1,
				strlen(hexstr)
			)
		);

		/* Always add to the tail, one by one, sparse addition is not supported */
		if (!fru_add_mr(fru, FRU_LIST_TAIL, &mr_rec)) {
			fru_warn("Failed to add a custom MR record");
			goto out;
		}

		debug(2, "Custom MR data loaded from JSON: %s", hexstr);
	}
/* TODO Fix this */
#if 0
	else if (!strcmp(type, "psu")) {
		debug(1, "Found a PSU info record (not yet supported, skipped)");
		continue;
	}
#endif
	else {
		warn("Multirecord type '%s' is not supported in JSON", type);
		goto out;
	}


	rc = true;
out:
	return rc;
}

static
bool load_mr_area(fru_t * fru, json_object * jso)
{
	(void)fru;
	bool rc = false;

	if (json_object_get_type(jso) != json_type_array) {
		warn("'multirec' object is not an array");
		goto out;
	}

	size_t alen = json_object_array_length(jso);
	if (!alen) {
		debug(1, "Multirecord area is an empty list");
		goto out;
	}

	for (size_t i = 0; i < alen; i++) {
		json_object *item;

		item = json_object_array_get_idx(jso, i);
		if (!item)
			continue;

		debug(3, "Parsing record #%zu/%zu", i + 1, alen);
		if (!load_mr_record(fru, i, item)) {
			warn("Failed to load MR record #%zu from JSON", i);
			goto out;
		}
	}

	rc = true;

out:
	return rc;
}

#if 0
/**
 * Allocate a multirecord area json object \a jso and build it from
 * the supplied multirecord area record list \a mr_reclist
 */
static
bool json_from_mr_reclist(json_object **jso,
                          const fru_mr_reclist_t *mr_reclist,
                          fru_flags_t flags)
{
}
#endif

static
bool load_info_area(fru_t * fru,
                             fru_area_type_t atype,
                             json_object * jso)
{
	if (!load_info_fields(fru, atype, jso)) {
		warn("Couldn't load standard or custom fields for %s",
		      area_names[atype].human);
		return false;
	}

	json_object * jsfield;
	if (FRU_CHASSIS_INFO == atype) {
		json_object_object_get_ex(jso, "type", &jsfield);
		if (jsfield) {
			errno = 0;
			fru->chassis.type = json_object_get_int(jsfield);
			if (errno)
				warn("chassis.type is not an integer, zeroed");

			debug(2, "Chassis type 0x%02X loaded from JSON",
			      fru->chassis.type);
		}
	}
	else if(FRU_BOARD_INFO == atype || FRU_PRODUCT_INFO == atype) {
		json_object_object_get_ex(jso, "lang", &jsfield);
		if (jsfield) {
			errno = 0;
			fru->board.lang = json_object_get_int(jsfield);
			if (errno) {
				warn("board.lang is not an integer, using English");
				fru->board.lang = FRU_LANG_ENGLISH;
			}

			debug(2, "Board language %d loaded from JSON",
			      fru->board.lang);
		}

		if (FRU_BOARD_INFO == atype) {
			json_object_object_get_ex(jso, "date", &jsfield);
			if (jsfield) {
				const char *s = json_object_get_string(jsfield);
				// get_string doesn't return any errors

				if (!datestr_to_tv(&fru->board.tv, s)) {
					warn("Invalid board date/time format in JSON file");
					return false;
				}

				fru->present[FRU_BOARD_INFO] = true;
				fru->board.tv_auto = false;

				debug(2, "Board date '%s' loaded from JSON", s);
			}
		}
	}
	return true;
}

void frugen_loadfile_json(fru_t * fru, const char * fname)
{
	bool success = false;
	json_object *jstree;

	debug(2, "Loading JSON from %s", fname);
	/* Allocate a new object and load contents from file */
	jstree = json_object_from_file(fname);
	if (NULL == jstree)
		fatal("Failed to load JSON FRU object from %s", fname);

	fru_area_type_t atype;
	FRU_FOREACH_AREA(atype) {
		json_object * jso;
		if (!json_object_object_get_ex(jstree, area_names[atype].json, &jso)) {
			debug(2, "%s Area ('%s') is not found in JSON",
			      area_names[atype].human, area_names[atype].json);
			continue;
		}

		// Add an area to the end of FRU file, that way we ensure
		// that areas in the output are in the same order as in the input JSON`
		fru_enable_area(fru, atype, FRU_APOS_LAST);
		debug(2, "Found %s Area in input template", area_names[atype].human);

		/* Intenal Use area needs special handling */
		if (FRU_INTERNAL_USE == atype) {
			const char *data = json_object_get_string(jso);
			if (!data) {
				debug(2, "Internal use are w/o data, skipping");
				continue;
			}

			if (!fru_set_internal_hexstring(fru, data)) {
				fru_warn("Failed to load internal use area");
				goto out;
			}

			goto nextloop;
		}

		if (FRU_IS_INFO_AREA(atype)) {
			if (!load_info_area(fru, atype, jso)) {
				warn("Incorrect definition of %s Area in input json",
				     area_names[atype].human);
				goto out;
			}

			goto nextloop;
		}

		/* Here it can only be FRU_MR */
		debug(2, "Processing multirecord area records");
		if (!load_mr_area(fru, jso))
			goto out;

		if (!fru->mr) {
			fru_disable_area(fru, FRU_MR);
			warn("Disabled an empty %s Area", area_names[atype].human);
		}

	nextloop:
		debug(2, "%s Area loaded from JSON", area_names[atype].human);
	}

	// Now as we've loaded everything, validate it by passing through
	// libfru encoder and decoder
	size_t fullsize = 0;
	uint8_t *frubuf = NULL;
	if (!fru_savebuffer((void **)&frubuf, &fullsize, fru)) {
		fru_warn("Failed to encode the loaded JSON");
		goto out;
	}
	fru_free(fru);
	fru = fru_loadbuffer(NULL, frubuf, fullsize, FRU_NOFLAGS);
	if (!fru) {
		fru_warn("Failed to decode the FRU encoded from JSON");
		goto out;
	}

	success = true;
out:
	
	if (!success) {
		if (FRU_IS_VALID_AREA(atype))
			fatal("Failed to load %s Area", area_names[atype].human);
		else
			fatal("Failed to load FRU from JSON file");
	}
	/* Deallocate the JSON object */
	json_object_put(jstree);
}


void add_iu_area_json(struct json_object * jso,
                      const char * internal)
{
	assert(internal);

	struct json_object *section = json_object_new_string(internal);
	json_object_object_add(jso, "internal", section);
}

static
bool add_info_field(struct json_object * jso,
                    const char * key,
                    const fru_field_t * field)
{
	struct json_object *string, *type_string, *entry;
	if ((string = json_object_new_string(field->val)) == NULL)
		goto STRING_ERR;

	if (field->enc == FRU_FE_AUTO) {
		entry = string;
	} else {
		const char *enc_name = frugen_enc_name_by_val(field->enc);
		if ((type_string = json_object_new_string(enc_name)) == NULL)
			goto TYPE_STRING_ERR;
		if ((entry = json_object_new_object()) == NULL)
			goto ENTRY_ERR;
		json_object_object_add(entry, "type", type_string);
		json_object_object_add(entry, "data", string);
	}
	if (key == NULL) {
		/* No key, make jso an array, used for multirecord */
		json_object_array_add(jso, entry);
	} else {
		/* Key is specified, add a named string or object */
		json_object_object_add(jso, key, entry);
	}
	return true;

ENTRY_ERR:
	json_object_put(type_string);
TYPE_STRING_ERR:
	json_object_put(string);
STRING_ERR:
	return false;
}


void add_info_area_json(struct json_object * jso,
                        fru_area_type_t atype,
                        const fru_t * fru)
{
	assert(fru);
	assert(jso);

	struct json_object * section = json_object_new_object();

	const fru_field_t * field = NULL;
	const char * const aname = area_names[atype].json;

	/* Add area-specific fields */
	struct json_object * tmp_obj = json_object_new_object();
	if (FRU_ATYPE_HAS_TYPE(atype)) {
		tmp_obj = json_object_new_int(fru->chassis.type);
		json_object_object_add(section, "type", tmp_obj);
	}
	else if (FRU_ATYPE_HAS_LANG(atype)) {
		section = json_object_new_object();
		tmp_obj = json_object_new_int(fru->product.lang);
		json_object_object_add(section, "lang", tmp_obj);
	}

	if (atype == FRU_BOARD_INFO) {
		/* Board has a date field */
		struct timeval tv = fru->board.tv;
		fru_field_t datefield;
		datefield.enc = FRU_FE_AUTO; // Ensure it's saved as a plain string

		if (fru->board.tv_auto) {
			strcpy(datefield.val, "auto");
		}
		else {
			tv_to_datestr(datefield.val, &tv);
		}
		if (datefield.val[0]) {
			section = json_object_new_object();
			add_info_field(section, "date", &datefield);
		}
	}

	/* Add standard fields */
	for (size_t i = 0; i < field_max[atype]; i++) {
		const char * const name = field_name[atype][i].json;
		field = fru_getfield(fru, atype, i);
		if (!field)
			fru_fatal("Failed to get standard field '%s' from '%s'", name, aname);

		if (!add_info_field(section, name, field))
			fatal("Failed to add field %s.%s to JSON", aname, name);
		debug(2, "Added %s.%s to JSON", aname, name);
	}

	/* Add custom fields */
	struct json_object * custom_array = json_object_new_array();
	size_t idx = FRU_LIST_HEAD;
	while ((field = fru_get_custom(fru, atype, idx))) {
		if (!add_info_field(custom_array, NULL, field))
			fatal("Failed to add field %s.custom.%zu to JSON", aname, idx);

		debug(2, "Added %s.custom.%zu to JSON", aname, idx);
		idx++;
	}
	if (fru_errno.code != FENOFIELD)
		fru_fatal("Failed to get custom fields");

	if (idx == FRU_LIST_HEAD) {
		/* The list is empty, don't add it */
		json_object_put(custom_array);
	}
	else {
		json_object_object_add(section, "custom", custom_array);
	}

	json_object_object_add(jso, aname, section);
}

void add_mr_record_json(struct json_object * jsa, fru_mr_rec_t * rec)
{
	struct json_object * js_rec = json_object_new_object();

	if (!js_rec)
		fatal("Failed to create a new JSON object for MR record");

	if (rec->type == FRU_MR_MGMT_ACCESS) {
		fru_mr_mgmt_type_t subtype = rec->mgmt.subtype;
		struct json_object * jsfield = NULL;
		off_t idx = FRU_MR_MGMT_SUBTYPE_TO_IDX(subtype);
		const char * recname = NULL;

		if (idx < 0) {
			json_object_put(js_rec);
			fatal("Invalid management access record subtype %d", subtype);
		}

		recname = frugen_mr_mgmt_name[idx].json;

		jsfield = json_object_new_string("management");
		json_object_object_add(js_rec, "type", jsfield);

		jsfield = json_object_new_string(recname);
		json_object_object_add(js_rec, "subtype", jsfield);

		jsfield = json_object_new_string(rec->mgmt.data);
		json_object_object_add(js_rec, recname, jsfield);
	}
/* TODO: Add more MR types
	else if (rec->type = ... ) {
		// Add code here
	}
*/
	else if (rec->type == FRU_MR_RAW) {
		uint8_t raw_type = rec->raw.type;
		struct json_object * jsfield = NULL;

		jsfield = json_object_new_string("custom");
		json_object_object_add(js_rec, "type", jsfield);

		jsfield = json_object_new_int(raw_type);
		json_object_object_add(js_rec, "custom_type", jsfield);

		jsfield = json_object_new_string(rec->raw.data);
		json_object_object_add(js_rec, "data", jsfield);
	}

	json_object_array_add(jsa, js_rec);
}

void add_mr_area_json(struct json_object * jso,
                      const fru_t * fru)
{
	struct json_object * js_mr = json_object_new_array();
	if (!js_mr)
		fatal("Failed to allocate a new JSON array for multirecord area\n");

	/* TODO: Add each MR record */
	fru_mr_rec_t *rec = NULL;
	size_t count = 0;
	while (true) {
		bool last = false;
		fru_clearerr();
		rec = fru_get_mr(fru, FRU_LIST_HEAD);
		last = (fru_errno.code == FEMREND);

		if (!rec) {
			break;
		}

		add_mr_record_json(js_mr, rec);
		count++;

		if (last)
			break;
	}

	if (count) {
		json_object_object_add(jso, "multirecord", js_mr);
		debug(2, "Added multirecord area to JSON");
	}
	else {
		json_object_put(js_mr);
	}

}

void save_to_json_file(FILE **fp, const char *fname,
                       const fru_t * fru)
{
	struct json_object *json_root = json_object_new_object();

	assert(fp);
	assert(fru);

	if (!*fp) {
		*fp = fopen(fname, "w");
	}

	if (!*fp) {
		fatal("Failed to open file '%s' for writing: %m", fname);
	}

	/* Write areas out in the requested order */
	fru_area_type_t order;
	FRU_FOREACH_AREA(order) {
		fru_area_type_t atype = fru->order[order];

		/* Skip disabled areas */
		if (!fru->present[atype])
			continue;

		switch (atype) {
		case FRU_INTERNAL_USE:
			add_iu_area_json(json_root, fru->internal);
			break;
		case FRU_CHASSIS_INFO:
		case FRU_BOARD_INFO:
		case FRU_PRODUCT_INFO:
			add_info_area_json(json_root, atype, fru);
			break;
		case FRU_MR:
			add_mr_area_json(json_root, fru);
			break;
		default:
			fatal("Invalid area (%d) in save order!", atype);
		}
	}

	/* Write out the json tree to file */
#ifndef JSON_C_TO_STRING_NOSLASHESCAPE
#define JSON_C_TO_STRING_NOSLASHESCAPE 0 // Not supported, ignore
#endif

	json_object_to_fd(fileno(*fp), json_root,
	                  JSON_C_TO_STRING_PRETTY
	                  | JSON_C_TO_STRING_SPACED
	                  | JSON_C_TO_STRING_NOSLASHESCAPE
	);
	json_object_put(json_root);

	return;
}

