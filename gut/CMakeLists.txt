# gut: lightweight commit-metadata fixer based on libgit2.
#
# Standalone project: pulls libgit2 in from the parent directory and
# links it statically, producing a single self-contained gut.exe.

cmake_minimum_required(VERSION 3.15)
project(gut C)

# Trim the libgit2 build down to the static library.  gut only touches
# local repositories, so HTTPS/SSH backends (and their TLS dependencies)
# are disabled as well.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Link libgit2 statically" FORCE)
set(BUILD_TESTS       OFF CACHE BOOL "" FORCE)
set(BUILD_CLI         OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES    OFF CACHE BOOL "" FORCE)
set(BUILD_FUZZERS     OFF CACHE BOOL "" FORCE)
set(BUILD_BENCHMARKS  OFF CACHE BOOL "" FORCE)
set(USE_HTTPS         OFF CACHE BOOL "" FORCE)
set(USE_SSH           OFF CACHE BOOL "" FORCE)
set(USE_NTLMCLIENT    OFF CACHE BOOL "NTLM needs an HTTPS backend" FORCE)

add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/.." libgit2 EXCLUDE_FROM_ALL)

add_executable(gut gut.c)
# libgit2's build defaults the whole project to strict C90 (cached
# CMAKE_C_STANDARD/_EXTENSIONS), but gut.c needs va_copy (C99) and
# timegm/localtime_r (POSIX, hidden by glibc in strict-ANSI mode).
set_target_properties(gut PROPERTIES C_STANDARD 99 C_EXTENSIONS ON)
target_compile_definitions(gut PRIVATE GIT_STATIC)
if (MSVC)
	target_compile_definitions(gut PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
target_include_directories(gut PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../include")
target_link_libraries(gut PRIVATE libgit2package)
