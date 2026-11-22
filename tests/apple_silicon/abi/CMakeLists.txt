cmake_minimum_required(VERSION 3.20)
project(ma2_arm64_abi_tests LANGUAGES C CXX)

if(NOT APPLE OR NOT CMAKE_OSX_ARCHITECTURES STREQUAL "arm64")
    message(FATAL_ERROR "The ABI tests require native macOS arm64")
endif()
if(NOT CMAKE_C_COMPILER_ID STREQUAL "AppleClang")
    message(FATAL_ERROR "The ABI tests require Apple Clang")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
enable_testing()
get_filename_component(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(RUNTIME "${ROOT}/kit/source/runtime")
set(APPLE_COMPAT "${ROOT}/kit/source/game/src/apple")
set(MA2_ABI_TYPES_DIR "${ROOT}/kit/toolkit/templates/runtime" CACHE PATH
    "Directory containing the ABI header to test")
if(NOT EXISTS "${MA2_ABI_TYPES_DIR}/recomp_types.h")
    message(FATAL_ERROR "The requested ABI header is missing")
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(ABI_OVERLAY_DIR "${CMAKE_CURRENT_BINARY_DIR}/abi-overlay")
execute_process(
    COMMAND "${Python3_EXECUTABLE}"
        "${APPLE_COMPAT}/abi/prepare_overlay.py"
        --template
        --header "${MA2_ABI_TYPES_DIR}/recomp_types.h"
        --output-dir "${ABI_OVERLAY_DIR}"
    RESULT_VARIABLE ABI_OVERLAY_RESULT
    OUTPUT_QUIET ERROR_QUIET)
if(NOT ABI_OVERLAY_RESULT EQUAL 0)
    message(FATAL_ERROR "The tracked ABI template could not be adapted")
endif()
set(ABI_OVERLAY "${ABI_OVERLAY_DIR}/overlay.json")
if(NOT EXISTS "${ABI_OVERLAY}")
    message(FATAL_ERROR "The ABI overlay was not created")
endif()

set(XBOXRECOMP_BUILD_D3D8 OFF CACHE BOOL "" FORCE)
add_subdirectory("${RUNTIME}" "${CMAKE_CURRENT_BINARY_DIR}/runtime" EXCLUDE_FROM_ALL)
find_package(Threads REQUIRED)

get_target_property(nv2a_sources xbox_nv2a SOURCES)
list(REMOVE_ITEM nv2a_sources nv2a_pgraph_d3d11.c nv2a_pb_replay.c nv2a_pb_test.c)
set_property(TARGET xbox_nv2a PROPERTY SOURCES "${nv2a_sources}")
target_sources(xbox_nv2a PRIVATE "${APPLE_COMPAT}/legacy_pgraph_unavailable.c")

function(abi_executable name)
    add_executable(${name} ${ARGN})
    target_include_directories(${name} PRIVATE "${MA2_ABI_TYPES_DIR}" "${APPLE_COMPAT}"
        "${APPLE_COMPAT}/recomp_compat" "${ROOT}/kit/source/game/src" "${RUNTIME}/src")
    target_compile_options(${name} PRIVATE -Wall -Wextra -Werror
        -Wno-unused-parameter -Werror=pointer-to-int-cast -Werror=int-to-pointer-cast
        -include "${APPLE_COMPAT}/recomp_compat.h"
        -ivfsoverlay "${ABI_OVERLAY}")
    target_link_libraries(${name} PRIVATE Threads::Threads)
endfunction()

abi_executable(abi_numeric numeric.c)
abi_executable(abi_dispatch dispatch.c)
abi_executable(abi_trace_concurrency trace_concurrency.c)
abi_executable(abi_cxx_linkage cxx_linkage.cpp)
target_link_libraries(abi_cxx_linkage PRIVATE platform)
abi_executable(abi_threads threads.c)
target_link_libraries(abi_threads PRIVATE xboxrecomp)
abi_executable(abi_unicode_atomics unicode_atomics.c)
target_link_libraries(abi_unicode_atomics PRIVATE platform)
abi_executable(abi_audio_lifetime audio_lifetime.c
    "${ROOT}/kit/source/game/src/audio_lifetime.c")
target_link_libraries(abi_audio_lifetime PRIVATE platform)

abi_executable(abi_numeric_sanitized numeric.c)
target_compile_options(abi_numeric_sanitized PRIVATE
    -fsanitize=address,undefined -fno-omit-frame-pointer)
target_link_options(abi_numeric_sanitized PRIVATE -fsanitize=address,undefined)

abi_executable(abi_trace_tsan trace_concurrency.c)
target_compile_options(abi_trace_tsan PRIVATE -fsanitize=thread -fno-omit-frame-pointer)
target_link_options(abi_trace_tsan PRIVATE -fsanitize=thread)

add_test(NAME abi_numeric_memory COMMAND abi_numeric memory)
add_test(NAME abi_numeric_float COMMAND abi_numeric float)
add_test(NAME abi_numeric_vectors COMMAND abi_numeric vectors)
add_test(NAME abi_dispatch COMMAND abi_dispatch)
add_test(NAME abi_trace_concurrency COMMAND abi_trace_concurrency)
add_test(NAME abi_cxx_linkage COMMAND abi_cxx_linkage)
add_test(NAME abi_threads COMMAND abi_threads)
add_test(NAME abi_unicode COMMAND abi_unicode_atomics unicode)
add_test(NAME abi_atomics COMMAND abi_unicode_atomics atomics)
add_test(NAME abi_audio_lifetime COMMAND abi_audio_lifetime)
add_test(NAME abi_numeric_sanitized_memory COMMAND abi_numeric_sanitized memory)
add_test(NAME abi_numeric_sanitized_float COMMAND abi_numeric_sanitized float)
add_test(NAME abi_numeric_sanitized_vectors COMMAND abi_numeric_sanitized vectors)
add_test(NAME abi_trace_tsan COMMAND abi_trace_tsan)

set_tests_properties(
    abi_numeric_sanitized_memory
    abi_numeric_sanitized_float
    abi_numeric_sanitized_vectors
    PROPERTIES ENVIRONMENT
        "ASAN_OPTIONS=detect_leaks=0:halt_on_error=1;UBSAN_OPTIONS=halt_on_error=1")
set_tests_properties(abi_trace_tsan PROPERTIES
    ENVIRONMENT "TSAN_OPTIONS=halt_on_error=1")

set(ABI_NATIVE_TARGETS
    abi_numeric
    abi_dispatch
    abi_trace_concurrency
    abi_cxx_linkage
    abi_threads
    abi_unicode_atomics
    abi_audio_lifetime
    abi_numeric_sanitized
    abi_trace_tsan)
foreach(target IN LISTS ABI_NATIVE_TARGETS)
    add_test(NAME "${target}_arm64"
        COMMAND /usr/bin/lipo -verify_arch arm64 "$<TARGET_FILE:${target}>")
endforeach()

add_test(NAME abi_x86_reference
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/run_x86_reference.py"
        --compiler "${CMAKE_C_COMPILER}"
        --source "${CMAKE_CURRENT_SOURCE_DIR}/x86_reference.c"
        --output "${CMAKE_CURRENT_BINARY_DIR}/abi_x86_reference")
set_tests_properties(abi_x86_reference PROPERTIES SKIP_RETURN_CODE 77)

set_tests_properties(
    abi_threads
    abi_trace_concurrency
    abi_trace_tsan
    PROPERTIES TIMEOUT 120)
