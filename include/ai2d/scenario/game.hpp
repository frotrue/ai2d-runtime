#pragma once

#include "ai2d/foundation/diagnostic.hpp"
#include "ai2d/foundation/types.hpp"
#include "ai2d/scenario/scenario.hpp"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ai2d {

class JsonWriter;

enum class GameAssetKind : std::uint8_t { png, wav, font };
enum class GameKey : std::uint8_t {
    escape,
    left,
    right,
    up,
    down,
    a,
    d,
    space,
    enter,
    tab,
};
enum class GameSchemaVersion : std::uint8_t { v0_2, v0_3, v0_4, v0_5 };
enum class GameOperationId : std::uint8_t {
    axis_control,
    set_velocity_on_press,
    simulate_collisions,
    grid_motion,
    follow_transform_chain,
    linear_motion,
};
enum class GameBodyMotion : std::uint8_t { static_body, kinematic_body, dynamic_body };
enum class GameCollisionInteraction : std::uint8_t { legacy, solid, trigger };
enum class GameReactionKind : std::uint8_t { reflect, deactivate, add_int_state, reset_group, play_sound };
enum class GameReactionTarget : std::uint8_t { a, b };
enum class GameDirection : std::uint8_t { none, up, down, left, right };
enum class GameComparison : std::uint8_t { equal, not_equal, less, less_equal, greater, greater_equal };
enum class GameRuleEventKind : std::uint8_t {
    scene_enter,
    action_pressed,
    action_released,
    fixed_interval,
    collision,
    contact_begin,
    contact_end,
};
enum class GameRuleConditionKind : std::uint8_t { int_state, group_active_count };
enum class GameRuleTargetKind : std::uint8_t { spawn_group, spawn_index, collision_a, collision_b };
enum class GameRuleActionKind : std::uint8_t {
    set_int_state,
    add_int_state,
    activate,
    deactivate,
    set_group_active_count,
    set_velocity,
    queue_grid_direction,
    reset_group,
    play_sound,
    relocate_to_free_cell,
    spawn_from_pool,
    release_to_pool,
    reset_pool,
};
enum class GamePoolExhaustionPolicy : std::uint8_t { skip, recycle_oldest };
enum class GamePoolSpawnPositionKind : std::uint8_t { initial, constant, target };
enum class GameInputEventKind : std::uint8_t { press, release, tap };
enum class TransitionConditionKind : std::uint8_t {
    action_pressed,
    int_state_at_most,
    int_state_at_least,
    group_inactive,
};
enum class UiElementKind : std::uint8_t { panel, text, button };

struct GameWindowPlan final {
    SymbolId title{0U};
    std::uint32_t width{1280U};
    std::uint32_t height{720U};
    std::uint32_t virtual_width{1280U};
    std::uint32_t virtual_height{720U};
    RenderFpsCap default_fps{RenderFpsCap::fps_60};
};

struct GlyphPlan final {
    std::uint32_t codepoint{0U};
    Rect uv{{0.0F, 0.0F}, {1.0F, 1.0F}};
    Vec2 size{};
    Vec2 bearing{};
    float advance{0.0F};
};

struct GameAssetPlan final {
    static constexpr std::size_t max_glyphs = 65'536U;

    SymbolId symbol{0U};
    GameAssetKind kind{GameAssetKind::png};
    std::filesystem::path path{};
    std::filesystem::path metadata_path{};
    std::vector<GlyphPlan> glyphs{};
    float line_height{0.0F};
};

struct ActionPlan final {
    static constexpr std::size_t max_keys = 4U;
    SymbolId symbol{0U};
    GameKey keys[max_keys]{};
    std::uint32_t key_count{0U};
    bool mouse_left{false};
};

struct IntStatePlan final {
    SymbolId symbol{0U};
    std::int32_t initial{0};
    std::int32_t minimum{0};
    std::int32_t maximum{0};
};

struct GamePlacementPlan final {
    static constexpr std::uint32_t max_columns = 10'000U;

    PlacementId kind{PlacementId::grid};
    Vec2 origin{};
    Vec2 spacing{1.0F, 1.0F};
    std::uint32_t columns{1U};
};

struct GameGridPlan final {
    static constexpr std::uint64_t max_cells = 1'000'000U;

    SymbolId symbol{0U};
    Vec2 first_cell_center{};
    Vec2 cell_size{1.0F, 1.0F};
    std::uint32_t columns{1U};
    std::uint32_t rows{1U};
};

