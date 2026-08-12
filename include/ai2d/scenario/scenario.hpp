#pragma once

#include "ai2d/foundation/diagnostic.hpp"
#include "ai2d/foundation/types.hpp"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ai2d {

class JsonWriter;

using SymbolId = std::uint32_t;

enum class ComponentId : std::uint8_t { transform2d, velocity2d, sprite2d };
enum class OperationId : std::uint8_t { integrate_velocity, wrap_bounds };
enum class PhaseId : std::uint8_t { fixed_update, post_update };
enum class PlacementId : std::uint8_t { grid, seeded_random };
enum class TextureGeneratorId : std::uint8_t { solid, checker };

struct OperationDescriptor final {
    OperationId id{OperationId::integrate_velocity};
    std::string_view name{};
    std::uint8_t query_mask{0U};
    std::uint8_t read_mask{0U};
    std::uint8_t write_mask{0U};
    std::uint8_t legal_phase_mask{0U};
    std::string_view complexity{};
    bool structural_effects{false};
    bool allocation_permitted{false};
};

struct TextureAssetPlan final {
    SymbolId symbol{0U};
    TextureGeneratorId generator{TextureGeneratorId::solid};
    std::uint32_t width{1U};
    std::uint32_t height{1U};
    std::uint32_t cell_size{1U};
    Color first{};
    Color second{};
};

struct PlacementPlan final {
    PlacementId kind{PlacementId::grid};
    Vec2 origin{};
    Vec2 spacing{1.0F, 1.0F};
    std::uint32_t columns{1U};
    Rect random_bounds{{-1.0F, -1.0F}, {1.0F, 1.0F}};
};

struct TransformInitializerPlan final {
    Vec2 position_offset{};
    float rotation{0.0F};
    Vec2 scale{1.0F, 1.0F};
};

struct VelocityInitializerPlan final {
    Vec2 linear{};
    float angular{0.0F};
};

struct SpriteInitializerPlan final {
    std::uint32_t texture_asset{0U};
    Vec2 size{1.0F, 1.0F};
    Vec2 pivot{0.5F, 0.5F};
    Color tint{};
    std::int32_t layer{0};
    bool visible{true};
};

struct SpawnGroupPlan final {
    SymbolId symbol{0U};
    std::uint32_t count{0U};
    PlacementPlan placement{};
    TransformInitializerPlan transform{};
    VelocityInitializerPlan velocity{};
    SpriteInitializerPlan sprite{};
    bool has_velocity{false};
    bool has_sprite{false};
};

struct OperationParameters final {
    float delta_seconds{1.0F / 60.0F};
    Rect bounds{{-16.0F, -9.0F}, {16.0F, 9.0F}};
};

struct PlannedSystem final {
    SymbolId symbol{0U};
    OperationId operation{OperationId::integrate_velocity};
    PhaseId phase{PhaseId::fixed_update};
    OperationParameters parameters{};
    std::vector<std::uint32_t> after_system_indices{};
    std::uint32_t estimated_cardinality{0U};
};

struct BenchmarkConfiguration final {
    std::uint32_t runs{3U};
    std::uint32_t warmup_frames{30U};
    std::uint32_t measurement_frames{200U};
};

struct ExecutionPlan final {
    static constexpr std::string_view supported_schema_version{"0.1"};

    // Debug/inspection names live here. Hot dispatch uses only the numeric fields above.
    std::vector<std::string> symbols{};
    SymbolId scenario_name{0U};
    std::uint64_t seed{0U};
    std::uint32_t world_capacity{0U};
    Vec2 camera_position{};
    Vec2 camera_half_extent{16.0F, 9.0F};
    std::vector<TextureAssetPlan> textures{};
    std::vector<SpawnGroupPlan> spawn_groups{};
    std::vector<PlannedSystem> systems{};
    BenchmarkConfiguration benchmark{};
    std::uint64_t scenario_hash{0U};
    std::uint64_t plan_hash{0U};
    std::uint32_t total_spawn_count{0U};

    [[nodiscard]] std::string_view symbol(SymbolId id) const noexcept;
};

[[nodiscard]] std::span<const OperationDescriptor> operation_descriptors() noexcept;
[[nodiscard]] const OperationDescriptor& operation_descriptor(OperationId operation) noexcept;
[[nodiscard]] std::string_view to_string(ComponentId component) noexcept;
[[nodiscard]] std::string_view to_string(OperationId operation) noexcept;
[[nodiscard]] std::string_view to_string(PhaseId phase) noexcept;
[[nodiscard]] std::string_view to_string(PlacementId placement) noexcept;
[[nodiscard]] std::string_view to_string(TextureGeneratorId generator) noexcept;

[[nodiscard]] Result<ExecutionPlan> compile_scenario_text(
    std::string_view source,
    std::string_view source_name = "<memory>");
[[nodiscard]] Result<ExecutionPlan> compile_scenario_file(const std::filesystem::path& path);
[[nodiscard]] std::uint64_t compute_execution_plan_hash(const ExecutionPlan& plan) noexcept;
[[nodiscard]] Result<void> validate_execution_plan(const ExecutionPlan& plan);

void write_execution_plan_json(JsonWriter& writer, const ExecutionPlan& plan);
void write_scenario_summary_json(JsonWriter& writer, const ExecutionPlan& plan);
void write_diagnostics_schema_json(JsonWriter& writer);

} // namespace ai2d
