/** @file
 *  @brief FRU generator utility JSON support code
 *
 *  @copyright
 *  Copyright (C) 2016-2024 Alexander Amelkin <alexander@amelkin.msk.ru>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later OR Apache-2.0
 */

#include <assert.h>
#include <time.h>
#include <limits.h>
#include <stdbool.h>

#include <json-c/json.h>

#include "fru-errno.h"
#include "frugen-json.h"

#if (JSON_C_MAJOR_VERSION == 0 && JSON_C_MINOR_VERSION < 13)
int
json_object_to_fd(int fd, struct json_object *obj, int flags)
{
	// implementation is copied from json-c v0.13
	int ret;
	const char *json_str;
	unsigned int wpos, wsize;

	if (!(json_str = json_object_to_json_string_ext(obj, flags))) {
		return -1;
	}

	/* CAW: probably unnecessary, but the most 64bit safe */
	wsize = (unsigned int)(strlen(json_str) & UINT_MAX);
	wpos = 0;
	while(wpos < wsize) {
		if((ret = write(fd, json_str + wpos, wsize-wpos)) < 0) {
			return -1;
		}

		/* because of the above check for ret < 0, we can safely cast and add */
		wpos += (unsigned int)ret;
	}

	return 0;
}
#endif

static
bool json_fill_fru_area_fields(json_object *jso, int count,
                               const char *fieldnames[],
                               fru_field_t *fields[])
{
	int i;
	json_object *jsfield;
	bool data_in_this_area = false;
	for (i = 0; i < count; i++) {
		if (json_object_object_get_ex(jso, fieldnames[i], &jsfield)) {
			// field is an object
			json_object *typefield, *valfield;
			if (json_object_object_get_ex(jsfield, "type", &typefield) &&
			    json_object_object_get_ex(jsfield, "data", &valfield)) {
				// expected subfields are type and val
				const char *type = json_object_get_string(typefield);
				const char *val = json_object_get_string(valfield);
				field_type_t field_type;
				field_type = frugen_enc_type_by_name(type);
				if (FIELD_TYPE_UNKNOWN == field_type) {
					debug(1, "Unknown type %s for field '%s'",
					      type, fieldnames[i]);
					continue;
				}
				fru_loadfield(fields[i], val, field_type);
				debug(2, "Field %s '%s' (%s) loaded from JSON",
				      fieldnames[i], val, type);
				data_in_this_area = true;
			} else {
				const char *s = json_object_get_string(jsfield);
				debug(2, "Field %s '%s' loaded from JSON",
				      fieldnames[i], s);
				fru_loadfield(fields[i], s, FIELD_TYPE_AUTO);
				data_in_this_area = true;
			}
		}
	}

	return data_in_this_area;
}

static
bool json_fill_fru_area_custom(json_object *jso, fru_reclist_t **custom)
{
	int i, alen;
	json_object *jsfield;
	bool data_in_this_area = false;
	fru_reclist_t *custptr;

	if (!custom)
		return false;

	json_object_object_get_ex(jso, "custom", &jsfield);
	if (!jsfield)
		return false;

	if (json_object_get_type(jsfield) != json_type_array)
		return false;

	alen = json_object_array_length(jsfield);
	if (!alen)
		return false;

	for (i = 0; i < alen; i++) {
		const char *type = NULL;
		const void *data = NULL;
		fru_field_t *field = NULL;
		json_object *item, *ifield;

		item = json_object_array_get_idx(jsfield, i);
		if (!item) continue;

		json_object_object_get_ex(item, "type", &ifield);
		if (!ifield || !(type = json_object_get_string(ifield))) {
			debug(3, "Using automatic text encoding for custom field %d", i);
			type = "auto";
		}

		json_object_object_get_ex(item, "data", &ifield);
		if (!ifield || !(data = json_object_get_string(ifield))) {
			debug(3, "Emtpy data or no data at all found for custom field %d", i);
			continue;
		}

		debug(4, "Custom pointer before addition was %p", *custom);
		custptr = add_reclist(custom);
		debug(4, "Custom pointer after addition is %p", *custom);

		if (!custptr)
			return false;

		field = calloc(1, sizeof(*field));
		if (!field) {
			free_reclist(custptr);
			return false;
		}
		field->type = FIELD_TYPE_AUTO;
		if (!strcmp("binary", type)) {
			field->type = FIELD_TYPE_BINARY;
		} else if (!strcmp("bcdplus", type)) {
			field->type = FIELD_TYPE_BCDPLUS;
		} else if (!strcmp("6bitascii", type)) {
			field->type = FIELD_TYPE_6BITASCII;
		} else if (!strcmp("text", type)) {
			field->type = FIELD_TYPE_TEXT;
		} else {
			debug(3, "The custom field will be auto-typed");
		}
		strncpy(field->val, data, FRU_FIELDMAXLEN);
		custptr->rec = field;

		if (!custptr->rec) {
			fatal("Failed to encode custom field. Memory allocation or field length problem.");
		}
		debug(2, "Custom field %i has been loaded from JSON at %p->rec = %p", i, *custom, (*custom)->rec);
		data_in_this_area = true;
	}

	custptr = *custom;
	debug(4, "Walking all custom fields from %p...", custptr);
	while(custptr) {
		debug(4, "Custom %p, next %p", custptr, custptr->next);
		custptr = custptr->next;
	}

	return data_in_this_area;
}

