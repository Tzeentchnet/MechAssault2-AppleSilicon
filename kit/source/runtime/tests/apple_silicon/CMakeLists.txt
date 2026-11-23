cmake_minimum_required(VERSION 3.20)
project(ma2_runtime_port_tests C CXX)

if(WIN32)
    message(FATAL_ERROR "These tests exercise the POSIX runtime")
endif()
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(XBOXRECOMP_BUILD_D3D8 OFF CACHE BOOL "" FORCE)
add_subdirectory(../.. runtime)
option(MA2_RUNTIME_TEST_INJECTION "Enable synthetic mapping/protection failures" ON)
if(MA2_RUNTIME_TEST_INJECTION)
    target_compile_definitions(platform PRIVATE XBOXRECOMP_TEST_FAILURES=1)
    target_compile_definitions(xbox_apu PRIVATE XBOXRECOMP_TEST_FAILURES=1)
    target_compile_definitions(xbox_usb PRIVATE XBOXRECOMP_TEST_FAILURES=1)
endif()
find_package(SDL2 REQUIRED)
find_package(Threads REQUIRED)
enable_testing()

add_test(NAME embed_override COMMAND ${CMAKE_COMMAND}
    -S ${CMAKE_CURRENT_SOURCE_DIR}/embed_override
    -B ${CMAKE_CURRENT_BINARY_DIR}/embed_override
    -DOPENSSL_ROOT_DIR=${OPENSSL_ROOT_DIR}
    -DCMAKE_PREFIX_PATH=${CMAKE_PREFIX_PATH}
)

add_executable(mmio_a64_test mmio_a64_test.c)
target_include_directories(mmio_a64_test PRIVATE ../../src/platform)
add_test(NAME mmio_a64 COMMAND mmio_a64_test)
set_tests_properties(mmio_a64 PROPERTIES SKIP_RETURN_CODE 77)

add_executable(mmio_signal_test mmio_signal_test.c ../../src/platform/posix_mmio.c)
target_include_directories(mmio_signal_test PRIVATE ../../src/platform)
target_compile_definitions(mmio_signal_test PRIVATE XBOX_POSIX_MMIO_TESTING=1)
target_link_libraries(mmio_signal_test PRIVATE Threads::Threads)
add_test(NAME mmio_signal COMMAND mmio_signal_test)
set_tests_properties(mmio_signal PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 90)

add_executable(audio_queue_test audio_queue_test.c ../../src/apu/apu_sdl2.c)
target_include_directories(audio_queue_test PRIVATE ../../src/apu)
target_link_libraries(audio_queue_test PRIVATE SDL2::SDL2)
add_test(NAME audio_queue COMMAND audio_queue_test)

add_executable(audio_monitor_test audio_monitor_test.c generated_stubs.c)
target_link_libraries(audio_monitor_test PRIVATE xboxrecomp)
add_test(NAME audio_monitor COMMAND audio_monitor_test)

add_executable(backend_contract_test backend_contract_test.cpp)
target_include_directories(backend_contract_test PRIVATE ../../src/kernel)
target_link_libraries(backend_contract_test PRIVATE xbox_kernel)
add_test(NAME backend_contract COMMAND backend_contract_test)

add_executable(path_partition_test path_partition_test.c
    ../../src/kernel/kernel_path.c ../../src/kernel/kernel_file.c)
target_include_directories(path_partition_test PRIVATE ../../src ../../src/kernel)
target_link_libraries(path_partition_test PRIVATE platform)
add_test(NAME path_partition COMMAND path_partition_test)

