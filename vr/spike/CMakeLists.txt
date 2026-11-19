# xrspike: feasibility test for a native OpenXR framewall (see xrspike.cpp).
# Built on the headset:  cmake -S vr/spike -B vr/spike/build && cmake --build vr/spike/build
cmake_minimum_required(VERSION 3.16)
project(xrspike CXX)
set(CMAKE_CXX_STANDARD 17)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(TP ${CMAKE_CURRENT_SOURCE_DIR}/../third_party)

# The Khronos OpenXR loader, built from the SDK.
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
set(DYNAMIC_LOADER ON CACHE BOOL "" FORCE)
add_subdirectory(${TP}/openxr-sdk openxr EXCLUDE_FROM_ALL)
find_package(Vulkan REQUIRED)
find_program(GLSLC glslc REQUIRED)

foreach(s panel.vert panel.frag)
  add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${s}.spv
    COMMAND ${GLSLC} -O ${CMAKE_CURRENT_SOURCE_DIR}/${s} -o ${CMAKE_CURRENT_BINARY_DIR}/${s}.spv
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${s})
  list(APPEND SPV ${CMAKE_CURRENT_BINARY_DIR}/${s}.spv)
endforeach()

add_executable(xrspike xrspike.cpp ${SPV})
target_compile_options(xrspike PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
target_link_libraries(xrspike PRIVATE openxr_loader Vulkan::Vulkan)
set_target_properties(xrspike PROPERTIES BUILD_RPATH "\$ORIGIN/openxr/src/loader")
