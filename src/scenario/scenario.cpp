#include "ai2d/scenario/scenario.hpp"

#include "ai2d/foundation/json_writer.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <numeric>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ai2d {
namespace {

using Json = nlohmann::json;

constexpr std::uint8_t component_bit(const ComponentId component) noexcept {
    return static_cast<std::uint8_t>(1U << static_cast<std::uint8_t>(component));
}

constexpr std::uint8_t phase_bit(const PhaseId phase) noexcept {
    return static_cast<std::uint8_t>(1U << static_cast<std::uint8_t>(phase));
}

constexpr std::array<OperationDescriptor, 2U> descriptors{{
    {
        OperationId::integrate_velocity,
        "integrate_velocity",
        static_cast<std::uint8_t>(component_bit(ComponentId::transform2d) | component_bit(ComponentId::velocity2d)),
        static_cast<std::uint8_t>(component_bit(ComponentId::transform2d) | component_bit(ComponentId::velocity2d)),
        component_bit(ComponentId::transform2d),
        phase_bit(PhaseId::fixed_update),
        "linear_dense",
        false,
        false,
    },
    {
        OperationId::wrap_bounds,
        "wrap_bounds",
        component_bit(ComponentId::transform2d),
        component_bit(ComponentId::transform2d),
        component_bit(ComponentId::transform2d),
        static_cast<std::uint8_t>(phase_bit(PhaseId::fixed_update) | phase_bit(PhaseId::post_update)),
        "linear_dense",
        false,
        false,
    },
}};

Diagnostic scenario_error(
    const DiagnosticCode code,
    std::string message,
    const std::string_view source_name,
    const std::string_view pointer = {}) {
    auto diagnostic = Diagnostic::make(code, Severity::error, "scenario", std::move(message));
    diagnostic.context.push_back({"source", std::string{source_name}});
    if (!pointer.empty()) {
        diagnostic.context.push_back({"json_pointer", std::string{pointer}});
    }
    return diagnostic;
}

Result<void> reject_unknown_members(
    const Json& object,
    const std::initializer_list<std::string_view> allowed,
    const std::string_view source_name,
    const std::string_view pointer) {
    if (!object.is_object()) {
        return std::unexpected(scenario_error(
            DiagnosticCode::ir_schema_invalid, "Expected a JSON object", source_name, pointer));
    }
    for (const auto& [key, value] : object.items()) {
        (void)value;
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end()) {
            return std::unexpected(scenario_error(
                DiagnosticCode::ir_schema_invalid,
                "ScenarioSpec object contains an unknown field",
                source_name,
                std::string{pointer} + "/" + key));
        }
    }
    return {};
}

Result<const Json*> required_member(
    const Json& object,
    const std::string_view key,
    const std::string_view source_name,
    const std::string_view parent_pointer) {
    if (!object.is_object()) {
        return std::unexpected(scenario_error(
            DiagnosticCode::ir_schema_invalid,
            "Expected a JSON object",
            source_name,
            parent_pointer));
    }
    const auto iterator = object.find(std::string{key});
    if (iterator == object.end()) {
        const auto pointer = std::string{parent_pointer} + "/" + std::string{key};
        return std::unexpected(scenario_error(
            DiagnosticCode::ir_schema_invalid,
            "Missing required ScenarioSpec field",
            source_name,
            pointer));
    }
    return &*iterator;
}

Result<std::string> required_string(
    const Json& object,
    const std::string_view key,
    const std::string_view source_name,
    const std::string_view parent_pointer) {
    auto value = required_member(object, key, source_name, parent_pointer);
    if (!value) {
        return std::unexpected(std::move(value.error()));
    }
    if (!(*value)->is_string() || (*value)->get_ref<const std::string&>().empty()) {
        return std::unexpected(scenario_error(
            DiagnosticCode::ir_schema_invalid,
            "Expected a non-empty string",
            source_name,
            std::string{parent_pointer} + "/" + std::string{key}));
    }
    return (*value)->get<std::string>();
}

Result<std::uint32_t> required_u32(
    const Json& object,
    const std::string_view key,
    const std::uint32_t minimum,
    const std::uint32_t maximum,
    const std::string_view source_name,
    const std::string_view parent_pointer) {
    auto value = required_member(object, key, source_name, parent_pointer);
    if (!value) {
        return std::unexpected(std::move(value.error()));
    }
    if (!(*value)->is_number_unsigned() && !(*value)->is_number_integer()) {
        return std::unexpected(scenario_error(
            DiagnosticCode::ir_schema_invalid,
            "Expected an integer",
            source_name,
            std::string{parent_pointer} + "/" + std::string{key}));
    }
    const auto signed_value = (*value)->get<std::int64_t>();
    if (signed_value < static_cast<std::int64_t>(minimum) ||
        signed_value > static_cast<std::int64_t>(maximum)) {
        return std::unexpected(scenario_error(
            DiagnosticCode::ir_schema_invalid,
            "Integer is outside the supported range",
            source_name,
            std::string{parent_pointer} + "/" + std::string{key}));
    }
    return static_cast<std::uint32_t>(signed_value);
}

Result<float> finite_float(
    const Json& value,
    const std::string_view source_name,
    const std::string_view pointer) {
    if (!value.is_number()) {
        return std::unexpected(scenario_error(
            DiagnosticCode::ir_schema_invalid, "Expected a number", source_name, pointer));
    }
    const auto number = value.get<double>();
    if (!std::isfinite(number) || number < -static_cast<double>(std::numeric_limits<float>::max()) ||
        number > static_cast<double>(std::numeric_limits<float>::max())) {
        return std::unexpected(scenario_error(
            DiagnosticCode::ir_schema_invalid, "Number must be finite and fit in a float", source_name, pointer));
    }
    return static_cast<float>(number);
}

Result<Vec2> vec2_value(
    const Json& value,
    const std::string_view source_name,
    const std::string_view pointer) {
    if (!value.is_array() || value.size() != 2U) {
        return std::unexpected(scenario_error(
            DiagnosticCode::ir_schema_invalid, "Expected a two-number array", source_name, pointer));
    }
    auto x = finite_float(value[0U], source_name, std::string{pointer} + "/0");
    if (!x) {
        return std::unexpected(std::move(x.error()));
    }
    auto y = finite_float(value[1U], source_name, std::string{pointer} + "/1");
    if (!y) {
        return std::unexpected(std::move(y.error()));
    }
    return Vec2{*x, *y};
}

Result<Color> color_value(
    const Json& value,
    const std::string_view source_name,
    const std::string_view pointer) {
    if (!value.is_array() || value.size() != 4U) {
        return std::unexpected(scenario_error(
            DiagnosticCode::ir_schema_invalid, "Expected a four-number color array", source_name, pointer));
    }
    std::array<float, 4U> channels{};
    for (std::size_t index = 0U; index < channels.size(); ++index) {
        auto channel = finite_float(value[index], source_name, std::string{pointer} + "/" + std::to_string(index));
        if (!channel) {
            return std::unexpected(std::move(channel.error()));
        }
        if (*channel < 0.0F || *channel > 1.0F) {
            return std::unexpected(scenario_error(
                DiagnosticCode::ir_schema_invalid,
                "Color channels must be in the inclusive range [0,1]",
                source_name,
                std::string{pointer} + "/" + std::to_string(index)));
        }
        channels[index] = *channel;
    }
    return Color{channels[0U], channels[1U], channels[2U], channels[3U]};
}

Result<Vec2> optional_vec2(
    const Json& object,
    const std::string_view key,
    const Vec2 fallback,
    const std::string_view source_name,
    const std::string_view parent_pointer) {
    const auto iterator = object.find(std::string{key});
    if (iterator == object.end()) {
        return fallback;
    }
    return vec2_value(*iterator, source_name, std::string{parent_pointer} + "/" + std::string{key});
}

Result<float> optional_float(
    const Json& object,
    const std::string_view key,
    const float fallback,
    const std::string_view source_name,
    const std::string_view parent_pointer) {
    const auto iterator = object.find(std::string{key});
    if (iterator == object.end()) {
        return fallback;
    }
    return finite_float(*iterator, source_name, std::string{parent_pointer} + "/" + std::string{key});
}

Result<Color> optional_color(
    const Json& object,
    const std::string_view key,
    const Color fallback,
    const std::string_view source_name,
    const std::string_view parent_pointer) {
    const auto iterator = object.find(std::string{key});
    if (iterator == object.end()) {
        return fallback;
    }
    return color_value(*iterator, source_name, std::string{parent_pointer} + "/" + std::string{key});
}

class SymbolTable final {
  public:
    explicit SymbolTable(std::vector<std::string>& storage) : storage_(storage) {}

    SymbolId intern(const std::string_view name) {
        const auto found = std::find(storage_.begin(), storage_.end(), name);
        if (found != storage_.end()) {
            return static_cast<SymbolId>(std::distance(storage_.begin(), found));
        }
        storage_.emplace_back(name);
        return static_cast<SymbolId>(storage_.size() - 1U);
    }

