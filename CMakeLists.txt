cmake_minimum_required(VERSION 3.5)

# To make frugen 32-bit, set BINARY_32BIT or use an external toolchain file
option(BINARY_32BIT "compile 32bit version" OFF)
if(BINARY_32BIT)
	# Note: this have to go before PROJECT
	set(CMAKE_C_COMPILER gcc -m32)
endif(BINARY_32BIT)

execute_process(COMMAND
	git
	describe
	--always
	--long
	--dirty
	WORKING_DIRECTORY
	"${CMAKE_CURRENT_SOURCE_DIR}"
	RESULT_VARIABLE
	res
	OUTPUT_VARIABLE
	gitver
	ERROR_QUIET
	OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT res EQUAL 0)
	set(gitver "v2.0-dirty-orphan")
endif()

string(REPLACE "v" "" gitver "${gitver}")
string(REPLACE "-" "." gitver "${gitver}")

# This is the version of the project w/o hash information
string(REGEX REPLACE "((\\.?[0-9]+)+).*" "\\1" gitver_short "${gitver}")
project(frugen VERSION ${gitver_short} LANGUAGES C)

option(BUILD_SHARED_LIB "build shared library" ON)
option(BINARY_STATIC "link all libs static when compile frugen" OFF)
option(ENABLE_JSON "enable JSON support" ON)
option(JSON_STATIC "link json-c library statically" OFF)
option(DEBUG_OUTPUT "show extra debug output" OFF)

set(CMAKE_C_FLAGS_RELEASE "-Os")
set(CMAKE_C_FLAGS_DEBUG "-g3 -O0")
if(MSVC)
	# warning level 4
	add_compile_options(/W4)
else(MSVC)
	# additional warnings
	add_compile_options(
		-Wall
		-Werror
		-Wfatal-errors
		-Wextra
		-Wunreachable-code
		-Wunused-macros
		-Wunused-local-typedefs
		-ffunction-sections
		-fdata-sections
	)
endif(MSVC)

if(DEBUG_OUTPUT)
	add_definitions(-DDEBUG)
endif(DEBUG_OUTPUT)

add_definitions(-DVERSION="${gitver}")

configure_file(fru.h.in fru.h @ONLY)

