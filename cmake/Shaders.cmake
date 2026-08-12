include_guard(GLOBAL)

function(ai2d_compile_slang output_variable source_file entry_point stage output_name)
    find_program(AI2D_SLANGC_EXECUTABLE
        NAMES slangc slangc.exe
        HINTS "$ENV{AI2D_SLANGC}"
        DOC "Pinned standalone Slang compiler"
        REQUIRED
    )
    find_program(AI2D_SPIRV_VAL_EXECUTABLE
        NAMES spirv-val spirv-val.exe
        HINTS "$ENV{AI2D_SPIRV_VAL}"
        DOC "SPIR-V validator from the Vulkan SDK"
        REQUIRED
    )

    set(output_path "${CMAKE_CURRENT_BINARY_DIR}/generated/shaders/${output_name}")
    add_custom_command(
        OUTPUT "${output_path}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated/shaders"
        COMMAND "${AI2D_SLANGC_EXECUTABLE}"
            "${source_file}"
            -entry "${entry_point}"
            -stage "${stage}"
            -target spirv
            -profile spirv_1_6
            -O2
            -o "${output_path}"
        COMMAND "${AI2D_SPIRV_VAL_EXECUTABLE}" --target-env vulkan1.3 "${output_path}"
        DEPENDS "${source_file}"
        COMMENT "Compiling and validating ${output_name}"
        VERBATIM
    )
    set(${output_variable} "${output_path}" PARENT_SCOPE)
endfunction()

function(ai2d_embed_spirv output_variable vertex_spv fragment_spv)
    if(NOT Python3_EXECUTABLE)
        message(FATAL_ERROR "Python3 is required to embed SPIR-V")
    endif()
    set(output_path "${CMAKE_CURRENT_BINARY_DIR}/generated/include/ai2d_generated/embedded_spirv.hpp")
    add_custom_command(
        OUTPUT "${output_path}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_spirv.py"
            --vertex "${vertex_spv}"
            --fragment "${fragment_spv}"
            --output "${output_path}"
        DEPENDS
            "${vertex_spv}"
            "${fragment_spv}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_spirv.py"
        COMMENT "Embedding validated SPIR-V in the renderer"
        VERBATIM
    )
    set(${output_variable} "${output_path}" PARENT_SCOPE)
endfunction()
