#include "ai2d/foundation/allocation_tracker.hpp"
#include "ai2d/foundation/build_info.hpp"
#include "ai2d/foundation/diagnostic.hpp"
#include "ai2d/foundation/json_writer.hpp"
#include "ai2d/foundation/statistics.hpp"
#include "ai2d/foundation/timer.hpp"
#include "ai2d/world/components.hpp"
#include "ai2d/world/operations.hpp"
#include "ai2d/world/world.hpp"
#include "render_benchmark.hpp"
#include "game_commands.hpp"
#include "scenario_benchmark.hpp"
#include "scenario_commands.hpp"

#if defined(AI2D_ENABLE_GPU)
#include "ai2d/platform/window.hpp"
#include "ai2d/renderer2d/renderer.hpp"
#endif

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct BenchmarkArguments final {
    std::vector<std::size_t> counts{1000U, 10000U, 50000U, 100000U};
    std::size_t runs{5U};
    std::size_t warmup{300U};
    std::size_t frames{3000U};
    bool quick{false};
    bool json{false};
    std::string suite{"world"};
};

struct WorldBenchmarkResult final {
    std::size_t entity_count{0U};
    double create_ms{0.0};
    ai2d::DistributionSummary query_transform_sprite_ms{};
    ai2d::DistributionSummary integrate_velocity_ms{};
    ai2d::DistributionSummary wrap_bounds_ms{};
    ai2d::AllocationSnapshot allocations{};
    ai2d::WorldMetrics world{};
    ai2d::WorldCapacitySnapshot capacity_before{};
    ai2d::WorldCapacitySnapshot capacity_after{};
    double checksum{0.0};
    bool invariant_pass{true};
};

bool json_requested(const int argument_count, const char* const* const arguments) noexcept {
    for (int index = 1; index < argument_count; ++index) {
        if (std::string_view{arguments[index]} == "--json") {
            return true;
        }
    }
    return false;
}

int emit_cli_failure(
    const std::string_view command,
    ai2d::Diagnostic diagnostic,
    const bool json,
    const int exit_code) {
    if (!json) {
        std::cerr << diagnostic.message << '\n';
        return exit_code;
    }
    const auto build = ai2d::current_build_info();
    ai2d::JsonWriter writer{};
    writer.begin_object();
    writer.key("schema_version");
    writer.value(std::uint64_t{1U});
    writer.key("command");
    writer.value(command);
    writer.key("status");
    writer.value(exit_code == 3 ? "unavailable" : "fail");
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
    return exit_code;
}

int emit_invalid_arguments(
    const std::string_view command,
    const bool json,
    const std::string_view message) {
    return emit_cli_failure(
        command,
        ai2d::Diagnostic::make(
            ai2d::DiagnosticCode::input_invalid,
            ai2d::Severity::error,
            "cli",
            std::string{message}),
        json,
        2);
}

