# BCB — Binary Compression by BT
# Copyright (c) 2026 호시 <jahyag@gmail.com>
# Licensed under the MIT License. See LICENSE.
#
# Cross-platform build (Linux / macOS / Windows-MinGW): static + shared library,
# CLI tools, tests, install rules (+ pkg-config + find_package(bcb)).
#
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build
#   ctest --test-dir build --output-on-failure
#   cmake --install build --prefix /usr/local
#
# The existing Makefile keeps working unchanged; this is an additional path.

cmake_minimum_required(VERSION 3.15)
project(bcb VERSION 0.2.0 LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(BCB_BUILD_SHARED "Build shared library (libbcb.so/.dll/.dylib)" ON)
option(BCB_BUILD_STATIC "Build static library (libbcb.a)" ON)
option(BCB_BUILD_TOOLS  "Build CLI tools (bcb-cli, bcb-prior-build)" ON)
option(BCB_BUILD_TESTS  "Build tests + register CTest" ON)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# ── core sources + (internal) include dirs ───────────────────────────────────
set(BCB_CORE_SOURCES
    src/v6_public/bcb_api.c
    src/v0_baseline/ce_compress.c
    src/v0_baseline/bt_model.c
    src/v3_integer_bt/btv3.c
    src/v5_mmap_prior/bcb_prior.c)
set(BCB_INTERNAL_INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/v0_baseline
    ${CMAKE_CURRENT_SOURCE_DIR}/src/v3_integer_bt
    ${CMAKE_CURRENT_SOURCE_DIR}/src/v5_mmap_prior)

# libm only where the math lib is a separate object (not on Windows/macOS).
set(BCB_MATH_LIB "")
if(UNIX AND NOT APPLE)
  set(BCB_MATH_LIB m)
endif()

# Helper: apply common settings to a library target.
function(_bcb_configure_lib tgt)
  target_include_directories(${tgt} PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
      $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
  # internal headers needed only while compiling the library itself
  target_include_directories(${tgt} PRIVATE ${BCB_INTERNAL_INCLUDES})
  target_link_libraries(${tgt} PRIVATE ${BCB_MATH_LIB})
  set_target_properties(${tgt} PROPERTIES OUTPUT_NAME bcb POSITION_INDEPENDENT_CODE ON)
endfunction()

set(BCB_INSTALL_TARGETS "")

if(BCB_BUILD_SHARED)
  add_library(bcb_shared SHARED ${BCB_CORE_SOURCES})
  _bcb_configure_lib(bcb_shared)
  # Only the public API (BCB_API) is exported; everything else hidden.
  set_target_properties(bcb_shared PROPERTIES
      C_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON
      VERSION ${PROJECT_VERSION} SOVERSION ${PROJECT_VERSION_MAJOR})
  target_compile_definitions(bcb_shared PRIVATE BCB_BUILD)   # dllexport / visibility(default)
  add_library(bcb::bcb ALIAS bcb_shared)
  list(APPEND BCB_INSTALL_TARGETS bcb_shared)
endif()

if(BCB_BUILD_STATIC)
  add_library(bcb_static STATIC ${BCB_CORE_SOURCES})
  _bcb_configure_lib(bcb_static)
  # Consumers of the static lib must NOT see dllimport on Windows.
  target_compile_definitions(bcb_static PUBLIC BCB_STATIC)
  if(NOT TARGET bcb::bcb)
    add_library(bcb::bcb ALIAS bcb_static)
  endif()
  list(APPEND BCB_INSTALL_TARGETS bcb_static)
endif()

if(NOT BCB_INSTALL_TARGETS)
  message(FATAL_ERROR "Enable at least one of BCB_BUILD_SHARED / BCB_BUILD_STATIC")
endif()

# Tools/tests link the static lib (it carries the internal symbols they call) and
# need the internal headers. Fall back to shared if static is disabled.
if(TARGET bcb_static)
  set(BCB_LINK_LIB bcb_static)
else()
  set(BCB_LINK_LIB bcb_shared)
endif()

# ── CLI tools ────────────────────────────────────────────────────────────────
if(BCB_BUILD_TOOLS)
  foreach(tool bcb-cli bcb-prior-build)
    add_executable(${tool} tools/${tool}.c)
    target_include_directories(${tool} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include ${BCB_INTERNAL_INCLUDES})
    target_link_libraries(${tool} PRIVATE ${BCB_LINK_LIB} ${BCB_MATH_LIB})
  endforeach()
  install(TARGETS bcb-cli bcb-prior-build RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

# ── tests ────────────────────────────────────────────────────────────────────
if(BCB_BUILD_TESTS)
  enable_testing()

  # self-contained v0 round-trip
  add_executable(test_roundtrip tests/test_roundtrip.c)
  target_include_directories(test_roundtrip PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/v0_baseline)
  target_link_libraries(test_roundtrip PRIVATE ${BCB_LINK_LIB} ${BCB_MATH_LIB})
  add_test(NAME roundtrip COMMAND test_roundtrip)

  # public-API tests linked against the SHARED library (proves the .so/.dll works)
  if(BCB_BUILD_SHARED AND BCB_BUILD_TOOLS)
    add_executable(test_api tests/test_api.c)
    target_include_directories(test_api PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(test_api PRIVATE bcb_shared)

    # Driver builds a prior from the committed corpus, then runs a public-API test
    # against the SHARED library — verifies dynamic-lib (.so/.dll/.dylib)
    # round-trip lossless + corruption detection. Cross-platform via `cmake -P`.
    set(_drv ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/api_roundtrip.cmake)
    add_test(NAME api_shared COMMAND ${CMAKE_COMMAND}
        -DPRIOR_BUILD=$<TARGET_FILE:bcb-prior-build>
        -DTEST_BIN=$<TARGET_FILE:test_api>
        -DCORPUS=${CMAKE_CURRENT_SOURCE_DIR}/tests/corpus/pride_and_prejudice.txt
        -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}
        -DMODE=landmark -P ${_drv})
    # Windows DLL must be discoverable at test time.
    set_tests_properties(api_shared PROPERTIES
        ENVIRONMENT "PATH=$<TARGET_FILE_DIR:bcb_shared>;$ENV{PATH}")

    # The thread test uses POSIX <pthread.h> directly, so it is POSIX-only.
    # (The library itself is thread-safe on every platform; only this test
    # harness is pthread-based.)
    if(NOT WIN32)
      find_package(Threads)
      add_executable(test_threads tests/test_threads.c)
      target_include_directories(test_threads PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
      target_link_libraries(test_threads PRIVATE bcb_shared Threads::Threads)
      add_test(NAME threads_shared COMMAND ${CMAKE_COMMAND}
          -DPRIOR_BUILD=$<TARGET_FILE:bcb-prior-build>
          -DTEST_BIN=$<TARGET_FILE:test_threads> -DTEST_ARGS=8\;2000
          -DCORPUS=${CMAKE_CURRENT_SOURCE_DIR}/tests/corpus/pride_and_prejudice.txt
          -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}
          -DMODE=landmark -P ${_drv})
      set_tests_properties(threads_shared PROPERTIES
          ENVIRONMENT "LD_LIBRARY_PATH=$<TARGET_FILE_DIR:bcb_shared>:$ENV{LD_LIBRARY_PATH}")
    endif()
  endif()
endif()

# ── install + pkg-config + find_package(bcb) ─────────────────────────────────
install(TARGETS ${BCB_INSTALL_TARGETS}
        EXPORT bcbTargets
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES include/bcb.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT bcbTargets
        FILE bcbTargets.cmake NAMESPACE bcb::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/bcb)

configure_package_config_file(
    cmake/bcbConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/bcbConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/bcb)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/bcbConfigVersion.cmake
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/bcbConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/bcbConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/bcb)

# pkg-config (.pc) for non-CMake consumers
set(BCB_PC_LIBM "")
if(BCB_MATH_LIB)
  set(BCB_PC_LIBM "-lm")
endif()
configure_file(cmake/bcb.pc.in ${CMAKE_CURRENT_BINARY_DIR}/bcb.pc @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/bcb.pc
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
