# framewall-vr: the wall as separate VR panels (SteamVR overlay app).
# Built on the headset (Steam Frame, aarch64) by vr/build.sh.
#
# Copyright (C) 2026 framewall contributors. GPL-3.0-or-later.
cmake_minimum_required(VERSION 3.16)
project(framewall-vr C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(TP ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
if(NOT EXISTS ${TP}/moonlight-common-c/CMakeLists.txt OR NOT EXISTS ${TP}/openvr-sdk/headers/openvr.h)
  message(FATAL_ERROR "third_party/ is missing: run vr/fetch-deps.sh first")
endif()

# Moonlight's streaming protocol library, built static.
set(BUILD_SHARED_LIBS OFF)
add_subdirectory(${TP}/moonlight-common-c EXCLUDE_FROM_ALL)
# Its -Werror trips over warnings from newer compilers.
target_compile_options(moonlight-common-c PRIVATE -Wno-error)

# OpenVR: the SDK's prebuilt client library for this architecture.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
  set(OPENVR_PLATFORM linuxarm64)
else()
  set(OPENVR_PLATFORM linux64)
endif()
set(OPENVR_LIB ${TP}/openvr-sdk/bin/${OPENVR_PLATFORM}/libopenvr_api.so)

find_package(PkgConfig REQUIRED)
pkg_check_modules(DEPS REQUIRED IMPORTED_TARGET sdl3 libcurl opus egl glesv2 gbm libdrm openssl)

# The settings file written on the first run, from the example.
set(CONF_EXAMPLE ${CMAKE_CURRENT_SOURCE_DIR}/../client/framewall-vr.conf.example)
file(READ ${CONF_EXAMPLE} FRAMEWALL_VR_CONF)
configure_file(src/conf_template.h.in ${CMAKE_CURRENT_BINARY_DIR}/conf_template.h @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CONF_EXAMPLE})

add_executable(framewall-vr
  src/main.cpp
  src/audio.cpp
  src/glconvert.cpp
  src/layout.cpp
  src/moonlight_conf.cpp
  src/nvhttp.cpp
  src/video.cpp)
target_compile_options(framewall-vr PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
target_include_directories(framewall-vr PRIVATE
  ${CMAKE_CURRENT_BINARY_DIR}
  ${TP}/openvr-sdk/headers
  ${TP}/moonlight-common-c/src)
target_link_libraries(framewall-vr PRIVATE moonlight-common-c PkgConfig::DEPS ${OPENVR_LIB} pthread)
# libopenvr_api.so is installed next to the binary.
set_target_properties(framewall-vr PROPERTIES BUILD_RPATH "\$ORIGIN" INSTALL_RPATH "\$ORIGIN" BUILD_WITH_INSTALL_RPATH ON)
