#include "ai2d/foundation/allocation_tracker.hpp"
#include "ai2d/runtime/game_runtime.hpp"
#include "ai2d/runtime/runtime.hpp"
#include "ai2d/scenario/game.hpp"
#include "ai2d/scenario/scenario.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

namespace {

const std::filesystem::path scenario_root{AI2D_SOURCE_DIR "/tests/fixtures/scenarios/valid"};
const std::filesystem::path snake_manifest{AI2D_SOURCE_DIR "/samples/snake/game.json"};
const std::filesystem::path projectile_arena_manifest{AI2D_SOURCE_DIR "/samples/projectile_arena/game.json"};
const std::filesystem::path timed_pickups_manifest{AI2D_SOURCE_DIR "/samples/timed_pickups/game.json"};
const std::filesystem::path pool_siege_manifest{AI2D_SOURCE_DIR "/samples/pool_siege/game.json"};
const std::filesystem::path contact_course_manifest{AI2D_SOURCE_DIR "/samples/contact_course/game.json"};

} // namespace

TEST_CASE("Numeric execution plan runs headless without measured frame allocation") {
    auto plan = ai2d::compile_scenario_file(scenario_root / "moving_sprites.json");
    REQUIRE(plan.has_value());

    ai2d::ScenarioRuntime runtime{};
    ai2d::RuntimeOptions options{};
    options.headless = true;
    auto loaded = runtime.initialize(*plan, options);
    REQUIRE(loaded.has_value());
    CHECK(loaded->spawned_entities == 1000U);

    for (std::uint32_t frame = 0U; frame < 2U; ++frame) {
        REQUIRE(runtime.run_frame().has_value());
    }

    ai2d::MeasuredAllocationScope measured{};
    ai2d::RuntimeFrameMetrics final_frame{};
    bool frames_ok = true;
    for (std::uint32_t frame = 0U; frame < 10U; ++frame) {
        auto result = runtime.run_frame();
        if (!result) {
            frames_ok = false;
            break;
        }
        final_frame = *result;
    }
    const auto allocations = measured.finish();

    REQUIRE(frames_ok);
    CHECK(allocations.allocations == 0U);
    CHECK(final_frame.system_count == 2U);
    CHECK(final_frame.systems[0U].operation == ai2d::OperationId::integrate_velocity);
    CHECK(final_frame.systems[0U].actual_cardinality == 1000U);
    CHECK(final_frame.systems[1U].actual_cardinality == 1000U);
    CHECK(final_frame.world.alive_entities == 1000U);
    CHECK(final_frame.world.capacity_growth_events == 0U);
    CHECK(final_frame.render.extracted_sprites == 1000U);
    CHECK(final_frame.render.capacity_growth_events == 0U);
}

TEST_CASE("GameRuntime numeric actions drive exact grid steps and reject reversal without allocation") {
    auto plan = ai2d::compile_game_file(snake_manifest);
    REQUIRE(plan);
    ai2d::GameRuntime runtime{};
    ai2d::GameRuntimeOptions options{};
    options.headless = true;
    options.load_saved_settings = false;
    REQUIRE(runtime.initialize(*plan, options));

    std::vector<ai2d::GameActionInput> actions(plan->actions.size());
    actions[2U].pressed = true; // left, opposite to the authored initial right direction
    actions[2U].down = true;
    ai2d::MeasuredAllocationScope measured{};
    const auto frame = runtime.run_exact_actions(actions, 6U);
    const auto allocations = measured.finish();

    REQUIRE(frame);
    CHECK(frame->fixed_ticks == 6U);
    CHECK(frame->grid_steps == 1U);
    CHECK(frame->rejected_direction_changes == 1U);
    CHECK(frame->follower_updates == 63U); // includes all inactive reserve tail entities
    CHECK(allocations.allocations == 0U);
    CHECK(allocations.bytes == 0U);

    const std::array<ai2d::GameActionInput, 1U> wrong_count{};
    CHECK_FALSE(runtime.run_exact_actions(wrong_count, 1U));
}

TEST_CASE("Ordered game rules observe earlier state mutations in the same fixed tick") {
    auto plan = ai2d::compile_game_file(snake_manifest);
    REQUIRE(plan);
    auto& scene = plan->scenes[plan->start_scene];
    const auto first_symbol = static_cast<ai2d::SymbolId>(plan->symbols.size());
    plan->symbols.emplace_back("test_set_score");
    const auto second_symbol = static_cast<ai2d::SymbolId>(plan->symbols.size());
    plan->symbols.emplace_back("test_observe_score");

    ai2d::GameRulePlan first{};
    first.symbol = first_symbol;
    first.event.kind = ai2d::GameRuleEventKind::fixed_interval;
    first.event.interval_ticks = 1U;
    ai2d::GameRuleActionPlan set{};
    set.kind = ai2d::GameRuleActionKind::set_int_state;
    set.state_index = 0U;
    set.value = 5;
    first.actions.push_back(set);
    scene.rules.push_back(first);

    ai2d::GameRulePlan second{};
    second.symbol = second_symbol;
    second.event.kind = ai2d::GameRuleEventKind::fixed_interval;
    second.event.interval_ticks = 1U;
    ai2d::GameRuleConditionPlan condition{};
    condition.kind = ai2d::GameRuleConditionKind::int_state;
    condition.comparison = ai2d::GameComparison::equal;
    condition.state_index = 0U;
    condition.value = 5;
    second.conditions.push_back(condition);
    ai2d::GameRuleActionPlan add{};
    add.kind = ai2d::GameRuleActionKind::add_int_state;
    add.state_index = 0U;
    add.value = 2;
    second.actions.push_back(add);
    scene.rules.push_back(second);
    plan->plan_hash = ai2d::compute_game_plan_hash(*plan);
    REQUIRE(ai2d::validate_game_plan(*plan));

    ai2d::GameRuntime runtime{};
    ai2d::GameRuntimeOptions options{};
    options.headless = true;
    options.load_saved_settings = false;
    REQUIRE(runtime.initialize(*plan, options));
    std::vector<ai2d::GameActionInput> actions(plan->actions.size());
    const auto frame = runtime.run_exact_actions(actions, 1U);
    REQUIRE(frame);
    REQUIRE(runtime.state_value("score"));
    CHECK(*runtime.state_value("score") == 7);
    CHECK(frame->rule_executions == 2U);
    CHECK(frame->condition_evaluations == 1U);
    CHECK(frame->action_executions == 2U);
}

