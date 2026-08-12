cmake_minimum_required(VERSION 3.28)

project(game_runtime_dev VERSION 0.3.0 LANGUAGES CXX)

option(AI2D_ENABLE_ASAN "Enable AddressSanitizer for project targets" OFF)
option(AI2D_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer where supported" OFF)
option(AI2D_ENABLE_GPU "Build SDL/Vulkan renderer and GPU integration paths" ON)
option(AI2D_ENABLE_VULKAN_VALIDATION "Request Vulkan validation in runtime defaults" OFF)
option(AI2D_ENABLE_SYNC_VALIDATION "Request synchronization validation" OFF)
option(AI2D_ENABLE_ALLOCATION_TRACKING "Link measured-scope global C++ allocation hook" ON)
option(AI2D_WARNINGS_AS_ERRORS "Treat project warnings as errors" ON)
option(AI2D_OFFLINE "Require already-populated FetchContent dependencies" OFF)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(Dependencies)
include(Warnings)
include(Sanitizers)
include(CTest)

find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)

ai2d_fetch_json()

if(BUILD_TESTING)
    ai2d_fetch_catch2()
    # MSVC's STL records ASan annotation mode in object metadata. Test framework
    # objects must use the same mode as the project test binary to remain link-safe.
    if(AI2D_ENABLE_ASAN AND MSVC)
        target_compile_options(Catch2 PRIVATE /fsanitize=address)
        target_compile_options(Catch2WithMain PRIVATE /fsanitize=address)
    endif()
endif()

add_library(ai2d_project_options INTERFACE)
target_compile_features(ai2d_project_options INTERFACE cxx_std_23)
ai2d_set_project_warnings(ai2d_project_options)
ai2d_enable_sanitizers(ai2d_project_options)