template <class Integer>
bool parse_integer(const std::string_view text, Integer& value) {
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

bool parse_counts(const std::string_view text, std::vector<std::size_t>& output) {
    output.clear();
    std::size_t begin = 0U;
    while (begin < text.size()) {
        const auto comma = text.find(',', begin);
        const auto end = comma == std::string_view::npos ? text.size() : comma;
        std::size_t count = 0U;
        if (!parse_integer(text.substr(begin, end - begin), count) || count == 0U || count > 1'000'000U) {
            return false;
        }
        output.push_back(count);
        if (comma == std::string_view::npos) {
            break;
        }
        begin = comma + 1U;
    }
    return !output.empty();
}

bool parse_arguments(const int count, const char* const* const values, BenchmarkArguments& arguments) {
    for (int index = 2; index < count; ++index) {
        const std::string_view option{values[index]};
        const auto require_value = [&]() -> const char* {
            if (index + 1 >= count) {
                return nullptr;
            }
            ++index;
            return values[index];
        };
        if (option == "--json") {
            arguments.json = true;
        } else if (option == "--quick") {
            arguments.quick = true;
        } else if (option == "--suite") {
            const auto* value = require_value();
            if (value == nullptr) {
                return false;
            }
            arguments.suite = value;
        } else if (option == "--counts") {
            const auto* value = require_value();
            if (value == nullptr || !parse_counts(value, arguments.counts)) {
                return false;
            }
        } else if (option == "--runs") {
            const auto* value = require_value();
            if (value == nullptr || !parse_integer(std::string_view{value}, arguments.runs) || arguments.runs == 0U) {
                return false;
            }
        } else if (option == "--warmup") {
            const auto* value = require_value();
            if (value == nullptr || !parse_integer(std::string_view{value}, arguments.warmup)) {
                return false;
            }
        } else if (option == "--frames") {
            const auto* value = require_value();
            if (value == nullptr || !parse_integer(std::string_view{value}, arguments.frames) ||
                arguments.frames == 0U) {
                return false;
            }
        } else {
            return false;
        }
    }
    if (arguments.quick) {
        arguments.runs = std::min(arguments.runs, std::size_t{3U});
        arguments.warmup = std::min(arguments.warmup, std::size_t{30U});
        arguments.frames = std::min(arguments.frames, std::size_t{200U});
    }
    return arguments.suite == "world" || arguments.suite == "render" || arguments.suite == "scenario";
}

bool populate_world(ai2d::World& world, const std::size_t entity_count, ai2d::Diagnostic& error) {
    if (auto reserved = world.reserve(entity_count); !reserved) {
        error = std::move(reserved.error());
        return false;
    }
    for (std::size_t index = 0U; index < entity_count; ++index) {
        const auto x = static_cast<float>(index % 1024U) * 0.125F;
        const auto y = static_cast<float>((index / 1024U) % 1024U) * 0.125F;
        const ai2d::Transform2D transform{{x, y}, 0.0F, {1.0F, 1.0F}};
        const ai2d::Velocity2D velocity{{0.25F + static_cast<float>(index % 7U) * 0.01F, -0.125F}, 0.01F};
        const ai2d::Sprite2D sprite{
            ai2d::TextureHandle{static_cast<std::uint32_t>(index % 4U), 1U},
            {1.0F, 1.0F},
            {0.5F, 0.5F},
            {},
            static_cast<std::int32_t>(index % 8U),
            true,
        };
        auto entity = world.create_entity(transform, velocity, sprite);
        if (!entity) {
            error = std::move(entity.error());
            return false;
        }
    }
    return true;
}

WorldBenchmarkResult run_world_benchmark(
    const std::size_t entity_count,
    const std::size_t runs,
    const std::size_t warmup,
    const std::size_t frames,
    ai2d::Diagnostic& error) {
    ai2d::World world{};
    ai2d::Stopwatch create_timer{};
    if (!populate_world(world, entity_count, error)) {
        return {};
    }
    const auto create_ms = create_timer.elapsed_milliseconds();
    constexpr ai2d::Rect bounds{{0.0F, 0.0F}, {128.0F, 128.0F}};
    constexpr float fixed_delta = 1.0F / 60.0F;

    for (std::size_t frame = 0U; frame < warmup; ++frame) {
        if (auto integrated = ai2d::integrate_velocity(world, fixed_delta); !integrated) {
            error = std::move(integrated.error());
            return {};
        }
        if (auto wrapped = ai2d::wrap_bounds(world, bounds); !wrapped) {
            error = std::move(wrapped.error());
            return {};
        }
    }

    const auto before = world.capacity_snapshot();
    const auto sample_capacity = runs * frames;
    std::vector<double> query_samples{};
    std::vector<double> integrate_samples{};
    std::vector<double> wrap_samples{};
    query_samples.reserve(sample_capacity);
    integrate_samples.reserve(sample_capacity);
    wrap_samples.reserve(sample_capacity);
    double checksum = 0.0;
    ai2d::AllocationSnapshot allocations{};

    for (std::size_t run = 0U; run < runs; ++run) {
        ai2d::MeasuredAllocationScope measured{};
        for (std::size_t frame = 0U; frame < frames; ++frame) {
            ai2d::Stopwatch timer{};
            std::size_t query_matches = 0U;
            {
                auto query = world.query<ai2d::Transform2D, ai2d::Sprite2D>();
                const auto visited = query.candidate_count();
                for (auto item : query) {
                    checksum += static_cast<double>(item.first.position.x) * 0.0000001 +
                                static_cast<double>(item.second.layer) * 0.00000001;
                    ++query_matches;
                }
                world.record_query(visited, query_matches);
            }
            query_samples.push_back(timer.elapsed_milliseconds());

            timer.reset();
            if (auto integrated = ai2d::integrate_velocity(world, fixed_delta); !integrated) {
                error = std::move(integrated.error());
                return {};
            }
            integrate_samples.push_back(timer.elapsed_milliseconds());

            timer.reset();
            if (auto wrapped = ai2d::wrap_bounds(world, bounds); !wrapped) {
                error = std::move(wrapped.error());
                return {};
            }
            wrap_samples.push_back(timer.elapsed_milliseconds());
        }
        const auto run_allocations = measured.finish();
        allocations.allocations += run_allocations.allocations;
        allocations.bytes += run_allocations.bytes;
    }

    const auto after = world.capacity_snapshot();
    const auto metrics = world.metrics();
    const auto invariant_pass =
        allocations.allocations == 0U && metrics.capacity_growth_events == 0U &&
        before.entity_slots == after.entity_slots && before.transform_capacity == after.transform_capacity &&
        before.velocity_capacity == after.velocity_capacity && before.sprite_capacity == after.sprite_capacity &&
        std::isfinite(checksum);
    return {
        entity_count,
        create_ms,
        ai2d::summarize(query_samples),
        ai2d::summarize(integrate_samples),
        ai2d::summarize(wrap_samples),
        allocations,
        metrics,
        before,
        after,
        checksum,
        invariant_pass,
    };
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

void write_result(ai2d::JsonWriter& writer, const WorldBenchmarkResult& result) {
    writer.begin_object();
    writer.key("benchmark_id");
    writer.value("world_dense_v1");
    writer.key("entity_count");
    writer.value(static_cast<std::uint64_t>(result.entity_count));
    writer.key("status");
    writer.value(result.invariant_pass ? "pass" : "fail");
    writer.key("create_ms");
    writer.value(result.create_ms);
    writer.key("query_transform_sprite_ms");
    write_distribution(writer, result.query_transform_sprite_ms);
    writer.key("integrate_velocity_ms");
    write_distribution(writer, result.integrate_velocity_ms);
    writer.key("wrap_bounds_ms");
    write_distribution(writer, result.wrap_bounds_ms);
    writer.key("world");
    writer.begin_object();
    writer.key("alive_entities");
    writer.value(static_cast<std::uint64_t>(result.world.alive_entities));
    writer.key("transform_count");
    writer.value(static_cast<std::uint64_t>(result.world.transform_count));
    writer.key("velocity_count");
    writer.value(static_cast<std::uint64_t>(result.world.velocity_count));
    writer.key("sprite_count");
    writer.value(static_cast<std::uint64_t>(result.world.sprite_count));
    writer.key("query_visited");
    writer.value(result.world.query_visited);
    writer.key("query_matched");
    writer.value(result.world.query_matched);
    writer.key("system_invocations");
    writer.value(result.world.system_invocations);
    writer.end_object();
    writer.key("memory");
    writer.begin_object();
    writer.key("tracked_cpp_heap_allocations");
    writer.value(result.allocations.allocations);
    writer.key("tracked_cpp_heap_bytes");
    writer.value(result.allocations.bytes);
    writer.key("capacity_growth_events");
    writer.value(result.world.capacity_growth_events);
    writer.key("frame_arena_overflows");
    writer.null_value();
    writer.key("frame_arena_overflows_unavailable_reason");
    writer.value("This benchmark does not use or instrument a FrameArena");
    writer.end_object();
    writer.key("checksum");
    writer.value(result.checksum);
    writer.end_object();
}

int benchmark_main(const int argument_count, const char* const* const arguments) {
    BenchmarkArguments benchmark_arguments{};
    if (!parse_arguments(argument_count, arguments, benchmark_arguments)) {
        return emit_invalid_arguments(
            "benchmark",
            json_requested(argument_count, arguments),
            "Invalid benchmark arguments");
    }
    if (benchmark_arguments.suite == "render") {
        return ai2d_render_benchmark_main(
            benchmark_arguments.counts,
            benchmark_arguments.runs,
            benchmark_arguments.warmup,
            benchmark_arguments.frames,
            benchmark_arguments.json);
    }
    if (benchmark_arguments.suite == "scenario") {
        return ai2d_scenario_benchmark_main(
            benchmark_arguments.counts,
            benchmark_arguments.runs,
            benchmark_arguments.warmup,
            benchmark_arguments.frames,
            benchmark_arguments.json);
    }

    std::vector<WorldBenchmarkResult> results{};
    results.reserve(benchmark_arguments.counts.size());
    std::vector<ai2d::Diagnostic> diagnostics{};
    diagnostics.reserve(benchmark_arguments.counts.size());
    bool all_pass = true;
    for (const auto count : benchmark_arguments.counts) {
        ai2d::Diagnostic error{};
        auto result = run_world_benchmark(
            count, benchmark_arguments.runs, benchmark_arguments.warmup, benchmark_arguments.frames, error);
        if (result.entity_count == 0U) {
            diagnostics.push_back(std::move(error));
            all_pass = false;
            break;
        }
        all_pass = all_pass && result.invariant_pass;
        if (!result.invariant_pass) {
            diagnostics.push_back(ai2d::Diagnostic::make(
                ai2d::DiagnosticCode::mem_frame_heap_allocation,
                ai2d::Severity::error,
                "benchmark",
                "A measured world benchmark invariant failed"));
        }
        results.push_back(result);
    }

    const auto build = ai2d::current_build_info();
    ai2d::JsonWriter writer{};
    writer.begin_object();
    writer.key("schema_version");
    writer.value(std::uint64_t{1U});
    writer.key("command");
    writer.value("benchmark");
    writer.key("status");
    writer.value(all_pass ? "pass" : "fail");
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
    writer.key("os");
    writer.value(build.operating_system);
    writer.key("architecture");
    writer.value(build.architecture);
    writer.key("gpu_timing_available");
    writer.value(false);
    writer.key("gpu_timing_unavailable_reason");
    writer.value("headless world suite");
    writer.end_object();
    writer.key("diagnostics");
    writer.begin_array();
    for (const auto& diagnostic_item : diagnostics) {
        ai2d::write_json(writer, diagnostic_item);
    }
    writer.end_array();
    writer.key("metrics");
    writer.begin_object();
    writer.key("suite");
    writer.value("world");
    writer.key("runs");
    writer.value(static_cast<std::uint64_t>(benchmark_arguments.runs));
    writer.key("warmup_frames");
    writer.value(static_cast<std::uint64_t>(benchmark_arguments.warmup));
    writer.key("measurement_frames");
    writer.value(static_cast<std::uint64_t>(benchmark_arguments.frames));
    writer.key("results");
    writer.begin_array();
    for (const auto& result : results) {
        write_result(writer, result);
    }
    writer.end_array();
    writer.end_object();
    writer.key("artifacts");
    writer.begin_array();
    writer.end_array();
    writer.end_object();

    if (!writer.complete()) {
        std::cerr << "benchmark JSON serialization failed\n";
        return 4;
    }
    if (benchmark_arguments.json) {
        std::cout << writer.str() << '\n';
    } else {
        std::cout << "world benchmark: " << (all_pass ? "PASS" : "FAIL") << " (" << results.size()
                  << " entity counts)\n";
    }
    return all_pass ? 0 : 1;
}

int inspect_capabilities_main(const int argument_count, const char* const* const arguments) {
    const bool requested_json = json_requested(argument_count, arguments);
    bool json = false;
    for (int index = 3; index < argument_count; ++index) {
        if (std::string_view{arguments[index]} == "--json") {
            json = true;
        } else {
            return emit_invalid_arguments("inspect", requested_json, "Unknown capabilities option");
        }
    }
#if defined(AI2D_ENABLE_GPU)
    ai2d::PlatformWindow window{};
    auto opened = window.open({"ai2d capability probe", 320U, 180U, true, false});
    if (!opened) {
        if (!json) {
            std::cerr << opened.error().message << '\n';
            return 3;
        }
        ai2d::JsonWriter failure{};
        failure.begin_object();
        failure.key("schema_version");
        failure.value(std::uint64_t{1U});
        failure.key("command");
        failure.value("inspect");
        failure.key("status");
        failure.value("unavailable");
        failure.key("build");
        failure.begin_object();
        failure.end_object();
        failure.key("environment");
        failure.begin_object();
        failure.end_object();
        failure.key("diagnostics");
        failure.begin_array();
        ai2d::write_json(failure, opened.error());
        failure.end_array();
        failure.key("metrics");
        failure.begin_object();
        failure.end_object();
        failure.key("artifacts");
        failure.begin_array();
        failure.end_array();
        failure.end_object();
        std::cout << failure.str() << '\n';
        return 3;
    }

    ai2d::Renderer2D renderer{};
    ai2d::RendererOptions options{};
    options.native_window = window.native_handle();
    options.width = 320U;
    options.height = 180U;
    options.require_present = true;
    auto initialized = renderer.initialize(options);
    if (!initialized) {
        if (!json) {
            std::cerr << initialized.error().message << '\n';
            return 3;
        }
        ai2d::JsonWriter failure{};
        failure.begin_object();
        failure.key("schema_version");
        failure.value(std::uint64_t{1U});
        failure.key("command");
        failure.value("inspect");
        failure.key("status");
        failure.value("unavailable");
        failure.key("build");
        failure.begin_object();
        failure.end_object();
        failure.key("environment");
        failure.begin_object();
        failure.end_object();
        failure.key("diagnostics");
        failure.begin_array();
        ai2d::write_json(failure, initialized.error());
        failure.end_array();
        failure.key("metrics");
        failure.begin_object();
        failure.end_object();
        failure.key("artifacts");
        failure.begin_array();
        failure.end_array();
        failure.end_object();
        std::cout << failure.str() << '\n';
        return 3;
    }

    const auto& capabilities = renderer.capabilities();
    if (!json) {
        std::cout << capabilities.device_name << ": Vulkan "
                  << (capabilities.api_version >> 22U) << '.'
                  << ((capabilities.api_version >> 12U) & 0x3FFU) << '.'
                  << (capabilities.api_version & 0xFFFU) << '\n';
        return 0;
    }
    const auto build = ai2d::current_build_info();
    ai2d::JsonWriter writer{};
    writer.begin_object();
    writer.key("schema_version");
    writer.value(std::uint64_t{1U});
    writer.key("command");
    writer.value("inspect");
    writer.key("status");
    writer.value(renderer.validation_diagnostics().empty() ? "pass" : "fail");
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
    writer.key("os");
    writer.value(build.operating_system);
    writer.key("architecture");
    writer.value(build.architecture);
    writer.end_object();
    writer.key("diagnostics");
    writer.begin_array();
    for (const auto& diagnostic : renderer.validation_diagnostics()) {
        ai2d::write_json(writer, diagnostic);
    }
    writer.end_array();
    writer.key("metrics");
    writer.begin_object();
    writer.key("kind");
    writer.value("capabilities");
    writer.key("device_name");
    writer.value(capabilities.device_name);
    writer.key("device_type");
    writer.value(capabilities.device_type);
    writer.key("driver_name");
    writer.value(capabilities.driver_name);
    writer.key("driver_info");
    writer.value(capabilities.driver_info);
    writer.key("vendor_id");
    writer.value(static_cast<std::uint64_t>(capabilities.vendor_id));
    writer.key("device_id");
    writer.value(static_cast<std::uint64_t>(capabilities.device_id));
    writer.key("api_version");
    writer.value(static_cast<std::uint64_t>(capabilities.api_version));
    writer.key("graphics_queue_family");
    writer.value(static_cast<std::uint64_t>(capabilities.graphics_queue_family));
    writer.key("dynamic_rendering");
    writer.value(capabilities.dynamic_rendering);
    writer.key("synchronization2");
    writer.value(capabilities.synchronization2);
    writer.key("shader_draw_parameters");
    writer.value(capabilities.shader_draw_parameters);
    writer.key("present_supported");
    writer.value(capabilities.present_supported);
    writer.key("timestamps_supported");
    writer.value(capabilities.timestamps_supported);
    writer.key("timestamp_period_nanoseconds");
    writer.value(static_cast<double>(capabilities.timestamp_period_nanoseconds));
    writer.key("validation_enabled");
    writer.value(capabilities.validation_enabled);
    writer.key("synchronization_validation_enabled");
    writer.value(capabilities.synchronization_validation_enabled);
    writer.end_object();
    writer.key("artifacts");
    writer.begin_array();
    writer.end_array();
    writer.end_object();
    std::cout << writer.str() << '\n';
    return renderer.validation_diagnostics().empty() ? 0 : 1;
#else
    return emit_cli_failure(
        "inspect",
        ai2d::Diagnostic::make(
            ai2d::DiagnosticCode::command_unavailable,
            ai2d::Severity::error,
            "renderer2d",
            "GPU support was disabled at configure time"),
        json,
        3);
#endif
}

} // namespace

int main(const int argument_count, const char* const* const arguments) {
    if (argument_count < 2) {
        return emit_invalid_arguments("", false, "usage: ai2d_cli <command>");
    }
    const std::string_view command{arguments[1]};
    if (command == "--help" || command == "-h" || command == "help") {
        std::cout
            << "usage: ai2d_cli <command> [options]\n"
            << "commands:\n"
            << "  game       validate, inspect, run, or verify GameManifest content\n"
            << "  validate   validate a ScenarioSpec\n"
            << "  inspect    inspect plans, schemas, or Vulkan capabilities\n"
            << "  run        run a ScenarioSpec\n"
            << "  profile    profile a ScenarioSpec\n"
            << "  benchmark  run bounded benchmark suites\n"
            << "use 'ai2d_cli game --help' for game options\n";
        return 0;
    }
    if (command == "benchmark") {
        return benchmark_main(argument_count, arguments);
    }
    if (command == "game") {
        return ai2d_game_command_main(argument_count, arguments);
    }
    if (command == "inspect" && argument_count >= 3 && std::string_view{arguments[2]} == "capabilities") {
        return inspect_capabilities_main(argument_count, arguments);
    }
    if (command == "validate" || command == "run" || command == "profile" || command == "inspect") {
        return ai2d_scenario_command_main(argument_count, arguments);
    }
    return emit_invalid_arguments(
        command,
        json_requested(argument_count, arguments),
        "Unknown command");
}
