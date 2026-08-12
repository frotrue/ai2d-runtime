include_guard(GLOBAL)

function(ai2d_set_project_warnings target)
    if(MSVC)
        target_compile_options(${target} INTERFACE
            /W4
            /permissive-
            /Zc:__cplusplus
            /Zc:preprocessor
            /utf-8
            $<$<BOOL:${AI2D_WARNINGS_AS_ERRORS}>:/WX>
        )
    else()
        target_compile_options(${target} INTERFACE
            -Wall
            -Wextra
            -Wpedantic
            -Wconversion
            -Wsign-conversion
            -Wshadow
            $<$<BOOL:${AI2D_WARNINGS_AS_ERRORS}>:-Werror>
        )
    endif()
endfunction()