  private:
    std::vector<std::string>& storage_;
};

class StableHash final {
  public:
    void text(const std::string_view value) noexcept {
        integer(static_cast<std::uint64_t>(value.size()));
        for (const auto character : value) {
            byte(static_cast<std::uint8_t>(character));
        }
    }

    template <class Integer>
    void integer(const Integer value) noexcept {
        using Unsigned = std::make_unsigned_t<Integer>;
        auto remaining = static_cast<Unsigned>(value);
        for (std::size_t index = 0U; index < sizeof(Unsigned); ++index) {
            byte(static_cast<std::uint8_t>(remaining & static_cast<Unsigned>(0xFFU)));
            remaining >>= 8U;
        }
    }

    void floating(const float value) noexcept { integer(std::bit_cast<std::uint32_t>(value)); }
    [[nodiscard]] std::uint64_t value() const noexcept { return value_; }

  private:
    void byte(const std::uint8_t value) noexcept {
        value_ ^= value;
        value_ *= 1099511628211ULL;
    }
    std::uint64_t value_{14695981039346656037ULL};
};

std::uint64_t hash_text(const std::string_view value) noexcept {
    StableHash hash{};
    hash.text(value);
    return hash.value();
}

void hash_vec2(StableHash& hash, const Vec2 value) noexcept {
    hash.floating(value.x);
    hash.floating(value.y);
}

void hash_color(StableHash& hash, const Color value) noexcept {
    hash.floating(value.r);
    hash.floating(value.g);
    hash.floating(value.b);
    hash.floating(value.a);
}

std::uint64_t hash_plan(const ExecutionPlan& plan) noexcept {
    StableHash hash{};
    hash.integer(plan.seed);
    hash.integer(plan.world_capacity);
    hash_vec2(hash, plan.camera_position);
    hash_vec2(hash, plan.camera_half_extent);
    hash.integer(static_cast<std::uint64_t>(plan.textures.size()));
    for (const auto& texture : plan.textures) {
        hash.integer(texture.symbol);
        hash.integer(static_cast<std::uint8_t>(texture.generator));
        hash.integer(texture.width);
        hash.integer(texture.height);
        hash.integer(texture.cell_size);
        hash_color(hash, texture.first);
        hash_color(hash, texture.second);
    }
    hash.integer(static_cast<std::uint64_t>(plan.spawn_groups.size()));
    for (const auto& spawn : plan.spawn_groups) {
        hash.integer(spawn.symbol);
        hash.integer(spawn.count);
        hash.integer(static_cast<std::uint8_t>(spawn.placement.kind));
        hash_vec2(hash, spawn.placement.origin);
        hash_vec2(hash, spawn.placement.spacing);
        hash.integer(spawn.placement.columns);
        hash_vec2(hash, spawn.placement.random_bounds.min);
        hash_vec2(hash, spawn.placement.random_bounds.max);
        hash_vec2(hash, spawn.transform.position_offset);
        hash.floating(spawn.transform.rotation);
        hash_vec2(hash, spawn.transform.scale);
        hash.integer(static_cast<std::uint8_t>(spawn.has_velocity));
        hash_vec2(hash, spawn.velocity.linear);
        hash.floating(spawn.velocity.angular);
        hash.integer(static_cast<std::uint8_t>(spawn.has_sprite));
        hash.integer(spawn.sprite.texture_asset);
        hash_vec2(hash, spawn.sprite.size);
        hash_vec2(hash, spawn.sprite.pivot);
        hash_color(hash, spawn.sprite.tint);
        hash.integer(spawn.sprite.layer);
        hash.integer(static_cast<std::uint8_t>(spawn.sprite.visible));
    }
    hash.integer(static_cast<std::uint64_t>(plan.systems.size()));
    for (const auto& system : plan.systems) {
        hash.integer(system.symbol);
        hash.integer(static_cast<std::uint8_t>(system.operation));
        hash.integer(static_cast<std::uint8_t>(system.phase));
        hash.floating(system.parameters.delta_seconds);
        hash_vec2(hash, system.parameters.bounds.min);
        hash_vec2(hash, system.parameters.bounds.max);
        hash.integer(system.estimated_cardinality);
        hash.integer(static_cast<std::uint64_t>(system.after_system_indices.size()));
        for (const auto dependency : system.after_system_indices) {
            hash.integer(dependency);
        }
    }
    hash.integer(plan.benchmark.runs);
    hash.integer(plan.benchmark.warmup_frames);
    hash.integer(plan.benchmark.measurement_frames);
    return hash.value();
}

Result<ComponentId> parse_component(
    const Json& value,
    const std::string_view source_name,
    const std::string_view pointer) {
    if (!value.is_string()) {
        return std::unexpected(scenario_error(
            DiagnosticCode::ir_access_mismatch, "Access component must be a string", source_name, pointer));
    }
    const auto& name = value.get_ref<const std::string&>();
    if (name == "Transform2D") {
        return ComponentId::transform2d;
    }
    if (name == "Velocity2D") {
        return ComponentId::velocity2d;
    }
    if (name == "Sprite2D") {
        return ComponentId::sprite2d;
    }
    return std::unexpected(scenario_error(
        DiagnosticCode::ir_access_mismatch, "Unknown component in expected access assertion", source_name, pointer));
}

Result<std::uint8_t> parse_component_mask(
    const Json& access,
    const std::string_view field,
    const std::string_view source_name,
    const std::string_view pointer) {
    const auto iterator = access.find(std::string{field});
    const auto field_pointer = std::string{pointer} + "/" + std::string{field};
    if (iterator == access.end() || !iterator->is_array()) {
        return std::unexpected(scenario_error(
            DiagnosticCode::ir_access_mismatch,
            "Expected access assertion requires query, reads, and writes arrays",
            source_name,
            field_pointer));
    }
    std::uint8_t mask = 0U;
    for (std::size_t index = 0U; index < iterator->size(); ++index) {
        auto component = parse_component((*iterator)[index], source_name, field_pointer + "/" + std::to_string(index));
        if (!component) {
            return std::unexpected(std::move(component.error()));
        }
        const auto bit = component_bit(*component);
        if ((mask & bit) != 0U) {
            return std::unexpected(scenario_error(
                DiagnosticCode::ir_access_mismatch,
                "Expected access assertion contains a duplicate component",
                source_name,
                field_pointer));
        }
        mask = static_cast<std::uint8_t>(mask | bit);
    }
    return mask;
}

Result<void> validate_access_assertion(
    const Json& system,
    const OperationDescriptor& descriptor,
    const std::string_view source_name,
    const std::string_view pointer) {
    const auto iterator = system.find("expected_access");
    if (iterator == system.end()) {
        return {};
    }
    if (!iterator->is_object()) {
        return std::unexpected(scenario_error(
            DiagnosticCode::ir_access_mismatch,
            "expected_access must be an object",
            source_name,
            std::string{pointer} + "/expected_access"));
    }
    const auto access_pointer = std::string{pointer} + "/expected_access";
    if (auto fields = reject_unknown_members(
            *iterator, {"query", "reads", "writes"}, source_name, access_pointer);
        !fields) {
        return fields;
    }
    auto query = parse_component_mask(*iterator, "query", source_name, access_pointer);
    if (!query) {
        return std::unexpected(std::move(query.error()));
    }
    auto reads = parse_component_mask(*iterator, "reads", source_name, access_pointer);
    if (!reads) {
        return std::unexpected(std::move(reads.error()));
    }
    auto writes = parse_component_mask(*iterator, "writes", source_name, access_pointer);
    if (!writes) {
        return std::unexpected(std::move(writes.error()));
    }
    if (*query != descriptor.query_mask || *reads != descriptor.read_mask || *writes != descriptor.write_mask) {
        return std::unexpected(scenario_error(
            DiagnosticCode::ir_access_mismatch,
            "Expected access assertion does not match the canonical operation descriptor",
            source_name,
            access_pointer));
    }
    return {};
}

Result<OperationId> parse_operation(
    const std::string_view name,
    const std::string_view source_name,
    const std::string_view pointer) {
    for (const auto& descriptor : descriptors) {
        if (descriptor.name == name) {
            return descriptor.id;
        }
    }
    return std::unexpected(scenario_error(
        DiagnosticCode::ir_unknown_operation, "Unknown built-in operation", source_name, pointer));
}

Result<PhaseId> parse_phase(
    const std::string_view name,
    const std::string_view source_name,
    const std::string_view pointer) {
    if (name == "fixed_update") {
        return PhaseId::fixed_update;
    }
    if (name == "post_update") {
        return PhaseId::post_update;
    }
    return std::unexpected(scenario_error(
        DiagnosticCode::ir_schema_invalid, "Unknown execution phase", source_name, pointer));
}

