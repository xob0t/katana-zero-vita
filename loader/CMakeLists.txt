cmake_minimum_required(VERSION 3.5)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()

project(YoYoLoader C CXX)
include("${VITASDK}/share/vita.cmake" REQUIRED)
set(VITA_APP_NAME "YoYo Loader")
set(VITA_TITLEID  "YYOLOADER")

# Dedicated Katana ZERO app: the loader is the eboot, game data stays in ux0:data/gms/katanazero/
if (KATANA_APP)
set(LOADER 1)
set(VITA_APP_NAME "Katana ZERO")
set(VITA_TITLEID  "KTNZ00001")
endif(KATANA_APP)
# App version (param.sfo APP_VER, shown by the Vita) from the latest release tag: vMAJOR.MINOR.PATCH -> "MM.mp", e.g.
# v1.3.0 -> 01.30, v1.2.1 -> 01.21 (APP_VER allows only XX.YY). Without a tag, or with a part over 9: 01.00
execute_process(COMMAND git describe --tags --abbrev=0 --match "v[0-9]*"
	WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
	OUTPUT_VARIABLE RELEASE_TAG
	OUTPUT_STRIP_TRAILING_WHITESPACE
	ERROR_QUIET
)
set(VITA_VERSION "01.00")
if (RELEASE_TAG MATCHES "^v([0-9])\\.([0-9])\\.([0-9])$")
	set(VITA_VERSION "0${CMAKE_MATCH_1}.${CMAKE_MATCH_2}${CMAKE_MATCH_3}")
elseif (RELEASE_TAG MATCHES "^v([0-9])\\.([0-9])$")
	set(VITA_VERSION "0${CMAKE_MATCH_1}.${CMAKE_MATCH_2}0")
endif()
message(STATUS "App version ${VITA_VERSION} (tag ${RELEASE_TAG})")
set(VITA_MKSFOEX_FLAGS "-d ATTRIBUTE2=12")

execute_process(COMMAND git describe --abbrev=6 --dirty --always
	OUTPUT_VARIABLE GIT_HASH
	OUTPUT_STRIP_TRAILING_WHITESPACE
)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fno-optimize-sibling-calls -fdiagnostics-color=always -g -Wl,-q,--wrap,memcpy,--allow-multiple-definition,--wrap,memmove,--wrap,memset,--wrap,kuKernelCpuUnrestrictedMemcpy -Wno-format-overflow -D_GNU_SOURCE -Wall -O3 -mfloat-abi=softfp")
set(CMAKE_CXX_FLAGS "${CMAKE_C_FLAGS} -fdiagnostics-color=always -Wno-write-strings -DGIT_VERSION=\"${GIT_HASH}\" -std=c++11")

if (LOADER)
if (VIDEO_SUPPORT)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DHAS_VIDEO_PLAYBACK_SUPPORT")
endif(VIDEO_SUPPORT)

if (KATANA_APP)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DAPP_GAME_NAME=\\\"katanazero\\\"")
endif(KATANA_APP)

if (STANDALONE)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DSTANDALONE_MODE")
endif(STANDALONE)

include(${CMAKE_CURRENT_SOURCE_DIR}/exact_sources.cmake)

