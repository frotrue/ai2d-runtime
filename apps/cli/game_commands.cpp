#include "game_commands.hpp"

#include "ai2d/foundation/build_info.hpp"
#include "ai2d/foundation/diagnostic.hpp"
#include "ai2d/foundation/json_writer.hpp"
#include "ai2d/runtime/game_runtime.hpp"
#include "ai2d/scenario/game.hpp"

#include <charconv>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

struct RunArguments final {
    std::uint32_t frames{0U};
    bool headless{false};
    bool offscreen{false};
    bool hidden{false};
    bool audio{true};
    bool saved_settings{true};
    bool validation{false};
    bool synchronization_validation{false};
    bool json{false};
    bool has_fps{false};
    std::filesystem::path input_script{};
    ai2d::RenderFpsCap fps{ai2d::RenderFpsCap::fps_60};
};

struct AggregateRuleMetrics final {
    std::uint64_t rule_executions{0U};
    std::uint64_t condition_evaluations{0U};
    std::uint64_t action_executions{0U};
    std::uint64_t grid_steps{0U};
    std::uint64_t rejected_direction_changes{0U};
    std::uint64_t follower_updates{0U};
    std::uint64_t active_state_changes{0U};
    std::uint64_t relocations{0U};
    std::uint64_t relocation_cells_scanned{0U};

    void add(const ai2d::GameRuntimeFrameMetrics& frame) noexcept {
        rule_executions += frame.rule_executions;
        condition_evaluations += frame.condition_evaluations;
        action_executions += frame.action_executions;
        grid_steps += frame.grid_steps;
        rejected_direction_changes += frame.rejected_direction_changes;
        follower_updates += frame.follower_updates;
        active_state_changes += frame.active_state_changes;
        relocations += frame.relocations;
        relocation_cells_scanned += frame.relocation_cells_scanned;
    }
};

