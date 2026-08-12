#include "ai2d/foundation/allocation_tracker.hpp"
#include "ai2d/world/collision.hpp"
#include "ai2d/world/components.hpp"
#include "ai2d/world/world.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <limits>

TEST_CASE("Swept AABB collision prevents tunneling and reflects on the contact normal") {
    ai2d::World world{};
    REQUIRE(world.reserve(2U));
    const auto moving = world.create_entity(
        ai2d::Transform2D{{-5.0F, 0.0F}},
        ai2d::Velocity2D{{20.0F, 0.0F}},
        std::nullopt);
    REQUIRE(moving);
    REQUIRE(world.add(*moving, ai2d::Collider2D{{}, {0.5F, 0.5F}, 1U, ai2d::BodyMotion2D::dynamic_body}));
    REQUIRE(world.add(*moving, ai2d::EntityState2D{}));

    const auto wall = world.create_entity(
        ai2d::Transform2D{{0.0F, 0.0F}},
        std::nullopt,
        std::nullopt);
    REQUIRE(wall);
    REQUIRE(world.add(*wall, ai2d::Collider2D{{}, {0.5F, 2.0F}, 2U, ai2d::BodyMotion2D::static_body}));
    REQUIRE(world.add(*wall, ai2d::EntityState2D{}));

    ai2d::CollisionGrid2D collision{};
    REQUIRE(collision.initialize({{{-10.0F, -5.0F}, {10.0F, 5.0F}}, {1.0F, 1.0F}, 2U, 128U, 32U, 4U}));
    ai2d::CollisionRule2D rule{};
    rule.group_a = 1U;
    rule.group_b = 2U;
    rule.reaction_count = 1U;
    rule.reactions[0] = {
        ai2d::CollisionReactionKind2D::reflect,
        ai2d::CollisionTarget2D::a,
    };
    const auto metrics = collision.simulate(world, std::span{&rule, 1U}, 0.5F);
    REQUIRE(metrics);
    CHECK(metrics->contacts == 1U);
    REQUIRE(collision.events().size() == 1U);
    CHECK(world.velocity(*moving)->linear.x == Catch::Approx(-20.0F));
    CHECK(world.transform(*moving)->position.x < -1.0F);
}

TEST_CASE("Collision API rejects malformed public reaction bounds before world mutation") {
    ai2d::World world{};
    REQUIRE(world.reserve(1U));
    ai2d::CollisionGrid2D collision{};
    REQUIRE(collision.initialize({{{-2.0F, -2.0F}, {2.0F, 2.0F}}, {1.0F, 1.0F}, 1U, 16U, 4U, 1U}));

    ai2d::CollisionRule2D valid{};
    valid.group_a = 1U;
    valid.group_b = 2U;
    valid.reaction_count = static_cast<std::uint32_t>(ai2d::CollisionRule2D::max_reactions);
    REQUIRE(collision.simulate(world, std::span{&valid, 1U}, 1.0F / 60.0F));

    auto excessive = valid;
    excessive.reaction_count = static_cast<std::uint32_t>(ai2d::CollisionRule2D::max_reactions + 1U);
    const auto excessive_result = collision.simulate(world, std::span{&excessive, 1U}, 1.0F / 60.0F);
    REQUIRE_FALSE(excessive_result);
    CHECK(excessive_result.error().code == ai2d::DiagnosticCode::input_invalid);

    auto invalid_enum = valid;
    invalid_enum.reaction_count = 1U;
    invalid_enum.reactions[0].target = static_cast<ai2d::CollisionTarget2D>(255U);
    const auto enum_result = collision.simulate(world, std::span{&invalid_enum, 1U}, 1.0F / 60.0F);
    REQUIRE_FALSE(enum_result);
    CHECK(enum_result.error().code == ai2d::DiagnosticCode::input_invalid);
    CHECK(collision.events().empty());
}

