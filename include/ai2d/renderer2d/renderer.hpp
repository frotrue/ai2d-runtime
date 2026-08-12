#pragma once

#include "ai2d/foundation/diagnostic.hpp"
#include "ai2d/foundation/types.hpp"
#include "ai2d/renderer2d/render_queue.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <span>
#include <vector>

namespace ai2d {

enum class PresentMode2D : std::uint8_t { fifo, mailbox, immediate };

[[nodiscard]] std::string_view present_mode_name(PresentMode2D mode) noexcept;

struct RendererOptions final {
    void* native_window{nullptr};
    std::uint32_t width{1280U};
    std::uint32_t height{720U};
    std::uint32_t max_sprites{100000U};
    std::uint32_t max_textures{16U};
    std::int32_t device_index{-1};
    bool enable_validation{false};
    bool enable_synchronization_validation{false};
    bool require_present{true};
    RenderFpsCap requested_fps_cap{RenderFpsCap::fps_60};
};

struct RendererCapabilities final {
    std::string device_name{};
    std::string device_type{};
    std::string driver_name{};
    std::string driver_info{};
    std::uint32_t vendor_id{0U};
    std::uint32_t device_id{0U};
    std::uint32_t api_version{0U};
    std::uint32_t driver_version{0U};
    std::uint32_t graphics_queue_family{0U};
    float timestamp_period_nanoseconds{0.0F};
    bool present_supported{false};
    bool timestamps_supported{false};
    bool validation_requested{false};
    bool validation_enabled{false};
    bool synchronization_validation_enabled{false};
    bool dynamic_rendering{false};
    bool synchronization2{false};
    bool shader_draw_parameters{false};
    RenderFpsCap requested_fps_cap{RenderFpsCap::fps_60};
    PresentMode2D requested_present_mode{PresentMode2D::fifo};
    PresentMode2D effective_present_mode{PresentMode2D::fifo};
};

struct GpuFrameMetrics final {
    std::uint64_t frame_index{0U};
    double cpu_submit_ms{0.0};
    double gpu_pass_ms{0.0};
    bool gpu_timing_available{false};
    bool presented{false};
    std::uint32_t width{0U};
    std::uint32_t height{0U};
};

struct RendererMetrics final {
    GpuFrameMetrics gpu{};
    RenderQueueMetrics queue{};
    std::uint64_t sprite_draw_calls{0U};
    std::uint64_t texture_binds{0U};
    std::uint64_t instance_upload_bytes{0U};
    std::uint64_t capacity_growth_events{0U};
    std::uint64_t vma_allocation_count{0U};
    std::uint64_t vma_allocation_bytes{0U};
    std::uint64_t texture_bytes{0U};
    std::uint64_t instance_buffer_capacity_bytes{0U};
    double queue_build_ms{0.0};
    double upload_ms{0.0};
};

class Renderer2D final {
  public:
    Renderer2D();
    ~Renderer2D();

    Renderer2D(const Renderer2D&) = delete;
    Renderer2D& operator=(const Renderer2D&) = delete;
    Renderer2D(Renderer2D&&) noexcept;
    Renderer2D& operator=(Renderer2D&&) noexcept;

    [[nodiscard]] Result<void> initialize(const RendererOptions& options);
    [[nodiscard]] Result<GpuFrameMetrics> clear(Color color);
    [[nodiscard]] Result<TextureHandle> create_texture_rgba(
        std::uint32_t width,
        std::uint32_t height,
        std::span<const std::uint8_t> rgba);
    [[nodiscard]] Result<TextureHandle> create_checker_texture(
        std::uint32_t width,
        std::uint32_t height,
        Color first,
        Color second,
        std::uint32_t cell_size = 8U);
    [[nodiscard]] Result<void> destroy_texture(TextureHandle texture);
    [[nodiscard]] Result<RendererMetrics> render(const RenderFrame2D& frame);
    [[nodiscard]] Result<Color> read_offscreen_pixel(std::uint32_t x, std::uint32_t y);
    [[nodiscard]] Result<void> resize(std::uint32_t width, std::uint32_t height);
    [[nodiscard]] Result<void> set_fps_cap(RenderFpsCap cap);
    [[nodiscard]] Result<void> wait_idle();
    [[nodiscard]] const RendererCapabilities& capabilities() const noexcept;
    [[nodiscard]] const std::vector<Diagnostic>& validation_diagnostics() const noexcept;
    [[nodiscard]] bool initialized() const noexcept;

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ai2d
