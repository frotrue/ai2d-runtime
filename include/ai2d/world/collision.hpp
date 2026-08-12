#pragma once

#include "ai2d/foundation/diagnostic.hpp"
#include "ai2d/foundation/types.hpp"
#include "ai2d/world/entity.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace ai2d {

class World;

enum class CollisionTarget2D : std::uint8_t { a, b };
enum class CollisionReactionKind2D : std::uint8_t {
    reflect,
    deactivate,
    add_int_state,
    reset_group,
    play_sound,
};

struct CollisionReaction2D final {
    CollisionReactionKind2D kind{CollisionReactionKind2D::reflect};
    CollisionTarget2D target{CollisionTarget2D::a};
    std::uint32_t state_slot{0U};
    std::int32_t value{0};
    std::uint32_t group{0U};
    std::uint32_t sound_asset{0U};
};

struct CollisionRule2D final {
    static constexpr std::size_t max_reactions = 8U;

    std::uint32_t group_a{0U};
    std::uint32_t group_b{0U};
    std::array<CollisionReaction2D, max_reactions> reactions{};
    std::uint32_t reaction_count{0U};
};

struct CollisionGridConfig2D final {
    Rect bounds{{-16.0F, -9.0F}, {16.0F, 9.0F}};
    Vec2 cell_size{1.0F, 1.0F};
    std::uint32_t max_colliders{10'000U};
    std::uint32_t max_grid_references{80'000U};
    std::uint32_t max_candidate_pairs{80'000U};
    std::uint32_t max_impacts_per_dynamic{4U};
};

struct CollisionEvent2D final {
    std::uint32_t rule_index{0U};
    EntityId entity_a{};
    EntityId entity_b{};
    Vec2 normal_for_a{};
    float time_of_impact{0.0F};
};

struct CollisionMetrics2D final {
    std::uint32_t active_colliders{0U};
    std::uint32_t active_state_changes{0U};
    std::uint32_t grid_references{0U};
    std::uint32_t candidate_pairs{0U};
    std::uint32_t narrowphase_tests{0U};
    std::uint32_t contacts{0U};
    std::uint32_t toi_iterations{0U};
    std::uint32_t iteration_limit_hits{0U};
    std::uint32_t grid_reference_capacity{0U};
    std::uint32_t candidate_capacity{0U};
};

class CollisionGrid2D final {
public:
    CollisionGrid2D();
    ~CollisionGrid2D();
    CollisionGrid2D(const CollisionGrid2D&) = delete;
    CollisionGrid2D& operator=(const CollisionGrid2D&) = delete;
    CollisionGrid2D(CollisionGrid2D&&) noexcept;
    CollisionGrid2D& operator=(CollisionGrid2D&&) noexcept;

    [[nodiscard]] Result<void> initialize(const CollisionGridConfig2D& config);
    [[nodiscard]] Result<CollisionMetrics2D> simulate(
        World& world,
        std::span<const CollisionRule2D> rules,
        float delta_seconds);
    [[nodiscard]] std::span<const CollisionEvent2D> events() const noexcept;
    [[nodiscard]] bool initialized() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ai2d
