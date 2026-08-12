#include "scenario_commands.hpp"

#include "ai2d/foundation/allocation_tracker.hpp"
#include "ai2d/foundation/build_info.hpp"
#include "ai2d/foundation/diagnostic.hpp"
#include "ai2d/foundation/json_writer.hpp"
#include "ai2d/foundation/statistics.hpp"
#include "ai2d/runtime/runtime.hpp"
#include "ai2d/scenario/scenario.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#endif

namespace {

struct CommandOptions final {
    std::filesystem::path scenario{};
    std::filesystem::path raw_jsonl{};
    std::uint32_t frames{600U};
    bool json{false};
    bool headless{false};
};

bool json_requested(const int argument_count, const char* const* const arguments) noexcept {
    for (int index = 1; index < argument_count; ++index) {
        if (std::string_view{arguments[index]} == "--json") {
            return true;
        }
    }
    return false;
}

bool parse_u32(const std::string_view value, std::uint32_t& output) noexcept {
    const auto result = std::from_chars(value.data(), value.data() + value.size(), output);
    return result.ec == std::errc{} && result.ptr == value.data() + value.size() && output > 0U;
}

bool parse_run_options(
    const int argument_count,
    const char* const* const arguments,
    const bool allow_headless,
    const bool allow_raw_jsonl,
    CommandOptions& options) {
    if (argument_count < 3 || std::string_view{arguments[2]}.starts_with("--")) {
        return false;
    }
    options.scenario = arguments[2];
    for (int index = 3; index < argument_count; ++index) {
        const std::string_view option{arguments[index]};
        if (option == "--json") {
            options.json = true;
        } else if (allow_headless && option == "--headless") {
            options.headless = true;
        } else if (option == "--frames" && index + 1 < argument_count) {
            ++index;
            if (!parse_u32(arguments[index], options.frames)) {
                return false;
            }
        } else if (allow_raw_jsonl && option == "--raw-jsonl" && index + 1 < argument_count) {
            options.raw_jsonl = arguments[++index];
        } else {
            return false;
        }
    }
    return true;
}

void write_build(ai2d::JsonWriter& writer) {
    const auto build = ai2d::current_build_info();
    writer.begin_object();
    writer.key("version");
    writer.value(build.version);
    writer.key("compiler");
    writer.value(build.compiler);
    writer.key("configuration");
    writer.value(build.configuration);
    writer.end_object();
}

void begin_envelope(
    ai2d::JsonWriter& writer,
    const std::string_view command,
    const std::string_view status,
    const std::span<const ai2d::Diagnostic> diagnostics = {}) {
    writer.begin_object();
    writer.key("schema_version");
    writer.value(std::uint64_t{1U});
    writer.key("command");
    writer.value(command);
    writer.key("status");
    writer.value(status);
    writer.key("build");
    write_build(writer);
    writer.key("environment");
    writer.begin_object();
    writer.end_object();
    writer.key("diagnostics");
    writer.begin_array();
    for (const auto& diagnostic : diagnostics) {
        ai2d::write_json(writer, diagnostic);
    }
    writer.end_array();
    writer.key("metrics");
}

void end_envelope(ai2d::JsonWriter& writer, const std::span<const std::filesystem::path> artifacts = {}) {
    writer.key("artifacts");
    writer.begin_array();
    for (const auto& artifact : artifacts) {
        writer.value(artifact.string());
    }
    writer.end_array();
    writer.end_object();
}

int emit_document(ai2d::JsonWriter& writer, const bool json, const std::string_view human, const int exit_code) {
    if (!writer.complete()) {
        std::cerr << "JSON serialization failed\n";
        return 4;
    }
    if (json) {
        std::cout << writer.str() << '\n';
    } else if (exit_code == 0) {
        std::cout << human << '\n';
    } else {
        std::cerr << human << '\n';
    }
    return exit_code;
}

int emit_failure(
    const std::string_view command,
    const ai2d::Diagnostic& diagnostic,
    const bool json,
    const int exit_code) {
    ai2d::JsonWriter writer{};
    const std::array diagnostics{diagnostic};
    begin_envelope(writer, command, "fail", diagnostics);
    writer.begin_object();
    writer.end_object();
    end_envelope(writer);
    return emit_document(writer, json, diagnostic.message, exit_code);
}

int emit_invalid_arguments(
    const std::string_view command,
    const bool json,
    const std::string_view message) {
    return emit_failure(
        command,
        ai2d::Diagnostic::make(
            ai2d::DiagnosticCode::input_invalid,
            ai2d::Severity::error,
            "cli",
            std::string{message}),
        json,
        2);
}

void write_raw_frame(ai2d::JsonWriter& writer, const ai2d::RuntimeFrameMetrics& frame);

std::filesystem::path temporary_output_path(const std::filesystem::path& destination) {
    static std::atomic_uint64_t sequence{0U};
    auto temporary = destination;
    const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
    temporary += ".tmp-" + std::to_string(timestamp) + "-" +
                 std::to_string(sequence.fetch_add(1U, std::memory_order_relaxed));
    return temporary;
}

ai2d::Result<void> replace_file_atomically(
    const std::filesystem::path& temporary,
    const std::filesystem::path& destination) {
#if defined(_WIN32)
    std::error_code exists_error{};
    const bool destination_exists = std::filesystem::exists(destination, exists_error);
    if (exists_error) {
        auto diagnostic = ai2d::Diagnostic::make(
            ai2d::DiagnosticCode::input_invalid,
            ai2d::Severity::error,
            "profile",
            "Raw JSONL destination could not be inspected before replacement");
        diagnostic.context.push_back({"path", destination.string()});
        diagnostic.context.push_back({"error", exists_error.message()});
        return std::unexpected(std::move(diagnostic));
    }
    const BOOL replaced = destination_exists
                              ? ReplaceFileW(
                                    destination.c_str(),
                                    temporary.c_str(),
                                    nullptr,
                                    REPLACEFILE_WRITE_THROUGH,
                                    nullptr,
                                    nullptr)
                              : MoveFileExW(
                                    temporary.c_str(),
                                    destination.c_str(),
                                    MOVEFILE_WRITE_THROUGH);
    if (replaced == 0) {
        auto diagnostic = ai2d::Diagnostic::make(
            ai2d::DiagnosticCode::input_invalid,
            ai2d::Severity::error,
            "profile",
            "Raw JSONL output could not atomically replace its destination");
        diagnostic.context.push_back({"path", destination.string()});
        diagnostic.context.push_back({"os_error", static_cast<std::uint64_t>(GetLastError())});
        return std::unexpected(std::move(diagnostic));
    }
#else
    std::error_code rename_error{};
    std::filesystem::rename(temporary, destination, rename_error);
    if (rename_error) {
        auto diagnostic = ai2d::Diagnostic::make(
            ai2d::DiagnosticCode::input_invalid,
            ai2d::Severity::error,
            "profile",
            "Raw JSONL output could not atomically replace its destination");
        diagnostic.context.push_back({"path", destination.string()});
        diagnostic.context.push_back({"error", rename_error.message()});
        return std::unexpected(std::move(diagnostic));
    }
#endif
    return {};
}

ai2d::Result<void> write_raw_frames_atomically(
    const std::filesystem::path& destination,
    const std::span<const ai2d::RuntimeFrameMetrics> frames) {
    const auto temporary = temporary_output_path(destination);
    const auto discard_temporary = [&temporary]() noexcept {
        std::error_code ignored{};
        std::filesystem::remove(temporary, ignored);
    };
    std::ofstream stream{temporary, std::ios::binary | std::ios::trunc};
    if (!stream) {
        auto diagnostic = ai2d::Diagnostic::make(
            ai2d::DiagnosticCode::input_invalid,
            ai2d::Severity::error,
            "profile",
            "Raw JSONL temporary output could not be opened");
        diagnostic.context.push_back({"path", destination.string()});
        return std::unexpected(std::move(diagnostic));
    }
    for (const auto& frame : frames) {
        ai2d::JsonWriter line{};
        write_raw_frame(line, frame);
        if (!line.complete()) {
            stream.close();
            discard_temporary();
            return std::unexpected(ai2d::Diagnostic::make(
                ai2d::DiagnosticCode::internal_error,
                ai2d::Severity::error,
                "profile",
                "Raw JSONL frame serialization failed"));
        }
        const auto& text = line.str();
        stream.write(text.data(), static_cast<std::streamsize>(text.size()));
        stream.put('\n');
        if (!stream) {
            stream.close();
            discard_temporary();
            return std::unexpected(ai2d::Diagnostic::make(
                ai2d::DiagnosticCode::input_invalid,
                ai2d::Severity::error,
                "profile",
                "Raw JSONL temporary output could not be written completely"));
        }
    }
    stream.flush();
    const bool flushed = stream.good();
    stream.close();
    if (!flushed || stream.fail()) {
        discard_temporary();
        return std::unexpected(ai2d::Diagnostic::make(
            ai2d::DiagnosticCode::input_invalid,
            ai2d::Severity::error,
            "profile",
            "Raw JSONL temporary output could not be finalized"));
    }
    auto replaced = replace_file_atomically(temporary, destination);
    if (!replaced) {
        discard_temporary();
        return replaced;
    }
    return {};
}

void write_world_metrics(ai2d::JsonWriter& writer, const ai2d::RuntimeWorldFrameMetrics& metrics) {
    writer.begin_object();
    writer.key("alive_entities");
    writer.value(static_cast<std::uint64_t>(metrics.alive_entities));
    writer.key("transform_count");
    writer.value(static_cast<std::uint64_t>(metrics.transform_count));
    writer.key("velocity_count");
    writer.value(static_cast<std::uint64_t>(metrics.velocity_count));
    writer.key("sprite_count");
    writer.value(static_cast<std::uint64_t>(metrics.sprite_count));
    writer.key("query_visited");
    writer.value(metrics.query_visited);
    writer.key("query_matched");
    writer.value(metrics.query_matched);
    writer.key("system_invocations");
    writer.value(metrics.system_invocations);
    writer.key("capacity_growth_events");
    writer.value(metrics.capacity_growth_events);
    writer.end_object();
}

void write_render_metrics(ai2d::JsonWriter& writer, const ai2d::RuntimeRenderFrameMetrics& metrics) {
    writer.begin_object();
    writer.key("extracted_sprites");
    writer.value(static_cast<std::uint64_t>(metrics.extracted_sprites));
    writer.key("visible_sprites");
    writer.value(static_cast<std::uint64_t>(metrics.visible_sprites));
    writer.key("culled_sprites");
    writer.value(static_cast<std::uint64_t>(metrics.culled_sprites));
    writer.key("batches");
    writer.value(static_cast<std::uint64_t>(metrics.batches));
    writer.key("sprite_draw_calls");
    writer.value(static_cast<std::uint64_t>(metrics.sprite_draw_calls));
    writer.key("texture_binds");
    writer.value(static_cast<std::uint64_t>(metrics.texture_binds));
    writer.key("instance_upload_bytes");
    writer.value(metrics.instance_upload_bytes);
    writer.key("render_queue_capacity");
    writer.value(static_cast<std::uint64_t>(metrics.render_queue_capacity));
    writer.key("unique_textures");
    writer.value(static_cast<std::uint64_t>(metrics.unique_textures));
    writer.key("active_layers");
    writer.value(static_cast<std::uint64_t>(metrics.active_layers));
    writer.key("capacity_growth_events");
    writer.value(metrics.capacity_growth_events);
    writer.key("vma_allocation_count");
    writer.value(metrics.vma_allocation_count);
    writer.key("vma_allocation_bytes");
    writer.value(metrics.vma_allocation_bytes);
    writer.key("texture_bytes");
    writer.value(metrics.texture_bytes);
    writer.key("instance_buffer_capacity_bytes");
    writer.value(metrics.instance_buffer_capacity_bytes);
    writer.end_object();
}

void write_distribution(ai2d::JsonWriter& writer, const ai2d::DistributionSummary& summary) {
    writer.begin_object();
    writer.key("sample_count");
    writer.value(static_cast<std::uint64_t>(summary.sample_count));
    writer.key("average");
    writer.value(summary.average);
    writer.key("median");
    writer.value(summary.median);
    writer.key("p95");
    writer.value(summary.p95);
    writer.key("p99");
    writer.value(summary.p99);
    writer.key("minimum");
    writer.value(summary.minimum);
    writer.key("maximum");
    writer.value(summary.maximum);
    writer.key("mad");
    writer.value(summary.mad);
    writer.end_object();
}

int validate_main(const int argument_count, const char* const* const arguments) {
    const bool requested_json = json_requested(argument_count, arguments);
    if (argument_count < 3 || std::string_view{arguments[2]}.starts_with("--")) {
        return emit_invalid_arguments(
            "validate", requested_json, "usage: ai2d_cli validate <scenario> [--json]");
    }
    bool json = false;
    for (int index = 3; index < argument_count; ++index) {
        if (std::string_view{arguments[index]} == "--json") {
            json = true;
        } else {
            return emit_invalid_arguments("validate", requested_json, "Unknown validate option");
        }
    }
    const auto plan = ai2d::compile_scenario_file(arguments[2]);
    if (!plan) {
        return emit_failure("validate", plan.error(), json, 1);
    }
    ai2d::JsonWriter writer{};
    begin_envelope(writer, "validate", "pass");
    writer.begin_object();
    writer.key("schema_version");
    writer.value(ai2d::ExecutionPlan::supported_schema_version);
    writer.key("name");
    writer.value(plan->symbol(plan->scenario_name));
    writer.key("scenario_hash");
    writer.value(plan->scenario_hash);
    writer.key("plan_hash");
    writer.value(plan->plan_hash);
    writer.key("spawn_count");
    writer.value(static_cast<std::uint64_t>(plan->total_spawn_count));
    writer.key("system_count");
    writer.value(static_cast<std::uint64_t>(plan->systems.size()));
    writer.end_object();
    end_envelope(writer);
    return emit_document(writer, json, "ScenarioSpec validation: PASS", 0);
}

int inspect_main(const int argument_count, const char* const* const arguments) {
    const bool requested_json = json_requested(argument_count, arguments);
    if (argument_count < 3) {
        return emit_invalid_arguments("inspect", requested_json, "inspect requires a kind");
    }
    const std::string_view kind{arguments[2]};
    bool json = false;
    std::filesystem::path input{};
    for (int index = 3; index < argument_count; ++index) {
        if (std::string_view{arguments[index]} == "--json") {
            json = true;
        } else if (input.empty()) {
            input = arguments[index];
        } else {
            return emit_invalid_arguments("inspect", requested_json, "Too many inspect arguments");
        }
    }
    ai2d::JsonWriter writer{};
    if (kind == "diagnostics-schema") {
        if (!input.empty()) {
            return emit_invalid_arguments(
                "inspect", requested_json, "diagnostics-schema does not accept an input file");
        }
        begin_envelope(writer, "inspect", "pass");
        ai2d::write_diagnostics_schema_json(writer);
        end_envelope(writer);
        return emit_document(writer, json, "diagnostics schema: PASS", 0);
    }
    if ((kind != "plan" && kind != "scenario") || input.empty()) {
        return emit_invalid_arguments("inspect", requested_json, "Unknown inspect kind or missing input file");
    }
    const auto plan = ai2d::compile_scenario_file(input);
    if (!plan) {
        return emit_failure("inspect", plan.error(), json, 1);
    }
    begin_envelope(writer, "inspect", "pass");
    if (kind == "plan") {
        ai2d::write_execution_plan_json(writer, *plan);
    } else {
        ai2d::write_scenario_summary_json(writer, *plan);
    }
    end_envelope(writer);
    return emit_document(writer, json, std::string{kind} + " inspection: PASS", 0);
}

int run_main(const int argument_count, const char* const* const arguments) {
    CommandOptions options{};
    if (!parse_run_options(argument_count, arguments, true, false, options)) {
        return emit_invalid_arguments(
            "run",
            json_requested(argument_count, arguments),
            "usage: ai2d_cli run <scenario> [--frames N] [--headless] [--json]");
    }
    const auto plan = ai2d::compile_scenario_file(options.scenario);
    if (!plan) {
        return emit_failure("run", plan.error(), options.json, 1);
    }
    ai2d::RuntimeOptions runtime_options{};
    runtime_options.headless = options.headless;
    runtime_options.hidden = false;
    ai2d::ScenarioRuntime runtime{};
    const auto loaded = runtime.initialize(*plan, runtime_options);
    if (!loaded) {
        return emit_failure("run", loaded.error(), options.json, 1);
    }
    ai2d::RuntimeFrameMetrics final_frame{};
    std::uint32_t completed = 0U;
    ai2d::MeasuredAllocationScope measured{};
    ai2d::Result<ai2d::RuntimeFrameMetrics> frame = final_frame;
    for (; completed < options.frames; ++completed) {
        frame = runtime.run_frame();
        if (!frame) {
            break;
        }
        final_frame = *frame;
        if (final_frame.quit_requested) {
            ++completed;
            break;
        }
    }
    const auto allocations = measured.finish();
    if (!frame) {
        return emit_failure("run", frame.error(), options.json, 1);
    }
    if (auto idle = runtime.wait_idle(); !idle) {
        return emit_failure("run", idle.error(), options.json, 1);
    }
    const auto invariant_pass = allocations.allocations == 0U &&
                                final_frame.world.capacity_growth_events == 0U &&
                                final_frame.render.capacity_growth_events == 0U;
    std::vector<ai2d::Diagnostic> diagnostics{};
    if (!invariant_pass) {
        diagnostics.push_back(ai2d::Diagnostic::make(
            ai2d::DiagnosticCode::mem_frame_heap_allocation,
            ai2d::Severity::error,
            "runtime",
            "Measured runtime frames violated a zero-growth memory invariant"));
    }
    ai2d::JsonWriter writer{};
    begin_envelope(writer, "run", invariant_pass ? "pass" : "fail", diagnostics);
    writer.begin_object();
    writer.key("name");
    writer.value(plan->symbol(plan->scenario_name));
    writer.key("scenario_hash");
    writer.value(plan->scenario_hash);
    writer.key("plan_hash");
    writer.value(plan->plan_hash);
    writer.key("mode");
    writer.value(options.headless ? "headless" : "presented");
    writer.key("frames_requested");
    writer.value(static_cast<std::uint64_t>(options.frames));
    writer.key("frames_completed");
    writer.value(static_cast<std::uint64_t>(completed));
    writer.key("load_ms");
    writer.value(loaded->load_ms);
    writer.key("final_frame_cpu_ms");
    writer.value(final_frame.frame_cpu_ms);
    writer.key("world");
    write_world_metrics(writer, final_frame.world);
    writer.key("render");
    write_render_metrics(writer, final_frame.render);
    writer.key("memory");
    writer.begin_object();
    writer.key("tracked_cpp_heap_allocations");
    writer.value(allocations.allocations);
    writer.key("tracked_cpp_heap_bytes");
    writer.value(allocations.bytes);
    writer.key("frame_arena_overflows");
    writer.null_value();
    writer.key("frame_arena_overflows_unavailable_reason");
    writer.value("Scenario execution does not use or instrument a FrameArena");
    writer.key("process_rss_bytes");
    writer.null_value();
    writer.key("process_rss_unavailable_reason");
    writer.value("No portable dependency-free process RSS probe is implemented");
    writer.end_object();
    writer.key("checksum");
    writer.value(final_frame.checksum);
    writer.end_object();
    end_envelope(writer);
    return emit_document(writer, options.json, invariant_pass ? "scenario run: PASS" : "scenario run: FAIL", invariant_pass ? 0 : 1);
}

void write_raw_frame(ai2d::JsonWriter& writer, const ai2d::RuntimeFrameMetrics& frame) {
    writer.begin_object();
    writer.key("frame_index");
    writer.value(frame.frame_index);
    writer.key("frame_cpu_ms");
    writer.value(frame.frame_cpu_ms);
    writer.key("fixed_update_ms");
    writer.value(frame.fixed_update_ms);
    writer.key("post_update_ms");
    writer.value(frame.post_update_ms);
    writer.key("render_extraction_ms");
    writer.value(frame.render_extraction_ms);
    writer.key("render_queue_build_ms");
    writer.value(frame.render_queue_build_ms);
    writer.key("render_submission_cpu_ms");
    writer.value(frame.render_submission_cpu_ms);
    writer.key("sprite_pass_gpu_ms");
    if (frame.gpu_timing_available) {
        writer.value(frame.sprite_pass_gpu_ms);
    } else {
        writer.null_value();
    }
    writer.end_object();
}

int profile_main(const int argument_count, const char* const* const arguments) {
    CommandOptions options{};
    if (!parse_run_options(argument_count, arguments, false, true, options)) {
        return emit_invalid_arguments(
            "profile",
            json_requested(argument_count, arguments),
            "usage: ai2d_cli profile <scenario> [--frames N] [--raw-jsonl path] [--json]");
    }
    const auto plan = ai2d::compile_scenario_file(options.scenario);
    if (!plan) {
        return emit_failure("profile", plan.error(), options.json, 1);
    }
    ai2d::RuntimeOptions runtime_options{};
    runtime_options.offscreen = true;
    runtime_options.hidden = true;
    ai2d::ScenarioRuntime runtime{};
    const auto loaded = runtime.initialize(*plan, runtime_options);
    if (!loaded) {
        return emit_failure("profile", loaded.error(), options.json, 1);
    }

    std::vector<double> frame_cpu{};
    std::vector<double> fixed_update{};
    std::vector<double> extraction{};
    std::vector<double> queue{};
    std::vector<double> submission{};
    std::vector<double> gpu{};
    frame_cpu.reserve(options.frames);
    fixed_update.reserve(options.frames);
    extraction.reserve(options.frames);
    queue.reserve(options.frames);
    submission.reserve(options.frames);
    gpu.reserve(options.frames);
    std::vector<ai2d::RuntimeFrameMetrics> raw_frames{};
    if (!options.raw_jsonl.empty()) {
        raw_frames.reserve(options.frames);
    }

    ai2d::RuntimeFrameMetrics final_frame{};
    bool frames_ok = true;
    ai2d::Diagnostic frame_error{};
    ai2d::MeasuredAllocationScope measured{};
    for (std::uint32_t index = 0U; index < options.frames; ++index) {
        auto frame = runtime.run_frame();
        if (!frame) {
            frames_ok = false;
            frame_error = std::move(frame.error());
            break;
        }
        final_frame = *frame;
        frame_cpu.push_back(frame->frame_cpu_ms);
        fixed_update.push_back(frame->fixed_update_ms);
        extraction.push_back(frame->render_extraction_ms);
        queue.push_back(frame->render_queue_build_ms);
        submission.push_back(frame->render_submission_cpu_ms);
        if (frame->gpu_timing_available) {
            gpu.push_back(frame->sprite_pass_gpu_ms);
        }
        if (!options.raw_jsonl.empty()) {
            raw_frames.push_back(*frame);
        }
    }
    const auto allocations = measured.finish();
    if (!frames_ok) {
        return emit_failure("profile", frame_error, options.json, 1);
    }
    if (auto idle = runtime.wait_idle(); !idle) {
        return emit_failure("profile", idle.error(), options.json, 1);
    }
    if (!options.raw_jsonl.empty()) {
        if (auto written = write_raw_frames_atomically(options.raw_jsonl, raw_frames); !written) {
            const auto exit_code = written.error().code == ai2d::DiagnosticCode::internal_error ? 4 : 2;
            return emit_failure("profile", written.error(), options.json, exit_code);
        }
    }
    const auto invariant_pass = allocations.allocations == 0U &&
                                final_frame.world.capacity_growth_events == 0U &&
                                final_frame.render.capacity_growth_events == 0U;
    ai2d::JsonWriter writer{};
    begin_envelope(writer, "profile", invariant_pass ? "pass" : "fail");
    writer.begin_object();
    writer.key("name");
    writer.value(plan->symbol(plan->scenario_name));
    writer.key("scenario_hash");
    writer.value(plan->scenario_hash);
    writer.key("plan_hash");
    writer.value(plan->plan_hash);
    writer.key("mode");
    writer.value("offscreen");
    writer.key("frames");
    writer.value(static_cast<std::uint64_t>(options.frames));
    writer.key("load_ms");
    writer.value(loaded->load_ms);
    writer.key("cpu_ms");
    writer.begin_object();
    writer.key("frame");
    write_distribution(writer, ai2d::summarize(frame_cpu));
    writer.key("fixed_update");
    write_distribution(writer, ai2d::summarize(fixed_update));
    writer.key("render_extraction");
    write_distribution(writer, ai2d::summarize(extraction));
    writer.key("render_queue_build");
    write_distribution(writer, ai2d::summarize(queue));
    writer.key("render_submission");
    write_distribution(writer, ai2d::summarize(submission));
    writer.end_object();
    writer.key("gpu");
    writer.begin_object();
    writer.key("sprite_pass_ms");
    if (!gpu.empty()) {
        write_distribution(writer, ai2d::summarize(gpu));
        writer.key("unavailable_reason");
        writer.null_value();
    } else {
        writer.null_value();
        writer.key("unavailable_reason");
        writer.value("Vulkan timestamp results were unavailable for all measured frames");
    }
    writer.end_object();
    writer.key("world");
    write_world_metrics(writer, final_frame.world);
    writer.key("render");
    write_render_metrics(writer, final_frame.render);
    writer.key("memory");
    writer.begin_object();
    writer.key("tracked_cpp_heap_allocations");
    writer.value(allocations.allocations);
    writer.key("tracked_cpp_heap_bytes");
    writer.value(allocations.bytes);
    writer.key("frame_arena_overflows");
    writer.null_value();
    writer.key("frame_arena_overflows_unavailable_reason");
    writer.value("Scenario profiling does not use or instrument a FrameArena");
    writer.key("process_rss_bytes");
    writer.null_value();
    writer.key("process_rss_unavailable_reason");
    writer.value("No portable dependency-free process RSS probe is implemented");
    writer.end_object();
    writer.key("checksum");
    writer.value(final_frame.checksum);
    writer.end_object();
    const std::array artifact_paths{options.raw_jsonl};
    end_envelope(writer, options.raw_jsonl.empty() ? std::span<const std::filesystem::path>{}
                                                  : std::span<const std::filesystem::path>{artifact_paths});
    return emit_document(writer, options.json, invariant_pass ? "scenario profile: PASS" : "scenario profile: FAIL", invariant_pass ? 0 : 1);
}

} // namespace

int ai2d_scenario_command_main(const int argument_count, const char* const* const arguments) {
    const std::string_view command{arguments[1]};
    if (command == "validate") {
        return validate_main(argument_count, arguments);
    }
    if (command == "run") {
        return run_main(argument_count, arguments);
    }
    if (command == "profile") {
        return profile_main(argument_count, arguments);
    }
    if (command == "inspect") {
        return inspect_main(argument_count, arguments);
    }
    return 2;
}
