#include "ai2d/foundation/allocation_tracker.hpp"
#include "ai2d/world/collision.hpp"
#include "ai2d/world/components.hpp"
#include "ai2d/world/world.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <vector>

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

TEST_CASE("Moving trigger contacts begin once, persist, and end deterministically") {
    ai2d::World world{};
    REQUIRE(world.reserve(2U));

    ai2d::Transform2D first_transform{{0.0F, 0.0F}};
    first_transform.previous_position = {-2.0F, 0.0F};
    const auto first = world.create_entity(first_transform, ai2d::Velocity2D{{2.0F, 0.0F}}, std::nullopt);
    REQUIRE(first);
    REQUIRE(world.add(*first, ai2d::Collider2D{{}, {0.5F, 0.5F}, 1U, ai2d::BodyMotion2D::kinematic_body}));
    REQUIRE(world.add(*first, ai2d::EntityState2D{}));

    ai2d::Transform2D second_transform{{0.0F, 0.0F}};
    second_transform.previous_position = {2.0F, 0.0F};
    const auto second = world.create_entity(second_transform, ai2d::Velocity2D{{-2.0F, 0.0F}}, std::nullopt);
    REQUIRE(second);
    REQUIRE(world.add(*second, ai2d::Collider2D{{}, {0.5F, 0.5F}, 2U, ai2d::BodyMotion2D::kinematic_body}));
    REQUIRE(world.add(*second, ai2d::EntityState2D{}));

    ai2d::CollisionRule2D trigger{};
    trigger.group_a = 1U;
    trigger.group_b = 2U;
    trigger.interaction = ai2d::CollisionInteraction2D::trigger;
    ai2d::CollisionGridConfig2D config{};
    config.bounds = {{-5.0F, -2.0F}, {5.0F, 2.0F}};
    config.cell_size = {0.5F, 0.5F};
    config.max_colliders = 2U;
    config.max_grid_references = 128U;
    config.max_candidate_pairs = 4U;
    config.max_contact_pairs = 2U;
    ai2d::CollisionGrid2D collision{};
    REQUIRE(collision.initialize(config));

    const auto entered = collision.simulate(world, std::span{&trigger, 1U}, 1.0F);
    REQUIRE(entered);
    CHECK(entered->contact_begins == 1U);
    CHECK(entered->contact_ends == 0U);
    CHECK(entered->active_contact_pairs == 1U);
    REQUIRE(collision.events().size() == 1U);
    CHECK(collision.events()[0].phase == ai2d::CollisionEventPhase2D::contact_begin);

    world.transform(*first)->previous_position = world.transform(*first)->position;
    world.transform(*second)->previous_position = world.transform(*second)->position;
    const auto persisted = collision.simulate(world, std::span{&trigger, 1U}, 1.0F / 60.0F);
    REQUIRE(persisted);
    CHECK(persisted->contact_begins == 0U);
    CHECK(persisted->contact_ends == 0U);
    CHECK(persisted->active_contact_pairs == 1U);
    CHECK(collision.events().empty());

    world.transform(*first)->previous_position = world.transform(*first)->position;
    world.transform(*second)->previous_position = world.transform(*second)->position;
    world.transform(*first)->position = {2.0F, 0.0F};
    world.transform(*second)->position = {-2.0F, 0.0F};
    const auto exited = collision.simulate(world, std::span{&trigger, 1U}, 1.0F);
    REQUIRE(exited);
    CHECK(exited->contact_begins == 0U);
    CHECK(exited->contact_ends == 1U);
    CHECK(exited->active_contact_pairs == 0U);
    REQUIRE(collision.events().size() == 1U);
    CHECK(collision.events()[0].phase == ai2d::CollisionEventPhase2D::contact_end);
}

