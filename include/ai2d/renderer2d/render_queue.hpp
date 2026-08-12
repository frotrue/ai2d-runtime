#pragma once

#include "ai2d/foundation/diagnostic.hpp"
#include "ai2d/foundation/types.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ai2d {

struct Camera2D final {
    Vec2 position{};
    Vec2 half_extent{16.0F, 9.0F};
};

struct UvRect final {
    float u{0.0F};
    float v{0.0F};
    float width{1.0F};
    float height{1.0F};
};

struct SpriteSubmission2D final {
    Vec2 position{};
    Vec2 size{1.0F, 1.0F};
    Vec2 pivot{0.5F, 0.5F};
    float rotation{0.0F};
    UvRect uv{};
    Color tint{};
    TextureHandle texture{};
    std::int32_t layer{0};
    bool visible{true};
};

struct RenderFrame2D final {
    Camera2D camera{};
    std::span<const SpriteSubmission2D> sprites{};
    Color clear_color{0.02F, 0.03F, 0.06F, 1.0F};
};

struct alignas(16) PreparedSprite2D final {
    float clip_position[2]{};
    float clip_size[2]{};
    float rotation{0.0F};
    float uv_rect[4]{};
    float tint[4]{};
    TextureHandle texture{};
    std::int32_t layer{0};
    std::uint32_t source_order{0U};
    float pivot[2]{0.5F, 0.5F};
    std::uint32_t reserved{0U};
};

struct SpriteBatch2D final {
    TextureHandle texture{};
    std::int32_t layer{0};
    std::uint32_t first_instance{0U};
    std::uint32_t instance_count{0U};
};

struct RenderQueueMetrics final {
    std::uint64_t visited{0U};
    std::uint64_t visible{0U};
    std::uint64_t culled{0U};
    std::uint64_t batches{0U};
    std::uint64_t capacity_growth_events{0U};
};

class RenderQueue2D final {
  public:
    void reserve(std::size_t max_sprites);
    [[nodiscard]] Result<RenderQueueMetrics> build(const RenderFrame2D& frame);
    [[nodiscard]] std::span<const PreparedSprite2D> instances() const noexcept;
    [[nodiscard]] std::span<const SpriteBatch2D> batches() const noexcept;
    [[nodiscard]] std::size_t sprite_capacity() const noexcept;
    [[nodiscard]] std::size_t batch_capacity() const noexcept;

  private:
    std::vector<PreparedSprite2D> instances_{};
    std::vector<SpriteBatch2D> batches_{};
};

} // namespace ai2d
