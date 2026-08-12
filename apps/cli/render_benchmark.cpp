#include "render_benchmark.hpp"

#include "ai2d/foundation/allocation_tracker.hpp"
#include "ai2d/foundation/build_info.hpp"
#include "ai2d/foundation/diagnostic.hpp"
#include "ai2d/foundation/json_writer.hpp"
#include "ai2d/foundation/statistics.hpp"
#include "ai2d/foundation/timer.hpp"
#include "ai2d/renderer2d/renderer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

struct RenderBenchmarkResult final {
    std::size_t sprite_count{0U};
    bool moving{false};
    ai2d::DistributionSummary update_ms{};
    ai2d::DistributionSummary queue_build_ms{};
    ai2d::DistributionSummary upload_ms{};
    ai2d::DistributionSummary cpu_submit_ms{};
    ai2d::DistributionSummary gpu_pass_ms{};
    ai2d::AllocationSnapshot allocations{};
    ai2d::RendererMetrics last{};
    double checksum{0.0};
    bool invariant_pass{false};
};

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

void populate_sprites(
    std::vector<ai2d::SpriteSubmission2D>& sprites,
    const std::size_t count,
    const ai2d::TextureHandle texture,
    ai2d::Camera2D& camera) {
    const auto columns = static_cast<std::size_t>(std::ceil(std::sqrt(static_cast<double>(count))));
    const auto rows = (count + columns - 1U) / columns;
    const float center_x = static_cast<float>(columns - 1U) * 0.5F;
    const float center_y = static_cast<float>(rows - 1U) * 0.5F;
    camera.position = {};
    camera.half_extent = {center_x + 1.0F, center_y + 1.0F};
    sprites.resize(count);
    for (std::size_t index = 0U; index < count; ++index) {
        auto& sprite = sprites[index];
        sprite.position = {
            static_cast<float>(index % columns) - center_x,
            static_cast<float>(index / columns) - center_y,
        };
        sprite.size = {0.8F, 0.8F};
        sprite.rotation = static_cast<float>(index % 13U) * 0.01F;
        sprite.texture = texture;
        sprite.tint = {0.85F, 0.9F, 1.0F, 1.0F};
        sprite.layer = 0;
    }
}

void update_moving_sprites(
    std::vector<ai2d::SpriteSubmission2D>& sprites,
    const ai2d::Camera2D& camera) noexcept {
    const float minimum = -camera.half_extent.x + 0.5F;
    const float maximum = camera.half_extent.x - 0.5F;
    const float span = maximum - minimum;
    for (std::size_t index = 0U; index < sprites.size(); ++index) {
        auto& sprite = sprites[index];
        sprite.position.x += 0.002F * static_cast<float>((index % 7U) + 1U);
        sprite.rotation += 0.0005F;
        if (sprite.position.x > maximum) {
            sprite.position.x -= span;
        }
    }
}

ai2d::Result<RenderBenchmarkResult> run_one(
    const std::size_t count,
    const bool moving,
    const std::size_t runs,
    const std::size_t warmup_frames,
    const std::size_t measurement_frames,
    ai2d::RendererCapabilities& capabilities) {
    ai2d::Renderer2D renderer{};
    ai2d::RendererOptions options{};
    options.width = 640U;
    options.height = 360U;
    options.max_sprites = static_cast<std::uint32_t>(count);
    options.max_textures = 1U;
    options.require_present = false;
    auto initialized = renderer.initialize(options);
    if (!initialized) {
        return std::unexpected(std::move(initialized.error()));
    }
    capabilities = renderer.capabilities();
    auto texture = renderer.create_checker_texture(
        8U, 8U, {0.15F, 0.35F, 0.95F, 1.0F}, {0.95F, 0.85F, 0.15F, 1.0F}, 2U);
    if (!texture) {
        return std::unexpected(std::move(texture.error()));
    }

    std::vector<ai2d::SpriteSubmission2D> sprites{};
    sprites.reserve(count);
    ai2d::Camera2D camera{};
    populate_sprites(sprites, count, texture.value(), camera);
    ai2d::RenderFrame2D frame{camera, sprites, {0.01F, 0.02F, 0.04F, 1.0F}};
    for (std::size_t index = 0U; index < warmup_frames; ++index) {
        if (moving) {
            update_moving_sprites(sprites, camera);
        }
        auto rendered = renderer.render(frame);
        if (!rendered) {
            return std::unexpected(std::move(rendered.error()));
        }
    }

    const std::size_t sample_capacity = runs * measurement_frames;
    std::vector<double> update_samples{};
    std::vector<double> queue_samples{};
    std::vector<double> upload_samples{};
    std::vector<double> submit_samples{};
    std::vector<double> gpu_samples{};
    update_samples.reserve(sample_capacity);
    queue_samples.reserve(sample_capacity);
    upload_samples.reserve(sample_capacity);
    submit_samples.reserve(sample_capacity);
    gpu_samples.reserve(sample_capacity);
    ai2d::AllocationSnapshot allocations{};
    ai2d::RendererMetrics last{};
    double checksum = 0.0;

    for (std::size_t run = 0U; run < runs; ++run) {
        ai2d::MeasuredAllocationScope measured{};
        for (std::size_t frame_index = 0U; frame_index < measurement_frames; ++frame_index) {
            ai2d::Stopwatch update_timer{};
            if (moving) {
                update_moving_sprites(sprites, camera);
            }
            update_samples.push_back(update_timer.elapsed_milliseconds());
            auto rendered = renderer.render(frame);
            if (!rendered) {
                return std::unexpected(std::move(rendered.error()));
            }
            last = rendered.value();
            queue_samples.push_back(last.queue_build_ms);
            upload_samples.push_back(last.upload_ms);
            submit_samples.push_back(last.gpu.cpu_submit_ms);
            if (last.gpu.gpu_timing_available) {
                gpu_samples.push_back(last.gpu.gpu_pass_ms);
            }
            checksum += static_cast<double>(last.queue.visible) * 0.000001 +
                        static_cast<double>(last.sprite_draw_calls) * 0.00001 +
                        static_cast<double>(sprites[frame_index % sprites.size()].position.x) * 0.0000001;
        }
        const auto run_allocations = measured.finish();
        allocations.allocations += run_allocations.allocations;
        allocations.bytes += run_allocations.bytes;
    }
    auto idle = renderer.wait_idle();
    if (!idle) {
        return std::unexpected(std::move(idle.error()));
    }

    const bool invariant_pass = allocations.allocations == 0U && last.capacity_growth_events == 0U &&
                                last.queue.visible == count && last.queue.culled == 0U &&
                                last.queue.batches == 1U && last.sprite_draw_calls == 1U &&
                                std::isfinite(checksum);
    return RenderBenchmarkResult{
        count,
        moving,
        ai2d::summarize(update_samples),
        ai2d::summarize(queue_samples),
        ai2d::summarize(upload_samples),
        ai2d::summarize(submit_samples),
        ai2d::summarize(gpu_samples),
        allocations,
        last,
        checksum,
        invariant_pass,
    };
}

