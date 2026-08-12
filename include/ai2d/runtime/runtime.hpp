#pragma once

#include "ai2d/foundation/diagnostic.hpp"
#include "ai2d/scenario/scenario.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <string>

namespace ai2d {

struct RuntimeOptions final {
    std::uint32_t width{1280U};
    std::uint32_t height{720U};
    bool headless{false};
    bool offscreen{false};
    bool hidden{false};
    bool enable_validation{false};
    bool enable_synchronization_validation{false};
};

struct RuntimeSystemFrameMetrics final {
    OperationId operation{OperationId::integrate_velocity};
    double cpu_ms{0.0};
    std::uint32_t estimated_cardinality{0U};
    std::uint32_t actual_cardinality{0U};
};

struct RuntimeWorldFrameMetrics final {
    std::uint32_t alive_entities{0U};
    std::uint32_t transform_count{0U};
    std::uint32_t velocity_count{0U};
    std::uint32_t sprite_count{0U};
    std::uint64_t query_visited{0U};
    std::uint64_t query_matched{0U};
    std::uint64_t system_invocations{0U};
    std::uint64_t capacity_growth_events{0U};
};

struct RuntimeRenderFrameMetrics final {
    std::uint32_t extracted_sprites{0U};
    std::uint32_t visible_sprites{0U};
    std::uint32_t culled_sprites{0U};
    std::uint32_t batches{0U};
    std::uint32_t sprite_draw_calls{0U};
    std::uint32_t texture_binds{0U};
    std::uint64_t instance_upload_bytes{0U};
    std::uint32_t render_queue_capacity{0U};
    std::uint32_t unique_textures{0U};
    std::uint32_t active_layers{0U};
    std::uint64_t capacity_growth_events{0U};
    std::uint64_t vma_allocation_count{0U};
    std::uint64_t vma_allocation_bytes{0U};
    std::uint64_t texture_bytes{0U};
    std::uint64_t instance_buffer_capacity_bytes{0U};
};

struct RuntimeFrameMetrics final {
    static constexpr std::size_t max_systems = 64U;

    std::uint64_t frame_index{0U};
    double frame_cpu_ms{0.0};
    double poll_input_ms{0.0};
    double fixed_update_ms{0.0};
    double post_update_ms{0.0};
    double render_extraction_ms{0.0};
    double render_queue_build_ms{0.0};
    double render_upload_ms{0.0};
    double render_submission_cpu_ms{0.0};
    double sprite_pass_gpu_ms{0.0};
    bool gpu_timing_available{false};
    bool presented{false};
    bool quit_requested{false};
    std::array<RuntimeSystemFrameMetrics, max_systems> systems{};
    std::uint32_t system_count{0U};
    RuntimeWorldFrameMetrics world{};
    RuntimeRenderFrameMetrics render{};
    double checksum{0.0};
};

struct RuntimeLoadMetrics final {
    double load_ms{0.0};
    std::uint32_t spawned_entities{0U};
    std::uint32_t created_textures{0U};
    std::uint32_t reserved_entities{0U};
    std::uint32_t reserved_sprites{0U};
};

struct RuntimeGpuCapabilities final {
    std::string device_name{};
    std::string device_type{};
    std::string driver_name{};
    std::string driver_info{};
    std::uint32_t vendor_id{0U};
    std::uint32_t device_id{0U};
    std::uint32_t api_version{0U};
    std::uint32_t driver_version{0U};
    bool available{false};
    bool timestamps_supported{false};
    bool validation_enabled{false};
    bool synchronization_validation_enabled{false};
};

class ScenarioRuntime final {
  public:
    ScenarioRuntime();
    ~ScenarioRuntime();

    ScenarioRuntime(const ScenarioRuntime&) = delete;
    ScenarioRuntime& operator=(const ScenarioRuntime&) = delete;
    ScenarioRuntime(ScenarioRuntime&&) noexcept;
    ScenarioRuntime& operator=(ScenarioRuntime&&) noexcept;

    [[nodiscard]] Result<RuntimeLoadMetrics> initialize(const ExecutionPlan& plan, const RuntimeOptions& options);
    [[nodiscard]] Result<RuntimeFrameMetrics> run_frame();
    [[nodiscard]] Result<void> wait_idle();
    [[nodiscard]] bool initialized() const noexcept;
    [[nodiscard]] const RuntimeGpuCapabilities& gpu_capabilities() const noexcept;

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ai2d
