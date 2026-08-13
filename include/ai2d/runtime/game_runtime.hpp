#pragma once

#include "ai2d/foundation/diagnostic.hpp"
#include "ai2d/foundation/types.hpp"
#include "ai2d/platform/window.hpp"
#include "ai2d/scenario/game.hpp"
#include "ai2d/world/collision.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace ai2d {

struct GameRuntimeOptions final {
    std::uint32_t width{0U};
    std::uint32_t height{0U};
    bool headless{false};
    bool offscreen{false};
    bool hidden{false};
    bool enable_audio{true};
    bool load_saved_settings{true};
    bool enable_validation{false};
    bool enable_synchronization_validation{false};
    bool has_fps_override{false};
    RenderFpsCap fps_override{RenderFpsCap::fps_60};
};

struct GameRuntimeLoadMetrics final {
    double load_ms{0.0};
    std::uint32_t loaded_assets{0U};
    std::uint32_t loaded_textures{0U};
    std::uint32_t loaded_audio_clips{0U};
    std::uint32_t spawned_entities{0U};
    bool audio_available{false};
    RenderFpsCap requested_fps{RenderFpsCap::fps_60};
    RenderFpsCap effective_fps{RenderFpsCap::fps_60};
};

struct GameRuntimeFrameMetrics final {
    std::uint64_t frame_index{0U};
    std::uint64_t simulation_tick{0U};
    std::uint32_t fixed_ticks{0U};
    std::uint32_t input_edge_ticks{0U};
    double interpolation_alpha{0.0};
    double dropped_seconds{0.0};
    double frame_cpu_ms{0.0};
    bool frame_time_clamped{false};
    bool catch_up_limited{false};
    bool scene_changed{false};
    bool quit_requested{false};
    bool presented{false};
    std::uint32_t scene_index{0U};
    std::uint32_t extracted_sprites{0U};
    CollisionMetrics2D collision{};
    std::uint64_t rule_executions{0U};
    std::uint64_t condition_evaluations{0U};
    std::uint64_t action_executions{0U};
    std::uint64_t grid_steps{0U};
    std::uint64_t rejected_direction_changes{0U};
    std::uint64_t follower_updates{0U};
    std::uint64_t active_state_changes{0U};
    std::uint64_t relocations{0U};
    std::uint64_t relocation_cells_scanned{0U};
    std::uint64_t pool_acquire_attempts{0U};
    std::uint64_t pool_acquire_successes{0U};
    std::uint64_t pool_releases{0U};
    std::uint64_t pool_release_misses{0U};
    std::uint64_t pool_exhaustions{0U};
    std::uint64_t pool_recycled_slots{0U};
    std::uint64_t pool_expirations{0U};
    std::uint64_t pool_resets{0U};
    std::uint64_t pool_lifetime_checks{0U};
    std::uint64_t trigger_narrowphase_tests{0U};
    std::uint64_t contact_begins{0U};
    std::uint64_t contact_ends{0U};
    std::uint64_t stale_contact_events{0U};
    std::uint64_t motion_segments{0U};
    std::uint64_t linear_motion_updates{0U};
    std::uint32_t active_contact_pairs{0U};
    std::uint32_t peak_contact_pairs{0U};
    std::uint32_t active_pooled_entities{0U};
    std::uint32_t peak_active_pooled_entities{0U};
    double state_checksum{0.0};
    double scene_state_checksum{0.0};
};

struct GameActionInput final {
    bool down{false};
    bool pressed{false};
    bool released{false};
};

class GameRuntime final {
public:
    GameRuntime();
    ~GameRuntime();
    GameRuntime(const GameRuntime&) = delete;
    GameRuntime& operator=(const GameRuntime&) = delete;
    GameRuntime(GameRuntime&&) noexcept;
    GameRuntime& operator=(GameRuntime&&) noexcept;

    [[nodiscard]] Result<GameRuntimeLoadMetrics> initialize(
        const GamePlan& plan,
        const GameRuntimeOptions& options = {});
    [[nodiscard]] Result<GameRuntimeFrameMetrics> run_frame();
    [[nodiscard]] Result<GameRuntimeFrameMetrics> advance(
        const InputSnapshot& input,
        double elapsed_seconds);
    [[nodiscard]] Result<GameRuntimeFrameMetrics> run_exact(
        const InputSnapshot& input,
        std::uint32_t fixed_ticks = 1U);
    [[nodiscard]] Result<GameRuntimeFrameMetrics> run_exact_actions(
        std::span<const GameActionInput> actions,
        std::uint32_t fixed_ticks = 1U);
    [[nodiscard]] Result<void> wait_idle();
    [[nodiscard]] Result<std::int32_t> state_value(std::string_view state_name) const;
    [[nodiscard]] Result<std::int32_t> state_value(std::uint32_t state_index) const;
    [[nodiscard]] Result<std::uint32_t> group_active_count(std::uint32_t spawn_group_index) const;
    [[nodiscard]] Result<bool> entity_active(std::uint32_t spawn_group_index, std::uint32_t item_index) const;
    [[nodiscard]] Result<Vec2> entity_position(std::uint32_t spawn_group_index, std::uint32_t item_index) const;
    [[nodiscard]] Result<Vec2> entity_velocity(std::uint32_t spawn_group_index, std::uint32_t item_index) const;
    [[nodiscard]] std::uint32_t current_scene_index() const noexcept;
    [[nodiscard]] std::uint64_t contact_state_checksum() const noexcept;
    [[nodiscard]] std::string_view current_scene() const noexcept;
    [[nodiscard]] RenderFpsCap requested_fps_cap() const noexcept;
    [[nodiscard]] RenderFpsCap effective_fps_cap() const noexcept;
    [[nodiscard]] std::string_view requested_present_mode() const noexcept;
    [[nodiscard]] std::string_view effective_present_mode() const noexcept;
    [[nodiscard]] std::span<const Diagnostic> diagnostics() const noexcept;
    [[nodiscard]] bool initialized() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ai2d
