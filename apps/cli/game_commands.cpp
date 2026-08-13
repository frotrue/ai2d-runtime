#include "game_commands.hpp"

#include "ai2d/foundation/build_info.hpp"
#include "ai2d/foundation/diagnostic.hpp"
#include "ai2d/foundation/json_writer.hpp"
#include "ai2d/runtime/game_runtime.hpp"
#include "ai2d/scenario/game.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
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
    std::uint64_t pool_acquire_attempts{0U};
    std::uint64_t pool_acquire_successes{0U};
    std::uint64_t pool_releases{0U};
    std::uint64_t pool_release_misses{0U};
    std::uint64_t pool_exhaustions{0U};
    std::uint64_t pool_recycled_slots{0U};
    std::uint64_t pool_expirations{0U};
    std::uint64_t pool_resets{0U};
    std::uint64_t pool_lifetime_checks{0U};
    std::uint64_t collision_contacts{0U};
    std::uint64_t trigger_narrowphase_tests{0U};
    std::uint64_t contact_begins{0U};
    std::uint64_t contact_ends{0U};
    std::uint64_t stale_contact_events{0U};
    std::uint64_t motion_segments{0U};
    std::uint64_t linear_motion_updates{0U};
    std::uint32_t active_contact_pairs{0U};
    std::uint32_t peak_contact_pairs{0U};
    std::uint32_t active_pooled_entities{0U};
    std::uint32_t peak_active_pooled_entities{0U};

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
        pool_acquire_attempts += frame.pool_acquire_attempts;
        pool_acquire_successes += frame.pool_acquire_successes;
        pool_releases += frame.pool_releases;
        pool_release_misses += frame.pool_release_misses;
        pool_exhaustions += frame.pool_exhaustions;
        pool_recycled_slots += frame.pool_recycled_slots;
        pool_expirations += frame.pool_expirations;
        pool_resets += frame.pool_resets;
        pool_lifetime_checks += frame.pool_lifetime_checks;
        collision_contacts += frame.collision.contacts;
        trigger_narrowphase_tests += frame.trigger_narrowphase_tests;
        contact_begins += frame.contact_begins;
        contact_ends += frame.contact_ends;
        stale_contact_events += frame.stale_contact_events;
        motion_segments += frame.motion_segments;
        linear_motion_updates += frame.linear_motion_updates;
        active_contact_pairs = frame.active_contact_pairs;
        peak_contact_pairs = std::max(peak_contact_pairs, frame.peak_contact_pairs);
        active_pooled_entities = frame.active_pooled_entities;
        peak_active_pooled_entities = std::max(
            peak_active_pooled_entities, frame.peak_active_pooled_entities);
    }

    friend bool operator==(const AggregateRuleMetrics&, const AggregateRuleMetrics&) = default;
};

struct VerifyArguments final {
    std::filesystem::path test_script{};
    std::uint32_t repeat{2U};
    bool offscreen{false};
    bool hidden{false};
    bool json{false};
};

struct AssertionObservation final {
    ai2d::GameTestAssertionKind kind{ai2d::GameTestAssertionKind::current_scene};
    std::uint64_t tick{0U};
    std::int64_t signed_value{0};
    std::uint64_t unsigned_value{0U};
    bool bool_value{false};
    ai2d::Vec2 vector_value{};
    std::string scene{};

    friend bool operator==(const AssertionObservation&, const AssertionObservation&) = default;
};

