#pragma once

#include "ai2d/foundation/types.hpp"

#include <cstdint>
#include <type_traits>

namespace ai2d {

struct Transform2D final {
    Vec2 position{};
    float rotation{0.0F};
    Vec2 scale{1.0F, 1.0F};
    Vec2 previous_position{};
};

struct Velocity2D final {
    Vec2 linear{};
    float angular{0.0F};
};

struct Sprite2D final {
    TextureHandle texture{};
    Vec2 size{1.0F, 1.0F};
    Vec2 pivot{0.5F, 0.5F};
    Color tint{};
    std::int32_t layer{0};
    bool visible{true};
    Rect uv{{0.0F, 0.0F}, {1.0F, 1.0F}};
};

enum class BodyMotion2D : std::uint8_t { static_body, kinematic_body, dynamic_body };

struct Collider2D final {
    Vec2 offset{};
    Vec2 half_extent{0.5F, 0.5F};
    std::uint32_t group{0U};
    BodyMotion2D motion{BodyMotion2D::static_body};
    bool trigger{false};
    bool enabled{true};
};

struct EntityState2D final {
    bool active{true};
};

static_assert(std::is_trivially_copyable_v<Transform2D>);
static_assert(std::is_trivially_copyable_v<Velocity2D>);
static_assert(std::is_trivially_copyable_v<Sprite2D>);
static_assert(std::is_trivially_copyable_v<Collider2D>);
static_assert(std::is_trivially_copyable_v<EntityState2D>);

} // namespace ai2d