add_executable(backend_executor_test backend_executor_test.c generated_stubs.c)
target_link_libraries(backend_executor_test PRIVATE xboxrecomp SDL2::SDL2)
add_test(NAME backend_executor COMMAND backend_executor_test)
add_test(NAME backend_executor_swizzled_surface COMMAND backend_executor_test swizzled-surface)
add_test(NAME backend_executor_strip_culling COMMAND backend_executor_test strip-culling)
add_test(NAME backend_executor_texture_addressing COMMAND backend_executor_test texture-addressing)
add_test(NAME backend_executor_texture_dependencies COMMAND backend_executor_test texture-dependencies)
add_test(NAME backend_executor_combiner_gate COMMAND backend_executor_test combiner-gate)
add_test(NAME backend_executor_parallel_raster COMMAND backend_executor_test parallel-raster)
set_tests_properties(backend_executor PROPERTIES
    ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy")
set_tests_properties(backend_executor_swizzled_surface backend_executor_strip_culling
    backend_executor_texture_addressing backend_executor_texture_dependencies
backend_executor_combiner_gate backend_executor_parallel_raster PROPERTIES
    ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy")

if(APPLE AND CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$" AND
   (NOT CMAKE_OSX_ARCHITECTURES OR CMAKE_OSX_ARCHITECTURES STREQUAL "arm64"))
    enable_language(OBJCXX)
    find_library(MA2_TEST_METAL_FRAMEWORK Metal REQUIRED)
    find_library(MA2_TEST_FOUNDATION_FRAMEWORK Foundation REQUIRED)
    set(MA2_TEST_METAL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../game/src/apple/metal")
    add_executable(metal_backend_test metal_backend_test.cpp generated_stubs.c
        "${MA2_TEST_METAL_DIR}/nv2a_metal.mm" "${MA2_TEST_METAL_DIR}/nv2a_metal_memory.c")
    set_target_properties(metal_backend_test PROPERTIES OBJCXX_STANDARD 17 OBJCXX_STANDARD_REQUIRED ON)
    target_compile_definitions(metal_backend_test PRIVATE MA2_METAL_TESTING=1)
    target_compile_options(metal_backend_test PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
    target_include_directories(metal_backend_test PRIVATE "${MA2_TEST_METAL_DIR}" ../../src)
    target_link_libraries(metal_backend_test PRIVATE xboxrecomp SDL2::SDL2
        "${MA2_TEST_METAL_FRAMEWORK}" "${MA2_TEST_FOUNDATION_FRAMEWORK}")
    add_test(NAME metal_backend COMMAND metal_backend_test)
    add_test(NAME metal_backend_interop_failure COMMAND metal_backend_test interop-failure)
    add_test(NAME metal_backend_color_failure COMMAND metal_backend_test color-failure)
    add_test(NAME metal_backend_depth_readback_failure COMMAND metal_backend_test depth-readback-failure)
    add_test(NAME metal_backend_clean_shutdown COMMAND metal_backend_test clean-shutdown)
    add_test(NAME metal_backend_textures COMMAND metal_backend_test textures)
    add_test(NAME metal_backend_textured_failure COMMAND metal_backend_test textured-failure)
    add_test(NAME metal_backend_alias_clear COMMAND metal_backend_test alias-clear)
    add_test(NAME metal_backend_alias_draw COMMAND metal_backend_test alias-draw)
    add_test(NAME metal_backend_linear_then_swizzled COMMAND metal_backend_test linear-then-swizzled)
    add_test(NAME metal_backend_swizzled_then_linear COMMAND metal_backend_test swizzled-then-linear)
    set_tests_properties(metal_backend metal_backend_interop_failure
        metal_backend_color_failure metal_backend_depth_readback_failure metal_backend_clean_shutdown
        metal_backend_textures metal_backend_textured_failure
        metal_backend_alias_clear metal_backend_alias_draw
        metal_backend_linear_then_swizzled metal_backend_swizzled_then_linear PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy" TIMEOUT 90)
endif()

add_executable(input_hotplug_test input_hotplug_test.c)
target_link_libraries(input_hotplug_test PRIVATE xbox_usb xbox_input SDL2::SDL2)
add_test(NAME input_hotplug COMMAND input_hotplug_test)
set_tests_properties(input_hotplug PROPERTIES ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy")

add_executable(frontend_movie_skip_test frontend_movie_skip_test.c)
target_link_libraries(frontend_movie_skip_test PRIVATE platform)
add_test(NAME frontend_movie_skip COMMAND frontend_movie_skip_test)

add_executable(memory_dispatch_test memory_dispatch_test.c generated_stubs.c)
target_link_libraries(memory_dispatch_test PRIVATE xboxrecomp)
if(MA2_RUNTIME_TEST_INJECTION)
    target_compile_definitions(memory_dispatch_test PRIVATE XBOXRECOMP_TEST_FAILURES=1)
endif()
add_test(NAME memory_dispatch COMMAND memory_dispatch_test)
set_tests_properties(memory_dispatch PROPERTIES
    ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy;RECOMP_HEADLESS=1"
    SKIP_RETURN_CODE 77)

add_executable(framebuffer_host_test framebuffer_host_test.c generated_stubs.c)
target_link_libraries(framebuffer_host_test PRIVATE xboxrecomp SDL2::SDL2)
add_test(NAME framebuffer_host COMMAND framebuffer_host_test)
set_tests_properties(framebuffer_host PROPERTIES ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy")

add_executable(privacy_width_test privacy_width_test.c generated_stubs.c)
target_link_libraries(privacy_width_test PRIVATE xboxrecomp)
add_test(NAME privacy_width COMMAND privacy_width_test)
