/** @file
 *  @brief FRU generator utility JSON support code
 *
 *  @copyright
 *  Copyright (C) 2016-2024 Alexander Amelkin <alexander@amelkin.msk.ru>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later OR Apache-2.0
 */
#include "frugen.h"

/**
 * Load a FRU template from JSON file into a FRU information structure
 */
void frugen_loadfile_json(fru_t * fru, const char * fname);

void save_to_json_file(FILE **fp, const char *fname,
                       const fru_t * fru);