TEST_CASE("Free-cell relocation is deterministic for matching seeded runs") {
    auto compiled = ai2d::compile_game_file(snake_manifest);
    REQUIRE(compiled);
    auto plan = std::move(*compiled);
    std::uint32_t relocation_rule_index = 0U;
    bool found_relocation = false;
    const auto& rules = plan.scenes[plan.start_scene].rules;
    for (std::uint32_t rule_index = 0U; rule_index < rules.size(); ++rule_index) {
        for (const auto& action : rules[rule_index].actions) {
            if (action.kind == ai2d::GameRuleActionKind::relocate_to_free_cell) {
                relocation_rule_index = rule_index;
                found_relocation = true;
            }
        }
    }
    REQUIRE(found_relocation);
    const std::string relocation_rule_name{
        plan.symbol(plan.scenes[plan.start_scene].rules[relocation_rule_index].symbol)};
    auto shifted_once = plan;
    shifted_once.symbols[shifted_once.scenes[shifted_once.start_scene].rules[relocation_rule_index].symbol] =
        "unused_replaced_rule_symbol_once";
    shifted_once.symbols.push_back(relocation_rule_name);
    shifted_once.scenes[shifted_once.start_scene].rules[relocation_rule_index].symbol =
        static_cast<ai2d::SymbolId>(shifted_once.symbols.size() - 1U);
    shifted_once.plan_hash = ai2d::compute_game_plan_hash(shifted_once);
    auto shifted_twice = plan;
    shifted_twice.symbols[shifted_twice.scenes[shifted_twice.start_scene].rules[relocation_rule_index].symbol] =
        "unused_replaced_rule_symbol_twice";
    shifted_twice.symbols.emplace_back("unused_symbol_for_ordinal_shift");
    shifted_twice.symbols.push_back(relocation_rule_name);
    shifted_twice.scenes[shifted_twice.start_scene].rules[relocation_rule_index].symbol =
        static_cast<ai2d::SymbolId>(shifted_twice.symbols.size() - 1U);
    shifted_twice.plan_hash = ai2d::compute_game_plan_hash(shifted_twice);

    ai2d::GameRuntime runtime{};
    ai2d::GameRuntime second_runtime{};
    ai2d::GameRuntime third_runtime{};
    ai2d::GameRuntimeOptions options{};
    options.headless = true;
    options.load_saved_settings = false;
    REQUIRE(runtime.initialize(plan, options));
    REQUIRE(second_runtime.initialize(shifted_once, options));
    REQUIRE(third_runtime.initialize(shifted_twice, options));
    std::vector<ai2d::GameActionInput> actions(plan.actions.size());
    const auto first = runtime.run_exact_actions(actions, 12U);
    const auto second = second_runtime.run_exact_actions(actions, 12U);
    const auto third = third_runtime.run_exact_actions(actions, 12U);
    REQUIRE(first);
    REQUIRE(second);
    REQUIRE(third);
    CHECK(first->relocations == 1U);
    CHECK(first->relocation_cells_scanned > 0U);
    CHECK(first->relocation_cells_scanned == second->relocation_cells_scanned);
    CHECK(first->relocation_cells_scanned == third->relocation_cells_scanned);
    CHECK(first->scene_state_checksum == second->scene_state_checksum);
    CHECK(first->scene_state_checksum == third->scene_state_checksum);
    REQUIRE(runtime.state_value("food_available"));
    CHECK(*runtime.state_value("food_available") == 1);
}

TEST_CASE("GameRuntime aggregates legacy physical deactivation state changes exactly once") {
    auto compiled = ai2d::compile_game_file(snake_manifest);
    REQUIRE(compiled);
    auto plan = std::move(*compiled);
    auto& eat_rule = plan.scenes[plan.start_scene].collision_rules[0U];
    ai2d::GameReactionPlan deactivate_food{};
    deactivate_food.kind = ai2d::GameReactionKind::deactivate;
    deactivate_food.target = ai2d::GameReactionTarget::b;
    eat_rule.reactions.push_back(deactivate_food);
    eat_rule.reactions.push_back(deactivate_food);
    plan.plan_hash = ai2d::compute_game_plan_hash(plan);
    REQUIRE(ai2d::validate_game_plan(plan));

    ai2d::GameRuntime runtime{};
    ai2d::GameRuntimeOptions options{};
    options.headless = true;
    options.load_saved_settings = false;
    REQUIRE(runtime.initialize(plan, options));
    std::vector<ai2d::GameActionInput> actions(plan.actions.size());
    const auto frame = runtime.run_exact_actions(actions, 12U);
    REQUIRE(frame);
    CHECK(frame->collision.active_state_changes == 1U);
    CHECK(frame->active_state_changes == 3U); // food off/on plus one newly active tail segment
}