struct GameSpritePlan final {
    std::uint32_t asset_index{0U};
    Vec2 size{1.0F, 1.0F};
    Vec2 pivot{0.5F, 0.5F};
    Rect uv{{0.0F, 0.0F}, {1.0F, 1.0F}};
    Color tint{};
    std::int32_t layer{0};
    bool visible{true};
};

struct GameColliderPlan final {
    Vec2 offset{};
    Vec2 half_extent{0.5F, 0.5F};
    SymbolId group{0U};
    GameBodyMotion motion{GameBodyMotion::static_body};
    bool trigger{false};
    bool enabled{true};
};

struct GameSpawnGroupPlan final {
    static constexpr std::uint32_t max_count = 10'000U;

    SymbolId symbol{0U};
    std::uint32_t count{0U};
    std::uint32_t active_count{0U};
    GamePlacementPlan placement{};
    TransformInitializerPlan transform{};
    VelocityInitializerPlan velocity{};
    GameSpritePlan sprite{};
    GameColliderPlan collider{};
    bool has_velocity{false};
    bool has_sprite{false};
    bool has_collider{false};
};

struct GamePoolPlan final {
    SymbolId symbol{0U};
    std::uint32_t spawn_group_index{0U};
    GamePoolExhaustionPolicy on_exhausted{GamePoolExhaustionPolicy::skip};
};

struct GameSystemPlan final {
    SymbolId symbol{0U};
    GameOperationId operation{GameOperationId::simulate_collisions};
    SymbolId group{0U};
    std::uint32_t negative_action{0U};
    std::uint32_t positive_action{0U};
    std::uint32_t action{0U};
    float speed{0.0F};
    float minimum{-16.0F};
    float maximum{16.0F};
    Vec2 velocity{};
    std::uint32_t spawn_group_index{0U};
    std::uint32_t grid_index{0U};
    std::uint32_t step_interval_ticks{1U};
    GameDirection initial_direction{GameDirection::none};
    bool prevent_reverse{false};
    std::uint32_t leader_group_index{0U};
    std::uint32_t follower_group_index{0U};
    std::uint32_t motion_system_index{0U};
};

struct GameReactionPlan final {
    GameReactionKind kind{GameReactionKind::reflect};
    GameReactionTarget target{GameReactionTarget::a};
    std::uint32_t state_index{0U};
    std::int32_t value{0};
    SymbolId group{0U};
    std::uint32_t asset_index{0U};
};

struct GameCollisionRulePlan final {
    SymbolId symbol{0U};
    SymbolId group_a{0U};
    SymbolId group_b{0U};
    GameCollisionInteraction interaction{GameCollisionInteraction::legacy};
    std::vector<GameReactionPlan> reactions{};
};

struct GameRuleTargetPlan final {
    GameRuleTargetKind kind{GameRuleTargetKind::spawn_group};
    std::uint32_t spawn_group_index{0U};
    std::uint32_t item_index{0U};
};

struct GameRuleEventPlan final {
    GameRuleEventKind kind{GameRuleEventKind::scene_enter};
    std::uint32_t action_index{0U};
    std::uint32_t interval_ticks{1U};
    std::uint32_t collision_rule_index{0U};
};

struct GameRuleConditionPlan final {
    GameRuleConditionKind kind{GameRuleConditionKind::int_state};
    GameComparison comparison{GameComparison::equal};
    std::uint32_t state_index{0U};
    std::uint32_t spawn_group_index{0U};
    std::int32_t value{0};
};

struct GameRuleActionPlan final {
    static constexpr std::size_t max_occupancy_groups = 32U;

    GameRuleActionKind kind{GameRuleActionKind::set_int_state};
    GameRuleTargetPlan target{};
    std::uint32_t state_index{0U};
    std::int32_t value{0};
    std::uint32_t spawn_group_index{0U};
    bool count_from_state{false};
    std::uint32_t count_state_index{0U};
    std::uint32_t count_constant{0U};
    Vec2 velocity{};
    std::uint32_t system_index{0U};
    GameDirection direction{GameDirection::none};
    std::uint32_t asset_index{0U};
    std::uint32_t grid_index{0U};
    std::vector<std::uint32_t> occupancy_group_indices{};
    bool has_result_state{false};
    std::uint32_t result_state_index{0U};
    std::uint32_t pool_index{0U};
    GamePoolSpawnPositionKind pool_position_kind{GamePoolSpawnPositionKind::initial};
    Vec2 pool_position{};
    GameRuleTargetPlan pool_position_target{};
    Vec2 pool_position_offset{};
    bool has_velocity_override{false};
    bool has_rotation_override{false};
    float rotation{0.0F};
    bool has_lifetime{false};
    std::uint32_t lifetime_ticks{0U};
};