include("doxygen.cmake")
set(DOXY_PROJECT_NUMBER "${gitver}")
set(DOXY_OUTPUT_LANGUAGE "English")
set(DOXY_INPUT "${PROJECT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/build")
set(DOXY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/docs")
add_documentation(docs Doxyfile)

# build libfru
set(libfru_SOURCES
	lib/fru__decode.c
	lib/fru_enable_area.c
	lib/fru_errno.c
	lib/fru_add_custom.c
	lib/fru_add_mr.c
	lib/fru_common.c
	lib/fru_delete_custom.c
	lib/fru_get_custom.c
	lib/fru_init.c
	lib/fru_internal.c
	lib/fru_load.c
	lib/fru_mr_ops.c
	lib/fru_save.c
	lib/fru_setfield.c
	lib/fru_setfield_binary.c
	lib/fru_getfield.c
)
set(libfru_HEADERS
	${CMAKE_CURRENT_BINARY_DIR}/fru.h
	fru_errno.h
	lib/fru-private.h
)

set(libfru_PUBLIC_HEADERS
	${CMAKE_CURRENT_BINARY_DIR}/fru.h
	fru_errno.h
)

if(BUILD_SHARED_LIB)
	add_library(fru-shared SHARED ${libfru_SOURCES} ${libfru_HEADERS})
	add_library(frugen::fru-shared ALIAS fru-shared)
	target_include_directories(fru-shared PUBLIC
		$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
		$<INSTALL_INTERFACE:include>
	)
	set_target_properties(fru-shared PROPERTIES OUTPUT_NAME fru CLEAN_DIRECT_OUTPUT 1)
	set_target_properties(fru-shared PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION ${PROJECT_VERSION_MAJOR})
	set_target_properties(fru-shared PROPERTIES PUBLIC_HEADER "${libfru_PUBLIC_HEADERS}")
	list(APPEND LIB_TARGETS "fru-shared")
endif(BUILD_SHARED_LIB)
if(BINARY_STATIC OR NOT BUILD_SHARED_LIB)
	add_library(fru-static STATIC ${libfru_SOURCES} ${libfru_HEADERS})
	add_library(frugen::fru-static ALIAS fru-static)
	target_include_directories(fru-static PUBLIC
		$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
		$<INSTALL_INTERFACE:include>
	)
	set_target_properties(fru-static PROPERTIES OUTPUT_NAME fru CLEAN_DIRECT_OUTPUT 1)
	set_target_properties(fru-static PROPERTIES PUBLIC_HEADER "${CMAKE_CURRENT_BINARY_DIR}/fru.h")
	list(APPEND LIB_TARGETS "fru-static")
endif(BINARY_STATIC OR NOT BUILD_SHARED_LIB)

# target for frugen
set(frugen_SOURCES frugen.c)
add_executable(frugen ${frugen_SOURCES})

if(ENABLE_JSON)
	find_package(PkgConfig)
	pkg_check_modules(json-c json-c)

# For debugging
#	get_cmake_property(_variableNames VARIABLES)
#	list (SORT _variableNames)
#	foreach (_variableName ${_variableNames})
#	    message(STATUS "VARIABLE ${_variableName}=${${_variableName}}")
#	endforeach()

	if(json-c_FOUND)
		if(JSON_STATIC OR BINARY_STATIC)
# TODO: Find a way to detect that static version is indeed present
#       as it is missing in Fedora by default
#			if (NOT json-c_STATIC_FOUND)
#				message (WARNING "No STATIC json-c library available")
#				set(ENABLE_JSON false)
#			else ()
				target_link_libraries(frugen ${json-c_STATIC_LIBRARIES})
#			endif ()
		else ()
			target_link_libraries(frugen ${json-c_LIBRARIES})
		endif ()
	endif ()

	if (ENABLE_JSON)
		add_definitions(-D__HAS_JSON__)
		list(APPEND frugen_SOURCES frugen-json.c)
		set_target_properties(frugen PROPERTIES SOURCES "${frugen_SOURCES}")
		target_include_directories(frugen PRIVATE ${json-c_INCLUDE_DIRS})
	else ()
		message (WARNING "JSON library support *disabled*!")
	endif ()
endif(ENABLE_JSON)

list(APPEND ALL_TARGETS ${LIB_TARGETS} "frugen")

if (CMAKE_BUILD_TYPE STREQUAL "Debug")
	set_target_properties(frugen PROPERTIES LINK_FLAGS "-Wl,--gc-sections,--print-gc-sections")
else()
	set_target_properties(frugen PROPERTIES LINK_FLAGS "-Wl,--gc-sections")
endif()

if(BINARY_STATIC)
	target_link_libraries(frugen -static)
endif(BINARY_STATIC)
if(BINARY_STATIC OR NOT BUILD_SHARED_LIB)
	target_link_libraries(frugen fru-static)
else(BINARY_STATIC OR NOT BUILD_SHARED_LIB)
	target_link_libraries(frugen fru-shared)
endif(BINARY_STATIC OR NOT BUILD_SHARED_LIB)

# install targets
install(TARGETS frugen RUNTIME DESTINATION bin)
install(TARGETS ${LIB_TARGETS}
	EXPORT ${PROJECT_NAME}-targets
	PUBLIC_HEADER DESTINATION include
	ARCHIVE DESTINATION lib
	LIBRARY DESTINATION lib
	RUNTIME DESTINATION bin
)
install(EXPORT ${PROJECT_NAME}-targets
	NAMESPACE ${PROJECT_NAME}::
	FILE ${PROJECT_NAME}-config.cmake
	DESTINATION lib/cmake/${PROJECT_NAME}
)