TEST_CASE("Contact set diff emits end after active teleport but not after deactivation") {
    const auto run_case = [](const bool deactivate) {
        ai2d::World world{};
        REQUIRE(world.reserve(2U));
        const auto first = world.create_entity(ai2d::Transform2D{}, std::nullopt, std::nullopt);
        const auto second = world.create_entity(ai2d::Transform2D{}, std::nullopt, std::nullopt);
        REQUIRE(first);
        REQUIRE(second);
        REQUIRE(world.add(*first, ai2d::Collider2D{{}, {0.5F, 0.5F}, 1U, ai2d::BodyMotion2D::static_body}));
        REQUIRE(world.add(*second, ai2d::Collider2D{{}, {0.5F, 0.5F}, 2U, ai2d::BodyMotion2D::static_body}));
        REQUIRE(world.add(*first, ai2d::EntityState2D{}));
        REQUIRE(world.add(*second, ai2d::EntityState2D{}));
        ai2d::CollisionRule2D trigger{};
        trigger.group_a = 1U;
        trigger.group_b = 2U;
        trigger.interaction = ai2d::CollisionInteraction2D::trigger;
        ai2d::CollisionGridConfig2D config{};
        config.bounds = {{-8.0F, -2.0F}, {8.0F, 2.0F}};
        config.cell_size = {1.0F, 1.0F};
        config.max_colliders = 2U;
        config.max_grid_references = 32U;
        config.max_candidate_pairs = 4U;
        config.max_contact_pairs = 2U;
        ai2d::CollisionGrid2D collision{};
        REQUIRE(collision.initialize(config));
        const auto entered = collision.simulate(world, std::span{&trigger, 1U}, 1.0F / 60.0F);
        REQUIRE(entered);
        REQUIRE(entered->active_contact_pairs == 1U);

        if (deactivate) {
            world.entity_state(*first)->active = false;
            world.collider(*first)->enabled = false;
        } else {
            world.transform(*first)->position = {-6.0F, 0.0F};
            world.transform(*first)->previous_position = {-6.0F, 0.0F};
        }
        const auto separated = collision.simulate(world, std::span{&trigger, 1U}, 1.0F / 60.0F);
        REQUIRE(separated);
        CHECK(separated->active_contact_pairs == 0U);
        CHECK(separated->contact_ends == (deactivate ? 0U : 1U));
        if (deactivate) {
            CHECK(collision.events().empty());
        } else {
            REQUIRE(collision.events().size() == 1U);
            CHECK(collision.events()[0].phase == ai2d::CollisionEventPhase2D::contact_end);
        }
    };

    SECTION("active teleport") { run_case(false); }
    SECTION("deactivation") { run_case(true); }
}

TEST_CASE("Dynamic trigger bodies report high-speed pass-through contact positions") {
    ai2d::World world{};
    REQUIRE(world.reserve(2U));
    const auto first = world.create_entity(
        ai2d::Transform2D{{-5.0F, 0.0F}}, ai2d::Velocity2D{{20.0F, 0.0F}}, std::nullopt);
    const auto second = world.create_entity(
        ai2d::Transform2D{{5.0F, 0.0F}}, ai2d::Velocity2D{{-20.0F, 0.0F}}, std::nullopt);
    REQUIRE(first);
    REQUIRE(second);
    REQUIRE(world.add(*first, ai2d::Collider2D{{}, {0.25F, 0.25F}, 1U, ai2d::BodyMotion2D::dynamic_body}));
    REQUIRE(world.add(*second, ai2d::Collider2D{{}, {0.25F, 0.25F}, 2U, ai2d::BodyMotion2D::dynamic_body}));
    REQUIRE(world.add(*first, ai2d::EntityState2D{}));
    REQUIRE(world.add(*second, ai2d::EntityState2D{}));

    ai2d::CollisionRule2D trigger{};
    trigger.group_a = 1U;
    trigger.group_b = 2U;
    trigger.interaction = ai2d::CollisionInteraction2D::trigger;
    ai2d::CollisionGridConfig2D config{};
    config.bounds = {{-8.0F, -2.0F}, {8.0F, 2.0F}};
    config.cell_size = {1.0F, 1.0F};
    config.max_colliders = 2U;
    config.max_grid_references = 128U;
    config.max_candidate_pairs = 4U;
    config.max_contact_pairs = 2U;
    ai2d::CollisionGrid2D collision{};
    REQUIRE(collision.initialize(config));

    const auto result = collision.simulate(world, std::span{&trigger, 1U}, 0.5F);
    REQUIRE(result);
    CHECK(result->contact_begins == 1U);
    CHECK(result->contact_ends == 0U);
    CHECK(result->active_contact_pairs == 0U);
    REQUIRE(collision.events().size() == 1U);
    const auto& event = collision.events()[0];
    CHECK(event.phase == ai2d::CollisionEventPhase2D::contact_begin);
    CHECK(event.time_of_impact == Catch::Approx(0.2375F));
    CHECK(event.position_a.x == Catch::Approx(-0.25F));
    CHECK(event.position_b.x == Catch::Approx(0.25F));
    CHECK(world.transform(*first)->position.x == Catch::Approx(5.0F));
    CHECK(world.transform(*second)->position.x == Catch::Approx(-5.0F));
}