TEST_CASE("Active-count chain growth initializes actual inactive holes without corrupting follower history") {
    auto compiled = ai2d::compile_game_file(snake_manifest);
    REQUIRE(compiled);
    auto count_plan = *compiled;
    auto reference_plan = *compiled;
    auto& count_rule = count_plan.scenes[count_plan.start_scene].rules[4U];
    auto& reference_rule = reference_plan.scenes[reference_plan.start_scene].rules[4U];
    ai2d::GameRuleActionPlan deactivate_hole{};
    deactivate_hole.kind = ai2d::GameRuleActionKind::deactivate;
    deactivate_hole.target.kind = ai2d::GameRuleTargetKind::spawn_index;
    deactivate_hole.target.spawn_group_index = 1U;
    deactivate_hole.target.item_index = 0U;
    count_rule.actions.insert(count_rule.actions.begin(), deactivate_hole);
    reference_rule.actions.insert(reference_rule.actions.begin(), deactivate_hole);
    const auto reference_count = std::find_if(
        reference_rule.actions.begin(), reference_rule.actions.end(),
        [](const ai2d::GameRuleActionPlan& action) {
            return action.kind == ai2d::GameRuleActionKind::set_group_active_count;
        });
    REQUIRE(reference_count != reference_rule.actions.end());
    ai2d::GameRuleActionPlan activate_hole{};
    activate_hole.kind = ai2d::GameRuleActionKind::activate;
    activate_hole.target = deactivate_hole.target;
    reference_rule.actions.insert(reference_count, activate_hole);
    count_plan.plan_hash = ai2d::compute_game_plan_hash(count_plan);
    reference_plan.plan_hash = ai2d::compute_game_plan_hash(reference_plan);
    REQUIRE(ai2d::validate_game_plan(count_plan));
    REQUIRE(ai2d::validate_game_plan(reference_plan));

    ai2d::GameRuntime count_runtime{};
    ai2d::GameRuntime reference_runtime{};
    ai2d::GameRuntime baseline_runtime{};
    ai2d::GameRuntimeOptions options{};
    options.headless = true;
    options.load_saved_settings = false;
    REQUIRE(count_runtime.initialize(count_plan, options));
    REQUIRE(reference_runtime.initialize(reference_plan, options));
    REQUIRE(baseline_runtime.initialize(*compiled, options));
    std::vector<ai2d::GameActionInput> actions(count_plan.actions.size());
    const auto count_frame = count_runtime.run_exact_actions(actions, 12U);
    const auto reference_frame = reference_runtime.run_exact_actions(actions, 12U);
    const auto baseline_frame = baseline_runtime.run_exact_actions(actions, 12U);
    REQUIRE(count_frame);
    REQUIRE(reference_frame);
    REQUIRE(baseline_frame);
    CHECK(reference_frame->scene_state_checksum == baseline_frame->scene_state_checksum);
    CHECK(count_frame->scene_state_checksum != reference_frame->scene_state_checksum);
    CHECK(count_frame->active_state_changes == reference_frame->active_state_changes);
    CHECK(count_frame->active_state_changes == baseline_frame->active_state_changes + 2U);
}

TEST_CASE("Seeded plan execution is deterministic") {
    constexpr std::string_view source = R"json({
      "schema_version":"0.1","name":"random","seed":123,"world":{"capacity":8},
      "camera":{"position":[0,0],"half_extent":[8,8]},"textures":[],
      "spawn_groups":[{"id":"randoms","count":8,"placement":{"kind":"seeded_random","bounds":{"min":[-4,-4],"max":[4,4]}},"transform":{}}],
      "systems":[{"id":"wrap","operation":"wrap_bounds","phase":"fixed_update","parameters":{"bounds":{"min":[-4,-4],"max":[4,4]}}}],
      "benchmark":{"runs":1,"warmup_frames":0,"measurement_frames":1}
    })json";
    auto plan = ai2d::compile_scenario_text(source);
    REQUIRE(plan.has_value());

    ai2d::RuntimeOptions options{};
    options.headless = true;
    ai2d::ScenarioRuntime first{};
    ai2d::ScenarioRuntime second{};
    REQUIRE(first.initialize(*plan, options).has_value());
    REQUIRE(second.initialize(*plan, options).has_value());
    auto first_frame = first.run_frame();
    auto second_frame = second.run_frame();
    REQUIRE(first_frame.has_value());
    REQUIRE(second_frame.has_value());
    CHECK(first_frame->checksum == second_frame->checksum);
}

TEST_CASE("Runtime rejects malformed public execution plans before mutating state") {
    auto valid = ai2d::compile_scenario_file(scenario_root / "moving_sprites.json");
    REQUIRE(valid);
    ai2d::RuntimeOptions options{};
    options.headless = true;
    ai2d::ScenarioRuntime runtime{};

    const auto rejected = [&](auto mutate, const ai2d::DiagnosticCode expected) {
        auto plan = *valid;
        mutate(plan);
        const auto loaded = runtime.initialize(plan, options);
        REQUIRE_FALSE(loaded);
        CHECK(loaded.error().code == expected);
        CHECK_FALSE(runtime.initialized());
    };

    rejected(
        [](auto& plan) { plan.spawn_groups[0U].placement.columns = 0U; },
        ai2d::DiagnosticCode::ir_schema_invalid);
    rejected(
        [](auto& plan) { plan.systems.resize(ai2d::RuntimeFrameMetrics::max_systems + 1U); },
        ai2d::DiagnosticCode::ir_schema_invalid);
    rejected(
        [](auto& plan) { plan.systems[0U].operation = static_cast<ai2d::OperationId>(255U); },
        ai2d::DiagnosticCode::ir_schema_invalid);
    rejected(
        [](auto& plan) { plan.spawn_groups[0U].sprite.texture_asset = 999U; },
        ai2d::DiagnosticCode::ir_missing_texture);
    rejected(
        [](auto& plan) { ++plan.total_spawn_count; },
        ai2d::DiagnosticCode::ir_schema_invalid);
    rejected(
        [](auto& plan) { plan.spawn_groups[0U].transform.rotation = std::numeric_limits<float>::infinity(); },
        ai2d::DiagnosticCode::ir_schema_invalid);

    const auto loaded = runtime.initialize(*valid, options);
    REQUIRE(loaded);
    CHECK(runtime.initialized());
}