TEST_CASE("Collision grid rejects unbounded capacities and extreme finite motion safely") {
    ai2d::CollisionGrid2D excessive{};
    auto config = ai2d::CollisionGridConfig2D{};
    config.max_grid_references = std::numeric_limits<std::uint32_t>::max();
    REQUIRE_FALSE(excessive.initialize(config));

    ai2d::World world{};
    REQUIRE(world.reserve(1U));
    const auto moving = world.create_entity(
        ai2d::Transform2D{{std::numeric_limits<float>::max(), 0.0F}},
        ai2d::Velocity2D{{std::numeric_limits<float>::max(), 0.0F}},
        std::nullopt);
    REQUIRE(moving);
    REQUIRE(world.add(*moving, ai2d::Collider2D{{}, {0.5F, 0.5F}, 1U, ai2d::BodyMotion2D::dynamic_body}));
    REQUIRE(world.add(*moving, ai2d::EntityState2D{}));
    ai2d::CollisionGrid2D collision{};
    REQUIRE(collision.initialize({{{-2.0F, -2.0F}, {2.0F, 2.0F}}, {1.0F, 1.0F}, 1U, 16U, 4U, 1U}));
    const auto result = collision.simulate(world, {}, 1.0F);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == ai2d::DiagnosticCode::runtime_numeric_state_invalid);
}

TEST_CASE("Reflected sweep finds a second wall outside the original forward path") {
    ai2d::World world{};
    REQUIRE(world.reserve(3U));
    const auto moving = world.create_entity(
        ai2d::Transform2D{{0.0F, 0.0F}}, ai2d::Velocity2D{{20.0F, 0.0F}}, std::nullopt);
    REQUIRE(moving);
    REQUIRE(world.add(*moving, ai2d::Collider2D{{}, {0.1F, 0.1F}, 1U, ai2d::BodyMotion2D::dynamic_body}));
    REQUIRE(world.add(*moving, ai2d::EntityState2D{}));
    for (const float x : {-2.0F, 2.0F}) {
        const auto wall = world.create_entity(ai2d::Transform2D{{x, 0.0F}}, std::nullopt, std::nullopt);
        REQUIRE(wall);
        REQUIRE(world.add(*wall, ai2d::Collider2D{{}, {0.5F, 2.0F}, 2U, ai2d::BodyMotion2D::static_body}));
        REQUIRE(world.add(*wall, ai2d::EntityState2D{}));
    }
    ai2d::CollisionRule2D rule{};
    rule.group_a = 1U;
    rule.group_b = 2U;
    rule.reaction_count = 1U;
    rule.reactions[0] = {ai2d::CollisionReactionKind2D::reflect, ai2d::CollisionTarget2D::a};
    ai2d::CollisionGrid2D collision{};
    REQUIRE(collision.initialize({{{-8.0F, -3.0F}, {8.0F, 3.0F}}, {1.0F, 1.0F}, 3U, 128U, 8U, 4U}));

    const auto result = collision.simulate(world, std::span{&rule, 1U}, 0.25F);
    REQUIRE(result);
    CHECK(result->contacts == 2U);
    REQUIRE(collision.events().size() == 2U);
    CHECK(world.velocity(*moving)->linear.x == Catch::Approx(20.0F));
    CHECK(world.transform(*moving)->position.x < 0.0F);
}