TEST_CASE("Dynamic and authored kinematic segments use relative trigger motion") {
    ai2d::World world{};
    REQUIRE(world.reserve(2U));
    const auto dynamic = world.create_entity(
        ai2d::Transform2D{{-2.0F, 0.0F}}, ai2d::Velocity2D{{4.0F, 0.0F}}, std::nullopt);
    REQUIRE(dynamic);
    REQUIRE(world.add(*dynamic, ai2d::Collider2D{{}, {0.25F, 0.25F}, 1U, ai2d::BodyMotion2D::dynamic_body}));
    REQUIRE(world.add(*dynamic, ai2d::EntityState2D{}));
    ai2d::Transform2D authored{{0.0F, 0.0F}};
    authored.previous_position = {2.0F, 0.0F};
    const auto kinematic = world.create_entity(authored, ai2d::Velocity2D{{-2.0F, 0.0F}}, std::nullopt);
    REQUIRE(kinematic);
    REQUIRE(world.add(*kinematic, ai2d::Collider2D{{}, {0.25F, 0.25F}, 2U, ai2d::BodyMotion2D::kinematic_body}));
    REQUIRE(world.add(*kinematic, ai2d::EntityState2D{}));
    ai2d::CollisionRule2D trigger{};
    trigger.group_a = 1U;
    trigger.group_b = 2U;
    trigger.interaction = ai2d::CollisionInteraction2D::trigger;
    ai2d::CollisionGridConfig2D config{};
    config.bounds = {{-4.0F, -2.0F}, {4.0F, 2.0F}};
    config.cell_size = {0.5F, 0.5F};
    config.max_colliders = 2U;
    config.max_grid_references = 128U;
    config.max_candidate_pairs = 4U;
    config.max_contact_pairs = 2U;
    ai2d::CollisionGrid2D collision{};
    REQUIRE(collision.initialize(config));

    const auto result = collision.simulate(world, std::span{&trigger, 1U}, 1.0F);
    REQUIRE(result);
    CHECK(result->contact_begins == 1U);
    CHECK(result->active_contact_pairs == 0U);
    REQUIRE(collision.events().size() == 1U);
    CHECK(collision.events()[0].time_of_impact == Catch::Approx(0.5833333F));
    CHECK(collision.events()[0].position_a.x == Catch::Approx(0.3333333F));
    CHECK(collision.events()[0].position_b.x == Catch::Approx(0.8333333F));
}