TEST_CASE("Extreme finite ScenarioSpec arithmetic returns a numeric diagnostic") {
    constexpr std::string_view source = R"json({
      "schema_version":"0.1","name":"extreme","seed":1,"world":{"capacity":1},
      "camera":{"position":[0,0],"half_extent":[8,8]},"textures":[],
      "spawn_groups":[{"id":"mover","count":1,"placement":{"kind":"grid","origin":[0,0],"spacing":[1,1],"columns":1},"transform":{},"velocity":{"linear":[3.4028234e38,0]}}],
      "systems":[
        {"id":"integrate","operation":"integrate_velocity","phase":"fixed_update","parameters":{"delta_seconds":3.4028234e38}},
        {"id":"wrap","operation":"wrap_bounds","phase":"fixed_update","after":["integrate"],"parameters":{"bounds":{"min":[-1,-1],"max":[1,1]}}}
      ],
      "benchmark":{"runs":1,"warmup_frames":0,"measurement_frames":1}
    })json";
    auto plan = ai2d::compile_scenario_text(source);
    REQUIRE(plan);
    ai2d::ScenarioRuntime runtime{};
    ai2d::RuntimeOptions options{};
    options.headless = true;
    REQUIRE(runtime.initialize(*plan, options));
    const auto frame = runtime.run_frame();
    REQUIRE_FALSE(frame);
    CHECK(frame.error().code == ai2d::DiagnosticCode::runtime_numeric_state_invalid);
}

TEST_CASE("Public GamePlan validation rejects overflowing derived spawn coordinates before runtime narrowing") {
    auto compiled = ai2d::compile_game_file(snake_manifest);
    REQUIRE(compiled);
    auto plan = *compiled;
    auto& wall = plan.scenes[plan.start_scene].spawn_groups[3U];
    wall.placement.origin = {std::numeric_limits<float>::max(), 0.0F};
    wall.transform.position_offset = {std::numeric_limits<float>::max(), 0.0F};
    plan.plan_hash = ai2d::compute_game_plan_hash(plan);
    REQUIRE_FALSE(ai2d::validate_game_plan(plan));

    ai2d::GameRuntime runtime{};
    ai2d::GameRuntimeOptions options{};
    options.headless = true;
    options.load_saved_settings = false;
    const auto initialized = runtime.initialize(plan, options);
    REQUIRE_FALSE(initialized);
}

TEST_CASE("Pool FIFO supports exhausted recycle ordered release reuse and release misses") {
    auto compiled = ai2d::compile_game_file(projectile_arena_manifest);
    REQUIRE(compiled);
    auto plan = *compiled;
    auto& scene = plan.scenes[plan.start_scene];
    scene.rules.clear();
    const auto symbol = static_cast<ai2d::SymbolId>(plan.symbols.size());
    plan.symbols.emplace_back("ordered_pool_churn");
    ai2d::GameRulePlan rule{};
    rule.symbol = symbol;
    rule.event.kind = ai2d::GameRuleEventKind::fixed_interval;
    rule.event.interval_ticks = 1U;
    for (std::uint32_t index = 0U; index < 3U; ++index) {
        ai2d::GameRuleActionPlan spawn{};
        spawn.kind = ai2d::GameRuleActionKind::spawn_from_pool;
        spawn.pool_index = 0U;
        spawn.pool_position_kind = ai2d::GamePoolSpawnPositionKind::constant;
        spawn.pool_position = {static_cast<float>(index + 1U), 0.0F};
        rule.actions.push_back(spawn);
    }
    ai2d::GameRuleActionPlan recycled{};
    recycled.kind = ai2d::GameRuleActionKind::spawn_from_pool;
    recycled.pool_index = 0U;
    recycled.pool_position_kind = ai2d::GamePoolSpawnPositionKind::constant;
    recycled.pool_position = {9.0F, 0.0F};
    rule.actions.push_back(recycled);
    ai2d::GameRuleActionPlan release{};
    release.kind = ai2d::GameRuleActionKind::release_to_pool;
    release.pool_index = 0U;
    release.target.kind = ai2d::GameRuleTargetKind::spawn_index;
    release.target.spawn_group_index = scene.pools[0U].spawn_group_index;
    release.target.item_index = 0U;
    rule.actions.push_back(release);
    rule.actions.push_back(release);
    ai2d::GameRuleActionPlan reuse = recycled;
    reuse.pool_position = {12.0F, 0.0F};
    rule.actions.push_back(reuse);
    scene.rules.push_back(rule);
    plan.plan_hash = ai2d::compute_game_plan_hash(plan);
    REQUIRE(ai2d::validate_game_plan(plan));

    ai2d::GameRuntime runtime{};
    ai2d::GameRuntimeOptions options{};
    options.headless = true;
    options.load_saved_settings = false;
    REQUIRE(runtime.initialize(plan, options));
    std::vector<ai2d::GameActionInput> actions(plan.actions.size());
    const auto frame = runtime.run_exact_actions(actions, 1U);
    REQUIRE(frame);
    CHECK(frame->pool_acquire_attempts == 5U);
    CHECK(frame->pool_acquire_successes == 5U);
    CHECK(frame->pool_exhaustions == 1U);
    CHECK(frame->pool_recycled_slots == 1U);
    CHECK(frame->pool_releases == 1U);
    CHECK(frame->pool_release_misses == 1U);
    CHECK(frame->active_pooled_entities == 3U);
    CHECK(frame->peak_active_pooled_entities == 3U);
}

TEST_CASE("Pool lifetime expires before its exact tick boundary and permits same-tick reacquire") {
    auto compiled = ai2d::compile_game_file(timed_pickups_manifest);
    REQUIRE(compiled);
    auto plan = *compiled;
    auto& scene = plan.scenes[plan.start_scene];
    scene.spawn_groups[scene.pools[0U].spawn_group_index].count = 1U;
    scene.total_spawn_count = 2U;
    scene.rules.clear();
    const auto symbol = static_cast<ai2d::SymbolId>(plan.symbols.size());
    plan.symbols.emplace_back("lifetime_boundary");
    ai2d::GameRulePlan rule{};
    rule.symbol = symbol;
    rule.event.kind = ai2d::GameRuleEventKind::fixed_interval;
    rule.event.interval_ticks = 1U;
    ai2d::GameRuleActionPlan spawn{};
    spawn.kind = ai2d::GameRuleActionKind::spawn_from_pool;
    spawn.pool_index = 0U;
    spawn.pool_position_kind = ai2d::GamePoolSpawnPositionKind::initial;
    spawn.has_lifetime = true;
    spawn.lifetime_ticks = 2U;
    rule.actions.push_back(spawn);
    scene.rules.push_back(rule);
    plan.plan_hash = ai2d::compute_game_plan_hash(plan);
    REQUIRE(ai2d::validate_game_plan(plan));

    ai2d::GameRuntime runtime{};
    ai2d::GameRuntimeOptions options{};
    options.headless = true;
    options.load_saved_settings = false;
    REQUIRE(runtime.initialize(plan, options));
    std::vector<ai2d::GameActionInput> actions(plan.actions.size());
    const auto first = runtime.run_exact_actions(actions, 1U);
    REQUIRE(first);
    CHECK(first->pool_acquire_successes == 1U);
    CHECK(first->active_pooled_entities == 1U);
    const auto second = runtime.run_exact_actions(actions, 1U);
    REQUIRE(second);
    CHECK(second->pool_expirations == 0U);
    CHECK(second->pool_exhaustions == 1U);
    CHECK(second->active_pooled_entities == 1U);
    const auto third = runtime.run_exact_actions(actions, 1U);
    REQUIRE(third);
    CHECK(third->pool_expirations == 1U);
    CHECK(third->pool_acquire_successes == 1U);
    CHECK(third->active_pooled_entities == 1U);
}

