cmake_minimum_required(VERSION 3.20)

if(CMAKE_HOST_APPLE AND (NOT DEFINED CMAKE_SYSTEM_NAME OR CMAKE_SYSTEM_NAME STREQUAL "Darwin")
   AND NOT DEFINED CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES arm64 CACHE STRING "macOS target architecture")
endif()

project(mechassault2_recomp LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(cmake/GeneratedInputs.cmake)
include(cmake/AssertBuildGraph.cmake)

if(WIN32)
    enable_language(RC)
    add_executable(mechassault2_launcher WIN32 src/launcher.c src/app.rc)
    set_source_files_properties(src/app.rc PROPERTIES INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/src")
    set_target_properties(mechassault2_launcher PROPERTIES
        OUTPUT_NAME "MechAssault 2"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/launcher")
    add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX)
    set(_ma2_default_runtime "${CMAKE_CURRENT_SOURCE_DIR}/..")
    set(_ma2_default_generated "${CMAKE_CURRENT_SOURCE_DIR}/src/recomp/gen")
elseif(APPLE)
    if(NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin" OR
       NOT CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
        message(FATAL_ERROR "The macOS game graph requires a native arm64 macOS host")
    endif()
    if(NOT CMAKE_OSX_ARCHITECTURES STREQUAL "arm64")
        message(FATAL_ERROR "The macOS game graph requires exactly CMAKE_OSX_ARCHITECTURES=arm64")
    endif()
    enable_language(OBJC OBJCXX)
    foreach(_language IN ITEMS C CXX OBJC OBJCXX)
        if(NOT CMAKE_${_language}_COMPILER_ID STREQUAL "AppleClang")
            message(FATAL_ERROR "The macOS game graph requires Apple Clang for ${_language}")
        endif()
    endforeach()
    if(DEFINED XBOXRECOMP_BUILD_D3D8 AND XBOXRECOMP_BUILD_D3D8)
        message(FATAL_ERROR "The macOS game graph requires XBOXRECOMP_BUILD_D3D8=OFF")
    endif()
    set(XBOXRECOMP_BUILD_D3D8 OFF CACHE BOOL "Build the bundled D3D8 layer")
    set(_ma2_default_runtime "${CMAKE_CURRENT_SOURCE_DIR}/../runtime")
    set(_ma2_default_generated "")
    find_package(SDL2 CONFIG REQUIRED)
    find_package(OpenSSL REQUIRED COMPONENTS Crypto)
    find_library(MA2_APPKIT_FRAMEWORK AppKit REQUIRED)
    find_library(MA2_FOUNDATION_FRAMEWORK Foundation REQUIRED)
    find_library(MA2_METAL_FRAMEWORK Metal REQUIRED)
    find_library(MA2_QUARTZCORE_FRAMEWORK QuartzCore REQUIRED)
    foreach(_framework IN ITEMS MA2_APPKIT_FRAMEWORK MA2_FOUNDATION_FRAMEWORK
        MA2_METAL_FRAMEWORK MA2_QUARTZCORE_FRAMEWORK)
        if(NOT EXISTS "${${_framework}}")
            message(FATAL_ERROR "Required macOS framework ${_framework} was not found")
        endif()
    endforeach()
    find_program(MA2_XCRUN_EXECUTABLE xcrun REQUIRED)
    function(ma2_discover_apple_tool name variable)
        if(NOT DEFINED ${variable})
            execute_process(COMMAND "${MA2_XCRUN_EXECUTABLE}" --sdk macosx --find "${name}"
                RESULT_VARIABLE _result OUTPUT_VARIABLE _path ERROR_QUIET
                OUTPUT_STRIP_TRAILING_WHITESPACE)
            if(NOT "${_result}" STREQUAL "0" OR NOT EXISTS "${_path}")
                message(FATAL_ERROR "The macOS toolchain does not provide ${name}")
            endif()
            set(${variable} "${_path}" CACHE FILEPATH "macOS ${name} tool")
        endif()
        if(NOT EXISTS "${${variable}}" OR IS_DIRECTORY "${${variable}}")
            message(FATAL_ERROR "The configured macOS ${name} tool does not exist")
        endif()
        execute_process(COMMAND "${${variable}}" --version RESULT_VARIABLE _result
            OUTPUT_QUIET ERROR_QUIET)
        if(NOT "${_result}" STREQUAL "0")
            message(FATAL_ERROR "The configured macOS ${name} tool is not executable")
        endif()
    endfunction()
    ma2_discover_apple_tool(metal MA2_METAL_COMPILER)
    ma2_discover_apple_tool(metallib MA2_METALLIB_EXECUTABLE)

    function(ma2_check_apple_toolchain)
        include(CheckCSourceCompiles)
        include(CheckOBJCSourceCompiles)
        include(CheckOBJCXXSourceCompiles)
        set(CMAKE_TRY_COMPILE_TARGET_TYPE EXECUTABLE)
        set(CMAKE_REQUIRED_LIBRARIES SDL2::SDL2 OpenSSL::Crypto)
        unset(MA2_APPLE_DEPENDENCIES_LINK CACHE)
        check_c_source_compiles([=[
            #include <SDL.h>
            #include <openssl/crypto.h>
            #if !defined(__APPLE__) || !defined(__aarch64__)
            #error Expected native arm64 macOS
            #endif
            int main(void) {
                SDL_version version;
                SDL_GetVersion(&version);
                return OPENSSL_init_crypto(0, 0) == 1 ? 0 : 1;
            }
        ]=] MA2_APPLE_DEPENDENCIES_LINK)
        set(CMAKE_REQUIRED_LIBRARIES "${MA2_APPKIT_FRAMEWORK}" "${MA2_FOUNDATION_FRAMEWORK}")
        unset(MA2_APPLE_OBJC_LINK CACHE)
        check_objc_source_compiles([=[
            #import <AppKit/AppKit.h>
            int main(void) {
                @autoreleasepool { return [NSApplication sharedApplication] == nil; }
            }
        ]=] MA2_APPLE_OBJC_LINK)
        set(CMAKE_REQUIRED_LIBRARIES "${MA2_METAL_FRAMEWORK}" "${MA2_QUARTZCORE_FRAMEWORK}"
            "${MA2_FOUNDATION_FRAMEWORK}")
        unset(MA2_APPLE_OBJCXX_METAL_LINK CACHE)
        check_objcxx_source_compiles([=[
            #import <Metal/Metal.h>
            #import <QuartzCore/CAMetalLayer.h>
            int main() {
                @autoreleasepool {
                    return MTLCreateSystemDefaultDevice() == nil || [CAMetalLayer layer] == nil;
                }
            }
        ]=] MA2_APPLE_OBJCXX_METAL_LINK)
        if(NOT MA2_APPLE_DEPENDENCIES_LINK OR NOT MA2_APPLE_OBJC_LINK OR
           NOT MA2_APPLE_OBJCXX_METAL_LINK)
            message(FATAL_ERROR "The native SDL2/OpenSSL or Objective-C/AppKit/Metal toolchain cannot link")
        endif()
    endfunction()
    ma2_check_apple_toolchain()
else()
    message(FATAL_ERROR "The MechAssault 2 game graph supports Windows or native macOS arm64")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(XBOXRECOMP_DIR "${_ma2_default_runtime}" CACHE PATH "Path to the xboxrecomp runtime")
set(MA2_GENERATED_DIR "${_ma2_default_generated}" CACHE PATH
    "Directory containing the patched src/recomp/gen sources; empty disables them on macOS")
if(MA2_GENERATED_DIR)
    get_filename_component(MA2_GENERATED_DIR "${MA2_GENERATED_DIR}" ABSOLUTE)
    ma2_generated_include_root("${MA2_GENERATED_DIR}" MA2_GENERATED_INCLUDE_ROOT)
    if(APPLE)
        set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
        set(MA2_PATCH_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../patches" CACHE PATH
            "Directory containing the shipped patch and FastCode manifests")
        ma2_validate_generated_inputs("${MA2_GENERATED_DIR}" "${MA2_PATCH_DIR}")
        ma2_prepare_apple_abi_overlay("${MA2_GENERATED_DIR}" MA2_ABI_OVERLAY)
    endif()
    # Keep the original non-recursive *.c discovery, including FastCode and
    # generated stubs, and reconfigure when sources are added or removed.
    file(GLOB RECOMP_GEN_SOURCES CONFIGURE_DEPENDS "${MA2_GENERATED_DIR}/*.c")
else()
    set(RECOMP_GEN_SOURCES)
    if(APPLE)
        message(STATUS "Generated game sources and their manual hooks are disabled: set MA2_GENERATED_DIR to enable them")
    else()
        message(FATAL_ERROR "The Windows game graph requires MA2_GENERATED_DIR")
    endif()
endif()

if(APPLE)
    add_subdirectory("${XBOXRECOMP_DIR}" "${CMAKE_BINARY_DIR}/xboxrecomp" EXCLUDE_FROM_ALL)
else()
    add_subdirectory("${XBOXRECOMP_DIR}" "${CMAKE_BINARY_DIR}/xboxrecomp")
endif()

if(MA2_GENERATED_DIR)
    set(MA2_LEGACY_XMM_NAMES
        recomp_0002.c recomp_0034.c recomp_0043.c recomp_0054.c recomp_0057.c
        recomp_0059.c recomp_0060.c recomp_0061.c recomp_0074.c recomp_0076.c
        recomp_0080.c recomp_0081.c recomp_0082.c recomp_0083.c recomp_0084.c
        recomp_0085.c recomp_0087.c recomp_0088.c recomp_0089.c recomp_0090.c
        recomp_0091.c recomp_0092.c recomp_0093.c recomp_0099.c recomp_0100.c
        recomp_bringup_0000.c recomp_bringup_0001.c)
    set(MA2_LEGACY_XMM_SOURCES)
    foreach(_name IN LISTS MA2_LEGACY_XMM_NAMES)
        list(APPEND MA2_LEGACY_XMM_SOURCES "${MA2_GENERATED_DIR}/${_name}")
    endforeach()
    set_property(SOURCE ${MA2_LEGACY_XMM_SOURCES}
        APPEND PROPERTY COMPILE_DEFINITIONS RECOMP_XMM_INTRIN=0)
    set(MA2_NOINLINE_SOURCES
        "${MA2_GENERATED_DIR}/recomp_0057.c" "${MA2_GENERATED_DIR}/recomp_0059.c")
    if(WIN32)
        set_source_files_properties(${MA2_NOINLINE_SOURCES} PROPERTIES COMPILE_OPTIONS /Ob0)
    else()
        set_source_files_properties(${MA2_NOINLINE_SOURCES}
            PROPERTIES COMPILE_OPTIONS -fno-inline-functions)
    endif()
endif()

if(WIN32)
    set(GAME_SOURCES
        src/main.c
        src/recomp_manual.c
        src/audio_lifetime.c
        src/pc_native_settings.cpp
        src/pc_draw_distance.c
        src/pc_gui_bridge.c
        src/pc_prompts.cpp
        src/pc_storage.c
        src/bink_native.c
        src/ma2_tank_fixedstep.c
        src/ma2_tank_trace.c
        src/ma2_cinematic.c
        src/bots/ma2_bots_core.c
        src/bots/ma2_bot_mapid.c
        src/bots/ma2_bot_nav.c
        src/bots/ma2_bot_brain.c
        src/bots/ma2_bots_ui.c
        src/bots/ma2_bot_content.c
        src/bots/ma2_bot_terrain.c
        src/bots/ma2_bot_path.c
        src/scenario/ma2_scenario.c
        src/scenario/ma2_scenario_audio.cpp
        src/app.rc
        vendor/imgui-1.92.5/imgui.cpp
        vendor/imgui-1.92.5/imgui_draw.cpp
        vendor/imgui-1.92.5/imgui_tables.cpp
        vendor/imgui-1.92.5/imgui_widgets.cpp
        vendor/imgui-1.92.5/backends/imgui_impl_win32.cpp
        vendor/imgui-1.92.5/backends/imgui_impl_dx11.cpp)
    add_executable(${PROJECT_NAME} WIN32 ${GAME_SOURCES} ${RECOMP_GEN_SOURCES})
    set_property(SOURCE vendor/imgui-1.92.5/backends/imgui_impl_win32.cpp
        APPEND PROPERTY COMPILE_DEFINITIONS IMGUI_IMPL_WIN32_DISABLE_GAMEPAD)
    target_link_libraries(${PROJECT_NAME} PRIVATE xboxrecomp)

    set(FFMPEG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendor/ffmpeg-9.0.2-full_build-shared")
    set_source_files_properties(src/bink_native.c PROPERTIES INCLUDE_DIRECTORIES "${FFMPEG_DIR}/include")
    target_link_libraries(${PROJECT_NAME} PRIVATE
        "${FFMPEG_DIR}/lib/avcodec.lib" "${FFMPEG_DIR}/lib/avformat.lib"
        "${FFMPEG_DIR}/lib/avutil.lib" "${FFMPEG_DIR}/lib/swscale.lib")
    foreach(dll avcodec-63 avformat-63 avutil-61 swresample-7 swscale-10)
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${FFMPEG_DIR}/bin/${dll}.dll" "${CMAKE_CURRENT_BINARY_DIR}")
    endforeach()
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${FFMPEG_DIR}/LICENSE" "${CMAKE_CURRENT_BINARY_DIR}/FFmpeg-GPL-LICENSE.txt"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/vendor/imgui-1.92.5/LICENSE.txt" "${CMAKE_CURRENT_BINARY_DIR}/ImGui-MIT-LICENSE.txt")
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${XBOXRECOMP_DIR}/src/apu/libsamplerate/COPYING"
            "${CMAKE_CURRENT_BINARY_DIR}/libsamplerate-LICENSE.txt"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${XBOXRECOMP_DIR}/src/apu/dsp/COPYING"
            "${CMAKE_CURRENT_BINARY_DIR}/DSP-GPL-LICENSE.txt"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${XBOXRECOMP_DIR}/src/apu/dsp/COPYING.LIB"
            "${CMAKE_CURRENT_BINARY_DIR}/runtime-LGPL-LICENSE.txt")
    target_include_directories(${PROJECT_NAME} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/bots"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/recomp"
        "${XBOXRECOMP_DIR}/include"
        "${XBOXRECOMP_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/vendor/imgui-1.92.5"
        "${MA2_GENERATED_INCLUDE_ROOT}")
    if(NOT MA2_GENERATED_DIR STREQUAL "${_ma2_default_generated}")
        ma2_add_generated_include_overlay(${PROJECT_NAME})
    endif()
    target_link_libraries(${PROJECT_NAME} PRIVATE d3d11 dxgi dxguid xinput winmm dbghelp)
    if(MSVC)
        foreach(application ${PROJECT_NAME} mechassault2_launcher)
            target_link_options(${application} PRIVATE /MANIFEST:EMBED
                "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/src/app.manifest")
        endforeach()
        target_link_options(${PROJECT_NAME} PRIVATE /LARGEADDRESSAWARE /MAP)
        target_compile_options(${PROJECT_NAME} PRIVATE /bigobj)
    endif()
    ma2_assert_windows_graph()
else()
    if(TARGET xbox_d3d8)
        message(FATAL_ERROR "A pre-existing xbox_d3d8 target cannot be used by the macOS game graph")
    endif()
    # The legacy sample translator requires the omitted D3D8 device. MA2 uses
    # the runtime's NV2A executor/backend instead; reaching the old path fails.
    get_target_property(_nv2a_sources xbox_nv2a SOURCES)
    foreach(_legacy IN ITEMS nv2a_pgraph_d3d11.c nv2a_pb_replay.c nv2a_pb_test.c)
        if(NOT _legacy IN_LIST _nv2a_sources)
            message(FATAL_ERROR "Unexpected xbox_nv2a source graph: ${_legacy} is missing")
        endif()
        list(REMOVE_ITEM _nv2a_sources "${_legacy}")
    endforeach()
    set_property(TARGET xbox_nv2a PROPERTY SOURCES "${_nv2a_sources}")
    target_sources(xbox_nv2a PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/apple/legacy_pgraph_unavailable.c")

    add_library(ma2_apple_host INTERFACE)
    target_link_libraries(ma2_apple_host INTERFACE SDL2::SDL2
        "${MA2_APPKIT_FRAMEWORK}" "${MA2_FOUNDATION_FRAMEWORK}" "${MA2_QUARTZCORE_FRAMEWORK}")
    add_library(ma2_apple_metal STATIC
        src/apple/metal/nv2a_metal.mm src/apple/metal/nv2a_metal_memory.c)
    set_target_properties(ma2_apple_metal PROPERTIES
        OBJCXX_STANDARD 17 OBJCXX_STANDARD_REQUIRED ON)
    target_include_directories(ma2_apple_metal PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/src/apple/metal"
        "${XBOXRECOMP_DIR}/src")
    target_compile_options(ma2_apple_metal PRIVATE
        $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc> -Wall -Wextra -Wpedantic)
    target_link_libraries(ma2_apple_metal PUBLIC
        "${MA2_METAL_FRAMEWORK}" "${MA2_FOUNDATION_FRAMEWORK}")

    set(MA2_PORTABLE_GAME_SOURCES
        src/audio_lifetime.c
        src/pc_draw_distance.c
        src/apple/ma2_bot_brain_compile.c
        src/bots/ma2_bot_content.c
        src/bots/ma2_bot_path.c
        src/bots/ma2_bot_terrain.c)
    if(MA2_GENERATED_DIR)
        list(APPEND MA2_PORTABLE_GAME_SOURCES
            src/recomp_manual.c src/ma2_tank_fixedstep.c src/bots/ma2_bots_ui.c
            src/apple/guest/optional_contracts.c)
    endif()
    add_library(ma2_game_core OBJECT ${MA2_PORTABLE_GAME_SOURCES} ${RECOMP_GEN_SOURCES})
    set_target_properties(ma2_game_core PROPERTIES EXCLUDE_FROM_ALL TRUE)
    target_link_libraries(ma2_game_core PUBLIC xboxrecomp)
    target_include_directories(ma2_game_core PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/bots"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/recomp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/apple/recomp_compat"
        "${XBOXRECOMP_DIR}/include"
        "${XBOXRECOMP_DIR}/src"
        "${XBOXRECOMP_DIR}/src/d3d")
    target_compile_options(ma2_game_core PRIVATE
        -include "${CMAKE_CURRENT_SOURCE_DIR}/src/apple/recomp_compat.h")
    if(MA2_GENERATED_DIR)
        target_include_directories(ma2_game_core PRIVATE "${MA2_GENERATED_INCLUDE_ROOT}")
        ma2_add_generated_include_overlay(ma2_game_core)
        target_compile_options(ma2_game_core PRIVATE -ivfsoverlay "${MA2_ABI_OVERLAY}")
        target_compile_definitions(ma2_game_core PRIVATE RECOMP_XMM_INTRIN=0)
        message(STATUS "Generated game sources, FastCode and manual hooks are enabled (compile-only)")
    endif()
    add_custom_target(ma2_macos_core)
    add_dependencies(ma2_macos_core ma2_game_core platform xbox_kernel xbox_dsound
        xbox_input xbox_usb xbox_apu xbox_nv2a xbox_video apu_samplerate)

    add_library(ma2_host_shell STATIC
        src/apple/host/host_clock.c
        src/apple/host/host_log.c
        src/apple/host/host_options.c
        src/apple/host/host_paths.c
        src/apple/host/host_signals.c
        src/apple/host/host_window.c
        src/apple/host/host_worker.c)
    target_include_directories(ma2_host_shell PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/src/apple/host")
    target_compile_options(ma2_host_shell PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(ma2_host_shell PUBLIC ma2_apple_host xboxrecomp)
    # The window implementation shares runtime archives with guest-only code.
    # Discard unused guest sections rather than supplying fake guest callbacks.
    target_link_options(ma2_host_shell INTERFACE "-Wl,-dead_strip")

    add_executable(${PROJECT_NAME} src/apple/host/host_main.c)
    target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(${PROJECT_NAME} PRIVATE ma2_host_shell)
    if(MA2_GENERATED_DIR)
        add_executable(mechassault2_recomp_guest
            src/apple/host/host_main.c
            src/apple/guest/guest_bootstrap.c
            src/apple/guest/guest_signals.c)
        target_compile_definitions(mechassault2_recomp_guest PRIVATE MA2_GUEST_ENABLED=1)
        target_compile_options(mechassault2_recomp_guest PRIVATE -Wall -Wextra -Wpedantic)
        target_include_directories(mechassault2_recomp_guest PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src/apple/guest"
            "${XBOXRECOMP_DIR}/include"
            "${XBOXRECOMP_DIR}/src")
        target_link_libraries(mechassault2_recomp_guest PRIVATE
            ma2_host_shell ma2_game_core ma2_apple_metal)
        message(STATUS "Private generated macOS guest executable enabled")
    else()
        message(STATUS "macOS host shell enabled; no generated guest was configured")
    endif()
    ma2_assert_apple_graph()
endif()

option(MA2_BUILD_PC_ASSETS "Build pc_assets during the game build (the installer does it separately)" OFF)
if(MA2_BUILD_PC_ASSETS)
    if(NOT WIN32)
        message(FATAL_ERROR "MA2_BUILD_PC_ASSETS and pc_package require the Windows game/launcher graph")
    endif()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(PC_ASSET_PROJECT_DIR "${CMAKE_CURRENT_SOURCE_DIR}" CACHE PATH
        "Game root containing game_files and the pc_assets output directory")
    add_custom_command(OUTPUT "${PC_ASSET_PROJECT_DIR}/pc_assets/common.mgf"
        "${PC_ASSET_PROJECT_DIR}/pc_assets/cache.version"
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/bringup/build_pc_assets.py"
            --project "${PC_ASSET_PROJECT_DIR}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/bringup/build_pc_assets.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/bringup/inspect_mgf.py"
            "${PC_ASSET_PROJECT_DIR}/game_files/common.mgf"
        COMMENT "Build native PC menu forms from the configured game root"
        VERBATIM)
    add_custom_target(pc_assets DEPENDS "${PC_ASSET_PROJECT_DIR}/pc_assets/common.mgf"
        "${PC_ASSET_PROJECT_DIR}/pc_assets/cache.version")
    add_dependencies(${PROJECT_NAME} pc_assets)
    add_custom_target(pc_package
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/bringup/package_pc.py"
        DEPENDS ${PROJECT_NAME} mechassault2_launcher
        COMMENT "Create the self-contained local MechAssault 2 PC build")
endif()