TEST_CASE("Trigger evaluation follows reflected dynamic motion segments") {
    ai2d::World world{};
    REQUIRE(world.reserve(3U));
    const auto moving = world.create_entity(
        ai2d::Transform2D{{0.0F, 0.0F}}, ai2d::Velocity2D{{10.0F, 0.0F}}, std::nullopt);
    const auto wall = world.create_entity(ai2d::Transform2D{{2.0F, 0.0F}}, std::nullopt, std::nullopt);
    const auto zone = world.create_entity(ai2d::Transform2D{{-1.0F, 0.0F}}, std::nullopt, std::nullopt);
    REQUIRE(moving);
    REQUIRE(wall);
    REQUIRE(zone);
    REQUIRE(world.add(*moving, ai2d::Collider2D{{}, {0.5F, 0.5F}, 1U, ai2d::BodyMotion2D::dynamic_body}));
    REQUIRE(world.add(*wall, ai2d::Collider2D{{}, {0.5F, 2.0F}, 2U, ai2d::BodyMotion2D::static_body}));
    REQUIRE(world.add(*zone, ai2d::Collider2D{{}, {0.2F, 1.0F}, 3U, ai2d::BodyMotion2D::static_body}));
    REQUIRE(world.add(*moving, ai2d::EntityState2D{}));
    REQUIRE(world.add(*wall, ai2d::EntityState2D{}));
    REQUIRE(world.add(*zone, ai2d::EntityState2D{}));

    ai2d::CollisionRule2D rules[2]{};
    rules[0].group_a = 1U;
    rules[0].group_b = 2U;
    rules[0].interaction = ai2d::CollisionInteraction2D::solid;
    rules[0].reaction_count = 1U;
    rules[0].reactions[0] = {ai2d::CollisionReactionKind2D::reflect, ai2d::CollisionTarget2D::a};
    rules[1].group_a = 1U;
    rules[1].group_b = 3U;
    rules[1].interaction = ai2d::CollisionInteraction2D::trigger;
    ai2d::CollisionGridConfig2D config{};
    config.bounds = {{-5.0F, -3.0F}, {5.0F, 3.0F}};
    config.cell_size = {0.5F, 0.5F};
    config.max_colliders = 3U;
    config.max_grid_references = 256U;
    config.max_candidate_pairs = 8U;
    config.max_contact_pairs = 2U;
    config.max_impacts_per_dynamic = 4U;
    ai2d::CollisionGrid2D collision{};
    REQUIRE(collision.initialize(config));

    const auto result = collision.simulate(world, rules, 0.5F);
    REQUIRE(result);
    CHECK(result->contacts == 1U);
    CHECK(result->contact_begins == 1U);
    CHECK(result->motion_segments >= 4U);
    REQUIRE(collision.events().size() == 2U);
    CHECK(collision.events()[0].phase == ai2d::CollisionEventPhase2D::collision);
    CHECK(collision.events()[1].phase == ai2d::CollisionEventPhase2D::contact_begin);
    CHECK(world.velocity(*moving)->linear.x == Catch::Approx(-10.0F));
    CHECK(world.transform(*moving)->position.x < -1.0F);
}

TEST_CASE("Trigger contact capacity has a stable dedicated diagnostic") {
    ai2d::World world{};
    REQUIRE(world.reserve(3U));
    const auto first = world.create_entity(ai2d::Transform2D{}, std::nullopt, std::nullopt);
    REQUIRE(first);
    REQUIRE(world.add(*first, ai2d::Collider2D{{}, {1.0F, 1.0F}, 1U, ai2d::BodyMotion2D::static_body}));
    REQUIRE(world.add(*first, ai2d::EntityState2D{}));
    for (std::uint32_t index = 0U; index < 2U; ++index) {
        const auto target = world.create_entity(ai2d::Transform2D{}, std::nullopt, std::nullopt);
        REQUIRE(target);
        REQUIRE(world.add(*target, ai2d::Collider2D{{}, {0.5F, 0.5F}, 2U, ai2d::BodyMotion2D::static_body}));
        REQUIRE(world.add(*target, ai2d::EntityState2D{}));
    }
    ai2d::CollisionRule2D trigger{};
    trigger.group_a = 1U;
    trigger.group_b = 2U;
    trigger.interaction = ai2d::CollisionInteraction2D::trigger;
    ai2d::CollisionGridConfig2D config{};
    config.bounds = {{-2.0F, -2.0F}, {2.0F, 2.0F}};
    config.cell_size = {1.0F, 1.0F};
    config.max_colliders = 3U;
    config.max_grid_references = 64U;
    config.max_candidate_pairs = 4U;
    config.max_contact_pairs = 1U;
    ai2d::CollisionGrid2D collision{};
    REQUIRE(collision.initialize(config));

    const auto result = collision.simulate(world, std::span{&trigger, 1U}, 1.0F / 60.0F);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == ai2d::DiagnosticCode::collision_contact_capacity_exceeded);
}

