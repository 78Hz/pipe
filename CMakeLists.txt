cmake_minimum_required(VERSION 3.15)

set(CMAKE_OSX_DEPLOYMENT_TARGET "10.15" CACHE STRING "Minimum macOS version")
project(pipe VERSION 0.2.4 LANGUAGES C CXX ASM)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

if(MINGW)
  # self-contained plugins: no libstdc++/libgcc/winpthread DLLs needed next to the host
  set(PIPE_STATIC "-static -static-libgcc -static-libstdc++")
  set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} ${PIPE_STATIC}")
  set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} ${PIPE_STATIC}")
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${PIPE_STATIC}")
endif()

# Local fixes to DPF, applied to the submodule at configure time (idempotent)
find_package(Git REQUIRED)
foreach(patch ${CMAKE_SOURCE_DIR}/patches/dpf-mpe-input.patch)
  execute_process(COMMAND "${GIT_EXECUTABLE}" apply --ignore-whitespace --reverse --check "${patch}" WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/dpf"
                  RESULT_VARIABLE already OUTPUT_QUIET ERROR_QUIET)
  if(NOT already EQUAL 0)
    execute_process(COMMAND "${GIT_EXECUTABLE}" apply --ignore-whitespace "${patch}" WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/dpf" RESULT_VARIABLE res)
    if(NOT res EQUAL 0)
      message(FATAL_ERROR "Could not apply ${patch} to dpf")
    endif()
    message(STATUS "Pipe: applied ${patch}")
  endif()
endforeach()
add_subdirectory(dpf)

set(PIPE_TARGETS lv2 vst3 clap)
if(APPLE)
  list(APPEND PIPE_TARGETS au)
endif()

# ---- measured rooms (OpenAIR impulse responses, prepared by tools/prepare_irs.py)
set(PIPE_IR_DIR "${CMAKE_BINARY_DIR}/irs" CACHE PATH "Folder with ir_blob.bin and IrMeta.hpp")
if(NOT EXISTS "${PIPE_IR_DIR}/ir_blob.bin" OR NOT EXISTS "${PIPE_IR_DIR}/IrMeta.hpp")
  message(STATUS "Pipe: no measured rooms in ${PIPE_IR_DIR} - building with lite rooms only")
  set(PIPE_IR_DIR "${CMAKE_BINARY_DIR}/irs-empty")
  file(MAKE_DIRECTORY "${PIPE_IR_DIR}")
  file(WRITE "${PIPE_IR_DIR}/ir_blob.bin" "EMPTY")
  file(WRITE "${PIPE_IR_DIR}/IrMeta.hpp" "#pragma once\nnamespace pipe { namespace irmeta {\nstruct Position { int room; int index; const char* name; float distance; float t30Low; float t30High; };\nconstexpr int kCount = 0;\nstatic const Position kPositions[] = { { -1, -1, \"\", 0.f, 0.f, 0.f } };\nstatic const int kAbPair[4][2] = { { -1, -1 }, { -1, -1 }, { -1, -1 }, { -1, -1 } };\nstatic const char* const kCredits[4] = { \"\", \"\", \"\", \"\" };\n}}\n")
else()
  message(STATUS "Pipe: measured rooms from ${PIPE_IR_DIR}")
endif()
set(PIPE_IR_BLOB "${PIPE_IR_DIR}/ir_blob.bin")
configure_file(src/engine/IrBlob.S.in "${CMAKE_BINARY_DIR}/IrBlob.S" @ONLY)
set_property(SOURCE "${CMAKE_BINARY_DIR}/IrBlob.S" APPEND PROPERTY OBJECT_DEPENDS "${PIPE_IR_BLOB}")

add_compile_definitions(PFFFT_STATIC_DEFINE)   # pffft is linked statically
add_library(pipe_dsp_support STATIC
  third_party/pffft/pffft.c
  third_party/pffft/pffft_common.c
  src/engine/IrBlob.cpp
  "${CMAKE_BINARY_DIR}/IrBlob.S")
target_include_directories(pipe_dsp_support PUBLIC third_party/pffft "${PIPE_IR_DIR}")
target_compile_definitions(pipe_dsp_support PRIVATE PIPE_HAVE_IR_BLOB)
set_target_properties(pipe_dsp_support PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
  target_compile_definitions(pipe_dsp_support PRIVATE _USE_MATH_DEFINES)
else()
  target_compile_options(pipe_dsp_support PRIVATE $<$<COMPILE_LANGUAGE:C>:-O3>)
endif()

dpf_add_plugin(Pipe
  TARGETS ${PIPE_TARGETS}
  USE_FILE_BROWSER
  FILES_DSP plugin/PipePlugin.cpp
  FILES_UI  plugin/PipeUI.cpp)

target_include_directories(Pipe PUBLIC plugin src third_party/pffft "${PIPE_IR_DIR}")
target_link_libraries(Pipe-dsp PRIVATE pipe_dsp_support)

if(MSVC)
  target_compile_options(Pipe PUBLIC /fp:fast /O2)
  target_compile_definitions(Pipe PUBLIC _USE_MATH_DEFINES NOMINMAX)
else()
  target_compile_options(Pipe PUBLIC -O3 -ffast-math -fno-finite-math-only)
endif()

# Offline engine checks (no plugin framework needed)
option(PIPE_BUILD_TESTS "Build the offline engine test" ON)
if(PIPE_BUILD_TESTS)
  find_package(Threads REQUIRED)
  # render_test: engine behaviour; pipes_test: every pipe speaks and is in tune (judged on the sound)
  foreach(t render pipes)
    add_executable(pipe_${t}_test tests/${t}_test.cpp)
    target_include_directories(pipe_${t}_test PRIVATE src)
    target_link_libraries(pipe_${t}_test PRIVATE pipe_dsp_support Threads::Threads)
    if(MSVC)
      target_compile_definitions(pipe_${t}_test PRIVATE _USE_MATH_DEFINES NOMINMAX)
    endif()
  endforeach()
endif()
