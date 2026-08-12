#include "ai2d/foundation/diagnostic.hpp"

#include "ai2d/foundation/json_writer.hpp"

#include <type_traits>

namespace ai2d {

Diagnostic Diagnostic::make(
    const DiagnosticCode input_code,
    const Severity input_severity,
    std::string input_subsystem,
    std::string input_message,
    const std::source_location& where) {
    Diagnostic diagnostic{};
    diagnostic.code = input_code;
    diagnostic.severity = input_severity;
    diagnostic.subsystem = std::move(input_subsystem);
    diagnostic.message = std::move(input_message);
    diagnostic.source = {where.file_name(), where.function_name(), where.line()};
    diagnostic.has_source = true;
    return diagnostic;
}

std::string_view to_string(const Severity severity) noexcept {
    switch (severity) {
    case Severity::info: return "info";
    case Severity::warning: return "warning";
    case Severity::error: return "error";
    case Severity::fatal: return "fatal";
    }
    return "error";
}

std::string_view to_string(const DiagnosticCode code) noexcept {
    switch (code) {
    case DiagnosticCode::arch_dependency_violation: return "ARCH_DEPENDENCY_VIOLATION";
    case DiagnosticCode::tool_missing: return "TOOL_MISSING";
    case DiagnosticCode::tool_version_unsupported: return "TOOL_VERSION_UNSUPPORTED";
    case DiagnosticCode::command_unavailable: return "COMMAND_UNAVAILABLE";
    case DiagnosticCode::input_invalid: return "INPUT_INVALID";
    case DiagnosticCode::ir_schema_invalid: return "IR_SCHEMA_INVALID";
    case DiagnosticCode::ir_schema_version_unsupported: return "IR_SCHEMA_VERSION_UNSUPPORTED";
    case DiagnosticCode::ir_unknown_operation: return "IR_UNKNOWN_OPERATION";
    case DiagnosticCode::ir_access_mismatch: return "IR_ACCESS_MISMATCH";
    case DiagnosticCode::ir_dependency_cycle: return "IR_DEPENDENCY_CYCLE";
    case DiagnosticCode::ir_ambiguous_write_order: return "IR_AMBIGUOUS_WRITE_ORDER";
    case DiagnosticCode::ir_capacity_exceeded: return "IR_CAPACITY_EXCEEDED";
    case DiagnosticCode::ir_missing_texture: return "IR_MISSING_TEXTURE";
    case DiagnosticCode::ir_invalid_bounds: return "IR_INVALID_BOUNDS";
    case DiagnosticCode::game_manifest_invalid: return "GAME_MANIFEST_INVALID";
    case DiagnosticCode::game_scene_invalid: return "GAME_SCENE_INVALID";
    case DiagnosticCode::game_transition_invalid: return "GAME_TRANSITION_INVALID";
    case DiagnosticCode::asset_path_invalid: return "ASSET_PATH_INVALID";
    case DiagnosticCode::asset_missing: return "ASSET_MISSING";
    case DiagnosticCode::asset_decode_failed: return "ASSET_DECODE_FAILED";
    case DiagnosticCode::asset_glyph_missing: return "ASSET_GLYPH_MISSING";
    case DiagnosticCode::world_stale_entity: return "WORLD_STALE_ENTITY";
    case DiagnosticCode::world_capacity_exceeded: return "WORLD_CAPACITY_EXCEEDED";
    case DiagnosticCode::world_structural_mutation_during_query: return "WORLD_STRUCTURAL_MUTATION_DURING_QUERY";
    case DiagnosticCode::runtime_numeric_state_invalid: return "RUNTIME_NUMERIC_STATE_INVALID";
    case DiagnosticCode::runtime_fixed_step_overrun: return "RUNTIME_FIXED_STEP_OVERRUN";
    case DiagnosticCode::collision_grid_capacity_exceeded: return "COLLISION_GRID_CAPACITY_EXCEEDED";
    case DiagnosticCode::collision_candidate_capacity_exceeded: return "COLLISION_CANDIDATE_CAPACITY_EXCEEDED";
    case DiagnosticCode::collision_iteration_limit: return "COLLISION_ITERATION_LIMIT";
    case DiagnosticCode::audio_device_unavailable: return "AUDIO_DEVICE_UNAVAILABLE";
    case DiagnosticCode::audio_voice_capacity_exceeded: return "AUDIO_VOICE_CAPACITY_EXCEEDED";
    case DiagnosticCode::settings_invalid: return "SETTINGS_INVALID";
    case DiagnosticCode::settings_write_failed: return "SETTINGS_WRITE_FAILED";
    case DiagnosticCode::mem_frame_arena_overflow: return "MEM_FRAME_ARENA_OVERFLOW";
    case DiagnosticCode::mem_frame_heap_allocation: return "MEM_FRAME_HEAP_ALLOCATION";
    case DiagnosticCode::mem_capacity_growth: return "MEM_CAPACITY_GROWTH";
    case DiagnosticCode::render_invalid_texture_handle: return "RENDER_INVALID_TEXTURE_HANDLE";
    case DiagnosticCode::render_batch_fragmentation: return "RENDER_BATCH_FRAGMENTATION";
    case DiagnosticCode::render_upload_capacity_exceeded: return "RENDER_UPLOAD_CAPACITY_EXCEEDED";
    case DiagnosticCode::vk_device_unsupported: return "VK_DEVICE_UNSUPPORTED";
    case DiagnosticCode::vk_validation: return "VK_VALIDATION";
    case DiagnosticCode::vk_swapchain_error: return "VK_SWAPCHAIN_ERROR";
    case DiagnosticCode::perf_baseline_incomparable: return "PERF_BASELINE_INCOMPARABLE";
    case DiagnosticCode::perf_regression: return "PERF_REGRESSION";
    case DiagnosticCode::perf_noise_too_high: return "PERF_NOISE_TOO_HIGH";
    case DiagnosticCode::internal_error: return "INTERNAL_ERROR";
    }
    return "INTERNAL_ERROR";
}

void write_json(JsonWriter& writer, const Diagnostic& diagnostic) {
    writer.begin_object();
    writer.key("code");
    writer.value(to_string(diagnostic.code));
    writer.key("severity");
    writer.value(to_string(diagnostic.severity));
    writer.key("subsystem");
    writer.value(diagnostic.subsystem);
    writer.key("message");
    writer.value(diagnostic.message);
    writer.key("context");
    writer.begin_object();
    for (const auto& entry : diagnostic.context) {
        writer.key(entry.key);
        std::visit(
            [&writer](const auto& value) {
                using Value = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<Value, std::monostate>) {
                    writer.null_value();
                } else if constexpr (std::is_same_v<Value, std::string>) {
                    writer.value(value);
                } else if constexpr (std::is_same_v<Value, bool>) {
                    writer.value(value);
                } else if constexpr (std::is_same_v<Value, std::int64_t>) {
                    writer.value(value);
                } else if constexpr (std::is_same_v<Value, std::uint64_t>) {
                    writer.value(value);
                } else {
                    writer.value(value);
                }
            },
            entry.value);
    }
    writer.end_object();
    writer.key("suggestions");
    writer.begin_array();
    for (const auto& suggestion : diagnostic.suggestions) {
        writer.value(suggestion);
    }
    writer.end_array();
    writer.key("source");
    if (diagnostic.has_source) {
        writer.begin_object();
        writer.key("file");
        writer.value(diagnostic.source.file);
        writer.key("line");
        writer.value(static_cast<std::uint64_t>(diagnostic.source.line));
        writer.key("function");
        writer.value(diagnostic.source.function);
        writer.end_object();
    } else {
        writer.null_value();
    }
    writer.end_object();
}

} // namespace ai2d