add_library(ai2d_foundation STATIC
    src/foundation/allocation_tracker.cpp
    src/foundation/build_info.cpp
    src/foundation/diagnostic.cpp
    src/foundation/frame_arena.cpp
    src/foundation/json_writer.cpp
    src/foundation/statistics.cpp
)
target_include_directories(ai2d_foundation PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(ai2d_foundation PUBLIC ai2d_project_options)
target_compile_definitions(ai2d_foundation PUBLIC
    AI2D_VERSION="${PROJECT_VERSION}"
    $<$<BOOL:${AI2D_ENABLE_ALLOCATION_TRACKING}>:AI2D_ENABLE_ALLOCATION_TRACKING=1>
)

add_library(ai2d_allocation_hook OBJECT src/foundation/allocation_hook.cpp)
target_include_directories(ai2d_allocation_hook PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(ai2d_allocation_hook PRIVATE ai2d_project_options)
target_compile_definitions(ai2d_allocation_hook PRIVATE
    $<$<BOOL:${AI2D_ENABLE_ALLOCATION_TRACKING}>:AI2D_ENABLE_ALLOCATION_TRACKING=1>
)

add_library(ai2d_world STATIC
    src/world/collision.cpp
    src/world/entity.cpp
    src/world/operations.cpp
    src/world/world.cpp
)
target_include_directories(ai2d_world PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(ai2d_world PUBLIC ai2d_foundation)

add_library(ai2d_scenario STATIC
    src/scenario/game.cpp
    src/scenario/scenario.cpp
)
target_include_directories(ai2d_scenario PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(ai2d_scenario PUBLIC ai2d_foundation PRIVATE nlohmann_json::nlohmann_json)

add_library(ai2d_render_queue STATIC src/renderer2d/render_queue.cpp)
target_include_directories(ai2d_render_queue PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(ai2d_render_queue PUBLIC ai2d_foundation)

if(AI2D_ENABLE_GPU)
    ai2d_fetch_sdl3()
    ai2d_fetch_vma()
    find_package(Vulkan 1.3 REQUIRED)
    include(Shaders)

    ai2d_compile_slang(
        AI2D_SPRITE_VERTEX_SPV
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/sprite.slang"
        vertex_main
        vertex
        sprite.vert.spv
    )
    ai2d_compile_slang(
        AI2D_SPRITE_FRAGMENT_SPV
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/sprite.slang"
        fragment_main
        fragment
        sprite.frag.spv
    )
    ai2d_embed_spirv(AI2D_EMBEDDED_SPIRV_HEADER "${AI2D_SPRITE_VERTEX_SPV}" "${AI2D_SPRITE_FRAGMENT_SPV}")
    add_custom_target(ai2d_shaders ALL DEPENDS
        "${AI2D_SPRITE_VERTEX_SPV}"
        "${AI2D_SPRITE_FRAGMENT_SPV}"
        "${AI2D_EMBEDDED_SPIRV_HEADER}"
    )

    add_library(ai2d_platform_sdl STATIC
        src/platform/sdl/assets.cpp
        src/platform/sdl/audio.cpp
        src/platform/sdl/settings.cpp
        src/platform/sdl/window.cpp
    )
    target_include_directories(ai2d_platform_sdl PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(ai2d_platform_sdl PUBLIC ai2d_foundation PRIVATE SDL3::SDL3-static)

    add_library(ai2d_renderer2d STATIC
        src/renderer2d/vulkan/backend.cpp
        src/renderer2d/vulkan/sdl_surface.cpp
        src/renderer2d/vulkan/vma_impl.cpp
    )
    add_dependencies(ai2d_renderer2d ai2d_shaders)
    target_include_directories(ai2d_renderer2d
        PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
        PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated/include"
    )
    target_link_libraries(ai2d_renderer2d
        PUBLIC ai2d_foundation ai2d_render_queue
        PRIVATE Vulkan::Vulkan SDL3::SDL3-static GPUOpen::VulkanMemoryAllocator
    )
    target_compile_definitions(ai2d_renderer2d PRIVATE
        $<$<BOOL:${AI2D_ENABLE_VULKAN_VALIDATION}>:AI2D_DEFAULT_VULKAN_VALIDATION=1>
        $<$<BOOL:${AI2D_ENABLE_SYNC_VALIDATION}>:AI2D_DEFAULT_SYNC_VALIDATION=1>
        $<$<BOOL:${BUILD_TESTING}>:AI2D_ENABLE_TEST_HOOKS=1>
    )
endif()

add_library(ai2d_runtime STATIC
    src/runtime/fixed_step.cpp
    src/runtime/game_runtime.cpp
    src/runtime/runtime.cpp
)
target_include_directories(ai2d_runtime PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(ai2d_runtime PUBLIC ai2d_foundation ai2d_scenario PRIVATE ai2d_world ai2d_render_queue)
if(AI2D_ENABLE_GPU)
    target_link_libraries(ai2d_runtime PRIVATE ai2d_platform_sdl ai2d_renderer2d)
    target_compile_definitions(ai2d_runtime PRIVATE AI2D_ENABLE_GPU=1)
endif()

if(AI2D_ENABLE_GPU AND WIN32)
    add_executable(ai2d_player WIN32 apps/player/main.cpp)
    target_link_libraries(ai2d_player PRIVATE ai2d_runtime ai2d_scenario ai2d_foundation shell32)
    if(AI2D_ENABLE_ALLOCATION_TRACKING)
        target_sources(ai2d_player PRIVATE $<TARGET_OBJECTS:ai2d_allocation_hook>)
    endif()
endif()

add_executable(ai2d_sandbox apps/sandbox/main.cpp)
target_link_libraries(ai2d_sandbox PRIVATE ai2d_foundation)
if(AI2D_ENABLE_GPU)
    target_link_libraries(ai2d_sandbox PRIVATE ai2d_platform_sdl ai2d_renderer2d)
    target_compile_definitions(ai2d_sandbox PRIVATE AI2D_ENABLE_GPU=1)
endif()
if(AI2D_ENABLE_ALLOCATION_TRACKING)
    target_sources(ai2d_sandbox PRIVATE $<TARGET_OBJECTS:ai2d_allocation_hook>)
endif()

add_executable(ai2d_cli
    apps/cli/game_commands.cpp
    apps/cli/main.cpp
    apps/cli/scenario_benchmark.cpp
    apps/cli/scenario_commands.cpp
)
target_link_libraries(ai2d_cli PRIVATE ai2d_world ai2d_scenario ai2d_runtime ai2d_foundation)
if(AI2D_ENABLE_GPU)
    target_sources(ai2d_cli PRIVATE apps/cli/render_benchmark.cpp)
    target_link_libraries(ai2d_cli PRIVATE ai2d_platform_sdl ai2d_renderer2d)
    target_compile_definitions(ai2d_cli PRIVATE AI2D_ENABLE_GPU=1)
else()
    target_sources(ai2d_cli PRIVATE apps/cli/render_benchmark_stub.cpp)
endif()
if(AI2D_ENABLE_ALLOCATION_TRACKING)
    target_sources(ai2d_cli PRIVATE $<TARGET_OBJECTS:ai2d_allocation_hook>)
endif()

if(BUILD_TESTING)
    list(APPEND CMAKE_MODULE_PATH "${AI2D_CATCH2_SOURCE_DIR}/extras")
    include(Catch)

    add_executable(ai2d_tests
        tests/unit/test_allocation_tracker.cpp
        tests/unit/test_diagnostic.cpp
        tests/unit/test_collision.cpp
        tests/unit/test_fixed_step.cpp
        tests/unit/test_frame_arena.cpp
        tests/unit/test_game.cpp
        tests/unit/test_json_writer.cpp
        tests/unit/test_render_queue.cpp
        tests/unit/test_runtime.cpp
        tests/unit/test_scenario.cpp
        tests/unit/test_statistics.cpp
        tests/unit/test_world.cpp
    )
    target_link_libraries(ai2d_tests PRIVATE ai2d_world ai2d_scenario ai2d_runtime ai2d_render_queue ai2d_foundation Catch2::Catch2WithMain)
    target_compile_definitions(ai2d_tests PRIVATE AI2D_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    if(AI2D_ENABLE_GPU)
        target_link_libraries(ai2d_tests PRIVATE ai2d_platform_sdl ai2d_renderer2d)
        target_compile_definitions(ai2d_tests PRIVATE AI2D_ENABLE_GPU=1)
    endif()
    if(AI2D_ENABLE_ALLOCATION_TRACKING)
        target_sources(ai2d_tests PRIVATE $<TARGET_OBJECTS:ai2d_allocation_hook>)
    endif()
    catch_discover_tests(ai2d_tests)

    add_test(NAME architecture
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_architecture.py" --root "${CMAKE_CURRENT_SOURCE_DIR}" --json
    )
    if(AI2D_ENABLE_GPU)
        add_test(NAME engine_json_contract
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_engine_json.py" "${CMAKE_CURRENT_SOURCE_DIR}"
        )
    else()
        add_test(NAME engine_json_contract
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_engine_json.py" "${CMAKE_CURRENT_SOURCE_DIR}" --gpu-disabled
        )
    endif()
    add_test(NAME world_benchmark_json
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_world_benchmark.py" "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    add_test(NAME scenario_cli_contract
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_scenario_cli.py" "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    set(AI2D_GAME_CLI_TEST_ARGS $<TARGET_FILE:ai2d_cli> "${CMAKE_CURRENT_SOURCE_DIR}")
    if(AI2D_ENABLE_GPU)
        list(APPEND AI2D_GAME_CLI_TEST_ARGS --gpu)
    endif()
    add_test(NAME game_cli_contract
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_game_cli.py" ${AI2D_GAME_CLI_TEST_ARGS}
    )
    add_test(NAME v03_sample_games_contract
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_v03_samples.py"
            $<TARGET_FILE:ai2d_cli> "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    add_test(NAME cli_help_contract
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_cli_help.py" $<TARGET_FILE:ai2d_cli>
    )
    add_test(NAME engine_package_contract
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_engine_package.py" "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    add_test(NAME benchmark_compare_policy
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_benchmark_compare.py"
    )
    set(AI2D_BENCHMARK_ARTIFACT_TEST_ARGS "${CMAKE_CURRENT_SOURCE_DIR}")
    if(AI2D_ENABLE_GPU)
        list(APPEND AI2D_BENCHMARK_ARTIFACT_TEST_ARGS --gpu-enabled)
    endif()
    add_test(NAME benchmark_artifact_contract
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_benchmark_artifact.py" ${AI2D_BENCHMARK_ARTIFACT_TEST_ARGS}
    )
    if(AI2D_ENABLE_GPU)
        add_test(NAME shader_vertex_spirv_validation
            COMMAND "${AI2D_SPIRV_VAL_EXECUTABLE}" --target-env vulkan1.3 "${AI2D_SPRITE_VERTEX_SPV}"
        )
        add_test(NAME shader_fragment_spirv_validation
            COMMAND "${AI2D_SPIRV_VAL_EXECUTABLE}" --target-env vulkan1.3 "${AI2D_SPRITE_FRAGMENT_SPV}"
        )
        add_test(NAME gpu_offscreen_smoke
            COMMAND ai2d_sandbox --offscreen --hidden --frames 8 --json
        )
        add_test(NAME gpu_present_resize_smoke
            COMMAND ai2d_sandbox --hidden --frames 12 --resize-smoke --json
        )
        add_test(NAME gpu_sprite_pixel_smoke
            COMMAND ai2d_sandbox --offscreen --hidden --frames 6 --sprite-smoke --invalid-texture-smoke --json
        )
        add_test(NAME gpu_capabilities_json
            COMMAND ai2d_cli inspect capabilities --json
        )
        add_test(NAME gpu_scenario_profile
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_scenario_profile.py" "${CMAKE_CURRENT_SOURCE_DIR}"
        )
        add_test(NAME gpu_acquire_out_of_date_recovery
            COMMAND ai2d_sandbox --hidden --frames 4 --json
        )
        add_test(NAME gpu_begin_command_recovery
            COMMAND ai2d_sandbox --offscreen --hidden --fault-recovery-smoke --json
        )
        add_test(NAME gpu_end_command_recovery
            COMMAND ai2d_sandbox --offscreen --hidden --fault-recovery-smoke --json
        )
        add_test(NAME gpu_reset_command_presented_terminal
            COMMAND ai2d_sandbox --hidden --fault-recovery-smoke --json
        )
        add_test(NAME gpu_begin_command_presented_terminal
            COMMAND ai2d_sandbox --hidden --fault-recovery-smoke --json
        )
        add_test(NAME gpu_end_command_presented_terminal
            COMMAND ai2d_sandbox --hidden --fault-recovery-smoke --json
        )
        add_test(NAME gpu_submit_failure_terminal
            COMMAND ai2d_sandbox --offscreen --hidden --fault-recovery-smoke --json
        )
        add_test(NAME gpu_initialization_retry
            COMMAND ai2d_sandbox --offscreen --hidden --init-retry-smoke --frames 2 --json
        )
        add_test(NAME gpu_resize_failure_terminal
            COMMAND ai2d_sandbox --offscreen --hidden --resize-failure-smoke --json
        )
        set_tests_properties(
            gpu_offscreen_smoke gpu_present_resize_smoke gpu_sprite_pixel_smoke gpu_capabilities_json gpu_scenario_profile
            PROPERTIES
                TIMEOUT 30
                LABELS "gpu"
                ENVIRONMENT "VK_LAYER_PATH=$ENV{VULKAN_SDK}/Bin"
        )
        set_tests_properties(
            shader_vertex_spirv_validation shader_fragment_spirv_validation
            PROPERTIES LABELS "shader"
        )
        set_tests_properties(
            gpu_acquire_out_of_date_recovery gpu_begin_command_recovery gpu_end_command_recovery
            gpu_reset_command_presented_terminal gpu_begin_command_presented_terminal
            gpu_end_command_presented_terminal gpu_submit_failure_terminal gpu_initialization_retry gpu_resize_failure_terminal
            PROPERTIES TIMEOUT 30 LABELS "gpu;fault-recovery"
        )
        set_tests_properties(gpu_acquire_out_of_date_recovery PROPERTIES
            ENVIRONMENT "AI2D_VK_TEST_FAULT=acquire_out_of_date_once"
        )
        set_tests_properties(gpu_begin_command_recovery PROPERTIES
            ENVIRONMENT "AI2D_VK_TEST_FAULT=begin_command_once"
        )
        set_tests_properties(gpu_end_command_recovery PROPERTIES
            ENVIRONMENT "AI2D_VK_TEST_FAULT=end_command_once"
        )
        set_tests_properties(gpu_reset_command_presented_terminal PROPERTIES
            ENVIRONMENT "AI2D_VK_TEST_FAULT=reset_command_pool_once"
        )
        set_tests_properties(gpu_begin_command_presented_terminal PROPERTIES
            ENVIRONMENT "AI2D_VK_TEST_FAULT=begin_command_once"
        )
        set_tests_properties(gpu_end_command_presented_terminal PROPERTIES
            ENVIRONMENT "AI2D_VK_TEST_FAULT=end_command_once"
        )
        set_tests_properties(gpu_submit_failure_terminal PROPERTIES
            ENVIRONMENT "AI2D_VK_TEST_FAULT=submit_once"
        )
        set_tests_properties(gpu_initialization_retry PROPERTIES
            ENVIRONMENT "AI2D_VK_TEST_FAULT=initialize_offscreen_view_once"
        )
        set_tests_properties(gpu_resize_failure_terminal PROPERTIES
            ENVIRONMENT "AI2D_VK_TEST_FAULT=resize_offscreen_view_once"
        )
    endif()
endif()
