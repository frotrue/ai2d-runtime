#include "ai2d/foundation/allocation_tracker.hpp"
#include "ai2d/world/components.hpp"
#include "ai2d/world/dense_storage.hpp"
#include "ai2d/world/entity.hpp"
#include "ai2d/world/operations.hpp"
#include "ai2d/world/world.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cmath>
#include <limits>
#include <optional>

TEST_CASE("EntityRegistry rejects capacity, detects stale generations, and reuses indices") {
    ai2d::EntityRegistry registry{};
    registry.reserve(2U);
    const auto first = registry.create();
    const auto second = registry.create();
    REQUIRE(first);
    REQUIRE(second);
    REQUIRE(registry.alive_count() == 2U);
    const auto exhausted = registry.create();
    REQUIRE_FALSE(exhausted);
    REQUIRE(exhausted.error().code == ai2d::DiagnosticCode::world_capacity_exceeded);

    REQUIRE(registry.destroy(*first));
    REQUIRE_FALSE(registry.is_alive(*first));
    const auto stale_destroy = registry.destroy(*first);
    REQUIRE_FALSE(stale_destroy);
    REQUIRE(stale_destroy.error().code == ai2d::DiagnosticCode::world_stale_entity);

    const auto reused = registry.create();
    REQUIRE(reused);
    REQUIRE(reused->index == first->index);
    REQUIRE(reused->generation != first->generation);
    REQUIRE(registry.is_alive(*reused));
}

TEST_CASE("DenseStorage preserves sparse mapping through swap-remove") {
    ai2d::DenseStorage<ai2d::Transform2D> storage{};
    REQUIRE(storage.reserve(4U, 4U));
    const ai2d::EntityId first{0U, 1U};
    const ai2d::EntityId middle{1U, 1U};
    const ai2d::EntityId last{2U, 1U};
    REQUIRE(storage.insert(first, {{1.0F, 0.0F}, 0.0F, {1.0F, 1.0F}}));
    REQUIRE(storage.insert(middle, {{2.0F, 0.0F}, 0.0F, {1.0F, 1.0F}}));
    REQUIRE(storage.insert(last, {{3.0F, 0.0F}, 0.0F, {1.0F, 1.0F}}));
    REQUIRE(storage.size() == 3U);
    REQUIRE_FALSE(storage.insert(first, {}));

    const auto shrinking = storage.reserve(2U, 2U);
    REQUIRE_FALSE(shrinking);
    CHECK(shrinking.error().code == ai2d::DiagnosticCode::input_invalid);

    REQUIRE(storage.remove(middle));
    REQUIRE(storage.size() == 2U);
    REQUIRE_FALSE(storage.contains(middle));
    REQUIRE(storage.contains(first));
    REQUIRE(storage.contains(last));
    REQUIRE(storage.get(last) != nullptr);
    REQUIRE(storage.get(last)->position.x == Catch::Approx(3.0F));
    REQUIRE(storage.runtime_capacity_growth_events() == 0U);
}

TEST_CASE("DenseStorage rejects a different generation in an occupied sparse slot") {
    ai2d::DenseStorage<ai2d::Transform2D> storage{};
    REQUIRE(storage.reserve(2U, 2U));
    const ai2d::EntityId original{0U, 1U};
    const ai2d::EntityId replacement{0U, 2U};
    REQUIRE(storage.insert(original, {{7.0F, 0.0F}, 0.0F, {1.0F, 1.0F}}));

    const auto conflict = storage.insert(replacement, {});
    REQUIRE_FALSE(conflict);
    CHECK(conflict.error().code == ai2d::DiagnosticCode::world_stale_entity);
    CHECK(storage.size() == 1U);
    REQUIRE(storage.get(original) != nullptr);
    CHECK(storage.get(original)->position.x == 7.0F);
    CHECK_FALSE(storage.contains(replacement));

    REQUIRE(storage.remove(original));
    REQUIRE(storage.insert(replacement, {}));
    CHECK_FALSE(storage.contains(original));
    CHECK(storage.contains(replacement));
    CHECK(storage.runtime_capacity_growth_events() == 0U);
}