struct GameRulePlan final {
    static constexpr std::size_t max_conditions = 8U;
    static constexpr std::size_t max_actions = 16U;

    SymbolId symbol{0U};
    GameRuleEventPlan event{};
    std::vector<GameRuleConditionPlan> conditions{};
    std::vector<GameRuleActionPlan> actions{};
};

struct UiElementPlan final {
    SymbolId symbol{0U};
    UiElementKind kind{UiElementKind::panel};
    Vec2 position{};
    Vec2 size{};
    SymbolId text{0U};
    std::uint32_t font_asset{0U};
    std::uint32_t action{0U};
    Color color{};
    Color hover_color{};
    Color text_color{};
    std::int32_t layer{1000};
    float text_scale{1.0F};
};

struct GameScenePlan final {
    static constexpr std::uint32_t max_world_capacity = 10'000U;
    static constexpr std::size_t max_spawn_groups = 1'024U;
    static constexpr std::size_t max_systems = 64U;
    static constexpr std::size_t max_collision_rules = 256U;
    static constexpr std::size_t max_ui_elements = 1'024U;
    static constexpr std::size_t max_rules = 512U;
    static constexpr std::size_t max_grids = 64U;
    static constexpr std::size_t max_pools = 1'024U;
    static constexpr std::uint32_t max_collision_capacity = 10'000'000U;
    static constexpr std::uint32_t max_impacts_per_dynamic = 16U;
    static constexpr std::uint64_t max_collision_cells = 1'000'000U;

