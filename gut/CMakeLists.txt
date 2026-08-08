# gut: lightweight commit-metadata fixer based on libgit2.
#
# Standalone project: pulls libgit2 in from the parent directory and
# links it statically, producing a single self-contained gut.exe.

cmake_minimum_required(VERSION 3.15)
project(gut C)

# Trim the libgit2 build down to the static library.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Link libgit2 statically" FORCE)
set(BUILD_TESTS       OFF CACHE BOOL "" FORCE)
set(BUILD_CLI         OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES    OFF CACHE BOOL "" FORCE)
set(BUILD_FUZZERS     OFF CACHE BOOL "" FORCE)
set(BUILD_BENCHMARKS  OFF CACHE BOOL "" FORCE)

add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/.." libgit2 EXCLUDE_FROM_ALL)

add_executable(gut gut.c)
target_compile_definitions(gut PRIVATE _CRT_SECURE_NO_WARNINGS GIT_STATIC)
target_include_directories(gut PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../include")
target_link_libraries(gut PRIVATE libgit2package)
