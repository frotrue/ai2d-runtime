#include "ai2d/runtime/game_runtime.hpp"
#include "ai2d/scenario/game.hpp"
#include "ai2d/foundation/allocation_tracker.hpp"

#if defined(AI2D_ENABLE_GPU)
#include "ai2d/platform/assets.hpp"
#include "ai2d/platform/settings.hpp"
#endif

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <thread>

namespace {

std::filesystem::path breakout_manifest() {
    return std::filesystem::path{AI2D_SOURCE_DIR} / "samples" / "breakout" / "game.json";
}

std::filesystem::path snake_manifest() {
    return std::filesystem::path{AI2D_SOURCE_DIR} / "samples" / "snake" / "game.json";
}

std::filesystem::path grid_collector_manifest() {
    return std::filesystem::path{AI2D_SOURCE_DIR} / "samples" / "grid_collector" / "game.json";
}

ai2d::InputSnapshot pressed(const ai2d::InputKey key) {
    ai2d::InputSnapshot input{};
    input.keys[static_cast<std::uint8_t>(key)] = true;
    input.pressed_keys[static_cast<std::uint8_t>(key)] = true;
    input.drawable_extent = {1280U, 720U};
    return input;
}

std::filesystem::path copy_game_fixture(const std::filesystem::path& source, const std::string_view name) {
    const auto root = std::filesystem::temp_directory_path() / std::string{name};
    std::error_code error{};
    std::filesystem::remove_all(root, error);
    error.clear();
    std::filesystem::copy(
        source.parent_path(), root,
        std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing, error);
    REQUIRE_FALSE(error);
    return root;
}

void replace_text_in_file(
    const std::filesystem::path& path, const std::string_view before, const std::string_view after) {
    std::ifstream input{path, std::ios::binary};
    REQUIRE(input);
    std::string text{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    const auto position = text.find(before);
    REQUIRE(position != std::string::npos);
    text.replace(position, before.size(), after);
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    REQUIRE(output);
    output << text;
}

} // namespace

TEST_CASE("GameManifest 0.2 compiles all declarative Breakout scenes") {
    const auto plan = ai2d::compile_game_file(breakout_manifest());
    REQUIRE(plan);
    CHECK(plan->scenes.size() == 5U);
    CHECK(plan->assets.size() == 4U);
    CHECK(plan->scenes[2].total_spawn_count == 66U);
    CHECK(plan->scenes[2].collision_rules.size() == 4U);
    REQUIRE(plan->scenes[0].ui.size() >= 3U);
    CHECK(plan->scenes[0].ui[1].text_color == plan->scenes[0].ui[1].color);
    CHECK(plan->scenes[0].ui[2].text_color == ai2d::Color{});
    CHECK(plan->plan_hash == ai2d::compute_game_plan_hash(*plan));
    CHECK(plan->source_hash == 9083610792174461301ULL);
    CHECK(plan->plan_hash == 6721347758608948258ULL);
    CHECK(plan->schema_version_text() == "0.2");
    CHECK(ai2d::validate_game_plan(*plan));

    auto invalid_plan = *plan;
    invalid_plan.scenes[0].ui[2].text_color.r = 2.0F;
    invalid_plan.plan_hash = ai2d::compute_game_plan_hash(invalid_plan);
    CHECK_FALSE(ai2d::validate_game_plan(invalid_plan));
}

TEST_CASE("GameManifest 0.3 compiles typed grids systems and ordered rules") {
    const auto plan = ai2d::compile_game_file(snake_manifest());
    REQUIRE(plan);
    CHECK(plan->schema_version == ai2d::GameSchemaVersion::v0_3);
    CHECK(plan->schema_version_text() == "0.3");
    REQUIRE_FALSE(plan->scenes.empty());
    const auto gameplay = std::find_if(plan->scenes.begin(), plan->scenes.end(), [&](const ai2d::GameScenePlan& scene) {
        return plan->symbol(scene.symbol) == "game";
    });
    REQUIRE(gameplay != plan->scenes.end());
    CHECK_FALSE(gameplay->grids.empty());
    CHECK_FALSE(gameplay->rules.empty());
    CHECK(std::any_of(gameplay->systems.begin(), gameplay->systems.end(), [](const ai2d::GameSystemPlan& system) {
        return system.operation == ai2d::GameOperationId::grid_motion;
    }));
    CHECK(ai2d::validate_game_plan(*plan));
}

TEST_CASE("SceneSpec 0.3 parsers reject sibling variant fields and missing required fields") {
    std::error_code error{};

    const auto reverse_root = copy_game_fixture(snake_manifest(), "ai2d-v03-required-prevent-reverse");
    replace_text_in_file(reverse_root / "scenes" / "game.json", R"(,"prevent_reverse":true)", "");
    CHECK_FALSE(ai2d::compile_game_file(reverse_root / "game.json"));
    std::filesystem::remove_all(reverse_root, error);
    CHECK_FALSE(error);

    const auto system_root = copy_game_fixture(snake_manifest(), "ai2d-v03-system-sibling-field");
    replace_text_in_file(
        system_root / "scenes" / "game.json", R"("prevent_reverse":true})",
        R"("prevent_reverse":true,"velocity":[1,0]})");
    CHECK_FALSE(ai2d::compile_game_file(system_root / "game.json"));
    std::filesystem::remove_all(system_root, error);
    CHECK_FALSE(error);

    const auto action_root = copy_game_fixture(snake_manifest(), "ai2d-v03-action-sibling-field");
    replace_text_in_file(
        action_root / "scenes" / "game.json", R"("kind":"add_int_state","state":"score","value":10})",
        R"("kind":"add_int_state","state":"score","value":10,"asset":"font"})");
    CHECK_FALSE(ai2d::compile_game_file(action_root / "game.json"));
    std::filesystem::remove_all(action_root, error);
    CHECK_FALSE(error);

    const auto missing_system_root = copy_game_fixture(snake_manifest(), "ai2d-v03-system-missing-required");
    replace_text_in_file(
        missing_system_root / "scenes" / "game.json", R"(,"initial_direction":"right")", "");
    CHECK_FALSE(ai2d::compile_game_file(missing_system_root / "game.json"));
    std::filesystem::remove_all(missing_system_root, error);
    CHECK_FALSE(error);

    const auto missing_action_root = copy_game_fixture(snake_manifest(), "ai2d-v03-action-missing-required");
    replace_text_in_file(
        missing_action_root / "scenes" / "game.json", R"(,"value":10})", "}");
    CHECK_FALSE(ai2d::compile_game_file(missing_action_root / "game.json"));
    std::filesystem::remove_all(missing_action_root, error);
    CHECK_FALSE(error);
}

TEST_CASE("GameManifest integer parsing rejects unsigned values above int32 without narrowing") {
    const auto root = copy_game_fixture(snake_manifest(), "ai2d-v03-unsigned-int32-overflow");
    replace_text_in_file(root / "game.json", R"("initial":0,"min":0,"max":9999)",
                         R"("initial":18446744073709551615,"min":-10,"max":9999)");
    const auto compiled = ai2d::compile_game_file(root / "game.json");
    REQUIRE_FALSE(compiled);
    CHECK(compiled.error().code == ai2d::DiagnosticCode::game_manifest_invalid);
    std::error_code error{};
    std::filesystem::remove_all(root, error);
    CHECK_FALSE(error);
}

TEST_CASE("SceneSpec and public GamePlan validation reject overflowing derived positions") {
    const auto root = copy_game_fixture(breakout_manifest(), "ai2d-derived-position-overflow");
    replace_text_in_file(root / "scenes" / "game.json", R"("spacing": [3, -0.78])",
                         R"("spacing": [3, 3.4028234e38])");
    CHECK_FALSE(ai2d::compile_game_file(root / "game.json"));
    std::error_code error{};
    std::filesystem::remove_all(root, error);
    CHECK_FALSE(error);

    const auto compiled = ai2d::compile_game_file(snake_manifest());
    REQUIRE(compiled);
    const auto gameplay = std::find_if(
        compiled->scenes.begin(), compiled->scenes.end(), [&](const ai2d::GameScenePlan& scene) {
            return compiled->symbol(scene.symbol) == "game";
        });
    REQUIRE(gameplay != compiled->scenes.end());
    const auto gameplay_index = static_cast<std::size_t>(std::distance(compiled->scenes.begin(), gameplay));

    const auto breakout = ai2d::compile_game_file(breakout_manifest());
    REQUIRE(breakout);
    auto invalid_spawn = *breakout;
    invalid_spawn.scenes[2].spawn_groups.back().placement.spacing.y = std::numeric_limits<float>::max();
    invalid_spawn.plan_hash = ai2d::compute_game_plan_hash(invalid_spawn);
    CHECK_FALSE(ai2d::validate_game_plan(invalid_spawn));

    auto invalid_grid = *compiled;
    auto& grid = invalid_grid.scenes[gameplay_index].grids.front();
    grid.columns = 1'000'000U;
    grid.rows = 1U;
    grid.cell_size.x = std::numeric_limits<float>::max();
    invalid_grid.plan_hash = ai2d::compute_game_plan_hash(invalid_grid);
    CHECK_FALSE(ai2d::validate_game_plan(invalid_grid));
}

TEST_CASE("GamePlan validation requires uniform collider-group velocity and reflect capability") {
    const auto compiled = ai2d::compile_game_file(breakout_manifest());
    REQUIRE(compiled);
    auto rejected = [](ai2d::GamePlan invalid) {
        invalid.plan_hash = ai2d::compute_game_plan_hash(invalid);
        CHECK_FALSE(ai2d::validate_game_plan(invalid));
    };

    auto partial_velocity = *compiled;
    auto& scene = partial_velocity.scenes[2];
    auto paddle = std::find_if(
        scene.spawn_groups.begin(), scene.spawn_groups.end(), [&](const ai2d::GameSpawnGroupPlan& group) {
            return partial_velocity.symbol(group.symbol) == "paddle";
        });
    REQUIRE(paddle != scene.spawn_groups.end());
    auto sibling = *paddle;
    sibling.symbol = static_cast<ai2d::SymbolId>(partial_velocity.symbols.size());
    partial_velocity.symbols.push_back("paddle_without_velocity");
    sibling.has_velocity = false;
    scene.spawn_groups.push_back(sibling);
    ++scene.total_spawn_count;
    ++scene.world_capacity;
    rejected(std::move(partial_velocity));

    auto invalid_reflect = *compiled;
    auto& reflect_scene = invalid_reflect.scenes[2];
    REQUIRE_FALSE(reflect_scene.collision_rules.empty());
    reflect_scene.collision_rules.front().reactions.front().target = ai2d::GameReactionTarget::b;
    rejected(std::move(invalid_reflect));
}

TEST_CASE("SceneSpec rejects partial collider-group velocity and incapable reflect targets") {
    std::error_code error{};
    const auto velocity_root = copy_game_fixture(breakout_manifest(), "ai2d-partial-collider-group-velocity");
    replace_text_in_file(
        velocity_root / "scenes" / "game.json", R"("spawn_groups": [)",
        R"("spawn_groups": [{"id":"paddle_shadow","count":1,"placement":{"kind":"grid","origin":[0,-7],"spacing":[1,1],"columns":1},"collider":{"half_extent":[1,0.3],"group":"paddle","body":"kinematic"}},)");
    CHECK_FALSE(ai2d::compile_game_file(velocity_root / "game.json"));
    std::filesystem::remove_all(velocity_root, error);
    CHECK_FALSE(error);

    const auto reflect_root = copy_game_fixture(breakout_manifest(), "ai2d-incapable-reflect-target");
    replace_text_in_file(
        reflect_root / "scenes" / "game.json", R"("kind": "reflect", "target": "a")",
        R"("kind": "reflect", "target": "b")");
    CHECK_FALSE(ai2d::compile_game_file(reflect_root / "game.json"));
    std::filesystem::remove_all(reflect_root, error);
    CHECK_FALSE(error);
}

TEST_CASE("SceneSpec and public GamePlan validation reject multiple follow owners") {
    const auto root = copy_game_fixture(snake_manifest(), "ai2d-v03-multiple-follow-owners");
    replace_text_in_file(
        root / "scenes" / "game.json", R"({"id":"tail_follow","operation":"follow_transform_chain","leader":"head","followers":"tail","motion_system":"snake_motion"})",
        R"({"id":"tail_follow","operation":"follow_transform_chain","leader":"head","followers":"tail","motion_system":"snake_motion"},{"id":"tail_follow_duplicate","operation":"follow_transform_chain","leader":"head","followers":"tail","motion_system":"snake_motion"})");
    CHECK_FALSE(ai2d::compile_game_file(root / "game.json"));
    std::error_code error{};
    std::filesystem::remove_all(root, error);
    CHECK_FALSE(error);

    const auto compiled = ai2d::compile_game_file(snake_manifest());
    REQUIRE(compiled);
    auto duplicate = *compiled;
    auto gameplay = std::find_if(
        duplicate.scenes.begin(), duplicate.scenes.end(), [&](const ai2d::GameScenePlan& scene) {
            return duplicate.symbol(scene.symbol) == "game";
        });
    REQUIRE(gameplay != duplicate.scenes.end());
    auto follow = std::find_if(
        gameplay->systems.begin(), gameplay->systems.end(), [](const ai2d::GameSystemPlan& system) {
            return system.operation == ai2d::GameOperationId::follow_transform_chain;
        });
    REQUIRE(follow != gameplay->systems.end());
    auto extra = *follow;
    extra.symbol = static_cast<ai2d::SymbolId>(duplicate.symbols.size());
    duplicate.symbols.push_back("second_follow_owner");
    gameplay->systems.push_back(extra);
    duplicate.plan_hash = ai2d::compute_game_plan_hash(duplicate);
    CHECK_FALSE(ai2d::validate_game_plan(duplicate));
}

TEST_CASE("GamePlan 0.3 public validation enforces fixed-tick upper bounds") {
    const auto compiled = ai2d::compile_game_file(snake_manifest());
    REQUIRE(compiled);
    const auto gameplay = std::find_if(
        compiled->scenes.begin(), compiled->scenes.end(), [&](const ai2d::GameScenePlan& scene) {
            return compiled->symbol(scene.symbol) == "game";
        });
    REQUIRE(gameplay != compiled->scenes.end());
    const auto gameplay_index = static_cast<std::size_t>(std::distance(compiled->scenes.begin(), gameplay));

    const auto rejected = [](ai2d::GamePlan invalid) {
        invalid.plan_hash = ai2d::compute_game_plan_hash(invalid);
        CHECK_FALSE(ai2d::validate_game_plan(invalid));
    };

    auto maximum_intervals = *compiled;
    auto& maximum_systems = maximum_intervals.scenes[gameplay_index].systems;
    const auto maximum_grid_motion = std::find_if(
        maximum_systems.begin(), maximum_systems.end(), [](const ai2d::GameSystemPlan& system) {
            return system.operation == ai2d::GameOperationId::grid_motion;
        });
    REQUIRE(maximum_grid_motion != maximum_systems.end());
    maximum_grid_motion->step_interval_ticks = 1'000'000U;
    REQUIRE_FALSE(maximum_intervals.scenes[gameplay_index].rules.empty());
    auto& maximum_interval_event = maximum_intervals.scenes[gameplay_index].rules.front().event;
    maximum_interval_event.kind = ai2d::GameRuleEventKind::fixed_interval;
    maximum_interval_event.interval_ticks = 1'000'000U;
    maximum_intervals.plan_hash = ai2d::compute_game_plan_hash(maximum_intervals);
    CHECK(ai2d::validate_game_plan(maximum_intervals));

    auto oversized_grid_step = *compiled;
    auto& grid_systems = oversized_grid_step.scenes[gameplay_index].systems;
    const auto grid_motion = std::find_if(
        grid_systems.begin(), grid_systems.end(), [](const ai2d::GameSystemPlan& system) {
            return system.operation == ai2d::GameOperationId::grid_motion;
        });
    REQUIRE(grid_motion != grid_systems.end());
    grid_motion->step_interval_ticks = 1'000'001U;
    rejected(std::move(oversized_grid_step));

    auto oversized_fixed_interval = *compiled;
    REQUIRE_FALSE(oversized_fixed_interval.scenes[gameplay_index].rules.empty());
    auto& interval_event = oversized_fixed_interval.scenes[gameplay_index].rules.front().event;
    interval_event.kind = ai2d::GameRuleEventKind::fixed_interval;
    interval_event.interval_ticks = 1'000'001U;
    rejected(std::move(oversized_fixed_interval));
}

TEST_CASE("GamePlan 0.3 public validation checks every grid reserve while allowing collider-free followers") {
    const auto compiled = ai2d::compile_game_file(snake_manifest());
    REQUIRE(compiled);
    const auto gameplay = std::find_if(
        compiled->scenes.begin(), compiled->scenes.end(), [&](const ai2d::GameScenePlan& scene) {
            return compiled->symbol(scene.symbol) == "game";
        });
    REQUIRE(gameplay != compiled->scenes.end());
    const auto gameplay_index = static_cast<std::size_t>(std::distance(compiled->scenes.begin(), gameplay));

    const auto rejected = [](ai2d::GamePlan invalid) {
        invalid.plan_hash = ai2d::compute_game_plan_hash(invalid);
        CHECK_FALSE(ai2d::validate_game_plan(invalid));
    };

    const auto motion = std::find_if(
        gameplay->systems.begin(), gameplay->systems.end(), [](const ai2d::GameSystemPlan& system) {
            return system.operation == ai2d::GameOperationId::grid_motion;
        });
    const auto follow = std::find_if(
        gameplay->systems.begin(), gameplay->systems.end(), [](const ai2d::GameSystemPlan& system) {
            return system.operation == ai2d::GameOperationId::follow_transform_chain;
        });
    REQUIRE(motion != gameplay->systems.end());
    REQUIRE(follow != gameplay->systems.end());

    auto misaligned_motion_reserve = *compiled;
    misaligned_motion_reserve.scenes[gameplay_index]
        .spawn_groups[motion->spawn_group_index]
        .transform.position_offset.x += 0.25F;
    rejected(std::move(misaligned_motion_reserve));

    auto misaligned_follower_reserve = *compiled;
    misaligned_follower_reserve.scenes[gameplay_index]
        .spawn_groups[follow->follower_group_index]
        .transform.position_offset.y += 0.25F;
    rejected(std::move(misaligned_follower_reserve));

    auto collider_free_followers = *compiled;
    auto& collider_free_scene = collider_free_followers.scenes[gameplay_index];
    auto& follower_group = collider_free_scene.spawn_groups[follow->follower_group_index];
    const auto follower_collider_group = follower_group.collider.group;
    follower_group.has_collider = false;
    const auto removed_collision = std::find_if(
        collider_free_scene.collision_rules.begin(), collider_free_scene.collision_rules.end(),
        [&](const ai2d::GameCollisionRulePlan& rule) {
            return rule.group_a == follower_collider_group || rule.group_b == follower_collider_group;
        });
    REQUIRE(removed_collision != collider_free_scene.collision_rules.end());
    const auto removed_collision_index =
        static_cast<std::uint32_t>(std::distance(collider_free_scene.collision_rules.begin(), removed_collision));
    collider_free_scene.collision_rules.erase(removed_collision);
    std::erase_if(collider_free_scene.rules, [&](const ai2d::GameRulePlan& rule) {
        return rule.event.kind == ai2d::GameRuleEventKind::collision &&
               rule.event.collision_rule_index == removed_collision_index;
    });
    for (auto& rule : collider_free_scene.rules) {
        if (rule.event.kind == ai2d::GameRuleEventKind::collision &&
            rule.event.collision_rule_index > removed_collision_index) {
            --rule.event.collision_rule_index;
        }
    }
    collider_free_followers.plan_hash = ai2d::compute_game_plan_hash(collider_free_followers);
    CHECK(ai2d::validate_game_plan(collider_free_followers));

    const auto collector = ai2d::compile_game_file(grid_collector_manifest());
    REQUIRE(collector);
    auto misaligned_occupancy_reserve = *collector;
    const auto collector_gameplay = std::find_if(
        misaligned_occupancy_reserve.scenes.begin(), misaligned_occupancy_reserve.scenes.end(),
        [&](const ai2d::GameScenePlan& scene) {
            return misaligned_occupancy_reserve.symbol(scene.symbol) == "game";
        });
    REQUIRE(collector_gameplay != misaligned_occupancy_reserve.scenes.end());
    const auto hazards = std::find_if(
        collector_gameplay->spawn_groups.begin(), collector_gameplay->spawn_groups.end(),
        [&](const ai2d::GameSpawnGroupPlan& group) {
            return misaligned_occupancy_reserve.symbol(group.symbol) == "hazards";
        });
    REQUIRE(hazards != collector_gameplay->spawn_groups.end());
    hazards->transform.position_offset.x += 0.25F;
    rejected(std::move(misaligned_occupancy_reserve));
}

TEST_CASE("GamePlan 0.3 permits 64 grids and one million cells per grid without an aggregate cap") {
    const auto compiled = ai2d::compile_game_file(snake_manifest());
    REQUIRE(compiled);
    const auto gameplay = std::find_if(
        compiled->scenes.begin(), compiled->scenes.end(), [&](const ai2d::GameScenePlan& scene) {
            return compiled->symbol(scene.symbol) == "game";
        });
    REQUIRE(gameplay != compiled->scenes.end());
    const auto gameplay_index = static_cast<std::size_t>(std::distance(compiled->scenes.begin(), gameplay));

    const auto append_grid = [](ai2d::GamePlan& plan, ai2d::GameScenePlan& scene, const std::string& name,
                                const std::uint32_t columns, const std::uint32_t rows) {
        ai2d::GameGridPlan grid{};
        grid.symbol = static_cast<ai2d::SymbolId>(plan.symbols.size());
        plan.symbols.push_back(name);
        grid.first_cell_center = {0.0F, 0.0F};
        grid.cell_size = {1.0F, 1.0F};
        grid.columns = columns;
        grid.rows = rows;
        scene.grids.push_back(grid);
    };

    auto independent_million_cell_grids = *compiled;
    auto& million_scene = independent_million_cell_grids.scenes[gameplay_index];
    million_scene.grids.front().columns = 1'000U;
    million_scene.grids.front().rows = 1'000U;
    append_grid(independent_million_cell_grids, million_scene, "second_million_cell_grid", 1'000U, 1'000U);
    independent_million_cell_grids.plan_hash = ai2d::compute_game_plan_hash(independent_million_cell_grids);
    CHECK(ai2d::validate_game_plan(independent_million_cell_grids));

    auto oversized_grid = independent_million_cell_grids;
    oversized_grid.scenes[gameplay_index].grids.back().columns = 1'001U;
    oversized_grid.plan_hash = ai2d::compute_game_plan_hash(oversized_grid);
    CHECK_FALSE(ai2d::validate_game_plan(oversized_grid));

    auto sixty_four_grids = *compiled;
    auto& grid_limit_scene = sixty_four_grids.scenes[gameplay_index];
    for (std::size_t index = grid_limit_scene.grids.size(); index < ai2d::GameScenePlan::max_grids; ++index) {
        append_grid(sixty_four_grids, grid_limit_scene, "extra_grid_" + std::to_string(index), 1U, 1U);
    }
    sixty_four_grids.plan_hash = ai2d::compute_game_plan_hash(sixty_four_grids);
    CHECK(ai2d::validate_game_plan(sixty_four_grids));

    append_grid(sixty_four_grids, grid_limit_scene, "sixty_fifth_grid", 1U, 1U);
    sixty_four_grids.plan_hash = ai2d::compute_game_plan_hash(sixty_four_grids);
    CHECK_FALSE(ai2d::validate_game_plan(sixty_four_grids));
}

TEST_CASE("Game input script resolves logical actions and rejects duplicate same-tick actions") {
    const auto game = ai2d::compile_game_file(snake_manifest());
    REQUIRE(game);
    const auto root = std::filesystem::temp_directory_path() / "ai2d-v03-input-script-test";
    std::error_code error{};
    std::filesystem::create_directories(root, error);
    REQUIRE_FALSE(error);
    const auto input = root / "input.json";
    {
        std::ofstream stream{input, std::ios::binary | std::ios::trunc};
        stream << R"({"schema_version":"1","events":[{"tick":0,"action":"up","kind":"tap"},{"tick":12,"action":"right","kind":"press"}]})";
    }
    const auto compiled = ai2d::compile_game_input_file(input, *game);
    REQUIRE(compiled);
    REQUIRE(compiled->events.size() == 2U);
    CHECK(compiled->last_tick == 12U);
    CHECK(compiled->events[0].kind == ai2d::GameInputEventKind::tap);

    {
        std::ofstream stream{input, std::ios::binary | std::ios::trunc};
        stream << R"({"schema_version":"1","events":[{"tick":2,"action":"up","kind":"press"},{"tick":2,"action":"up","kind":"release"}]})";
    }
    CHECK_FALSE(ai2d::compile_game_input_file(input, *game));
    std::filesystem::remove_all(root, error);
    CHECK_FALSE(error);
}

TEST_CASE("GameManifest rejects a referenced scene with a mixed schema version") {
    const auto root = std::filesystem::temp_directory_path() / "ai2d-v03-mixed-version-test";
    std::error_code error{};
    std::filesystem::remove_all(root, error);
    error.clear();
    std::filesystem::copy(
        snake_manifest().parent_path(), root,
        std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing, error);
    REQUIRE_FALSE(error);
    const auto scene = root / "scenes" / "game.json";
    std::ifstream input{scene, std::ios::binary};
    REQUIRE(input);
    std::string text{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    input.close();
    const auto spaced_version = text.find("\"schema_version\": \"0.3\"");
    const auto compact_version = text.find("\"schema_version\":\"0.3\"");
    const auto version = spaced_version != std::string::npos ? spaced_version : compact_version;
    REQUIRE(version != std::string::npos);
    const auto original_size = spaced_version != std::string::npos
                                   ? std::string_view{"\"schema_version\": \"0.3\""}.size()
                                   : std::string_view{"\"schema_version\":\"0.3\""}.size();
    text.replace(version, original_size, "\"schema_version\":\"0.2\"");
    std::ofstream output{scene, std::ios::binary | std::ios::trunc};
    output << text;
    output.close();
    const auto compiled = ai2d::compile_game_file(root / "game.json");
    REQUIRE_FALSE(compiled);
    CHECK(compiled.error().code == ai2d::DiagnosticCode::game_scene_invalid);
    std::filesystem::remove_all(root, error);
    CHECK_FALSE(error);
}

TEST_CASE("GameRuntime handles UI scene transitions and FPS settings headlessly") {
    const auto plan = ai2d::compile_game_file(breakout_manifest());
    REQUIRE(plan);
    ai2d::GameRuntime runtime{};
    ai2d::GameRuntimeOptions options{};
    options.headless = true;
    options.load_saved_settings = false;
    REQUIRE(runtime.initialize(*plan, options));
    CHECK(runtime.current_scene() == "menu");

    ai2d::InputSnapshot settings_click{};
    settings_click.mouse_x = 640.0F;
    settings_click.mouse_y = 415.0F;
    settings_click.mouse_left = true;
    settings_click.mouse_left_pressed = true;
    settings_click.drawable_extent = {1280U, 720U};
    REQUIRE(runtime.run_exact(settings_click));
    CHECK(runtime.current_scene() == "settings");

    ai2d::InputSnapshot fps_click{};
    fps_click.mouse_x = 820.0F;
    fps_click.mouse_y = 235.0F;
    fps_click.mouse_left = true;
    fps_click.mouse_left_pressed = true;
    fps_click.drawable_extent = {1280U, 720U};
    REQUIRE(runtime.run_exact(fps_click));
    CHECK(runtime.requested_fps_cap() == ai2d::RenderFpsCap::fps_120);

    REQUIRE(runtime.run_exact(pressed(ai2d::InputKey::escape)));
    CHECK(runtime.current_scene() == "menu");
}

TEST_CASE("Declarative Breakout launches and scores through continuous collisions") {
    const auto plan = ai2d::compile_game_file(breakout_manifest());
    REQUIRE(plan);
    ai2d::GameRuntime runtime{};
    ai2d::GameRuntimeOptions options{};
    options.headless = true;
    options.load_saved_settings = false;
    REQUIRE(runtime.initialize(*plan, options));

    REQUIRE(runtime.run_exact(pressed(ai2d::InputKey::space)));
    REQUIRE(runtime.current_scene() == "game");
    REQUIRE(runtime.run_exact(pressed(ai2d::InputKey::space)));
    for (std::uint32_t tick = 0U; tick < 600U && runtime.current_scene() == "game"; ++tick) {
        REQUIRE(runtime.run_exact({}));
    }
    const auto score = runtime.state_value("score");
    REQUIRE(score);
    CHECK(*score >= 100);
}

TEST_CASE("Mutable GamePlan hashing is safe and runtime validation rejects compiler-boundary bypasses") {
    const auto compiled = ai2d::compile_game_file(breakout_manifest());
    REQUIRE(compiled);
    ai2d::GameRuntimeOptions options{};
    options.headless = true;
    options.load_saved_settings = false;

    const auto rejected = [&](ai2d::GamePlan invalid) {
        invalid.plan_hash = ai2d::compute_game_plan_hash(invalid);
        CHECK_FALSE(ai2d::validate_game_plan(invalid));
        ai2d::GameRuntime runtime{};
        CHECK_FALSE(runtime.initialize(invalid, options));
        CHECK_FALSE(runtime.initialized());
    };

    auto invalid_keys = *compiled;
    invalid_keys.actions[0].key_count = std::numeric_limits<std::uint32_t>::max();
    rejected(std::move(invalid_keys));

    auto invalid_capacity = *compiled;
    invalid_capacity.scenes[0].world_capacity = ai2d::GameScenePlan::max_world_capacity + 1U;
    rejected(std::move(invalid_capacity));

    auto invalid_collision_capacity = *compiled;
    invalid_collision_capacity.scenes[0].max_grid_references =
        ai2d::GameScenePlan::max_collision_capacity + 1U;
    rejected(std::move(invalid_collision_capacity));

    auto invalid_numeric = *compiled;
    invalid_numeric.scenes[2].spawn_groups[0].placement.origin.x =
        std::numeric_limits<float>::quiet_NaN();
    rejected(std::move(invalid_numeric));

    auto invalid_font = *compiled;
    invalid_font.assets.back().line_height = std::numeric_limits<float>::infinity();
    rejected(std::move(invalid_font));

    auto invalid_fps = *compiled;
    invalid_fps.window.default_fps = static_cast<ai2d::RenderFpsCap>(255U);
    rejected(std::move(invalid_fps));

    auto invalid_key = *compiled;
    invalid_key.actions[0].keys[0] = static_cast<ai2d::GameKey>(255U);
    rejected(std::move(invalid_key));

    auto invalid_system = *compiled;
    const auto axis = std::find_if(
        invalid_system.scenes[2].systems.begin(),
        invalid_system.scenes[2].systems.end(),
        [](const ai2d::GameSystemPlan& system) {
            return system.operation == ai2d::GameOperationId::axis_control;
        });
    REQUIRE(axis != invalid_system.scenes[2].systems.end());
    axis->speed = std::numeric_limits<float>::infinity();
    rejected(std::move(invalid_system));

    auto invalid_reaction = *compiled;
    invalid_reaction.scenes[2].collision_rules[0].reactions[0].kind =
        static_cast<ai2d::GameReactionKind>(255U);
    rejected(std::move(invalid_reaction));

    auto invalid_transition = *compiled;
    invalid_transition.transitions[0].condition.kind =
        static_cast<ai2d::TransitionConditionKind>(255U);
    rejected(std::move(invalid_transition));

    auto oversized_ui = *compiled;
    oversized_ui.scenes[0].ui.resize(ai2d::GameScenePlan::max_ui_elements + 1U);
    rejected(std::move(oversized_ui));
}

TEST_CASE("GamePlan validation pins the exact renderer submission boundary") {
    const auto compiled = ai2d::compile_game_file(breakout_manifest());
    REQUIRE(compiled);
    auto plan = *compiled;
    auto text = std::find_if(plan.scenes[0].ui.begin(), plan.scenes[0].ui.end(), [](const ai2d::UiElementPlan& element) {
        return element.kind == ai2d::UiElementKind::text;
    });
    REQUIRE(text != plan.scenes[0].ui.end());
    const auto text_symbol = text->text;
    plan.scenes[0].ui = {*text};
    plan.symbols[text_symbol] = std::string(ai2d::GamePlan::max_render_submissions, '0');
    plan.plan_hash = ai2d::compute_game_plan_hash(plan);
    CHECK(ai2d::validate_game_plan(plan));

    plan.symbols[text_symbol].push_back('0');
    plan.plan_hash = ai2d::compute_game_plan_hash(plan);
    CHECK_FALSE(ai2d::validate_game_plan(plan));
}

TEST_CASE("Gameplay input edges survive zero ticks and are consumed by one catch-up tick") {
    const auto plan = ai2d::compile_game_file(breakout_manifest());
    REQUIRE(plan);
    ai2d::GameRuntime runtime{};
    ai2d::GameRuntimeOptions options{};
    options.headless = true;
    options.load_saved_settings = false;
    REQUIRE(runtime.initialize(*plan, options));
    REQUIRE(runtime.run_exact(pressed(ai2d::InputKey::space), 1U));
    REQUIRE(runtime.current_scene() == "game");

    const auto zero_tick = runtime.advance(pressed(ai2d::InputKey::space), 0.0);
    REQUIRE(zero_tick);
    CHECK(zero_tick->input_edge_ticks == 0U);
    const auto consumed_later = runtime.run_exact({}, 1U);
    REQUIRE(consumed_later);
    CHECK(consumed_later->input_edge_ticks == 1U);

    const auto catch_up = runtime.run_exact(pressed(ai2d::InputKey::space), 3U);
    REQUIRE(catch_up);
    CHECK(catch_up->fixed_ticks == 3U);
    CHECK(catch_up->input_edge_ticks == 1U);
}

TEST_CASE("reset_scene controls retained scene re-entry state") {
    const auto compiled = ai2d::compile_game_file(breakout_manifest());
    REQUIRE(compiled);
    const auto run_cycle = [](const ai2d::GamePlan& plan) {
        ai2d::GameRuntime runtime{};
        ai2d::GameRuntimeOptions options{};
        options.headless = true;
        options.load_saved_settings = false;
        REQUIRE(runtime.initialize(plan, options));
        const auto entered = runtime.advance(pressed(ai2d::InputKey::space), 1.0 / 120.0);
        REQUIRE(entered);
        CHECK(entered->scene_changed);
        CHECK(entered->interpolation_alpha == 0.0);
        REQUIRE(runtime.current_scene() == "game");
        const double initial = entered->scene_state_checksum;
        const auto immediate_destination = runtime.advance({}, 0.0);
        REQUIRE(immediate_destination);
        CHECK(immediate_destination->interpolation_alpha == 0.0);
        CHECK(immediate_destination->scene_state_checksum == Catch::Approx(initial));
        REQUIRE(runtime.run_exact(pressed(ai2d::InputKey::space), 1U));
        const auto moved = runtime.run_exact({}, 10U);
        REQUIRE(moved);
        REQUIRE(moved->scene_state_checksum != Catch::Approx(initial));
        const auto left = runtime.advance(pressed(ai2d::InputKey::escape), 1.0 / 120.0);
        REQUIRE(left);
        CHECK(left->scene_changed);
        CHECK(left->interpolation_alpha == 0.0);
        REQUIRE(runtime.current_scene() == "menu");
        const auto reentered = runtime.advance(pressed(ai2d::InputKey::space), 1.0 / 120.0);
        REQUIRE(reentered);
        CHECK(reentered->scene_changed);
        CHECK(reentered->interpolation_alpha == 0.0);
        REQUIRE(runtime.current_scene() == "game");
        const auto normalized_destination = runtime.advance({}, 0.0);
        REQUIRE(normalized_destination);
        CHECK(normalized_destination->interpolation_alpha == 0.0);
        CHECK(normalized_destination->scene_state_checksum == Catch::Approx(reentered->scene_state_checksum));
        return std::array{initial, moved->scene_state_checksum, reentered->scene_state_checksum};
    };

    const auto reset_cycle = run_cycle(*compiled);
    CHECK(reset_cycle[2] == Catch::Approx(reset_cycle[0]));

    auto retained_plan = *compiled;
    for (auto& transition : retained_plan.transitions) {
        const auto from = retained_plan.symbol(retained_plan.scenes[transition.from_scene].symbol);
        const auto to = transition.quit
                            ? std::string_view{"$quit"}
                            : retained_plan.symbol(retained_plan.scenes[transition.to_scene].symbol);
        if ((from == "menu" && to == "game") || (from == "game" && to == "menu")) {
            transition.reset_scene = false;
        }
    }
    retained_plan.plan_hash = ai2d::compute_game_plan_hash(retained_plan);
    REQUIRE(ai2d::validate_game_plan(retained_plan));
    const auto retained_cycle = run_cycle(retained_plan);
    CHECK(retained_cycle[2] != Catch::Approx(retained_cycle[1]));
}

TEST_CASE("same-scene retained transitions run scene_enter after session reset") {
    auto compiled = ai2d::compile_game_file(snake_manifest());
    REQUIRE(compiled);
    auto plan = std::move(*compiled);
    auto& scene = plan.scenes[plan.start_scene];

    const auto rule_symbol = static_cast<ai2d::SymbolId>(plan.symbols.size());
    plan.symbols.emplace_back("same_scene_enter_probe");
    ai2d::GameRulePlan enter_rule{};
    enter_rule.symbol = rule_symbol;
    enter_rule.event.kind = ai2d::GameRuleEventKind::scene_enter;
    ai2d::GameRuleActionPlan set_score{};
    set_score.kind = ai2d::GameRuleActionKind::set_int_state;
    set_score.state_index = 0U;
    set_score.value = 3;
    enter_rule.actions.push_back(set_score);
    scene.rules.push_back(std::move(enter_rule));

    ai2d::SceneTransitionPlan transition{};
    transition.from_scene = plan.start_scene;
    transition.to_scene = plan.start_scene;
    transition.condition.kind = ai2d::TransitionConditionKind::action_pressed;
    transition.condition.action = 4U; // Snake restart.
    transition.reset_scene = false;
    transition.reset_session = true;
    plan.transitions.push_back(transition);
    plan.plan_hash = ai2d::compute_game_plan_hash(plan);
    REQUIRE(ai2d::validate_game_plan(plan));

    ai2d::GameRuntime runtime{};
    ai2d::GameRuntimeOptions options{};
    options.headless = true;
    options.load_saved_settings = false;
    REQUIRE(runtime.initialize(plan, options));
    REQUIRE(runtime.state_value("score"));
    CHECK(*runtime.state_value("score") == 3);

    REQUIRE(runtime.run_exact({}, 12U));
    REQUIRE(runtime.state_value("score"));
    CHECK(*runtime.state_value("score") == 13);
    const auto reentered = runtime.advance(pressed(ai2d::InputKey::space), 0.0);
    REQUIRE(reentered);
    CHECK(reentered->scene_changed);
    CHECK(reentered->interpolation_alpha == 0.0);
    CHECK(reentered->rule_executions == 1U);
    REQUIRE(runtime.state_value("score"));
    CHECK(*runtime.state_value("score") == 3);
}

#if defined(AI2D_ENABLE_GPU)
TEST_CASE("SDL core decodes the sample PNG and WAV assets") {
    const auto root = breakout_manifest().parent_path() / "assets";
    const auto image = ai2d::load_png_rgba8(root / "sprites.png");
    REQUIRE(image);
    CHECK(image->width == 256U);
    CHECK(image->height == 64U);
    CHECK(image->pixels.size() == 256U * 64U * 4U);

    const auto wave = ai2d::load_wav_f32_stereo(root / "hit.wav");
    REQUIRE(wave);
    CHECK_FALSE(wave->samples.empty());
    CHECK(wave->samples.size() % 2U == 0U);
}

TEST_CASE("Asset preflight rejects oversized decoded PNG and WAV declarations") {
    const auto root = std::filesystem::temp_directory_path() / "ai2d-v02-asset-preflight";
    std::error_code error{};
    std::filesystem::create_directories(root, error);
    REQUIRE_FALSE(error);

    const auto png = root / "oversized.png";
    constexpr std::array<std::uint8_t, 24U> png_header{
        137U, 80U, 78U, 71U, 13U, 10U, 26U, 10U,
        0U, 0U, 0U, 13U, 'I', 'H', 'D', 'R',
        0U, 1U, 0U, 0U, 0U, 1U, 0U, 0U,
    };
    {
        std::ofstream stream{png, std::ios::binary | std::ios::trunc};
        stream.write(reinterpret_cast<const char*>(png_header.data()), static_cast<std::streamsize>(png_header.size()));
    }
    const auto image = ai2d::load_png_rgba8(png);
    REQUIRE_FALSE(image);
    CHECK(image.error().code == ai2d::DiagnosticCode::asset_decode_failed);

    const auto wav = root / "expanding.wav";
    constexpr std::uint32_t data_size = 6U * 1024U * 1024U;
    std::array<std::uint8_t, 44U> wav_header{};
    const auto put_u16 = [&](const std::size_t offset, const std::uint16_t value) {
        wav_header[offset] = static_cast<std::uint8_t>(value & 0xFFU);
        wav_header[offset + 1U] = static_cast<std::uint8_t>(value >> 8U);
    };
    const auto put_u32 = [&](const std::size_t offset, const std::uint32_t value) {
        for (std::size_t byte = 0U; byte < 4U; ++byte) {
            wav_header[offset + byte] = static_cast<std::uint8_t>((value >> (byte * 8U)) & 0xFFU);
        }
    };
    std::copy_n("RIFF", 4U, reinterpret_cast<char*>(wav_header.data()));
    put_u32(4U, data_size + 36U);
    std::copy_n("WAVEfmt ", 8U, reinterpret_cast<char*>(wav_header.data() + 8U));
    put_u32(16U, 16U);
    put_u16(20U, 1U);
    put_u16(22U, 1U);
    put_u32(24U, 8'000U);
    put_u32(28U, 8'000U);
    put_u16(32U, 1U);
    put_u16(34U, 8U);
    std::copy_n("data", 4U, reinterpret_cast<char*>(wav_header.data() + 36U));
    put_u32(40U, data_size);
    {
        std::ofstream stream{wav, std::ios::binary | std::ios::trunc};
        stream.write(reinterpret_cast<const char*>(wav_header.data()), static_cast<std::streamsize>(wav_header.size()));
    }
    std::filesystem::resize_file(wav, static_cast<std::uintmax_t>(44U) + data_size, error);
    REQUIRE_FALSE(error);
    const auto wave = ai2d::load_wav_f32_stereo(wav);
    REQUIRE_FALSE(wave);
    CHECK(wave.error().code == ai2d::DiagnosticCode::asset_decode_failed);

    std::filesystem::remove_all(root, error);
    CHECK_FALSE(error);
}

TEST_CASE("GameRuntime rejects aggregate decoded assets before renderer initialization") {
    const auto root = std::filesystem::temp_directory_path() / "ai2d-v03-aggregate-assets";
    std::error_code error{};
    std::filesystem::remove_all(root, error);
    error.clear();
    std::filesystem::copy(
        breakout_manifest().parent_path(), root,
        std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing, error);
    REQUIRE_FALSE(error);
    auto compiled = ai2d::compile_game_file(root / "game.json");
    REQUIRE(compiled);

    const auto large_png = root / "assets" / "aggregate.png";
    constexpr std::array<std::uint8_t, 24U> png_header{
        137U, 80U, 78U, 71U, 13U, 10U, 26U, 10U,
        0U, 0U, 0U, 13U, 'I', 'H', 'D', 'R',
        0U, 0U, 32U, 0U, 0U, 0U, 32U, 0U,
    };
    {
        std::ofstream stream{large_png, std::ios::binary | std::ios::trunc};
        stream.write(reinterpret_cast<const char*>(png_header.data()), static_cast<std::streamsize>(png_header.size()));
    }
    const auto inspected = ai2d::preflight_png_asset(large_png);
    REQUIRE(inspected);
    CHECK(inspected->decoded_bytes == 256ULL * 1024ULL * 1024ULL);

    auto plan = *compiled;
    plan.assets[0U].path = large_png;
    plan.assets[1U].path = large_png;
    auto extra = plan.assets[0U];
    extra.symbol = static_cast<ai2d::SymbolId>(plan.symbols.size());
    plan.symbols.emplace_back("aggregate_extra");
    plan.assets.push_back(std::move(extra));
    plan.plan_hash = ai2d::compute_game_plan_hash(plan);
    REQUIRE(ai2d::validate_game_plan(plan));

    ai2d::GameRuntime runtime{};
    ai2d::GameRuntimeOptions options{};
    options.offscreen = true;
    options.enable_audio = false;
    options.load_saved_settings = false;
    const auto initialized = runtime.initialize(plan, options);
    REQUIRE_FALSE(initialized);
    CHECK(initialized.error().code == ai2d::DiagnosticCode::asset_decode_failed);
    CHECK_FALSE(runtime.initialized());

    std::filesystem::remove_all(root, error);
    CHECK_FALSE(error);
}

TEST_CASE("Settings preserve first-run manifest precedence and strict schema") {
    constexpr std::string_view organization{"OpenAI-AI2D-Tests"};
    constexpr std::string_view application{"settings-contract-v02"};
    const auto path = ai2d::settings_file_path(organization, application);
    REQUIRE(path);
    std::error_code error{};
    std::filesystem::remove(*path, error);
    error.clear();

    const auto missing = ai2d::load_game_settings(organization, application);
    REQUIRE(missing);
    CHECK_FALSE(missing->has_value());

    auto plan = ai2d::compile_game_file(breakout_manifest());
    REQUIRE(plan);
    plan->symbols[plan->organization] = std::string{organization};
    plan->symbols[plan->application] = std::string{application};
    plan->window.default_fps = ai2d::RenderFpsCap::fps_144;
    plan->plan_hash = ai2d::compute_game_plan_hash(*plan);
    REQUIRE(ai2d::validate_game_plan(*plan));
    {
        ai2d::GameRuntime runtime{};
        ai2d::GameRuntimeOptions options{};
        options.offscreen = true;
        options.enable_audio = false;
        options.load_saved_settings = true;
        REQUIRE(runtime.initialize(*plan, options));
        CHECK(runtime.requested_fps_cap() == ai2d::RenderFpsCap::fps_144);
    }

    {
        std::ofstream stream{*path, std::ios::binary | std::ios::trunc};
        stream << "{\"schema_version\":10,\"fps_cap\":\"120\",\"master_volume\":0.5}\n";
    }
    const auto future = ai2d::load_game_settings(organization, application);
    REQUIRE_FALSE(future);
    CHECK(future.error().code == ai2d::DiagnosticCode::settings_invalid);

    std::atomic<std::uint32_t> successful_writes{0U};
    std::array<std::thread, 8U> writers{};
    for (std::size_t index = 0U; index < writers.size(); ++index) {
        writers[index] = std::thread{[&, index] {
            const auto cap = index % 2U == 0U ? ai2d::RenderFpsCap::fps_120 : ai2d::RenderFpsCap::fps_240;
            if (ai2d::save_game_settings(organization, application, {cap, 0.5F})) {
                successful_writes.fetch_add(1U, std::memory_order_relaxed);
            }
        }};
    }
    for (auto& writer : writers) writer.join();
    CHECK(successful_writes.load(std::memory_order_relaxed) == writers.size());
    const auto saved = ai2d::load_game_settings(organization, application);
    REQUIRE(saved);
    REQUIRE(saved->has_value());
    CHECK((saved->value().fps_cap == ai2d::RenderFpsCap::fps_120 ||
           saved->value().fps_cap == ai2d::RenderFpsCap::fps_240));

    std::filesystem::remove(*path, error);
    CHECK_FALSE(error);
}

TEST_CASE("Compiled UI formatting performs no measured steady-state heap allocation") {
#if defined(AI2D_ENABLE_ALLOCATION_TRACKING)
    auto plan = ai2d::compile_game_file(breakout_manifest());
    REQUIRE(plan);
    const std::string long_state_name(96U, 's');
    for (auto& symbol : plan->symbols) {
        const auto placeholder = symbol.find("{score}");
        if (placeholder != std::string::npos) {
            symbol.replace(placeholder + 1U, std::string_view{"score"}.size(), long_state_name);
        }
    }
    const auto score = std::find_if(plan->states.begin(), plan->states.end(), [&](const ai2d::IntStatePlan& state) {
        return plan->symbol(state.symbol) == "score";
    });
    REQUIRE(score != plan->states.end());
    plan->symbols[score->symbol] = long_state_name;
    plan->plan_hash = ai2d::compute_game_plan_hash(*plan);
    REQUIRE(ai2d::validate_game_plan(*plan));

    ai2d::GameRuntime runtime{};
    ai2d::GameRuntimeOptions options{};
    options.offscreen = true;
    options.enable_audio = false;
    options.load_saved_settings = false;
    REQUIRE(runtime.initialize(*plan, options));
    REQUIRE(runtime.advance(pressed(ai2d::InputKey::space), 0.0));
    REQUIRE(runtime.run_exact({}, 2U));
    ai2d::MeasuredAllocationScope scope{};
    const auto frame = runtime.run_exact({}, 1U);
    const auto allocations = scope.finish();
    REQUIRE(frame);
    CHECK(allocations.allocations == 0U);
    CHECK(allocations.bytes == 0U);
#else
    SKIP("allocation hook disabled by configuration");
#endif
}
#endif
