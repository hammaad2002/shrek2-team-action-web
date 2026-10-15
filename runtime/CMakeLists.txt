cmake_minimum_required(VERSION 3.20)
project(shrek2web C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Generated, translated game code (never committed; produced by recomp/src/cli.ts gen-c).
set(GEN_DIR "${CMAKE_SOURCE_DIR}/../work/gen-c" CACHE PATH "Directory of recompiled C sources")
set(GEN_OPT "-O1" CACHE STRING "Optimisation flags for the recompiled code")
file(GLOB GEN_SOURCES "${GEN_DIR}/funcs_*.c" "${GEN_DIR}/recomp_table.c")
if(NOT GEN_SOURCES)
  message(FATAL_ERROR "No generated sources in ${GEN_DIR}: run node recomp/src/cli.ts gen-c first")
endif()

add_library(recomp_game STATIC ${GEN_SOURCES})
target_include_directories(recomp_game PUBLIC include ${GEN_DIR})
separate_arguments(GEN_OPT_LIST UNIX_COMMAND "${GEN_OPT}")
target_compile_options(recomp_game PRIVATE -w ${GEN_OPT_LIST})
if(RECOMP_TRACE)
  target_compile_definitions(recomp_game PUBLIC RECOMP_TRACE=1)
endif()

add_library(dlmalloc STATIC ../third_party/dlmalloc/malloc.c)
target_compile_definitions(dlmalloc PRIVATE ONLY_MSPACES=1 MSPACES=1 USE_LOCKS=1 HAVE_MMAP=0 HAVE_MORECORE=0)
target_compile_options(dlmalloc PRIVATE -w -O2)

find_package(SDL2 REQUIRED)

add_executable(shrek2
  src/main.cpp
  src/cpu_helpers.c
  src/core/core.cpp
  src/core/memory.cpp
  src/core/objects.cpp
  src/hle/kernel32.cpp
  src/hle/kernel32_files.cpp
  src/hle/advapi32.cpp
  src/hle/user32.cpp
  src/platform/platform_sdl.cpp)
target_include_directories(shrek2 PRIVATE include src)
target_compile_options(shrek2 PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(shrek2 PRIVATE recomp_game dlmalloc SDL2::SDL2 pthread m)