TEST_CASE("World typed query iterates the smallest storage and exact membership") {
    ai2d::World world{};
    REQUIRE(world.reserve(4U));
    const auto first = world.create_entity(
        {{1.0F, 0.0F}, 0.0F, {1.0F, 1.0F}}, ai2d::Velocity2D{}, ai2d::Sprite2D{});
    const auto second = world.create_entity(
        {{2.0F, 0.0F}, 0.0F, {1.0F, 1.0F}}, ai2d::Velocity2D{}, std::nullopt);
    const auto third = world.create_entity(
        {{3.0F, 0.0F}, 0.0F, {1.0F, 1.0F}}, std::nullopt, ai2d::Sprite2D{});
    REQUIRE(first);
    REQUIRE(second);
    REQUIRE(third);

    auto query = world.query<ai2d::Transform2D, ai2d::Sprite2D>();
    REQUIRE(query.candidate_count() == 2U);
    std::size_t matched = 0U;
    float sum = 0.0F;
    for (auto item : query) {
        ++matched;
        sum += item.first.position.x;
    }
    REQUIRE(matched == 2U);
    REQUIRE(sum == Catch::Approx(4.0F));
}

TEST_CASE("World rejects structural mutation during a borrowed query") {
    ai2d::World world{};
    REQUIRE(world.reserve(2U));
    REQUIRE(world.create_entity());
    {
        auto query = world.query<ai2d::Transform2D>();
        const auto mutation = world.create_entity();
        REQUIRE_FALSE(mutation);
        REQUIRE(mutation.error().code == ai2d::DiagnosticCode::world_structural_mutation_during_query);
        REQUIRE(query.candidate_count() == 0U);
    }
    REQUIRE(world.create_entity());
    REQUIRE(world.metrics().structural_mutation_errors == 1U);
}

TEST_CASE("World rejects capacity changes after population or during a query") {
    ai2d::World world{};
    REQUIRE(world.reserve(2U));
    const auto first = world.create_entity(ai2d::Transform2D{}, std::nullopt, std::nullopt);
    const auto second = world.create_entity(ai2d::Transform2D{}, std::nullopt, std::nullopt);
    REQUIRE(first);
    REQUIRE(second);
    const auto before = world.capacity_snapshot();

    const auto shrinking = world.reserve(1U);
    REQUIRE_FALSE(shrinking);
    CHECK(shrinking.error().code == ai2d::DiagnosticCode::input_invalid);
    CHECK(world.capacity_snapshot().entity_slots == before.entity_slots);
    REQUIRE(world.destroy_entity(*first));
    CHECK(world.is_alive(*second));

    ai2d::World empty{};
    REQUIRE(empty.reserve(2U));
    {
        auto query = empty.query<ai2d::Transform2D>();
        const auto growing = empty.reserve(4U);
        REQUIRE_FALSE(growing);
        CHECK(growing.error().code == ai2d::DiagnosticCode::world_structural_mutation_during_query);
        CHECK(query.candidate_count() == 0U);
    }
    CHECK(empty.metrics().structural_mutation_errors == 1U);
}

TEST_CASE("World rejects re-reservation after every entity has been destroyed") {
    ai2d::World world{};
    REQUIRE(world.reserve(10U));
    std::array<ai2d::EntityId, 10U> entities{};
    for (auto& entity : entities) {
        const auto created = world.create_entity(ai2d::Transform2D{}, std::nullopt, std::nullopt);
        REQUIRE(created);
        entity = *created;
    }
    for (auto iterator = entities.rbegin(); iterator != entities.rend(); ++iterator) {
        REQUIRE(world.destroy_entity(*iterator));
    }

    const auto before = world.capacity_snapshot();
    const auto shrinking = world.reserve(1U);
    REQUIRE_FALSE(shrinking);
    CHECK(shrinking.error().code == ai2d::DiagnosticCode::input_invalid);
    CHECK(world.capacity_snapshot().entity_slots == before.entity_slots);

    const auto reused = world.create_entity(ai2d::Transform2D{}, std::nullopt, std::nullopt);
    REQUIRE(reused);
    CHECK(world.transform(*reused) != nullptr);
}

