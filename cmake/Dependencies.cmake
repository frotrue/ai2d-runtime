include_guard(GLOBAL)
include(FetchContent)

set(FETCHCONTENT_UPDATES_DISCONNECTED ON CACHE BOOL "Do not update fetched dependencies" FORCE)
if(AI2D_OFFLINE)
    set(FETCHCONTENT_FULLY_DISCONNECTED ON CACHE BOOL "Use pre-populated dependencies only" FORCE)
endif()

function(ai2d_fetch_catch2)
    set(CATCH_INSTALL_DOCS OFF CACHE BOOL "" FORCE)
    set(CATCH_INSTALL_EXTRAS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG 8b08d4d79514f45f7e4ce2a607ac9c94e920d1bb
        GIT_PROGRESS TRUE
    )
    FetchContent_MakeAvailable(catch2)
    FetchContent_GetProperties(catch2 SOURCE_DIR catch2_resolved_source_dir)
    set(AI2D_CATCH2_SOURCE_DIR "${catch2_resolved_source_dir}" PARENT_SCOPE)
endfunction()

function(ai2d_fetch_sdl3)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_TESTS OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(sdl3
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG 147a8ee32dbf9ac02f3794964490687b6bbda1bc
        GIT_PROGRESS TRUE
    )
    FetchContent_MakeAvailable(sdl3)
endfunction()

function(ai2d_fetch_json)
    set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
    set(JSON_Install OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG 55f93686c01528224f448c19128836e7df245f72
        GIT_PROGRESS TRUE
    )
    FetchContent_MakeAvailable(nlohmann_json)
endfunction()

function(ai2d_fetch_vma)
    FetchContent_Declare(vma
        GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
        GIT_TAG 3aa921224c154a0d2c43912bc88e1c42ce1f7607
        GIT_PROGRESS TRUE
    )
    FetchContent_MakeAvailable(vma)
endfunction()