struct VerifyResult final {
    std::uint32_t scene_index{0U};
    std::vector<std::int32_t> states{};
    double state_checksum{0.0};
    double scene_state_checksum{0.0};
    std::uint64_t contact_state_checksum{0U};
    AggregateRuleMetrics metrics{};
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
    writer.key("pool_acquire_attempts");
    writer.value(aggregate.pool_acquire_attempts);
    writer.key("pool_acquire_successes");
    writer.value(aggregate.pool_acquire_successes);
    writer.key("pool_releases");
    writer.value(aggregate.pool_releases);
    writer.key("pool_release_misses");
    writer.value(aggregate.pool_release_misses);
    writer.key("pool_exhaustions");
    writer.value(aggregate.pool_exhaustions);
    writer.key("pool_recycled_slots");
    writer.value(aggregate.pool_recycled_slots);
    writer.key("pool_expirations");
    writer.value(aggregate.pool_expirations);
    writer.key("pool_resets");
    writer.value(aggregate.pool_resets);
    writer.key("pool_lifetime_checks");
    writer.value(aggregate.pool_lifetime_checks);
    writer.key("trigger_narrowphase_tests");
    writer.value(aggregate.trigger_narrowphase_tests);
    writer.key("contact_begins");
    writer.value(aggregate.contact_begins);
    writer.key("contact_ends");
    writer.value(aggregate.contact_ends);
    writer.key("stale_contact_events");
    writer.value(aggregate.stale_contact_events);
    writer.key("motion_segments");
    writer.value(aggregate.motion_segments);
    writer.key("linear_motion_updates");
    writer.value(aggregate.linear_motion_updates);
    writer.key("active_contact_pairs");
    writer.value(static_cast<std::uint64_t>(aggregate.active_contact_pairs));
    writer.key("peak_contact_pairs");
    writer.value(static_cast<std::uint64_t>(aggregate.peak_contact_pairs));
    writer.key("active_pooled_entities");
    writer.value(static_cast<std::uint64_t>(last_frame.active_pooled_entities));
    writer.key("peak_active_pooled_entities");
    writer.value(static_cast<std::uint64_t>(aggregate.peak_active_pooled_entities));
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
    writer.key("active_colliders");
    writer.value(static_cast<std::uint64_t>(last_frame.collision.active_colliders));
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

bool parse_verify_arguments(
    const int argument_count,
    const char* const* arguments,
    VerifyArguments& output) {
    for (int index = 4; index < argument_count; ++index) {
        const std::string_view option{arguments[index]};
        const auto value = [&]() -> const char* {
            if (index + 1 >= argument_count) return nullptr;
            return arguments[++index];
        };
        if (option == "--test-script") {
            const auto* text = value();
            if (text == nullptr || *text == '\0') return false;
            output.test_script = text;
        } else if (option == "--repeat") {
            const auto* text = value();
            if (text == nullptr || !parse_u32(text, output.repeat) || output.repeat == 0U || output.repeat > 16U) {
                return false;
            }
        } else if (option == "--offscreen") output.offscreen = true;
        else if (option == "--hidden") output.hidden = true;
        else if (option == "--json") output.json = true;
        else return false;
    }
    return !output.test_script.empty();
}

template <class Value>
bool comparison_matches(
    const Value actual,
    const Value expected,
    const ai2d::GameComparison comparison) noexcept {
    switch (comparison) {
    case ai2d::GameComparison::equal: return actual == expected;
    case ai2d::GameComparison::not_equal: return actual != expected;
    case ai2d::GameComparison::less: return actual < expected;
    case ai2d::GameComparison::less_equal: return actual <= expected;
    case ai2d::GameComparison::greater: return actual > expected;
    case ai2d::GameComparison::greater_equal: return actual >= expected;
    }
    return false;
}

std::uint64_t metric_value(
    const AggregateRuleMetrics& metrics,
    const ai2d::GameTestMetric metric) noexcept {
    switch (metric) {
    case ai2d::GameTestMetric::rule_executions: return metrics.rule_executions;
    case ai2d::GameTestMetric::condition_evaluations: return metrics.condition_evaluations;
    case ai2d::GameTestMetric::action_executions: return metrics.action_executions;
    case ai2d::GameTestMetric::grid_steps: return metrics.grid_steps;
    case ai2d::GameTestMetric::rejected_direction_changes: return metrics.rejected_direction_changes;
    case ai2d::GameTestMetric::follower_updates: return metrics.follower_updates;
    case ai2d::GameTestMetric::active_state_changes: return metrics.active_state_changes;
    case ai2d::GameTestMetric::relocations: return metrics.relocations;
    case ai2d::GameTestMetric::relocation_cells_scanned: return metrics.relocation_cells_scanned;
    case ai2d::GameTestMetric::collision_contacts: return metrics.collision_contacts;
    case ai2d::GameTestMetric::trigger_narrowphase_tests: return metrics.trigger_narrowphase_tests;
    case ai2d::GameTestMetric::contact_begins: return metrics.contact_begins;
    case ai2d::GameTestMetric::contact_ends: return metrics.contact_ends;
    case ai2d::GameTestMetric::stale_contact_events: return metrics.stale_contact_events;
    case ai2d::GameTestMetric::active_contact_pairs: return metrics.active_contact_pairs;
    case ai2d::GameTestMetric::peak_contact_pairs: return metrics.peak_contact_pairs;
    case ai2d::GameTestMetric::motion_segments: return metrics.motion_segments;
    case ai2d::GameTestMetric::linear_motion_updates: return metrics.linear_motion_updates;
    case ai2d::GameTestMetric::pool_acquire_attempts: return metrics.pool_acquire_attempts;
    case ai2d::GameTestMetric::pool_acquire_successes: return metrics.pool_acquire_successes;
    case ai2d::GameTestMetric::pool_releases: return metrics.pool_releases;
    case ai2d::GameTestMetric::pool_release_misses: return metrics.pool_release_misses;
    case ai2d::GameTestMetric::pool_exhaustions: return metrics.pool_exhaustions;
    case ai2d::GameTestMetric::pool_recycled_slots: return metrics.pool_recycled_slots;
    case ai2d::GameTestMetric::pool_expirations: return metrics.pool_expirations;
    case ai2d::GameTestMetric::pool_resets: return metrics.pool_resets;
    case ai2d::GameTestMetric::pool_lifetime_checks: return metrics.pool_lifetime_checks;
    case ai2d::GameTestMetric::active_pooled_entities: return metrics.active_pooled_entities;
    case ai2d::GameTestMetric::peak_active_pooled_entities: return metrics.peak_active_pooled_entities;
    }
    return 0U;
}

ai2d::Result<AssertionObservation> evaluate_assertion(
    const ai2d::GameTestAssertionPlan& assertion,
    ai2d::GameRuntime& runtime,
    const AggregateRuleMetrics& metrics,
    const ai2d::GamePlan& plan) {
    AssertionObservation observation{};
    observation.kind = assertion.kind;
    observation.tick = assertion.tick;
    bool passed = false;
    switch (assertion.kind) {
    case ai2d::GameTestAssertionKind::current_scene:
        observation.scene = std::string{runtime.current_scene()};
        passed = runtime.current_scene_index() == assertion.scene_index;
        break;
    case ai2d::GameTestAssertionKind::int_state: {
        auto value = runtime.state_value(assertion.state_index);
        if (!value) return std::unexpected(std::move(value.error()));
        observation.signed_value = *value;
        passed = comparison_matches(
            observation.signed_value, assertion.expected_integer, assertion.comparison);
        break;
    }
    case ai2d::GameTestAssertionKind::group_active_count: {
        if (runtime.current_scene_index() != assertion.scene_index) break;
        auto value = runtime.group_active_count(assertion.spawn_group_index);
        if (!value) return std::unexpected(std::move(value.error()));
        observation.unsigned_value = *value;
        passed = comparison_matches(
            observation.unsigned_value, assertion.expected_unsigned, assertion.comparison);
        break;
    }
    case ai2d::GameTestAssertionKind::entity_active: {
        if (runtime.current_scene_index() != assertion.scene_index) break;
        auto value = runtime.entity_active(assertion.spawn_group_index, assertion.item_index);
        if (!value) return std::unexpected(std::move(value.error()));
        observation.bool_value = *value;
        passed = *value == assertion.expected_active;
        break;
    }
    case ai2d::GameTestAssertionKind::position:
    case ai2d::GameTestAssertionKind::velocity: {
        if (runtime.current_scene_index() != assertion.scene_index) break;
        auto value = assertion.kind == ai2d::GameTestAssertionKind::position
                         ? runtime.entity_position(assertion.spawn_group_index, assertion.item_index)
                         : runtime.entity_velocity(assertion.spawn_group_index, assertion.item_index);
        if (!value) return std::unexpected(std::move(value.error()));
        observation.vector_value = *value;
        passed = std::abs(value->x - assertion.expected_vector.x) <= assertion.tolerance &&
                 std::abs(value->y - assertion.expected_vector.y) <= assertion.tolerance;
        break;
    }
    case ai2d::GameTestAssertionKind::runtime_metric:
        observation.unsigned_value = metric_value(metrics, assertion.metric);
        passed = comparison_matches(
            observation.unsigned_value, assertion.expected_unsigned, assertion.comparison);
        break;
    }
    if (passed) return observation;
    auto diagnostic = ai2d::Diagnostic::make(
        ai2d::DiagnosticCode::game_test_assertion_failed,
        ai2d::Severity::error,
        "game_verify",
        "A compiled game-test assertion did not match the runtime observation");
    diagnostic.context.push_back({"tick", assertion.tick});
    diagnostic.context.push_back({"kind", std::string{ai2d::to_string(assertion.kind)}});
    diagnostic.context.push_back({"scene", std::string{runtime.current_scene()}});
    if (assertion.kind == ai2d::GameTestAssertionKind::current_scene) {
        diagnostic.context.push_back({"actual", observation.scene});
        diagnostic.context.push_back({"expected", std::string{plan.symbol(plan.scenes[assertion.scene_index].symbol)}});
    } else if (assertion.kind == ai2d::GameTestAssertionKind::int_state) {
        diagnostic.context.push_back({"actual", observation.signed_value});
        diagnostic.context.push_back({"expected", assertion.expected_integer});
    } else if (assertion.kind == ai2d::GameTestAssertionKind::runtime_metric ||
               assertion.kind == ai2d::GameTestAssertionKind::group_active_count) {
        diagnostic.context.push_back({"actual", observation.unsigned_value});
        diagnostic.context.push_back({"expected", assertion.expected_unsigned});
    } else if (assertion.kind == ai2d::GameTestAssertionKind::entity_active) {
        diagnostic.context.push_back({"actual", observation.bool_value});
        diagnostic.context.push_back({"expected", assertion.expected_active});
    } else if (assertion.kind == ai2d::GameTestAssertionKind::position ||
               assertion.kind == ai2d::GameTestAssertionKind::velocity) {
        diagnostic.context.push_back({"actual_x", static_cast<double>(observation.vector_value.x)});
        diagnostic.context.push_back({"actual_y", static_cast<double>(observation.vector_value.y)});
        diagnostic.context.push_back({"expected_x", static_cast<double>(assertion.expected_vector.x)});
        diagnostic.context.push_back({"expected_y", static_cast<double>(assertion.expected_vector.y)});
        diagnostic.context.push_back({"tolerance", static_cast<double>(assertion.tolerance)});
    }
    return std::unexpected(std::move(diagnostic));
}

int verify_game(const int argument_count, const char* const* arguments) {
    const bool requested_json = wants_json(argument_count, arguments);
    if (argument_count < 4) return emit_invalid("verify", requested_json, "GameManifest path is required");
    VerifyArguments options{};
    if (!parse_verify_arguments(argument_count, arguments, options)) {
        return emit_invalid(
            "verify", requested_json,
            "usage: ai2d_cli game verify <game.json> --test-script FILE [--repeat 1..16] [--offscreen] [--json]");
    }
    auto plan = ai2d::compile_game_file(arguments[3]);
    if (!plan) return emit_failure("verify", std::move(plan.error()), options.json);
    auto script = ai2d::compile_game_test_file(options.test_script, *plan);
    if (!script) return emit_failure("verify", std::move(script.error()), options.json);
    std::vector<AssertionObservation> observations{};
    observations.reserve(script->assertions.size());
    VerifyResult baseline{};
    bool has_baseline = false;
    for (std::uint32_t repeat = 0U; repeat < options.repeat; ++repeat) {
        ai2d::GameRuntime runtime{};
        ai2d::GameRuntimeOptions runtime_options{};
        runtime_options.headless = !options.offscreen;
        runtime_options.offscreen = options.offscreen;
        runtime_options.hidden = options.hidden;
        runtime_options.enable_audio = false;
        runtime_options.load_saved_settings = false;
        auto loaded = runtime.initialize(*plan, runtime_options);
        if (!loaded) return emit_failure("verify", std::move(loaded.error()), options.json);
        std::vector<ai2d::GameActionInput> actions(plan->actions.size());
        AggregateRuleMetrics aggregate{};
        ai2d::GameRuntimeFrameMetrics last_frame{};
        std::vector<AssertionObservation> repeat_observations{};
        repeat_observations.reserve(script->assertions.size());
        std::size_t next_event = 0U;
        std::size_t next_assertion = 0U;
        for (std::uint64_t tick = 0U; tick < script->frames; ++tick) {
            for (auto& action : actions) {
                action.pressed = false;
                action.released = false;
            }
            while (next_event < script->events.size() && script->events[next_event].tick == tick) {
                const auto& event = script->events[next_event++];
                auto& action = actions[event.action_index];
                if (event.kind == ai2d::GameInputEventKind::press) {
                    action.down = true;
                    action.pressed = true;
                } else if (event.kind == ai2d::GameInputEventKind::release) {
                    action.down = false;
                    action.released = true;
                } else {
                    action.down = false;
                    action.pressed = true;
                    action.released = true;
                }
            }
            auto frame = runtime.run_exact_actions(actions, 1U);
            if (!frame) return emit_failure("verify", std::move(frame.error()), options.json);
            last_frame = *frame;
            aggregate.add(*frame);
            while (next_assertion < script->assertions.size() && script->assertions[next_assertion].tick == tick) {
                auto observed = evaluate_assertion(script->assertions[next_assertion], runtime, aggregate, *plan);
                if (!observed) return emit_failure("verify", std::move(observed.error()), options.json);
                repeat_observations.push_back(std::move(*observed));
                ++next_assertion;
            }
        }
        if (auto idle = runtime.wait_idle(); !idle) return emit_failure("verify", std::move(idle.error()), options.json);
        VerifyResult result{};
        result.scene_index = runtime.current_scene_index();
        result.states.reserve(plan->states.size());
        for (std::uint32_t state_index = 0U; state_index < plan->states.size(); ++state_index) {
            auto value = runtime.state_value(state_index);
            if (!value) return emit_failure("verify", std::move(value.error()), options.json);
            result.states.push_back(*value);
        }
        result.state_checksum = last_frame.state_checksum;
        result.scene_state_checksum = last_frame.scene_state_checksum;
        result.contact_state_checksum = runtime.contact_state_checksum();
        result.metrics = aggregate;
        if (!has_baseline) {
            baseline = std::move(result);
            observations = std::move(repeat_observations);
            has_baseline = true;
        } else if (result.scene_index != baseline.scene_index || result.states != baseline.states ||
                   result.state_checksum != baseline.state_checksum ||
                   result.scene_state_checksum != baseline.scene_state_checksum ||
                   result.contact_state_checksum != baseline.contact_state_checksum ||
                   !(result.metrics == baseline.metrics) || repeat_observations != observations) {
            auto diagnostic = ai2d::Diagnostic::make(
                ai2d::DiagnosticCode::game_test_nondeterministic,
                ai2d::Severity::error,
                "game_verify",
                "Repeated game-test runs produced different final observations");
            diagnostic.context.push_back({"repeat", static_cast<std::uint64_t>(repeat + 1U)});
            return emit_failure("verify", std::move(diagnostic), options.json);
        }
    }
    if (!options.json) {
        std::cout << "game verify: PASS (frames=" << script->frames << ", repeat=" << options.repeat << ")\n";
        return 0;
    }
    ai2d::JsonWriter writer{};
    begin_success(writer, "verify");
    writer.key("diagnostics");
    writer.begin_array();
    writer.end_array();
    writer.key("metrics");
    writer.begin_object();
    writer.key("manifest");
    writer.value(plan->manifest_path.generic_string());
    writer.key("test_script");
    writer.value(options.test_script.generic_string());
    writer.key("frames");
    writer.value(static_cast<std::uint64_t>(script->frames));
    writer.key("repeat");
    writer.value(static_cast<std::uint64_t>(options.repeat));
    writer.key("scene");
    writer.value(plan->symbol(plan->scenes[baseline.scene_index].symbol));
    writer.key("states");
    writer.begin_object();
    for (std::size_t index = 0U; index < baseline.states.size(); ++index) {
        writer.key(plan->symbol(plan->states[index].symbol));
        writer.value(static_cast<std::int64_t>(baseline.states[index]));
    }
    writer.end_object();
    writer.key("state_checksum");
    writer.value(baseline.state_checksum);
    writer.key("scene_state_checksum");
    writer.value(baseline.scene_state_checksum);
    writer.key("contact_state_checksum");
    writer.value(baseline.contact_state_checksum);
    writer.key("contact_begins");
    writer.value(baseline.metrics.contact_begins);
    writer.key("contact_ends");
    writer.value(baseline.metrics.contact_ends);
    writer.key("trigger_narrowphase_tests");
    writer.value(baseline.metrics.trigger_narrowphase_tests);
    writer.key("linear_motion_updates");
    writer.value(baseline.metrics.linear_motion_updates);
    writer.key("assertions");
    writer.begin_array();
    for (const auto& observation : observations) {
        writer.begin_object();
        writer.key("tick");
        writer.value(observation.tick);
        writer.key("kind");
        writer.value(ai2d::to_string(observation.kind));
        writer.key("actual");
        if (observation.kind == ai2d::GameTestAssertionKind::current_scene) {
            writer.value(observation.scene);
        } else if (observation.kind == ai2d::GameTestAssertionKind::int_state) {
            writer.value(observation.signed_value);
        } else if (observation.kind == ai2d::GameTestAssertionKind::entity_active) {
            writer.value(observation.bool_value);
        } else if (observation.kind == ai2d::GameTestAssertionKind::position ||
                   observation.kind == ai2d::GameTestAssertionKind::velocity) {
            writer.begin_array();
            writer.value(static_cast<double>(observation.vector_value.x));
            writer.value(static_cast<double>(observation.vector_value.y));
            writer.end_array();
        } else {
            writer.value(observation.unsigned_value);
        }
        writer.end_object();
    }
    writer.end_array();
    writer.end_object();
    end_success(writer);
    std::cout << writer.str() << '\n';
    return 0;
}

} // namespace

