# framewall-vr: the wall as separate VR panels (native OpenXR app).
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
if(NOT EXISTS ${TP}/moonlight-common-c/CMakeLists.txt OR NOT EXISTS ${TP}/openxr-sdk/CMakeLists.txt)
  message(FATAL_ERROR "third_party/ is missing: run vr/fetch-deps.sh first")
endif()

# Moonlight's streaming protocol library, built static.
set(BUILD_SHARED_LIBS OFF)
add_subdirectory(${TP}/moonlight-common-c EXCLUDE_FROM_ALL)
# Its -Werror trips over warnings from newer compilers.
target_compile_options(moonlight-common-c PRIVATE -Wno-error)

# The Khronos OpenXR loader, built from the SDK (it finds SteamVR's runtime).
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
set(DYNAMIC_LOADER ON CACHE BOOL "" FORCE)
add_subdirectory(${TP}/openxr-sdk openxr EXCLUDE_FROM_ALL)
find_package(Vulkan REQUIRED)
find_program(GLSLC glslc REQUIRED)

# Shaders, compiled to SPIR-V and included in the binary.
foreach(s wall.vert wall.frag)
  add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${s}.inc
    COMMAND ${GLSLC} -O -mfmt=num ${CMAKE_CURRENT_SOURCE_DIR}/src/${s} -o ${CMAKE_CURRENT_BINARY_DIR}/${s}.inc
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/${s})
  list(APPEND SHADERS ${CMAKE_CURRENT_BINARY_DIR}/${s}.inc)
endforeach()

find_package(PkgConfig REQUIRED)
pkg_check_modules(DEPS REQUIRED IMPORTED_TARGET sdl3 libcurl opus libdrm openssl)

# The settings file written on the first run, from the example.
set(CONF_EXAMPLE ${CMAKE_CURRENT_SOURCE_DIR}/../client/framewall-vr.conf.example)
file(READ ${CONF_EXAMPLE} FRAMEWALL_VR_CONF)
configure_file(src/conf_template.h.in ${CMAKE_CURRENT_BINARY_DIR}/conf_template.h @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CONF_EXAMPLE})

add_executable(framewall-vr
  src/main.cpp
  src/audio.cpp
  src/control.cpp
  src/layout.cpp
  src/menu.cpp
  src/moonlight_conf.cpp
  src/nvhttp.cpp
  src/pointer.cpp
  src/settings.cpp
  src/video.cpp
  src/wallrender.cpp
  src/xr.cpp
  ${SHADERS})
target_compile_options(framewall-vr PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
target_include_directories(framewall-vr PRIVATE
  ${CMAKE_CURRENT_BINARY_DIR}
  ${TP}/moonlight-common-c/src)
target_link_libraries(framewall-vr PRIVATE moonlight-common-c PkgConfig::DEPS openxr_loader Vulkan::Vulkan pthread)
# libopenxr_loader.so is installed next to the binary.
set_target_properties(framewall-vr PROPERTIES BUILD_RPATH "\$ORIGIN" INSTALL_RPATH "\$ORIGIN" BUILD_WITH_INSTALL_RPATH ON)