static
bool json_fill_fru_mr_reclist(json_object *jso, fru_mr_reclist_t **mr_reclist)
{
	int i, alen;
	fru_mr_reclist_t *mr_reclist_tail;
	bool has_multirec = false;

	if (!mr_reclist)
		goto out;

	if (json_object_get_type(jso) != json_type_array)
		goto out;

	alen = json_object_array_length(jso);
	if (!alen)
		goto out;

	debug(4, "Multirecord area record list is initially at %p", *mr_reclist);

	for (i = 0; i < alen; i++) {
		const char *type = NULL;
		json_object *item, *ifield;

		item = json_object_array_get_idx(jso, i);
		if (!item) continue;

		debug(3, "Parsing record #%d/%d", i + 1, alen);

		json_object_object_get_ex(item, "type", &ifield);
		if (!ifield || !(type = json_object_get_string(ifield))) {
			fatal("Each multirecord area record must have a type specifier");
		}

		debug(3, "Record is of type '%s'", type);

		mr_reclist_tail = add_reclist(mr_reclist);
		if (!mr_reclist_tail)
			fatal("JSON: Failed to allocate multirecord area list");

		debug(4, "Multirecord area record list is now at %p", *mr_reclist);
		debug(4, "Multirecord area record list tail is at %p", mr_reclist_tail);

		if (!strcmp(type, "management")) {
			const char *subtype = NULL;
			fru_mr_mgmt_type_t subtype_id;
			json_object_object_get_ex(item, "subtype", &ifield);
			if (!ifield || !(subtype = json_object_get_string(ifield))) {
				fatal("Each management record must have a subtype");
			}

			debug(3, "Management record subtype is '%s'", subtype);
			subtype_id = frugen_mr_mgmt_type_by_name(subtype);
			if (!subtype_id) {
				fatal("Management record subtype '%s' is invalid", subtype);
			}

			debug(3, "Management record subtype ID is '%d'", subtype_id);

			if (!strcmp(subtype, "uuid")) {
				const char *uuid = NULL;
				json_object_object_get_ex(item, "uuid", &ifield);
				if (ifield) {
					uuid = json_object_get_string(ifield);
				}

				if (!ifield || !uuid) {
					fatal("A uuid management record must have a uuid field");
				}

				debug(3, "Parsing UUID %s", uuid);
				fru_errno = fru_mr_uuid2rec(&mr_reclist_tail->rec, uuid);
				if (fru_errno)
					fatal("Failed to convert UUID: %s", fru_strerr(fru_errno));
				debug(2, "System UUID loaded from JSON: %s", uuid);
				has_multirec = true;
			}
			else {
				int err;
				const char *val = NULL;
				json_object_object_get_ex(item, subtype, &ifield);
				if (ifield) {
					val = json_object_get_string(ifield);
				}

				if (!ifield || !val) {
					fatal("Management record '%s' must have a '%s' field",
					      subtype, subtype);
				}

				err = fru_mr_mgmt_str2rec(&mr_reclist_tail->rec,
				                          val, subtype_id);
				if (err) {
					fatal("Failed to convert '%s' to a record: %s",
						  subtype, strerror(err < 0 ? -err : err));
				}
				debug(2, "Loaded '%s' from JSON: %s", subtype, val);
				has_multirec = true;
			}
		}
		else if (!strcmp(type, "psu")) {
			debug(1, "Found a PSU info record (not yet supported, skipped)");
			continue;
		}
		else {
			fatal("Multirecord type '%s' is not supported", type);
			continue;
		}
	}

out:
	return has_multirec;
}

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
		// Pointers to static strings, depending on the found record
		const char *type = NULL;
		const char *subtype = NULL;
		const char *key = NULL;
		// Pointer to an allocated string to be freed at the end of iteration
		char *val = NULL;

		switch (rec->hdr.type_id) {
			case FRU_MR_MGMT_ACCESS: {
				fru_mr_mgmt_rec_t *mgmt = (fru_mr_mgmt_rec_t *)rec;
				int rc;
				type = "management";
#define MGMT_TYPE_ID(type) ((type) - FRU_MR_MGMT_MIN)
				switch (mgmt->subtype) {
				case FRU_MR_MGMT_SYS_UUID:
					if ((rc = fru_mr_rec2uuid(&val, mgmt, flags))) {
						printf("Could not decode the UUID record: %s\n",
						       strerror(-rc));
						break;
					}
					key = subtype = fru_mr_mgmt_name_by_type(mgmt->subtype);
					break;
				case FRU_MR_MGMT_SYS_URL:
				case FRU_MR_MGMT_SYS_NAME:
				case FRU_MR_MGMT_SYS_PING:
				case FRU_MR_MGMT_COMPONENT_URL:
				case FRU_MR_MGMT_COMPONENT_NAME:
				case FRU_MR_MGMT_COMPONENT_PING:
					if ((rc = fru_mr_mgmt_rec2str(&val, mgmt, flags))) {
						printf("Could not decode the Mgmt Access record '%s': %s\n",
						       fru_mr_mgmt_name_by_type(mgmt->subtype),
						       strerror(-rc));
						break;
					}
					type = "management";
					key = subtype = fru_mr_mgmt_name_by_type(mgmt->subtype);
					break;
				default:
					debug(1, "Multirecord Management subtype 0x%02X is not yet supported",
					      mgmt->subtype);
				}
				break;
			}
			case FRU_MR_PSU_INFO:
				debug(1, "Found a PSU info record (not yet supported, skipped)");
				break;
			default:
				debug(1, "Multirecord type 0x%02X is not yet supported", rec->hdr.type_id);
		}

		if (type && subtype && key && val) {
			struct json_object *val_string, *type_string, *subtype_string, *entry;
			val_string = json_object_new_string((const char *)val);
			if (val) { // We don't need it anymore, it's in val_string already
				free(val);
				val = NULL;
			}
			if (NULL == val_string) {
				printf("Failed to allocate a JSON string for MR record value");
				goto out;
			}
			if ((type_string = json_object_new_string((const char *)type)) == NULL) {
				printf("Failed to allocate a JSON string for MR record type");
				json_object_put(val_string);
				goto out;
			}
			if ((subtype_string = json_object_new_string((const char *)subtype)) == NULL) {
				printf("Failed to allocate a JSON string for MR record subtype");
				json_object_put(type_string);
				json_object_put(val_string);
				goto out;
			}
			if ((entry = json_object_new_object()) == NULL) {
				printf("Failed to allocate a new JSON entry for MR record");
				json_object_put(subtype_string);
				json_object_put(type_string);
				json_object_put(val_string);
				goto out;
			}
			json_object_object_add(entry, "type", type_string);
			json_object_object_add(entry, "subtype", subtype_string);
			json_object_object_add(entry, key, val_string);
			json_object_array_add(mr_jso, entry);
		}

		item = item->next;
		if (val) {
			free(val);
		}
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