Result<std::uint32_t> texture_index_by_name(
    const ExecutionPlan& plan,
    const std::string_view name,
    const std::string_view source_name,
    const std::string_view pointer) {
    for (std::size_t index = 0U; index < plan.textures.size(); ++index) {
        if (plan.symbol(plan.textures[index].symbol) == name) {
            return static_cast<std::uint32_t>(index);
        }
    }
    return std::unexpected(scenario_error(
        DiagnosticCode::ir_missing_texture, "Sprite references an undeclared texture", source_name, pointer));
}

Result<TextureAssetPlan> parse_texture(
    const Json& texture,
    const std::size_t index,
    ExecutionPlan& plan,
    SymbolTable& symbols,
    const std::string_view source_name) {
    const auto pointer = "/textures/" + std::to_string(index);
    if (auto fields = reject_unknown_members(
            texture, {"id", "kind", "width", "height", "cell_size", "color", "color2"}, source_name, pointer);
        !fields) {
        return std::unexpected(std::move(fields.error()));
    }
    auto name = required_string(texture, "id", source_name, pointer);
    if (!name) {
        return std::unexpected(std::move(name.error()));
    }
    if (std::any_of(plan.textures.begin(), plan.textures.end(), [&](const auto& existing) {
            return plan.symbol(existing.symbol) == *name;
        })) {
        return std::unexpected(scenario_error(
            DiagnosticCode::ir_schema_invalid, "Texture id must be unique", source_name, pointer + "/id"));
    }
    auto kind = required_string(texture, "kind", source_name, pointer);
    if (!kind) {
        return std::unexpected(std::move(kind.error()));
    }
    TextureAssetPlan result{};
    result.symbol = symbols.intern(*name);
    if (*kind == "solid") {
        result.generator = TextureGeneratorId::solid;
    } else if (*kind == "checker") {
        result.generator = TextureGeneratorId::checker;
    } else {
        return std::unexpected(scenario_error(
            DiagnosticCode::ir_schema_invalid, "Unknown procedural texture kind", source_name, pointer + "/kind"));
    }
    auto width = required_u32(texture, "width", 1U, 4096U, source_name, pointer);
    if (!width) {
        return std::unexpected(std::move(width.error()));
    }
    auto height = required_u32(texture, "height", 1U, 4096U, source_name, pointer);
    if (!height) {
        return std::unexpected(std::move(height.error()));
    }
    result.width = *width;
    result.height = *height;
    auto first_member = required_member(texture, "color", source_name, pointer);
    if (!first_member) {
        return std::unexpected(std::move(first_member.error()));
    }
    auto first = color_value(**first_member, source_name, pointer + "/color");
    if (!first) {
        return std::unexpected(std::move(first.error()));
    }
    result.first = *first;
    result.second = result.first;
    if (result.generator == TextureGeneratorId::checker) {
        auto second_member = required_member(texture, "color2", source_name, pointer);
        if (!second_member) {
            return std::unexpected(std::move(second_member.error()));
        }
        auto second = color_value(**second_member, source_name, pointer + "/color2");
        if (!second) {
            return std::unexpected(std::move(second.error()));
        }
        result.second = *second;
        auto cell_size = required_u32(texture, "cell_size", 1U, 4096U, source_name, pointer);
        if (!cell_size) {
            return std::unexpected(std::move(cell_size.error()));
        }
        result.cell_size = *cell_size;
    }
    return result;
}

Result<SpawnGroupPlan> parse_spawn_group(
    const Json& spawn,
    const std::size_t index,
    ExecutionPlan& plan,
    SymbolTable& symbols,
    const std::string_view source_name) {
    const auto pointer = "/spawn_groups/" + std::to_string(index);
    if (auto fields = reject_unknown_members(
            spawn, {"id", "count", "placement", "transform", "velocity", "sprite"}, source_name, pointer);
        !fields) {
        return std::unexpected(std::move(fields.error()));
    }
    auto name = required_string(spawn, "id", source_name, pointer);
    if (!name) {
        return std::unexpected(std::move(name.error()));
    }
    if (std::any_of(plan.spawn_groups.begin(), plan.spawn_groups.end(), [&](const auto& existing) {
            return plan.symbol(existing.symbol) == *name;
        })) {
        return std::unexpected(scenario_error(
            DiagnosticCode::ir_schema_invalid, "Spawn group id must be unique", source_name, pointer + "/id"));
    }
    auto count = required_u32(spawn, "count", 1U, 1'000'000U, source_name, pointer);
    if (!count) {
        return std::unexpected(std::move(count.error()));
    }
    SpawnGroupPlan result{};
    result.symbol = symbols.intern(*name);
    result.count = *count;

    auto placement_member = required_member(spawn, "placement", source_name, pointer);
    if (!placement_member) {
        return std::unexpected(std::move(placement_member.error()));
    }
    const auto& placement = **placement_member;
    const auto placement_pointer = pointer + "/placement";
    auto placement_kind = required_string(placement, "kind", source_name, placement_pointer);
    if (!placement_kind) {
        return std::unexpected(std::move(placement_kind.error()));
    }
    if (*placement_kind == "grid") {
        if (auto fields = reject_unknown_members(
                placement, {"kind", "origin", "spacing", "columns"}, source_name, placement_pointer);
            !fields) {
            return std::unexpected(std::move(fields.error()));
        }
        result.placement.kind = PlacementId::grid;
        auto origin_member = required_member(placement, "origin", source_name, placement_pointer);
        if (!origin_member) {
            return std::unexpected(std::move(origin_member.error()));
        }
        auto origin = vec2_value(**origin_member, source_name, placement_pointer + "/origin");
        if (!origin) {
            return std::unexpected(std::move(origin.error()));
        }
        auto spacing_member = required_member(placement, "spacing", source_name, placement_pointer);
        if (!spacing_member) {
            return std::unexpected(std::move(spacing_member.error()));
        }
        auto spacing = vec2_value(**spacing_member, source_name, placement_pointer + "/spacing");
        if (!spacing) {
            return std::unexpected(std::move(spacing.error()));
        }
        auto columns = required_u32(placement, "columns", 1U, 1'000'000U, source_name, placement_pointer);
        if (!columns) {
            return std::unexpected(std::move(columns.error()));
        }
        result.placement.origin = *origin;
        result.placement.spacing = *spacing;
        result.placement.columns = *columns;
    } else if (*placement_kind == "seeded_random") {
        if (auto fields = reject_unknown_members(
                placement, {"kind", "bounds"}, source_name, placement_pointer);
            !fields) {
            return std::unexpected(std::move(fields.error()));
        }
        result.placement.kind = PlacementId::seeded_random;
        auto bounds_member = required_member(placement, "bounds", source_name, placement_pointer);
        if (!bounds_member) {
            return std::unexpected(std::move(bounds_member.error()));
        }
        if (auto fields = reject_unknown_members(
                **bounds_member, {"min", "max"}, source_name, placement_pointer + "/bounds");
            !fields) {
            return std::unexpected(std::move(fields.error()));
        }
        auto minimum_member = required_member(**bounds_member, "min", source_name, placement_pointer + "/bounds");
        if (!minimum_member) {
            return std::unexpected(std::move(minimum_member.error()));
        }
        auto maximum_member = required_member(**bounds_member, "max", source_name, placement_pointer + "/bounds");
        if (!maximum_member) {
            return std::unexpected(std::move(maximum_member.error()));
        }
        auto minimum = vec2_value(**minimum_member, source_name, placement_pointer + "/bounds/min");
        if (!minimum) {
            return std::unexpected(std::move(minimum.error()));
        }
        auto maximum = vec2_value(**maximum_member, source_name, placement_pointer + "/bounds/max");
        if (!maximum) {
            return std::unexpected(std::move(maximum.error()));
        }
        if (minimum->x >= maximum->x || minimum->y >= maximum->y) {
            return std::unexpected(scenario_error(
                DiagnosticCode::ir_invalid_bounds,
                "Random placement bounds must have positive width and height",
                source_name,
                placement_pointer + "/bounds"));
        }
        result.placement.random_bounds = {*minimum, *maximum};
    } else {
        return std::unexpected(scenario_error(
            DiagnosticCode::ir_schema_invalid,
            "Unknown spawn placement kind",
            source_name,
            placement_pointer + "/kind"));
    }

    auto transform_member = required_member(spawn, "transform", source_name, pointer);
    if (!transform_member) {
        return std::unexpected(std::move(transform_member.error()));
    }
    const auto transform_pointer = pointer + "/transform";
    if (auto fields = reject_unknown_members(
            **transform_member, {"position_offset", "rotation", "scale"}, source_name, transform_pointer);
        !fields) {
        return std::unexpected(std::move(fields.error()));
    }
    auto offset = optional_vec2(**transform_member, "position_offset", {}, source_name, transform_pointer);
    if (!offset) {
        return std::unexpected(std::move(offset.error()));
    }
    auto rotation = optional_float(**transform_member, "rotation", 0.0F, source_name, transform_pointer);
    if (!rotation) {
        return std::unexpected(std::move(rotation.error()));
    }
    auto scale = optional_vec2(**transform_member, "scale", {1.0F, 1.0F}, source_name, transform_pointer);
    if (!scale) {
        return std::unexpected(std::move(scale.error()));
    }
    if (scale->x <= 0.0F || scale->y <= 0.0F) {
        return std::unexpected(scenario_error(
            DiagnosticCode::ir_schema_invalid,
            "Transform scale must be positive",
            source_name,
            transform_pointer + "/scale"));
    }
    result.transform = {*offset, *rotation, *scale};

    if (const auto velocity = spawn.find("velocity"); velocity != spawn.end()) {
        if (!velocity->is_object()) {
            return std::unexpected(scenario_error(
                DiagnosticCode::ir_schema_invalid, "velocity must be an object", source_name, pointer + "/velocity"));
        }
        if (auto fields = reject_unknown_members(
                *velocity, {"linear", "angular"}, source_name, pointer + "/velocity");
            !fields) {
            return std::unexpected(std::move(fields.error()));
        }
        auto linear = optional_vec2(*velocity, "linear", {}, source_name, pointer + "/velocity");
        if (!linear) {
            return std::unexpected(std::move(linear.error()));
        }
        auto angular = optional_float(*velocity, "angular", 0.0F, source_name, pointer + "/velocity");
        if (!angular) {
            return std::unexpected(std::move(angular.error()));
        }
        result.velocity = {*linear, *angular};
        result.has_velocity = true;
    }

    if (const auto sprite = spawn.find("sprite"); sprite != spawn.end()) {
        if (!sprite->is_object()) {
            return std::unexpected(scenario_error(
                DiagnosticCode::ir_schema_invalid, "sprite must be an object", source_name, pointer + "/sprite"));
        }
        if (auto fields = reject_unknown_members(
                *sprite, {"texture", "size", "pivot", "tint", "layer", "visible"}, source_name, pointer + "/sprite");
            !fields) {
            return std::unexpected(std::move(fields.error()));
        }
        auto texture_name = required_string(*sprite, "texture", source_name, pointer + "/sprite");
        if (!texture_name) {
            return std::unexpected(std::move(texture_name.error()));
        }
        auto texture_index = texture_index_by_name(
            plan, *texture_name, source_name, pointer + "/sprite/texture");
        if (!texture_index) {
            return std::unexpected(std::move(texture_index.error()));
        }
        auto size = optional_vec2(*sprite, "size", {1.0F, 1.0F}, source_name, pointer + "/sprite");
        if (!size) {
            return std::unexpected(std::move(size.error()));
        }
        auto pivot = optional_vec2(*sprite, "pivot", {0.5F, 0.5F}, source_name, pointer + "/sprite");
        if (!pivot) {
            return std::unexpected(std::move(pivot.error()));
        }
        auto tint = optional_color(*sprite, "tint", {}, source_name, pointer + "/sprite");
        if (!tint) {
            return std::unexpected(std::move(tint.error()));
        }
        std::int32_t layer = 0;
        if (const auto layer_value = sprite->find("layer"); layer_value != sprite->end()) {
            if (!layer_value->is_number_integer()) {
                return std::unexpected(scenario_error(
                    DiagnosticCode::ir_schema_invalid,
                    "Sprite layer must be an integer",
                    source_name,
                    pointer + "/sprite/layer"));
            }
            const auto raw_layer = layer_value->get<std::int64_t>();
            if (raw_layer < std::numeric_limits<std::int32_t>::min() ||
                raw_layer > std::numeric_limits<std::int32_t>::max()) {
                return std::unexpected(scenario_error(
                    DiagnosticCode::ir_schema_invalid,
                    "Sprite layer is outside int32 range",
                    source_name,
                    pointer + "/sprite/layer"));
            }
            layer = static_cast<std::int32_t>(raw_layer);
        }
        bool visible = true;
        if (const auto visible_value = sprite->find("visible"); visible_value != sprite->end()) {
            if (!visible_value->is_boolean()) {
                return std::unexpected(scenario_error(
                    DiagnosticCode::ir_schema_invalid,
                    "Sprite visible must be boolean",
                    source_name,
                    pointer + "/sprite/visible"));
            }
            visible = visible_value->get<bool>();
        }
        if (size->x <= 0.0F || size->y <= 0.0F) {
            return std::unexpected(scenario_error(
                DiagnosticCode::ir_schema_invalid,
                "Sprite size must be positive",
                source_name,
                pointer + "/sprite/size"));
        }
        result.sprite = {*texture_index, *size, *pivot, *tint, layer, visible};
        result.has_sprite = true;
    }
    return result;
}

