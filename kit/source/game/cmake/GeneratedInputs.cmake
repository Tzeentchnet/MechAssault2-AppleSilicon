include_guard(GLOBAL)

function(ma2_require_file path description)
    if(NOT EXISTS "${path}" OR IS_DIRECTORY "${path}")
        message(FATAL_ERROR "Missing ${description}: ${path}")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${path}")
endfunction()

function(ma2_manifest_outputs manifest generated_dir description)
    ma2_require_file("${manifest}" "${description} manifest")
    file(READ "${manifest}" _json)
    string(JSON _type ERROR_VARIABLE _error TYPE "${_json}" final)
    if(_error OR NOT _type STREQUAL "OBJECT")
        message(FATAL_ERROR "Invalid ${description} manifest: expected final outputs")
    endif()
    string(JSON _count LENGTH "${_json}" final)
    if(_count LESS 1)
        message(FATAL_ERROR "Invalid ${description} manifest: no final outputs")
    endif()
    math(EXPR _last "${_count} - 1")
    foreach(_index RANGE 0 ${_last})
        string(JSON _name MEMBER "${_json}" final ${_index})
        if(_name MATCHES "[/\\\\]" OR _name STREQUAL "." OR _name STREQUAL "..")
            message(FATAL_ERROR "Invalid file name in ${description} manifest")
        endif()
        ma2_require_file("${generated_dir}/${_name}" "${description} output")
    endforeach()
endfunction()

function(ma2_manifest_payloads manifest member base suffix)
    file(READ "${manifest}" _json)
    string(JSON _type ERROR_VARIABLE _error TYPE "${_json}" "${member}")
    if(_error OR NOT _type STREQUAL "ARRAY")
        message(FATAL_ERROR "Invalid patch manifest: expected ${member} array")
    endif()
    string(JSON _count LENGTH "${_json}" "${member}")
    if(_count GREATER 0)
        math(EXPR _last "${_count} - 1")
        foreach(_index RANGE 0 ${_last})
            string(JSON _name GET "${_json}" "${member}" ${_index})
            if(_name MATCHES "[/\\\\]" OR _name STREQUAL "." OR _name STREQUAL "..")
                message(FATAL_ERROR "Invalid file name in patch manifest")
            endif()
            ma2_require_file("${base}/${_name}${suffix}" "patch input")
        endforeach()
    endif()
endfunction()

function(ma2_generated_include_root generated_dir output)
    get_filename_component(_gen_name "${generated_dir}" NAME)
    get_filename_component(_recomp_dir "${generated_dir}" DIRECTORY)
    get_filename_component(_recomp_name "${_recomp_dir}" NAME)
    if(NOT _gen_name STREQUAL "gen" OR NOT _recomp_name STREQUAL "recomp")
        message(FATAL_ERROR
            "MA2_GENERATED_DIR must name a src/recomp/gen directory in the generated tree")
    endif()
    get_filename_component(_src_dir "${_recomp_dir}" DIRECTORY)
    set(${output} "${_src_dir}" PARENT_SCOPE)
endfunction()