TEST_CASE("Nonblocking trigger does not suppress a later blocking contact") {
    ai2d::World world{};
    REQUIRE(world.reserve(3U));
    const auto moving = world.create_entity(
        ai2d::Transform2D{{-4.0F, 0.0F}}, ai2d::Velocity2D{{10.0F, 0.0F}}, std::nullopt);
    REQUIRE(moving);
    REQUIRE(world.add(*moving, ai2d::Collider2D{{}, {0.2F, 0.2F}, 1U, ai2d::BodyMotion2D::dynamic_body}));
    REQUIRE(world.add(*moving, ai2d::EntityState2D{}));
    const auto trigger = world.create_entity(ai2d::Transform2D{{-1.0F, 0.0F}}, std::nullopt, std::nullopt);
    REQUIRE(trigger);
    REQUIRE(world.add(*trigger, ai2d::Collider2D{{}, {0.2F, 1.0F}, 2U, ai2d::BodyMotion2D::static_body, true}));
    REQUIRE(world.add(*trigger, ai2d::EntityState2D{}));
    const auto wall = world.create_entity(ai2d::Transform2D{{2.0F, 0.0F}}, std::nullopt, std::nullopt);
    REQUIRE(wall);
    REQUIRE(world.add(*wall, ai2d::Collider2D{{}, {0.2F, 1.0F}, 3U, ai2d::BodyMotion2D::static_body}));
    REQUIRE(world.add(*wall, ai2d::EntityState2D{}));

    ai2d::CollisionRule2D rules[2]{};
    rules[0].group_a = 1U;
    rules[0].group_b = 2U;
    rules[0].reaction_count = 1U;
    rules[0].reactions[0].kind = ai2d::CollisionReactionKind2D::add_int_state;
    rules[1].group_a = 1U;
    rules[1].group_b = 3U;
    rules[1].reaction_count = 1U;
    rules[1].reactions[0] = {ai2d::CollisionReactionKind2D::reflect, ai2d::CollisionTarget2D::a};
    ai2d::CollisionGrid2D collision{};
    REQUIRE(collision.initialize({{{-6.0F, -2.0F}, {6.0F, 2.0F}}, {1.0F, 1.0F}, 3U, 128U, 8U, 4U}));

    const auto result = collision.simulate(world, rules, 1.0F);
    REQUIRE(result);
    CHECK(result->contacts == 2U);
    CHECK(world.velocity(*moving)->linear.x == Catch::Approx(-10.0F));
}

TEST_CASE("Kinematic sweep uses start position and relative velocity") {
    ai2d::World world{};
    REQUIRE(world.reserve(2U));
    const auto moving = world.create_entity(
        ai2d::Transform2D{{0.0F, 0.0F}}, ai2d::Velocity2D{{0.0F, 0.0F}}, std::nullopt);
    REQUIRE(moving);
    REQUIRE(world.add(*moving, ai2d::Collider2D{{}, {0.25F, 0.25F}, 1U, ai2d::BodyMotion2D::dynamic_body}));
    REQUIRE(world.add(*moving, ai2d::EntityState2D{}));
    ai2d::Transform2D kinematic_transform{{2.0F, 0.0F}};
    kinematic_transform.previous_position = {-2.0F, 0.0F};
    const auto kinematic = world.create_entity(
        kinematic_transform, ai2d::Velocity2D{{4.0F, 0.0F}}, std::nullopt);
    REQUIRE(kinematic);
    REQUIRE(world.add(*kinematic, ai2d::Collider2D{{}, {0.25F, 0.25F}, 2U, ai2d::BodyMotion2D::kinematic_body}));
    REQUIRE(world.add(*kinematic, ai2d::EntityState2D{}));
    ai2d::CollisionRule2D rule{};
    rule.group_a = 1U;
    rule.group_b = 2U;
    rule.reaction_count = 1U;
    rule.reactions[0] = {ai2d::CollisionReactionKind2D::reflect, ai2d::CollisionTarget2D::a};
    ai2d::CollisionGrid2D collision{};
    REQUIRE(collision.initialize({{{-4.0F, -2.0F}, {6.0F, 2.0F}}, {0.5F, 0.5F}, 2U, 128U, 4U, 4U}));

    const auto result = collision.simulate(world, std::span{&rule, 1U}, 1.0F);
    REQUIRE(result);
    CHECK(result->contacts == 1U);
    CHECK(world.velocity(*moving)->linear.x == Catch::Approx(8.0F));
}