void write_result(ai2d::JsonWriter& writer, const RenderBenchmarkResult& result) {
    writer.begin_object();
    writer.key("benchmark_id");
    writer.value(result.moving ? "renderer2d_offscreen_moving_v1" : "renderer2d_offscreen_static_v1");
    writer.key("sprite_count");
    writer.value(static_cast<std::uint64_t>(result.sprite_count));
    writer.key("status");
    writer.value(result.invariant_pass ? "pass" : "fail");
    writer.key("update_ms");
    write_distribution(writer, result.update_ms);
    writer.key("queue_build_ms");
    write_distribution(writer, result.queue_build_ms);
    writer.key("upload_ms");
    write_distribution(writer, result.upload_ms);
    writer.key("cpu_submit_ms");
    write_distribution(writer, result.cpu_submit_ms);
    writer.key("gpu_pass_ms");
    write_distribution(writer, result.gpu_pass_ms);
    writer.key("render");
    writer.begin_object();
    writer.key("visited_sprites");
    writer.value(result.last.queue.visited);
    writer.key("visible_sprites");
    writer.value(result.last.queue.visible);
    writer.key("culled_sprites");
    writer.value(result.last.queue.culled);
    writer.key("batches");
    writer.value(result.last.queue.batches);
    writer.key("sprite_draw_calls");
    writer.value(result.last.sprite_draw_calls);
    writer.key("texture_binds");
    writer.value(result.last.texture_binds);
    writer.key("instance_upload_bytes");
    writer.value(result.last.instance_upload_bytes);
    writer.end_object();
    writer.key("memory");
    writer.begin_object();
    writer.key("tracked_cpp_heap_allocations");
    writer.value(result.allocations.allocations);
    writer.key("tracked_cpp_heap_bytes");
    writer.value(result.allocations.bytes);
    writer.key("capacity_growth_events");
    writer.value(result.last.capacity_growth_events);
    writer.key("frame_arena_overflows");
    writer.null_value();
    writer.key("frame_arena_overflows_unavailable_reason");
    writer.value("This benchmark does not use or instrument a FrameArena");
    writer.key("vma_allocation_count");
    writer.value(result.last.vma_allocation_count);
    writer.key("vma_allocation_bytes");
    writer.value(result.last.vma_allocation_bytes);
    writer.key("texture_bytes");
    writer.value(result.last.texture_bytes);
    writer.key("instance_buffer_capacity_bytes");
    writer.value(result.last.instance_buffer_capacity_bytes);
    writer.end_object();
    writer.key("checksum");
    writer.value(result.checksum);
    writer.end_object();
}

} // namespace

int ai2d_render_benchmark_main(
    const std::span<const std::size_t> counts,
    const std::size_t runs,
    const std::size_t warmup_frames,
    const std::size_t measurement_frames,
    const bool json_mode) {
    std::vector<RenderBenchmarkResult> results{};
    results.reserve(counts.size() * 2U);
    std::vector<ai2d::Diagnostic> diagnostics{};
    diagnostics.reserve(1U);
    ai2d::RendererCapabilities capabilities{};
    bool all_pass = true;
    for (const auto count : counts) {
        for (const bool moving : {false, true}) {
            auto result = run_one(count, moving, runs, warmup_frames, measurement_frames, capabilities);
            if (!result) {
                diagnostics.push_back(std::move(result.error()));
                all_pass = false;
                break;
            }
            all_pass = all_pass && result->invariant_pass;
            results.push_back(std::move(result.value()));
        }
        if (!all_pass) {
            break;
        }
    }

    if (!json_mode) {
        std::cout << "renderer2d offscreen benchmark: " << (all_pass ? "PASS" : "FAIL") << " ("
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
    writer.key("gpu_timing_available");
    writer.value(capabilities.timestamps_supported);
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
    writer.value("render");
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