TEST_CASE("Collision rules can release and spawn a one-tick pooled object before its exact expiration") {
    auto compiled = ai2d::compile_game_file(projectile_arena_manifest);
    REQUIRE(compiled);
    auto plan = *compiled;
    auto& scene = plan.scenes[plan.start_scene];
    auto collision_rule = std::find_if(
        scene.rules.begin(), scene.rules.end(),
        [](const ai2d::GameRulePlan& rule) {
            return rule.event.kind == ai2d::GameRuleEventKind::collision &&
                   rule.event.collision_rule_index == 0U;
        });
    REQUIRE(collision_rule != scene.rules.end());
    collision_rule->actions.clear();
    ai2d::GameRuleActionPlan release{};
    release.kind = ai2d::GameRuleActionKind::release_to_pool;
    release.pool_index = 0U;
    release.target.kind = ai2d::GameRuleTargetKind::collision_a;
    collision_rule->actions.push_back(release);
    ai2d::GameRuleActionPlan spawn{};
    spawn.kind = ai2d::GameRuleActionKind::spawn_from_pool;
    spawn.pool_index = 0U;
    spawn.pool_position_kind = ai2d::GamePoolSpawnPositionKind::target;
    spawn.pool_position_target.kind = ai2d::GameRuleTargetKind::collision_b;
    spawn.pool_position_offset = {0.0F, 3.0F};
    spawn.has_velocity_override = true;
    spawn.velocity = {};
    spawn.has_lifetime = true;
    spawn.lifetime_ticks = 1U;
    collision_rule->actions.push_back(spawn);
    plan.plan_hash = ai2d::compute_game_plan_hash(plan);
    REQUIRE(ai2d::validate_game_plan(plan));

    ai2d::GameRuntime runtime{};
    ai2d::GameRuntimeOptions options{};
    options.headless = true;
    options.load_saved_settings = false;
    REQUIRE(runtime.initialize(plan, options));
    std::vector<ai2d::GameActionInput> actions(plan.actions.size());
    actions[0U].pressed = true;
    actions[0U].down = true;
    REQUIRE(runtime.run_exact_actions(actions, 1U));
    actions[0U] = {};

    bool collision_spawned = false;
    for (std::uint32_t tick = 0U; tick < 100U; ++tick) {
        const auto frame = runtime.run_exact_actions(actions, 1U);
        REQUIRE(frame);
        if (frame->pool_releases == 0U) continue;
        CHECK(frame->collision.contacts >= 1U);
        CHECK(frame->pool_acquire_successes == 1U);
        CHECK(frame->active_pooled_entities == 1U);
        collision_spawned = true;
        break;
    }
    REQUIRE(collision_spawned);
    const auto expired = runtime.run_exact_actions(actions, 1U);
    REQUIRE(expired);
    CHECK(expired->pool_expirations == 1U);
    CHECK(expired->active_pooled_entities == 0U);
}

TEST_CASE("Pool spawn synchronizes interpolation history after restoring authored components") {
    auto compiled = ai2d::compile_game_file(projectile_arena_manifest);
    REQUIRE(compiled);
    auto constant_plan = *compiled;
    auto initial_plan = *compiled;
    const auto prepare = [](ai2d::GamePlan& plan, const bool authored_position) {
        auto& scene = plan.scenes[plan.start_scene];
        auto& group = scene.spawn_groups[scene.pools[0U].spawn_group_index];
        group.count = 1U;
        group.active_count = 0U;
        if (authored_position) group.placement.origin = {4.0F, 2.0F};
        scene.total_spawn_count = 4U;
        scene.rules.clear();
        const auto symbol = static_cast<ai2d::SymbolId>(plan.symbols.size());
        plan.symbols.emplace_back(authored_position ? "spawn_initial_history" : "spawn_constant_history");
        ai2d::GameRulePlan rule{};
        rule.symbol = symbol;
        rule.event.kind = ai2d::GameRuleEventKind::fixed_interval;
        rule.event.interval_ticks = 1U;
        ai2d::GameRuleActionPlan spawn{};
        spawn.kind = ai2d::GameRuleActionKind::spawn_from_pool;
        spawn.pool_index = 0U;
        spawn.pool_position_kind = authored_position
                                       ? ai2d::GamePoolSpawnPositionKind::initial
                                       : ai2d::GamePoolSpawnPositionKind::constant;
        spawn.pool_position = {4.0F, 2.0F};
        rule.actions.push_back(spawn);
        scene.rules.push_back(rule);
        plan.transitions.clear();
        plan.plan_hash = ai2d::compute_game_plan_hash(plan);
    };
    prepare(constant_plan, false);
    prepare(initial_plan, true);
    REQUIRE(ai2d::validate_game_plan(constant_plan));
    REQUIRE(ai2d::validate_game_plan(initial_plan));

    ai2d::GameRuntime constant_runtime{};
    ai2d::GameRuntime initial_runtime{};
    ai2d::GameRuntimeOptions options{};
    options.headless = true;
    options.load_saved_settings = false;
    REQUIRE(constant_runtime.initialize(constant_plan, options));
    REQUIRE(initial_runtime.initialize(initial_plan, options));
    std::vector<ai2d::GameActionInput> actions(constant_plan.actions.size());
    const auto constant = constant_runtime.run_exact_actions(actions, 1U);
    const auto initial = initial_runtime.run_exact_actions(actions, 1U);
    REQUIRE(constant);
    REQUIRE(initial);
    CHECK(constant->active_pooled_entities == 1U);
    CHECK(constant->scene_state_checksum == initial->scene_state_checksum);
}