TEST_CASE("Reflected segment re-queries cells after a kinematic speed transfer") {
    ai2d::World world{};
    REQUIRE(world.reserve(3U));
    const auto moving = world.create_entity(
        ai2d::Transform2D{{0.0F, 0.0F}}, ai2d::Velocity2D{{0.0F, 0.0F}}, std::nullopt);
    REQUIRE(moving);
    REQUIRE(world.add(*moving, ai2d::Collider2D{{}, {0.25F, 0.25F}, 1U, ai2d::BodyMotion2D::dynamic_body}));
    REQUIRE(world.add(*moving, ai2d::EntityState2D{}));

    ai2d::Transform2D kinematic_transform{{2.0F, 0.0F}};
    kinematic_transform.previous_position = {-2.0F, 0.0F};
    const auto kinematic = world.create_entity(
        kinematic_transform, ai2d::Velocity2D{{4.0F, 0.0F}}, std::nullopt);
    REQUIRE(kinematic);
    REQUIRE(world.add(*kinematic, ai2d::Collider2D{{}, {0.25F, 0.25F}, 2U, ai2d::BodyMotion2D::kinematic_body}));
    REQUIRE(world.add(*kinematic, ai2d::EntityState2D{}));

    const auto wall = world.create_entity(ai2d::Transform2D{{5.0F, 0.0F}}, std::nullopt, std::nullopt);
    REQUIRE(wall);
    REQUIRE(world.add(*wall, ai2d::Collider2D{{}, {0.25F, 1.0F}, 3U, ai2d::BodyMotion2D::static_body}));
    REQUIRE(world.add(*wall, ai2d::EntityState2D{}));

    ai2d::CollisionRule2D rules[2]{};
    for (std::size_t index = 0U; index < 2U; ++index) {
        rules[index].group_a = 1U;
        rules[index].group_b = static_cast<std::uint32_t>(2U + index);
        rules[index].reaction_count = 1U;
        rules[index].reactions[0] = {
            ai2d::CollisionReactionKind2D::reflect,
            ai2d::CollisionTarget2D::a,
        };
    }
    ai2d::CollisionGrid2D collision{};
    REQUIRE(collision.initialize({{{-4.0F, -2.0F}, {6.0F, 2.0F}}, {0.5F, 0.5F}, 3U, 128U, 4U, 4U}));

    const auto result = collision.simulate(world, rules, 1.0F);
    REQUIRE(result);
    CHECK(result->contacts == 2U);
    CHECK(world.velocity(*moving)->linear.x == Catch::Approx(-8.0F));
    CHECK(world.transform(*moving)->position.x < 5.0F);
}

TEST_CASE("Touching body moving away is not reflected back into the wall") {
    ai2d::World world{};
    REQUIRE(world.reserve(2U));
    const auto moving = world.create_entity(
        ai2d::Transform2D{{1.0F, 0.0F}}, ai2d::Velocity2D{{1.0F, 0.0F}}, std::nullopt);
    REQUIRE(moving);
    REQUIRE(world.add(*moving, ai2d::Collider2D{{}, {0.5F, 0.5F}, 1U, ai2d::BodyMotion2D::dynamic_body}));
    REQUIRE(world.add(*moving, ai2d::EntityState2D{}));
    const auto wall = world.create_entity(ai2d::Transform2D{{0.0F, 0.0F}}, std::nullopt, std::nullopt);
    REQUIRE(wall);
    REQUIRE(world.add(*wall, ai2d::Collider2D{{}, {0.5F, 1.0F}, 2U, ai2d::BodyMotion2D::static_body}));
    REQUIRE(world.add(*wall, ai2d::EntityState2D{}));
    ai2d::CollisionRule2D rule{};
    rule.group_a = 1U;
    rule.group_b = 2U;
    rule.reaction_count = 1U;
    rule.reactions[0] = {ai2d::CollisionReactionKind2D::reflect, ai2d::CollisionTarget2D::a};
    ai2d::CollisionGrid2D collision{};
    REQUIRE(collision.initialize({{{-2.0F, -2.0F}, {4.0F, 2.0F}}, {1.0F, 1.0F}, 2U, 32U, 2U, 2U}));

    const auto result = collision.simulate(world, std::span{&rule, 1U}, 1.0F);
    REQUIRE(result);
    CHECK(result->contacts == 0U);
    CHECK(world.velocity(*moving)->linear.x == Catch::Approx(1.0F));
    CHECK(world.transform(*moving)->position.x == Catch::Approx(2.0F));
}