struct UnresolvedSystem final {
    PlannedSystem system{};
    std::vector<std::string> after_names{};
    std::size_t authoring_index{0U};
};

Result<UnresolvedSystem> parse_system(
    const Json& system,
    const std::size_t index,
    const ExecutionPlan& plan,
    SymbolTable& symbols,
    const std::string_view source_name,
    const std::vector<UnresolvedSystem>& parsed) {
    const auto pointer = "/systems/" + std::to_string(index);
    if (auto fields = reject_unknown_members(
            system, {"id", "operation", "phase", "after", "parameters", "expected_access"}, source_name, pointer);
        !fields) {
        return std::unexpected(std::move(fields.error()));
    }
    auto name = required_string(system, "id", source_name, pointer);
    if (!name) {
        return std::unexpected(std::move(name.error()));
    }
    if (std::any_of(parsed.begin(), parsed.end(), [&](const auto& existing) {
            return plan.symbol(existing.system.symbol) == *name;
        })) {
        return std::unexpected(scenario_error(
            DiagnosticCode::ir_schema_invalid, "System id must be unique", source_name, pointer + "/id"));
    }
    auto operation_name = required_string(system, "operation", source_name, pointer);
    if (!operation_name) {
        return std::unexpected(std::move(operation_name.error()));
    }
    auto operation = parse_operation(*operation_name, source_name, pointer + "/operation");
    if (!operation) {
        return std::unexpected(std::move(operation.error()));
    }
    auto phase_name = required_string(system, "phase", source_name, pointer);
    if (!phase_name) {
        return std::unexpected(std::move(phase_name.error()));
    }
    auto phase = parse_phase(*phase_name, source_name, pointer + "/phase");
    if (!phase) {
        return std::unexpected(std::move(phase.error()));
    }
    const auto& descriptor = operation_descriptor(*operation);
    if ((descriptor.legal_phase_mask & phase_bit(*phase)) == 0U) {
        return std::unexpected(scenario_error(
            DiagnosticCode::ir_schema_invalid,
            "Operation is not legal in the selected phase",
            source_name,
            pointer + "/phase"));
    }
    if (auto access = validate_access_assertion(system, descriptor, source_name, pointer); !access) {
        return std::unexpected(std::move(access.error()));
    }

    UnresolvedSystem result{};
    result.authoring_index = index;
    result.system.symbol = symbols.intern(*name);
    result.system.operation = *operation;
    result.system.phase = *phase;
    if (*operation == OperationId::integrate_velocity) {
        result.system.estimated_cardinality = std::accumulate(
            plan.spawn_groups.begin(), plan.spawn_groups.end(), std::uint32_t{0U}, [](const auto total, const auto& group) {
                return total + (group.has_velocity ? group.count : 0U);
            });
    } else {
        result.system.estimated_cardinality = plan.total_spawn_count;
    }

    auto parameters_member = required_member(system, "parameters", source_name, pointer);
    if (!parameters_member) {
        return std::unexpected(std::move(parameters_member.error()));
    }
    const auto& parameters = **parameters_member;
    if (!parameters.is_object()) {
        return std::unexpected(scenario_error(
            DiagnosticCode::ir_schema_invalid,
            "System parameters must be an object",
            source_name,
            pointer + "/parameters"));
    }
    if (*operation == OperationId::integrate_velocity) {
        if (auto fields = reject_unknown_members(
                parameters, {"delta_seconds"}, source_name, pointer + "/parameters");
            !fields) {
            return std::unexpected(std::move(fields.error()));
        }
        auto delta_member = required_member(parameters, "delta_seconds", source_name, pointer + "/parameters");
        if (!delta_member) {
            return std::unexpected(std::move(delta_member.error()));
        }
        auto delta = finite_float(**delta_member, source_name, pointer + "/parameters/delta_seconds");
        if (!delta) {
            return std::unexpected(std::move(delta.error()));
        }
        if (*delta <= 0.0F) {
            return std::unexpected(scenario_error(
                DiagnosticCode::ir_schema_invalid,
                "delta_seconds must be positive",
                source_name,
                pointer + "/parameters/delta_seconds"));
        }
        result.system.parameters.delta_seconds = *delta;
    } else {
        if (auto fields = reject_unknown_members(
                parameters, {"bounds"}, source_name, pointer + "/parameters");
            !fields) {
            return std::unexpected(std::move(fields.error()));
        }
        auto bounds_member = required_member(parameters, "bounds", source_name, pointer + "/parameters");
        if (!bounds_member) {
            return std::unexpected(std::move(bounds_member.error()));
        }
        if (auto fields = reject_unknown_members(
                **bounds_member, {"min", "max"}, source_name, pointer + "/parameters/bounds");
            !fields) {
            return std::unexpected(std::move(fields.error()));
        }
        auto minimum_member = required_member(**bounds_member, "min", source_name, pointer + "/parameters/bounds");
        if (!minimum_member) {
            return std::unexpected(std::move(minimum_member.error()));
        }
        auto maximum_member = required_member(**bounds_member, "max", source_name, pointer + "/parameters/bounds");
        if (!maximum_member) {
            return std::unexpected(std::move(maximum_member.error()));
        }
        auto minimum = vec2_value(**minimum_member, source_name, pointer + "/parameters/bounds/min");
        if (!minimum) {
            return std::unexpected(std::move(minimum.error()));
        }
        auto maximum = vec2_value(**maximum_member, source_name, pointer + "/parameters/bounds/max");
        if (!maximum) {
            return std::unexpected(std::move(maximum.error()));
        }
        if (minimum->x >= maximum->x || minimum->y >= maximum->y) {
            return std::unexpected(scenario_error(
                DiagnosticCode::ir_invalid_bounds,
                "Wrap bounds must have positive width and height",
                source_name,
                pointer + "/parameters/bounds"));
        }
        result.system.parameters.bounds = {*minimum, *maximum};
    }

    if (const auto after = system.find("after"); after != system.end()) {
        if (!after->is_array()) {
            return std::unexpected(scenario_error(
                DiagnosticCode::ir_schema_invalid, "after must be an array", source_name, pointer + "/after"));
        }
        for (std::size_t dependency = 0U; dependency < after->size(); ++dependency) {
            if (!(*after)[dependency].is_string() || (*after)[dependency].get_ref<const std::string&>().empty()) {
                return std::unexpected(scenario_error(
                    DiagnosticCode::ir_schema_invalid,
                    "System dependency must be a non-empty string",
                    source_name,
                    pointer + "/after/" + std::to_string(dependency)));
            }
            result.after_names.push_back((*after)[dependency].get<std::string>());
        }
    }
    return result;
}

