#pragma once

#include <cstdint>
#include <expected>
#include <source_location>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace ai2d {

class JsonWriter;

enum class Severity : std::uint8_t { info, warning, error, fatal };

enum class DiagnosticCode : std::uint16_t {
    arch_dependency_violation,
    tool_missing,
    tool_version_unsupported,
    command_unavailable,
    input_invalid,
    ir_schema_invalid,
    ir_schema_version_unsupported,
    ir_unknown_operation,
    ir_access_mismatch,
    ir_dependency_cycle,
    ir_ambiguous_write_order,
    ir_capacity_exceeded,
    ir_missing_texture,
    ir_invalid_bounds,
    game_manifest_invalid,
    game_scene_invalid,
    game_pool_invalid,
    game_collision_interaction_invalid,
    game_animation_invalid,
    game_prefab_invalid,
    game_save_invalid,
    game_tile_field_invalid,
    game_input_profile_invalid,
    game_localization_invalid,
    game_presentation_invalid,
    game_test_assertion_failed,
    game_test_nondeterministic,
    game_transition_invalid,
    asset_path_invalid,
    asset_missing,
    asset_decode_failed,
    asset_glyph_missing,
    world_stale_entity,
    world_capacity_exceeded,
    world_structural_mutation_during_query,
    runtime_numeric_state_invalid,
    runtime_pool_state_invalid,
    runtime_contact_state_invalid,
    runtime_animation_state_invalid,
    runtime_save_state_invalid,
    runtime_tile_field_state_invalid,
    runtime_fixed_step_overrun,
    collision_grid_capacity_exceeded,
    collision_candidate_capacity_exceeded,
    collision_contact_capacity_exceeded,
    collision_iteration_limit,
    audio_device_unavailable,
    audio_voice_capacity_exceeded,
    settings_invalid,
    settings_write_failed,
    mem_frame_arena_overflow,
    mem_frame_heap_allocation,
    mem_capacity_growth,
    render_invalid_texture_handle,
    render_batch_fragmentation,
    render_upload_capacity_exceeded,
    vk_device_unsupported,
    vk_validation,
    vk_swapchain_error,
    perf_baseline_incomparable,
    perf_regression,
    perf_noise_too_high,
    internal_error,
};

using DiagnosticValue = std::variant<std::monostate, bool, std::int64_t, std::uint64_t, double, std::string>;

struct DiagnosticContext final {
    std::string key{};
    DiagnosticValue value{};
};

struct SourceContext final {
    std::string file{};
    std::string function{};
    std::uint_least32_t line{0U};
};

struct Diagnostic final {
    DiagnosticCode code{DiagnosticCode::internal_error};
    Severity severity{Severity::error};
    std::string subsystem{};
    std::string message{};
    std::vector<DiagnosticContext> context{};
    std::vector<std::string> suggestions{};
    SourceContext source{};
    bool has_source{false};

    [[nodiscard]] static Diagnostic make(
        DiagnosticCode code,
        Severity severity,
        std::string subsystem,
        std::string message,
        const std::source_location& where = std::source_location::current());
};

[[nodiscard]] std::string_view to_string(Severity severity) noexcept;
[[nodiscard]] std::string_view to_string(DiagnosticCode code) noexcept;
void write_json(JsonWriter& writer, const Diagnostic& diagnostic);

template <class T>
using Result = std::expected<T, Diagnostic>;

} // namespace ai2d