TEST_CASE("Reserved typed query and built-in operations allocate no measured C++ heap") {
#if defined(AI2D_ENABLE_ALLOCATION_TRACKING)
    ai2d::World world{};
    constexpr std::size_t entity_count = 256U;
    REQUIRE(world.reserve(entity_count));
    for (std::size_t index = 0U; index < entity_count; ++index) {
        const auto entity = world.create_entity(
            ai2d::Transform2D{{static_cast<float>(index), 0.0F}, 0.0F, {1.0F, 1.0F}},
            ai2d::Velocity2D{{1.0F, 0.0F}, 0.0F},
            ai2d::Sprite2D{});
        REQUIRE(entity);
    }
    const auto before = world.capacity_snapshot();
    std::size_t matched = 0U;
    ai2d::MeasuredAllocationScope scope{};
    {
        auto query = world.query<ai2d::Transform2D, ai2d::Velocity2D>();
        for (auto item : query) {
            item.first.position.x += item.second.linear.x;
            ++matched;
        }
    }
    REQUIRE(ai2d::integrate_velocity(world, 1.0F / 60.0F));
    REQUIRE(ai2d::wrap_bounds(world, {{0.0F, 0.0F}, {512.0F, 512.0F}}));
    const auto allocations = scope.finish();
    const auto after = world.capacity_snapshot();

    REQUIRE(matched == entity_count);
    REQUIRE(allocations.allocations == 0U);
    REQUIRE(allocations.bytes == 0U);
    REQUIRE(world.metrics().capacity_growth_events == 0U);
    REQUIRE(before.transform_capacity == after.transform_capacity);
    REQUIRE(before.velocity_capacity == after.velocity_capacity);
    REQUIRE(before.sprite_capacity == after.sprite_capacity);
#else
    SKIP("allocation hook disabled by configuration");
#endif
}

TEST_CASE("World operations reject non-finite state and wrap extreme finite bounds in constant time") {
    ai2d::World world{};
    REQUIRE(world.reserve(1U));
    const auto entity = world.create_entity(
        ai2d::Transform2D{{std::numeric_limits<float>::max(), 0.0F}, 0.0F, {1.0F, 1.0F}},
        ai2d::Velocity2D{{std::numeric_limits<float>::max(), 0.0F}, 0.0F},
        std::nullopt);
    REQUIRE(entity);

    const auto integrated = ai2d::integrate_velocity(world, std::numeric_limits<float>::max());
    REQUIRE_FALSE(integrated);
    CHECK(integrated.error().code == ai2d::DiagnosticCode::runtime_numeric_state_invalid);

    auto* transform = world.transform(*entity);
    REQUIRE(transform != nullptr);
    transform->position = {0.0F, 0.0F};
    const auto limit = std::numeric_limits<float>::max();
    REQUIRE(ai2d::wrap_bounds(world, {{-limit, -limit}, {limit, limit}}));
    CHECK(std::isfinite(transform->position.x));
    CHECK(std::isfinite(transform->position.y));

    transform->position.x = std::numeric_limits<float>::infinity();
    const auto non_finite = ai2d::wrap_bounds(world, {{-1.0F, -1.0F}, {1.0F, 1.0F}});
    REQUIRE_FALSE(non_finite);
    CHECK(non_finite.error().code == ai2d::DiagnosticCode::runtime_numeric_state_invalid);
}