bool reachable(
    const std::size_t from,
    const std::size_t target,
    const std::vector<std::vector<std::size_t>>& outgoing) {
    std::vector<bool> visited(outgoing.size(), false);
    std::vector<std::size_t> stack{from};
    while (!stack.empty()) {
        const auto current = stack.back();
        stack.pop_back();
        if (current == target) {
            return true;
        }
        if (visited[current]) {
            continue;
        }
        visited[current] = true;
        stack.insert(stack.end(), outgoing[current].begin(), outgoing[current].end());
    }
    return false;
}

Result<void> resolve_and_order_systems(
    ExecutionPlan& plan,
    std::vector<UnresolvedSystem>& unresolved,
    const std::string_view source_name) {
    const auto count = unresolved.size();
    std::vector<std::vector<std::size_t>> outgoing(count);
    std::vector<std::uint32_t> indegree(count, 0U);
    for (std::size_t system_index = 0U; system_index < count; ++system_index) {
        for (const auto& dependency_name : unresolved[system_index].after_names) {
            const auto found = std::find_if(unresolved.begin(), unresolved.end(), [&](const auto& candidate) {
                return plan.symbol(candidate.system.symbol) == dependency_name;
            });
            if (found == unresolved.end()) {
                return std::unexpected(scenario_error(
                    DiagnosticCode::ir_schema_invalid,
                    "System dependency names an unknown system",
                    source_name,
                    "/systems/" + std::to_string(system_index) + "/after"));
            }
            const auto dependency_index = static_cast<std::size_t>(std::distance(unresolved.begin(), found));
            if (unresolved[dependency_index].system.phase != unresolved[system_index].system.phase) {
                return std::unexpected(scenario_error(
                    DiagnosticCode::ir_schema_invalid,
                    "System dependency cannot cross execution phases",
                    source_name,
                    "/systems/" + std::to_string(system_index) + "/after"));
            }
            if (std::find(outgoing[dependency_index].begin(), outgoing[dependency_index].end(), system_index) !=
                outgoing[dependency_index].end()) {
                return std::unexpected(scenario_error(
                    DiagnosticCode::ir_schema_invalid,
                    "System dependency is duplicated",
                    source_name,
                    "/systems/" + std::to_string(system_index) + "/after"));
            }
            outgoing[dependency_index].push_back(system_index);
            ++indegree[system_index];
        }
    }

    for (std::size_t first = 0U; first < count; ++first) {
        for (std::size_t second = first + 1U; second < count; ++second) {
            if (unresolved[first].system.phase != unresolved[second].system.phase) {
                continue;
            }
            const auto first_writes = operation_descriptor(unresolved[first].system.operation).write_mask;
            const auto second_writes = operation_descriptor(unresolved[second].system.operation).write_mask;
            if ((first_writes & second_writes) == 0U) {
                continue;
            }
            if (!reachable(first, second, outgoing) && !reachable(second, first, outgoing)) {
                return std::unexpected(scenario_error(
                    DiagnosticCode::ir_ambiguous_write_order,
                    "Systems in the same phase write the same component without an explicit dependency order",
                    source_name,
                    "/systems"));
            }
        }
    }

    std::vector<std::size_t> ordered{};
    ordered.reserve(count);
    for (const auto phase : {PhaseId::fixed_update, PhaseId::post_update}) {
        while (true) {
            std::size_t selected = count;
            for (std::size_t index = 0U; index < count; ++index) {
                if (unresolved[index].system.phase != phase || indegree[index] != 0U ||
                    std::find(ordered.begin(), ordered.end(), index) != ordered.end()) {
                    continue;
                }
                selected = index;
                break;
            }
            if (selected == count) {
                break;
            }
            ordered.push_back(selected);
            for (const auto next : outgoing[selected]) {
                --indegree[next];
            }
        }
    }
    if (ordered.size() != count) {
        return std::unexpected(scenario_error(
            DiagnosticCode::ir_dependency_cycle,
            "System dependency graph contains a cycle",
            source_name,
            "/systems"));
    }

    std::vector<std::uint32_t> author_to_ordered(count, 0U);
    for (std::size_t ordered_index = 0U; ordered_index < ordered.size(); ++ordered_index) {
        author_to_ordered[ordered[ordered_index]] = static_cast<std::uint32_t>(ordered_index);
    }
    plan.systems.reserve(count);
    for (const auto author_index : ordered) {
        auto system = std::move(unresolved[author_index].system);
        system.after_system_indices.reserve(unresolved[author_index].after_names.size());
        for (const auto& dependency_name : unresolved[author_index].after_names) {
            const auto found = std::find_if(unresolved.begin(), unresolved.end(), [&](const auto& candidate) {
                return plan.symbol(candidate.system.symbol) == dependency_name;
            });
            const auto dependency_index = static_cast<std::size_t>(std::distance(unresolved.begin(), found));
            system.after_system_indices.push_back(author_to_ordered[dependency_index]);
        }
        std::sort(system.after_system_indices.begin(), system.after_system_indices.end());
        plan.systems.push_back(std::move(system));
    }
    return {};
}

