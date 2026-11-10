cmake_minimum_required(VERSION 3.15)

set(CMAKE_OSX_DEPLOYMENT_TARGET "10.15" CACHE STRING "Minimum macOS version")
project(pipe VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_subdirectory(dpf)

set(PIPE_TARGETS lv2 vst3 clap)
if(APPLE)
  list(APPEND PIPE_TARGETS au)
endif()

dpf_add_plugin(Pipe
  TARGETS ${PIPE_TARGETS}
  USE_FILE_BROWSER
  FILES_DSP plugin/PipePlugin.cpp
  FILES_UI  plugin/PipeUI.cpp)

target_include_directories(Pipe PUBLIC plugin src)

if(MSVC)
  target_compile_options(Pipe PUBLIC /fp:fast /O2)
  target_compile_definitions(Pipe PUBLIC _USE_MATH_DEFINES NOMINMAX)
else()
  target_compile_options(Pipe PUBLIC -O3 -ffast-math -fno-finite-math-only)
endif()

# Offline engine checks (no plugin framework needed)
option(PIPE_BUILD_TESTS "Build the offline engine test" ON)
if(PIPE_BUILD_TESTS)
  add_executable(pipe_render_test tests/render_test.cpp)
  target_include_directories(pipe_render_test PRIVATE src)
  if(MSVC)
    target_compile_definitions(pipe_render_test PRIVATE _USE_MATH_DEFINES NOMINMAX)
  endif()
endif()
