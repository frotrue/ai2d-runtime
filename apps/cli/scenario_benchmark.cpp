#include "scenario_benchmark.hpp"

#include "ai2d/foundation/allocation_tracker.hpp"
#include "ai2d/foundation/build_info.hpp"
#include "ai2d/foundation/diagnostic.hpp"
#include "ai2d/foundation/json_writer.hpp"
#include "ai2d/foundation/statistics.hpp"
#include "ai2d/runtime/runtime.hpp"
#include "ai2d/scenario/scenario.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string_view>
#include <utility>
#include <vector>

namespace {

enum class Workload : std::uint8_t { static_sprites, moving_sprites, texture_switching, mostly_visible, mostly_invisible };

struct ScenarioBenchmarkResult final {
    Workload workload{Workload::static_sprites};
    std::size_t sprite_count{0U};
    std::uint64_t scenario_hash{0U};
    std::uint64_t plan_hash{0U};
    double load_ms{0.0};
    ai2d::DistributionSummary frame_cpu_ms{};
    ai2d::DistributionSummary fixed_update_ms{};
    ai2d::DistributionSummary extraction_ms{};
    ai2d::DistributionSummary queue_build_ms{};
    ai2d::DistributionSummary upload_ms{};
    ai2d::DistributionSummary cpu_submit_ms{};
    ai2d::DistributionSummary gpu_pass_ms{};
    ai2d::AllocationSnapshot allocations{};
    ai2d::RuntimeFrameMetrics last{};
    std::uint32_t expected_batches{0U};
    double checksum{0.0};
    bool invariant_pass{false};
};

std::string_view workload_name(const Workload workload) noexcept {
    switch (workload) {
    case Workload::static_sprites: return "integrated_offscreen_static_v1";
    case Workload::moving_sprites: return "integrated_offscreen_moving_v1";
    case Workload::texture_switching: return "integrated_offscreen_texture_switching_v1";
    case Workload::mostly_visible: return "integrated_offscreen_mostly_visible_v1";
    case Workload::mostly_invisible: return "integrated_offscreen_mostly_invisible_v1";
    }
    return "integrated_offscreen_static_v1";
}

std::uint64_t stable_workload_hash(const Workload workload, const std::size_t count, const std::uint64_t salt) noexcept {
    auto value = 14695981039346656037ULL;
    const auto mix = [&value](const std::uint64_t input) {
        auto remaining = input;
        for (std::size_t index = 0U; index < sizeof(input); ++index) {
            value ^= remaining & 0xFFU;
            value *= 1099511628211ULL;
            remaining >>= 8U;
        }
    };
    mix(static_cast<std::uint8_t>(workload));
    mix(static_cast<std::uint64_t>(count));
    mix(salt);
    return value;
}

ai2d::ExecutionPlan make_plan(const Workload workload, const std::size_t count) {
    ai2d::ExecutionPlan plan{};
    plan.symbols = {
        std::string{workload_name(workload)}, "texture_0", "texture_1", "texture_2", "texture_3",
        "sprites_0", "sprites_1", "sprites_2", "sprites_3", "integrate", "wrap",
    };
    plan.scenario_name = 0U;
    plan.seed = 0xA12D0001ULL;
    plan.world_capacity = static_cast<std::uint32_t>(count);
    plan.total_spawn_count = static_cast<std::uint32_t>(count);
    const auto columns = static_cast<std::uint32_t>(std::ceil(std::sqrt(static_cast<double>(count))));
    const auto rows = static_cast<std::uint32_t>((count + columns - 1U) / columns);
    const auto center_x = static_cast<float>(columns - 1U) * 0.5F;
    const auto center_y = static_cast<float>(rows - 1U) * 0.5F;
    plan.camera_half_extent = workload == Workload::mostly_invisible
                                  ? ai2d::Vec2{1.5F, 1.5F}
                                  : ai2d::Vec2{center_x + 1.0F, center_y + 1.0F};

    const auto texture_count = workload == Workload::texture_switching ? 4U : 1U;
    plan.textures.reserve(texture_count);
    constexpr std::array colors{
        ai2d::Color{0.95F, 0.2F, 0.15F, 1.0F},
        ai2d::Color{0.15F, 0.9F, 0.3F, 1.0F},
        ai2d::Color{0.15F, 0.35F, 1.0F, 1.0F},
        ai2d::Color{1.0F, 0.85F, 0.1F, 1.0F},
    };
    for (std::uint32_t index = 0U; index < texture_count; ++index) {
        plan.textures.push_back({1U + index, ai2d::TextureGeneratorId::checker, 8U, 8U, 2U, colors[index], {0.1F, 0.1F, 0.12F, 1.0F}});
    }

    plan.spawn_groups.reserve(texture_count);
    std::uint32_t remaining = static_cast<std::uint32_t>(count);
    for (std::uint32_t index = 0U; index < texture_count; ++index) {
        const auto groups_left = texture_count - index;
        const auto group_count = remaining / groups_left;
        remaining -= group_count;
        ai2d::SpawnGroupPlan group{};
        group.symbol = 5U + index;
        group.count = group_count;
        group.placement.kind = ai2d::PlacementId::grid;
        group.placement.origin = {-center_x + static_cast<float>(index) * 0.05F, -center_y};
        group.placement.spacing = {1.0F, 1.0F};
        group.placement.columns = columns;
        group.transform.scale = {0.8F, 0.8F};
        group.has_velocity = workload == Workload::moving_sprites;
        group.velocity = {{0.75F, 0.2F}, 0.05F};
        group.has_sprite = true;
        group.sprite.texture_asset = index;
        group.sprite.size = {0.8F, 0.8F};
        group.sprite.tint = {};
        group.sprite.layer = 0;
        plan.spawn_groups.push_back(group);
    }

    if (workload == Workload::moving_sprites) {
        ai2d::PlannedSystem integrate{};
        integrate.symbol = 9U;
        integrate.operation = ai2d::OperationId::integrate_velocity;
        integrate.phase = ai2d::PhaseId::fixed_update;
        integrate.parameters.delta_seconds = 1.0F / 60.0F;
        integrate.estimated_cardinality = static_cast<std::uint32_t>(count);
        ai2d::PlannedSystem wrap{};
        wrap.symbol = 10U;
        wrap.operation = ai2d::OperationId::wrap_bounds;
        wrap.phase = ai2d::PhaseId::fixed_update;
        wrap.parameters.bounds = {{-center_x - 1.0F, -center_y - 1.0F}, {center_x + 1.0F, center_y + 1.0F}};
        wrap.after_system_indices.push_back(0U);
        wrap.estimated_cardinality = static_cast<std::uint32_t>(count);
        plan.systems.push_back(std::move(integrate));
        plan.systems.push_back(std::move(wrap));
    }
    plan.scenario_hash = stable_workload_hash(workload, count, 0x5343454E4152494FULL);
    plan.plan_hash = ai2d::compute_execution_plan_hash(plan);
    return plan;
}

ai2d::Result<ScenarioBenchmarkResult> run_one(
    const Workload workload,
    const std::size_t count,
    const std::size_t runs,
    const std::size_t warmup_frames,
    const std::size_t measurement_frames,
    ai2d::RuntimeGpuCapabilities& gpu_capabilities) {
    const auto plan = make_plan(workload, count);
    ai2d::ScenarioRuntime runtime{};
    ai2d::RuntimeOptions options{};
    options.width = 640U;
    options.height = 360U;
    options.offscreen = true;
    options.hidden = true;
    auto loaded = runtime.initialize(plan, options);
    if (!loaded) {
        return std::unexpected(std::move(loaded.error()));
    }
    gpu_capabilities = runtime.gpu_capabilities();
    for (std::size_t frame = 0U; frame < warmup_frames; ++frame) {
        auto warmed = runtime.run_frame();
        if (!warmed) {
            return std::unexpected(std::move(warmed.error()));
        }
    }

    const auto sample_capacity = runs * measurement_frames;
    std::vector<double> frame_samples{};
    std::vector<double> update_samples{};
    std::vector<double> extraction_samples{};
    std::vector<double> queue_samples{};
    std::vector<double> upload_samples{};
    std::vector<double> submit_samples{};
    std::vector<double> gpu_samples{};
    for (auto* samples : {&frame_samples, &update_samples, &extraction_samples, &queue_samples, &upload_samples, &submit_samples, &gpu_samples}) {
        samples->reserve(sample_capacity);
    }
    ai2d::AllocationSnapshot allocations{};
    ai2d::RuntimeFrameMetrics last{};
    double checksum = 0.0;
    for (std::size_t run = 0U; run < runs; ++run) {
        bool run_ok = true;
        ai2d::Diagnostic run_error{};
        ai2d::MeasuredAllocationScope measured{};
        for (std::size_t frame = 0U; frame < measurement_frames; ++frame) {
            auto result = runtime.run_frame();
            if (!result) {
                run_ok = false;
                run_error = std::move(result.error());
                break;
            }
            last = *result;
            frame_samples.push_back(last.frame_cpu_ms);
            update_samples.push_back(last.fixed_update_ms + last.post_update_ms);
            extraction_samples.push_back(last.render_extraction_ms);
            queue_samples.push_back(last.render_queue_build_ms);
            upload_samples.push_back(last.render_upload_ms);
            submit_samples.push_back(last.render_submission_cpu_ms);
            if (last.gpu_timing_available) {
                gpu_samples.push_back(last.sprite_pass_gpu_ms);
            }
            checksum += last.checksum + static_cast<double>(last.render.visible_sprites) * 0.0000001;
        }
        const auto run_allocations = measured.finish();
        allocations.allocations += run_allocations.allocations;
        allocations.bytes += run_allocations.bytes;
        if (!run_ok) {
            return std::unexpected(std::move(run_error));
        }
    }
    if (auto idle = runtime.wait_idle(); !idle) {
        return std::unexpected(std::move(idle.error()));
    }

    const auto expected_batches = workload == Workload::texture_switching ? 4U : 1U;
    const auto visible_ok = workload == Workload::mostly_invisible
                                ? last.render.visible_sprites < count / 10U && last.render.culled_sprites > count * 9U / 10U
                                : last.render.visible_sprites == count && last.render.culled_sprites == 0U;
    const auto invariant_pass = allocations.allocations == 0U && last.world.capacity_growth_events == 0U &&
                                last.render.capacity_growth_events == 0U && visible_ok &&
                                last.render.batches == expected_batches &&
                                last.render.sprite_draw_calls == expected_batches &&
                                last.render.texture_binds == expected_batches && std::isfinite(checksum);
    return ScenarioBenchmarkResult{
        workload,
        count,
        plan.scenario_hash,
        plan.plan_hash,
        loaded->load_ms,
        ai2d::summarize(frame_samples),
        ai2d::summarize(update_samples),
        ai2d::summarize(extraction_samples),
        ai2d::summarize(queue_samples),
        ai2d::summarize(upload_samples),
        ai2d::summarize(submit_samples),
        ai2d::summarize(gpu_samples),
        allocations,
        last,
        expected_batches,
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

std::string_view bottleneck(const ScenarioBenchmarkResult& result) noexcept {
    constexpr std::array names{"fixed_update", "render_extraction", "render_queue_build", "render_upload", "cpu_submit", "gpu_pass"};
    const std::array values{
        result.fixed_update_ms.median,
        result.extraction_ms.median,
        result.queue_build_ms.median,
        result.upload_ms.median,
        result.cpu_submit_ms.median,
        result.gpu_pass_ms.median,
    };
    return names[static_cast<std::size_t>(std::distance(values.begin(), std::max_element(values.begin(), values.end())))];
}

void write_result(ai2d::JsonWriter& writer, const ScenarioBenchmarkResult& result) {
    writer.begin_object();
    writer.key("benchmark_id");
    writer.value(workload_name(result.workload));
    writer.key("sprite_count");
    writer.value(static_cast<std::uint64_t>(result.sprite_count));
    writer.key("scenario_hash");
    writer.value(result.scenario_hash);
    writer.key("plan_hash");
    writer.value(result.plan_hash);
    writer.key("status");
    writer.value(result.invariant_pass ? "pass" : "fail");
    writer.key("load_ms");
    writer.value(result.load_ms);
    writer.key("frame_cpu_ms");
    write_distribution(writer, result.frame_cpu_ms);
    writer.key("fixed_update_ms");
    write_distribution(writer, result.fixed_update_ms);
    writer.key("render_extraction_ms");
    write_distribution(writer, result.extraction_ms);
    writer.key("render_queue_build_ms");
    write_distribution(writer, result.queue_build_ms);
    writer.key("render_upload_ms");
    write_distribution(writer, result.upload_ms);
    writer.key("render_submission_cpu_ms");
    write_distribution(writer, result.cpu_submit_ms);
    writer.key("sprite_pass_gpu_ms");
    if (result.gpu_pass_ms.sample_count > 0U) {
        write_distribution(writer, result.gpu_pass_ms);
        writer.key("gpu_timing_unavailable_reason");
        writer.null_value();
    } else {
        writer.null_value();
        writer.key("gpu_timing_unavailable_reason");
        writer.value("Vulkan timestamp results were unavailable for all measured frames");
    }
    writer.key("bottleneck_phase");
    writer.value(bottleneck(result));
    writer.key("world");
    writer.begin_object();
    writer.key("alive_entities");
    writer.value(static_cast<std::uint64_t>(result.last.world.alive_entities));
    writer.key("transform_count");
    writer.value(static_cast<std::uint64_t>(result.last.world.transform_count));
    writer.key("velocity_count");
    writer.value(static_cast<std::uint64_t>(result.last.world.velocity_count));
    writer.key("sprite_count");
    writer.value(static_cast<std::uint64_t>(result.last.world.sprite_count));
    writer.key("query_visited");
    writer.value(result.last.world.query_visited);
    writer.key("query_matched");
    writer.value(result.last.world.query_matched);
    writer.key("system_invocations");
    writer.value(result.last.world.system_invocations);
    writer.end_object();
    writer.key("render");
    writer.begin_object();
    writer.key("extracted_sprites");
    writer.value(static_cast<std::uint64_t>(result.last.render.extracted_sprites));
    writer.key("visible_sprites");
    writer.value(static_cast<std::uint64_t>(result.last.render.visible_sprites));
    writer.key("culled_sprites");
    writer.value(static_cast<std::uint64_t>(result.last.render.culled_sprites));
    writer.key("batches");
    writer.value(static_cast<std::uint64_t>(result.last.render.batches));
    writer.key("expected_batches");
    writer.value(static_cast<std::uint64_t>(result.expected_batches));
    writer.key("sprite_draw_calls");
    writer.value(static_cast<std::uint64_t>(result.last.render.sprite_draw_calls));
    writer.key("texture_binds");
    writer.value(static_cast<std::uint64_t>(result.last.render.texture_binds));
    writer.key("instance_upload_bytes");
    writer.value(result.last.render.instance_upload_bytes);
    writer.key("render_queue_capacity");
    writer.value(static_cast<std::uint64_t>(result.last.render.render_queue_capacity));
    writer.key("unique_textures");
    writer.value(static_cast<std::uint64_t>(result.last.render.unique_textures));
    writer.key("active_layers");
    writer.value(static_cast<std::uint64_t>(result.last.render.active_layers));
    writer.end_object();
    writer.key("memory");
    writer.begin_object();
    writer.key("tracked_cpp_heap_allocations");
    writer.value(result.allocations.allocations);
    writer.key("tracked_cpp_heap_bytes");
    writer.value(result.allocations.bytes);
    writer.key("capacity_growth_events");
    writer.value(result.last.world.capacity_growth_events + result.last.render.capacity_growth_events);
    writer.key("frame_arena_overflows");
    writer.null_value();
    writer.key("frame_arena_overflows_unavailable_reason");
    writer.value("This benchmark does not use or instrument a FrameArena");
    writer.key("vma_allocation_count");
    writer.value(result.last.render.vma_allocation_count);
    writer.key("vma_allocation_bytes");
    writer.value(result.last.render.vma_allocation_bytes);
    writer.key("texture_bytes");
    writer.value(result.last.render.texture_bytes);
    writer.key("instance_buffer_capacity_bytes");
    writer.value(result.last.render.instance_buffer_capacity_bytes);
    writer.end_object();
    writer.key("checksum");
    writer.value(result.checksum);
    writer.end_object();
}

} // namespace

int ai2d_scenario_benchmark_main(
    const std::span<const std::size_t> counts,
    const std::size_t runs,
    const std::size_t warmup_frames,
    const std::size_t measurement_frames,
    const bool json_mode) {
    std::vector<ScenarioBenchmarkResult> results{};
    results.reserve(counts.size() * 5U);
    std::vector<ai2d::Diagnostic> diagnostics{};
    diagnostics.reserve(1U);
    ai2d::RuntimeGpuCapabilities gpu{};
    bool all_pass = true;
    constexpr std::array workloads{
        Workload::static_sprites,
        Workload::moving_sprites,
        Workload::texture_switching,
        Workload::mostly_visible,
        Workload::mostly_invisible,
    };
    for (const auto count : counts) {
        for (const auto workload : workloads) {
            auto result = run_one(workload, count, runs, warmup_frames, measurement_frames, gpu);
            if (!result) {
                diagnostics.push_back(std::move(result.error()));
                all_pass = false;
                break;
            }
            all_pass = all_pass && result->invariant_pass;
            results.push_back(std::move(*result));
        }
        if (!all_pass) {
            break;
        }
    }
    if (!json_mode) {
        std::cout << "integrated offscreen benchmark: " << (all_pass ? "PASS" : "FAIL") << " ("
                  << results.size() << " results)\n";
        return all_pass ? 0 : 1;
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
    writer.key("device_name");
    writer.value(gpu.device_name);
    writer.key("device_type");
    writer.value(gpu.device_type);
    writer.key("driver_name");
    writer.value(gpu.driver_name);
    writer.key("driver_info");
    writer.value(gpu.driver_info);
    writer.key("vendor_id");
    writer.value(static_cast<std::uint64_t>(gpu.vendor_id));
    writer.key("device_id");
    writer.value(static_cast<std::uint64_t>(gpu.device_id));
    writer.key("api_version");
    writer.value(static_cast<std::uint64_t>(gpu.api_version));
    writer.key("driver_version");
    writer.value(static_cast<std::uint64_t>(gpu.driver_version));
    writer.key("gpu_timing_available");
    writer.value(gpu.timestamps_supported);
    writer.end_object();
    writer.key("diagnostics");
    writer.begin_array();
    for (const auto& diagnostic : diagnostics) {
        ai2d::write_json(writer, diagnostic);
    }
    writer.end_array();
    writer.key("metrics");
    writer.begin_object();
    writer.key("suite");
    writer.value("scenario");
    writer.key("target");
    writer.value("offscreen");
    writer.key("resolution_width");
    writer.value(std::uint64_t{640U});
    writer.key("resolution_height");
    writer.value(std::uint64_t{360U});
    writer.key("runs");
    writer.value(static_cast<std::uint64_t>(runs));
    writer.key("warmup_frames");
    writer.value(static_cast<std::uint64_t>(warmup_frames));
    writer.key("measurement_frames");
    writer.value(static_cast<std::uint64_t>(measurement_frames));
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
    std::cout << writer.str() << '\n';
    return all_pass ? 0 : 1;
}
