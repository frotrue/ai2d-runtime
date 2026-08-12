#include "ai2d/foundation/allocation_tracker.hpp"
#include "ai2d/runtime/game_runtime.hpp"
#include "ai2d/runtime/runtime.hpp"
#include "ai2d/scenario/game.hpp"
#include "ai2d/scenario/scenario.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

namespace {

const std::filesystem::path scenario_root{AI2D_SOURCE_DIR "/tests/fixtures/scenarios/valid"};
const std::filesystem::path snake_manifest{AI2D_SOURCE_DIR "/samples/snake/game.json"};

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