static
int json_object_add_with_type(struct json_object* obj,
                              const char* key,
                              const char* val,
                              int type)
{
	struct json_object *string, *type_string, *entry;
	if ((string = json_object_new_string((const char *)val)) == NULL)
		goto STRING_ERR;

	if (type == FIELD_TYPE_AUTO) {
		entry = string;
	} else {
		const char *enc_name = fru_enc_name_by_type(type);
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

void frugen_loadfile_json(fru_t * fru, const char * fname)
{
	json_object *jstree, *jso, *jsfield;
	json_object_iter iter;

	debug(2, "Data format is JSON");
	/* Allocate a new object and load contents from file */
	jstree = json_object_from_file(fname);
	if (NULL == jstree)
		fatal("Failed to load JSON FRU object from %s", fname);

	json_object_object_foreachC(jstree, iter) {
		jso = iter.val;
		if (!strcmp(iter.key, "internal")) {
			size_t len;
			const char *data = json_object_get_string(jso);
			if (!data) {
				debug(2, "Internal use are w/o data, skipping");
				continue;
			}

			if (!fru_set_internal_hexstring(fru, data)) {
				fatal("Failed to load internal use area: %s",
				      fru_strerr(fru_errno));
			}

			debug(2, "Internal use area data loaded from JSON");
			continue;
		} else if (!strcmp(iter.key, "chassis")) {
			const char *fieldname[] = { "pn", "serial" };
			fru_field_t * field[] = {
				&fru->chassis.pn, &fru->chassis.serial
			};
			json_object_object_get_ex(jso, "type", &jsfield);
			if (jsfield) {
				fru->chassis.type = json_object_get_int(jsfield);
				debug(2, "Chassis type 0x%02X loaded from JSON",
				      fru->chassis.type);
				fru->present[FRU_CHASSIS_INFO] = true;
			}
			/* Now get values for the string fields */
			fru->present[FRU_CHASSIS_INFO] |=
				json_fill_fru_area_fields(jso, ARRAY_SZ(field), fieldname,
				                          field);
			debug(9, "chassis custom was %p", fru->chassis.cust);
			fru->present[FRU_CHASSIS_INFO] |=
				json_fill_fru_area_custom(jso, &fru->chassis.cust);
			debug(9, "chassis custom now %p", fru->chassis.cust);
		} else if (!strcmp(iter.key, "board")) {
			const char *fieldname[] = {
				"mfg", "pname", "pn", "serial", "file"
			};
			fru_field_t *field[] = {
				&fru->board.mfg,
				&fru->board.pname,
				&fru->board.pn,
				&fru->board.serial,
				&fru->board.file
			};
			/* Get values for non-string fields */
#if 0 /* TODO: Language support is not implemented yet */
			json_object_object_get_ex(jso, "lang", &jsfield);
			if (jsfield) {
				fru->board.lang = json_object_get_int(jsfield);
				debug(2, "Board language 0x%02X loaded from JSON", fru->board.lang);
				info->has_board = true;
			}
#endif
			json_object_object_get_ex(jso, "date", &jsfield);
			if (jsfield) {
				const char *date = json_object_get_string(jsfield);
				debug(2, "Board date '%s' will be loaded from JSON", date);
				if (!datestr_to_tv(date, &fru->board.tv))
					fatal("Invalid board date/time format in JSON file");
				fru->present[FRU_BOARD_INFO] = true;
				fru->board.tv_auto = false;
			}
			/* Now get values for the string fields */
			fru->present[FRU_BOARD_INFO] |=
				json_fill_fru_area_fields(jso, ARRAY_SZ(field),
			                              fieldname, field);
			debug(9, "board custom was %p", fru->board.cust);
			fru->present[FRU_BOARD_INFO] |=
				json_fill_fru_area_custom(jso, &fru->board.cust);
			debug(9, "board custom now %p", fru->board.cust);
		} else if (!strcmp(iter.key, "product")) {
			const char *fieldname[] = {
				"mfg", "pname", "pn", "ver", "serial", "atag", "file"
			};
			fru_field_t *field[] = {
				&fru->product.mfg,
				&fru->product.pname,
				&fru->product.pn,
				&fru->product.ver,
				&fru->product.serial,
				&fru->product.atag,
				&fru->product.file
			};
#if 0 /* TODO: Language support is not implemented yet */
			/* Get values for non-string fields */
			json_object_object_get_ex(jso, "lang", &jsfield);
			if (jsfield) {
				product.lang = json_object_get_int(jsfield);
				debug(2, "Product language 0x%02X loaded from JSON", product.lang);
				info->has_product = true;
			}
#endif
			/* Now get values for the string fields */
			fru->present[FRU_PRODUCT_INFO] |=
				json_fill_fru_area_fields(jso, ARRAY_SZ(field), fieldname,
				                          field);
			fru->present[FRU_PRODUCT_INFO] |=
				json_fill_fru_area_custom(jso, &fru->product.cust);
		} else if (!strcmp(iter.key, "multirecord")) {
			debug(2, "Processing multirecord area records");
			json_fill_fru_mr_reclist(fru, jso);
		}
	}

	/* Deallocate the JSON object */
	json_object_put(jstree);
}

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