TEST_CASE("Candidate capacity counts a logical pair only once across shared cells") {
    ai2d::World world{};
    REQUIRE(world.reserve(2U));
    const auto moving = world.create_entity(
        ai2d::Transform2D{{0.0F, 0.0F}}, ai2d::Velocity2D{}, std::nullopt);
    REQUIRE(moving);
    REQUIRE(world.add(*moving, ai2d::Collider2D{{}, {2.0F, 2.0F}, 1U, ai2d::BodyMotion2D::dynamic_body}));
    REQUIRE(world.add(*moving, ai2d::EntityState2D{}));
    const auto other = world.create_entity(ai2d::Transform2D{{0.0F, 0.0F}}, std::nullopt, std::nullopt);
    REQUIRE(other);
    REQUIRE(world.add(*other, ai2d::Collider2D{{}, {2.0F, 2.0F}, 2U, ai2d::BodyMotion2D::static_body, true}));
    REQUIRE(world.add(*other, ai2d::EntityState2D{}));
    ai2d::CollisionRule2D rule{};
    rule.group_a = 1U;
    rule.group_b = 2U;
    rule.reaction_count = 1U;
    rule.reactions[0].kind = ai2d::CollisionReactionKind2D::add_int_state;
    ai2d::CollisionGrid2D collision{};
    REQUIRE(collision.initialize({{{-3.0F, -3.0F}, {3.0F, 3.0F}}, {0.5F, 0.5F}, 2U, 512U, 1U, 1U}));

    const auto result = collision.simulate(world, std::span{&rule, 1U}, 1.0F / 60.0F);
    REQUIRE(result);
    CHECK(result->candidate_pairs == 1U);
}

TEST_CASE("Collision rules may emit events without physical reactions") {
    ai2d::World world{};
    REQUIRE(world.reserve(2U));
    const auto moving = world.create_entity(
        ai2d::Transform2D{{0.0F, 0.0F}}, ai2d::Velocity2D{{2.0F, 0.0F}}, std::nullopt);
    REQUIRE(moving);
    REQUIRE(world.add(*moving, ai2d::Collider2D{{}, {0.25F, 0.25F}, 1U, ai2d::BodyMotion2D::dynamic_body}));
    REQUIRE(world.add(*moving, ai2d::EntityState2D{}));
    const auto trigger = world.create_entity(ai2d::Transform2D{{1.0F, 0.0F}}, std::nullopt, std::nullopt);
    REQUIRE(trigger);
    REQUIRE(world.add(*trigger, ai2d::Collider2D{{}, {0.25F, 0.25F}, 2U, ai2d::BodyMotion2D::static_body, true}));
    REQUIRE(world.add(*trigger, ai2d::EntityState2D{}));

    ai2d::CollisionRule2D rule{};
    rule.group_a = 1U;
    rule.group_b = 2U;
    rule.reaction_count = 0U;
    ai2d::CollisionGrid2D collision{};
    REQUIRE(collision.initialize({{{-2.0F, -2.0F}, {3.0F, 2.0F}}, {0.5F, 0.5F}, 2U, 64U, 8U, 2U}));

    const auto result = collision.simulate(world, std::span{&rule, 1U}, 1.0F);
    REQUIRE(result);
    REQUIRE(collision.events().size() == 1U);
    CHECK(collision.events()[0].rule_index == 0U);
    CHECK(collision.events()[0].entity_a == *moving);
    CHECK(collision.events()[0].entity_b == *trigger);
    CHECK(world.entity_state(*moving)->active);
    CHECK(world.entity_state(*trigger)->active);
}