    SymbolId symbol{0U};
    std::filesystem::path source_path{};
    std::uint32_t world_capacity{0U};
    Vec2 camera_position{};
    Vec2 camera_half_extent{16.0F, 9.0F};
    Rect collision_bounds{{-16.0F, -9.0F}, {16.0F, 9.0F}};
    Vec2 collision_cell_size{1.0F, 1.0F};
    std::uint32_t max_colliders{10'000U};
    std::uint32_t max_grid_references{80'000U};
    std::uint32_t max_candidate_pairs{80'000U};
    std::uint32_t max_contact_pairs{0U};
    std::uint32_t max_impacts{4U};
    std::vector<GameGridPlan> grids{};
    std::vector<GameSpawnGroupPlan> spawn_groups{};
    std::vector<GamePoolPlan> pools{};
    std::vector<GameSystemPlan> systems{};
    std::vector<GameCollisionRulePlan> collision_rules{};
    std::vector<GameRulePlan> rules{};
    std::vector<UiElementPlan> ui{};
    std::uint32_t total_spawn_count{0U};
};

struct TransitionConditionPlan final {
    TransitionConditionKind kind{TransitionConditionKind::action_pressed};
    std::uint32_t action{0U};
    std::uint32_t state_index{0U};
    std::int32_t value{0};
    SymbolId group{0U};
};

struct SceneTransitionPlan final {
    std::uint32_t from_scene{0U};
    TransitionConditionPlan condition{};
    std::uint32_t to_scene{0U};
    bool quit{false};
    bool reset_scene{false};
    bool reset_session{false};
};

struct FpsActionPlan final {
    std::uint32_t action{0U};
    RenderFpsCap cap{RenderFpsCap::fps_60};
};

struct GamePlan final {
    static constexpr std::string_view legacy_schema_version{"0.2"};
    static constexpr std::string_view event_action_schema_version{"0.3"};
    static constexpr std::string_view object_pool_schema_version{"0.4"};
    static constexpr std::string_view supported_schema_version{"0.5"};
    static constexpr std::size_t max_assets = 256U;
    static constexpr std::size_t max_actions = 64U;
    static constexpr std::size_t max_states = 64U;
    static constexpr std::size_t max_scenes = 64U;
    static constexpr std::size_t max_transitions = 256U;
    static constexpr std::size_t max_fps_actions = 64U;
    static constexpr std::uint32_t max_render_submissions = 100'000U;

    std::vector<std::string> symbols{};
    GameSchemaVersion schema_version{GameSchemaVersion::v0_2};
    std::uint64_t seed{0U};
    SymbolId name{0U};
    SymbolId organization{0U};
    SymbolId application{0U};
    GameWindowPlan window{};
    std::vector<GameAssetPlan> assets{};
    std::vector<ActionPlan> actions{};
    std::vector<IntStatePlan> states{};
    std::vector<GameScenePlan> scenes{};
    std::vector<SceneTransitionPlan> transitions{};
    std::vector<FpsActionPlan> fps_actions{};
    std::uint32_t start_scene{0U};
    std::filesystem::path manifest_path{};
    std::filesystem::path content_root{};
    std::uint64_t source_hash{0U};
    std::uint64_t plan_hash{0U};

    [[nodiscard]] std::string_view symbol(SymbolId id) const noexcept;
    [[nodiscard]] std::string_view schema_version_text() const noexcept;
};

struct GameInputEventPlan final {
    std::uint64_t tick{0U};
    std::uint32_t action_index{0U};
    GameInputEventKind kind{GameInputEventKind::tap};
};

struct GameInputScriptPlan final {
    static constexpr std::size_t max_events = 1'000'000U;

    std::vector<GameInputEventPlan> events{};
    std::uint64_t last_tick{0U};
};

enum class GameTestAssertionKind : std::uint8_t {
    current_scene,
    int_state,
    group_active_count,
    entity_active,
    position,
    velocity,
    runtime_metric,
};

enum class GameTestMetric : std::uint8_t {
    rule_executions,
    condition_evaluations,
    action_executions,
    grid_steps,
    rejected_direction_changes,
    follower_updates,
    active_state_changes,
    relocations,
    relocation_cells_scanned,
    collision_contacts,
    trigger_narrowphase_tests,
    contact_begins,
    contact_ends,
    stale_contact_events,
    active_contact_pairs,
    peak_contact_pairs,
    motion_segments,
    linear_motion_updates,
    pool_acquire_attempts,
    pool_acquire_successes,
    pool_releases,
    pool_release_misses,
    pool_exhaustions,
    pool_recycled_slots,
    pool_expirations,
    pool_resets,
    pool_lifetime_checks,
    active_pooled_entities,
    peak_active_pooled_entities,
};

struct GameTestAssertionPlan final {
    std::uint64_t tick{0U};
    GameTestAssertionKind kind{GameTestAssertionKind::current_scene};
    GameComparison comparison{GameComparison::equal};
    std::uint32_t scene_index{0U};
    std::uint32_t state_index{0U};
    std::uint32_t spawn_group_index{0U};
    std::uint32_t item_index{0U};
    std::int64_t expected_integer{0};
    std::uint64_t expected_unsigned{0U};
    bool expected_active{false};
    Vec2 expected_vector{};
    float tolerance{1.0e-5F};
    GameTestMetric metric{GameTestMetric::rule_executions};
};

struct GameTestScriptPlan final {
    static constexpr std::size_t max_events = 1'000'000U;
    static constexpr std::size_t max_assertions = 4'096U;
    static constexpr std::uint32_t max_frames = 1'000'000U;

    std::uint32_t frames{1U};
    std::vector<GameInputEventPlan> events{};
    std::vector<GameTestAssertionPlan> assertions{};
};

[[nodiscard]] std::string_view to_string(GameAssetKind value) noexcept;
[[nodiscard]] std::string_view to_string(GameOperationId value) noexcept;
[[nodiscard]] std::string_view to_string(GameReactionKind value) noexcept;
[[nodiscard]] std::string_view to_string(TransitionConditionKind value) noexcept;
[[nodiscard]] std::string_view to_string(UiElementKind value) noexcept;
[[nodiscard]] std::string_view to_string(RenderFpsCap value) noexcept;
[[nodiscard]] std::string_view to_string(GameDirection value) noexcept;
[[nodiscard]] std::string_view to_string(GameRuleEventKind value) noexcept;
[[nodiscard]] std::string_view to_string(GameRuleActionKind value) noexcept;
[[nodiscard]] std::string_view to_string(GamePoolExhaustionPolicy value) noexcept;
[[nodiscard]] std::string_view to_string(GamePoolSpawnPositionKind value) noexcept;
[[nodiscard]] std::string_view to_string(GameCollisionInteraction value) noexcept;
[[nodiscard]] std::string_view to_string(GameTestAssertionKind value) noexcept;
[[nodiscard]] std::string_view to_string(GameTestMetric value) noexcept;

[[nodiscard]] Result<GamePlan> compile_game_file(const std::filesystem::path& manifest_path);
[[nodiscard]] Result<GameInputScriptPlan> compile_game_input_file(
    const std::filesystem::path& input_path,
    const GamePlan& game_plan);
[[nodiscard]] Result<GameTestScriptPlan> compile_game_test_file(
    const std::filesystem::path& test_path,
    const GamePlan& game_plan);
[[nodiscard]] Result<void> validate_game_plan(const GamePlan& plan);
[[nodiscard]] std::uint64_t compute_game_plan_hash(const GamePlan& plan);
void write_game_summary_json(JsonWriter& writer, const GamePlan& plan);

} // namespace ai2d
