/** @file
 *  @brief FRU generator utility JSON support code
 *
 *  Copyright (C) 2016-2023 Alexander Amelkin <alexander@amelkin.msk.ru>
 *  SPDX-License-Identifier: GPL-2.0-or-later OR Apache-2.0
 */
#include "fru.h"
#include "frugen.h"

/**
 * Load a FRU template from JSON file into a FRU information structure
 */
void load_from_json_file(const char *fname, struct frugen_fruinfo_s *info);
void save_to_json_file(FILE **fp, const char *fname,
                       const struct frugen_fruinfo_s *info,
                       const struct frugen_config_s *config);