TEST_CASE("Ten thousand sparse trigger entities churn contacts without tracked allocation") {
    constexpr std::uint32_t pair_count = 5'000U;
    constexpr std::uint32_t entity_count = pair_count * 2U;
    ai2d::World world{};
    REQUIRE(world.reserve(entity_count));
    std::vector<ai2d::EntityId> moving_endpoints{};
    moving_endpoints.reserve(pair_count);
    for (std::uint32_t pair = 0U; pair < pair_count; ++pair) {
        const float x = static_cast<float>(pair * 3U);
        const auto first = world.create_entity(ai2d::Transform2D{{x, 0.0F}}, std::nullopt, std::nullopt);
        ai2d::Transform2D second_transform{{x, 0.0F}};
        second_transform.previous_position = second_transform.position;
        const auto second = world.create_entity(second_transform, std::nullopt, std::nullopt);
        REQUIRE(first);
        REQUIRE(second);
        REQUIRE(world.add(*first, ai2d::Collider2D{{}, {0.1F, 0.1F}, 1U, ai2d::BodyMotion2D::static_body}));
        REQUIRE(world.add(*second, ai2d::Collider2D{{}, {0.1F, 0.1F}, 2U, ai2d::BodyMotion2D::kinematic_body}));
        REQUIRE(world.add(*first, ai2d::EntityState2D{}));
        REQUIRE(world.add(*second, ai2d::EntityState2D{}));
        moving_endpoints.push_back(*second);
    }
    ai2d::CollisionRule2D trigger{};
    trigger.group_a = 1U;
    trigger.group_b = 2U;
    trigger.interaction = ai2d::CollisionInteraction2D::trigger;
    ai2d::CollisionGridConfig2D config{};
    config.bounds = {{-2.0F, -1.0F}, {static_cast<float>(pair_count * 3U + 2U), 1.0F}};
    config.cell_size = {1.0F, 1.0F};
    config.max_colliders = entity_count;
    config.max_grid_references = entity_count * 4U;
    config.max_candidate_pairs = pair_count;
    config.max_contact_pairs = pair_count;
    ai2d::CollisionGrid2D collision{};
    REQUIRE(collision.initialize(config));
    const auto warmup = collision.simulate(world, std::span{&trigger, 1U}, 1.0F / 60.0F);
    REQUIRE(warmup);
    REQUIRE(warmup->active_contact_pairs == pair_count);

    bool succeeded = true;
    ai2d::CollisionMetrics2D last{};
    ai2d::MeasuredAllocationScope measured{};
    for (std::uint32_t iteration = 0U; iteration < 10U; ++iteration) {
        const float offset = iteration % 2U == 0U ? 1.0F : 0.0F;
        for (std::uint32_t pair = 0U; pair < pair_count; ++pair) {
            auto* transform = world.transform(moving_endpoints[pair]);
            transform->position = {static_cast<float>(pair * 3U) + offset, 0.0F};
            transform->previous_position = transform->position;
        }
        const auto result = collision.simulate(world, std::span{&trigger, 1U}, 1.0F / 60.0F);
        if (!result) {
            succeeded = false;
            break;
        }
        last = *result;
    }
    const auto allocations = measured.finish();
    REQUIRE(succeeded);
    CHECK(last.contact_begins == pair_count);
    CHECK(last.active_contact_pairs == pair_count);
    CHECK(allocations.allocations == 0U);
    CHECK(allocations.bytes == 0U);
}

TEST_CASE("Contact snapshot restoration rejects malformed and stale pair identities") {
    ai2d::CollisionGridConfig2D config{};
    config.bounds = {{-2.0F, -2.0F}, {2.0F, 2.0F}};
    config.cell_size = {1.0F, 1.0F};
    config.max_colliders = 2U;
    config.max_grid_references = 16U;
    config.max_candidate_pairs = 2U;
    config.max_contact_pairs = 1U;
    ai2d::CollisionGrid2D collision{};
    REQUIRE(collision.initialize(config));

    const std::array excessive{
        ai2d::CollisionContactPair2D{0U, {0U, 1U}, {1U, 1U}},
        ai2d::CollisionContactPair2D{0U, {2U, 1U}, {3U, 1U}},
    };
    const auto rejected = collision.restore_contact_pairs(excessive);
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().code == ai2d::DiagnosticCode::runtime_contact_state_invalid);

    const std::array stale{
        ai2d::CollisionContactPair2D{0U, {100U, 1U}, {101U, 1U}},
    };
    REQUIRE(collision.restore_contact_pairs(stale));
    ai2d::World world{};
    REQUIRE(world.reserve(1U));
    ai2d::CollisionRule2D trigger{};
    trigger.group_a = 1U;
    trigger.group_b = 2U;
    trigger.interaction = ai2d::CollisionInteraction2D::trigger;
    const auto simulated = collision.simulate(world, std::span{&trigger, 1U}, 1.0F / 60.0F);
    REQUIRE_FALSE(simulated);
    CHECK(simulated.error().code == ai2d::DiagnosticCode::runtime_contact_state_invalid);
}
