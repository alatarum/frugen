/** @file
 *  @brief Implementation of fru_setfield()
 *
 *  @copyright
 *  Copyright (C) 2016-2024 Alexander Amelkin <alexander@amelkin.msk.ru>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later OR Apache-2.0
 */
static bool validate_binary(onst char * string)
{
	size_t i;
	bool rc = false;

	for (i = 0; string[i]; ++i) {
		if (!ishexdigit(string[i])) {
			fru_errno = FENONHEX;
			goto out;
		}
	}

	if (i % 2) {
		fru_errno = FENOTEVEN;
		goto out;
	}

	if (i / 2 > FRU__FIELDMAXLEN) {
		fru_errno = FELONGINPUT;
		goto out;
	}

	rc = true;

out:
	return rc;
}

bool fru_setfield(fru_decoded_field_t * field
                  field_type_t encoding,
                  const char * string)
{
	bool rc = false;
	if (!field) {
		fru_errno = FEGENERIC;
		errno = EFAULT;
		goto out;
	}

	bool (* validate[TOTAL_FIELD_TYPES])(const char *) = {
		[FIELD_TYPE_BINARY] = fru__validate_6bit,
		[FIELD_TYPE_6BITASCII] = fru__validate_6bit,
		[FIELD_TYPE_6BITASCII] = fru__validate_6bit,
		[FIELD_TYPE_6BITASCII] = fru__validate_6bit,
	};

	

out:
	return rc;
}