Result<ExecutionPlan> compile_document(
    const Json& document,
    const std::string_view canonical_source,
    const std::string_view source_name) {
    if (!document.is_object()) {
        return std::unexpected(scenario_error(
            DiagnosticCode::ir_schema_invalid, "ScenarioSpec root must be an object", source_name, "/"));
    }
    if (auto fields = reject_unknown_members(
            document,
            {"schema_version", "name", "seed", "world", "camera", "textures", "spawn_groups", "systems", "benchmark"},
            source_name,
            "");
        !fields) {
        return std::unexpected(std::move(fields.error()));
    }
    auto version_member = required_member(document, "schema_version", source_name, "");
    if (!version_member) {
        return std::unexpected(std::move(version_member.error()));
    }
    if (!(*version_member)->is_string() ||
        (*version_member)->get_ref<const std::string&>() != ExecutionPlan::supported_schema_version) {
        return std::unexpected(scenario_error(
            DiagnosticCode::ir_schema_version_unsupported,
            "Unsupported ScenarioSpec schema_version; this runtime accepts only 0.1",
            source_name,
            "/schema_version"));
    }

    ExecutionPlan plan{};
    SymbolTable symbols{plan.symbols};
    auto name = required_string(document, "name", source_name, "");
    if (!name) {
        return std::unexpected(std::move(name.error()));
    }
    plan.scenario_name = symbols.intern(*name);
    auto seed_member = required_member(document, "seed", source_name, "");
    if (!seed_member) {
        return std::unexpected(std::move(seed_member.error()));
    }
    if (!(*seed_member)->is_number_unsigned() && !(*seed_member)->is_number_integer()) {
        return std::unexpected(scenario_error(
            DiagnosticCode::ir_schema_invalid, "seed must be a non-negative integer", source_name, "/seed"));
    }
    const auto seed = (*seed_member)->get<std::int64_t>();
    if (seed < 0) {
        return std::unexpected(scenario_error(
            DiagnosticCode::ir_schema_invalid, "seed must be a non-negative integer", source_name, "/seed"));
    }
    plan.seed = static_cast<std::uint64_t>(seed);

    auto world_member = required_member(document, "world", source_name, "");
    if (!world_member) {
        return std::unexpected(std::move(world_member.error()));
    }
    if (auto fields = reject_unknown_members(**world_member, {"capacity"}, source_name, "/world"); !fields) {
        return std::unexpected(std::move(fields.error()));
    }
    auto capacity = required_u32(**world_member, "capacity", 1U, 1'000'000U, source_name, "/world");
    if (!capacity) {
        return std::unexpected(std::move(capacity.error()));
    }
    plan.world_capacity = *capacity;

    auto camera_member = required_member(document, "camera", source_name, "");
    if (!camera_member) {
        return std::unexpected(std::move(camera_member.error()));
    }
    if (auto fields = reject_unknown_members(
            **camera_member, {"position", "half_extent"}, source_name, "/camera");
        !fields) {
        return std::unexpected(std::move(fields.error()));
    }
    auto camera_position_member = required_member(**camera_member, "position", source_name, "/camera");
    if (!camera_position_member) {
        return std::unexpected(std::move(camera_position_member.error()));
    }
    auto camera_position = vec2_value(**camera_position_member, source_name, "/camera/position");
    if (!camera_position) {
        return std::unexpected(std::move(camera_position.error()));
    }
    auto camera_extent_member = required_member(**camera_member, "half_extent", source_name, "/camera");
    if (!camera_extent_member) {
        return std::unexpected(std::move(camera_extent_member.error()));
    }
    auto camera_extent = vec2_value(**camera_extent_member, source_name, "/camera/half_extent");
    if (!camera_extent) {
        return std::unexpected(std::move(camera_extent.error()));
    }
    if (camera_extent->x <= 0.0F || camera_extent->y <= 0.0F) {
        return std::unexpected(scenario_error(
            DiagnosticCode::ir_invalid_bounds,
            "Camera half_extent must be positive",
            source_name,
            "/camera/half_extent"));
    }
    plan.camera_position = *camera_position;
    plan.camera_half_extent = *camera_extent;

    auto textures_member = required_member(document, "textures", source_name, "");
    if (!textures_member) {
        return std::unexpected(std::move(textures_member.error()));
    }
    if (!(*textures_member)->is_array() || (*textures_member)->size() > 64U) {
        return std::unexpected(scenario_error(
            DiagnosticCode::ir_schema_invalid,
            "textures must be an array with at most 64 entries",
            source_name,
            "/textures"));
    }
    plan.textures.reserve((*textures_member)->size());
    for (std::size_t index = 0U; index < (*textures_member)->size(); ++index) {
        auto texture = parse_texture((**textures_member)[index], index, plan, symbols, source_name);
        if (!texture) {
            return std::unexpected(std::move(texture.error()));
        }
        plan.textures.push_back(*texture);
    }

    auto spawns_member = required_member(document, "spawn_groups", source_name, "");
    if (!spawns_member) {
        return std::unexpected(std::move(spawns_member.error()));
    }
    if (!(*spawns_member)->is_array() || (*spawns_member)->size() > 256U) {
        return std::unexpected(scenario_error(
            DiagnosticCode::ir_schema_invalid,
            "spawn_groups must be an array with at most 256 entries",
            source_name,
            "/spawn_groups"));
    }
    plan.spawn_groups.reserve((*spawns_member)->size());
    std::uint64_t total_spawn_count = 0U;
    for (std::size_t index = 0U; index < (*spawns_member)->size(); ++index) {
        auto spawn = parse_spawn_group((**spawns_member)[index], index, plan, symbols, source_name);
        if (!spawn) {
            return std::unexpected(std::move(spawn.error()));
        }
        total_spawn_count += spawn->count;
        if (total_spawn_count > plan.world_capacity) {
            return std::unexpected(scenario_error(
                DiagnosticCode::ir_capacity_exceeded,
                "Spawn groups exceed the reserved world capacity",
                source_name,
                "/spawn_groups"));
        }
        plan.spawn_groups.push_back(*spawn);
    }
    plan.total_spawn_count = static_cast<std::uint32_t>(total_spawn_count);

    auto systems_member = required_member(document, "systems", source_name, "");
    if (!systems_member) {
        return std::unexpected(std::move(systems_member.error()));
    }
    if (!(*systems_member)->is_array() || (*systems_member)->size() > 64U) {
        return std::unexpected(scenario_error(
            DiagnosticCode::ir_schema_invalid,
            "systems must be an array with at most 64 entries",
            source_name,
            "/systems"));
    }
    std::vector<UnresolvedSystem> unresolved{};
    unresolved.reserve((*systems_member)->size());
    for (std::size_t index = 0U; index < (*systems_member)->size(); ++index) {
        auto system = parse_system((**systems_member)[index], index, plan, symbols, source_name, unresolved);
        if (!system) {
            return std::unexpected(std::move(system.error()));
        }
        unresolved.push_back(std::move(*system));
    }
    if (auto ordering = resolve_and_order_systems(plan, unresolved, source_name); !ordering) {
        return std::unexpected(std::move(ordering.error()));
    }

    auto benchmark_member = required_member(document, "benchmark", source_name, "");
    if (!benchmark_member) {
        return std::unexpected(std::move(benchmark_member.error()));
    }
    if (auto fields = reject_unknown_members(
            **benchmark_member, {"runs", "warmup_frames", "measurement_frames"}, source_name, "/benchmark");
        !fields) {
        return std::unexpected(std::move(fields.error()));
    }
    auto runs = required_u32(**benchmark_member, "runs", 1U, 100U, source_name, "/benchmark");
    if (!runs) {
        return std::unexpected(std::move(runs.error()));
    }
    auto warmup = required_u32(**benchmark_member, "warmup_frames", 0U, 1'000'000U, source_name, "/benchmark");
    if (!warmup) {
        return std::unexpected(std::move(warmup.error()));
    }
    auto measurement = required_u32(
        **benchmark_member, "measurement_frames", 1U, 1'000'000U, source_name, "/benchmark");
    if (!measurement) {
        return std::unexpected(std::move(measurement.error()));
    }
    plan.benchmark = {*runs, *warmup, *measurement};
    plan.scenario_hash = hash_text(canonical_source);
    plan.plan_hash = hash_plan(plan);
    if (auto validated = validate_execution_plan(plan); !validated) {
        return std::unexpected(std::move(validated.error()));
    }
    return plan;
}

void write_vec2(JsonWriter& writer, const Vec2 value) {
    writer.begin_array();
    writer.value(static_cast<double>(value.x));
    writer.value(static_cast<double>(value.y));
    writer.end_array();
}

void write_color(JsonWriter& writer, const Color value) {
    writer.begin_array();
    writer.value(static_cast<double>(value.r));
    writer.value(static_cast<double>(value.g));
    writer.value(static_cast<double>(value.b));
    writer.value(static_cast<double>(value.a));
    writer.end_array();
}

void write_component_set(JsonWriter& writer, const std::uint8_t mask) {
    writer.begin_array();
    for (const auto component : {ComponentId::transform2d, ComponentId::velocity2d, ComponentId::sprite2d}) {
        if ((mask & component_bit(component)) != 0U) {
            writer.value(to_string(component));
        }
    }
    writer.end_array();
}

} // namespace

std::string_view ExecutionPlan::symbol(const SymbolId id) const noexcept {
    if (id >= symbols.size()) {
        return {};
    }
    return symbols[id];
}

