/** @file
 *
 *  Definitions for various enums defined in DMTF SMBIOS Specification.
 *
 *  @copyright
 *  Copyright (C) 2016-2024 Alexander Amelkin <alexander@amelkin.msk.ru>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later OR Apache-2.0
 */

#pragma once

enum {
	SMBIOS_CHASSIS_UNDEFINED,
	SMBIOS_CHASSIS_OTHER,
	SMBIOS_CHASSIS_UNKNOWN,
	SMBIOS_CHASSIS_DESKTOP,
	SMBIOS_CHASSIS_LPDESKTOP,
	SMBIOS_CHASSIS_PIZZABOX,
	SMBIOS_CHASSIS_MINITOWER,
	SMBIOS_CHASSIS_TOWER,
	SMBIOS_CHASSIS_PORTABLE,
	SMBIOS_CHASSIS_LAPTOP,
	SMBIOS_CHASSIS_NOTEBOOK,
	SMBIOS_CHASSIS_HANDHELD,
	SMBIOS_CHASSIS_DOCKSTATION,
	SMBIOS_CHASSIS_AIO,
	SMBIOS_CHASSIS_SUBNOTEBOOK,
	SMBIOS_CHASSIS_SPACESAVING,
	SMBIOS_CHASSIS_LUNCHBOX,
	SMBIOS_CHASSIS_MAINSERVER,
	SMBIOS_CHASSIS_EXPANSION,
	SMBIOS_CHASSIS_SUBCHASSIS,
	SMBIOS_CHASSIS_BUSEXPANSION,
	SMBIOS_CHASSIS_PERIPHERAL,
	SMBIOS_CHASSIS_RAID,
	SMBIOS_CHASSIS_RACKMOUNT,
	SMBIOS_CHASSIS_SEALED,
	SMBIOS_CHASSIS_MULTISYSTEM,
	SMBIOS_CHASSIS_COMPACT_PCI,
	SMBIOS_CHASSIS_ADVANCED_TCA,
	SMBIOS_CHASSIS_BLADE,
	SMBIOS_CHASSIS_BLADE_ENCLOSURE,
	SMBIOS_CHASSIS_TABLET,
	SMBIOS_CHASSIS_CONVERTIBLE,
	SMBIOS_CHASSIS_DETACHABLE,
	SMBIOS_CHASSIS_TYPES_TOTAL,
};

/** Check if chassis type code is valid according to SMBIOS Specification
 *
 * @retval true Type is valid
 * @retval false Type is invalid
 */
#define SMBIOS_CHASSIS_IS_VALID(t) ((t) > SMBIOS_CHASSIS_UNDEFINED && (t) < SMBIOS_CHASSIS_TYPES_TOTAL)
