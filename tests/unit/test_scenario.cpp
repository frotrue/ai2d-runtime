#include "ai2d/foundation/json_writer.hpp"
#include "ai2d/scenario/scenario.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <filesystem>
#include <string_view>
#include <utility>

namespace {

const std::filesystem::path fixture_root{AI2D_SOURCE_DIR "/tests/fixtures/scenarios"};

} // namespace

TEST_CASE("Moving ScenarioSpec resolves to a deterministic numeric plan") {
    const auto path = fixture_root / "valid" / "moving_sprites.json";
    auto first = ai2d::compile_scenario_file(path);
    auto second = ai2d::compile_scenario_file(path);

    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK(first->scenario_hash == second->scenario_hash);
    CHECK(first->plan_hash == second->plan_hash);
    CHECK(first->world_capacity == 1000U);
    CHECK(first->total_spawn_count == 1000U);
    REQUIRE(first->systems.size() == 2U);
    CHECK(first->systems[0U].operation == ai2d::OperationId::integrate_velocity);
    CHECK(first->systems[1U].operation == ai2d::OperationId::wrap_bounds);
    REQUIRE(first->systems[1U].after_system_indices.size() == 1U);
    CHECK(first->systems[1U].after_system_indices[0U] == 0U);
    CHECK(first->systems[0U].estimated_cardinality == 1000U);
    CHECK(first->systems[1U].estimated_cardinality == 1000U);

    ai2d::JsonWriter writer{};
    ai2d::write_execution_plan_json(writer, *first);
    CHECK(writer.complete());
    CHECK(writer.str().find("ordered_systems") != std::string::npos);
}

TEST_CASE("All required valid ScenarioSpec workloads compile") {
    for (const auto name : {"static_sprites.json", "moving_sprites.json", "texture_switching.json"}) {
        INFO(name);
        const auto compiled = ai2d::compile_scenario_file(fixture_root / "valid" / name);
        REQUIRE(compiled.has_value());
    }
}

TEST_CASE("Invalid ScenarioSpec fixtures return their stable diagnostic codes") {
    constexpr std::array cases{
        std::pair{"unknown_operation.json", ai2d::DiagnosticCode::ir_unknown_operation},
        std::pair{"invalid_access.json", ai2d::DiagnosticCode::ir_access_mismatch},
        std::pair{"missing_texture.json", ai2d::DiagnosticCode::ir_missing_texture},
        std::pair{"dependency_cycle.json", ai2d::DiagnosticCode::ir_dependency_cycle},
        std::pair{"ambiguous_write_order.json", ai2d::DiagnosticCode::ir_ambiguous_write_order},
        std::pair{"capacity_exceeded.json", ai2d::DiagnosticCode::ir_capacity_exceeded},
        std::pair{"invalid_bounds.json", ai2d::DiagnosticCode::ir_invalid_bounds},
        std::pair{"unsupported_version.json", ai2d::DiagnosticCode::ir_schema_version_unsupported},
    };

    for (const auto& [name, expected] : cases) {
        INFO(name);
        const auto compiled = ai2d::compile_scenario_file(fixture_root / "invalid" / name);
        REQUIRE_FALSE(compiled.has_value());
        CHECK(compiled.error().code == expected);
        CHECK_FALSE(compiled.error().context.empty());
    }
}

TEST_CASE("Canonical operation descriptors own access and allocation metadata") {
    const auto descriptors = ai2d::operation_descriptors();
    REQUIRE(descriptors.size() == 2U);
    CHECK(descriptors[0U].name == "integrate_velocity");
    CHECK(descriptors[1U].name == "wrap_bounds");
    CHECK(descriptors[0U].complexity == "linear_dense");
    CHECK_FALSE(descriptors[0U].structural_effects);
    CHECK_FALSE(descriptors[0U].allocation_permitted);
}

TEST_CASE("ScenarioSpec rejects unknown fields instead of silently ignoring authoring mistakes") {
    constexpr std::string_view source = R"json({
      "schema_version":"0.1","name":"unknown_field","seed":1,"mystery":true,
      "world":{"capacity":1},"camera":{"position":[0,0],"half_extent":[1,1]},
      "textures":[],"spawn_groups":[],"systems":[],
      "benchmark":{"runs":1,"warmup_frames":0,"measurement_frames":1}
    })json";
    const auto compiled = ai2d::compile_scenario_text(source);
    REQUIRE_FALSE(compiled.has_value());
    CHECK(compiled.error().code == ai2d::DiagnosticCode::ir_schema_invalid);
}