int ai2d_game_command_main(const int argument_count, const char* const* arguments) {
    const bool json = wants_json(argument_count, arguments);
    if (argument_count < 3) return emit_invalid("", json, "usage: ai2d_cli game <validate|inspect|run|verify>");
    const std::string_view operation{arguments[2]};
    if (operation == "--help" || operation == "-h" || operation == "help") {
        std::cout
            << "usage: ai2d_cli game <validate|inspect|run|verify> [options]\n"
            << "  validate <game.json>                     compile and validate a game\n"
            << "  inspect <game.json>                      emit compiled plan metadata\n"
            << "  run <game.json> [--frames N]             run a game\n"
            << "      [--headless|--offscreen] [--hidden] [--input-script FILE]\n"
            << "      [--fps 60|120|144|240|unlimited] [--no-audio] [--json]\n";
        std::cout
            << "  verify <game.json> --test-script FILE    run deterministic assertions\n"
            << "      [--repeat 1..16] [--offscreen] [--json]\n";
        return 0;
    }
    if (operation == "validate" || operation == "inspect") {
        return validate_or_inspect(operation, argument_count, arguments);
    }
    if (operation == "run") return run_game(argument_count, arguments);
    if (operation == "verify") {
        if (argument_count == 4 &&
            (std::string_view{arguments[3]} == "--help" || std::string_view{arguments[3]} == "-h")) {
            std::cout
                << "usage: ai2d_cli game verify <game.json> --test-script FILE\n"
                << "       [--repeat 1..16] [--offscreen] [--hidden] [--json]\n";
            return 0;
        }
        return verify_game(argument_count, arguments);
    }
    return emit_invalid(operation, json, "Unknown game operation");
}
