

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
full compliance with [IPMI FRU Information Storage Definition v1.0, rev. 1.3.](http://www.intel.com/content/www/us/en/servers/ipmi/ipmi-platform-mgt-fru-infostorage-def-v1-0-rev-1-3-spec-update.html)



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

### The limitations:

  * Internal use area creation/modification is available only from template file,
    not available from the command line arguments.
  * You should specify the UUID either in the template file OR in the command
    line, not both. The command line does NOT override the template file in
    that regard, but creates an additional UUID record, which may be undesired.

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

### Operation principles

  The `frugen` tool takes a template in any of the supported formats and builds
  an 'exploded' and 'decoded' version of fru file in memory.  It then takes
  command line arguments and applies any changes to the fru fields specified by
  those arguments. After that step is complete, it either encodes/packs the
  data into a binary fru file, or dumps it in json or plain text format to a
  file or stdout.

  Any text field in FRU has an encoding type. The IPMI FRU specification
  defines 4 types: Text (ASCII), 6-bit ASCII, BCD+, and binary. The tool can
  detect which encoding suits best for the provided data or it can preserve the
  original encoding used in the source template.

  If autodetection is chosen (see the 'set' option below), then the tool will
  attempt to encode the new data the most compact/efficient way.  It will start
  with binary encoding. If your data string contains anything except for hex
  digits (0-9, A-F), the tool will expand the charset and attempt to use BCD+.
  If that doesn't fit, 6-bit ASCII will be attempted.  Finally, the data string
  will be copied as is (plain text encoding) if none of the encoding worked.

  If you choose to preserve the original encoding, please note that `frugen`
  will fail if your new data cannot be encoded using the same encoding type as
  the original. For instance, if your original data was "ACME" encoded as 6-bit
  ASCII, and you want to preserve the encoding, but the new data is "A Company
  Making Everything", then the tool will fail because 6-bit encoding doesn't
  allow for lowercase characters. Please note, though, that if you choose text
  or json output format, then no validity check will be immediately performed
  on the new value. The tool will then fail only on the attempt to convert the
  resulting json template to binary form, not earlier.

  Input binary FRU files (or templates as considered by frugen) may contain
  multirecord areas. Such areas consists of many so-called 'records' of various
  types, including some OEM ones that aren't covered by IPMI FRU specification.
  It is impossible for `frugen` or `libfru` to support them all, but the best
  effort is made to not destroy them at least. When using a binary FRU file as
  a template, and binary format for the output, `frugen` will copy any
  unsupported MR records from the source into the destination without any
  alteration. The same approach will be applied to the iternal use area,
  the contents of which are completely OEM-specific and can not be parsed.

### Invocation

For the most up-to-date information on the frugen tool invocation and options, please
use `frugen -h`, below is an example of the output of that command:

```
FRU Generator v2.0.0.gXXXXXXX (C) 2016-2024, Alexander Amelkin <alexander@amelkin.msk.ru>

Usage: frugen [options] <filename>

Options:

	-d <argument>, --board-date <argument>
		Set board manufacturing date/time, use "DD/MM/YYYY HH:MM:SS" format.
		By default the current system date/time is used unless -u is specified.

	-g <argument>, --debug <argument>
		Set debug flag (use multiple times for multiple flags):
			fver  - Ignore wrong version in FRU header
			aver  - Ignore wrong version in area headers
			rver  - Ignore wrong verison in multirecord area record version
			asum  - Ignore wrong area checksum (for standard areas)
			rhsum - Ignore wrong record header checksum (for multirecord)
			rdsum - Ignore wrong data checksum (for multirecord)
			rend  - Ignore missing EOL record, use any found records.

	-h[<argument>], --help[=<argument>]
		Display this help. Use any option name as an argument to show
		help for a single option.

		Examples:
			frugen -h     # Show full program help
			frugen -hhelp # Help for long option '--help'
			frugen -hh    # Help for short option '-h'.

	-j <argument>, --json <argument>
		Load FRU information from a JSON file, use '-' for stdin.

	-o <argument>, --out-format <argument>
		Output format, one of:
		binary - Default format when writing to a file.
		         For stdout, the following will be used, even
		         if 'binary' is explicitly specified:
		json   - Default when writing to stdout.
		text   - Plain text format, no decoding of MR area records.

	-r <argument>, --raw <argument>
		Load FRU information from a raw binary file, use '-' for stdin.

	-s <argument>, --set <argument>
		Set a text field in an area to the given value, use given encoding
		Requires an argument in form [<encoding>:]<area>.<field>=<value>
		If an encoding is not specified at all, frugen will attempt to
		preserve the encoding specified in the template or will use 'auto'
		if none is set there. To force 'auto' encoding you may either
		specify it explicitly or use a bare ':' without any preceding text.

		Supported encodings:
			auto      - Autodetect encoding based on the used characters.
			            This will attempt to use the most compact encoding
			            among the following.
			6bitascii - 6-bit ASCII, available characters:
			             !"#$%^&'()*+,-./
			            1234567890:;<=>?
			            @ABCDEFGHIJKLMNO
			            PQRSTUVWXYZ[\]^_
			bcdplus   - BCD+, available characters:
			            01234567890 -.
			text      - Plain text (Latin alphabet only).
			            Characters: Any printable 8-bit ASCII byte.
			binary    - Binary data represented as a hex string.
			            Characters: 0123456789ABCDEFabcdef

		For area and field names, please refer to example.json

		You may specify field name 'custom' to add a new custom field.
		Alternatively, you may specify field name 'custom.<N>' to
		replace the value of the custom field number N given in the
		input template file.

		Examples:

			frugen -r fru-template.bin -s text:board.pname="MY BOARD" out.fru
				# (encode board.pname as text)
			frugen -r fru-template.bin -s board.pname="MY BOARD" out.fru
				# (preserve original encoding type if possible)
			frugen -r fru-template.bin -s :board.pname="MY BOARD" out.fru
				# (auto-encode board.pname as 6-bit ASCII)
			frugen -j fru-template.json -s binary:board.custom=0102DEADBEEF out.fru
				# (add a new binary-encoded custom field to board)
			frugen -j fru-template.json -s binary:board.custom.2=0102DEADBEEF out.fru
				# (replace custom field 2 in board with new value).

	-t <argument>, --chassis-type <argument>
		Set chassis type (hex). Defaults to 0x02 ('Unknown').

	-u, --board-date-unspec
		Don't use current system date/time for board mfg. date, use 'Unspecified'.

	-U <argument>, --mr-uuid <argument>
		Set System Unique ID (UUID/GUID)
		NOTE: This does NOT replace the data specified in the template.

	-v, --verbose
		Increase program verbosity (debug) level.

Example (encode from scratch):
	frugen -s board.mfg="Biggest International Corp." \
	       --set board.pname="Some Cool Product" \
	       --set text:board.pn="BRD-PN-123" \
	       --board-date "10/1/2017 12:58:00" \
	       --set board.serial="01171234" \
	       --set board.file="Command Line" \
	       --set binary:board.custom="01020304FEAD1E" \
	       fru.bin

Example (decode to json, output to stdout):
	frugen --raw fru.bin -o json -

Example (modify binary file):
	frugen --raw fru.bin \
	       --set text:board.serial=123456789 \
	       --set text:board.custom.1="My custom field" \
	       fru.bin
```

### JSON

__Dependency:__ [json-c library](https://github.com/json-c/json-c)

The frugen tool supports JSON files. You may specify all the FRU info fields (mind the
general tool limitations) in a file and use it as an input for the tool:

    frugen --json example.json fru.bin

An example file 'example.json' is provided for your reference.

NOTE: The JSON file for frugen is allowed to have C-style comments (`/* comment */`),
which is an extension to the standard JSON format.

## Building

### Linux

    mkdir build && cd build
    cmake ..
    make

There is a number of optional parameters for cmake to control build procedure:

|Option          |Default|Description                                                          |
|----------------|-------|---------------------------------------------------------------------|
|BINARY_32BIT    | OFF   |Build 32-bit versions of everything                                  |
|BUILD_SHARED_LIB| ON    |Build libfru as a shared library, implies dynamic linking of `frugen`|
|BINARY_STATIC   | OFF   |Force full static linking of `frugen`, makes it HUGE                 |
|ENABLE_JSON     | ON    |Enable JSON support if json-c library is available                   |
|JSON_STATIC     | OFF   |Link json-c library statically into `frugen`                         |

**NOTE**: `BUILD_SHARED_LIB` and `BINARY_STATIC` are not mutually exclusive: while first option
controls building `libfru`, second one is related to `frugen`. When both options are enabled
static and shared versions of `libfru` will be compiled. When both options are disabled libfru
will be linked statically into `frugen`, while other libraries are linked shared.

To build a debug version use the following command:

    mkdir build && cd build
    cmake -DCMAKE_BUILD_TYPE=Debug -DDEBUG_OUTPUT=yes ..
    make

To build a semi-statically linked version (with `libfru` and `libjson-c` built-in), use:

    mkdir build && cd build
    cmake -DBINARY_STATIC=ON -DJSON_STATIC=ON ..
    make

To build project documentation (requies `doxygen`), run `make docs`.

### Windows (cross-compiled on Linux)

You will need a MingW32 toolchain. This chapter is written in assumption you're
using `x86_64-w64-mingw32`.

First of all you will need to create a `x86_64-w64-mingw32-toolchain.cmake`
file describing your cross-compilation toolchain.

This file assumes that you use `$HOME/mingw-install` as an installation prefix
for all mingw32-compiled libraries (e.g., `libjson-c`).

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

    cmake -DCMAKE_TOOLCHAIN_FILE=x86_64-w64-mingw32-toolchain.cmake .
    make

## Contact information

Should you have any questions or proposals, please feel free to:

 * Submit changes as a pull request via https://codeberg.org/IPMITool/frugen/pulls
 * Report a problem by creating an issue at https://codeberg.org/IPMITool/frugen/issues