bool parse_u32(const std::string_view text, std::uint32_t& value) {
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

bool parse_fps(const std::string_view text, ai2d::RenderFpsCap& value) {
    if (text == "60") value = ai2d::RenderFpsCap::fps_60;
    else if (text == "120") value = ai2d::RenderFpsCap::fps_120;
    else if (text == "144") value = ai2d::RenderFpsCap::fps_144;
    else if (text == "240") value = ai2d::RenderFpsCap::fps_240;
    else if (text == "unlimited") value = ai2d::RenderFpsCap::unlimited;
    else return false;
    return true;
}

bool wants_json(const int count, const char* const* arguments) {
    for (int index = 1; index < count; ++index) {
        if (std::string_view{arguments[index]} == "--json") return true;
    }
    return false;
}

int diagnostic_exit_code(const ai2d::DiagnosticCode code) noexcept {
    switch (code) {
    case ai2d::DiagnosticCode::input_invalid: return 2;
    case ai2d::DiagnosticCode::tool_missing:
    case ai2d::DiagnosticCode::tool_version_unsupported:
    case ai2d::DiagnosticCode::command_unavailable:
    case ai2d::DiagnosticCode::vk_device_unsupported: return 3;
    case ai2d::DiagnosticCode::internal_error: return 4;
    default: return 1;
    }
}

int emit_failure(
    const std::string_view operation,
    ai2d::Diagnostic diagnostic,
    const bool json,
    const int requested_code = -1) {
    const int code = requested_code < 0 ? diagnostic_exit_code(diagnostic.code) : requested_code;
    if (!json) {
        std::cerr << diagnostic.message << '\n';
        return code;
    }
    const auto build = ai2d::current_build_info();
    ai2d::JsonWriter writer{};
    writer.begin_object();
    writer.key("schema_version");
    writer.value(std::uint64_t{1U});
    writer.key("command");
    writer.value("game");
    writer.key("operation");
    writer.value(operation);
    writer.key("status");
    writer.value(code == 3 ? "unavailable" : "fail");
    writer.key("build");
    writer.begin_object();
    writer.key("version");
    writer.value(build.version);
    writer.key("compiler");
    writer.value(build.compiler);
    writer.key("configuration");
    writer.value(build.configuration);
    writer.end_object();
    writer.key("environment");
    writer.begin_object();
    writer.end_object();
    writer.key("diagnostics");
    writer.begin_array();
    ai2d::write_json(writer, diagnostic);
    writer.end_array();
    writer.key("metrics");
    writer.begin_object();
    writer.end_object();
    writer.key("artifacts");
    writer.begin_array();
    writer.end_array();
    writer.end_object();
    std::cout << writer.str() << '\n';
    return code;
}

int emit_invalid(const std::string_view operation, const bool json, const char* const message) {
    return emit_failure(
        operation,
        ai2d::Diagnostic::make(ai2d::DiagnosticCode::input_invalid, ai2d::Severity::error, "cli", message),
        json,
        2);
}

void begin_success(ai2d::JsonWriter& writer, const std::string_view operation) {
    const auto build = ai2d::current_build_info();
    writer.begin_object();
    writer.key("schema_version");
    writer.value(std::uint64_t{1U});
    writer.key("command");
    writer.value("game");
    writer.key("operation");
    writer.value(operation);
    writer.key("status");
    writer.value("pass");
    writer.key("build");
    writer.begin_object();
    writer.key("version");
    writer.value(build.version);
    writer.key("compiler");
    writer.value(build.compiler);
    writer.key("configuration");
    writer.value(build.configuration);
    writer.end_object();
    writer.key("environment");
    writer.begin_object();
    writer.end_object();
}

void end_success(ai2d::JsonWriter& writer) {
    writer.key("artifacts");
    writer.begin_array();
    writer.end_array();
    writer.end_object();
}

int validate_or_inspect(
    const std::string_view operation,
    const int argument_count,
    const char* const* arguments) {
    const bool json = wants_json(argument_count, arguments);
    if (argument_count < 4) return emit_invalid(operation, json, "GameManifest path is required");
    for (int index = 4; index < argument_count; ++index) {
        if (std::string_view{arguments[index]} != "--json") return emit_invalid(operation, json, "Unknown game option");
    }
    auto plan = ai2d::compile_game_file(arguments[3]);
    if (!plan) return emit_failure(operation, std::move(plan.error()), json);
    if (!json) {
        std::cout << "game " << operation << ": PASS (" << plan->scenes.size() << " scenes, "
                  << plan->assets.size() << " assets)\n";
        return 0;
    }
    ai2d::JsonWriter writer{};
    begin_success(writer, operation);
    writer.key("diagnostics");
    writer.begin_array();
    writer.end_array();
    writer.key("metrics");
    if (operation == "inspect") {
        ai2d::write_game_summary_json(writer, *plan);
    } else {
        writer.begin_object();
        writer.key("plan_hash");
        writer.value(plan->plan_hash);
        writer.key("scene_count");
        writer.value(static_cast<std::uint64_t>(plan->scenes.size()));
        writer.end_object();
    }
    end_success(writer);
    std::cout << writer.str() << '\n';
    return 0;
}

bool parse_run_arguments(
    const int argument_count,
    const char* const* arguments,
    RunArguments& output) {
    for (int index = 4; index < argument_count; ++index) {
        const std::string_view option{arguments[index]};
        const auto value = [&]() -> const char* {
            if (index + 1 >= argument_count) return nullptr;
            return arguments[++index];
        };
        if (option == "--frames") {
            const auto* text = value();
            if (text == nullptr || !parse_u32(text, output.frames) || output.frames > 10'000'000U) return false;
        } else if (option == "--fps") {
            const auto* text = value();
            if (text == nullptr || !parse_fps(text, output.fps)) return false;
            output.has_fps = true;
        } else if (option == "--input-script") {
            const auto* text = value();
            if (text == nullptr || *text == '\0') return false;
            output.input_script = text;
        } else if (option == "--headless") output.headless = true;
        else if (option == "--offscreen") output.offscreen = true;
        else if (option == "--hidden") output.hidden = true;
        else if (option == "--no-audio") output.audio = false;
        else if (option == "--no-saved-settings") output.saved_settings = false;
        else if (option == "--validation") output.validation = true;
        else if (option == "--sync-validation") {
            output.validation = true;
            output.synchronization_validation = true;
        } else if (option == "--json") output.json = true;
        else return false;
    }
    return !(output.headless && output.offscreen) &&
           !((output.headless || output.offscreen) && output.frames == 0U) &&
           (output.input_script.empty() || output.headless || output.offscreen);
}

int run_game(const int argument_count, const char* const* arguments) {
    const bool requested_json = wants_json(argument_count, arguments);
    if (argument_count < 4) return emit_invalid("run", requested_json, "GameManifest path is required");
    RunArguments run{};
    if (!parse_run_arguments(argument_count, arguments, run)) {
        return emit_invalid(
            "run",
            requested_json,
            "usage: ai2d_cli game run <game.json> [--frames N] [--headless|--offscreen] [--input-script FILE] [--fps CAP] [--json]");
    }
    auto plan = ai2d::compile_game_file(arguments[3]);
    if (!plan) return emit_failure("run", std::move(plan.error()), run.json);
    ai2d::GameInputScriptPlan input_script{};
    if (!run.input_script.empty()) {
        auto compiled_input = ai2d::compile_game_input_file(run.input_script, *plan);
        if (!compiled_input) return emit_failure("run", std::move(compiled_input.error()), run.json);
        input_script = std::move(*compiled_input);
    }
    ai2d::GameRuntime runtime{};
    ai2d::GameRuntimeOptions options{};
    options.headless = run.headless;
    options.offscreen = run.offscreen;
    options.hidden = run.hidden;
    options.enable_audio = run.audio;
    options.load_saved_settings = run.saved_settings;
    options.enable_validation = run.validation;
    options.enable_synchronization_validation = run.synchronization_validation;
    options.has_fps_override = run.has_fps;
    options.fps_override = run.fps;
    auto loaded = runtime.initialize(*plan, options);
    if (!loaded) return emit_failure("run", std::move(loaded.error()), run.json);
    ai2d::GameRuntimeFrameMetrics last_frame{};
    AggregateRuleMetrics aggregate{};
    std::vector<ai2d::GameActionInput> scripted_actions{};
    if (!run.input_script.empty()) scripted_actions.resize(plan->actions.size());
    std::size_t next_input_event = 0U;
    std::uint64_t executed_frames = 0U;
    while (run.frames == 0U || executed_frames < run.frames) {
        ai2d::Result<ai2d::GameRuntimeFrameMetrics> frame{};
        if (!run.input_script.empty()) {
            for (auto& action : scripted_actions) {
                action.pressed = false;
                action.released = false;
            }
            while (next_input_event < input_script.events.size() &&
                   input_script.events[next_input_event].tick == executed_frames) {
                const auto& event = input_script.events[next_input_event++];
                auto& action = scripted_actions[event.action_index];
                switch (event.kind) {
                case ai2d::GameInputEventKind::press:
                    action.down = true;
                    action.pressed = true;
                    break;
                case ai2d::GameInputEventKind::release:
                    action.down = false;
                    action.released = true;
                    break;
                case ai2d::GameInputEventKind::tap:
                    action.down = false;
                    action.pressed = true;
                    action.released = true;
                    break;
                }
            }
            frame = runtime.run_exact_actions(scripted_actions, 1U);
        } else {
            frame = run.headless || run.offscreen ? runtime.run_exact({}, 1U) : runtime.run_frame();
        }
        if (!frame) return emit_failure("run", std::move(frame.error()), run.json);
        last_frame = *frame;
        aggregate.add(*frame);
        ++executed_frames;
        if (frame->quit_requested) break;
    }
    if (auto idle = runtime.wait_idle(); !idle) return emit_failure("run", std::move(idle.error()), run.json);
    if (!run.json) {
        std::cout << "game run: PASS (scene=" << runtime.current_scene() << ", frames=" << executed_frames << ")\n";
        return 0;
    }
    ai2d::JsonWriter writer{};
    begin_success(writer, "run");
    writer.key("diagnostics");
    writer.begin_array();
    for (const auto& diagnostic : runtime.diagnostics()) ai2d::write_json(writer, diagnostic);
    writer.end_array();
    writer.key("metrics");
    writer.begin_object();
    writer.key("manifest");
    writer.value(plan->manifest_path.generic_string());
    writer.key("plan_hash");
    writer.value(plan->plan_hash);
    writer.key("frames");
    writer.value(executed_frames);
    writer.key("simulation_ticks");
    writer.value(last_frame.simulation_tick);
    writer.key("input_script");
    if (run.input_script.empty()) writer.null_value();
    else writer.value(run.input_script.generic_string());
    writer.key("scene");
    writer.value(runtime.current_scene());
    writer.key("spawned_entities");
    writer.value(static_cast<std::uint64_t>(loaded->spawned_entities));
    writer.key("loaded_textures");
    writer.value(static_cast<std::uint64_t>(loaded->loaded_textures));
    writer.key("loaded_audio_clips");
    writer.value(static_cast<std::uint64_t>(loaded->loaded_audio_clips));
    writer.key("audio_available");
    writer.value(loaded->audio_available);
    writer.key("requested_fps");
    writer.value(ai2d::to_string(runtime.requested_fps_cap()));
    writer.key("effective_fps");
    writer.value(ai2d::to_string(runtime.effective_fps_cap()));
    writer.key("requested_present_mode");
    writer.value(runtime.requested_present_mode());
    writer.key("effective_present_mode");
    writer.value(runtime.effective_present_mode());
    writer.key("states");
    writer.begin_object();
    for (const auto& state : plan->states) {
        auto value = runtime.state_value(plan->symbol(state.symbol));
        if (value) {
            writer.key(plan->symbol(state.symbol));
            writer.value(static_cast<std::int64_t>(*value));
        }
    }
    writer.end_object();
    writer.key("rule_executions");
    writer.value(aggregate.rule_executions);
    writer.key("condition_evaluations");
    writer.value(aggregate.condition_evaluations);
    writer.key("action_executions");
    writer.value(aggregate.action_executions);
    writer.key("grid_steps");
    writer.value(aggregate.grid_steps);
    writer.key("rejected_direction_changes");
    writer.value(aggregate.rejected_direction_changes);
    writer.key("follower_updates");
    writer.value(aggregate.follower_updates);
    writer.key("active_state_changes");
    writer.value(aggregate.active_state_changes);
    writer.key("relocations");
    writer.value(aggregate.relocations);
    writer.key("relocation_cells_scanned");
    writer.value(aggregate.relocation_cells_scanned);
    writer.key("last_frame");
    writer.begin_object();
    writer.key("fixed_ticks");
    writer.value(static_cast<std::uint64_t>(last_frame.fixed_ticks));
    writer.key("input_edge_ticks");
    writer.value(static_cast<std::uint64_t>(last_frame.input_edge_ticks));
    writer.key("sprites");
    writer.value(static_cast<std::uint64_t>(last_frame.extracted_sprites));
    writer.key("contacts");
    writer.value(static_cast<std::uint64_t>(last_frame.collision.contacts));
    writer.key("state_checksum");
    writer.value(last_frame.state_checksum);
    writer.key("scene_state_checksum");
    writer.value(last_frame.scene_state_checksum);
    writer.end_object();
    writer.end_object();
    end_success(writer);
    std::cout << writer.str() << '\n';
    return 0;
}

} // namespace

int ai2d_game_command_main(const int argument_count, const char* const* arguments) {
    const bool json = wants_json(argument_count, arguments);
    if (argument_count < 3) return emit_invalid("", json, "usage: ai2d_cli game <validate|inspect|run>");
    const std::string_view operation{arguments[2]};
    if (operation == "--help" || operation == "-h" || operation == "help") {
        std::cout
            << "usage: ai2d_cli game <validate|inspect|run> [options]\n"
            << "  validate <game.json>                     compile and validate a game\n"
            << "  inspect <game.json>                      emit compiled plan metadata\n"
            << "  run <game.json> [--frames N]             run a game\n"
            << "      [--headless|--offscreen] [--hidden] [--input-script FILE]\n"
            << "      [--fps 60|120|144|240|unlimited] [--no-audio] [--json]\n";
        return 0;
    }
    if (operation == "validate" || operation == "inspect") {
        return validate_or_inspect(operation, argument_count, arguments);
    }
    if (operation == "run") return run_game(argument_count, arguments);
    return emit_invalid(operation, json, "Unknown game operation");
}