Result<void> validate_execution_plan(const ExecutionPlan& plan) {
    const auto invalid = [](const DiagnosticCode code, const std::string_view message) -> Result<void> {
        return std::unexpected(Diagnostic::make(code, Severity::error, "scenario", std::string{message}));
    };
    const auto finite_vec2 = [](const Vec2 value) noexcept {
        return std::isfinite(value.x) && std::isfinite(value.y);
    };
    const auto valid_color = [](const Color value) noexcept {
        return std::isfinite(value.r) && std::isfinite(value.g) && std::isfinite(value.b) &&
               std::isfinite(value.a) && value.r >= 0.0F && value.r <= 1.0F &&
               value.g >= 0.0F && value.g <= 1.0F && value.b >= 0.0F && value.b <= 1.0F &&
               value.a >= 0.0F && value.a <= 1.0F;
    };
    const auto valid_symbol = [&plan](const SymbolId symbol) noexcept {
        return symbol < plan.symbols.size() && !plan.symbols[symbol].empty();
    };
    const auto unique_symbols = [](const auto& items) noexcept {
        for (std::size_t left = 0U; left < items.size(); ++left) {
            for (std::size_t right = left + 1U; right < items.size(); ++right) {
                if (items[left].symbol == items[right].symbol) {
                    return false;
                }
            }
        }
        return true;
    };

    if (plan.symbols.empty() || !valid_symbol(plan.scenario_name)) {
        return invalid(DiagnosticCode::ir_schema_invalid, "ExecutionPlan has an invalid scenario symbol");
    }
    for (std::size_t left = 0U; left < plan.symbols.size(); ++left) {
        if (plan.symbols[left].empty()) {
            return invalid(DiagnosticCode::ir_schema_invalid, "ExecutionPlan contains an empty symbol");
        }
        for (std::size_t right = left + 1U; right < plan.symbols.size(); ++right) {
            if (plan.symbols[left] == plan.symbols[right]) {
                return invalid(DiagnosticCode::ir_schema_invalid, "ExecutionPlan contains duplicate symbols");
            }
        }
    }
    if (plan.world_capacity == 0U || plan.world_capacity > 1'000'000U) {
        return invalid(DiagnosticCode::ir_capacity_exceeded, "ExecutionPlan world capacity is outside the supported range");
    }
    if (!finite_vec2(plan.camera_position) || !finite_vec2(plan.camera_half_extent) ||
        plan.camera_half_extent.x <= 0.0F || plan.camera_half_extent.y <= 0.0F) {
        return invalid(DiagnosticCode::ir_invalid_bounds, "ExecutionPlan camera is non-finite or has invalid extents");
    }

    if (plan.textures.size() > 64U || !unique_symbols(plan.textures)) {
        return invalid(DiagnosticCode::ir_schema_invalid, "ExecutionPlan texture table is invalid");
    }
    for (const auto& texture : plan.textures) {
        const auto generator = static_cast<std::uint8_t>(texture.generator);
        if (!valid_symbol(texture.symbol) || generator > static_cast<std::uint8_t>(TextureGeneratorId::checker) ||
            texture.width == 0U || texture.width > 4096U || texture.height == 0U || texture.height > 4096U ||
            texture.cell_size == 0U || texture.cell_size > 4096U ||
            !valid_color(texture.first) || !valid_color(texture.second)) {
            return invalid(DiagnosticCode::ir_schema_invalid, "ExecutionPlan contains an invalid texture asset");
        }
    }

    if (plan.spawn_groups.size() > 256U || !unique_symbols(plan.spawn_groups)) {
        return invalid(DiagnosticCode::ir_schema_invalid, "ExecutionPlan spawn-group table is invalid");
    }
    std::uint64_t total_spawn_count = 0U;
    std::uint64_t velocity_spawn_count = 0U;
    for (const auto& group : plan.spawn_groups) {
        const auto placement = static_cast<std::uint8_t>(group.placement.kind);
        if (!valid_symbol(group.symbol) || group.count == 0U || group.count > 1'000'000U ||
            placement > static_cast<std::uint8_t>(PlacementId::seeded_random) ||
            !finite_vec2(group.placement.origin) || !finite_vec2(group.placement.spacing) ||
            !finite_vec2(group.placement.random_bounds.min) || !finite_vec2(group.placement.random_bounds.max) ||
            !finite_vec2(group.transform.position_offset) || !std::isfinite(group.transform.rotation) ||
            !finite_vec2(group.transform.scale) || group.transform.scale.x <= 0.0F ||
            group.transform.scale.y <= 0.0F || !finite_vec2(group.velocity.linear) ||
            !std::isfinite(group.velocity.angular) || !finite_vec2(group.sprite.size) ||
            !finite_vec2(group.sprite.pivot) || !valid_color(group.sprite.tint)) {
            return invalid(DiagnosticCode::ir_schema_invalid, "ExecutionPlan contains an invalid spawn group");
        }
        if (group.placement.kind == PlacementId::grid &&
            (group.placement.columns == 0U || group.placement.columns > 1'000'000U)) {
            return invalid(DiagnosticCode::ir_schema_invalid, "ExecutionPlan grid placement has invalid columns");
        }
        if (group.placement.kind == PlacementId::seeded_random &&
            (group.placement.random_bounds.min.x >= group.placement.random_bounds.max.x ||
             group.placement.random_bounds.min.y >= group.placement.random_bounds.max.y)) {
            return invalid(DiagnosticCode::ir_invalid_bounds, "ExecutionPlan random placement bounds are invalid");
        }
        if (group.has_sprite &&
            (group.sprite.texture_asset >= plan.textures.size() || group.sprite.size.x <= 0.0F ||
             group.sprite.size.y <= 0.0F)) {
            return invalid(DiagnosticCode::ir_missing_texture, "ExecutionPlan sprite initializer is invalid");
        }
        total_spawn_count += group.count;
        velocity_spawn_count += group.has_velocity ? group.count : 0U;
        if (total_spawn_count > plan.world_capacity) {
            return invalid(DiagnosticCode::ir_capacity_exceeded, "ExecutionPlan spawn groups exceed world capacity");
        }
    }
    if (total_spawn_count != plan.total_spawn_count) {
        return invalid(DiagnosticCode::ir_schema_invalid, "ExecutionPlan total spawn count does not match its groups");
    }

    if (plan.systems.size() > 64U || !unique_symbols(plan.systems)) {
        return invalid(DiagnosticCode::ir_schema_invalid, "ExecutionPlan system table exceeds the runtime limit or has duplicates");
    }
    std::uint8_t previous_phase = 0U;
    for (std::size_t index = 0U; index < plan.systems.size(); ++index) {
        const auto& system = plan.systems[index];
        const auto operation = static_cast<std::uint8_t>(system.operation);
        const auto phase = static_cast<std::uint8_t>(system.phase);
        if (!valid_symbol(system.symbol) || operation >= descriptors.size() ||
            phase > static_cast<std::uint8_t>(PhaseId::post_update) || (index > 0U && phase < previous_phase)) {
            return invalid(DiagnosticCode::ir_schema_invalid, "ExecutionPlan contains an invalid system descriptor");
        }
        previous_phase = phase;
        const auto& descriptor = descriptors[operation];
        if ((descriptor.legal_phase_mask & phase_bit(system.phase)) == 0U) {
            return invalid(DiagnosticCode::ir_schema_invalid, "ExecutionPlan operation is illegal in its phase");
        }
        const auto expected_cardinality = system.operation == OperationId::integrate_velocity
                                              ? velocity_spawn_count
                                              : total_spawn_count;
        if (system.estimated_cardinality != expected_cardinality) {
            return invalid(DiagnosticCode::ir_schema_invalid, "ExecutionPlan system cardinality is inconsistent");
        }
        for (std::size_t dependency_index = 0U;
             dependency_index < system.after_system_indices.size();
             ++dependency_index) {
            const auto dependency = system.after_system_indices[dependency_index];
            if (dependency >= index ||
                (dependency_index > 0U && dependency <= system.after_system_indices[dependency_index - 1U])) {
                return invalid(DiagnosticCode::ir_dependency_cycle, "ExecutionPlan system dependencies are not topologically ordered");
            }
        }
        if (system.operation == OperationId::integrate_velocity) {
            if (!std::isfinite(system.parameters.delta_seconds) || system.parameters.delta_seconds <= 0.0F) {
                return invalid(DiagnosticCode::ir_schema_invalid, "ExecutionPlan integration delta is invalid");
            }
        } else {
            const auto& bounds = system.parameters.bounds;
            if (!finite_vec2(bounds.min) || !finite_vec2(bounds.max) ||
                bounds.min.x >= bounds.max.x || bounds.min.y >= bounds.max.y) {
                return invalid(DiagnosticCode::ir_invalid_bounds, "ExecutionPlan wrap bounds are invalid");
            }
        }
    }
    if (plan.benchmark.runs == 0U || plan.benchmark.runs > 100U ||
        plan.benchmark.warmup_frames > 1'000'000U || plan.benchmark.measurement_frames == 0U ||
        plan.benchmark.measurement_frames > 1'000'000U) {
        return invalid(DiagnosticCode::ir_schema_invalid, "ExecutionPlan benchmark configuration is invalid");
    }
    if (plan.plan_hash != 0U && plan.plan_hash != hash_plan(plan)) {
        return invalid(DiagnosticCode::ir_schema_invalid, "ExecutionPlan contents do not match its plan hash");
    }
    return {};
}

std::uint64_t compute_execution_plan_hash(const ExecutionPlan& plan) noexcept { return hash_plan(plan); }

std::span<const OperationDescriptor> operation_descriptors() noexcept { return descriptors; }

const OperationDescriptor& operation_descriptor(const OperationId operation) noexcept {
    const auto index = static_cast<std::size_t>(operation);
    return index < descriptors.size() ? descriptors[index] : descriptors.front();
}

