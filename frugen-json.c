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
	bool rc = false;
	json_object *mr_jso = NULL;
	const fru_mr_reclist_t *item = mr_reclist;

	if (!mr_reclist)
		goto out;

	if (!jso)
		goto out;

	mr_jso = json_object_new_array();
	if (!mr_jso) {
		printf("Failed to allocate a new JSON array for multirecord area\n");
		goto out;
	}

	if (json_object_get_type(mr_jso) != json_type_array)
		goto out;

	while (item) {
		fru_mr_rec_t *rec = item->rec;
		size_t key_count = 0;
		bool mr_valid = false;
		// Pointers to allocated strings, depending on the found record
#define MAX_MR_KEYS 5 // Index 0 is always 'type', the rest depends on the type
		union { char *str; int num; } values[MAX_MR_KEYS] = {};
		char *keys[MAX_MR_KEYS] = {};
		enum { MR_TYPE_STR, MR_TYPE_INT } types[MAX_MR_KEYS] = {};
		switch (rec->hdr.type_id) {
			case FRU_MR_MGMT_ACCESS: {
				bool mgmt_valid = true;
				fru_mr_mgmt_rec_t *mgmt = (fru_mr_mgmt_rec_t *)rec;
				int rc;
				/* We need to allocate keys entries */
				sscanf("management", "%ms", &values[0].str); // keys[0] allocated later
				sscanf("subtype", "%ms", &keys[1]); // values[1] allocated later
#define MGMT_TYPE_ID(type) ((type) - FRU_MR_MGMT_MIN)
				switch (mgmt->subtype) {
				case FRU_MR_MGMT_SYS_UUID:
					if ((rc = fru_mr_rec2uuid(&values[2].str, mgmt, flags))) {
						printf("Could not decode the UUID record: %s\n",
						       strerror(-rc));
						break;
					}
					break;
				case FRU_MR_MGMT_SYS_URL:
				case FRU_MR_MGMT_SYS_NAME:
				case FRU_MR_MGMT_SYS_PING:
				case FRU_MR_MGMT_COMPONENT_URL:
				case FRU_MR_MGMT_COMPONENT_NAME:
				case FRU_MR_MGMT_COMPONENT_PING:
					if ((rc = fru_mr_mgmt_rec2str(&values[2].str, mgmt, flags))) {
						char *subtype = fru_mr_mgmt_name_by_type(mgmt->subtype);
						printf("Could not decode the Mgmt Access record '%s': %s\n",
						       subtype,
						       strerror(-rc));
						free(subtype);
						break;
					}
					break;
				default:
					debug(1, "Multirecord Management subtype 0x%02X is not yet supported",
					      mgmt->subtype);
					mgmt_valid = false;
				}

				if (mgmt_valid) {
					/* Two calls to make it allocated twice for uniform deallocation later */
					values[1].str = fru_mr_mgmt_name_by_type(mgmt->subtype);
					keys[2] = fru_mr_mgmt_name_by_type(mgmt->subtype);
					key_count = 3;
					mr_valid = true;
				}
				break;
			}
			case FRU_MR_PSU_INFO:
				debug(1, "Found a PSU info record (not yet supported, skipped)");
				break;
			default: {
				debug(1, "Multirecord type 0x%02X is not yet supported, decoding as 'custom'",
				      rec->hdr.type_id);
				int rc;
				sscanf("custom", "%ms", &values[0].str); // keys[0] allocated later
				sscanf("custom_type", "%ms", &keys[1]); // values[1] allocated later
				types[1] = MR_TYPE_INT;
				values[1].num = rec->hdr.type_id;

				sscanf("data", "%ms", &keys[2]); // values[1] allocated later
				rc = fru_mr_rec2hexstr(&values[2].str, rec, flags);
				if (rc) {
					fatal("Could not decode as a custom record: %s\n", strerror(-rc));
					break;
				}
				mr_valid = true;
				key_count = 3;
				debug(1, "Custom MR record decoded: %s", values[2].str);
				break;
			}

		}

		if (mr_valid && key_count) {
			struct json_object *entry;
			size_t j = 0;

			if ((entry = json_object_new_object()) == NULL)
				fatal("Failed to allocate a new JSON entry for MR record");

			sscanf("type", "%ms", &keys[0]); // This is common for all valid MR records
			for (; j < key_count; j++) {
				struct json_object *vobj = NULL;

				if (!keys[j] || (MR_TYPE_STR == types[j] && !values[j].str))
					fatal("Internal error. Required key or value not found. Please report.");

				switch (types[j]) {
					case MR_TYPE_STR:
						vobj = json_object_new_string((const char *)values[j].str);
						free(values[j].str);
						values[j].str = NULL;
						break;
					case MR_TYPE_INT:
						vobj = json_object_new_int((int32_t)values[j].num);
						values[j].num = 0;
						break;
					default:
						break;
				}
				if (!vobj)
					fatal("Failed to allocate a JSON object for value of '%s'", keys[j]);

				json_object_object_add(entry, keys[j], vobj);
				free(keys[j]);
				keys[j] = NULL;
			}
			if (j && j == key_count) {
				json_object_array_add(mr_jso, entry);
			}
		}
		item = item->next;
	}

	rc = true;
