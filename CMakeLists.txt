cmake_minimum_required(VERSION 3.0)
project(frugen C)
if(COMMAND cmake_policy)
	cmake_policy(SET CMP0003 NEW)
endif(COMMAND cmake_policy)

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
	set(gitver "UNKNOWN")
endif()

string(REPLACE "-" "." gitver "${gitver}")

option(BUILD_SHARED_LIB "build shared library" ON)
option(BINARY_STATIC "link all libs static when compile frugen" OFF)
option(BINARY_32BIT "compile 32bit version" OFF)
option(ENABLE_JSON "enable JSON support" ON)
option(DEBUG_OUTPUT "show extra debug output" OFF)

set(CMAKE_C_FLAGS_RELEASE "-O3 -Wall -Werror -Wfatal-errors")
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

add_definitions(-DVERSION="${gitver}")

# build libfru
if(BUILD_SHARED_LIB)
  add_library(fru-shared SHARED fru-errno.c fru.c)
  set_target_properties(fru-shared PROPERTIES OUTPUT_NAME fru CLEAN_DIRECT_OUTPUT 1)
  list(APPEND ALL_TARGETS "fru-shared")
endif(BUILD_SHARED_LIB)

if(BINARY_STATIC OR NOT BUILD_SHARED_LIB)
  add_library(fru-static STATIC fru-errno.c fru.c)
  set_target_properties(fru-static PROPERTIES OUTPUT_NAME fru CLEAN_DIRECT_OUTPUT 1)
  list(APPEND ALL_TARGETS "fru-static")
endif(BINARY_STATIC OR NOT BUILD_SHARED_LIB)

unset (JSON_LIB CACHE)
if (ENABLE_JSON)
	# looking for JSON library
	find_library(JSON_LIB json-c)
endif (ENABLE_JSON)

# target for frugen
set(frugen_SOURCES)
list(APPEND frugen_SOURCES frugen.c)
if(JSON_LIB)
  list(APPEND frugen_SOURCES frugen-json.c)
endif(JSON_LIB)
add_executable(frugen ${frugen_SOURCES})
list(APPEND ALL_TARGETS "frugen")
if(BINARY_STATIC)
  target_link_libraries(frugen fru-static -static)
elseif(NOT BUILD_SHARED_LIB)
  target_link_libraries(frugen fru-static)
else(BINARY_STATIC)
  target_link_libraries(frugen fru-shared)
endif(BINARY_STATIC)

if(JSON_LIB)
  message (STATUS "Using JSON Library found at " ${JSON_LIB})
  add_definitions(-D__HAS_JSON__)
  if(BINARY_STATIC)
    target_link_libraries(frugen ${JSON_LIB} -static)
  else(BINARY_STATIC)
    target_link_libraries(frugen ${JSON_LIB})
  endif(BINARY_STATIC)
else(JSON_LIB)
	message (WARNING "JSON library *NOT* found. JSON support *disabled*!")
endif(JSON_LIB)

# set additional params
foreach(TARGET IN LISTS ALL_TARGETS)
  # To make frugen 32-bit, set BINARY_32BIT or use an external toolchain file
  if(BINARY_32BIT)
    set_target_properties(${TARGET} PROPERTIES COMPILE_FLAGS "-m32" LINK_FLAGS "-m32")
  endif(BINARY_32BIT)
endforeach()

# install targets
install(TARGETS frugen RUNTIME DESTINATION bin)
if(BUILD_SHARED_LIB)
  install(TARGETS fru-shared DESTINATION lib)
  install(FILES fru.h DESTINATION include)
endif(BUILD_SHARED_LIB)
