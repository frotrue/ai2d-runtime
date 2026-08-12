#pragma once

#include <cstdint>
#include <limits>

namespace ai2d {

struct Vec2 final {
    float x{0.0F};
    float y{0.0F};

    friend constexpr bool operator==(const Vec2&, const Vec2&) = default;
};

struct Rect final {
    Vec2 min{};
    Vec2 max{};

    [[nodiscard]] constexpr bool contains(const Vec2 point) const noexcept {
        return point.x >= min.x && point.x <= max.x && point.y >= min.y && point.y <= max.y;
    }
};

struct Color final {
    float r{1.0F};
    float g{1.0F};
    float b{1.0F};
    float a{1.0F};

    friend constexpr bool operator==(const Color&, const Color&) = default;
};

struct TextureHandle final {
    static constexpr std::uint32_t invalid_index = std::numeric_limits<std::uint32_t>::max();

    std::uint32_t index{invalid_index};
    std::uint32_t generation{0U};

    [[nodiscard]] constexpr bool valid() const noexcept { return index != invalid_index; }
    [[nodiscard]] static constexpr TextureHandle invalid() noexcept { return {}; }
    friend constexpr bool operator==(const TextureHandle&, const TextureHandle&) = default;
};

enum class RenderFpsCap : std::uint8_t { fps_60, fps_120, fps_144, fps_240, unlimited };

[[nodiscard]] constexpr std::uint32_t frames_per_second(const RenderFpsCap cap) noexcept {
    switch (cap) {
    case RenderFpsCap::fps_60: return 60U;
    case RenderFpsCap::fps_120: return 120U;
    case RenderFpsCap::fps_144: return 144U;
    case RenderFpsCap::fps_240: return 240U;
    case RenderFpsCap::unlimited: return 0U;
    }
    return 60U;
}

} // namespace ai2d