out:
	if (!rc && mr_jso) {
		json_object_put(mr_jso);
		mr_jso = NULL;
	}
	*jso = mr_jso;
	return rc;
}
#endif

#if 0
static
int json_object_add_with_type(struct json_object* obj,
                              const char* key,
                              const char* val,
                              int type)
{
	struct json_object *string, *type_string, *entry;
	if ((string = json_object_new_string((const char *)val)) == NULL)
		goto STRING_ERR;

	if (type == FRU_FE_AUTO) {
		entry = string;
	} else {
		const char *enc_name = frugen_enc_name_by_val(type);
		if ((type_string = json_object_new_string(enc_name)) == NULL)
			goto TYPE_STRING_ERR;
		if ((entry = json_object_new_object()) == NULL)
			goto ENTRY_ERR;
		json_object_object_add(entry, "type", type_string);
		json_object_object_add(entry, "data", string);
	}
	if (key == NULL) {
		return json_object_array_add(obj, entry);
	} else {
		json_object_object_add(obj, key, entry);
		return 0;
	}

ENTRY_ERR:
	json_object_put(type_string);
TYPE_STRING_ERR:
	json_object_put(string);
STRING_ERR:
	return -1;
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

#if 0
void save_to_json_file(FILE **fp, const char *fname,
                       const struct frugen_fruinfo_s *info,
                       const struct frugen_config_s *config)
{
	struct json_object *json_root = json_object_new_object();
	struct json_object *section = NULL, *temp_obj = NULL;

	assert(fp);

	if (!*fp) {
		*fp = fopen(fname, "w");
	}

	if (!*fp) {
		fatal("Failed to open file '%s' for writing: %m", fname);
	}

	if (info->has_internal) {
		section = json_object_new_string((char *)fru->internal_use);
		json_object_object_add(json_root, "internal", section);
	}

	if (info->has_chassis) {
		section = json_object_new_object();
		temp_obj = json_object_new_int(fru->chassis.type);
		json_object_object_add(section, "type", temp_obj);
		json_object_add_with_type(section, "pn", fru->chassis.pn.val, fru->chassis.pn.type);
		json_object_add_with_type(section, "serial", fru->chassis.serial.val, fru->chassis.serial.type);
		temp_obj = json_object_new_array();
		fru_reclist_t *next = fru->chassis.cust;
		while (next != NULL) {
			json_object_add_with_type(temp_obj, NULL, next->rec->val,
									  next->rec->type);
			next = next->next;
		}
		json_object_object_add(section, "custom", temp_obj);
		json_object_object_add(json_root, "chassis", section);
	}

	if (info->has_product) {
		section = json_object_new_object();
		temp_obj = json_object_new_int(fru->product.lang);
		json_object_object_add(section, "lang", temp_obj);
		json_object_add_with_type(section, "mfg", fru->product.mfg.val, fru->product.mfg.type);
		json_object_add_with_type(section, "pname", fru->product.pname.val, fru->product.pname.type);
		json_object_add_with_type(section, "serial", fru->product.serial.val, fru->product.serial.type);
		json_object_add_with_type(section, "pn", fru->product.pn.val, fru->product.pn.type);
		json_object_add_with_type(section, "ver", fru->product.ver.val, fru->product.ver.type);
		json_object_add_with_type(section, "atag", fru->product.atag.val, fru->product.atag.type);
		json_object_add_with_type(section, "file", fru->product.file.val, fru->product.file.type);
		temp_obj = json_object_new_array();
		fru_reclist_t *next = fru->product.cust;
		while (next != NULL) {
			json_object_add_with_type(temp_obj, NULL, next->rec->val,
									  next->rec->type);
			next = next->next;
		}
		json_object_object_add(section, "custom", temp_obj);
		json_object_object_add(json_root, "product", section);
	}

	if (info->has_board) {
		char timebuf[DATEBUF_SZ] = {0};
		tv_to_datestr(timebuf, &fru->board.tv);

		section = json_object_new_object();
		temp_obj = json_object_new_int(fru->board.lang);
		json_object_object_add(section, "lang", temp_obj);
		json_object_add_with_type(section, "date", timebuf, 0);
		json_object_add_with_type(section, "mfg", fru->board.mfg.val, fru->board.mfg.type);
		json_object_add_with_type(section, "pname", fru->board.pname.val, fru->board.pname.type);
		json_object_add_with_type(section, "serial", fru->board.serial.val, fru->board.serial.type);
		json_object_add_with_type(section, "pn", fru->board.pn.val, fru->board.pn.type);
		json_object_add_with_type(section, "file", fru->board.file.val, fru->board.file.type);
		temp_obj = json_object_new_array();
		fru_reclist_t *next = fru->board.cust;
		while (next != NULL) {
			json_object_add_with_type(temp_obj, NULL, next->rec->val,
									  next->rec->type);
			next = next->next;
		}
		json_object_object_add(section, "custom", temp_obj);
		json_object_object_add(json_root, "board", section);
	}
	if (info->has_multirec) {
		json_object *jso = NULL;
		json_from_mr_reclist(&jso, fru->mr_reclist, config->flags);
		json_object_object_add(json_root, "multirecord", jso);
	}
	json_object_to_fd(fileno(*fp), json_root,
					  JSON_C_TO_STRING_PRETTY | JSON_C_TO_STRING_SPACED);
	json_object_put(json_root);
}
#endif
