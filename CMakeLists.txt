cmake_minimum_required(VERSION 3.0)
if(COMMAND cmake_policy)
	cmake_policy(SET CMP0003 NEW)
endif(COMMAND cmake_policy)

include("doxygen.cmake")

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
	OUTPUT_STRIP_TRAILING_WHITESPACE)

if(NOT res EQUAL 0)
	set(gitver "v2.0-dirty-orphan")
endif()

string(REPLACE "v" "" gitver "${gitver}")
string(REPLACE "-" "." gitver "${gitver}")

# This is the version of the project w/o hash information
string(REGEX REPLACE "((\\.?[0-9]+)+).*" "\\1" gitver_short "${gitver}")

option(BUILD_SHARED_LIB "build shared library" ON)
option(BINARY_STATIC "link all libs static when compile frugen" OFF)
option(BINARY_32BIT "compile 32bit version" OFF)
option(ENABLE_JSON "enable JSON support" ON)
option(JSON_STATIC "link json-c library statically" OFF)
option(DEBUG_OUTPUT "show extra debug output" OFF)

set(CMAKE_C_FLAGS_RELEASE "-Os -Wall -Werror -Wfatal-errors")
set(CMAKE_C_FLAGS_DEBUG "-g3 -O0 -Wall -Werror -Wfatal-errors")
if(MSVC)
	# warning level 4
	add_compile_options(/W4)
else(MSVC)
	# additional warnings
	add_compile_options(-Wall -Wextra -Wunreachable-code)
endif(MSVC)

if(DEBUG_OUTPUT)
	add_definitions(-DDEBUG)
endif(DEBUG_OUTPUT)

project(frugen VERSION ${gitver_short} LANGUAGES C)
add_definitions(-DVERSION="${gitver}")

configure_file(fru.h.in fru.h @ONLY)
set(DOXY_PROJECT_NUMBER "${gitver}")
set(DOXY_OUTPUT_LANGUAGE "English")
set(DOXY_INPUT ${PROJECT_SOURCE_DIR})
set(DOXY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/docs")
add_documentation(docs Doxyfile)

# build libfru
if(BUILD_SHARED_LIB)
	add_library(fru-shared SHARED fru-errno.c fru.c)
	set_target_properties(fru-shared PROPERTIES OUTPUT_NAME fru CLEAN_DIRECT_OUTPUT 1)
	set_target_properties(fru-shared PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION ${PROJECT_VERSION_MAJOR})

	list(APPEND ALL_TARGETS "fru-shared")
endif(BUILD_SHARED_LIB)

if(BINARY_STATIC OR NOT BUILD_SHARED_LIB)
	add_library(fru-static STATIC fru-errno.c fru.c)
	set_target_properties(fru-static PROPERTIES OUTPUT_NAME fru CLEAN_DIRECT_OUTPUT 1)
	list(APPEND ALL_TARGETS "fru-static")
endif(BINARY_STATIC OR NOT BUILD_SHARED_LIB)

# target for frugen
set(frugen_SOURCES)
list(APPEND frugen_SOURCES frugen.c)
if(ENABLE_JSON)
	list(APPEND frugen_SOURCES frugen-json.c)
endif(ENABLE_JSON)
add_executable(frugen ${frugen_SOURCES})
list(APPEND ALL_TARGETS "frugen")

if(BINARY_STATIC)
	target_link_libraries(frugen fru-static -static)
elseif(NOT BUILD_SHARED_LIB)
	target_link_libraries(frugen fru-static)
else(BINARY_STATIC)
	target_link_libraries(frugen fru-shared)
endif(BINARY_STATIC)

if(ENABLE_JSON)
	find_package(json-c CONFIG REQUIRED)
	message (STATUS "Using JSON Library found at " ${json-c_DIR})
	add_definitions(-D__HAS_JSON__)
	if(JSON_STATIC OR BINARY_STATIC)
		target_link_libraries(frugen json-c::json-c-static)
	else(JSON_STATIC OR BINARY_STATIC)
		target_link_libraries(frugen json-c::json-c)
	endif(JSON_STATIC OR BINARY_STATIC)
else(ENABLE_JSON)
	message (WARNING "JSON library support *disabled*!")
endif(ENABLE_JSON)

# set additional params
foreach(TARGET IN LISTS ALL_TARGETS)
	target_include_directories(${TARGET} PUBLIC "${CMAKE_CURRENT_BINARY_DIR}")
	# To make frugen 32-bit, set BINARY_32BIT or use an external toolchain file
	if(BINARY_32BIT)
		set_target_properties(${TARGET} PROPERTIES COMPILE_FLAGS "-m32" LINK_FLAGS "-m32")
	endif(BINARY_32BIT)
endforeach()

# install targets
install(TARGETS frugen RUNTIME DESTINATION bin)
if(BUILD_SHARED_LIB)
	install(TARGETS fru-shared DESTINATION lib)
	install(FILES ${CMAKE_CURRENT_BINARY_DIR}/fru.h DESTINATION include)
endif(BUILD_SHARED_LIB)