TEST_CASE("Physical deactivation metrics count each active target transition once") {
    ai2d::World world{};
    REQUIRE(world.reserve(2U));
    const auto moving = world.create_entity(
        ai2d::Transform2D{{0.0F, 0.0F}},
        ai2d::Velocity2D{{2.0F, 0.0F}},
        std::nullopt);
    REQUIRE(moving);
    REQUIRE(world.add(*moving, ai2d::Collider2D{{}, {0.25F, 0.25F}, 1U, ai2d::BodyMotion2D::dynamic_body}));
    REQUIRE(world.add(*moving, ai2d::EntityState2D{}));
    const auto target = world.create_entity(ai2d::Transform2D{{1.0F, 0.0F}}, std::nullopt, std::nullopt);
    REQUIRE(target);
    REQUIRE(world.add(*target, ai2d::Collider2D{{}, {0.25F, 0.25F}, 2U, ai2d::BodyMotion2D::static_body, true}));
    REQUIRE(world.add(*target, ai2d::EntityState2D{}));

    ai2d::CollisionRule2D rule{};
    rule.group_a = 1U;
    rule.group_b = 2U;
    rule.reaction_count = 2U;
    rule.reactions[0] = {ai2d::CollisionReactionKind2D::deactivate, ai2d::CollisionTarget2D::b};
    rule.reactions[1] = {ai2d::CollisionReactionKind2D::deactivate, ai2d::CollisionTarget2D::b};
    ai2d::CollisionGrid2D collision{};
    REQUIRE(collision.initialize({{{-2.0F, -2.0F}, {3.0F, 2.0F}}, {0.5F, 0.5F}, 2U, 64U, 8U, 2U}));

    const auto first = collision.simulate(world, std::span{&rule, 1U}, 1.0F);
    REQUIRE(first);
    CHECK(first->active_state_changes == 1U);
    CHECK_FALSE(world.entity_state(*target)->active);
    CHECK_FALSE(world.collider(*target)->enabled);

    const auto second = collision.simulate(world, std::span{&rule, 1U}, 1.0F);
    REQUIRE(second);
    CHECK(second->active_state_changes == 0U);
}

TEST_CASE("Collision candidate batching remains deterministic and allocation-free at overlap density") {
    constexpr std::uint32_t collider_count = 128U;
    ai2d::World world{};
    REQUIRE(world.reserve(collider_count));
    const auto moving = world.create_entity(
        ai2d::Transform2D{{0.0F, 0.0F}}, ai2d::Velocity2D{}, std::nullopt);
    REQUIRE(moving);
    REQUIRE(world.add(*moving, ai2d::Collider2D{{}, {0.1F, 0.1F}, 1U, ai2d::BodyMotion2D::dynamic_body}));
    REQUIRE(world.add(*moving, ai2d::EntityState2D{}));
    for (std::uint32_t index = 1U; index < collider_count; ++index) {
        const auto target = world.create_entity(ai2d::Transform2D{{0.0F, 0.0F}}, std::nullopt, std::nullopt);
        REQUIRE(target);
        REQUIRE(world.add(*target, ai2d::Collider2D{{}, {0.1F, 0.1F}, 2U, ai2d::BodyMotion2D::static_body, true}));
        REQUIRE(world.add(*target, ai2d::EntityState2D{}));
    }
    ai2d::CollisionRule2D rule{};
    rule.group_a = 1U;
    rule.group_b = 2U;
    ai2d::CollisionGrid2D collision{};
    REQUIRE(collision.initialize({
        {{-1.0F, -1.0F}, {1.0F, 1.0F}}, {0.25F, 0.25F}, collider_count, 512U,
        collider_count - 1U, 16U}));

    ai2d::MeasuredAllocationScope measured{};
    const auto result = collision.simulate(world, std::span{&rule, 1U}, 1.0F / 60.0F);
    const auto allocations = measured.finish();
    REQUIRE(result);
    CHECK(result->candidate_pairs == collider_count - 1U);
    CHECK(allocations.allocations == 0U);
    CHECK(allocations.bytes == 0U);
}
