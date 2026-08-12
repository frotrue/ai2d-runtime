#include "ai2d/runtime/runtime.hpp"

#include "ai2d/foundation/timer.hpp"
#include "ai2d/renderer2d/render_queue.hpp"
#include "ai2d/world/components.hpp"
#include "ai2d/world/operations.hpp"
#include "ai2d/world/world.hpp"

#if defined(AI2D_ENABLE_GPU)
#include "ai2d/platform/window.hpp"
#include "ai2d/renderer2d/renderer.hpp"
#endif

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ai2d {
namespace {

Diagnostic runtime_error(const DiagnosticCode code, std::string message) {
    return Diagnostic::make(code, Severity::error, "runtime", std::move(message));
}

class DeterministicRandom final {
  public:
    explicit DeterministicRandom(const std::uint64_t seed) noexcept : state_(seed) {}

    [[nodiscard]] float unit() noexcept {
        state_ += 0x9E3779B97F4A7C15ULL;
        auto value = state_;
        value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
        value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
        value ^= value >> 31U;
        return static_cast<float>((value >> 40U) & 0xFFFFFFULL) / 16777216.0F;
    }

  private:
    std::uint64_t state_{0U};
};

bool fits_finite_float(const double value) noexcept {
    constexpr auto limit = static_cast<double>(std::numeric_limits<float>::max());
    return std::isfinite(value) && value >= -limit && value <= limit;
}

Result<Vec2> spawn_position(
    const SpawnGroupPlan& group,
    const std::uint32_t item,
    DeterministicRandom& random) {
    double x = 0.0;
    double y = 0.0;
    if (group.placement.kind == PlacementId::grid) {
        const auto column = item % group.placement.columns;
        const auto row = item / group.placement.columns;
        x = static_cast<double>(group.placement.origin.x) +
            static_cast<double>(column) * group.placement.spacing.x +
            group.transform.position_offset.x;
        y = static_cast<double>(group.placement.origin.y) +
            static_cast<double>(row) * group.placement.spacing.y +
            group.transform.position_offset.y;
    } else {
        const auto& bounds = group.placement.random_bounds;
        x = static_cast<double>(bounds.min.x) + random.unit() *
                (static_cast<double>(bounds.max.x) - bounds.min.x) +
            group.transform.position_offset.x;
        y = static_cast<double>(bounds.min.y) + random.unit() *
                (static_cast<double>(bounds.max.y) - bounds.min.y) +
            group.transform.position_offset.y;
    }
    if (!fits_finite_float(x) || !fits_finite_float(y)) {
        return std::unexpected(runtime_error(
            DiagnosticCode::runtime_numeric_state_invalid,
            "Spawn placement produced a non-finite or out-of-range position"));
    }
    return Vec2{static_cast<float>(x), static_cast<float>(y)};
}

std::uint32_t checked_u32(const std::size_t value) noexcept {
    return value > std::numeric_limits<std::uint32_t>::max()
               ? std::numeric_limits<std::uint32_t>::max()
               : static_cast<std::uint32_t>(value);
}

} // namespace

class ScenarioRuntime::Impl final {
  public:
    ExecutionPlan plan{};
    RuntimeOptions options{};
    World world{};
    std::vector<TextureHandle> texture_handles{};
    std::vector<SpriteSubmission2D> extracted{};
#if defined(AI2D_ENABLE_GPU)
    PlatformWindow window{};
    Renderer2D renderer{};
#endif
    std::uint64_t frame_index{0U};
    std::uint32_t active_layers{0U};
    RuntimeGpuCapabilities gpu_capabilities{};
    bool ready{false};

    Result<RuntimeLoadMetrics> initialize(const ExecutionPlan& input_plan, const RuntimeOptions& input_options) {
        if (ready) {
            return std::unexpected(runtime_error(DiagnosticCode::input_invalid, "Runtime is already initialized"));
        }
        if (auto validated = validate_execution_plan(input_plan); !validated) {
            return std::unexpected(std::move(validated.error()));
        }
        Stopwatch timer{};
        plan = input_plan;
        options = input_options;
        if (auto reserved = world.reserve(plan.world_capacity); !reserved) {
            return std::unexpected(std::move(reserved.error()));
        }
        texture_handles.reserve(plan.textures.size());
        extracted.reserve(plan.world_capacity);

#if defined(AI2D_ENABLE_GPU)
        if (!options.headless) {
            void* native_window = nullptr;
            if (!options.offscreen) {
                WindowOptions window_options{};
                window_options.title = plan.symbol(plan.scenario_name).data();
                window_options.width = options.width;
                window_options.height = options.height;
                window_options.hidden = options.hidden;
                auto opened = window.open(window_options);
                if (!opened) {
                    return std::unexpected(std::move(opened.error()));
                }
                native_window = window.native_handle();
            }
            RendererOptions renderer_options{};
            renderer_options.native_window = native_window;
            renderer_options.width = options.width;
            renderer_options.height = options.height;
            renderer_options.max_sprites = plan.world_capacity;
            renderer_options.max_textures = std::max<std::uint32_t>(1U, checked_u32(plan.textures.size()));
            renderer_options.enable_validation = options.enable_validation;
            renderer_options.enable_synchronization_validation = options.enable_synchronization_validation;
            renderer_options.require_present = !options.offscreen;
            auto initialized = renderer.initialize(renderer_options);
            if (!initialized) {
                return std::unexpected(std::move(initialized.error()));
            }
            const auto& capabilities = renderer.capabilities();
            gpu_capabilities = {
                capabilities.device_name,
                capabilities.device_type,
                capabilities.driver_name,
                capabilities.driver_info,
                capabilities.vendor_id,
                capabilities.device_id,
                capabilities.api_version,
                capabilities.driver_version,
                true,
                capabilities.timestamps_supported,
                capabilities.validation_enabled,
                capabilities.synchronization_validation_enabled,
            };
            for (const auto& texture : plan.textures) {
                auto created = renderer.create_checker_texture(
                    texture.width,
                    texture.height,
                    texture.first,
                    texture.second,
                    texture.generator == TextureGeneratorId::checker ? texture.cell_size : texture.width);
                if (!created) {
                    return std::unexpected(std::move(created.error()));
                }
                texture_handles.push_back(*created);
            }
        }
#else
        if (!options.headless) {
            return std::unexpected(runtime_error(
                DiagnosticCode::command_unavailable,
                "This build has no GPU runtime; use the headless option"));
        }
#endif
        if (options.headless) {
            for (std::size_t index = 0U; index < plan.textures.size(); ++index) {
                texture_handles.push_back({static_cast<std::uint32_t>(index), 1U});
            }
        }

        std::unordered_set<std::int32_t> layers{};
        layers.reserve(plan.spawn_groups.size());
        for (std::size_t group_index = 0U; group_index < plan.spawn_groups.size(); ++group_index) {
            const auto& group = plan.spawn_groups[group_index];
            DeterministicRandom random{plan.seed ^ (static_cast<std::uint64_t>(group_index) << 32U)};
            for (std::uint32_t item = 0U; item < group.count; ++item) {
                Transform2D transform{};
                auto position = spawn_position(group, item, random);
                if (!position) {
                    return std::unexpected(std::move(position.error()));
                }
                transform.position = *position;
                transform.rotation = group.transform.rotation;
                transform.scale = group.transform.scale;
                std::optional<Velocity2D> velocity{};
                if (group.has_velocity) {
                    velocity = Velocity2D{group.velocity.linear, group.velocity.angular};
                }
                std::optional<Sprite2D> sprite{};
                if (group.has_sprite) {
                    if (group.sprite.texture_asset >= texture_handles.size()) {
                        return std::unexpected(runtime_error(
                            DiagnosticCode::ir_missing_texture,
                            "ExecutionPlan contains an invalid resolved texture index"));
                    }
                    sprite = Sprite2D{
                        texture_handles[group.sprite.texture_asset],
                        group.sprite.size,
                        group.sprite.pivot,
                        group.sprite.tint,
                        group.sprite.layer,
                        group.sprite.visible,
                    };
                    layers.insert(group.sprite.layer);
                }
                auto entity = world.create_entity(transform, velocity, sprite);
                if (!entity) {
                    return std::unexpected(std::move(entity.error()));
                }
            }
        }
        active_layers = checked_u32(layers.size());
        ready = true;
        return RuntimeLoadMetrics{
            timer.elapsed_milliseconds(),
            plan.total_spawn_count,
            checked_u32(texture_handles.size()),
            plan.world_capacity,
            plan.world_capacity,
        };
    }

    Result<RuntimeFrameMetrics> run_frame() {
        if (!ready) {
            return std::unexpected(runtime_error(
                DiagnosticCode::input_invalid, "Runtime frame requested before initialization"));
        }
        RuntimeFrameMetrics frame{};
        frame.frame_index = frame_index;
        Stopwatch frame_timer{};

#if defined(AI2D_ENABLE_GPU)
        if (!options.headless && !options.offscreen) {
            Stopwatch input_timer{};
            auto input = window.poll_input();
            if (!input) {
                return std::unexpected(std::move(input.error()));
            }
            frame.poll_input_ms = input_timer.elapsed_milliseconds();
            frame.quit_requested = input->quit_requested || input->down(InputKey::escape);
            if (input->resized && input->drawable_extent.width > 0U && input->drawable_extent.height > 0U) {
                auto resized = renderer.resize(input->drawable_extent.width, input->drawable_extent.height);
                if (!resized) {
                    return std::unexpected(std::move(resized.error()));
                }
            }
        }
#endif

        const auto metrics_before = world.metrics();
        for (const auto& system : plan.systems) {
            Stopwatch system_timer{};
            const auto matched_before = world.metrics().query_matched;
            switch (system.operation) {
            case OperationId::integrate_velocity: {
                auto integrated = integrate_velocity(world, system.parameters.delta_seconds);
                if (!integrated) {
                    return std::unexpected(std::move(integrated.error()));
                }
                break;
            }
            case OperationId::wrap_bounds: {
                auto wrapped = wrap_bounds(world, system.parameters.bounds);
                if (!wrapped) {
                    return std::unexpected(std::move(wrapped.error()));
                }
                break;
            }
            }
            const auto elapsed = system_timer.elapsed_milliseconds();
            const auto matched_after = world.metrics().query_matched;
            if (frame.system_count >= frame.systems.size()) {
                return std::unexpected(runtime_error(
                    DiagnosticCode::internal_error,
                    "Runtime system metrics capacity was exceeded"));
            }
            auto& system_metrics = frame.systems[frame.system_count++];
            system_metrics = {
                system.operation,
                elapsed,
                system.estimated_cardinality,
                checked_u32(static_cast<std::size_t>(matched_after - matched_before)),
            };
            if (system.phase == PhaseId::fixed_update) {
                frame.fixed_update_ms += elapsed;
            } else {
                frame.post_update_ms += elapsed;
            }
        }

        Stopwatch extraction_timer{};
        extracted.clear();
        auto query = world.query<Transform2D, Sprite2D>();
        for (auto item : query) {
            const auto effective_width = static_cast<double>(item.second.size.x) * item.first.scale.x;
            const auto effective_height = static_cast<double>(item.second.size.y) * item.first.scale.y;
            if (!fits_finite_float(effective_width) || !fits_finite_float(effective_height)) {
                return std::unexpected(runtime_error(
                    DiagnosticCode::runtime_numeric_state_invalid,
                    "Sprite scale produced non-finite or out-of-range geometry"));
            }
            const Vec2 effective_size{
                static_cast<float>(effective_width),
                static_cast<float>(effective_height),
            };
            extracted.push_back({
                item.first.position,
                effective_size,
                item.second.pivot,
                item.first.rotation,
                {item.second.uv.min.x, item.second.uv.min.y, item.second.uv.max.x, item.second.uv.max.y},
                item.second.tint,
                item.second.texture,
                item.second.layer,
                item.second.visible,
            });
            frame.checksum += static_cast<double>(item.first.position.x) * 0.000001 +
                              static_cast<double>(item.first.position.y) * 0.000003 +
                              static_cast<double>(item.first.rotation) * 0.000007 +
                              static_cast<double>(effective_size.x) * 0.00000011 +
                              static_cast<double>(effective_size.y) * 0.00000013 +
                              static_cast<double>(item.second.pivot.x) * 0.00000017 +
                              static_cast<double>(item.second.pivot.y) * 0.00000019;
        }
        world.record_query(query.candidate_count(), extracted.size());
        frame.render_extraction_ms = extraction_timer.elapsed_milliseconds();
        frame.render.extracted_sprites = checked_u32(extracted.size());
        frame.render.render_queue_capacity = checked_u32(extracted.capacity());
        frame.render.unique_textures = checked_u32(texture_handles.size());
        frame.render.active_layers = active_layers;
#if defined(AI2D_ENABLE_GPU)
        if (!options.headless) {
            RenderFrame2D render_frame{};
            render_frame.camera = {plan.camera_position, plan.camera_half_extent};
            render_frame.sprites = extracted;
            auto rendered = renderer.render(render_frame);
            if (!rendered) {
                return std::unexpected(std::move(rendered.error()));
            }
            frame.render_queue_build_ms = rendered->queue_build_ms;
            frame.render_upload_ms = rendered->upload_ms;
            frame.render_submission_cpu_ms = rendered->gpu.cpu_submit_ms;
            frame.sprite_pass_gpu_ms = rendered->gpu.gpu_pass_ms;
            frame.gpu_timing_available = rendered->gpu.gpu_timing_available;
            frame.presented = rendered->gpu.presented;
            frame.render.visible_sprites = checked_u32(rendered->queue.visible);
            frame.render.culled_sprites = checked_u32(rendered->queue.culled);
            frame.render.batches = checked_u32(rendered->queue.batches);
            frame.render.sprite_draw_calls = checked_u32(rendered->sprite_draw_calls);
            frame.render.texture_binds = checked_u32(rendered->texture_binds);
            frame.render.instance_upload_bytes = rendered->instance_upload_bytes;
            frame.render.capacity_growth_events = rendered->capacity_growth_events;
            frame.render.vma_allocation_count = rendered->vma_allocation_count;
            frame.render.vma_allocation_bytes = rendered->vma_allocation_bytes;
            frame.render.texture_bytes = rendered->texture_bytes;
            frame.render.instance_buffer_capacity_bytes = rendered->instance_buffer_capacity_bytes;
        }
#endif

        const auto metrics_after = world.metrics();
        frame.world = {
            checked_u32(metrics_after.alive_entities),
            checked_u32(metrics_after.transform_count),
            checked_u32(metrics_after.velocity_count),
            checked_u32(metrics_after.sprite_count),
            metrics_after.query_visited - metrics_before.query_visited,
            metrics_after.query_matched - metrics_before.query_matched,
            metrics_after.system_invocations - metrics_before.system_invocations,
            metrics_after.capacity_growth_events,
        };
        frame.frame_cpu_ms = frame_timer.elapsed_milliseconds();
        ++frame_index;
        return frame;
    }

    Result<void> wait_idle() {
#if defined(AI2D_ENABLE_GPU)
        if (ready && !options.headless) {
            return renderer.wait_idle();
        }
#endif
        return {};
    }
};

ScenarioRuntime::ScenarioRuntime() : impl_(std::make_unique<Impl>()) {}
ScenarioRuntime::~ScenarioRuntime() = default;
ScenarioRuntime::ScenarioRuntime(ScenarioRuntime&&) noexcept = default;
ScenarioRuntime& ScenarioRuntime::operator=(ScenarioRuntime&&) noexcept = default;

Result<RuntimeLoadMetrics> ScenarioRuntime::initialize(const ExecutionPlan& plan, const RuntimeOptions& options) {
    const bool was_initialized = impl_->ready;
    auto result = impl_->initialize(plan, options);
    if (!result && !was_initialized) {
        impl_ = std::make_unique<Impl>();
    }
    return result;
}

Result<RuntimeFrameMetrics> ScenarioRuntime::run_frame() { return impl_->run_frame(); }
Result<void> ScenarioRuntime::wait_idle() { return impl_->wait_idle(); }
bool ScenarioRuntime::initialized() const noexcept { return impl_->ready; }
const RuntimeGpuCapabilities& ScenarioRuntime::gpu_capabilities() const noexcept { return impl_->gpu_capabilities; }

} // namespace ai2d
