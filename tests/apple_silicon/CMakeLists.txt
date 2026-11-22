cmake_minimum_required(VERSION 3.20)
project(ma2_build_graph_tests LANGUAGES C)

if(NOT APPLE OR NOT CMAKE_OSX_ARCHITECTURES STREQUAL "arm64")
    message(FATAL_ERROR "These tests require a native macOS arm64 configuration")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
find_package(Threads REQUIRED)
find_package(SDL2 CONFIG REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
enable_testing()

set(GAME_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../../kit/source/game/src")
if(DEFINED MA2_GENERATED_DIR AND NOT MA2_GENERATED_DIR STREQUAL "")
    message(FATAL_ERROR "The synthetic graph tests must not use generated game inputs")
endif()
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../kit/source/game"
    "${CMAKE_CURRENT_BINARY_DIR}/game" EXCLUDE_FROM_ALL)

add_executable(recomp_compat_test recomp_compat_test.c)
target_include_directories(recomp_compat_test PRIVATE "${GAME_SRC}/apple")
target_link_libraries(recomp_compat_test PRIVATE Threads::Threads)
add_test(NAME recomp_compat COMMAND recomp_compat_test)

add_executable(legacy_pgraph_probe legacy_pgraph_probe.c)
target_link_libraries(legacy_pgraph_probe PRIVATE xbox_nv2a)
add_test(NAME legacy_pgraph_rejected COMMAND "${CMAKE_COMMAND}"
    "-DPROBE=$<TARGET_FILE:legacy_pgraph_probe>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/expect_pgraph_failure.cmake")

set(RUNTIME_TESTS "${CMAKE_CURRENT_SOURCE_DIR}/../../kit/source/runtime/tests/apple_silicon")
add_executable(game_graph_backend_test "${RUNTIME_TESTS}/backend_executor_test.c"
    "${RUNTIME_TESTS}/generated_stubs.c")
target_link_libraries(game_graph_backend_test PRIVATE xboxrecomp SDL2::SDL2)
add_test(NAME game_graph_backend COMMAND game_graph_backend_test)
set_tests_properties(game_graph_backend PROPERTIES
    ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy")

add_executable(host_lifecycle_test host/host_lifecycle_test.c)
target_link_libraries(host_lifecycle_test PRIVATE ma2_host_shell)
target_compile_options(host_lifecycle_test PRIVATE -Wall -Wextra -Wpedantic)
add_dependencies(host_lifecycle_test mechassault2_recomp)
add_test(NAME host_lifecycle COMMAND host_lifecycle_test)
set_tests_properties(host_lifecycle PROPERTIES
    ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy"
    TIMEOUT 20)

add_executable(host_signals_test host/host_signals_test.c)
target_link_libraries(host_signals_test PRIVATE ma2_host_shell)
target_compile_options(host_signals_test PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME host_signals COMMAND host_signals_test)
set_tests_properties(host_signals PROPERTIES TIMEOUT 15)

add_test(NAME host_process COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/host/test_host.py" "$<TARGET_FILE:mechassault2_recomp>")
set_tests_properties(host_process PROPERTIES TIMEOUT 60)