# Every loader/*.c is part of the app; a new file needs no edit here (CONFIGURE_DEPENDS re-runs the glob at build time)
file(GLOB APP_SOURCES RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} CONFIGURE_DEPENDS loader/*.c)
# qoi_decoder.c belongs to the selector build below
list(REMOVE_ITEM APP_SOURCES loader/qoi_decoder.c)
add_executable(YoYoLoader ${APP_SOURCES} loader/lz4/lz4.c)
# page_reserve.c hands held heap blocks to vitaGL's newlib fallback (its memalign)
target_link_options(YoYoLoader PRIVATE -Wl,--wrap,memalign)

target_link_libraries(YoYoLoader
  -Wl,--whole-archive pthread -Wl,--no-whole-archive
  OpenSLES
  sndfile
  openal
  m
  z
  FLAC
  vorbis
  vorbisfile
  vorbisenc
  ogg
  vitaGL
  stdc++
  vitashark
  SceShaccCgExt
  mathneon
  mpg123
  curl
  ssl
  crypto
  taihen_stub
  kubridge_stub
  SceAppMgr_stub
  SceAppUtil_stub
  SceAudio_stub
  SceAudioIn_stub
  SceCtrl_stub
  SceCommonDialog_stub
  SceDisplay_stub
  SceKernelDmacMgr_stub
  SceFios2_stub
  SceGxm_stub
  SceMotion_stub
  SceNet_stub
  SceRegistryMgr_stub
  SceShaccCg_stub
  SceSysmodule_stub
  ScePower_stub
  SceTouch_stub
  SceVshBridge_stub
  SceAvPlayer_stub
  SceNpTrophy_stub
)

if (KATANA_APP)
# LiveArea art is game art: generated from the user's APK by scripts/make_livearea.py (build.py runs it), not committed
set(LIVEAREA_ART ${CMAKE_SOURCE_DIR}/../livearea)
foreach(image icon0.png bg0.png startup.png)
  if (NOT EXISTS ${LIVEAREA_ART}/${image})
    message(FATAL_ERROR "livearea/${image} missing: run uv run build.py <Katana ZERO APK>")
  endif()
endforeach()
vita_create_self(eboot.bin YoYoLoader UNSAFE)
vita_create_vpk(KatanaZero.vpk ${VITA_TITLEID} eboot.bin
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  FILE ${LIVEAREA_ART}/icon0.png sce_sys/icon0.png
       ${LIVEAREA_ART}/bg0.png sce_sys/livearea/contents/bg0.png
       ${LIVEAREA_ART}/startup.png sce_sys/livearea/contents/startup.png
       ${CMAKE_SOURCE_DIR}/../livearea/template.xml sce_sys/livearea/contents/template.xml
)
else()
vita_create_self(loader2.bin YoYoLoader UNSAFE)
endif(KATANA_APP)
else()
add_executable(YoYoLoader
  selector/main.cpp
  selector/dialogs.cpp
  selector/strings.cpp
  selector/player.cpp
  loader/qoi_decoder.c
  loader/ioapi.c
  loader/unzip.c
  loader/zip.c
  loader/dialog.c
  loader/so_util.c
  loader/so_elf.c
)

target_link_libraries(YoYoLoader
  -Wl,--whole-archive pthread -Wl,--no-whole-archive
  OpenSLES
  sndfile
  FLAC
  vorbis
  vorbisenc
  ogg
  imgui
  m
  z
  stdc++
  vitaGL
  vitashark
  SceShaccCgExt
  mathneon
  curl
  ssl
  crypto
  taihen_stub
  kubridge_stub
  SceAppMgr_stub
  SceAppUtil_stub
  SceAudio_stub
  SceAudioIn_stub
  SceCtrl_stub
  SceCommonDialog_stub
  SceDisplay_stub
  SceKernelDmacMgr_stub
  SceGxm_stub
  SceNet_stub
  SceShaccCg_stub
  SceSysmodule_stub
  ScePower_stub
  SceTouch_stub
  SceVshBridge_stub
  SceAvPlayer_stub
)

vita_create_self(eboot.bin YoYoLoader UNSAFE)
vita_create_vpk(YoYoLoader.vpk ${VITA_TITLEID} eboot.bin
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  FILE ${CMAKE_SOURCE_DIR}/livearea/icon0.png sce_sys/icon0.png
       ${CMAKE_SOURCE_DIR}/livearea/default_gate.png sce_sys/livearea/contents/default_gate.png
       ${CMAKE_SOURCE_DIR}/livearea/bg0.png sce_sys/livearea/contents/bg0.png
       ${CMAKE_SOURCE_DIR}/livearea/template.xml sce_sys/livearea/contents/template.xml
       ${CMAKE_BINARY_DIR}/loader.bin loader.bin
       ${CMAKE_BINARY_DIR}/loader2.bin loader2.bin
       ${CMAKE_SOURCE_DIR}/translations/Czech.ini lang/Czech.ini
       ${CMAKE_SOURCE_DIR}/translations/English.ini lang/English.ini
       ${CMAKE_SOURCE_DIR}/translations/German.ini lang/German.ini
       ${CMAKE_SOURCE_DIR}/translations/Italian.ini lang/Italian.ini
       ${CMAKE_SOURCE_DIR}/translations/Spanish.ini lang/Spanish.ini
       ${CMAKE_SOURCE_DIR}/translations/Russian.ini lang/Russian.ini
       ${CMAKE_SOURCE_DIR}/translations/Ukrainian.ini lang/Ukrainian.ini
       ${CMAKE_SOURCE_DIR}/translations/Portuguese_BR.ini lang/Portuguese_BR.ini
       ${CMAKE_SOURCE_DIR}/translations/Ryukyuan.ini lang/Ryukyuan.ini
       ${CMAKE_SOURCE_DIR}/translations/Japanese.ini lang/Japanese.ini
       ${CMAKE_SOURCE_DIR}/translations/Chinese_Simplified.ini lang/Chinese_Simplified.ini
       ${CMAKE_SOURCE_DIR}/translations/Chinese_Traditional.ini lang/Chinese_Traditional.ini
       ${CMAKE_SOURCE_DIR}/Roboto.ttf Roboto.ttf
)
endif()