std::string_view to_string(const ComponentId component) noexcept {
    switch (component) {
    case ComponentId::transform2d: return "Transform2D";
    case ComponentId::velocity2d: return "Velocity2D";
    case ComponentId::sprite2d: return "Sprite2D";
    }
    return "Transform2D";
}

std::string_view to_string(const OperationId operation) noexcept {
    const auto index = static_cast<std::size_t>(operation);
    return index < descriptors.size() ? descriptors[index].name : "unknown";
}

std::string_view to_string(const PhaseId phase) noexcept {
    switch (phase) {
    case PhaseId::fixed_update: return "fixed_update";
    case PhaseId::post_update: return "post_update";
    }
    return "fixed_update";
}

std::string_view to_string(const PlacementId placement) noexcept {
    switch (placement) {
    case PlacementId::grid: return "grid";
    case PlacementId::seeded_random: return "seeded_random";
    }
    return "grid";
}

std::string_view to_string(const TextureGeneratorId generator) noexcept {
    switch (generator) {
    case TextureGeneratorId::solid: return "solid";
    case TextureGeneratorId::checker: return "checker";
    }
    return "solid";
}

Result<ExecutionPlan> compile_scenario_text(const std::string_view source, const std::string_view source_name) {
    try {
        const auto document = Json::parse(source.begin(), source.end());
        return compile_document(document, document.dump(), source_name);
    } catch (const Json::parse_error& error) {
        auto diagnostic = scenario_error(
            DiagnosticCode::ir_schema_invalid, "ScenarioSpec is not valid JSON", source_name, "/");
        diagnostic.context.push_back({"parse_error", std::string{error.what()}});
        return std::unexpected(std::move(diagnostic));
    } catch (const Json::exception& error) {
        auto diagnostic = scenario_error(
            DiagnosticCode::ir_schema_invalid, "ScenarioSpec value has an invalid type or range", source_name, "/");
        diagnostic.context.push_back({"parser_error", std::string{error.what()}});
        return std::unexpected(std::move(diagnostic));
    }
}

Result<ExecutionPlan> compile_scenario_file(const std::filesystem::path& path) {
    std::ifstream stream{path, std::ios::binary};
    if (!stream) {
        auto diagnostic = scenario_error(
            DiagnosticCode::input_invalid, "ScenarioSpec file could not be opened", path.string());
        diagnostic.suggestions.push_back("Check that the scenario path exists and is readable.");
        return std::unexpected(std::move(diagnostic));
    }
    const std::string source{std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
    return compile_scenario_text(source, path.string());
}

void write_execution_plan_json(JsonWriter& writer, const ExecutionPlan& plan) {
    writer.begin_object();
    writer.key("schema_version");
    writer.value(ExecutionPlan::supported_schema_version);
    writer.key("name");
    writer.value(plan.symbol(plan.scenario_name));
    writer.key("scenario_hash");
    writer.value(plan.scenario_hash);
    writer.key("plan_hash");
    writer.value(plan.plan_hash);
    writer.key("world_capacity");
    writer.value(static_cast<std::uint64_t>(plan.world_capacity));
    writer.key("total_spawn_count");
    writer.value(static_cast<std::uint64_t>(plan.total_spawn_count));
    writer.key("ordered_systems");
    writer.begin_array();
    for (std::size_t index = 0U; index < plan.systems.size(); ++index) {
        const auto& system = plan.systems[index];
        const auto& descriptor = operation_descriptor(system.operation);
        writer.begin_object();
        writer.key("index");
        writer.value(static_cast<std::uint64_t>(index));
        writer.key("id");
        writer.value(plan.symbol(system.symbol));
        writer.key("phase");
        writer.value(to_string(system.phase));
        writer.key("operation");
        writer.value(descriptor.name);
        writer.key("query");
        write_component_set(writer, descriptor.query_mask);
        writer.key("reads");
        write_component_set(writer, descriptor.read_mask);
        writer.key("writes");
        write_component_set(writer, descriptor.write_mask);
        writer.key("structural_effects");
        writer.value(descriptor.structural_effects);
        writer.key("complexity");
        writer.value(descriptor.complexity);
        writer.key("allocation_permitted");
        writer.value(descriptor.allocation_permitted);
        writer.key("estimated_cardinality");
        writer.value(static_cast<std::uint64_t>(system.estimated_cardinality));
        writer.key("after_indices");
        writer.begin_array();
        for (const auto dependency : system.after_system_indices) {
            writer.value(static_cast<std::uint64_t>(dependency));
        }
        writer.end_array();
        writer.key("parameters");
        writer.begin_object();
        if (system.operation == OperationId::integrate_velocity) {
            writer.key("delta_seconds");
            writer.value(static_cast<double>(system.parameters.delta_seconds));
        } else {
            writer.key("bounds");
            writer.begin_object();
            writer.key("min");
            write_vec2(writer, system.parameters.bounds.min);
            writer.key("max");
            write_vec2(writer, system.parameters.bounds.max);
            writer.end_object();
        }
        writer.end_object();
        writer.end_object();
    }
    writer.end_array();
    writer.end_object();
}

void write_scenario_summary_json(JsonWriter& writer, const ExecutionPlan& plan) {
    writer.begin_object();
    writer.key("schema_version");
    writer.value(ExecutionPlan::supported_schema_version);
    writer.key("name");
    writer.value(plan.symbol(plan.scenario_name));
    writer.key("seed");
    writer.value(plan.seed);
    writer.key("scenario_hash");
    writer.value(plan.scenario_hash);
    writer.key("plan_hash");
    writer.value(plan.plan_hash);
    writer.key("world_capacity");
    writer.value(static_cast<std::uint64_t>(plan.world_capacity));
    writer.key("camera");
    writer.begin_object();
    writer.key("position");
    write_vec2(writer, plan.camera_position);
    writer.key("half_extent");
    write_vec2(writer, plan.camera_half_extent);
    writer.end_object();
    writer.key("textures");
    writer.begin_array();
    for (const auto& texture : plan.textures) {
        writer.begin_object();
        writer.key("id");
        writer.value(plan.symbol(texture.symbol));
        writer.key("kind");
        writer.value(to_string(texture.generator));
        writer.key("width");
        writer.value(static_cast<std::uint64_t>(texture.width));
        writer.key("height");
        writer.value(static_cast<std::uint64_t>(texture.height));
        writer.key("color");
        write_color(writer, texture.first);
        writer.key("color2");
        write_color(writer, texture.second);
        writer.end_object();
    }
    writer.end_array();
    writer.key("spawn_groups");
    writer.begin_array();
    for (const auto& group : plan.spawn_groups) {
        writer.begin_object();
        writer.key("id");
        writer.value(plan.symbol(group.symbol));
        writer.key("count");
        writer.value(static_cast<std::uint64_t>(group.count));
        writer.key("placement");
        writer.value(to_string(group.placement.kind));
        writer.key("velocity");
        writer.value(group.has_velocity);
        writer.key("sprite");
        writer.value(group.has_sprite);
        if (group.has_sprite) {
            writer.key("texture_index");
            writer.value(static_cast<std::uint64_t>(group.sprite.texture_asset));
        }
        writer.end_object();
    }
    writer.end_array();
    writer.key("system_count");
    writer.value(static_cast<std::uint64_t>(plan.systems.size()));
    writer.key("benchmark");
    writer.begin_object();
    writer.key("runs");
    writer.value(static_cast<std::uint64_t>(plan.benchmark.runs));
    writer.key("warmup_frames");
    writer.value(static_cast<std::uint64_t>(plan.benchmark.warmup_frames));
    writer.key("measurement_frames");
    writer.value(static_cast<std::uint64_t>(plan.benchmark.measurement_frames));
    writer.end_object();
    writer.end_object();
}

void write_diagnostics_schema_json(JsonWriter& writer) {
    writer.begin_object();
    writer.key("schema_version");
    writer.value(std::uint64_t{1U});
    writer.key("required_fields");
    writer.begin_array();
    for (const auto field : {"code", "severity", "subsystem", "message", "context", "suggestions", "source"}) {
        writer.value(field);
    }
    writer.end_array();
    writer.key("scenario_codes");
    writer.begin_array();
    for (const auto code : {
             DiagnosticCode::ir_schema_invalid,
             DiagnosticCode::ir_schema_version_unsupported,
             DiagnosticCode::ir_unknown_operation,
             DiagnosticCode::ir_access_mismatch,
             DiagnosticCode::ir_dependency_cycle,
             DiagnosticCode::ir_ambiguous_write_order,
             DiagnosticCode::ir_capacity_exceeded,
             DiagnosticCode::ir_missing_texture,
             DiagnosticCode::ir_invalid_bounds,
         }) {
        writer.value(to_string(code));
    }
    writer.end_array();
    writer.end_object();
}

} // namespace ai2d