function(ma2_validate_generated_inputs generated_dir patch_dir)
    if(NOT IS_DIRECTORY "${generated_dir}")
        message(FATAL_ERROR "MA2_GENERATED_DIR does not name a generated-source directory")
    endif()
    foreach(_name
        recomp_types.h recomp_funcs.h recomp_bringup.h
        recomp_dispatch.c recomp_stubs_unresolved.c recomp_fastpaths.c
        recomp_bringup_0000.c recomp_bringup_0001.c)
        ma2_require_file("${generated_dir}/${_name}" "generated input")
    endforeach()

    set(_patch_manifest "${patch_dir}/manifest.json")
    set(_fast_dir "${patch_dir}/fastcode")
    set(_fast_manifest "${_fast_dir}/fastcode.json")
    ma2_manifest_outputs("${_patch_manifest}" "${generated_dir}" "patch")
    ma2_manifest_payloads("${_patch_manifest}" patched "${patch_dir}/diff" ".json")
    ma2_manifest_payloads("${_patch_manifest}" added "${patch_dir}/add" "")
    ma2_manifest_outputs("${_fast_manifest}" "${generated_dir}" "FastCode")
    ma2_require_file("${_fast_dir}/fastcode.py" "FastCode tool")
    file(READ "${_fast_manifest}" _fast_json)
    string(JSON _hot ERROR_VARIABLE _error GET "${_fast_json}" hot)
    if(_error OR _hot MATCHES "[/\\\\]" OR _hot STREQUAL "" OR
       _hot STREQUAL "." OR _hot STREQUAL "..")
        message(FATAL_ERROR "Invalid FastCode manifest: expected hot-list file")
    endif()
    ma2_require_file("${_fast_dir}/${_hot}" "FastCode hot list")
    string(JSON _post ERROR_VARIABLE _error GET "${_fast_json}" post)
    if(_error OR _post MATCHES "[/\\\\]" OR _post STREQUAL "" OR
       _post STREQUAL "." OR _post STREQUAL "..")
        message(FATAL_ERROR "Invalid FastCode manifest: expected post-patch directory")
    endif()
    set(_post_manifest "${_fast_dir}/${_post}/manifest.json")
    ma2_manifest_outputs("${_post_manifest}" "${generated_dir}" "post-FastCode patch")
    ma2_manifest_payloads("${_post_manifest}" patched "${_fast_dir}/${_post}/diff" ".json")
    ma2_manifest_payloads("${_post_manifest}" added "${_fast_dir}/${_post}/add" "")
endfunction()

function(ma2_prepare_apple_abi_overlay generated_dir output)
    set(_script "${CMAKE_CURRENT_SOURCE_DIR}/src/apple/abi/prepare_overlay.py")
    ma2_require_file("${_script}" "Apple Silicon ABI adapter")
    ma2_require_file("${generated_dir}/recomp_types.h" "generated ABI header")
    find_package(Python3 REQUIRED COMPONENTS Interpreter)

    set(MA2_ABI_OVERLAY_DIR "" CACHE PATH
        "Owner-only directory outside the repository for the private ABI overlay")
    set(MA2_SYNTHETIC_ABI_TEMPLATE OFF CACHE BOOL
        "Use the tracked recomp_types.h template for synthetic graph tests only")
    if(MA2_SYNTHETIC_ABI_TEMPLATE)
        set(_overlay_dir "${CMAKE_BINARY_DIR}/ma2-synthetic-abi-overlay")
        set(_template_arg --template)
    else()
        if(NOT MA2_ABI_OVERLAY_DIR)
            message(FATAL_ERROR
                "Private generated inputs require MA2_ABI_OVERLAY_DIR outside the repository")
        endif()
        get_filename_component(_overlay_dir "${MA2_ABI_OVERLAY_DIR}" ABSOLUTE
            BASE_DIR "${CMAKE_BINARY_DIR}")
        set(_template_arg)
    endif()

    execute_process(
        COMMAND "${Python3_EXECUTABLE}" "${_script}"
            --header "${generated_dir}/recomp_types.h"
            --output-dir "${_overlay_dir}"
            ${_template_arg}
        RESULT_VARIABLE _result
        OUTPUT_QUIET ERROR_QUIET)
    if(NOT _result EQUAL 0)
        message(FATAL_ERROR
            "The Apple Silicon ABI overlay could not be verified and prepared")
    endif()
    ma2_require_file("${_overlay_dir}/overlay.json" "Apple Silicon ABI overlay")
    set(${output} "${_overlay_dir}/overlay.json" PARENT_SCOPE)
endfunction()

# Generated sources retain relative references to these hand-written headers.
# Stage only the tracked headers, never the generated inputs, in the build tree.
function(ma2_add_generated_include_overlay target)
    set(_overlay "${CMAKE_CURRENT_BINARY_DIR}/ma2-generated-includes")
    file(MAKE_DIRECTORY "${_overlay}/recomp/gen")
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/src/recomp/recomp_apu_io.h"
        "${_overlay}/recomp/recomp_apu_io.h" COPYONLY)
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/src/ma2_tank_trace.h"
        "${_overlay}/ma2_tank_trace.h" COPYONLY)
    target_include_directories(${target} PRIVATE "${_overlay}/recomp/gen")
endfunction()