TEST_CASE("reset_pool restores initial prefix components and lifecycle ordering") {
    auto compiled = ai2d::compile_game_file(projectile_arena_manifest);
    REQUIRE(compiled);
    auto plan = *compiled;
    auto& scene = plan.scenes[plan.start_scene];
    scene.rules.clear();
    const auto symbol = static_cast<ai2d::SymbolId>(plan.symbols.size());
    plan.symbols.emplace_back("spawn_then_reset_pool");
    ai2d::GameRulePlan rule{};
    rule.symbol = symbol;
    rule.event.kind = ai2d::GameRuleEventKind::fixed_interval;
    rule.event.interval_ticks = 1U;
    ai2d::GameRuleActionPlan spawn{};
    spawn.kind = ai2d::GameRuleActionKind::spawn_from_pool;
    spawn.pool_index = 0U;
    spawn.pool_position_kind = ai2d::GamePoolSpawnPositionKind::constant;
    spawn.pool_position = {9.0F, 3.0F};
    spawn.has_velocity_override = true;
    spawn.velocity = {4.0F, 2.0F};
    spawn.has_rotation_override = true;
    spawn.rotation = 1.0F;
    spawn.has_lifetime = true;
    spawn.lifetime_ticks = 99U;
    rule.actions.push_back(spawn);
    ai2d::GameRuleActionPlan reset{};
    reset.kind = ai2d::GameRuleActionKind::reset_pool;
    reset.pool_index = 0U;
    rule.actions.push_back(reset);
    scene.rules.push_back(rule);
    plan.plan_hash = ai2d::compute_game_plan_hash(plan);
    REQUIRE(ai2d::validate_game_plan(plan));

    ai2d::GameRuntime runtime{};
    ai2d::GameRuntimeOptions options{};
    options.headless = true;
    options.load_saved_settings = false;
    REQUIRE(runtime.initialize(plan, options));
    std::vector<ai2d::GameActionInput> actions(plan.actions.size());
    const auto frame = runtime.run_exact_actions(actions, 1U);
    REQUIRE(frame);
    CHECK(frame->pool_acquire_successes == 1U);
    CHECK(frame->pool_resets == 1U);
    CHECK(frame->active_pooled_entities == 0U);
    CHECK(frame->pool_lifetime_checks == 0U);
}

TEST_CASE("Retained scene snapshots restore exact pool FIFO active order and lifetime state") {
    auto plan = ai2d::compile_game_file(timed_pickups_manifest);
    REQUIRE(plan);
    ai2d::GameRuntime runtime{};
    ai2d::GameRuntime reference_runtime{};
    ai2d::GameRuntimeOptions options{};
    options.headless = true;
    options.load_saved_settings = false;
    REQUIRE(runtime.initialize(*plan, options));
    REQUIRE(reference_runtime.initialize(*plan, options));
    std::vector<ai2d::GameActionInput> actions(plan->actions.size());
    auto before = reference_runtime.run_exact_actions(actions, 30U);
    REQUIRE(before);
    CHECK(before->active_pooled_entities == 1U);
    REQUIRE(runtime.run_exact_actions(actions, 29U));
    actions[2U].pressed = true;
    actions[2U].down = true;
    const auto paused = runtime.run_exact_actions(actions, 1U);
    REQUIRE(paused);
    CHECK(runtime.current_scene() == "pause");
    actions[2U].down = false;
    actions[2U].pressed = false;
    REQUIRE(runtime.run_exact_actions(actions, 1U));
    actions[2U].pressed = true;
    actions[2U].down = true;
    const auto restored = runtime.run_exact_actions(actions, 1U);
    REQUIRE(restored);
    CHECK(runtime.current_scene() == "game");
    CHECK(restored->active_pooled_entities == 1U);
    CHECK(restored->scene_state_checksum == before->scene_state_checksum);
}

