cmake_minimum_required(VERSION 3.20)
project(shrek2web C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(EMSCRIPTEN)
  # Every object must use shared memory (the game runs on pthreads).
  add_compile_options(-pthread)
  add_link_options(-pthread)
endif()

# Generated, translated game code (never committed; produced by recomp/src/cli.ts gen-c).
# Builds that link it (native, web dev) are for local development only and are never deployed.
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

set(RUNTIME_SOURCES
  src/cpu_helpers.c
  src/core/core.cpp
  src/core/memory.cpp
  src/core/objects.cpp
  src/hle/kernel32.cpp
  src/hle/kernel32_files.cpp
  src/hle/advapi32.cpp
  src/hle/user32.cpp
  src/hle/com.cpp
  src/hle/dsound.cpp
  src/hle/dinput8.cpp
  src/hle/binkw32.cpp
  src/audio/mixer.cpp
  src/d3d9/direct3d9.cpp
  src/d3d9/device.cpp
  src/d3d9/resources.cpp
  src/d3d9/formats.cpp
  src/d3d9/recorder.cpp
  src/gles/executor.cpp
  src/gles/renderer.cpp
  src/gles/ffp.cpp
  src/gles/sm_vs.cpp)

if(EMSCRIPTEN)
  # Minimal FFmpeg (Bink decoders) built by tools/build-ffmpeg-web.sh.
  set(FFMPEG_WEB "${CMAKE_SOURCE_DIR}/../work/tools/ffmpeg-web" CACHE PATH "Emscripten build of libavcodec/libavutil")
  add_executable(shrek2 ${RUNTIME_SOURCES} src/platform/platform_web.cpp src/web/web_main.cpp)
  target_include_directories(shrek2 PRIVATE include src ${FFMPEG_WEB}/include)
  target_compile_options(shrek2 PRIVATE -Wall -Wextra -Wno-unused-parameter)
  target_link_libraries(shrek2 PRIVATE recomp_game dlmalloc ${FFMPEG_WEB}/lib/libavcodec.a ${FFMPEG_WEB}/lib/libavutil.a)
  set(WEB_EXPORTS "_main,_malloc,_free,_web_render_init,_web_render_tick,_web_audio_ring_create,_web_canvas_size,_web_key,_web_mouse_move,_web_mouse_button,_web_mouse_wheel,_web_focus")
  target_link_options(shrek2 PRIVATE
    # Guest addresses [0, 1 GiB) are identity-mapped into linear memory; Emscripten's own data,
    # stacks and heap start above the guest window.
    -sGLOBAL_BASE=1073741824
    -sINITIAL_MEMORY=2147483648
    -sALLOW_MEMORY_GROWTH=0
    -sSTACK_SIZE=4194304
    -sDEFAULT_PTHREAD_STACK_SIZE=4194304
    # The game's main thread is a pthread; the page's main thread renders and handles input.
    -sPROXY_TO_PTHREAD=1
    -sPTHREAD_POOL_SIZE=8
    -sWASMFS=1
    -sFORCE_FILESYSTEM=1
    -sMIN_WEBGL_VERSION=2
    -sMAX_WEBGL_VERSION=2
    -sMODULARIZE=1
    -sEXPORT_ES6=1
    -sEXPORT_NAME=createShrek2
    -sINVOKE_RUN=0
    -sENVIRONMENT=web,worker
    -sEXPORTED_FUNCTIONS=${WEB_EXPORTS}
    -sEXPORTED_RUNTIME_METHODS=callMain,ccall,cwrap,HEAPU8,HEAPU32,HEAPF32,stringToNewUTF8,ENV
    -sASSERTIONS=1)

  # Player build (deployable): no game code inside; the game is translated in the player's browser
  # and loaded as a separate module (src/web/web_game.cpp).
  add_executable(shrek2_player ${RUNTIME_SOURCES} src/platform/platform_web.cpp src/web/web_main.cpp src/web/web_game.cpp)
  target_compile_definitions(shrek2_player PRIVATE RT_PLAYER=1)
  target_include_directories(shrek2_player PRIVATE include src ${FFMPEG_WEB}/include)
  target_compile_options(shrek2_player PRIVATE -Wall -Wextra -Wno-unused-parameter)
  target_link_libraries(shrek2_player PRIVATE dlmalloc ${FFMPEG_WEB}/lib/libavcodec.a ${FFMPEG_WEB}/lib/libavutil.a)
  set(PLAYER_EXPORTS "${WEB_EXPORTS},_web_game_alloc,_web_game_register,_web_game_import,_web_game_bytes,_web_game_len,_web_game_index")
  get_target_property(WEB_LINK_OPTIONS shrek2 LINK_OPTIONS)
  list(TRANSFORM WEB_LINK_OPTIONS REPLACE "^-sEXPORTED_FUNCTIONS=.*" "-sEXPORTED_FUNCTIONS=${PLAYER_EXPORTS}")
  # The deployable build drops Emscripten's runtime assertions (the development engine keeps them).
  list(TRANSFORM WEB_LINK_OPTIONS REPLACE "^-sASSERTIONS=1$" "-sASSERTIONS=0")
  target_link_options(shrek2_player PRIVATE ${WEB_LINK_OPTIONS} -sALLOW_TABLE_GROWTH=1)
else()
  find_package(SDL2 REQUIRED)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(AVCODEC REQUIRED IMPORTED_TARGET libavcodec libavutil)
  add_executable(shrek2 ${RUNTIME_SOURCES} src/main.cpp src/platform/platform_sdl.cpp)
  target_include_directories(shrek2 PRIVATE include src ../third_party/khronos)
  target_compile_options(shrek2 PRIVATE -Wall -Wextra -Wno-unused-parameter)
  target_link_libraries(shrek2 PRIVATE recomp_game dlmalloc SDL2::SDL2 PkgConfig::AVCODEC GLESv2 pthread m)
endif()
