# frugen / libfru

## License

This work is dual-licensed under Apache 2.0 and GPL 2.0 (or any later version)
for the frugen utility, or under Apache 2.0 and Lesser GPL 2.0 (or any later version)
for the fru library.

You can choose between one of them if you use this work.

`SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later`

## Introduction

This project was incepted to eventually create a universal, full-featured IPMI
FRU Information generator / editor library and command line tool, written in
full compliance with IPMI FRU Information Storage Definition v1.0, rev. 1.3., see
http://www.intel.com/content/www/us/en/servers/ipmi/ipmi-platform-mgt-fru-infostorage-def-v1-0-rev-1-3-spec-update.html

## libfru

So far supported in libfru:

  * Data encoding into all the defined formats (binary, BCD plus, 6-bit ASCII, language code specific text).
    The exceptions are:

    * all text is always encoded as if the language code was English (ASCII, 1 byte per character)
    * encoding is selected either automatically based on value range of the supplied data, or by
      specifying it explicitly

  * Data decoding from all the formats defined by the specification.
    Exception: Unicode is not supported

  * Internal use area creation from and decoding to a hex string,
    only with automatic sizing
  * Chassis information area creation and decoding
  * Board information area creation and decoding
  * Product information area creation and decoding
  * Multirecord area creation and decoding with the following record types:

    * Management Access Record with the following subtypes:

      * System UUID (uuid)
      * System name (sname)
      * System management URL (surl)
      * System ping address (spingaddr)
      * Component name (cname)
      * Component management URL (curl)
      * Component ping address (cpingaddr)

  * FRU area finding (in a memory buffer)
  * FRU file creation (in a memory buffer)

_NOT supported:_

  * Miltirecord area record types/subtypes other than listed above.
    NOTE: The unsupported MR record types/subtypes can not be decoded
          or encoded, but they are still found as raw records and
          can be used to build a FRU file if both input and output
          formats are binary rather than json.

## frugen

The frugen tool supports the following (limitations imposed by the libfru library):

  * Internal use area (from/to a raw hex string)
  * Board area creation (including custom fields)
  * Product area creation (including custom fields)
  * Chassis area creation (including custom fields)
  * Multirecord area creation and decoding (see libfru supported types above)

The limitations:

  * All data fields (except custom) provided as command line arguments are always
    treated as ASCII text, and the encoding is automatically selected based on the
    byte range of the provided data. Custom fields may be forced to be binary
    using `--binary` option. Encoding may be specified explicitly for the data fields
    in a JSON input template.
  * Encodings will be reset to automatic when a binary FRU file is used as a template.
    There is no way so far to enforce any specific encoding or to preserve the original one.
  * Internal use area creation/modification is available only from template file,
    not available from the command line arguments.
  * You should specify the UUID and the custom fields in either
    the template file OR in the command line. The command line does
    NOT override the template file in that regard, but creates an additional
    UUID or custom record, which may be undesirable

_NOTE:_ You may use `frugen` to modify some standard fields even on FRU files that
        contain unsupported multirecord area records if you use binary input and
        binary output formats. Those fields will be copied as is from the source to the
        destination file.

_NOTE:_ You may use `frugen` to fix area and record checksums in your FRU file.
        For that purpose use binary input and output formats, and specify the debug (`-g`)
        flags to ingnore the errors in your source FRU file. The checksums for the
        destination FRU file will be recalculated properly according to the specification.
        Please note that FRU files with bad header checksum will not be processed as
        there is no debug flag to ignore that error.

For the most up-to-date information on the frugen tool invocation and options, please
use `frugen -h`, below is an example of the output of that command:

```
FRU Generator v1.4.xx.gXXXXXXX (C) 2016-2023, Alexander Amelkin <alexander@amelkin.msk.ru>

Usage: frugen [options] <filename>

Options:

	-h, --help
		Display this help.

	-v, --verbose
		Increase program verbosity (debug) level.

	-g, --debug <argument>
		Set debug flag (use multiple times for multiple flags):
			fver  - Ignore wrong version in FRU header
			aver  - Ignore wrong version in area headers
			rver  - Ignore wrong verison in multirecord area record version
			asum  - Ignore wrong area checksum (for standard areas)
			rhsum - Ignore wrong record header checksum (for multirecord)
			rdsum - Ignore wrong data checksum (for multirecord)
			rend  - Ignore missing EOL record, use any found records.

	-b, --binary
		Mark the next --*-custom option's argument as binary.
		Use hex string representation for the next custom argument.

		Example: frugen --binary --board-custom 0012DEADBEAF

		There must be an even number of characters in a 'binary' argument.

	-I, --ascii
		Disable auto-encoding on all fields, force ASCII.
		Out of ASCII range data will still result in binary encoding.

	-j, --json
		Set input file format to JSON. Specify before '--from'.

	-r, --raw
		Set input file format to raw binary. Specify before '--from'.

	-z, --from <argument>
		Load FRU information from a file, use '-' for stdout.

	-o, --out-format <argument>
		Output format, one of:
		binary - Default format when writing to a file.
		         For stdout, the following will be used, even
		         if 'binary' is explicitly specified:
		json   - Default when writing to stdout.
		text   - Plain text format, no decoding of MR area records.

	-t, --chassis-type <argument>
		Set chassis type (hex). Defaults to 0x02 ('Unknown').

	-a, --chassis-pn <argument>
		Set chassis part number.

	-c, --chassis-serial <argument>
		Set chassis serial number.

	-C, --chassis-custom <argument>
		Add a custom chassis information field, may be used multiple times.
		NOTE: This does NOT replace the data specified in the template.

	-n, --board-pname <argument>
		Set board product name.

	-m, --board-mfg <argument>
		Set board manufacturer name.

	-d, --board-date <argument>
		Set board manufacturing date/time, use "DD/MM/YYYY HH:MM:SS" format.
		By default the current system date/time is used unless -u is specified.

	-u, --board-date-unspec
		Don't use current system date/time for board mfg. date, use 'Unspecified'.

	-p, --board-pn <argument>
		Set board part number.

	-s, --board-serial <argument>
		Set board serial number.

	-f, --board-file <argument>
		Set board FRU file ID.

	-B, --board-custom <argument>
		Add a custom board information field, may be used multiple times.
		NOTE: This does NOT replace the data specified in the template.

	-N, --prod-name <argument>
		Set product name.

	-G, --prod-mfg <argument>
		Set product manufacturer name.

	-M, --prod-modelpn <argument>
		Set product model / part number.

	-V, --prod-version <argument>
		Set product version.

	-S, --prod-serial <argument>
		Set product serial number.

	-F, --prod-file <argument>
		Set product FRU file ID.

	-A, --prod-atag <argument>
		Set product Asset Tag.

	-P, --prod-custom <argument>
		Add a custom product information field, may be used multiple times
		NOTE: This does NOT replace the data specified in the template.

	-U, --mr-uuid <argument>
		Set System Unique ID (UUID/GUID)
		NOTE: This does NOT replace the data specified in the template.

Example (encode):
	frugen --board-mfg "Biggest International Corp." \
	       --board-pname "Some Cool Product" \
	       --board-pn "BRD-PN-123" \
	       --board-date "10/1/2017 12:58:00" \
	       --board-serial "01171234" \
	       --board-file "Command Line" \
	       --binary --board-custom "01020304FEAD1E" \
	       fru.bin

Example (decode):
	frugen --raw --from fru.bin -
```

### JSON

__Dependency:__ json-c library (https://github.com/json-c/json-c)

The frugen tool supports JSON files. You may specify all the FRU info fields (mind the
general tool limitations) in a file and use it as an input for the tool:

    frugen --json --from=example.json fru.bin

An example file 'example.json' is provided for your reference.

NOTE: The JSON file for frugen is allowed to have C-style comments (`/* comment */`),
which is an extension to the standard JSON format.

## Building

### Linux

    mkdir build && cd build
    cmake ..
    make

There are number of optional parameters for cmake to control build procedure:
* BUILD_SHARED_LIB - build libfru as a shared library (default ON)
* BINARY_STATIC - link all libs static when compile frugen (default OFF)
* BINARY_32BIT - compile 32bit version (default OFF)
* DEBUG_OUTPUT - show extra debug output (default OFF)

Note that BUILD_SHARED_LIB and BINARY_STATIC are not mutually exclusive: while first option
controls building libfru, second one is related to frugen. When both options are enabled
static and shared versions of libfru will be compiled. When both options are disabled libfru
will be linked statically into frugen, while other libraries are linked shared.

To compile debug version use the following command:

    cmake -DCMAKE_BUILD_TYPE=Debug -DDEBUG_OUTPUT=yes .. && make

To build project documentation (requies `doxygen`), run `make docs`.

### Windows (cross-compiled on Linux)

You will need a MingW32 toolchain. This chapter is written in assumption you're
using x86\_64-w64-mingw32.

First of all you will need to create a x86\_64-w64-mingw32-toolchain.cmake
file describing your cross-compilation toolchain.

This file assumes that you use $HOME/mingw-install as an installation prefix
for all mingw32-compiled libraries (e.g., libjson-c).

    # the name of the target operating system
    SET(CMAKE_SYSTEM_NAME Windows)

    SET(MINGW32_INSTALL_DIR $ENV{HOME}/mingw-install)

    # which compilers to use for C and C++
    SET(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)
    SET(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
    SET(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)

    # here is the target environment located
    SET(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32/ ${MINGW32_INSTALL_DIR})

    # adjust the default behaviour of the FIND_XXX() commands:
    # search headers and libraries in the target environment, search
    # programs in the host environment
    set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
    set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
    set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

    # let the compiler find the includes and linker find the libraries
    # MinGW linker doesn't accept absolute library paths found
    # by find_library()
    include_directories(BEFORE ${MINGW32_INSTALL_DIR}/include)
    set(CMAKE_LIBRARY_PATH ${MINGW32_INSTALL_DIR}/lib ${MINGW32_INSTALL_DIR}/usr/lib ${CMAKE_LIBRARY_PATH})

Once you have that file, build the tool as follows:

    cmake -DCMAKE_TOOLCHAIN_FILE=x86\_64-w64-mingw32-toolchain.cmake  .
    make

## Contact information

Should you have any questions or proposals, please feel free to:

 * Submit changes as a pull request via https://codeberg.org/IPMITool/frugen/pulls
 * Report a problem by creating an issue at https://codeberg.org/IPMITool/frugen/issues