TEST_CASE("Maximum pool capacity sustains recycle churn with zero tracked allocation") {
    auto compiled = ai2d::compile_game_file(projectile_arena_manifest);
    REQUIRE(compiled);
    auto plan = *compiled;
    auto& scene = plan.scenes[plan.start_scene];
    auto pooled_group = scene.spawn_groups[scene.pools[0U].spawn_group_index];
    pooled_group.count = 10'000U;
    pooled_group.active_count = 10'000U;
    pooled_group.placement.origin = {};
    pooled_group.placement.spacing = {};
    pooled_group.placement.columns = 10'000U;
    scene.spawn_groups.clear();
    scene.spawn_groups.push_back(pooled_group);
    scene.pools[0U].spawn_group_index = 0U;
    scene.world_capacity = 10'000U;
    scene.total_spawn_count = 10'000U;
    scene.max_colliders = 10'000U;
    scene.systems.clear();
    scene.collision_rules.clear();
    scene.ui.clear();
    scene.rules.clear();
    const auto symbol = static_cast<ai2d::SymbolId>(plan.symbols.size());
    plan.symbols.emplace_back("maximum_pool_churn");
    ai2d::GameRulePlan rule{};
    rule.symbol = symbol;
    rule.event.kind = ai2d::GameRuleEventKind::fixed_interval;
    rule.event.interval_ticks = 1U;
    ai2d::GameRuleActionPlan spawn{};
    spawn.kind = ai2d::GameRuleActionKind::spawn_from_pool;
    spawn.pool_index = 0U;
    spawn.pool_position_kind = ai2d::GamePoolSpawnPositionKind::initial;
    rule.actions.push_back(spawn);
    scene.rules.push_back(rule);
    plan.transitions.clear();
    plan.plan_hash = ai2d::compute_game_plan_hash(plan);
    REQUIRE(ai2d::validate_game_plan(plan));

    ai2d::GameRuntime runtime{};
    ai2d::GameRuntimeOptions options{};
    options.headless = true;
    options.load_saved_settings = false;
    REQUIRE(runtime.initialize(plan, options));
    std::vector<ai2d::GameActionInput> actions(plan.actions.size());
    REQUIRE(runtime.run_exact_actions(actions, 2U));
    ai2d::MeasuredAllocationScope measured{};
    const auto frame = runtime.run_exact_actions(actions, 1'000U);
    const auto allocations = measured.finish();
    REQUIRE(frame);
    CHECK(frame->pool_acquire_successes == 1'000U);
    CHECK(frame->pool_exhaustions == 1'000U);
    CHECK(frame->pool_recycled_slots == 1'000U);
    CHECK(frame->active_pooled_entities == 10'000U);
    CHECK(allocations.allocations == 0U);
    CHECK(allocations.bytes == 0U);
}

TEST_CASE("Moving contact runtime observes one persistent begin and one active separation end") {
    const auto plan = ai2d::compile_game_file(contact_course_manifest);
    REQUIRE(plan);
    ai2d::GameRuntime runtime{};
    ai2d::GameRuntimeOptions options{};
    options.headless = true;
    options.load_saved_settings = false;
    REQUIRE(runtime.initialize(*plan, options));
    std::vector<ai2d::GameActionInput> actions(plan->actions.size());

    ai2d::MeasuredAllocationScope measured{};
    const auto entered = runtime.run_exact_actions(actions, 56U);
    const auto persisted = runtime.run_exact_actions(actions, 25U);
    const auto exited = runtime.run_exact_actions(actions, 25U);
    const auto allocations = measured.finish();
    REQUIRE(entered);
    REQUIRE(persisted);
    REQUIRE(exited);
    CHECK(entered->collision.contact_begins == 1U);
    CHECK(entered->collision.contact_ends == 0U);
    CHECK(entered->collision.active_contact_pairs == 1U);
    CHECK(persisted->collision.contact_begins == 0U);
    CHECK(persisted->collision.contact_ends == 0U);
    CHECK(persisted->collision.active_contact_pairs == 1U);
    CHECK(exited->collision.contact_begins == 0U);
    CHECK(exited->collision.contact_ends == 1U);
    CHECK(exited->collision.active_contact_pairs == 0U);
    REQUIRE(runtime.state_value(0U));
    REQUIRE(runtime.state_value(1U));
    CHECK(*runtime.state_value(0U) == 1);
    CHECK(*runtime.state_value(1U) == 1);
    REQUIRE(runtime.entity_position(0U, 0U));
    REQUIRE(runtime.entity_velocity(0U, 0U));
    CHECK(runtime.entity_position(0U, 0U)->x > 4.0F);
    CHECK(runtime.entity_velocity(0U, 0U)->x == 6.0F);
    CHECK(allocations.allocations == 0U);
    CHECK(allocations.bytes == 0U);
}

TEST_CASE("Retained scene re-entry restores active contact identity without duplicate begin") {
    const auto compiled = ai2d::compile_game_file(contact_course_manifest);
    REQUIRE(compiled);
    auto plan = *compiled;
    auto pause = plan.scenes.front();
    pause.symbol = static_cast<ai2d::SymbolId>(plan.symbols.size());
    plan.symbols.emplace_back("pause");
    pause.max_contact_pairs = 0U;
    pause.systems.clear();
    pause.collision_rules.clear();
    pause.rules.clear();
    pause.ui.clear();
    plan.scenes.push_back(std::move(pause));
    plan.transitions.clear();
    ai2d::SceneTransitionPlan leave{};
    leave.from_scene = 0U;
    leave.condition.kind = ai2d::TransitionConditionKind::action_pressed;
    leave.condition.action = 0U;
    leave.to_scene = 1U;
    leave.reset_scene = false;
    plan.transitions.push_back(leave);
    auto return_to_course = leave;
    return_to_course.from_scene = 1U;
    return_to_course.to_scene = 0U;
    plan.transitions.push_back(return_to_course);
    plan.plan_hash = ai2d::compute_game_plan_hash(plan);
    REQUIRE(ai2d::validate_game_plan(plan));

    ai2d::GameRuntime runtime{};
    ai2d::GameRuntimeOptions options{};
    options.headless = true;
    options.load_saved_settings = false;
    REQUIRE(runtime.initialize(plan, options));
    std::vector<ai2d::GameActionInput> actions(plan.actions.size());
    const auto entered = runtime.run_exact_actions(actions, 56U);
    REQUIRE(entered);
    CHECK(entered->collision.contact_begins == 1U);
    const auto contact_checksum = runtime.contact_state_checksum();
    CHECK(contact_checksum != 14'695'981'039'346'656'037ULL);

    actions[0U].pressed = true;
    const auto left = runtime.run_exact_actions(actions, 1U);
    REQUIRE(left);
    CHECK(left->scene_changed);
    CHECK(runtime.current_scene() == "pause");
    actions[0U] = {};
    REQUIRE(runtime.run_exact_actions(actions, 1U));
    actions[0U].pressed = true;
    const auto returned = runtime.run_exact_actions(actions, 1U);
    REQUIRE(returned);
    CHECK(returned->scene_changed);
    CHECK(runtime.current_scene() == "course");
    CHECK(runtime.contact_state_checksum() == contact_checksum);

    actions[0U] = {};
    const auto persisted = runtime.run_exact_actions(actions, 1U);
    REQUIRE(persisted);
    CHECK(persisted->collision.contact_begins == 0U);
    CHECK(persisted->collision.contact_ends == 0U);
    REQUIRE(runtime.state_value(0U));
    CHECK(*runtime.state_value(0U) == 1);
}

TEST_CASE("reset_scene clears the active trigger contact set") {
    const auto compiled = ai2d::compile_game_file(contact_course_manifest);
    REQUIRE(compiled);
    auto plan = *compiled;
    plan.transitions.clear();
    ai2d::SceneTransitionPlan reset{};
    reset.from_scene = 0U;
    reset.condition.kind = ai2d::TransitionConditionKind::action_pressed;
    reset.condition.action = 0U;
    reset.to_scene = 0U;
    reset.reset_scene = true;
    plan.transitions.push_back(reset);
    plan.plan_hash = ai2d::compute_game_plan_hash(plan);
    REQUIRE(ai2d::validate_game_plan(plan));

    ai2d::GameRuntime runtime{};
    ai2d::GameRuntimeOptions options{};
    options.headless = true;
    options.load_saved_settings = false;
    REQUIRE(runtime.initialize(plan, options));
    std::vector<ai2d::GameActionInput> actions(plan.actions.size());
    REQUIRE(runtime.run_exact_actions(actions, 56U));
    CHECK(runtime.contact_state_checksum() != 14'695'981'039'346'656'037ULL);
    actions[0U].pressed = true;
    const auto reset_frame = runtime.run_exact_actions(actions, 1U);
    REQUIRE(reset_frame);
    CHECK(reset_frame->scene_changed);
    CHECK(runtime.contact_state_checksum() == 14'695'981'039'346'656'037ULL);
}

TEST_CASE("Direct moving pool contacts release endpoints and spawn at captured contact positions") {
    const auto plan = ai2d::compile_game_file(pool_siege_manifest);
    REQUIRE(plan);
    ai2d::GameRuntime runtime{};
    ai2d::GameRuntimeOptions options{};
    options.headless = true;
    options.load_saved_settings = false;
    REQUIRE(runtime.initialize(*plan, options));
    std::vector<ai2d::GameActionInput> actions(plan->actions.size());
    std::uint64_t contact_begins = 0U;
    std::uint64_t pool_releases = 0U;
    std::uint64_t acquire_successes = 0U;
    std::uint64_t linear_updates = 0U;

    bool frames_ok = true;
    for (std::uint32_t tick = 0U; tick < 150U; ++tick) {
        std::fill(actions.begin(), actions.end(), ai2d::GameActionInput{});
        if (tick == 0U) actions[1U].pressed = true;
        if (tick == 30U) actions[0U].pressed = true;
        if (tick == 60U) actions[2U].pressed = true;
        const auto frame = runtime.run_exact_actions(actions, 1U);
        if (!frame) {
            frames_ok = false;
            break;
        }
        contact_begins += frame->collision.contact_begins;
        pool_releases += frame->pool_releases;
        acquire_successes += frame->pool_acquire_successes;
        linear_updates += frame->linear_motion_updates;
    }
    REQUIRE(frames_ok);
    CHECK(runtime.current_scene() == "win");
    REQUIRE(runtime.state_value(0U));
    CHECK(*runtime.state_value(0U) == 3);
    CHECK(contact_begins == 3U);
    CHECK(pool_releases == 6U);
    // Three projectiles and the first two contact-position pickups acquire
    // slots; the third pickup intentionally exercises the two-slot skip pool.
    CHECK(acquire_successes == 5U);
    CHECK(linear_updates > 400U);
}

TEST_CASE("Pool release and immediate reuse invalidates later contacts from the same collision batch") {
    const auto compiled = ai2d::compile_game_file(pool_siege_manifest);
    REQUIRE(compiled);
    auto plan = *compiled;
    auto& scene = plan.scenes[plan.start_scene];
    scene.spawn_groups[1U].count = 1U;
    scene.spawn_groups[1U].active_count = 1U;
    scene.spawn_groups[1U].placement.origin = {};
    scene.spawn_groups[1U].velocity.linear = {};
    scene.spawn_groups[2U].count = 2U;
    scene.spawn_groups[2U].active_count = 2U;
    scene.spawn_groups[2U].placement.origin = {};
    scene.spawn_groups[2U].placement.spacing = {};
    scene.spawn_groups[2U].velocity.linear = {};
    scene.total_spawn_count = 6U;
    scene.collision_rules.resize(1U);
    scene.rules.clear();
    const auto symbol = static_cast<ai2d::SymbolId>(plan.symbols.size());
    plan.symbols.emplace_back("release_reuse_contact_identity");
    ai2d::GameRulePlan rule{};
    rule.symbol = symbol;
    rule.event.kind = ai2d::GameRuleEventKind::contact_begin;
    rule.event.collision_rule_index = 0U;
    ai2d::GameRuleActionPlan release{};
    release.kind = ai2d::GameRuleActionKind::release_to_pool;
    release.pool_index = 0U;
    release.target.kind = ai2d::GameRuleTargetKind::collision_a;
    rule.actions.push_back(release);
    ai2d::GameRuleActionPlan respawn{};
    respawn.kind = ai2d::GameRuleActionKind::spawn_from_pool;
    respawn.pool_index = 0U;
    respawn.pool_position_kind = ai2d::GamePoolSpawnPositionKind::constant;
    respawn.pool_position = {};
    rule.actions.push_back(respawn);
    ai2d::GameRuleActionPlan score{};
    score.kind = ai2d::GameRuleActionKind::add_int_state;
    score.state_index = 0U;
    score.value = 1;
    rule.actions.push_back(score);
    scene.rules.push_back(rule);
    plan.transitions.clear();
    plan.plan_hash = ai2d::compute_game_plan_hash(plan);
    REQUIRE(ai2d::validate_game_plan(plan));

    ai2d::GameRuntime runtime{};
    ai2d::GameRuntimeOptions options{};
    options.headless = true;
    options.load_saved_settings = false;
    REQUIRE(runtime.initialize(plan, options));
    std::vector<ai2d::GameActionInput> actions(plan.actions.size());
    const auto frame = runtime.run_exact_actions(actions, 1U);
    REQUIRE(frame);
    CHECK(frame->collision.contact_begins == 2U);
    CHECK(frame->stale_contact_events == 1U);
    CHECK(frame->rule_executions == 1U);
    CHECK(frame->pool_releases == 1U);
    CHECK(frame->pool_acquire_successes == 1U);
    REQUIRE(runtime.state_value(0U));
    CHECK(*runtime.state_value(0U) == 1);
}
