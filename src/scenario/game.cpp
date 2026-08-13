#include "ai2d/scenario/game.hpp"

#include "ai2d/foundation/preference_path.hpp"

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
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace ai2d {
namespace {

using Json = nlohmann::json;

struct PrefabSource final {
    std::string id{};
    std::filesystem::path path{};
    Json document{};
};

Diagnostic game_error(
    const DiagnosticCode code,
    std::string message,
    const std::string_view source,
    const std::string_view pointer = {}) {
    auto diagnostic = Diagnostic::make(code, Severity::error, "game", std::move(message));
    diagnostic.context.push_back({"source", std::string{source}});
    if (!pointer.empty()) {
        diagnostic.context.push_back({"json_pointer", std::string{pointer}});
    }
    return diagnostic;
}

Result<void> reject_unknown(
    const Json& object,
    const std::initializer_list<std::string_view> allowed,
    const std::string_view source,
    const std::string_view pointer,
    const DiagnosticCode code = DiagnosticCode::game_manifest_invalid) {
    if (!object.is_object()) {
        return std::unexpected(game_error(
            code, "Expected a JSON object", source, pointer));
    }
    for (const auto& [key, value] : object.items()) {
        (void)value;
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end()) {
            return std::unexpected(game_error(
                code,
                "Object contains an unknown field",
                source,
                std::string{pointer} + "/" + key));
        }
    }
    return {};
}

Result<const Json*> required(
    const Json& object,
    const std::string_view key,
    const std::string_view source,
    const std::string_view pointer,
    const DiagnosticCode code = DiagnosticCode::game_manifest_invalid) {
    if (!object.is_object()) {
        return std::unexpected(game_error(code, "Expected a JSON object", source, pointer));
    }
    const auto iterator = object.find(std::string{key});
    if (iterator == object.end()) {
        return std::unexpected(game_error(
            code, "Missing required field", source, std::string{pointer} + "/" + std::string{key}));
    }
    return &*iterator;
}

const Json* optional(const Json& object, const std::string_view key) noexcept {
    if (!object.is_object()) {
        return nullptr;
    }
    const auto iterator = object.find(std::string{key});
    return iterator == object.end() ? nullptr : &*iterator;
}

Result<std::string> string_value(
    const Json& object,
    const std::string_view key,
    const std::string_view source,
    const std::string_view pointer,
    const DiagnosticCode code = DiagnosticCode::game_manifest_invalid) {
    auto member = required(object, key, source, pointer, code);
    if (!member) {
        return std::unexpected(std::move(member.error()));
    }
    if (!(*member)->is_string() || (*member)->get_ref<const std::string&>().empty()) {
        return std::unexpected(game_error(
            code,
            "Expected a non-empty string",
            source,
            std::string{pointer} + "/" + std::string{key}));
    }
    return (*member)->get<std::string>();
}

Result<std::uint32_t> u32_value(
    const Json& object,
    const std::string_view key,
    const std::uint32_t minimum,
    const std::uint32_t maximum,
    const std::string_view source,
    const std::string_view pointer,
    const DiagnosticCode code = DiagnosticCode::game_manifest_invalid) {
    auto member = required(object, key, source, pointer, code);
    if (!member) {
        return std::unexpected(std::move(member.error()));
    }
    if (!(*member)->is_number_integer() && !(*member)->is_number_unsigned()) {
        return std::unexpected(game_error(
            code, "Expected an integer", source, std::string{pointer} + "/" + std::string{key}));
    }
    const auto value = (*member)->get<std::int64_t>();
    if (value < static_cast<std::int64_t>(minimum) || value > static_cast<std::int64_t>(maximum)) {
        return std::unexpected(game_error(
            code, "Integer is outside the supported range", source,
            std::string{pointer} + "/" + std::string{key}));
    }
    return static_cast<std::uint32_t>(value);
}

Result<std::uint64_t> u64_value(
    const Json& object,
    const std::string_view key,
    const std::uint64_t minimum,
    const std::uint64_t maximum,
    const std::string_view source,
    const std::string_view pointer,
    const DiagnosticCode code = DiagnosticCode::game_manifest_invalid) {
    auto member = required(object, key, source, pointer, code);
    if (!member) return std::unexpected(std::move(member.error()));
    if (!(*member)->is_number_integer() && !(*member)->is_number_unsigned()) {
        return std::unexpected(game_error(
            code, "Expected an unsigned integer", source,
            std::string{pointer} + "/" + std::string{key}));
    }
    if ((*member)->is_number_unsigned()) {
        const auto value = (*member)->get<std::uint64_t>();
        if (value < minimum || value > maximum) {
            return std::unexpected(game_error(
                code, "Integer is outside the supported range", source,
                std::string{pointer} + "/" + std::string{key}));
        }
        return value;
    }
    if ((*member)->is_number_integer()) {
        const auto value = (*member)->get<std::int64_t>();
        if (value < 0 || static_cast<std::uint64_t>(value) < minimum ||
            static_cast<std::uint64_t>(value) > maximum) {
            return std::unexpected(game_error(
                code, "Integer is outside the supported range", source,
                std::string{pointer} + "/" + std::string{key}));
        }
        return static_cast<std::uint64_t>(value);
    }
    return std::unexpected(game_error(
        code, "Expected an unsigned integer", source,
        std::string{pointer} + "/" + std::string{key}));
}

Result<std::int32_t> i32_value(
    const Json& object,
    const std::string_view key,
    const std::int32_t minimum,
    const std::int32_t maximum,
    const std::string_view source,
    const std::string_view pointer,
    const DiagnosticCode code = DiagnosticCode::game_manifest_invalid) {
    auto member = required(object, key, source, pointer, code);
    if (!member) {
        return std::unexpected(std::move(member.error()));
    }
    if (!(*member)->is_number_integer() && !(*member)->is_number_unsigned()) {
        return std::unexpected(game_error(
            code, "Expected an integer", source, std::string{pointer} + "/" + std::string{key}));
    }
    if ((*member)->is_number_unsigned()) {
        const auto value = (*member)->get<std::uint64_t>();
        if (maximum < 0 || (minimum > 0 && value < static_cast<std::uint64_t>(minimum)) ||
            value > static_cast<std::uint64_t>(maximum)) {
            return std::unexpected(game_error(
                code, "Integer is outside the supported range", source,
                std::string{pointer} + "/" + std::string{key}));
        }
        return static_cast<std::int32_t>(value);
    }
    const auto value = (*member)->get<std::int64_t>();
    if (value < minimum || value > maximum) {
        return std::unexpected(game_error(
            code, "Integer is outside the supported range", source,
            std::string{pointer} + "/" + std::string{key}));
    }
    return static_cast<std::int32_t>(value);
}

[[nodiscard]] bool representable_float(const double value) noexcept {
    return std::isfinite(value) && value >= -static_cast<double>(std::numeric_limits<float>::max()) &&
           value <= static_cast<double>(std::numeric_limits<float>::max());
}

[[nodiscard]] bool spawn_positions_representable(const GameSpawnGroupPlan& group) noexcept {
    if (group.count == 0U || group.placement.columns == 0U) return false;
    const auto max_column = std::min(group.count, group.placement.columns) - 1U;
    const auto max_row = (group.count - 1U) / group.placement.columns;
    const double x_step = static_cast<double>(max_column) * group.placement.spacing.x;
    const double y_step = static_cast<double>(max_row) * group.placement.spacing.y;
    const double last_base_x = static_cast<double>(group.placement.origin.x) + x_step;
    const double last_base_y = static_cast<double>(group.placement.origin.y) + y_step;
    const double first_x = static_cast<double>(group.placement.origin.x) + group.transform.position_offset.x;
    const double first_y = static_cast<double>(group.placement.origin.y) + group.transform.position_offset.y;
    const double last_x = last_base_x + group.transform.position_offset.x;
    const double last_y = last_base_y + group.transform.position_offset.y;
    return representable_float(x_step) && representable_float(y_step) && representable_float(last_base_x) &&
           representable_float(last_base_y) && representable_float(first_x) && representable_float(first_y) &&
           representable_float(last_x) && representable_float(last_y);
}

[[nodiscard]] bool grid_positions_representable(const GameGridPlan& grid) noexcept {
    if (grid.columns == 0U || grid.rows == 0U) return false;
    const double x_step = static_cast<double>(grid.columns - 1U) * grid.cell_size.x;
    const double y_step = static_cast<double>(grid.rows - 1U) * grid.cell_size.y;
    const double last_x = static_cast<double>(grid.first_cell_center.x) + x_step;
    const double last_y = static_cast<double>(grid.first_cell_center.y) + y_step;
    return representable_float(x_step) && representable_float(y_step) && representable_float(last_x) &&
           representable_float(last_y);
}

Result<float> number(
    const Json& value,
    const std::string_view source,
    const std::string_view pointer,
    const DiagnosticCode code = DiagnosticCode::game_scene_invalid) {
    if (!value.is_number()) {
        return std::unexpected(game_error(code, "Expected a number", source, pointer));
    }
    const auto converted = value.get<double>();
    if (!std::isfinite(converted) || converted < -std::numeric_limits<float>::max() ||
        converted > std::numeric_limits<float>::max()) {
        return std::unexpected(game_error(code, "Number is not a finite float", source, pointer));
    }
    return static_cast<float>(converted);
}

Result<std::int32_t> integer_number(
    const Json& value,
    const std::int32_t minimum,
    const std::int32_t maximum,
    const std::string_view source,
    const std::string_view pointer,
    const DiagnosticCode code) {
    if (!value.is_number_integer() && !value.is_number_unsigned()) {
        return std::unexpected(game_error(code, "Expected an integer", source, pointer));
    }
    if (value.is_number_unsigned()) {
        const auto converted = value.get<std::uint64_t>();
        if (maximum < 0 || converted > static_cast<std::uint64_t>(maximum) ||
            (minimum > 0 && converted < static_cast<std::uint64_t>(minimum))) {
            return std::unexpected(game_error(code, "Integer is outside the supported range", source, pointer));
        }
        return static_cast<std::int32_t>(converted);
    }
    const auto converted = value.get<std::int64_t>();
    if (converted < minimum || converted > maximum) {
        return std::unexpected(game_error(code, "Integer is outside the supported range", source, pointer));
    }
    return static_cast<std::int32_t>(converted);
}

Result<Vec2> vec2(
    const Json& value,
    const std::string_view source,
    const std::string_view pointer,
    const DiagnosticCode code = DiagnosticCode::game_scene_invalid) {
    if (!value.is_array() || value.size() != 2U) {
        return std::unexpected(game_error(code, "Expected a two-number array", source, pointer));
    }
    auto x = number(value[0U], source, std::string{pointer} + "/0", code);
    if (!x) {
        return std::unexpected(std::move(x.error()));
    }
    auto y = number(value[1U], source, std::string{pointer} + "/1", code);
    if (!y) {
        return std::unexpected(std::move(y.error()));
    }
    return Vec2{*x, *y};
}

Result<Rect> rect4(
    const Json& value,
    const std::string_view source,
    const std::string_view pointer,
    const DiagnosticCode code = DiagnosticCode::game_scene_invalid) {
    if (!value.is_array() || value.size() != 4U) {
        return std::unexpected(game_error(code, "Expected a four-number array", source, pointer));
    }
    float values[4]{};
    for (std::size_t index = 0U; index < 4U; ++index) {
        auto item = number(value[index], source, std::string{pointer} + "/" + std::to_string(index), code);
        if (!item) {
            return std::unexpected(std::move(item.error()));
        }
        values[index] = *item;
    }
    return Rect{{values[0], values[1]}, {values[2], values[3]}};
}

Result<Color> color4(
    const Json& value,
    const std::string_view source,
    const std::string_view pointer,
    const DiagnosticCode code = DiagnosticCode::game_scene_invalid) {
    auto raw = rect4(value, source, pointer, code);
    if (!raw) {
        return std::unexpected(std::move(raw.error()));
    }
    const Color color{raw->min.x, raw->min.y, raw->max.x, raw->max.y};
    if (color.r < 0.0F || color.r > 1.0F || color.g < 0.0F || color.g > 1.0F ||
        color.b < 0.0F || color.b > 1.0F || color.a < 0.0F || color.a > 1.0F) {
        return std::unexpected(game_error(code, "Color channels must be between zero and one", source, pointer));
    }
    return color;
}

Result<bool> optional_bool(
    const Json& object,
    const std::string_view key,
    const bool fallback,
    const std::string_view source,
    const std::string_view pointer,
    const DiagnosticCode code = DiagnosticCode::game_scene_invalid) {
    const auto* value = optional(object, key);
    if (value == nullptr) {
        return fallback;
    }
    if (!value->is_boolean()) {
        return std::unexpected(game_error(
            code, "Expected a boolean", source, std::string{pointer} + "/" + std::string{key}));
    }
    return value->get<bool>();
}

Result<float> optional_number(
    const Json& object,
    const std::string_view key,
    const float fallback,
    const std::string_view source,
    const std::string_view pointer,
    const DiagnosticCode code = DiagnosticCode::game_scene_invalid) {
    const auto* value = optional(object, key);
    return value == nullptr ? Result<float>{fallback}
                            : number(*value, source, std::string{pointer} + "/" + std::string{key}, code);
}

Result<Vec2> optional_vec2(
    const Json& object,
    const std::string_view key,
    const Vec2 fallback,
    const std::string_view source,
    const std::string_view pointer,
    const DiagnosticCode code = DiagnosticCode::game_scene_invalid) {
    const auto* value = optional(object, key);
    return value == nullptr ? Result<Vec2>{fallback}
                            : vec2(*value, source, std::string{pointer} + "/" + std::string{key}, code);
}

Result<Color> optional_color(
    const Json& object,
    const std::string_view key,
    const Color fallback,
    const std::string_view source,
    const std::string_view pointer,
    const DiagnosticCode code = DiagnosticCode::game_scene_invalid) {
    const auto* value = optional(object, key);
    return value == nullptr ? Result<Color>{fallback}
                            : color4(*value, source, std::string{pointer} + "/" + std::string{key}, code);
}

class Symbols final {
public:
    SymbolId intern(std::string value) {
        const auto found = lookup_.find(value);
        if (found != lookup_.end()) {
            return found->second;
        }
        const auto id = static_cast<SymbolId>(values_.size());
        values_.push_back(std::move(value));
        lookup_.emplace(values_.back(), id);
        return id;
    }

    [[nodiscard]] std::optional<SymbolId> find(const std::string_view value) const {
        const auto found = lookup_.find(std::string{value});
        return found == lookup_.end() ? std::nullopt : std::optional<SymbolId>{found->second};
    }

    [[nodiscard]] std::string_view view(const SymbolId id) const noexcept {
        return id < values_.size() ? std::string_view{values_[id]} : std::string_view{};
    }

    std::vector<std::string> take() { return std::move(values_); }

private:
    std::vector<std::string> values_{};
    std::unordered_map<std::string, SymbolId> lookup_{};
};

class StableHash final {
public:
    void bytes(const void* data, const std::size_t size) noexcept {
        const auto* raw = static_cast<const std::uint8_t*>(data);
        for (std::size_t index = 0U; index < size; ++index) {
            value_ ^= raw[index];
            value_ *= 1099511628211ULL;
        }
    }

    template <class Value>
    void scalar(const Value value) noexcept {
        const auto bits = std::bit_cast<std::array<std::byte, sizeof(Value)>>(value);
        bytes(bits.data(), bits.size());
    }

    void text(const std::string_view value) noexcept {
        scalar(static_cast<std::uint64_t>(value.size()));
        bytes(value.data(), value.size());
    }

    [[nodiscard]] std::uint64_t value() const noexcept { return value_; }

private:
    std::uint64_t value_{14695981039346656037ULL};
};

Result<std::string> read_text_file(
    const std::filesystem::path& path,
    const std::uintmax_t maximum_size,
    const DiagnosticCode code) {
    std::error_code error{};
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > maximum_size) {
        auto diagnostic = game_error(code, "File is missing, unreadable, or too large", path.string());
        diagnostic.context.push_back({"maximum_bytes", std::to_string(maximum_size)});
        return std::unexpected(std::move(diagnostic));
    }
    std::ifstream stream{path, std::ios::binary};
    if (!stream) {
        return std::unexpected(game_error(code, "File could not be opened", path.string()));
    }
    return std::string{std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

bool path_is_within(const std::filesystem::path& root, const std::filesystem::path& candidate) {
    std::error_code error{};
    const auto relative = std::filesystem::relative(candidate, root, error);
    if (error || relative.empty() || relative.is_absolute()) {
        return false;
    }
    const auto first = *relative.begin();
    return first != "..";
}

bool canonical_content_file(
    const std::filesystem::path& root,
    const std::filesystem::path& candidate) {
    if (root.empty() || candidate.empty()) return false;
    std::error_code error{};
    const auto canonical_root = std::filesystem::weakly_canonical(root, error);
    if (error || canonical_root != root.lexically_normal() ||
        !std::filesystem::is_directory(canonical_root, error) || error) {
        return false;
    }
    const auto canonical_candidate = std::filesystem::weakly_canonical(candidate, error);
    return !error && canonical_candidate == candidate.lexically_normal() &&
           path_is_within(canonical_root, canonical_candidate) &&
           std::filesystem::is_regular_file(canonical_candidate, error) && !error;
}

Result<std::filesystem::path> resolve_content_path(
    const std::filesystem::path& root,
    const std::string_view raw,
    const std::string_view source,
    const std::string_view pointer) {
    const std::filesystem::path supplied{raw};
    if (supplied.empty() || supplied.is_absolute()) {
        return std::unexpected(game_error(
            DiagnosticCode::asset_path_invalid, "Content path must be relative", source, pointer));
    }
    std::error_code error{};
    const auto authored = (root / supplied).lexically_normal();
    const auto resolved = std::filesystem::weakly_canonical(authored, error);
    if (error || !path_is_within(root, resolved)) {
        return std::unexpected(game_error(
            DiagnosticCode::asset_path_invalid, "Content path escapes the manifest directory", source, pointer));
    }
    if (resolved != authored) {
        return std::unexpected(game_error(
            DiagnosticCode::asset_path_invalid,
            "Content paths may not traverse symbolic links or directory junctions", source, pointer));
    }
    if (!std::filesystem::is_regular_file(resolved, error) || error) {
        return std::unexpected(game_error(
            DiagnosticCode::asset_missing, "Referenced content file does not exist", source, pointer));
    }
    return resolved;
}

Result<Json> parse_json(const std::string& text, const std::string_view source, const DiagnosticCode code) {
    auto document = Json::parse(text, nullptr, false);
    if (document.is_discarded()) {
        return std::unexpected(game_error(code, "File is not valid JSON", source, "/"));
    }
    return document;
}

Result<RenderFpsCap> parse_fps(
    const std::string_view value,
    const std::string_view source,
    const std::string_view pointer) {
    if (value == "60") return RenderFpsCap::fps_60;
    if (value == "120") return RenderFpsCap::fps_120;
    if (value == "144") return RenderFpsCap::fps_144;
    if (value == "240") return RenderFpsCap::fps_240;
    if (value == "unlimited") return RenderFpsCap::unlimited;
    return std::unexpected(game_error(
        DiagnosticCode::game_manifest_invalid, "Unsupported render FPS cap", source, pointer));
}

Result<GameSchemaVersion> parse_game_schema_version(
    const std::string_view value,
    const std::string_view source,
    const std::string_view pointer,
    const DiagnosticCode code) {
    if (value == GamePlan::legacy_schema_version) return GameSchemaVersion::v0_2;
    if (value == GamePlan::event_action_schema_version) return GameSchemaVersion::v0_3;
    if (value == GamePlan::object_pool_schema_version) return GameSchemaVersion::v0_4;
    if (value == GamePlan::motion_contacts_schema_version) return GameSchemaVersion::v0_5;
    if (value == GamePlan::supported_schema_version) return GameSchemaVersion::v0_6;
    return std::unexpected(game_error(code, "Unsupported game schema version", source, pointer));
}

Result<GameDirection> parse_direction(
    const std::string_view value,
    const std::string_view source,
    const std::string_view pointer) {
    if (value == "up") return GameDirection::up;
    if (value == "down") return GameDirection::down;
    if (value == "left") return GameDirection::left;
    if (value == "right") return GameDirection::right;
    return std::unexpected(game_error(
        DiagnosticCode::game_scene_invalid, "Direction must be up, down, left, or right", source, pointer));
}

Result<GameComparison> parse_comparison(
    const std::string_view value,
    const std::string_view source,
    const std::string_view pointer) {
    if (value == "eq") return GameComparison::equal;
    if (value == "ne") return GameComparison::not_equal;
    if (value == "lt") return GameComparison::less;
    if (value == "le") return GameComparison::less_equal;
    if (value == "gt") return GameComparison::greater;
    if (value == "ge") return GameComparison::greater_equal;
    return std::unexpected(game_error(
        DiagnosticCode::game_scene_invalid, "Comparison must be eq, ne, lt, le, gt, or ge", source, pointer));
}

Result<GameKey> parse_key(
    const std::string_view value,
    const std::string_view source,
    const std::string_view pointer) {
    if (value == "escape") return GameKey::escape;
    if (value == "left") return GameKey::left;
    if (value == "right") return GameKey::right;
    if (value == "up") return GameKey::up;
    if (value == "down") return GameKey::down;
    if (value == "a") return GameKey::a;
    if (value == "d") return GameKey::d;
    if (value == "w") return GameKey::w;
    if (value == "s") return GameKey::s;
    if (value == "q") return GameKey::q;
    if (value == "e") return GameKey::e;
    if (value == "r") return GameKey::r;
    if (value == "f") return GameKey::f;
    if (value == "left_shift") return GameKey::left_shift;
    if (value == "left_control") return GameKey::left_control;
    if (value == "space") return GameKey::space;
    if (value == "enter") return GameKey::enter;
    if (value == "tab") return GameKey::tab;
    return std::unexpected(game_error(
        DiagnosticCode::game_manifest_invalid, "Unsupported input key", source, pointer));
}

Result<GamepadButton> parse_gamepad_button(
    const std::string_view value,
    const std::string_view source,
    const std::string_view pointer) {
    if (value == "south") return GamepadButton::south;
    if (value == "east") return GamepadButton::east;
    if (value == "west") return GamepadButton::west;
    if (value == "north") return GamepadButton::north;
    if (value == "back") return GamepadButton::back;
    if (value == "start") return GamepadButton::start;
    if (value == "left_stick") return GamepadButton::left_stick;
    if (value == "right_stick") return GamepadButton::right_stick;
    if (value == "left_shoulder") return GamepadButton::left_shoulder;
    if (value == "right_shoulder") return GamepadButton::right_shoulder;
    if (value == "dpad_up") return GamepadButton::dpad_up;
    if (value == "dpad_down") return GamepadButton::dpad_down;
    if (value == "dpad_left") return GamepadButton::dpad_left;
    if (value == "dpad_right") return GamepadButton::dpad_right;
    return std::unexpected(game_error(
        DiagnosticCode::game_input_profile_invalid, "Unsupported gamepad button", source, pointer));
}

Result<GamepadAxis> parse_gamepad_axis(
    const std::string_view value,
    const std::string_view source,
    const std::string_view pointer) {
    if (value == "left_x") return GamepadAxis::left_x;
    if (value == "left_y") return GamepadAxis::left_y;
    if (value == "right_x") return GamepadAxis::right_x;
    if (value == "right_y") return GamepadAxis::right_y;
    if (value == "left_trigger") return GamepadAxis::left_trigger;
    if (value == "right_trigger") return GamepadAxis::right_trigger;
    return std::unexpected(game_error(
        DiagnosticCode::game_input_profile_invalid, "Unsupported gamepad axis", source, pointer));
}

Result<GameUiAnchor> parse_ui_anchor(
    const std::string_view value,
    const std::string_view source,
    const std::string_view pointer) {
    if (value == "top_left") return GameUiAnchor::top_left;
    if (value == "top") return GameUiAnchor::top;
    if (value == "top_right") return GameUiAnchor::top_right;
    if (value == "left") return GameUiAnchor::left;
    if (value == "center") return GameUiAnchor::center;
    if (value == "right") return GameUiAnchor::right;
    if (value == "bottom_left") return GameUiAnchor::bottom_left;
    if (value == "bottom") return GameUiAnchor::bottom;
    if (value == "bottom_right") return GameUiAnchor::bottom_right;
    return std::unexpected(game_error(
        DiagnosticCode::game_presentation_invalid, "Unsupported UI anchor", source, pointer));
}

Vec2 anchored_ui_center(
    const GameUiAnchor anchor,
    const Vec2 size,
    const float width,
    const float height) noexcept {
    const float half_width = size.x * 0.5F;
    const float half_height = size.y * 0.5F;
    switch (anchor) {
    case GameUiAnchor::top_left: return {half_width, half_height};
    case GameUiAnchor::top: return {width * 0.5F, half_height};
    case GameUiAnchor::top_right: return {width - half_width, half_height};
    case GameUiAnchor::left: return {half_width, height * 0.5F};
    case GameUiAnchor::center: return {width * 0.5F, height * 0.5F};
    case GameUiAnchor::right: return {width - half_width, height * 0.5F};
    case GameUiAnchor::bottom_left: return {half_width, height - half_height};
    case GameUiAnchor::bottom: return {width * 0.5F, height - half_height};
    case GameUiAnchor::bottom_right: return {width - half_width, height - half_height};
    }
    return {half_width, half_height};
}

Result<std::uint32_t> index_by_name(
    const std::unordered_map<std::string, std::uint32_t>& table,
    const std::string_view name,
    const std::string_view kind,
    const std::string_view source,
    const std::string_view pointer,
    const DiagnosticCode code) {
    const auto found = table.find(std::string{name});
    if (found == table.end()) {
        return std::unexpected(game_error(
            code, "Reference names an unknown " + std::string{kind}, source, pointer));
    }
    return found->second;
}

Result<void> parse_font_metadata(GameAssetPlan& asset, const std::string_view source) {
    auto text = read_text_file(asset.metadata_path, 4U * 1024U * 1024U, DiagnosticCode::asset_decode_failed);
    if (!text) {
        return std::unexpected(std::move(text.error()));
    }
    auto document = parse_json(*text, source, DiagnosticCode::asset_decode_failed);
    if (!document) {
        return std::unexpected(std::move(document.error()));
    }
    if (auto rejected = reject_unknown(*document, {"schema_version", "line_height", "glyphs"}, source, "/");
        !rejected) {
        return rejected;
    }
    auto version = string_value(*document, "schema_version", source, "/", DiagnosticCode::asset_decode_failed);
    if (!version) return std::unexpected(std::move(version.error()));
    if (*version != "1") {
        return std::unexpected(game_error(
            DiagnosticCode::asset_decode_failed, "Unsupported font metadata version", source, "/schema_version"));
    }
    const auto* line_height_value = optional(*document, "line_height");
    if (line_height_value == nullptr) {
        return std::unexpected(game_error(
            DiagnosticCode::asset_decode_failed, "Missing font line_height", source, "/line_height"));
    }
    auto line_height = number(*line_height_value, source, "/line_height", DiagnosticCode::asset_decode_failed);
    if (!line_height || *line_height <= 0.0F) {
        return line_height ? std::unexpected(game_error(
                                 DiagnosticCode::asset_decode_failed,
                                 "Font line height must be positive",
                                 source,
                                 "/line_height"))
                           : std::unexpected(std::move(line_height.error()));
    }
    asset.line_height = *line_height;
    auto glyphs_member = required(*document, "glyphs", source, "/", DiagnosticCode::asset_decode_failed);
    if (!glyphs_member) return std::unexpected(std::move(glyphs_member.error()));
    if (!(*glyphs_member)->is_array() || (*glyphs_member)->empty() ||
        (*glyphs_member)->size() > GameAssetPlan::max_glyphs) {
        return std::unexpected(game_error(
            DiagnosticCode::asset_decode_failed, "Font glyphs must be a non-empty bounded array", source, "/glyphs"));
    }
    asset.glyphs.reserve((*glyphs_member)->size());
    for (std::size_t index = 0U; index < (*glyphs_member)->size(); ++index) {
        const auto& item = (**glyphs_member)[index];
        const auto pointer = "/glyphs/" + std::to_string(index);
        if (auto rejected = reject_unknown(item, {"codepoint", "uv", "size", "bearing", "advance"}, source, pointer);
            !rejected) {
            return rejected;
        }
        auto codepoint = u32_value(
            item, "codepoint", 1U, 0x10FFFFU, source, pointer, DiagnosticCode::asset_decode_failed);
        if (!codepoint) return std::unexpected(std::move(codepoint.error()));
        auto uv_member = required(item, "uv", source, pointer, DiagnosticCode::asset_decode_failed);
        if (!uv_member) return std::unexpected(std::move(uv_member.error()));
        auto uv = rect4(**uv_member, source, pointer + "/uv", DiagnosticCode::asset_decode_failed);
        if (!uv) return std::unexpected(std::move(uv.error()));
        auto size_member = required(item, "size", source, pointer, DiagnosticCode::asset_decode_failed);
        if (!size_member) return std::unexpected(std::move(size_member.error()));
        auto size = vec2(**size_member, source, pointer + "/size", DiagnosticCode::asset_decode_failed);
        if (!size) return std::unexpected(std::move(size.error()));
        auto bearing = optional_vec2(
            item, "bearing", {}, source, pointer, DiagnosticCode::asset_decode_failed);
        if (!bearing) return std::unexpected(std::move(bearing.error()));
        auto advance = optional_number(
            item, "advance", size->x, source, pointer, DiagnosticCode::asset_decode_failed);
        if (!advance) return std::unexpected(std::move(advance.error()));
        if (size->x < 0.0F || size->y < 0.0F || *advance < 0.0F || uv->min.x < 0.0F || uv->min.y < 0.0F ||
            uv->max.x < 0.0F || uv->max.y < 0.0F || uv->min.x + uv->max.x > 1.0001F ||
            uv->min.y + uv->max.y > 1.0001F) {
            return std::unexpected(game_error(
                DiagnosticCode::asset_decode_failed, "Font glyph metrics are invalid", source, pointer));
        }
        asset.glyphs.push_back({*codepoint, *uv, *size, *bearing, *advance});
    }
    std::sort(asset.glyphs.begin(), asset.glyphs.end(), [](const GlyphPlan& left, const GlyphPlan& right) {
        return left.codepoint < right.codepoint;
    });
    const auto duplicate = std::adjacent_find(
        asset.glyphs.begin(), asset.glyphs.end(), [](const GlyphPlan& left, const GlyphPlan& right) {
            return left.codepoint == right.codepoint;
        });
    if (duplicate != asset.glyphs.end()) {
        return std::unexpected(game_error(
            DiagnosticCode::asset_decode_failed, "Font metadata contains duplicate codepoints", source, "/glyphs"));
    }
    return {};
}

Result<GameAssetPlan> parse_asset(
    const Json& object,
    const std::size_t index,
    const std::filesystem::path& root,
    Symbols& symbols,
    const std::string_view source,
    const bool version_0_6) {
    const auto pointer = "/assets/" + std::to_string(index);
    if (auto rejected = reject_unknown(object, {"id", "kind", "path", "metadata"}, source, pointer); !rejected) {
        return std::unexpected(std::move(rejected.error()));
    }
    auto id = string_value(object, "id", source, pointer);
    if (!id) return std::unexpected(std::move(id.error()));
    auto kind = string_value(object, "kind", source, pointer);
    if (!kind) return std::unexpected(std::move(kind.error()));
    auto raw_path = string_value(object, "path", source, pointer);
    if (!raw_path) return std::unexpected(std::move(raw_path.error()));
    auto path = resolve_content_path(root, *raw_path, source, pointer + "/path");
    if (!path) return std::unexpected(std::move(path.error()));

    GameAssetPlan plan{};
    plan.symbol = symbols.intern(std::move(*id));
    plan.path = std::move(*path);
    if (*kind == "png") {
        plan.kind = GameAssetKind::png;
        if (optional(object, "metadata") != nullptr) {
            return std::unexpected(game_error(
                DiagnosticCode::game_manifest_invalid, "PNG assets cannot declare metadata", source, pointer + "/metadata"));
        }
    } else if (*kind == "wav") {
        plan.kind = GameAssetKind::wav;
        if (optional(object, "metadata") != nullptr) {
            return std::unexpected(game_error(
                DiagnosticCode::game_manifest_invalid, "WAV assets cannot declare metadata", source, pointer + "/metadata"));
        }
    } else if (*kind == "music" && version_0_6) {
        plan.kind = GameAssetKind::music;
        if (optional(object, "metadata") != nullptr) {
            return std::unexpected(game_error(
                DiagnosticCode::game_manifest_invalid, "Music assets cannot declare metadata", source, pointer + "/metadata"));
        }
    } else if (*kind == "font") {
        plan.kind = GameAssetKind::font;
        auto raw_metadata = string_value(object, "metadata", source, pointer);
        if (!raw_metadata) return std::unexpected(std::move(raw_metadata.error()));
        auto metadata = resolve_content_path(root, *raw_metadata, source, pointer + "/metadata");
        if (!metadata) return std::unexpected(std::move(metadata.error()));
        plan.metadata_path = std::move(*metadata);
        if (auto parsed = parse_font_metadata(plan, plan.metadata_path.string()); !parsed) {
            return std::unexpected(std::move(parsed.error()));
        }
    } else {
        return std::unexpected(game_error(
            DiagnosticCode::game_manifest_invalid, "Unsupported asset kind", source, pointer + "/kind"));
    }
    return plan;
}

Result<ActionPlan> parse_action(
    const Json& object,
    const std::size_t index,
    Symbols& symbols,
    const std::string_view source) {
    const auto pointer = "/actions/" + std::to_string(index);
    if (auto rejected = reject_unknown(object, {"id", "keys", "mouse_left"}, source, pointer); !rejected) {
        return std::unexpected(std::move(rejected.error()));
    }
    auto id = string_value(object, "id", source, pointer);
    if (!id) return std::unexpected(std::move(id.error()));
    ActionPlan plan{};
    plan.symbol = symbols.intern(std::move(*id));
    auto mouse_left = optional_bool(object, "mouse_left", false, source, pointer, DiagnosticCode::game_manifest_invalid);
    if (!mouse_left) return std::unexpected(std::move(mouse_left.error()));
    plan.mouse_left = *mouse_left;
    if (const auto* keys = optional(object, "keys"); keys != nullptr) {
        if (!keys->is_array() || keys->size() > ActionPlan::max_keys) {
            return std::unexpected(game_error(
                DiagnosticCode::game_manifest_invalid, "Action keys must be a bounded array", source, pointer + "/keys"));
        }
        for (std::size_t key_index = 0U; key_index < keys->size(); ++key_index) {
            if (!(*keys)[key_index].is_string()) {
                return std::unexpected(game_error(
                    DiagnosticCode::game_manifest_invalid,
                    "Action key must be a string",
                    source,
                    pointer + "/keys/" + std::to_string(key_index)));
            }
            auto key = parse_key(
                (*keys)[key_index].get_ref<const std::string&>(),
                source,
                pointer + "/keys/" + std::to_string(key_index));
            if (!key) return std::unexpected(std::move(key.error()));
            plan.keys[plan.key_count++] = *key;
        }
    }
    return plan;
}

Result<GameAnimationPlan> parse_animation(
    const Json& object,
    const std::size_t index,
    const std::unordered_map<std::string, std::uint32_t>& assets,
    const std::vector<GameAssetPlan>& asset_plans,
    Symbols& symbols,
    const std::string_view source) {
    const auto pointer = "/animations/" + std::to_string(index);
    if (auto rejected = reject_unknown(
            object, {"id", "texture", "mode", "frames"}, source, pointer,
            DiagnosticCode::game_animation_invalid);
        !rejected) {
        return std::unexpected(std::move(rejected.error()));
    }
    auto id = string_value(object, "id", source, pointer, DiagnosticCode::game_animation_invalid);
    if (!id) return std::unexpected(std::move(id.error()));
    auto texture = string_value(object, "texture", source, pointer, DiagnosticCode::game_animation_invalid);
    if (!texture) return std::unexpected(std::move(texture.error()));
    auto asset_index = index_by_name(
        assets, *texture, "texture asset", source, pointer + "/texture", DiagnosticCode::game_animation_invalid);
    if (!asset_index) return std::unexpected(std::move(asset_index.error()));
    if (asset_plans[*asset_index].kind != GameAssetKind::png &&
        asset_plans[*asset_index].kind != GameAssetKind::font) {
        return std::unexpected(game_error(
            DiagnosticCode::game_animation_invalid, "Animation texture must be a PNG or font asset", source,
            pointer + "/texture"));
    }
    auto mode = string_value(object, "mode", source, pointer, DiagnosticCode::game_animation_invalid);
    if (!mode) return std::unexpected(std::move(mode.error()));
    GameAnimationMode parsed_mode{};
    if (*mode == "once") parsed_mode = GameAnimationMode::once;
    else if (*mode == "loop") parsed_mode = GameAnimationMode::loop;
    else if (*mode == "ping_pong") parsed_mode = GameAnimationMode::ping_pong;
    else {
        return std::unexpected(game_error(
            DiagnosticCode::game_animation_invalid, "Animation mode must be once, loop, or ping_pong", source,
            pointer + "/mode"));
    }
    auto frames = required(object, "frames", source, pointer, DiagnosticCode::game_animation_invalid);
    if (!frames) return std::unexpected(std::move(frames.error()));
    if (!(*frames)->is_array() || (*frames)->empty() || (*frames)->size() > GameAnimationPlan::max_frames) {
        return std::unexpected(game_error(
            DiagnosticCode::game_animation_invalid, "Animation frames must be a non-empty bounded array", source,
            pointer + "/frames"));
    }
    GameAnimationPlan plan{};
    plan.symbol = symbols.intern(std::move(*id));
    plan.asset_index = *asset_index;
    plan.mode = parsed_mode;
    plan.frames.reserve((*frames)->size());
    for (std::size_t frame_index = 0U; frame_index < (*frames)->size(); ++frame_index) {
        const auto& frame = (**frames)[frame_index];
        const auto frame_pointer = pointer + "/frames/" + std::to_string(frame_index);
        if (auto rejected = reject_unknown(
                frame, {"uv", "duration_ticks"}, source, frame_pointer,
                DiagnosticCode::game_animation_invalid);
            !rejected) {
            return std::unexpected(std::move(rejected.error()));
        }
        auto uv_member = required(frame, "uv", source, frame_pointer, DiagnosticCode::game_animation_invalid);
        if (!uv_member) return std::unexpected(std::move(uv_member.error()));
        auto uv = rect4(**uv_member, source, frame_pointer + "/uv", DiagnosticCode::game_animation_invalid);
        if (!uv) return std::unexpected(std::move(uv.error()));
        if (uv->min.x < 0.0F || uv->min.y < 0.0F || uv->max.x <= 0.0F || uv->max.y <= 0.0F ||
            uv->min.x + uv->max.x > 1.0001F || uv->min.y + uv->max.y > 1.0001F) {
            return std::unexpected(game_error(
                DiagnosticCode::game_animation_invalid, "Animation frame UV rectangle is invalid", source,
                frame_pointer + "/uv"));
        }
        auto duration = u32_value(
            frame, "duration_ticks", 1U, 1'000'000U, source, frame_pointer,
            DiagnosticCode::game_animation_invalid);
        if (!duration) return std::unexpected(std::move(duration.error()));
        plan.frames.push_back({*uv, *duration});
    }
    return plan;
}

Result<GameInputProfilePlan> parse_input_profile(
    const Json& object,
    const std::size_t index,
    const std::unordered_map<std::string, std::uint32_t>& actions,
    const std::vector<ActionPlan>& base_actions,
    Symbols& symbols,
    const std::string_view source) {
    const auto pointer = "/input_profiles/" + std::to_string(index);
    if (auto rejected = reject_unknown(
            object, {"id", "bindings"}, source, pointer, DiagnosticCode::game_input_profile_invalid);
        !rejected) {
        return std::unexpected(std::move(rejected.error()));
    }
    auto id = string_value(object, "id", source, pointer, DiagnosticCode::game_input_profile_invalid);
    if (!id) return std::unexpected(std::move(id.error()));
    auto bindings = required(object, "bindings", source, pointer, DiagnosticCode::game_input_profile_invalid);
    if (!bindings) return std::unexpected(std::move(bindings.error()));
    if (!(*bindings)->is_array() || (*bindings)->size() > base_actions.size()) {
        return std::unexpected(game_error(
            DiagnosticCode::game_input_profile_invalid, "Input profile bindings must be a bounded array", source,
            pointer + "/bindings"));
    }
    GameInputProfilePlan plan{};
    plan.symbol = symbols.intern(std::move(*id));
    plan.actions = base_actions;
    std::unordered_set<std::uint32_t> seen{};
    for (std::size_t binding_index = 0U; binding_index < (*bindings)->size(); ++binding_index) {
        const auto& binding = (**bindings)[binding_index];
        const auto binding_pointer = pointer + "/bindings/" + std::to_string(binding_index);
        if (auto rejected = reject_unknown(
                binding, {"action", "keys", "mouse_left", "gamepad_buttons", "gamepad_axes"}, source,
                binding_pointer, DiagnosticCode::game_input_profile_invalid);
            !rejected) {
            return std::unexpected(std::move(rejected.error()));
        }
        auto action = string_value(
            binding, "action", source, binding_pointer, DiagnosticCode::game_input_profile_invalid);
        if (!action) return std::unexpected(std::move(action.error()));
        auto action_index = index_by_name(
            actions, *action, "action", source, binding_pointer + "/action",
            DiagnosticCode::game_input_profile_invalid);
        if (!action_index) return std::unexpected(std::move(action_index.error()));
        if (!seen.insert(*action_index).second) {
            return std::unexpected(game_error(
                DiagnosticCode::game_input_profile_invalid, "Input profile binds an action more than once", source,
                binding_pointer + "/action"));
        }
        auto& output = plan.actions[*action_index];
        if (const auto* keys = optional(binding, "keys"); keys != nullptr) {
            if (!keys->is_array() || keys->size() > ActionPlan::max_keys) {
                return std::unexpected(game_error(
                    DiagnosticCode::game_input_profile_invalid, "Profile keys exceed the bounded limit", source,
                    binding_pointer + "/keys"));
            }
            output.key_count = 0U;
            for (std::size_t key_index = 0U; key_index < keys->size(); ++key_index) {
                if (!(*keys)[key_index].is_string()) {
                    return std::unexpected(game_error(
                        DiagnosticCode::game_input_profile_invalid, "Profile key must be a string", source,
                        binding_pointer + "/keys/" + std::to_string(key_index)));
                }
                auto parsed = parse_key(
                    (*keys)[key_index].get_ref<const std::string&>(), source,
                    binding_pointer + "/keys/" + std::to_string(key_index));
                if (!parsed) return std::unexpected(std::move(parsed.error()));
                output.keys[output.key_count++] = *parsed;
            }
        }
        if (optional(binding, "mouse_left") != nullptr) {
            auto mouse = optional_bool(
                binding, "mouse_left", false, source, binding_pointer,
                DiagnosticCode::game_input_profile_invalid);
            if (!mouse) return std::unexpected(std::move(mouse.error()));
            output.mouse_left = *mouse;
        }
        if (const auto* buttons = optional(binding, "gamepad_buttons"); buttons != nullptr) {
            if (!buttons->is_array() || buttons->size() > ActionPlan::max_gamepad_buttons) {
                return std::unexpected(game_error(
                    DiagnosticCode::game_input_profile_invalid, "Gamepad buttons exceed the bounded limit", source,
                    binding_pointer + "/gamepad_buttons"));
            }
            output.gamepad_button_count = 0U;
            for (std::size_t button_index = 0U; button_index < buttons->size(); ++button_index) {
                if (!(*buttons)[button_index].is_string()) {
                    return std::unexpected(game_error(
                        DiagnosticCode::game_input_profile_invalid, "Gamepad button must be a string", source,
                        binding_pointer + "/gamepad_buttons/" + std::to_string(button_index)));
                }
                auto parsed = parse_gamepad_button(
                    (*buttons)[button_index].get_ref<const std::string&>(), source,
                    binding_pointer + "/gamepad_buttons/" + std::to_string(button_index));
                if (!parsed) return std::unexpected(std::move(parsed.error()));
                output.gamepad_buttons[output.gamepad_button_count++] = *parsed;
            }
        }
        if (const auto* axes = optional(binding, "gamepad_axes"); axes != nullptr) {
            if (!axes->is_array() || axes->size() > ActionPlan::max_gamepad_axes) {
                return std::unexpected(game_error(
                    DiagnosticCode::game_input_profile_invalid, "Gamepad axes exceed the bounded limit", source,
                    binding_pointer + "/gamepad_axes"));
            }
            output.gamepad_axis_count = 0U;
            for (std::size_t axis_index = 0U; axis_index < axes->size(); ++axis_index) {
                const auto& axis = (*axes)[axis_index];
                const auto axis_pointer = binding_pointer + "/gamepad_axes/" + std::to_string(axis_index);
                if (auto rejected = reject_unknown(
                        axis, {"axis", "direction", "deadzone"}, source, axis_pointer,
                        DiagnosticCode::game_input_profile_invalid);
                    !rejected) {
                    return std::unexpected(std::move(rejected.error()));
                }
                auto axis_name = string_value(
                    axis, "axis", source, axis_pointer, DiagnosticCode::game_input_profile_invalid);
                if (!axis_name) return std::unexpected(std::move(axis_name.error()));
                auto parsed_axis = parse_gamepad_axis(*axis_name, source, axis_pointer + "/axis");
                if (!parsed_axis) return std::unexpected(std::move(parsed_axis.error()));
                auto direction = string_value(
                    axis, "direction", source, axis_pointer, DiagnosticCode::game_input_profile_invalid);
                if (!direction) return std::unexpected(std::move(direction.error()));
                const std::int8_t parsed_direction = *direction == "positive" ? 1 : *direction == "negative" ? -1 : 0;
                if (parsed_direction == 0) {
                    return std::unexpected(game_error(
                        DiagnosticCode::game_input_profile_invalid,
                        "Gamepad axis direction must be positive or negative", source, axis_pointer + "/direction"));
                }
                auto deadzone = optional_number(
                    axis, "deadzone", 0.25F, source, axis_pointer,
                    DiagnosticCode::game_input_profile_invalid);
                if (!deadzone) return std::unexpected(std::move(deadzone.error()));
                if (*deadzone <= 0.0F || *deadzone > 0.95F) {
                    return std::unexpected(game_error(
                        DiagnosticCode::game_input_profile_invalid,
                        "Gamepad axis deadzone must be greater than zero and at most 0.95", source,
                        axis_pointer + "/deadzone"));
                }
                output.gamepad_axes[output.gamepad_axis_count++] = {*parsed_axis, parsed_direction, *deadzone};
            }
        }
    }
    return plan;
}

Result<GameLocalizationPlan> parse_localization(
    const Json& declaration,
    const std::size_t index,
    const std::filesystem::path& root,
    Symbols& symbols,
    const std::string_view source,
    std::string& source_text) {
    const auto pointer = "/localizations/" + std::to_string(index);
    if (auto rejected = reject_unknown(
            declaration, {"locale", "path"}, source, pointer, DiagnosticCode::game_localization_invalid);
        !rejected) {
        return std::unexpected(std::move(rejected.error()));
    }
    auto locale = string_value(
        declaration, "locale", source, pointer, DiagnosticCode::game_localization_invalid);
    if (!locale) return std::unexpected(std::move(locale.error()));
    auto raw_path = string_value(
        declaration, "path", source, pointer, DiagnosticCode::game_localization_invalid);
    if (!raw_path) return std::unexpected(std::move(raw_path.error()));
    auto path = resolve_content_path(root, *raw_path, source, pointer + "/path");
    if (!path) return std::unexpected(std::move(path.error()));
    auto text = read_text_file(*path, 4U * 1024U * 1024U, DiagnosticCode::game_localization_invalid);
    if (!text) return std::unexpected(std::move(text.error()));
    source_text = *text;
    auto document = parse_json(*text, path->string(), DiagnosticCode::game_localization_invalid);
    if (!document) return std::unexpected(std::move(document.error()));
    if (auto rejected = reject_unknown(
            *document, {"schema_version", "strings"}, path->string(), "/",
            DiagnosticCode::game_localization_invalid);
        !rejected) {
        return std::unexpected(std::move(rejected.error()));
    }
    auto version = string_value(
        *document, "schema_version", path->string(), "/", DiagnosticCode::game_localization_invalid);
    if (!version) return std::unexpected(std::move(version.error()));
    if (*version != "1") {
        return std::unexpected(game_error(
            DiagnosticCode::game_localization_invalid, "Unsupported localization version", path->string(),
            "/schema_version"));
    }
    auto strings = required(*document, "strings", path->string(), "/", DiagnosticCode::game_localization_invalid);
    if (!strings) return std::unexpected(std::move(strings.error()));
    if (!(*strings)->is_object() || (*strings)->empty() || (*strings)->size() > 4'096U) {
        return std::unexpected(game_error(
            DiagnosticCode::game_localization_invalid, "Localization strings must be a non-empty bounded object",
            path->string(), "/strings"));
    }
    GameLocalizationPlan plan{};
    plan.locale = symbols.intern(std::move(*locale));
    plan.path = std::move(*path);
    plan.entries.reserve((*strings)->size());
    for (const auto& [key, value] : (*strings)->items()) {
        if (key.empty() || key.size() > 256U || !value.is_string() || value.get_ref<const std::string&>().empty() ||
            value.get_ref<const std::string&>().size() > 4'096U) {
            return std::unexpected(game_error(
                DiagnosticCode::game_localization_invalid, "Localization key or value is invalid", plan.path.string(),
                "/strings/" + key));
        }
        plan.entries.push_back({symbols.intern(key), symbols.intern(value.get<std::string>())});
    }
    return plan;
}

Result<IntStatePlan> parse_state(
    const Json& object,
    const std::size_t index,
    Symbols& symbols,
    const std::string_view source) {
    const auto pointer = "/states/" + std::to_string(index);
    if (auto rejected = reject_unknown(object, {"id", "initial", "min", "max"}, source, pointer); !rejected) {
        return std::unexpected(std::move(rejected.error()));
    }
    auto id = string_value(object, "id", source, pointer);
    if (!id) return std::unexpected(std::move(id.error()));
    auto minimum = i32_value(object, "min", std::numeric_limits<std::int32_t>::min(),
                             std::numeric_limits<std::int32_t>::max(), source, pointer);
    if (!minimum) return std::unexpected(std::move(minimum.error()));
    auto maximum = i32_value(object, "max", *minimum, std::numeric_limits<std::int32_t>::max(), source, pointer);
    if (!maximum) return std::unexpected(std::move(maximum.error()));
    auto initial = i32_value(object, "initial", *minimum, *maximum, source, pointer);
    if (!initial) return std::unexpected(std::move(initial.error()));
    return IntStatePlan{symbols.intern(std::move(*id)), *initial, *minimum, *maximum};
}

Result<GamePlacementPlan> parse_placement(
    const Json& object,
    const std::string_view source,
    const std::string_view pointer) {
    if (auto rejected = reject_unknown(
            object, {"kind", "origin", "spacing", "columns"}, source, pointer,
            DiagnosticCode::game_scene_invalid);
        !rejected) {
        return std::unexpected(std::move(rejected.error()));
    }
    auto kind = string_value(object, "kind", source, pointer, DiagnosticCode::game_scene_invalid);
    if (!kind) return std::unexpected(std::move(kind.error()));
    if (*kind != "grid") {
        return std::unexpected(game_error(
            DiagnosticCode::game_scene_invalid, "Only grid placement is supported in SceneSpec 0.2", source,
            std::string{pointer} + "/kind"));
    }
    auto origin_member = required(object, "origin", source, pointer, DiagnosticCode::game_scene_invalid);
    if (!origin_member) return std::unexpected(std::move(origin_member.error()));
    auto origin = vec2(**origin_member, source, std::string{pointer} + "/origin");
    if (!origin) return std::unexpected(std::move(origin.error()));
    auto spacing_member = required(object, "spacing", source, pointer, DiagnosticCode::game_scene_invalid);
    if (!spacing_member) return std::unexpected(std::move(spacing_member.error()));
    auto spacing = vec2(**spacing_member, source, std::string{pointer} + "/spacing");
    if (!spacing) return std::unexpected(std::move(spacing.error()));
    auto columns = u32_value(
        object, "columns", 1U, GamePlacementPlan::max_columns, source, pointer,
        DiagnosticCode::game_scene_invalid);
    if (!columns) return std::unexpected(std::move(columns.error()));
    return GamePlacementPlan{PlacementId::grid, *origin, *spacing, *columns};
}

Result<GameSpawnGroupPlan> parse_spawn_group(
    const Json& object,
    const std::size_t index,
    const bool version_0_3,
    const bool version_0_5,
    const bool version_0_6,
    const std::unordered_map<std::string, std::uint32_t>& assets,
    const std::vector<GameAssetPlan>& asset_plans,
    const std::unordered_map<std::string, std::uint32_t>& animations,
    const std::vector<GameAnimationPlan>& animation_plans,
    Symbols& symbols,
    const std::string_view source) {
    const auto pointer = "/spawn_groups/" + std::to_string(index);
    if (auto rejected = reject_unknown(
            object,
            version_0_6
                ? std::initializer_list<std::string_view>{
                      "id", "count", "active_count", "placement", "transform", "velocity", "sprite", "collider",
                      "animation"}
                : version_0_3
                    ? std::initializer_list<std::string_view>{
                          "id", "count", "active_count", "placement", "transform", "velocity", "sprite", "collider"}
                : std::initializer_list<std::string_view>{
                      "id", "count", "placement", "transform", "velocity", "sprite", "collider"},
            source,
            pointer,
            DiagnosticCode::game_scene_invalid);
        !rejected) {
        return std::unexpected(std::move(rejected.error()));
    }
    auto id = string_value(object, "id", source, pointer, DiagnosticCode::game_scene_invalid);
    if (!id) return std::unexpected(std::move(id.error()));
    auto count = u32_value(
        object, "count", 1U, GameSpawnGroupPlan::max_count, source, pointer,
        DiagnosticCode::game_scene_invalid);
    if (!count) return std::unexpected(std::move(count.error()));
    auto placement_member = required(object, "placement", source, pointer, DiagnosticCode::game_scene_invalid);
    if (!placement_member) return std::unexpected(std::move(placement_member.error()));
    auto placement = parse_placement(**placement_member, source, pointer + "/placement");
    if (!placement) return std::unexpected(std::move(placement.error()));

    GameSpawnGroupPlan plan{};
    plan.symbol = symbols.intern(std::move(*id));
    plan.count = *count;
    plan.active_count = plan.count;
    if (version_0_3 && optional(object, "active_count") != nullptr) {
        auto active_count = u32_value(
            object, "active_count", 0U, plan.count, source, pointer, DiagnosticCode::game_scene_invalid);
        if (!active_count) return std::unexpected(std::move(active_count.error()));
        plan.active_count = *active_count;
    }
    plan.placement = *placement;
    if (const auto* transform = optional(object, "transform"); transform != nullptr) {
        if (auto rejected = reject_unknown(
                *transform, {"position_offset", "rotation", "scale"}, source, pointer + "/transform",
                DiagnosticCode::game_scene_invalid);
            !rejected) {
            return std::unexpected(std::move(rejected.error()));
        }
        auto offset = optional_vec2(*transform, "position_offset", {}, source, pointer + "/transform");
        if (!offset) return std::unexpected(std::move(offset.error()));
        auto rotation = optional_number(*transform, "rotation", 0.0F, source, pointer + "/transform");
        if (!rotation) return std::unexpected(std::move(rotation.error()));
        auto scale = optional_vec2(*transform, "scale", {1.0F, 1.0F}, source, pointer + "/transform");
        if (!scale) return std::unexpected(std::move(scale.error()));
        if (scale->x <= 0.0F || scale->y <= 0.0F) {
            return std::unexpected(game_error(
                DiagnosticCode::game_scene_invalid, "Transform scale must be positive", source,
                pointer + "/transform/scale"));
        }
        plan.transform = {*offset, *rotation, *scale};
    }
    if (const auto* velocity = optional(object, "velocity"); velocity != nullptr) {
        if (auto rejected = reject_unknown(
                *velocity, {"linear", "angular"}, source, pointer + "/velocity",
                DiagnosticCode::game_scene_invalid);
            !rejected) {
            return std::unexpected(std::move(rejected.error()));
        }
        auto linear = optional_vec2(*velocity, "linear", {}, source, pointer + "/velocity");
        if (!linear) return std::unexpected(std::move(linear.error()));
        auto angular = optional_number(*velocity, "angular", 0.0F, source, pointer + "/velocity");
        if (!angular) return std::unexpected(std::move(angular.error()));
        plan.velocity = {*linear, *angular};
        plan.has_velocity = true;
    }
    if (const auto* sprite = optional(object, "sprite"); sprite != nullptr) {
        if (auto rejected = reject_unknown(
                *sprite, {"texture", "size", "pivot", "uv", "tint", "layer", "visible"}, source,
                pointer + "/sprite", DiagnosticCode::game_scene_invalid);
            !rejected) {
            return std::unexpected(std::move(rejected.error()));
        }
        auto texture = string_value(*sprite, "texture", source, pointer + "/sprite", DiagnosticCode::game_scene_invalid);
        if (!texture) return std::unexpected(std::move(texture.error()));
        auto asset_index = index_by_name(
            assets, *texture, "texture asset", source, pointer + "/sprite/texture", DiagnosticCode::game_scene_invalid);
        if (!asset_index) return std::unexpected(std::move(asset_index.error()));
        if (asset_plans[*asset_index].kind != GameAssetKind::png &&
            asset_plans[*asset_index].kind != GameAssetKind::font) {
            return std::unexpected(game_error(
                DiagnosticCode::game_scene_invalid, "Sprite texture must reference a PNG or font asset", source,
                pointer + "/sprite/texture"));
        }
        auto size_member = required(*sprite, "size", source, pointer + "/sprite", DiagnosticCode::game_scene_invalid);
        if (!size_member) return std::unexpected(std::move(size_member.error()));
        auto size = vec2(**size_member, source, pointer + "/sprite/size");
        if (!size) return std::unexpected(std::move(size.error()));
        auto pivot = optional_vec2(*sprite, "pivot", {0.5F, 0.5F}, source, pointer + "/sprite");
        if (!pivot) return std::unexpected(std::move(pivot.error()));
        Rect uv{{0.0F, 0.0F}, {1.0F, 1.0F}};
        if (const auto* uv_member = optional(*sprite, "uv"); uv_member != nullptr) {
            auto parsed = rect4(*uv_member, source, pointer + "/sprite/uv");
            if (!parsed) return std::unexpected(std::move(parsed.error()));
            uv = *parsed;
        }
        auto tint = optional_color(*sprite, "tint", {}, source, pointer + "/sprite");
        if (!tint) return std::unexpected(std::move(tint.error()));
        std::int32_t layer = 0;
        if (optional(*sprite, "layer") != nullptr) {
            auto parsed = i32_value(*sprite, "layer", -1'000'000, 1'000'000, source, pointer + "/sprite",
                                    DiagnosticCode::game_scene_invalid);
            if (!parsed) return std::unexpected(std::move(parsed.error()));
            layer = *parsed;
        }
        auto visible = optional_bool(*sprite, "visible", true, source, pointer + "/sprite");
        if (!visible) return std::unexpected(std::move(visible.error()));
        if (size->x <= 0.0F || size->y <= 0.0F || uv.min.x < 0.0F || uv.min.y < 0.0F || uv.max.x <= 0.0F ||
            uv.max.y <= 0.0F || uv.min.x + uv.max.x > 1.0001F || uv.min.y + uv.max.y > 1.0001F) {
            return std::unexpected(game_error(
                DiagnosticCode::game_scene_invalid, "Sprite size or UV rectangle is invalid", source,
                pointer + "/sprite"));
        }
        plan.sprite = {*asset_index, *size, *pivot, uv, *tint, layer, *visible};
        plan.has_sprite = true;
    }
    if (const auto* collider = optional(object, "collider"); collider != nullptr) {
        if (version_0_5 && optional(*collider, "trigger") != nullptr) {
            return std::unexpected(game_error(
                DiagnosticCode::game_collision_interaction_invalid,
                "SceneSpec 0.5 uses collision rule interaction instead of collider.trigger", source,
                pointer + "/collider/trigger"));
        }
        if (auto rejected = reject_unknown(
                *collider,
                version_0_5
                    ? std::initializer_list<std::string_view>{"offset", "half_extent", "group", "body", "enabled"}
                    : std::initializer_list<std::string_view>{"offset", "half_extent", "group", "body", "trigger", "enabled"},
                source,
                pointer + "/collider", DiagnosticCode::game_scene_invalid);
            !rejected) {
            return std::unexpected(std::move(rejected.error()));
        }
        auto offset = optional_vec2(*collider, "offset", {}, source, pointer + "/collider");
        if (!offset) return std::unexpected(std::move(offset.error()));
        auto half_member = required(
            *collider, "half_extent", source, pointer + "/collider", DiagnosticCode::game_scene_invalid);
        if (!half_member) return std::unexpected(std::move(half_member.error()));
        auto half_extent = vec2(**half_member, source, pointer + "/collider/half_extent");
        if (!half_extent) return std::unexpected(std::move(half_extent.error()));
        auto group = string_value(
            *collider, "group", source, pointer + "/collider", DiagnosticCode::game_scene_invalid);
        if (!group) return std::unexpected(std::move(group.error()));
        auto body = string_value(*collider, "body", source, pointer + "/collider", DiagnosticCode::game_scene_invalid);
        if (!body) return std::unexpected(std::move(body.error()));
        GameBodyMotion motion{};
        if (*body == "static") motion = GameBodyMotion::static_body;
        else if (*body == "kinematic") motion = GameBodyMotion::kinematic_body;
        else if (*body == "dynamic") motion = GameBodyMotion::dynamic_body;
        else {
            return std::unexpected(game_error(
                DiagnosticCode::game_scene_invalid, "Unsupported collider body kind", source,
                pointer + "/collider/body"));
        }
        auto trigger = optional_bool(*collider, "trigger", false, source, pointer + "/collider");
        if (!trigger) return std::unexpected(std::move(trigger.error()));
        auto enabled = optional_bool(*collider, "enabled", true, source, pointer + "/collider");
        if (!enabled) return std::unexpected(std::move(enabled.error()));
        if (half_extent->x <= 0.0F || half_extent->y <= 0.0F) {
            return std::unexpected(game_error(
                DiagnosticCode::game_scene_invalid, "Collider half extent must be positive", source,
                pointer + "/collider/half_extent"));
        }
        plan.collider = {*offset, *half_extent, symbols.intern(std::move(*group)), motion, *trigger, *enabled};
        plan.has_collider = true;
    }
    if (const auto* animation = optional(object, "animation"); animation != nullptr) {
        if (auto rejected = reject_unknown(
                *animation, {"clip", "autoplay"}, source, pointer + "/animation",
                DiagnosticCode::game_animation_invalid);
            !rejected) {
            return std::unexpected(std::move(rejected.error()));
        }
        if (!plan.has_sprite) {
            return std::unexpected(game_error(
                DiagnosticCode::game_animation_invalid, "Initial animation requires a sprite component", source,
                pointer + "/animation"));
        }
        auto clip = string_value(
            *animation, "clip", source, pointer + "/animation", DiagnosticCode::game_animation_invalid);
        if (!clip) return std::unexpected(std::move(clip.error()));
        auto animation_index = index_by_name(
            animations, *clip, "animation", source, pointer + "/animation/clip",
            DiagnosticCode::game_animation_invalid);
        if (!animation_index) return std::unexpected(std::move(animation_index.error()));
        auto autoplay = optional_bool(
            *animation, "autoplay", true, source, pointer + "/animation",
            DiagnosticCode::game_animation_invalid);
        if (!autoplay) return std::unexpected(std::move(autoplay.error()));
        plan.initial_animation_index = *animation_index;
        plan.has_initial_animation = true;
        plan.animation_autoplay = *autoplay;
        plan.sprite.asset_index = animation_plans[*animation_index].asset_index;
        plan.sprite.uv = animation_plans[*animation_index].frames.front().uv;
    }
    return plan;
}

Result<GameSystemPlan> parse_system(
    const Json& object,
    const std::size_t index,
    const bool version_0_3,
    const bool version_0_5,
    const std::unordered_map<std::string, std::uint32_t>& actions,
    const std::unordered_map<std::string, std::uint32_t>& spawn_groups,
    const std::unordered_map<std::string, std::uint32_t>& grids,
    const std::unordered_map<std::string, std::uint32_t>& prior_systems,
    const std::vector<GameSystemPlan>& system_plans,
    Symbols& symbols,
    const std::string_view source) {
    const auto pointer = "/systems/" + std::to_string(index);
    if (auto rejected = reject_unknown(
            object,
            version_0_3
                ? std::initializer_list<std::string_view>{
                      "id", "operation", "group", "negative_action", "positive_action", "action", "speed", "min",
                      "max", "velocity", "grid", "step_interval_ticks", "initial_direction", "prevent_reverse",
                      "leader", "followers", "motion_system", "spawn_group"}
                : std::initializer_list<std::string_view>{
                      "id", "operation", "group", "negative_action", "positive_action", "action", "speed", "min",
                      "max", "velocity"},
            source,
            pointer,
            DiagnosticCode::game_scene_invalid);
        !rejected) {
        return std::unexpected(std::move(rejected.error()));
    }
    auto id = string_value(object, "id", source, pointer, DiagnosticCode::game_scene_invalid);
    if (!id) return std::unexpected(std::move(id.error()));
    auto operation = string_value(object, "operation", source, pointer, DiagnosticCode::game_scene_invalid);
    if (!operation) return std::unexpected(std::move(operation.error()));
    const auto reject_operation_parameters = [&](const std::initializer_list<std::string_view> allowed) -> Result<void> {
        const auto is_allowed = [&](const std::string_view field) {
            return field == "id" || field == "operation" || std::find(allowed.begin(), allowed.end(), field) != allowed.end();
        };
        for (const auto field : {
                 "group", "negative_action", "positive_action", "action", "speed", "min", "max", "velocity",
                 "grid", "step_interval_ticks", "initial_direction", "prevent_reverse", "leader", "followers",
                 "motion_system", "spawn_group"}) {
            if (!is_allowed(field) && optional(object, field) != nullptr) {
                return std::unexpected(game_error(
                    DiagnosticCode::game_scene_invalid, "Field is not valid for this system operation", source,
                    pointer + "/" + field));
            }
        }
        return {};
    };
    GameSystemPlan plan{};
    plan.symbol = symbols.intern(std::move(*id));
    if (*operation == "axis_control") {
        if (auto checked = reject_operation_parameters(
                {"group", "negative_action", "positive_action", "speed", "min", "max"}); !checked) {
            return std::unexpected(std::move(checked.error()));
        }
        plan.operation = GameOperationId::axis_control;
        auto group = string_value(object, "group", source, pointer, DiagnosticCode::game_scene_invalid);
        if (!group) return std::unexpected(std::move(group.error()));
        plan.group = symbols.intern(std::move(*group));
        auto negative = string_value(object, "negative_action", source, pointer, DiagnosticCode::game_scene_invalid);
        if (!negative) return std::unexpected(std::move(negative.error()));
        auto positive = string_value(object, "positive_action", source, pointer, DiagnosticCode::game_scene_invalid);
        if (!positive) return std::unexpected(std::move(positive.error()));
        auto negative_index = index_by_name(
            actions, *negative, "action", source, pointer + "/negative_action", DiagnosticCode::game_scene_invalid);
        if (!negative_index) return std::unexpected(std::move(negative_index.error()));
        auto positive_index = index_by_name(
            actions, *positive, "action", source, pointer + "/positive_action", DiagnosticCode::game_scene_invalid);
        if (!positive_index) return std::unexpected(std::move(positive_index.error()));
        plan.negative_action = *negative_index;
        plan.positive_action = *positive_index;
        const auto* speed_member = optional(object, "speed");
        if (speed_member == nullptr) {
            return std::unexpected(game_error(
                DiagnosticCode::game_scene_invalid, "axis_control requires speed", source, pointer + "/speed"));
        }
        auto speed = number(*speed_member, source, pointer + "/speed");
        if (!speed || *speed <= 0.0F) {
            return speed ? std::unexpected(game_error(
                               DiagnosticCode::game_scene_invalid, "axis_control speed must be positive", source,
                               pointer + "/speed"))
                         : std::unexpected(std::move(speed.error()));
        }
        plan.speed = *speed;
        const auto* minimum_member = optional(object, "min");
        const auto* maximum_member = optional(object, "max");
        if (minimum_member == nullptr || maximum_member == nullptr) {
            return std::unexpected(game_error(
                DiagnosticCode::game_scene_invalid, "axis_control requires min and max", source, pointer));
        }
        auto minimum = number(*minimum_member, source, pointer + "/min");
        if (!minimum) return std::unexpected(std::move(minimum.error()));
        auto maximum = number(*maximum_member, source, pointer + "/max");
        if (!maximum) return std::unexpected(std::move(maximum.error()));
        if (*minimum >= *maximum) {
            return std::unexpected(game_error(
                DiagnosticCode::game_scene_invalid, "axis_control min must be below max", source, pointer));
        }
        plan.minimum = *minimum;
        plan.maximum = *maximum;
    } else if (*operation == "set_velocity_on_press") {
        if (auto checked = reject_operation_parameters({"group", "action", "velocity"}); !checked) {
            return std::unexpected(std::move(checked.error()));
        }
        plan.operation = GameOperationId::set_velocity_on_press;
        auto group = string_value(object, "group", source, pointer, DiagnosticCode::game_scene_invalid);
        if (!group) return std::unexpected(std::move(group.error()));
        plan.group = symbols.intern(std::move(*group));
        auto action = string_value(object, "action", source, pointer, DiagnosticCode::game_scene_invalid);
        if (!action) return std::unexpected(std::move(action.error()));
        auto action_index = index_by_name(
            actions, *action, "action", source, pointer + "/action", DiagnosticCode::game_scene_invalid);
        if (!action_index) return std::unexpected(std::move(action_index.error()));
        plan.action = *action_index;
        auto velocity_member = required(object, "velocity", source, pointer, DiagnosticCode::game_scene_invalid);
        if (!velocity_member) return std::unexpected(std::move(velocity_member.error()));
        auto velocity = vec2(**velocity_member, source, pointer + "/velocity");
        if (!velocity) return std::unexpected(std::move(velocity.error()));
        plan.velocity = *velocity;
    } else if (*operation == "simulate_collisions") {
        if (auto checked = reject_operation_parameters({}); !checked) {
            return std::unexpected(std::move(checked.error()));
        }
        plan.operation = GameOperationId::simulate_collisions;
    } else if (version_0_3 && *operation == "grid_motion") {
        if (auto checked = reject_operation_parameters(
                {"group", "grid", "step_interval_ticks", "initial_direction", "prevent_reverse"}); !checked) {
            return std::unexpected(std::move(checked.error()));
        }
        plan.operation = GameOperationId::grid_motion;
        auto grid = string_value(object, "grid", source, pointer, DiagnosticCode::game_scene_invalid);
        if (!grid) return std::unexpected(std::move(grid.error()));
        auto grid_index = index_by_name(
            grids, *grid, "grid", source, pointer + "/grid", DiagnosticCode::game_scene_invalid);
        if (!grid_index) return std::unexpected(std::move(grid_index.error()));
        auto group = string_value(object, "group", source, pointer, DiagnosticCode::game_scene_invalid);
        if (!group) return std::unexpected(std::move(group.error()));
        auto group_index = index_by_name(
            spawn_groups, *group, "spawn group", source, pointer + "/group", DiagnosticCode::game_scene_invalid);
        if (!group_index) return std::unexpected(std::move(group_index.error()));
        auto interval = u32_value(
            object, "step_interval_ticks", 1U, 1'000'000U, source, pointer,
            DiagnosticCode::game_scene_invalid);
        if (!interval) return std::unexpected(std::move(interval.error()));
        auto direction_text = string_value(
            object, "initial_direction", source, pointer, DiagnosticCode::game_scene_invalid);
        if (!direction_text) return std::unexpected(std::move(direction_text.error()));
        auto direction = parse_direction(*direction_text, source, pointer + "/initial_direction");
        if (!direction) return std::unexpected(std::move(direction.error()));
        auto prevent_reverse_member = required(
            object, "prevent_reverse", source, pointer, DiagnosticCode::game_scene_invalid);
        if (!prevent_reverse_member) return std::unexpected(std::move(prevent_reverse_member.error()));
        if (!(**prevent_reverse_member).is_boolean()) {
            return std::unexpected(game_error(
                DiagnosticCode::game_scene_invalid, "prevent_reverse must be a boolean", source,
                pointer + "/prevent_reverse"));
        }
        plan.grid_index = *grid_index;
        plan.spawn_group_index = *group_index;
        plan.step_interval_ticks = *interval;
        plan.initial_direction = *direction;
        plan.prevent_reverse = (**prevent_reverse_member).get<bool>();
    } else if (version_0_3 && *operation == "follow_transform_chain") {
        if (auto checked = reject_operation_parameters({"leader", "followers", "motion_system"}); !checked) {
            return std::unexpected(std::move(checked.error()));
        }
        plan.operation = GameOperationId::follow_transform_chain;
        auto leader = string_value(object, "leader", source, pointer, DiagnosticCode::game_scene_invalid);
        if (!leader) return std::unexpected(std::move(leader.error()));
        auto leader_index = index_by_name(
            spawn_groups, *leader, "spawn group", source, pointer + "/leader", DiagnosticCode::game_scene_invalid);
        if (!leader_index) return std::unexpected(std::move(leader_index.error()));
        auto followers = string_value(object, "followers", source, pointer, DiagnosticCode::game_scene_invalid);
        if (!followers) return std::unexpected(std::move(followers.error()));
        auto follower_index = index_by_name(
            spawn_groups, *followers, "spawn group", source, pointer + "/followers",
            DiagnosticCode::game_scene_invalid);
        if (!follower_index) return std::unexpected(std::move(follower_index.error()));
        auto motion = string_value(object, "motion_system", source, pointer, DiagnosticCode::game_scene_invalid);
        if (!motion) return std::unexpected(std::move(motion.error()));
        auto motion_index = index_by_name(
            prior_systems, *motion, "prior system", source, pointer + "/motion_system",
            DiagnosticCode::game_scene_invalid);
        if (!motion_index) return std::unexpected(std::move(motion_index.error()));
        if (system_plans[*motion_index].operation != GameOperationId::grid_motion ||
            system_plans[*motion_index].spawn_group_index != *leader_index) {
            return std::unexpected(game_error(
                DiagnosticCode::game_scene_invalid,
                "follow_transform_chain motion_system must be a prior grid_motion for the leader",
                source, pointer + "/motion_system"));
        }
        plan.leader_group_index = *leader_index;
        plan.follower_group_index = *follower_index;
        plan.motion_system_index = *motion_index;
    } else if (version_0_5 && *operation == "linear_motion") {
        if (auto checked = reject_operation_parameters({"spawn_group"}); !checked) {
            return std::unexpected(std::move(checked.error()));
        }
        plan.operation = GameOperationId::linear_motion;
        auto group = string_value(object, "spawn_group", source, pointer, DiagnosticCode::game_scene_invalid);
        if (!group) return std::unexpected(std::move(group.error()));
        auto group_index = index_by_name(
            spawn_groups, *group, "spawn group", source, pointer + "/spawn_group",
            DiagnosticCode::game_scene_invalid);
        if (!group_index) return std::unexpected(std::move(group_index.error()));
        plan.spawn_group_index = *group_index;
    } else {
        return std::unexpected(game_error(
            DiagnosticCode::game_scene_invalid, "Unsupported scene operation", source, pointer + "/operation"));
    }
    return plan;
}

Result<GameReactionPlan> parse_reaction(
    const Json& object,
    const std::string_view pointer,
    const std::unordered_map<std::string, std::uint32_t>& states,
    const std::unordered_map<std::string, std::uint32_t>& assets,
    const std::vector<GameAssetPlan>& asset_plans,
    Symbols& symbols,
    const std::string_view source) {
    if (auto rejected = reject_unknown(
            object, {"kind", "target", "state", "value", "group", "asset"}, source, pointer,
            DiagnosticCode::game_scene_invalid);
        !rejected) {
        return std::unexpected(std::move(rejected.error()));
    }
    auto kind = string_value(object, "kind", source, pointer, DiagnosticCode::game_scene_invalid);
    if (!kind) return std::unexpected(std::move(kind.error()));
    GameReactionPlan plan{};
    if (const auto* target = optional(object, "target"); target != nullptr) {
        if (!target->is_string()) {
            return std::unexpected(game_error(
                DiagnosticCode::game_scene_invalid, "Reaction target must be a string", source,
                std::string{pointer} + "/target"));
        }
        const auto value = target->get_ref<const std::string&>();
        if (value == "a") plan.target = GameReactionTarget::a;
        else if (value == "b") plan.target = GameReactionTarget::b;
        else {
            return std::unexpected(game_error(
                DiagnosticCode::game_scene_invalid, "Reaction target must be a or b", source,
                std::string{pointer} + "/target"));
        }
    }
    if (*kind == "reflect") {
        plan.kind = GameReactionKind::reflect;
        if (optional(object, "target") == nullptr) {
            return std::unexpected(game_error(
                DiagnosticCode::game_scene_invalid, "reflect requires target", source,
                std::string{pointer} + "/target"));
        }
    } else if (*kind == "deactivate") {
        plan.kind = GameReactionKind::deactivate;
        if (optional(object, "target") == nullptr) {
            return std::unexpected(game_error(
                DiagnosticCode::game_scene_invalid, "deactivate requires target", source,
                std::string{pointer} + "/target"));
        }
    } else if (*kind == "add_int_state") {
        plan.kind = GameReactionKind::add_int_state;
        auto state = string_value(object, "state", source, pointer, DiagnosticCode::game_scene_invalid);
        if (!state) return std::unexpected(std::move(state.error()));
        auto state_index = index_by_name(
            states, *state, "integer state", source, std::string{pointer} + "/state",
            DiagnosticCode::game_scene_invalid);
        if (!state_index) return std::unexpected(std::move(state_index.error()));
        auto value = i32_value(
            object, "value", std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max(),
            source, pointer, DiagnosticCode::game_scene_invalid);
        if (!value) return std::unexpected(std::move(value.error()));
        plan.state_index = *state_index;
        plan.value = *value;
    } else if (*kind == "reset_group") {
        plan.kind = GameReactionKind::reset_group;
        auto group = string_value(object, "group", source, pointer, DiagnosticCode::game_scene_invalid);
        if (!group) return std::unexpected(std::move(group.error()));
        plan.group = symbols.intern(std::move(*group));
    } else if (*kind == "play_sound") {
        plan.kind = GameReactionKind::play_sound;
        auto asset = string_value(object, "asset", source, pointer, DiagnosticCode::game_scene_invalid);
        if (!asset) return std::unexpected(std::move(asset.error()));
        auto asset_index = index_by_name(
            assets, *asset, "audio asset", source, std::string{pointer} + "/asset",
            DiagnosticCode::game_scene_invalid);
        if (!asset_index) return std::unexpected(std::move(asset_index.error()));
        if (asset_plans[*asset_index].kind != GameAssetKind::wav) {
            return std::unexpected(game_error(
                DiagnosticCode::game_scene_invalid, "play_sound must reference a WAV asset", source,
                std::string{pointer} + "/asset"));
        }
        plan.asset_index = *asset_index;
    } else {
        return std::unexpected(game_error(
            DiagnosticCode::game_scene_invalid, "Unsupported collision reaction", source,
            std::string{pointer} + "/kind"));
    }

    const auto reject_extraneous = [&](const std::string_view field, const bool allowed) -> Result<void> {
        if (!allowed && optional(object, field) != nullptr) {
            return std::unexpected(game_error(
                DiagnosticCode::game_scene_invalid, "Field is not valid for this reaction", source,
                std::string{pointer} + "/" + std::string{field}));
        }
        return {};
    };
    const bool targeted = plan.kind == GameReactionKind::reflect || plan.kind == GameReactionKind::deactivate;
    if (auto checked = reject_extraneous("target", targeted); !checked) return std::unexpected(std::move(checked.error()));
    if (auto checked = reject_extraneous("state", plan.kind == GameReactionKind::add_int_state); !checked)
        return std::unexpected(std::move(checked.error()));
    if (auto checked = reject_extraneous("value", plan.kind == GameReactionKind::add_int_state); !checked)
        return std::unexpected(std::move(checked.error()));
    if (auto checked = reject_extraneous("group", plan.kind == GameReactionKind::reset_group); !checked)
        return std::unexpected(std::move(checked.error()));
    if (auto checked = reject_extraneous("asset", plan.kind == GameReactionKind::play_sound); !checked)
        return std::unexpected(std::move(checked.error()));
    return plan;
}

Result<GameCollisionRulePlan> parse_collision_rule(
    const Json& object,
    const std::size_t index,
    const bool version_0_3,
    const bool version_0_5,
    const std::unordered_map<std::string, std::uint32_t>& states,
    const std::unordered_map<std::string, std::uint32_t>& assets,
    const std::vector<GameAssetPlan>& asset_plans,
    Symbols& symbols,
    const std::string_view source) {
    const auto pointer = "/collision_rules/" + std::to_string(index);
    if (auto rejected = reject_unknown(
            object,
            version_0_5 ? std::initializer_list<std::string_view>{"id", "a", "b", "interaction", "reactions"}
                        : version_0_3 ? std::initializer_list<std::string_view>{"id", "a", "b", "reactions"}
                        : std::initializer_list<std::string_view>{"a", "b", "reactions"},
            source, pointer, DiagnosticCode::game_scene_invalid);
        !rejected) {
        return std::unexpected(std::move(rejected.error()));
    }
    auto a = string_value(object, "a", source, pointer, DiagnosticCode::game_scene_invalid);
    if (!a) return std::unexpected(std::move(a.error()));
    auto b = string_value(object, "b", source, pointer, DiagnosticCode::game_scene_invalid);
    if (!b) return std::unexpected(std::move(b.error()));
    if (*a == *b) {
        return std::unexpected(game_error(
            DiagnosticCode::game_scene_invalid, "Collision rule groups must be distinct", source, pointer));
    }
    auto reactions = required(object, "reactions", source, pointer, DiagnosticCode::game_scene_invalid);
    if (!reactions) return std::unexpected(std::move(reactions.error()));
    if (!(*reactions)->is_array() || (!version_0_3 && (*reactions)->empty()) || (*reactions)->size() > 8U) {
        return std::unexpected(game_error(
            DiagnosticCode::game_scene_invalid,
            version_0_3 ? "Collision reactions must contain zero to eight items"
                        : "Collision reactions must contain one to eight items",
            source,
            pointer + "/reactions"));
    }
    GameCollisionRulePlan plan{};
    if (version_0_3) {
        auto id = string_value(object, "id", source, pointer, DiagnosticCode::game_scene_invalid);
        if (!id) return std::unexpected(std::move(id.error()));
        plan.symbol = symbols.intern(std::move(*id));
    }
    plan.group_a = symbols.intern(std::move(*a));
    plan.group_b = symbols.intern(std::move(*b));
    if (version_0_5) {
        auto interaction = string_value(
            object, "interaction", source, pointer, DiagnosticCode::game_collision_interaction_invalid);
        if (!interaction) return std::unexpected(std::move(interaction.error()));
        if (*interaction == "solid") plan.interaction = GameCollisionInteraction::solid;
        else if (*interaction == "trigger") plan.interaction = GameCollisionInteraction::trigger;
        else {
            return std::unexpected(game_error(
                DiagnosticCode::game_collision_interaction_invalid,
                "Collision interaction must be solid or trigger", source, pointer + "/interaction"));
        }
    }
    plan.reactions.reserve((*reactions)->size());
    for (std::size_t reaction_index = 0U; reaction_index < (*reactions)->size(); ++reaction_index) {
        auto reaction = parse_reaction(
            (**reactions)[reaction_index],
            pointer + "/reactions/" + std::to_string(reaction_index),
            states,
            assets,
            asset_plans,
            symbols,
            source);
        if (!reaction) return std::unexpected(std::move(reaction.error()));
        plan.reactions.push_back(*reaction);
    }
    if (version_0_5 && plan.interaction == GameCollisionInteraction::trigger && !plan.reactions.empty()) {
        return std::unexpected(game_error(
            DiagnosticCode::game_collision_interaction_invalid,
            "Trigger collision rules require an empty reactions array", source, pointer + "/reactions"));
    }
    if (version_0_5 && plan.interaction == GameCollisionInteraction::solid &&
        std::none_of(plan.reactions.begin(), plan.reactions.end(), [](const GameReactionPlan& reaction) {
            return reaction.kind == GameReactionKind::reflect;
        })) {
        return std::unexpected(game_error(
            DiagnosticCode::game_collision_interaction_invalid,
            "Solid collision rules require at least one reflect reaction", source, pointer + "/reactions"));
    }
    return plan;
}

Result<GameGridPlan> parse_grid(
    const Json& object,
    const std::size_t index,
    Symbols& symbols,
    const std::string_view source) {
    const auto pointer = "/grids/" + std::to_string(index);
    if (auto rejected = reject_unknown(
            object, {"id", "first_cell_center", "cell_size", "columns", "rows"}, source, pointer,
            DiagnosticCode::game_scene_invalid);
        !rejected) {
        return std::unexpected(std::move(rejected.error()));
    }
    auto id = string_value(object, "id", source, pointer, DiagnosticCode::game_scene_invalid);
    if (!id) return std::unexpected(std::move(id.error()));
    auto center_member = required(
        object, "first_cell_center", source, pointer, DiagnosticCode::game_scene_invalid);
    if (!center_member) return std::unexpected(std::move(center_member.error()));
    auto center = vec2(**center_member, source, pointer + "/first_cell_center");
    if (!center) return std::unexpected(std::move(center.error()));
    auto size_member = required(object, "cell_size", source, pointer, DiagnosticCode::game_scene_invalid);
    if (!size_member) return std::unexpected(std::move(size_member.error()));
    auto size = vec2(**size_member, source, pointer + "/cell_size");
    if (!size) return std::unexpected(std::move(size.error()));
    if (size->x <= 0.0F || size->y <= 0.0F) {
        return std::unexpected(game_error(
            DiagnosticCode::game_scene_invalid, "Grid cell size must be positive", source, pointer + "/cell_size"));
    }
    auto columns = u32_value(object, "columns", 1U, 1'000'000U, source, pointer,
                             DiagnosticCode::game_scene_invalid);
    if (!columns) return std::unexpected(std::move(columns.error()));
    auto rows = u32_value(object, "rows", 1U, 1'000'000U, source, pointer,
                          DiagnosticCode::game_scene_invalid);
    if (!rows) return std::unexpected(std::move(rows.error()));
    if (static_cast<std::uint64_t>(*columns) * *rows > GameGridPlan::max_cells) {
        return std::unexpected(game_error(
            DiagnosticCode::game_scene_invalid, "Grid exceeds the one-million-cell limit", source, pointer));
    }
    GameGridPlan plan{symbols.intern(std::move(*id)), *center, *size, *columns, *rows};
    if (!grid_positions_representable(plan)) {
        return std::unexpected(game_error(
            DiagnosticCode::game_scene_invalid, "Derived logical-grid positions exceed the finite float range", source,
            pointer));
    }
    return plan;
}

Result<GamePoolPlan> parse_pool(
    const Json& object,
    const std::size_t index,
    const std::unordered_map<std::string, std::uint32_t>& spawn_groups,
    Symbols& symbols,
    const std::string_view source) {
    const auto pointer = "/pools/" + std::to_string(index);
    if (auto rejected = reject_unknown(
            object, {"id", "group", "on_exhausted"}, source, pointer,
            DiagnosticCode::game_pool_invalid);
        !rejected) {
        return std::unexpected(std::move(rejected.error()));
    }
    auto id = string_value(object, "id", source, pointer, DiagnosticCode::game_pool_invalid);
    if (!id) return std::unexpected(std::move(id.error()));
    auto group = string_value(object, "group", source, pointer, DiagnosticCode::game_pool_invalid);
    if (!group) return std::unexpected(std::move(group.error()));
    auto group_index = index_by_name(
        spawn_groups, *group, "spawn group", source, pointer + "/group",
        DiagnosticCode::game_pool_invalid);
    if (!group_index) return std::unexpected(std::move(group_index.error()));
    auto policy = string_value(object, "on_exhausted", source, pointer, DiagnosticCode::game_pool_invalid);
    if (!policy) return std::unexpected(std::move(policy.error()));
    GamePoolExhaustionPolicy parsed_policy{};
    if (*policy == "skip") {
        parsed_policy = GamePoolExhaustionPolicy::skip;
    } else if (*policy == "recycle_oldest") {
        parsed_policy = GamePoolExhaustionPolicy::recycle_oldest;
    } else {
        return std::unexpected(game_error(
            DiagnosticCode::game_pool_invalid,
            "Pool on_exhausted must be skip or recycle_oldest", source, pointer + "/on_exhausted"));
    }
    return GamePoolPlan{symbols.intern(std::move(*id)), *group_index, parsed_policy};
}

Result<GameSpritePlan> parse_presentation_sprite(
    const Json& object,
    const std::string_view pointer,
    const std::unordered_map<std::string, std::uint32_t>& assets,
    const std::vector<GameAssetPlan>& asset_plans,
    const std::string_view source) {
    if (auto rejected = reject_unknown(
            object, {"texture", "size", "pivot", "uv", "tint", "layer", "visible"}, source, pointer,
            DiagnosticCode::game_presentation_invalid);
        !rejected) {
        return std::unexpected(std::move(rejected.error()));
    }
    auto texture = string_value(
        object, "texture", source, pointer, DiagnosticCode::game_presentation_invalid);
    if (!texture) return std::unexpected(std::move(texture.error()));
    auto asset_index = index_by_name(
        assets, *texture, "texture asset", source, std::string{pointer} + "/texture",
        DiagnosticCode::game_presentation_invalid);
    if (!asset_index) return std::unexpected(std::move(asset_index.error()));
    if (asset_plans[*asset_index].kind != GameAssetKind::png &&
        asset_plans[*asset_index].kind != GameAssetKind::font) {
        return std::unexpected(game_error(
            DiagnosticCode::game_presentation_invalid, "Sprite must reference a PNG or font asset", source,
            std::string{pointer} + "/texture"));
    }
    auto size_member = required(object, "size", source, pointer, DiagnosticCode::game_presentation_invalid);
    if (!size_member) return std::unexpected(std::move(size_member.error()));
    auto size = vec2(**size_member, source, std::string{pointer} + "/size", DiagnosticCode::game_presentation_invalid);
    if (!size) return std::unexpected(std::move(size.error()));
    auto pivot = optional_vec2(
        object, "pivot", {0.5F, 0.5F}, source, pointer, DiagnosticCode::game_presentation_invalid);
    if (!pivot) return std::unexpected(std::move(pivot.error()));
    Rect uv{{0.0F, 0.0F}, {1.0F, 1.0F}};
    if (const auto* uv_member = optional(object, "uv"); uv_member != nullptr) {
        auto parsed = rect4(
            *uv_member, source, std::string{pointer} + "/uv", DiagnosticCode::game_presentation_invalid);
        if (!parsed) return std::unexpected(std::move(parsed.error()));
        uv = *parsed;
    }
    auto tint = optional_color(
        object, "tint", {}, source, pointer, DiagnosticCode::game_presentation_invalid);
    if (!tint) return std::unexpected(std::move(tint.error()));
    std::int32_t layer = 0;
    if (optional(object, "layer") != nullptr) {
        auto parsed = i32_value(
            object, "layer", -1'000'000, 1'000'000, source, pointer,
            DiagnosticCode::game_presentation_invalid);
        if (!parsed) return std::unexpected(std::move(parsed.error()));
        layer = *parsed;
    }
    auto visible = optional_bool(
        object, "visible", true, source, pointer, DiagnosticCode::game_presentation_invalid);
    if (!visible) return std::unexpected(std::move(visible.error()));
    if (size->x <= 0.0F || size->y <= 0.0F || uv.min.x < 0.0F || uv.min.y < 0.0F || uv.max.x <= 0.0F ||
        uv.max.y <= 0.0F || uv.min.x + uv.max.x > 1.0001F || uv.min.y + uv.max.y > 1.0001F) {
        return std::unexpected(game_error(
            DiagnosticCode::game_presentation_invalid, "Presentation sprite size or UV is invalid", source, pointer));
    }
    return GameSpritePlan{*asset_index, *size, *pivot, uv, *tint, layer, *visible};
}

Result<GameTileLayerPlan> parse_tile_layer(
    const Json& object,
    const std::size_t index,
    const std::unordered_map<std::string, std::uint32_t>& grids,
    const std::vector<GameGridPlan>& grid_plans,
    const std::unordered_map<std::string, std::uint32_t>& assets,
    const std::vector<GameAssetPlan>& asset_plans,
    Symbols& symbols,
    const std::string_view source) {
    const auto pointer = "/tile_layers/" + std::to_string(index);
    if (auto rejected = reject_unknown(
            object, {"id", "grid", "texture", "atlas_columns", "atlas_rows", "fill", "cells", "tint", "layer", "visible"},
            source, pointer, DiagnosticCode::game_tile_field_invalid);
        !rejected) {
        return std::unexpected(std::move(rejected.error()));
    }
    auto id = string_value(object, "id", source, pointer, DiagnosticCode::game_tile_field_invalid);
    if (!id) return std::unexpected(std::move(id.error()));
    auto grid = string_value(object, "grid", source, pointer, DiagnosticCode::game_tile_field_invalid);
    if (!grid) return std::unexpected(std::move(grid.error()));
    auto grid_index = index_by_name(
        grids, *grid, "logical grid", source, pointer + "/grid", DiagnosticCode::game_tile_field_invalid);
    if (!grid_index) return std::unexpected(std::move(grid_index.error()));
    auto texture = string_value(object, "texture", source, pointer, DiagnosticCode::game_tile_field_invalid);
    if (!texture) return std::unexpected(std::move(texture.error()));
    auto asset_index = index_by_name(
        assets, *texture, "texture asset", source, pointer + "/texture", DiagnosticCode::game_tile_field_invalid);
    if (!asset_index) return std::unexpected(std::move(asset_index.error()));
    if (asset_plans[*asset_index].kind != GameAssetKind::png &&
        asset_plans[*asset_index].kind != GameAssetKind::font) {
        return std::unexpected(game_error(
            DiagnosticCode::game_tile_field_invalid, "Tile texture must reference a PNG or font asset", source,
            pointer + "/texture"));
    }
    auto atlas_columns = u32_value(
        object, "atlas_columns", 1U, 256U, source, pointer, DiagnosticCode::game_tile_field_invalid);
    if (!atlas_columns) return std::unexpected(std::move(atlas_columns.error()));
    auto atlas_rows = u32_value(
        object, "atlas_rows", 1U, 256U, source, pointer, DiagnosticCode::game_tile_field_invalid);
    if (!atlas_rows) return std::unexpected(std::move(atlas_rows.error()));
    const auto atlas_count = *atlas_columns * *atlas_rows;
    if (atlas_count > std::numeric_limits<std::uint16_t>::max()) {
        return std::unexpected(game_error(
            DiagnosticCode::game_tile_field_invalid, "Tile atlas exceeds the 16-bit tile limit", source, pointer));
    }
    const auto cell_count = static_cast<std::size_t>(grid_plans[*grid_index].columns) * grid_plans[*grid_index].rows;
    if (optional(object, "fill") != nullptr && optional(object, "cells") != nullptr) {
        return std::unexpected(game_error(
            DiagnosticCode::game_tile_field_invalid, "Tile layer cannot declare both fill and cells", source, pointer));
    }
    std::uint32_t fill = 0U;
    if (optional(object, "fill") != nullptr) {
        auto parsed = u32_value(
            object, "fill", 0U, atlas_count, source, pointer, DiagnosticCode::game_tile_field_invalid);
        if (!parsed) return std::unexpected(std::move(parsed.error()));
        fill = *parsed;
    }
    GameTileLayerPlan plan{};
    plan.symbol = symbols.intern(std::move(*id));
    plan.grid_index = *grid_index;
    plan.asset_index = *asset_index;
    plan.atlas_columns = *atlas_columns;
    plan.atlas_rows = *atlas_rows;
    plan.initial_cells.assign(cell_count, static_cast<std::uint16_t>(fill));
    if (const auto* cells = optional(object, "cells"); cells != nullptr) {
        if (!cells->is_array() || cells->size() != cell_count) {
            return std::unexpected(game_error(
                DiagnosticCode::game_tile_field_invalid, "Tile cells must exactly match the logical grid", source,
                pointer + "/cells"));
        }
        for (std::size_t cell = 0U; cell < cell_count; ++cell) {
            auto value = integer_number(
                (*cells)[cell], 0, static_cast<std::int32_t>(atlas_count), source,
                pointer + "/cells/" + std::to_string(cell), DiagnosticCode::game_tile_field_invalid);
            if (!value) return std::unexpected(std::move(value.error()));
            plan.initial_cells[cell] = static_cast<std::uint16_t>(*value);
        }
    }
    auto tint = optional_color(
        object, "tint", {}, source, pointer, DiagnosticCode::game_tile_field_invalid);
    if (!tint) return std::unexpected(std::move(tint.error()));
    plan.tint = *tint;
    if (optional(object, "layer") != nullptr) {
        auto layer = i32_value(
            object, "layer", -1'000'000, 1'000'000, source, pointer,
            DiagnosticCode::game_tile_field_invalid);
        if (!layer) return std::unexpected(std::move(layer.error()));
        plan.layer = *layer;
    }
    auto visible = optional_bool(
        object, "visible", true, source, pointer, DiagnosticCode::game_tile_field_invalid);
    if (!visible) return std::unexpected(std::move(visible.error()));
    plan.visible = *visible;
    return plan;
}

Result<GameFieldPlan> parse_field(
    const Json& object,
    const std::size_t index,
    const std::unordered_map<std::string, std::uint32_t>& grids,
    const std::vector<GameGridPlan>& grid_plans,
    Symbols& symbols,
    const std::string_view source) {
    const auto pointer = "/fields/" + std::to_string(index);
    if (auto rejected = reject_unknown(
            object, {"id", "grid", "min", "max", "initial", "cells"}, source, pointer,
            DiagnosticCode::game_tile_field_invalid);
        !rejected) {
        return std::unexpected(std::move(rejected.error()));
    }
    auto id = string_value(object, "id", source, pointer, DiagnosticCode::game_tile_field_invalid);
    if (!id) return std::unexpected(std::move(id.error()));
    auto grid = string_value(object, "grid", source, pointer, DiagnosticCode::game_tile_field_invalid);
    if (!grid) return std::unexpected(std::move(grid.error()));
    auto grid_index = index_by_name(
        grids, *grid, "logical grid", source, pointer + "/grid", DiagnosticCode::game_tile_field_invalid);
    if (!grid_index) return std::unexpected(std::move(grid_index.error()));
    auto minimum = i32_value(
        object, "min", std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max(), source,
        pointer, DiagnosticCode::game_tile_field_invalid);
    if (!minimum) return std::unexpected(std::move(minimum.error()));
    auto maximum = i32_value(
        object, "max", *minimum, std::numeric_limits<std::int32_t>::max(), source, pointer,
        DiagnosticCode::game_tile_field_invalid);
    if (!maximum) return std::unexpected(std::move(maximum.error()));
    if (optional(object, "initial") != nullptr && optional(object, "cells") != nullptr) {
        return std::unexpected(game_error(
            DiagnosticCode::game_tile_field_invalid, "Field cannot declare both initial and cells", source, pointer));
    }
    std::int32_t initial = *minimum <= 0 && *maximum >= 0 ? 0 : *minimum;
    if (optional(object, "initial") != nullptr) {
        auto parsed = i32_value(
            object, "initial", *minimum, *maximum, source, pointer, DiagnosticCode::game_tile_field_invalid);
        if (!parsed) return std::unexpected(std::move(parsed.error()));
        initial = *parsed;
    }
    const auto cell_count = static_cast<std::size_t>(grid_plans[*grid_index].columns) * grid_plans[*grid_index].rows;
    GameFieldPlan plan{};
    plan.symbol = symbols.intern(std::move(*id));
    plan.grid_index = *grid_index;
    plan.minimum = *minimum;
    plan.maximum = *maximum;
    plan.initial_cells.assign(cell_count, initial);
    if (const auto* cells = optional(object, "cells"); cells != nullptr) {
        if (!cells->is_array() || cells->size() != cell_count) {
            return std::unexpected(game_error(
                DiagnosticCode::game_tile_field_invalid, "Field cells must exactly match the logical grid", source,
                pointer + "/cells"));
        }
        for (std::size_t cell = 0U; cell < cell_count; ++cell) {
            auto value = integer_number(
                (*cells)[cell], *minimum, *maximum, source, pointer + "/cells/" + std::to_string(cell),
                DiagnosticCode::game_tile_field_invalid);
            if (!value) return std::unexpected(std::move(value.error()));
            plan.initial_cells[cell] = *value;
        }
    }
    return plan;
}

Result<GameParticleEmitterPlan> parse_particle_emitter(
    const Json& object,
    const std::size_t index,
    const std::unordered_map<std::string, std::uint32_t>& assets,
    const std::vector<GameAssetPlan>& asset_plans,
    Symbols& symbols,
    const std::string_view source) {
    const auto pointer = "/particle_emitters/" + std::to_string(index);
    if (auto rejected = reject_unknown(
            object, {"id", "capacity", "on_exhausted", "sprite", "lifetime_ticks", "velocity_min", "velocity_max"},
            source, pointer, DiagnosticCode::game_presentation_invalid);
        !rejected) {
        return std::unexpected(std::move(rejected.error()));
    }
    auto id = string_value(object, "id", source, pointer, DiagnosticCode::game_presentation_invalid);
    if (!id) return std::unexpected(std::move(id.error()));
    auto capacity = u32_value(
        object, "capacity", 1U, 10'000U, source, pointer, DiagnosticCode::game_presentation_invalid);
    if (!capacity) return std::unexpected(std::move(capacity.error()));
    auto exhausted = string_value(
        object, "on_exhausted", source, pointer, DiagnosticCode::game_presentation_invalid);
    if (!exhausted) return std::unexpected(std::move(exhausted.error()));
    GamePoolExhaustionPolicy policy{};
    if (*exhausted == "skip") policy = GamePoolExhaustionPolicy::skip;
    else if (*exhausted == "recycle_oldest") policy = GamePoolExhaustionPolicy::recycle_oldest;
    else {
        return std::unexpected(game_error(
            DiagnosticCode::game_presentation_invalid, "Particle exhaustion must be skip or recycle_oldest", source,
            pointer + "/on_exhausted"));
    }
    auto sprite_member = required(
        object, "sprite", source, pointer, DiagnosticCode::game_presentation_invalid);
    if (!sprite_member) return std::unexpected(std::move(sprite_member.error()));
    auto sprite = parse_presentation_sprite(**sprite_member, pointer + "/sprite", assets, asset_plans, source);
    if (!sprite) return std::unexpected(std::move(sprite.error()));
    auto lifetime = required(
        object, "lifetime_ticks", source, pointer, DiagnosticCode::game_presentation_invalid);
    if (!lifetime) return std::unexpected(std::move(lifetime.error()));
    if (!(*lifetime)->is_array() || (*lifetime)->size() != 2U) {
        return std::unexpected(game_error(
            DiagnosticCode::game_presentation_invalid, "Particle lifetime_ticks must be a two-integer array", source,
            pointer + "/lifetime_ticks"));
    }
    auto lifetime_min = integer_number(
        (**lifetime)[0], 1, 1'000'000, source, pointer + "/lifetime_ticks/0",
        DiagnosticCode::game_presentation_invalid);
    if (!lifetime_min) return std::unexpected(std::move(lifetime_min.error()));
    auto lifetime_max = integer_number(
        (**lifetime)[1], *lifetime_min, 1'000'000, source, pointer + "/lifetime_ticks/1",
        DiagnosticCode::game_presentation_invalid);
    if (!lifetime_max) return std::unexpected(std::move(lifetime_max.error()));
    auto velocity_min = optional_vec2(
        object, "velocity_min", {}, source, pointer, DiagnosticCode::game_presentation_invalid);
    if (!velocity_min) return std::unexpected(std::move(velocity_min.error()));
    auto velocity_max = optional_vec2(
        object, "velocity_max", *velocity_min, source, pointer, DiagnosticCode::game_presentation_invalid);
    if (!velocity_max) return std::unexpected(std::move(velocity_max.error()));
    if (velocity_min->x > velocity_max->x || velocity_min->y > velocity_max->y) {
        return std::unexpected(game_error(
            DiagnosticCode::game_presentation_invalid, "Particle velocity_min must not exceed velocity_max", source,
            pointer));
    }
    return GameParticleEmitterPlan{
        symbols.intern(std::move(*id)), *capacity, policy, *sprite,
        static_cast<std::uint32_t>(*lifetime_min), static_cast<std::uint32_t>(*lifetime_max),
        *velocity_min, *velocity_max};
}

Result<GameRuleTargetPlan> parse_rule_target(
    const Json& object,
    const std::string_view pointer,
    const std::unordered_map<std::string, std::uint32_t>& spawn_groups,
    const std::vector<GameSpawnGroupPlan>& spawn_plans,
    const bool collision_context,
    const bool event_entity_context,
    const std::string_view source,
    const DiagnosticCode diagnostic_code) {
    if (auto rejected = reject_unknown(
            object, {"kind", "group", "index"}, source, pointer, diagnostic_code);
        !rejected) {
        return std::unexpected(std::move(rejected.error()));
    }
    auto kind = string_value(object, "kind", source, pointer, diagnostic_code);
    if (!kind) return std::unexpected(std::move(kind.error()));
    GameRuleTargetPlan target{};
    if (*kind == "group" || *kind == "index") {
        auto group = string_value(object, "group", source, pointer, diagnostic_code);
        if (!group) return std::unexpected(std::move(group.error()));
        auto group_index = index_by_name(
            spawn_groups, *group, "spawn group", source, std::string{pointer} + "/group",
            diagnostic_code);
        if (!group_index) return std::unexpected(std::move(group_index.error()));
        target.kind = *kind == "group" ? GameRuleTargetKind::spawn_group : GameRuleTargetKind::spawn_index;
        target.spawn_group_index = *group_index;
        if (*kind == "index") {
            auto item = u32_value(
                object, "index", 0U, spawn_plans[*group_index].count - 1U, source, pointer,
                diagnostic_code);
            if (!item) return std::unexpected(std::move(item.error()));
            target.item_index = *item;
        } else if (optional(object, "index") != nullptr) {
            return std::unexpected(game_error(
                diagnostic_code, "Group target does not accept index", source,
                std::string{pointer} + "/index"));
        }
    } else if (*kind == "collision_a" || *kind == "collision_b") {
        if (!collision_context) {
            return std::unexpected(game_error(
                diagnostic_code, "Collision targets require a collision event", source, pointer));
        }
        if (optional(object, "group") != nullptr || optional(object, "index") != nullptr) {
            return std::unexpected(game_error(
                diagnostic_code, "Collision target does not accept group or index", source, pointer));
        }
        target.kind = *kind == "collision_a" ? GameRuleTargetKind::collision_a : GameRuleTargetKind::collision_b;
    } else if (*kind == "event_entity") {
        if (!event_entity_context) {
            return std::unexpected(game_error(
                diagnostic_code, "event_entity target requires an animation_finished event", source, pointer));
        }
        if (optional(object, "group") != nullptr || optional(object, "index") != nullptr) {
            return std::unexpected(game_error(
                diagnostic_code, "event_entity target does not accept group or index", source, pointer));
        }
        target.kind = GameRuleTargetKind::event_entity;
    } else {
        return std::unexpected(game_error(
            diagnostic_code, "Unsupported rule target kind", source,
            std::string{pointer} + "/kind"));
    }
    return target;
}

struct ParsedCellSource final {
    GameCellSourceKind kind{GameCellSourceKind::constant};
    std::uint32_t x{0U};
    std::uint32_t y{0U};
    GameRuleTargetPlan target{};
};

Result<ParsedCellSource> parse_cell_source(
    const Json& object,
    const std::string_view pointer,
    const GameGridPlan& grid,
    const std::unordered_map<std::string, std::uint32_t>& spawn_groups,
    const std::vector<GameSpawnGroupPlan>& spawn_plans,
    const bool collision_context,
    const bool event_entity_context,
    const std::string_view source,
    const DiagnosticCode diagnostic_code) {
    const auto* target_member = optional(object, "target");
    const auto* x_member = optional(object, "x");
    const auto* y_member = optional(object, "y");
    if (target_member != nullptr) {
        if (x_member != nullptr || y_member != nullptr) {
            return std::unexpected(game_error(
                diagnostic_code, "Cell source cannot combine target with x or y", source,
                std::string{pointer}));
        }
        auto target = parse_rule_target(
            *target_member, std::string{pointer} + "/target", spawn_groups, spawn_plans,
            collision_context, event_entity_context, source, diagnostic_code);
        if (!target) return std::unexpected(std::move(target.error()));
        if (target->kind == GameRuleTargetKind::spawn_group) {
            return std::unexpected(game_error(
                diagnostic_code, "Cell target must identify one entity", source,
                std::string{pointer} + "/target"));
        }
        ParsedCellSource parsed{};
        parsed.kind = GameCellSourceKind::target;
        parsed.target = *target;
        return parsed;
    }
    if (x_member == nullptr || y_member == nullptr) {
        return std::unexpected(game_error(
            diagnostic_code, "Cell source requires either target or both x and y", source,
            std::string{pointer}));
    }
    auto x = u32_value(
        object, "x", 0U, grid.columns - 1U, source, pointer, diagnostic_code);
    if (!x) return std::unexpected(std::move(x.error()));
    auto y = u32_value(
        object, "y", 0U, grid.rows - 1U, source, pointer, diagnostic_code);
    if (!y) return std::unexpected(std::move(y.error()));
    ParsedCellSource parsed{};
    parsed.x = *x;
    parsed.y = *y;
    return parsed;
}

Result<GameRuleEventPlan> parse_rule_event(
    const Json& object,
    const std::string_view pointer,
    const std::unordered_map<std::string, std::uint32_t>& actions,
    const std::unordered_map<std::string, std::uint32_t>& collision_rules,
    const std::vector<GameCollisionRulePlan>& collision_rule_plans,
    const bool version_0_5,
    const bool version_0_6,
    const std::unordered_map<std::string, std::uint32_t>& animations,
    const std::string_view source) {
    if (auto rejected = reject_unknown(
            object,
            version_0_6
                ? std::initializer_list<std::string_view>{"kind", "action", "interval_ticks", "rule", "animation"}
                : std::initializer_list<std::string_view>{"kind", "action", "interval_ticks", "rule"},
            source, pointer,
            DiagnosticCode::game_scene_invalid);
        !rejected) {
        return std::unexpected(std::move(rejected.error()));
    }
    auto kind = string_value(object, "kind", source, pointer, DiagnosticCode::game_scene_invalid);
    if (!kind) return std::unexpected(std::move(kind.error()));
    GameRuleEventPlan event{};
    if (*kind == "scene_enter") {
        event.kind = GameRuleEventKind::scene_enter;
    } else if (*kind == "action_pressed" || *kind == "action_released") {
        event.kind = *kind == "action_pressed" ? GameRuleEventKind::action_pressed
                                                : GameRuleEventKind::action_released;
        auto action = string_value(object, "action", source, pointer, DiagnosticCode::game_scene_invalid);
        if (!action) return std::unexpected(std::move(action.error()));
        auto index = index_by_name(
            actions, *action, "action", source, std::string{pointer} + "/action",
            DiagnosticCode::game_scene_invalid);
        if (!index) return std::unexpected(std::move(index.error()));
        event.action_index = *index;
    } else if (*kind == "fixed_interval") {
        event.kind = GameRuleEventKind::fixed_interval;
        auto interval = u32_value(
            object, "interval_ticks", 1U, 1'000'000U, source, pointer,
            DiagnosticCode::game_scene_invalid);
        if (!interval) return std::unexpected(std::move(interval.error()));
        event.interval_ticks = *interval;
    } else if (*kind == "collision" || *kind == "contact_begin" || *kind == "contact_end") {
        if ((*kind == "contact_begin" || *kind == "contact_end") && !version_0_5) {
            return std::unexpected(game_error(
                DiagnosticCode::game_scene_invalid, "Contact events require SceneSpec 0.5", source,
                std::string{pointer} + "/kind"));
        }
        event.kind = *kind == "collision" ? GameRuleEventKind::collision
                   : *kind == "contact_begin" ? GameRuleEventKind::contact_begin
                                               : GameRuleEventKind::contact_end;
        auto rule = string_value(object, "rule", source, pointer, DiagnosticCode::game_scene_invalid);
        if (!rule) return std::unexpected(std::move(rule.error()));
        auto index = index_by_name(
            collision_rules, *rule, "collision rule", source, std::string{pointer} + "/rule",
            DiagnosticCode::game_scene_invalid);
        if (!index) return std::unexpected(std::move(index.error()));
        event.collision_rule_index = *index;
        const auto interaction = collision_rule_plans[*index].interaction;
        if ((event.kind == GameRuleEventKind::collision && version_0_5 &&
             interaction != GameCollisionInteraction::solid) ||
            ((event.kind == GameRuleEventKind::contact_begin || event.kind == GameRuleEventKind::contact_end) &&
             interaction != GameCollisionInteraction::trigger)) {
            return std::unexpected(game_error(
                DiagnosticCode::game_collision_interaction_invalid,
                "Rule event kind does not match the collision interaction", source,
                std::string{pointer} + "/rule"));
        }
    } else if (*kind == "animation_finished" && version_0_6) {
        event.kind = GameRuleEventKind::animation_finished;
        auto animation = string_value(
            object, "animation", source, pointer, DiagnosticCode::game_animation_invalid);
        if (!animation) return std::unexpected(std::move(animation.error()));
        auto animation_index = index_by_name(
            animations, *animation, "animation", source, std::string{pointer} + "/animation",
            DiagnosticCode::game_animation_invalid);
        if (!animation_index) return std::unexpected(std::move(animation_index.error()));
        event.animation_index = *animation_index;
    } else {
        return std::unexpected(game_error(
            DiagnosticCode::game_scene_invalid, "Unsupported rule event kind", source,
            std::string{pointer} + "/kind"));
    }
    const bool action_event = event.kind == GameRuleEventKind::action_pressed ||
                              event.kind == GameRuleEventKind::action_released;
    for (const auto& [field, allowed] : {
             std::pair<std::string_view, bool>{"action", action_event},
             {"interval_ticks", event.kind == GameRuleEventKind::fixed_interval},
             {"rule", event.kind == GameRuleEventKind::collision ||
                          event.kind == GameRuleEventKind::contact_begin ||
                           event.kind == GameRuleEventKind::contact_end},
             {"animation", event.kind == GameRuleEventKind::animation_finished},
         }) {
        if (!allowed && optional(object, field) != nullptr) {
            return std::unexpected(game_error(
                DiagnosticCode::game_scene_invalid, "Field is not valid for this rule event", source,
                std::string{pointer} + "/" + std::string{field}));
        }
    }
    return event;
}

Result<GameRuleConditionPlan> parse_rule_condition(
    const Json& object,
    const std::string_view pointer,
    const GameRuleEventPlan& event,
    const std::unordered_map<std::string, std::uint32_t>& states,
    const std::unordered_map<std::string, std::uint32_t>& spawn_groups,
    const std::vector<GameSpawnGroupPlan>& spawn_plans,
    const std::unordered_map<std::string, std::uint32_t>& tile_layers,
    const std::vector<GameTileLayerPlan>& tile_layer_plans,
    const std::unordered_map<std::string, std::uint32_t>& fields,
    const std::vector<GameFieldPlan>& field_plans,
    const std::vector<GameGridPlan>& grid_plans,
    const bool version_0_6,
    const std::string_view source) {
    if (auto rejected = reject_unknown(
            object,
            version_0_6
                ? std::initializer_list<std::string_view>{
                      "kind", "state", "group", "op", "value", "layer", "field", "x", "y", "target"}
                : std::initializer_list<std::string_view>{"kind", "state", "group", "op", "value"},
            source, pointer,
            DiagnosticCode::game_scene_invalid);
        !rejected) {
        return std::unexpected(std::move(rejected.error()));
    }
    auto kind = string_value(object, "kind", source, pointer, DiagnosticCode::game_scene_invalid);
    if (!kind) return std::unexpected(std::move(kind.error()));
    auto op = string_value(object, "op", source, pointer, DiagnosticCode::game_scene_invalid);
    if (!op) return std::unexpected(std::move(op.error()));
    auto comparison = parse_comparison(*op, source, std::string{pointer} + "/op");
    if (!comparison) return std::unexpected(std::move(comparison.error()));
    GameRuleConditionPlan condition{};
    condition.comparison = *comparison;
    const bool collision_context = event.kind == GameRuleEventKind::collision ||
                                   event.kind == GameRuleEventKind::contact_begin ||
                                   event.kind == GameRuleEventKind::contact_end;
    const bool event_entity_context = event.kind == GameRuleEventKind::animation_finished;
    if (*kind == "int_state") {
        condition.kind = GameRuleConditionKind::int_state;
        auto state = string_value(object, "state", source, pointer, DiagnosticCode::game_scene_invalid);
        if (!state) return std::unexpected(std::move(state.error()));
        auto state_index = index_by_name(
            states, *state, "integer state", source, std::string{pointer} + "/state",
            DiagnosticCode::game_scene_invalid);
        if (!state_index) return std::unexpected(std::move(state_index.error()));
        condition.state_index = *state_index;
        auto value = i32_value(
            object, "value", std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max(),
            source, pointer, DiagnosticCode::game_scene_invalid);
        if (!value) return std::unexpected(std::move(value.error()));
        condition.value = *value;
        if (optional(object, "group") != nullptr) {
            return std::unexpected(game_error(
                DiagnosticCode::game_scene_invalid, "int_state condition does not accept group", source,
                std::string{pointer} + "/group"));
        }
    } else if (*kind == "group_active_count") {
        condition.kind = GameRuleConditionKind::group_active_count;
        auto group = string_value(object, "group", source, pointer, DiagnosticCode::game_scene_invalid);
        if (!group) return std::unexpected(std::move(group.error()));
        auto group_index = index_by_name(
            spawn_groups, *group, "spawn group", source, std::string{pointer} + "/group",
            DiagnosticCode::game_scene_invalid);
        if (!group_index) return std::unexpected(std::move(group_index.error()));
        condition.spawn_group_index = *group_index;
        auto value = i32_value(
            object, "value", 0, static_cast<std::int32_t>(spawn_plans[*group_index].count), source, pointer,
            DiagnosticCode::game_scene_invalid);
        if (!value) return std::unexpected(std::move(value.error()));
        condition.value = *value;
        if (optional(object, "state") != nullptr) {
            return std::unexpected(game_error(
                DiagnosticCode::game_scene_invalid, "group_active_count condition does not accept state", source,
                std::string{pointer} + "/state"));
        }
    } else if (version_0_6 && (*kind == "tile_value" || *kind == "field_value")) {
        const bool tile_condition = *kind == "tile_value";
        condition.kind = tile_condition ? GameRuleConditionKind::tile_value
                                        : GameRuleConditionKind::field_value;
        std::uint32_t grid_index = 0U;
        if (tile_condition) {
            auto layer = string_value(
                object, "layer", source, pointer, DiagnosticCode::game_tile_field_invalid);
            if (!layer) return std::unexpected(std::move(layer.error()));
            auto layer_index = index_by_name(
                tile_layers, *layer, "tile layer", source, std::string{pointer} + "/layer",
                DiagnosticCode::game_tile_field_invalid);
            if (!layer_index) return std::unexpected(std::move(layer_index.error()));
            condition.tile_layer_index = *layer_index;
            grid_index = tile_layer_plans[*layer_index].grid_index;
            const auto maximum = static_cast<std::int32_t>(
                tile_layer_plans[*layer_index].atlas_columns * tile_layer_plans[*layer_index].atlas_rows);
            auto value = i32_value(
                object, "value", 0, maximum, source, pointer, DiagnosticCode::game_tile_field_invalid);
            if (!value) return std::unexpected(std::move(value.error()));
            condition.value = *value;
            if (optional(object, "field") != nullptr) {
                return std::unexpected(game_error(
                    DiagnosticCode::game_tile_field_invalid,
                    "tile_value does not accept field", source, std::string{pointer} + "/field"));
            }
        } else {
            auto field = string_value(
                object, "field", source, pointer, DiagnosticCode::game_tile_field_invalid);
            if (!field) return std::unexpected(std::move(field.error()));
            auto field_index = index_by_name(
                fields, *field, "integer field", source, std::string{pointer} + "/field",
                DiagnosticCode::game_tile_field_invalid);
            if (!field_index) return std::unexpected(std::move(field_index.error()));
            condition.field_index = *field_index;
            grid_index = field_plans[*field_index].grid_index;
            auto value = i32_value(
                object, "value", field_plans[*field_index].minimum, field_plans[*field_index].maximum,
                source, pointer, DiagnosticCode::game_tile_field_invalid);
            if (!value) return std::unexpected(std::move(value.error()));
            condition.value = *value;
            if (optional(object, "layer") != nullptr) {
                return std::unexpected(game_error(
                    DiagnosticCode::game_tile_field_invalid,
                    "field_value does not accept layer", source, std::string{pointer} + "/layer"));
            }
        }
        if (optional(object, "state") != nullptr || optional(object, "group") != nullptr) {
            return std::unexpected(game_error(
                DiagnosticCode::game_tile_field_invalid,
                "Tile and field conditions do not accept state or group", source, std::string{pointer}));
        }
        auto cell = parse_cell_source(
            object, pointer, grid_plans[grid_index], spawn_groups, spawn_plans,
            collision_context, event_entity_context, source, DiagnosticCode::game_tile_field_invalid);
        if (!cell) return std::unexpected(std::move(cell.error()));
        condition.cell_source = cell->kind;
        condition.cell_x = cell->x;
        condition.cell_y = cell->y;
        condition.cell_target = cell->target;
    } else {
        return std::unexpected(game_error(
            DiagnosticCode::game_scene_invalid, "Unsupported rule condition kind", source,
            std::string{pointer} + "/kind"));
    }
    if (condition.kind == GameRuleConditionKind::int_state ||
        condition.kind == GameRuleConditionKind::group_active_count) {
        for (const auto field : {"layer", "field", "x", "y", "target"}) {
            if (optional(object, field) != nullptr) {
                return std::unexpected(game_error(
                    DiagnosticCode::game_scene_invalid,
                    "Field is not valid for this rule condition", source,
                    std::string{pointer} + "/" + field));
            }
        }
    }
    return condition;
}

Result<GameRuleActionPlan> parse_rule_action(
    const Json& object,
    const std::string_view pointer,
    const GameRuleEventPlan& event,
    const std::unordered_map<std::string, std::uint32_t>& states,
    const std::vector<IntStatePlan>& state_plans,
    const std::unordered_map<std::string, std::uint32_t>& spawn_groups,
    const std::vector<GameSpawnGroupPlan>& spawn_plans,
    const std::unordered_map<std::string, std::uint32_t>& pools,
    const std::vector<GamePoolPlan>& pool_plans,
    const std::unordered_map<std::string, std::uint32_t>& grids,
    const std::unordered_map<std::string, std::uint32_t>& systems,
    const std::vector<GameSystemPlan>& system_plans,
    const std::vector<GameCollisionRulePlan>& collision_rule_plans,
    const std::unordered_map<std::string, std::uint32_t>& assets,
    const std::vector<GameAssetPlan>& asset_plans,
    const std::unordered_map<std::string, std::uint32_t>& animations,
    const std::vector<GameAnimationPlan>& animation_plans,
    const std::unordered_map<std::string, std::uint32_t>& tile_layers,
    const std::vector<GameTileLayerPlan>& tile_layer_plans,
    const std::unordered_map<std::string, std::uint32_t>& fields,
    const std::vector<GameFieldPlan>& field_plans,
    const std::unordered_map<std::string, std::uint32_t>& locales,
    const std::unordered_map<std::string, std::uint32_t>& input_profiles,
    const std::unordered_map<std::string, std::uint32_t>& particle_emitters,
    const std::vector<GameParticleEmitterPlan>& particle_emitter_plans,
    const std::vector<GameGridPlan>& grid_plans,
    const GameSavePlan& save_plan,
    const bool version_0_6,
    const std::string_view source) {
    if (auto rejected = reject_unknown(
            object,
            {"kind", "state", "value", "target", "group", "count", "velocity", "system", "direction",
             "asset", "grid", "occupied_groups", "result_state", "pool", "position", "rotation",
             "lifetime_ticks", "animation", "restart", "layer", "field", "x", "y", "slot",
             "amplitude", "duration_ticks", "zoom", "locale", "profile", "loop", "fade_ticks",
             "volume", "emitter", "particles"},
            source, pointer, DiagnosticCode::game_scene_invalid);
        !rejected) {
        return std::unexpected(std::move(rejected.error()));
    }
    auto kind = string_value(object, "kind", source, pointer, DiagnosticCode::game_scene_invalid);
    if (!kind) return std::unexpected(std::move(kind.error()));
    GameRuleActionPlan action{};
    const bool collision_context = event.kind == GameRuleEventKind::collision ||
                                   event.kind == GameRuleEventKind::contact_begin ||
                                   event.kind == GameRuleEventKind::contact_end;
    const bool event_entity_context = event.kind == GameRuleEventKind::animation_finished;
    const auto reject_action_parameters = [&](
        const std::initializer_list<std::string_view> allowed,
        const DiagnosticCode diagnostic_code = DiagnosticCode::game_scene_invalid) -> Result<void> {
        const auto is_allowed = [&](const std::string_view field) {
            return field == "kind" || std::find(allowed.begin(), allowed.end(), field) != allowed.end();
        };
        for (const auto field : {
                 "state", "value", "target", "group", "count", "velocity", "system", "direction", "asset",
                 "grid", "occupied_groups", "result_state", "pool", "position", "rotation",
                 "lifetime_ticks", "animation", "restart", "layer", "field", "x", "y", "slot",
                 "amplitude", "duration_ticks", "zoom", "locale", "profile", "loop", "fade_ticks",
                 "volume", "emitter", "particles"}) {
            if (!is_allowed(field) && optional(object, field) != nullptr) {
                return std::unexpected(game_error(
                    diagnostic_code, "Field is not valid for this rule action", source,
                    std::string{pointer} + "/" + field));
            }
        }
        return {};
    };
    const auto parse_state_reference = [&](const std::string_view field) -> Result<std::uint32_t> {
        auto state = string_value(object, field, source, pointer, DiagnosticCode::game_scene_invalid);
        if (!state) return std::unexpected(std::move(state.error()));
        return index_by_name(
            states, *state, "integer state", source, std::string{pointer} + "/" + std::string{field},
            DiagnosticCode::game_scene_invalid);
    };
    const auto parse_target = [&]() -> Result<GameRuleTargetPlan> {
        auto member = required(object, "target", source, pointer, DiagnosticCode::game_scene_invalid);
        if (!member) return std::unexpected(std::move(member.error()));
        return parse_rule_target(
            **member, std::string{pointer} + "/target", spawn_groups, spawn_plans, collision_context,
            event_entity_context, source,
            DiagnosticCode::game_scene_invalid);
    };
    const auto parse_pool_reference = [&]() -> Result<std::uint32_t> {
        auto pool = string_value(object, "pool", source, pointer, DiagnosticCode::game_pool_invalid);
        if (!pool) return std::unexpected(std::move(pool.error()));
        return index_by_name(
            pools, *pool, "pool", source, std::string{pointer} + "/pool", DiagnosticCode::game_pool_invalid);
    };
    const auto parse_optional_result_state = [&]
        (const DiagnosticCode diagnostic_code = DiagnosticCode::game_pool_invalid) -> Result<void> {
        if (optional(object, "result_state") == nullptr) return {};
        auto state = string_value(object, "result_state", source, pointer, diagnostic_code);
        if (!state) return std::unexpected(std::move(state.error()));
        auto state_index = index_by_name(
            states, *state, "integer state", source, std::string{pointer} + "/result_state",
            diagnostic_code);
        if (!state_index) return std::unexpected(std::move(state_index.error()));
        if (state_plans[*state_index].minimum > 0 || state_plans[*state_index].maximum < 1) {
            return std::unexpected(game_error(
                diagnostic_code, "result_state range must contain zero and one", source,
                std::string{pointer} + "/result_state"));
        }
        action.has_result_state = true;
        action.result_state_index = *state_index;
        return {};
    };
    if (*kind == "set_int_state" || *kind == "add_int_state") {
        if (auto checked = reject_action_parameters({"state", "value"}); !checked) {
            return std::unexpected(std::move(checked.error()));
        }
        action.kind = *kind == "set_int_state" ? GameRuleActionKind::set_int_state
                                                : GameRuleActionKind::add_int_state;
        auto state_index = parse_state_reference("state");
        if (!state_index) return std::unexpected(std::move(state_index.error()));
        action.state_index = *state_index;
        const auto minimum = action.kind == GameRuleActionKind::set_int_state
                                 ? state_plans[*state_index].minimum
                                 : std::numeric_limits<std::int32_t>::min();
        const auto maximum = action.kind == GameRuleActionKind::set_int_state
                                 ? state_plans[*state_index].maximum
                                 : std::numeric_limits<std::int32_t>::max();
        auto value = i32_value(
            object, "value", minimum, maximum, source, pointer, DiagnosticCode::game_scene_invalid);
        if (!value) return std::unexpected(std::move(value.error()));
        action.value = *value;
    } else if (*kind == "activate" || *kind == "deactivate") {
        if (auto checked = reject_action_parameters({"target"}); !checked) {
            return std::unexpected(std::move(checked.error()));
        }
        action.kind = *kind == "activate" ? GameRuleActionKind::activate : GameRuleActionKind::deactivate;
        auto target = parse_target();
        if (!target) return std::unexpected(std::move(target.error()));
        action.target = *target;
    } else if (*kind == "set_group_active_count") {
        if (auto checked = reject_action_parameters({"group", "count"}); !checked) {
            return std::unexpected(std::move(checked.error()));
        }
        action.kind = GameRuleActionKind::set_group_active_count;
        auto group = string_value(object, "group", source, pointer, DiagnosticCode::game_scene_invalid);
        if (!group) return std::unexpected(std::move(group.error()));
        auto group_index = index_by_name(
            spawn_groups, *group, "spawn group", source, std::string{pointer} + "/group",
            DiagnosticCode::game_scene_invalid);
        if (!group_index) return std::unexpected(std::move(group_index.error()));
        action.spawn_group_index = *group_index;
        auto count_member = required(object, "count", source, pointer, DiagnosticCode::game_scene_invalid);
        if (!count_member) return std::unexpected(std::move(count_member.error()));
        if ((*count_member)->is_object()) {
            if (auto rejected = reject_unknown(
                    **count_member, {"state"}, source, std::string{pointer} + "/count",
                    DiagnosticCode::game_scene_invalid);
                !rejected) {
                return std::unexpected(std::move(rejected.error()));
            }
            auto state = string_value(
                **count_member, "state", source, std::string{pointer} + "/count",
                DiagnosticCode::game_scene_invalid);
            if (!state) return std::unexpected(std::move(state.error()));
            auto state_index = index_by_name(
                states, *state, "integer state", source, std::string{pointer} + "/count/state",
                DiagnosticCode::game_scene_invalid);
            if (!state_index) return std::unexpected(std::move(state_index.error()));
            if (state_plans[*state_index].minimum < 0 ||
                state_plans[*state_index].maximum > static_cast<std::int32_t>(spawn_plans[*group_index].count)) {
                return std::unexpected(game_error(
                    DiagnosticCode::game_scene_invalid,
                    "Active-count state range must fit the spawn group count",
                    source, std::string{pointer} + "/count/state"));
            }
            action.count_from_state = true;
            action.count_state_index = *state_index;
        } else if ((*count_member)->is_number_integer() || (*count_member)->is_number_unsigned()) {
            auto count = u32_value(
                object, "count", 0U, spawn_plans[*group_index].count, source, pointer,
                DiagnosticCode::game_scene_invalid);
            if (!count) return std::unexpected(std::move(count.error()));
            action.count_constant = *count;
        } else {
            return std::unexpected(game_error(
                DiagnosticCode::game_scene_invalid, "count must be an integer or a state reference", source,
                std::string{pointer} + "/count"));
        }
    } else if (*kind == "set_velocity") {
        if (auto checked = reject_action_parameters({"target", "velocity"}); !checked) {
            return std::unexpected(std::move(checked.error()));
        }
        action.kind = GameRuleActionKind::set_velocity;
        auto target = parse_target();
        if (!target) return std::unexpected(std::move(target.error()));
        action.target = *target;
        if ((target->kind == GameRuleTargetKind::spawn_group || target->kind == GameRuleTargetKind::spawn_index) &&
            !spawn_plans[target->spawn_group_index].has_velocity) {
            return std::unexpected(game_error(
                DiagnosticCode::game_scene_invalid, "set_velocity target requires Velocity2D", source,
                std::string{pointer} + "/target"));
        }
        if (target->kind == GameRuleTargetKind::collision_a || target->kind == GameRuleTargetKind::collision_b) {
            const auto collision_group = target->kind == GameRuleTargetKind::collision_a
                                             ? collision_rule_plans[event.collision_rule_index].group_a
                                             : collision_rule_plans[event.collision_rule_index].group_b;
            const bool every_target_has_velocity = std::ranges::all_of(
                spawn_plans, [collision_group](const GameSpawnGroupPlan& spawn) {
                    return !spawn.has_collider || spawn.collider.group != collision_group || spawn.has_velocity;
                });
            if (!every_target_has_velocity) {
                return std::unexpected(game_error(
                    DiagnosticCode::game_scene_invalid,
                    "set_velocity collision target requires Velocity2D on every endpoint group", source,
                    std::string{pointer} + "/target"));
            }
        }
        auto velocity_member = required(
            object, "velocity", source, pointer, DiagnosticCode::game_scene_invalid);
        if (!velocity_member) return std::unexpected(std::move(velocity_member.error()));
        auto velocity = vec2(**velocity_member, source, std::string{pointer} + "/velocity");
        if (!velocity) return std::unexpected(std::move(velocity.error()));
        action.velocity = *velocity;
    } else if (*kind == "queue_grid_direction") {
        if (auto checked = reject_action_parameters({"system", "direction"}); !checked) {
            return std::unexpected(std::move(checked.error()));
        }
        action.kind = GameRuleActionKind::queue_grid_direction;
        auto system = string_value(object, "system", source, pointer, DiagnosticCode::game_scene_invalid);
        if (!system) return std::unexpected(std::move(system.error()));
        auto system_index = index_by_name(
            systems, *system, "system", source, std::string{pointer} + "/system",
            DiagnosticCode::game_scene_invalid);
        if (!system_index) return std::unexpected(std::move(system_index.error()));
        if (system_plans[*system_index].operation != GameOperationId::grid_motion) {
            return std::unexpected(game_error(
                DiagnosticCode::game_scene_invalid, "queue_grid_direction requires a grid_motion system", source,
                std::string{pointer} + "/system"));
        }
        auto direction_text = string_value(
            object, "direction", source, pointer, DiagnosticCode::game_scene_invalid);
        if (!direction_text) return std::unexpected(std::move(direction_text.error()));
        auto direction = parse_direction(*direction_text, source, std::string{pointer} + "/direction");
        if (!direction) return std::unexpected(std::move(direction.error()));
        action.system_index = *system_index;
        action.direction = *direction;
    } else if (*kind == "reset_group") {
        if (auto checked = reject_action_parameters({"group"}); !checked) {
            return std::unexpected(std::move(checked.error()));
        }
        action.kind = GameRuleActionKind::reset_group;
        auto group = string_value(object, "group", source, pointer, DiagnosticCode::game_scene_invalid);
        if (!group) return std::unexpected(std::move(group.error()));
        auto group_index = index_by_name(
            spawn_groups, *group, "spawn group", source, std::string{pointer} + "/group",
            DiagnosticCode::game_scene_invalid);
        if (!group_index) return std::unexpected(std::move(group_index.error()));
        action.spawn_group_index = *group_index;
    } else if (*kind == "play_sound") {
        if (auto checked = reject_action_parameters({"asset"}); !checked) {
            return std::unexpected(std::move(checked.error()));
        }
        action.kind = GameRuleActionKind::play_sound;
        auto asset = string_value(object, "asset", source, pointer, DiagnosticCode::game_scene_invalid);
        if (!asset) return std::unexpected(std::move(asset.error()));
        auto asset_index = index_by_name(
            assets, *asset, "audio asset", source, std::string{pointer} + "/asset",
            DiagnosticCode::game_scene_invalid);
        if (!asset_index) return std::unexpected(std::move(asset_index.error()));
        if (asset_plans[*asset_index].kind != GameAssetKind::wav) {
            return std::unexpected(game_error(
                DiagnosticCode::game_scene_invalid, "play_sound must reference a WAV asset", source,
                std::string{pointer} + "/asset"));
        }
        action.asset_index = *asset_index;
    } else if (*kind == "relocate_to_free_cell") {
        if (auto checked = reject_action_parameters(
                {"target", "grid", "occupied_groups", "result_state"}); !checked) {
            return std::unexpected(std::move(checked.error()));
        }
        action.kind = GameRuleActionKind::relocate_to_free_cell;
        if (!collision_context) {
            return std::unexpected(game_error(
                DiagnosticCode::game_scene_invalid, "relocate_to_free_cell requires a collision event", source,
                pointer));
        }
        auto target = parse_target();
        if (!target) return std::unexpected(std::move(target.error()));
        if (target->kind != GameRuleTargetKind::collision_a && target->kind != GameRuleTargetKind::collision_b) {
            return std::unexpected(game_error(
                DiagnosticCode::game_scene_invalid,
                "relocate_to_free_cell target must be collision_a or collision_b",
                source, std::string{pointer} + "/target"));
        }
        action.target = *target;
        auto grid = string_value(object, "grid", source, pointer, DiagnosticCode::game_scene_invalid);
        if (!grid) return std::unexpected(std::move(grid.error()));
        auto grid_index = index_by_name(
            grids, *grid, "grid", source, std::string{pointer} + "/grid",
            DiagnosticCode::game_scene_invalid);
        if (!grid_index) return std::unexpected(std::move(grid_index.error()));
        action.grid_index = *grid_index;
        auto occupied = required(
            object, "occupied_groups", source, pointer, DiagnosticCode::game_scene_invalid);
        if (!occupied) return std::unexpected(std::move(occupied.error()));
        if (!(*occupied)->is_array() || (*occupied)->empty() ||
            (*occupied)->size() > GameRuleActionPlan::max_occupancy_groups) {
            return std::unexpected(game_error(
                DiagnosticCode::game_scene_invalid, "occupied_groups must contain one to 32 groups", source,
                std::string{pointer} + "/occupied_groups"));
        }
        action.occupancy_group_indices.reserve((*occupied)->size());
        for (std::size_t index = 0U; index < (*occupied)->size(); ++index) {
            if (!(**occupied)[index].is_string()) {
                return std::unexpected(game_error(
                    DiagnosticCode::game_scene_invalid, "Occupied group must be a string", source,
                    std::string{pointer} + "/occupied_groups/" + std::to_string(index)));
            }
            const auto& name = (**occupied)[index].get_ref<const std::string&>();
            auto group_index = index_by_name(
                spawn_groups, name, "spawn group", source,
                std::string{pointer} + "/occupied_groups/" + std::to_string(index),
                DiagnosticCode::game_scene_invalid);
            if (!group_index) return std::unexpected(std::move(group_index.error()));
            if (std::find(action.occupancy_group_indices.begin(), action.occupancy_group_indices.end(),
                          *group_index) != action.occupancy_group_indices.end()) {
                return std::unexpected(game_error(
                    DiagnosticCode::game_scene_invalid, "occupied_groups contains a duplicate", source,
                    std::string{pointer} + "/occupied_groups/" + std::to_string(index)));
            }
            action.occupancy_group_indices.push_back(*group_index);
        }
        if (optional(object, "result_state") != nullptr) {
            auto state_index = parse_state_reference("result_state");
            if (!state_index) return std::unexpected(std::move(state_index.error()));
            if (state_plans[*state_index].minimum > 0 || state_plans[*state_index].maximum < 1) {
                return std::unexpected(game_error(
                    DiagnosticCode::game_scene_invalid, "result_state range must contain zero and one", source,
                    std::string{pointer} + "/result_state"));
            }
            action.has_result_state = true;
            action.result_state_index = *state_index;
        }
    } else if (*kind == "spawn_from_pool") {
        if (auto checked = reject_action_parameters(
                {"pool", "position", "velocity", "rotation", "lifetime_ticks", "result_state"},
                DiagnosticCode::game_pool_invalid);
            !checked) {
            return std::unexpected(std::move(checked.error()));
        }
        action.kind = GameRuleActionKind::spawn_from_pool;
        auto pool_index = parse_pool_reference();
        if (!pool_index) return std::unexpected(std::move(pool_index.error()));
        action.pool_index = *pool_index;
        const auto& pool = pool_plans[*pool_index];
        const auto& pool_group = spawn_plans[pool.spawn_group_index];
        auto position_member = required(
            object, "position", source, pointer, DiagnosticCode::game_pool_invalid);
        if (!position_member) return std::unexpected(std::move(position_member.error()));
        const auto position_pointer = std::string{pointer} + "/position";
        if (auto rejected = reject_unknown(
                **position_member, {"kind", "value", "target", "offset"}, source, position_pointer,
                DiagnosticCode::game_pool_invalid);
            !rejected) {
            return std::unexpected(std::move(rejected.error()));
        }
        auto position_kind = string_value(
            **position_member, "kind", source, position_pointer, DiagnosticCode::game_pool_invalid);
        if (!position_kind) return std::unexpected(std::move(position_kind.error()));
        auto offset = optional_vec2(
            **position_member, "offset", {}, source, position_pointer, DiagnosticCode::game_pool_invalid);
        if (!offset) return std::unexpected(std::move(offset.error()));
        action.pool_position_offset = *offset;
        if (*position_kind == "initial") {
            action.pool_position_kind = GamePoolSpawnPositionKind::initial;
            if (optional(**position_member, "value") != nullptr || optional(**position_member, "target") != nullptr) {
                return std::unexpected(game_error(
                    DiagnosticCode::game_pool_invalid, "initial position accepts only an optional offset", source,
                    position_pointer));
            }
        } else if (*position_kind == "constant") {
            action.pool_position_kind = GamePoolSpawnPositionKind::constant;
            auto value = required(
                **position_member, "value", source, position_pointer, DiagnosticCode::game_pool_invalid);
            if (!value) return std::unexpected(std::move(value.error()));
            auto parsed = vec2(**value, source, position_pointer + "/value", DiagnosticCode::game_pool_invalid);
            if (!parsed) return std::unexpected(std::move(parsed.error()));
            action.pool_position = *parsed;
            if (optional(**position_member, "target") != nullptr) {
                return std::unexpected(game_error(
                    DiagnosticCode::game_pool_invalid, "constant position does not accept target", source,
                    position_pointer + "/target"));
            }
        } else if (*position_kind == "target") {
            action.pool_position_kind = GamePoolSpawnPositionKind::target;
            auto target_member = required(
                **position_member, "target", source, position_pointer, DiagnosticCode::game_pool_invalid);
            if (!target_member) return std::unexpected(std::move(target_member.error()));
            auto target = parse_rule_target(
                **target_member, position_pointer + "/target", spawn_groups, spawn_plans,
                collision_context, false, source, DiagnosticCode::game_pool_invalid);
            if (!target) return std::unexpected(std::move(target.error()));
            if (target->kind == GameRuleTargetKind::spawn_group) {
                return std::unexpected(game_error(
                    DiagnosticCode::game_pool_invalid,
                    "Pool spawn position target must be a fixed index or collision endpoint", source,
                    position_pointer + "/target"));
            }
            action.pool_position_target = *target;
            if (optional(**position_member, "value") != nullptr) {
                return std::unexpected(game_error(
                    DiagnosticCode::game_pool_invalid, "target position does not accept value", source,
                    position_pointer + "/value"));
            }
        } else {
            return std::unexpected(game_error(
                DiagnosticCode::game_pool_invalid,
                "Pool spawn position kind must be initial, constant, or target", source,
                position_pointer + "/kind"));
        }
        if (optional(object, "velocity") != nullptr) {
            if (!pool_group.has_velocity) {
                return std::unexpected(game_error(
                    DiagnosticCode::game_pool_invalid,
                    "Pool velocity override requires Velocity2D on every slot", source,
                    std::string{pointer} + "/velocity"));
            }
            auto parsed = vec2(
                *optional(object, "velocity"), source, std::string{pointer} + "/velocity",
                DiagnosticCode::game_pool_invalid);
            if (!parsed) return std::unexpected(std::move(parsed.error()));
            action.has_velocity_override = true;
            action.velocity = *parsed;
        }
        if (const auto* rotation = optional(object, "rotation"); rotation != nullptr) {
            auto parsed = number(
                *rotation, source, std::string{pointer} + "/rotation", DiagnosticCode::game_pool_invalid);
            if (!parsed) return std::unexpected(std::move(parsed.error()));
            action.has_rotation_override = true;
            action.rotation = *parsed;
        }
        if (optional(object, "lifetime_ticks") != nullptr) {
            auto lifetime = u32_value(
                object, "lifetime_ticks", 1U, 1'000'000U, source, pointer,
                DiagnosticCode::game_pool_invalid);
            if (!lifetime) return std::unexpected(std::move(lifetime.error()));
            action.has_lifetime = true;
            action.lifetime_ticks = *lifetime;
        }
        if (auto result = parse_optional_result_state(); !result) {
            return std::unexpected(std::move(result.error()));
        }
    } else if (*kind == "release_to_pool") {
        if (auto checked = reject_action_parameters(
                {"pool", "target", "result_state"}, DiagnosticCode::game_pool_invalid);
            !checked) {
            return std::unexpected(std::move(checked.error()));
        }
        action.kind = GameRuleActionKind::release_to_pool;
        auto pool_index = parse_pool_reference();
        if (!pool_index) return std::unexpected(std::move(pool_index.error()));
        action.pool_index = *pool_index;
        auto target_member = required(object, "target", source, pointer, DiagnosticCode::game_pool_invalid);
        if (!target_member) return std::unexpected(std::move(target_member.error()));
        auto target = parse_rule_target(
            **target_member, std::string{pointer} + "/target", spawn_groups, spawn_plans,
            collision_context, false, source, DiagnosticCode::game_pool_invalid);
        if (!target) return std::unexpected(std::move(target.error()));
        if (target->kind == GameRuleTargetKind::spawn_group) {
            return std::unexpected(game_error(
                DiagnosticCode::game_pool_invalid,
                "release_to_pool target must be a fixed index or collision endpoint", source,
                std::string{pointer} + "/target"));
        }
        if (target->kind == GameRuleTargetKind::spawn_index &&
            target->spawn_group_index != pool_plans[*pool_index].spawn_group_index) {
            return std::unexpected(game_error(
                DiagnosticCode::game_pool_invalid, "release_to_pool index target is not owned by the pool", source,
                std::string{pointer} + "/target"));
        }
        action.target = *target;
        if (auto result = parse_optional_result_state(); !result) {
            return std::unexpected(std::move(result.error()));
        }
    } else if (*kind == "reset_pool") {
        if (auto checked = reject_action_parameters({"pool"}, DiagnosticCode::game_pool_invalid); !checked) {
            return std::unexpected(std::move(checked.error()));
        }
        action.kind = GameRuleActionKind::reset_pool;
        auto pool_index = parse_pool_reference();
        if (!pool_index) return std::unexpected(std::move(pool_index.error()));
        action.pool_index = *pool_index;
    } else if (version_0_6 && (*kind == "play_animation" || *kind == "stop_animation")) {
        const bool play = *kind == "play_animation";
        if (auto checked = reject_action_parameters(
                play ? std::initializer_list<std::string_view>{"target", "animation", "restart"}
                     : std::initializer_list<std::string_view>{"target"},
                DiagnosticCode::game_animation_invalid);
            !checked) {
            return std::unexpected(std::move(checked.error()));
        }
        action.kind = play ? GameRuleActionKind::play_animation : GameRuleActionKind::stop_animation;
        auto target = parse_target();
        if (!target) return std::unexpected(std::move(target.error()));
        action.target = *target;
        const auto target_group_has_sprite = [&](const std::uint32_t group_index) {
            return group_index < spawn_plans.size() && spawn_plans[group_index].has_sprite;
        };
        if ((target->kind == GameRuleTargetKind::spawn_group ||
             target->kind == GameRuleTargetKind::spawn_index) &&
            !target_group_has_sprite(target->spawn_group_index)) {
            return std::unexpected(game_error(
                DiagnosticCode::game_animation_invalid,
                "Animation target requires a sprite component", source,
                std::string{pointer} + "/target"));
        }
        if (target->kind == GameRuleTargetKind::collision_a ||
            target->kind == GameRuleTargetKind::collision_b) {
            const auto collision_group = target->kind == GameRuleTargetKind::collision_a
                                             ? collision_rule_plans[event.collision_rule_index].group_a
                                             : collision_rule_plans[event.collision_rule_index].group_b;
            const bool all_have_sprite = std::ranges::all_of(
                spawn_plans, [collision_group](const GameSpawnGroupPlan& spawn) {
                    return !spawn.has_collider || spawn.collider.group != collision_group || spawn.has_sprite;
                });
            if (!all_have_sprite) {
                return std::unexpected(game_error(
                    DiagnosticCode::game_animation_invalid,
                    "Animation collision target requires a sprite on every endpoint group", source,
                    std::string{pointer} + "/target"));
            }
        }
        if (play) {
            auto animation = string_value(
                object, "animation", source, pointer, DiagnosticCode::game_animation_invalid);
            if (!animation) return std::unexpected(std::move(animation.error()));
            auto animation_index = index_by_name(
                animations, *animation, "animation", source, std::string{pointer} + "/animation",
                DiagnosticCode::game_animation_invalid);
            if (!animation_index) return std::unexpected(std::move(animation_index.error()));
            if (*animation_index >= animation_plans.size()) {
                return std::unexpected(game_error(
                    DiagnosticCode::game_animation_invalid, "Animation index is invalid", source,
                    std::string{pointer} + "/animation"));
            }
            action.animation_index = *animation_index;
            auto restart = optional_bool(
                object, "restart", true, source, pointer, DiagnosticCode::game_animation_invalid);
            if (!restart) return std::unexpected(std::move(restart.error()));
            action.restart_animation = *restart;
        }
    } else if (version_0_6 && (*kind == "set_tile" || *kind == "set_field" || *kind == "add_field")) {
        if (auto checked = reject_action_parameters(
                *kind == "set_tile"
                    ? std::initializer_list<std::string_view>{"layer", "x", "y", "target", "value", "result_state"}
                    : std::initializer_list<std::string_view>{"field", "x", "y", "target", "value", "result_state"},
                DiagnosticCode::game_tile_field_invalid);
            !checked) {
            return std::unexpected(std::move(checked.error()));
        }
        const bool tile_action = *kind == "set_tile";
        std::uint32_t grid_index = 0U;
        if (tile_action) {
            action.kind = GameRuleActionKind::set_tile;
            auto layer = string_value(
                object, "layer", source, pointer, DiagnosticCode::game_tile_field_invalid);
            if (!layer) return std::unexpected(std::move(layer.error()));
            auto layer_index = index_by_name(
                tile_layers, *layer, "tile layer", source, std::string{pointer} + "/layer",
                DiagnosticCode::game_tile_field_invalid);
            if (!layer_index) return std::unexpected(std::move(layer_index.error()));
            action.tile_layer_index = *layer_index;
            grid_index = tile_layer_plans[*layer_index].grid_index;
            const auto maximum = tile_layer_plans[*layer_index].atlas_columns *
                                 tile_layer_plans[*layer_index].atlas_rows;
            auto value = u32_value(
                object, "value", 0U, maximum, source, pointer, DiagnosticCode::game_tile_field_invalid);
            if (!value) return std::unexpected(std::move(value.error()));
            action.tile_value = *value;
        } else {
            action.kind = *kind == "set_field" ? GameRuleActionKind::set_field
                                                : GameRuleActionKind::add_field;
            auto field = string_value(
                object, "field", source, pointer, DiagnosticCode::game_tile_field_invalid);
            if (!field) return std::unexpected(std::move(field.error()));
            auto field_index = index_by_name(
                fields, *field, "integer field", source, std::string{pointer} + "/field",
                DiagnosticCode::game_tile_field_invalid);
            if (!field_index) return std::unexpected(std::move(field_index.error()));
            action.field_index = *field_index;
            grid_index = field_plans[*field_index].grid_index;
            const auto minimum = action.kind == GameRuleActionKind::set_field
                                     ? field_plans[*field_index].minimum
                                     : std::numeric_limits<std::int32_t>::min();
            const auto maximum = action.kind == GameRuleActionKind::set_field
                                     ? field_plans[*field_index].maximum
                                     : std::numeric_limits<std::int32_t>::max();
            auto value = i32_value(
                object, "value", minimum, maximum, source, pointer,
                DiagnosticCode::game_tile_field_invalid);
            if (!value) return std::unexpected(std::move(value.error()));
            action.value = *value;
        }
        auto cell = parse_cell_source(
            object, pointer, grid_plans[grid_index], spawn_groups, spawn_plans,
            collision_context, event_entity_context, source, DiagnosticCode::game_tile_field_invalid);
        if (!cell) return std::unexpected(std::move(cell.error()));
        action.cell_source = cell->kind;
        action.cell_x = cell->x;
        action.cell_y = cell->y;
        action.cell_target = cell->target;
        if (auto result = parse_optional_result_state(DiagnosticCode::game_tile_field_invalid); !result) {
            return std::unexpected(std::move(result.error()));
        }
    } else if (version_0_6 &&
               (*kind == "save_slot" || *kind == "load_slot" || *kind == "delete_slot")) {
        if (auto checked = reject_action_parameters(
                {"slot", "result_state"}, DiagnosticCode::game_save_invalid);
            !checked) {
            return std::unexpected(std::move(checked.error()));
        }
        if (!save_plan.enabled) {
            return std::unexpected(game_error(
                DiagnosticCode::game_save_invalid,
                "Save actions require a manifest save declaration", source, pointer));
        }
        action.kind = *kind == "save_slot" ? GameRuleActionKind::save_slot
                    : *kind == "load_slot" ? GameRuleActionKind::load_slot
                                             : GameRuleActionKind::delete_slot;
        auto slot = u32_value(
            object, "slot", 0U, save_plan.slot_count - 1U, source, pointer,
            DiagnosticCode::game_save_invalid);
        if (!slot) return std::unexpected(std::move(slot.error()));
        action.save_slot = *slot;
        if (auto result = parse_optional_result_state(DiagnosticCode::game_save_invalid); !result) {
            return std::unexpected(std::move(result.error()));
        }
    } else if (version_0_6 && *kind == "camera_shake") {
        if (auto checked = reject_action_parameters(
                {"amplitude", "duration_ticks"}, DiagnosticCode::game_presentation_invalid);
            !checked) {
            return std::unexpected(std::move(checked.error()));
        }
        action.kind = GameRuleActionKind::camera_shake;
        auto amplitude_member = required(
            object, "amplitude", source, pointer, DiagnosticCode::game_presentation_invalid);
        if (!amplitude_member) return std::unexpected(std::move(amplitude_member.error()));
        auto amplitude = number(
            **amplitude_member, source, std::string{pointer} + "/amplitude",
            DiagnosticCode::game_presentation_invalid);
        if (!amplitude) return std::unexpected(std::move(amplitude.error()));
        if (*amplitude < 0.0F || *amplitude > 1'000.0F) {
            return std::unexpected(game_error(
                DiagnosticCode::game_presentation_invalid,
                "Camera shake amplitude must be between zero and 1000", source,
                std::string{pointer} + "/amplitude"));
        }
        action.scalar = *amplitude;
        auto duration = u32_value(
            object, "duration_ticks", 1U, 1'000'000U, source, pointer,
            DiagnosticCode::game_presentation_invalid);
        if (!duration) return std::unexpected(std::move(duration.error()));
        action.duration_ticks = *duration;
    } else if (version_0_6 && *kind == "set_camera_zoom") {
        if (auto checked = reject_action_parameters(
                {"zoom"}, DiagnosticCode::game_presentation_invalid);
            !checked) {
            return std::unexpected(std::move(checked.error()));
        }
        action.kind = GameRuleActionKind::set_camera_zoom;
        auto zoom_member = required(
            object, "zoom", source, pointer, DiagnosticCode::game_presentation_invalid);
        if (!zoom_member) return std::unexpected(std::move(zoom_member.error()));
        auto zoom = number(
            **zoom_member, source, std::string{pointer} + "/zoom",
            DiagnosticCode::game_presentation_invalid);
        if (!zoom) return std::unexpected(std::move(zoom.error()));
        if (*zoom < 0.1F || *zoom > 10.0F) {
            return std::unexpected(game_error(
                DiagnosticCode::game_presentation_invalid,
                "Camera zoom must be between 0.1 and 10", source,
                std::string{pointer} + "/zoom"));
        }
        action.scalar = *zoom;
    } else if (version_0_6 && (*kind == "set_locale" || *kind == "set_input_profile")) {
        const bool locale_action = *kind == "set_locale";
        if (auto checked = reject_action_parameters(
                locale_action ? std::initializer_list<std::string_view>{"locale"}
                              : std::initializer_list<std::string_view>{"profile"},
                locale_action ? DiagnosticCode::game_localization_invalid
                              : DiagnosticCode::game_input_profile_invalid);
            !checked) {
            return std::unexpected(std::move(checked.error()));
        }
        action.kind = locale_action ? GameRuleActionKind::set_locale
                                    : GameRuleActionKind::set_input_profile;
        const auto field = locale_action ? "locale" : "profile";
        auto name = string_value(
            object, field, source, pointer,
            locale_action ? DiagnosticCode::game_localization_invalid
                          : DiagnosticCode::game_input_profile_invalid);
        if (!name) return std::unexpected(std::move(name.error()));
        auto index = index_by_name(
            locale_action ? locales : input_profiles, *name,
            locale_action ? "locale" : "input profile", source,
            std::string{pointer} + "/" + field,
            locale_action ? DiagnosticCode::game_localization_invalid
                          : DiagnosticCode::game_input_profile_invalid);
        if (!index) return std::unexpected(std::move(index.error()));
        if (locale_action) action.locale_index = *index;
        else action.input_profile_index = *index;
    } else if (version_0_6 &&
               (*kind == "play_music" || *kind == "stop_music" || *kind == "set_music_volume")) {
        if (*kind == "play_music") {
            if (auto checked = reject_action_parameters(
                    {"asset", "loop", "fade_ticks", "volume"}, DiagnosticCode::game_presentation_invalid);
                !checked) {
                return std::unexpected(std::move(checked.error()));
            }
            action.kind = GameRuleActionKind::play_music;
            auto asset = string_value(
                object, "asset", source, pointer, DiagnosticCode::game_presentation_invalid);
            if (!asset) return std::unexpected(std::move(asset.error()));
            auto asset_index = index_by_name(
                assets, *asset, "music asset", source, std::string{pointer} + "/asset",
                DiagnosticCode::game_presentation_invalid);
            if (!asset_index) return std::unexpected(std::move(asset_index.error()));
            if (asset_plans[*asset_index].kind != GameAssetKind::music) {
                return std::unexpected(game_error(
                    DiagnosticCode::game_presentation_invalid,
                    "play_music must reference a music asset", source,
                    std::string{pointer} + "/asset"));
            }
            action.asset_index = *asset_index;
            auto loop = optional_bool(
                object, "loop", false, source, pointer, DiagnosticCode::game_presentation_invalid);
            if (!loop) return std::unexpected(std::move(loop.error()));
            action.loop = *loop;
        } else if (*kind == "stop_music") {
            if (auto checked = reject_action_parameters(
                    {"fade_ticks"}, DiagnosticCode::game_presentation_invalid);
                !checked) {
                return std::unexpected(std::move(checked.error()));
            }
            action.kind = GameRuleActionKind::stop_music;
        } else {
            if (auto checked = reject_action_parameters(
                    {"volume", "fade_ticks"}, DiagnosticCode::game_presentation_invalid);
                !checked) {
                return std::unexpected(std::move(checked.error()));
            }
            action.kind = GameRuleActionKind::set_music_volume;
        }
        if (optional(object, "fade_ticks") != nullptr) {
            auto fade = u32_value(
                object, "fade_ticks", 0U, 1'000'000U, source, pointer,
                DiagnosticCode::game_presentation_invalid);
            if (!fade) return std::unexpected(std::move(fade.error()));
            action.fade_ticks = *fade;
        }
        if (optional(object, "volume") != nullptr) {
            auto volume_member = required(
                object, "volume", source, pointer, DiagnosticCode::game_presentation_invalid);
            if (!volume_member) return std::unexpected(std::move(volume_member.error()));
            auto volume = number(
                **volume_member, source, std::string{pointer} + "/volume",
                DiagnosticCode::game_presentation_invalid);
            if (!volume) return std::unexpected(std::move(volume.error()));
            if (*volume < 0.0F || *volume > 1.0F) {
                return std::unexpected(game_error(
                    DiagnosticCode::game_presentation_invalid,
                    "Music volume must be between zero and one", source,
                    std::string{pointer} + "/volume"));
            }
            action.scalar = *volume;
        } else if (action.kind == GameRuleActionKind::play_music) {
            action.scalar = 1.0F;
        } else if (action.kind == GameRuleActionKind::set_music_volume) {
            return std::unexpected(game_error(
                DiagnosticCode::game_presentation_invalid,
                "set_music_volume requires volume", source,
                std::string{pointer} + "/volume"));
        }
    } else if (version_0_6 && *kind == "emit_particles") {
        if (auto checked = reject_action_parameters(
                {"emitter", "particles", "position"}, DiagnosticCode::game_presentation_invalid);
            !checked) {
            return std::unexpected(std::move(checked.error()));
        }
        action.kind = GameRuleActionKind::emit_particles;
        auto emitter = string_value(
            object, "emitter", source, pointer, DiagnosticCode::game_presentation_invalid);
        if (!emitter) return std::unexpected(std::move(emitter.error()));
        auto emitter_index = index_by_name(
            particle_emitters, *emitter, "particle emitter", source,
            std::string{pointer} + "/emitter", DiagnosticCode::game_presentation_invalid);
        if (!emitter_index) return std::unexpected(std::move(emitter_index.error()));
        action.particle_emitter_index = *emitter_index;
        auto count = u32_value(
            object, "particles", 1U, particle_emitter_plans[*emitter_index].capacity,
            source, pointer, DiagnosticCode::game_presentation_invalid);
        if (!count) return std::unexpected(std::move(count.error()));
        action.particle_count = *count;
        auto position_member = required(
            object, "position", source, pointer, DiagnosticCode::game_presentation_invalid);
        if (!position_member) return std::unexpected(std::move(position_member.error()));
        const auto position_pointer = std::string{pointer} + "/position";
        if (auto rejected = reject_unknown(
                **position_member, {"kind", "value", "target", "offset"}, source, position_pointer,
                DiagnosticCode::game_presentation_invalid);
            !rejected) {
            return std::unexpected(std::move(rejected.error()));
        }
        auto position_kind = string_value(
            **position_member, "kind", source, position_pointer,
            DiagnosticCode::game_presentation_invalid);
        if (!position_kind) return std::unexpected(std::move(position_kind.error()));
        auto offset = optional_vec2(
            **position_member, "offset", {}, source, position_pointer,
            DiagnosticCode::game_presentation_invalid);
        if (!offset) return std::unexpected(std::move(offset.error()));
        action.particle_position_offset = *offset;
        if (*position_kind == "constant") {
            action.particle_position_kind = GamePoolSpawnPositionKind::constant;
            auto value = required(
                **position_member, "value", source, position_pointer,
                DiagnosticCode::game_presentation_invalid);
            if (!value) return std::unexpected(std::move(value.error()));
            auto parsed = vec2(
                **value, source, position_pointer + "/value",
                DiagnosticCode::game_presentation_invalid);
            if (!parsed) return std::unexpected(std::move(parsed.error()));
            action.particle_position = *parsed;
            if (optional(**position_member, "target") != nullptr) {
                return std::unexpected(game_error(
                    DiagnosticCode::game_presentation_invalid,
                    "Constant particle position does not accept target", source,
                    position_pointer + "/target"));
            }
        } else if (*position_kind == "target") {
            action.particle_position_kind = GamePoolSpawnPositionKind::target;
            auto target_member = required(
                **position_member, "target", source, position_pointer,
                DiagnosticCode::game_presentation_invalid);
            if (!target_member) return std::unexpected(std::move(target_member.error()));
            auto target = parse_rule_target(
                **target_member, position_pointer + "/target", spawn_groups, spawn_plans,
                collision_context, event_entity_context, source,
                DiagnosticCode::game_presentation_invalid);
            if (!target) return std::unexpected(std::move(target.error()));
            if (target->kind == GameRuleTargetKind::spawn_group) {
                return std::unexpected(game_error(
                    DiagnosticCode::game_presentation_invalid,
                    "Particle position target must identify one entity", source,
                    position_pointer + "/target"));
            }
            action.particle_position_target = *target;
            if (optional(**position_member, "value") != nullptr) {
                return std::unexpected(game_error(
                    DiagnosticCode::game_presentation_invalid,
                    "Target particle position does not accept value", source,
                    position_pointer + "/value"));
            }
        } else {
            return std::unexpected(game_error(
                DiagnosticCode::game_presentation_invalid,
                "Particle position kind must be constant or target", source,
                position_pointer + "/kind"));
        }
    } else {
        return std::unexpected(game_error(
            DiagnosticCode::game_scene_invalid, "Unsupported rule action kind", source,
            std::string{pointer} + "/kind"));
    }
    return action;
}

Result<GameRulePlan> parse_game_rule(
    const Json& object,
    const std::size_t index,
    const std::unordered_map<std::string, std::uint32_t>& actions,
    const std::unordered_map<std::string, std::uint32_t>& states,
    const std::vector<IntStatePlan>& state_plans,
    const std::unordered_map<std::string, std::uint32_t>& spawn_groups,
    const std::vector<GameSpawnGroupPlan>& spawn_plans,
    const std::unordered_map<std::string, std::uint32_t>& pools,
    const std::vector<GamePoolPlan>& pool_plans,
    const std::unordered_map<std::string, std::uint32_t>& grids,
    const std::unordered_map<std::string, std::uint32_t>& systems,
    const std::vector<GameSystemPlan>& system_plans,
    const std::unordered_map<std::string, std::uint32_t>& collision_rules,
    const std::vector<GameCollisionRulePlan>& collision_rule_plans,
    const bool version_0_5,
    const bool version_0_6,
    const std::unordered_map<std::string, std::uint32_t>& assets,
    const std::vector<GameAssetPlan>& asset_plans,
    const std::unordered_map<std::string, std::uint32_t>& animations,
    const std::vector<GameAnimationPlan>& animation_plans,
    const std::unordered_map<std::string, std::uint32_t>& tile_layers,
    const std::vector<GameTileLayerPlan>& tile_layer_plans,
    const std::unordered_map<std::string, std::uint32_t>& fields,
    const std::vector<GameFieldPlan>& field_plans,
    const std::unordered_map<std::string, std::uint32_t>& locales,
    const std::unordered_map<std::string, std::uint32_t>& input_profiles,
    const std::unordered_map<std::string, std::uint32_t>& particle_emitters,
    const std::vector<GameParticleEmitterPlan>& particle_emitter_plans,
    const std::vector<GameGridPlan>& grid_plans,
    const GameSavePlan& save_plan,
    Symbols& symbols,
    const std::string_view source) {
    const auto pointer = "/rules/" + std::to_string(index);
    if (auto rejected = reject_unknown(
            object, {"id", "event", "conditions", "actions"}, source, pointer,
            DiagnosticCode::game_scene_invalid);
        !rejected) {
        return std::unexpected(std::move(rejected.error()));
    }
    auto id = string_value(object, "id", source, pointer, DiagnosticCode::game_scene_invalid);
    if (!id) return std::unexpected(std::move(id.error()));
    auto event_member = required(object, "event", source, pointer, DiagnosticCode::game_scene_invalid);
    if (!event_member) return std::unexpected(std::move(event_member.error()));
    auto event = parse_rule_event(
        **event_member, pointer + "/event", actions, collision_rules, collision_rule_plans,
        version_0_5, version_0_6, animations, source);
    if (!event) return std::unexpected(std::move(event.error()));
    auto action_members = required(object, "actions", source, pointer, DiagnosticCode::game_scene_invalid);
    if (!action_members) return std::unexpected(std::move(action_members.error()));
    if (!(*action_members)->is_array() || (*action_members)->empty() ||
        (*action_members)->size() > GameRulePlan::max_actions) {
        return std::unexpected(game_error(
            DiagnosticCode::game_scene_invalid, "Rule actions must contain one to 16 items", source,
            pointer + "/actions"));
    }
    GameRulePlan plan{};
    plan.symbol = symbols.intern(std::move(*id));
    plan.event = *event;
    if (const auto* conditions = optional(object, "conditions"); conditions != nullptr) {
        if (!conditions->is_array() || conditions->size() > GameRulePlan::max_conditions) {
            return std::unexpected(game_error(
                DiagnosticCode::game_scene_invalid, "Rule conditions must contain at most eight items", source,
                pointer + "/conditions"));
        }
        plan.conditions.reserve(conditions->size());
        for (std::size_t condition_index = 0U; condition_index < conditions->size(); ++condition_index) {
            auto condition = parse_rule_condition(
                (*conditions)[condition_index], pointer + "/conditions/" + std::to_string(condition_index),
                plan.event, states, spawn_groups, spawn_plans, tile_layers, tile_layer_plans,
                fields, field_plans, grid_plans, version_0_6, source);
            if (!condition) return std::unexpected(std::move(condition.error()));
            plan.conditions.push_back(*condition);
        }
    }
    plan.actions.reserve((*action_members)->size());
    for (std::size_t action_index = 0U; action_index < (*action_members)->size(); ++action_index) {
        auto action = parse_rule_action(
            (**action_members)[action_index], pointer + "/actions/" + std::to_string(action_index), plan.event,
            states, state_plans, spawn_groups, spawn_plans, pools, pool_plans, grids, systems, system_plans,
            collision_rule_plans,
            assets, asset_plans, animations, animation_plans, tile_layers, tile_layer_plans,
            fields, field_plans, locales, input_profiles, particle_emitters, particle_emitter_plans,
            grid_plans, save_plan, version_0_6,
            source);
        if (!action) return std::unexpected(std::move(action.error()));
        plan.actions.push_back(std::move(*action));
    }
    return plan;
}

Result<UiElementPlan> parse_ui(
    const Json& object,
    const std::size_t index,
    const std::unordered_map<std::string, std::uint32_t>& actions,
    const std::unordered_map<std::string, std::uint32_t>& assets,
    const std::vector<GameAssetPlan>& asset_plans,
    const bool version_0_6,
    const std::unordered_map<std::string, std::uint32_t>& localization_keys,
    const GameWindowPlan& window_plan,
    Symbols& symbols,
    const std::string_view source) {
    const auto pointer = "/ui/" + std::to_string(index);
    if (auto rejected = reject_unknown(
            object,
            version_0_6
                ? std::initializer_list<std::string_view>{
                      "id", "kind", "position", "size", "text", "text_key", "font", "action", "color",
                      "hover_color", "text_color", "layer", "text_scale", "anchor"}
                : std::initializer_list<std::string_view>{
                      "id", "kind", "position", "size", "text", "font", "action", "color", "hover_color",
                      "text_color", "layer", "text_scale"},
            source,
            pointer,
            DiagnosticCode::game_scene_invalid);
        !rejected) {
        return std::unexpected(std::move(rejected.error()));
    }
    auto id = string_value(object, "id", source, pointer, DiagnosticCode::game_scene_invalid);
    if (!id) return std::unexpected(std::move(id.error()));
    auto kind = string_value(object, "kind", source, pointer, DiagnosticCode::game_scene_invalid);
    if (!kind) return std::unexpected(std::move(kind.error()));
    auto position_member = required(object, "position", source, pointer, DiagnosticCode::game_scene_invalid);
    if (!position_member) return std::unexpected(std::move(position_member.error()));
    auto position = vec2(**position_member, source, pointer + "/position");
    if (!position) return std::unexpected(std::move(position.error()));
    auto size_member = required(object, "size", source, pointer, DiagnosticCode::game_scene_invalid);
    if (!size_member) return std::unexpected(std::move(size_member.error()));
    auto size = vec2(**size_member, source, pointer + "/size");
    if (!size) return std::unexpected(std::move(size.error()));
    if (size->x <= 0.0F || size->y <= 0.0F) {
        return std::unexpected(game_error(
            DiagnosticCode::game_scene_invalid, "UI size must be positive", source, pointer + "/size"));
    }
    UiElementPlan plan{};
    plan.symbol = symbols.intern(std::move(*id));
    plan.position = *position;
    plan.size = *size;
    if (version_0_6 && optional(object, "anchor") != nullptr) {
        auto anchor = string_value(
            object, "anchor", source, pointer, DiagnosticCode::game_presentation_invalid);
        if (!anchor) return std::unexpected(std::move(anchor.error()));
        auto parsed = parse_ui_anchor(*anchor, source, pointer + "/anchor");
        if (!parsed) return std::unexpected(std::move(parsed.error()));
        plan.anchor = *parsed;
        const float width = static_cast<float>(window_plan.virtual_width);
        const float height = static_cast<float>(window_plan.virtual_height);
        const auto base = anchored_ui_center(*parsed, *size, width, height);
        plan.position = {base.x + position->x, base.y + position->y};
        if (!representable_float(plan.position.x) || !representable_float(plan.position.y)) {
            return std::unexpected(game_error(
                DiagnosticCode::game_presentation_invalid, "Anchored UI position exceeds finite float range", source,
                pointer + "/position"));
        }
    }
    if (*kind == "panel") plan.kind = UiElementKind::panel;
    else if (*kind == "text") plan.kind = UiElementKind::text;
    else if (*kind == "button") plan.kind = UiElementKind::button;
    else {
        return std::unexpected(game_error(
            DiagnosticCode::game_scene_invalid, "Unsupported UI element kind", source, pointer + "/kind"));
    }
    auto color = optional_color(object, "color", {}, source, pointer);
    if (!color) return std::unexpected(std::move(color.error()));
    plan.color = *color;
    auto hover_color = optional_color(object, "hover_color", *color, source, pointer);
    if (!hover_color) return std::unexpected(std::move(hover_color.error()));
    plan.hover_color = *hover_color;
    const auto default_text_color = plan.kind == UiElementKind::button ? Color{} : *color;
    auto text_color = optional_color(object, "text_color", default_text_color, source, pointer);
    if (!text_color) return std::unexpected(std::move(text_color.error()));
    plan.text_color = *text_color;
    if (optional(object, "layer") != nullptr) {
        auto layer = i32_value(
            object, "layer", -1'000'000, 1'000'000, source, pointer, DiagnosticCode::game_scene_invalid);
        if (!layer) return std::unexpected(std::move(layer.error()));
        plan.layer = *layer;
    }
    auto text_scale = optional_number(object, "text_scale", 1.0F, source, pointer);
    if (!text_scale || *text_scale <= 0.0F || *text_scale > 16.0F) {
        return text_scale ? std::unexpected(game_error(
                                DiagnosticCode::game_scene_invalid, "UI text scale is outside the supported range",
                                source, pointer + "/text_scale"))
                          : std::unexpected(std::move(text_scale.error()));
    }
    plan.text_scale = *text_scale;
    if (plan.kind == UiElementKind::panel) {
        for (const auto field : {"text", "text_key", "font", "action", "text_color"}) {
            if (optional(object, field) != nullptr) {
                return std::unexpected(game_error(
                    DiagnosticCode::game_scene_invalid, "Panel cannot declare text, font, action, or text_color", source,
                    pointer + "/" + field));
            }
        }
        return plan;
    }
    const bool has_text = optional(object, "text") != nullptr;
    const bool has_text_key = optional(object, "text_key") != nullptr;
    if (!has_text && !has_text_key) {
        return std::unexpected(game_error(
            DiagnosticCode::game_scene_invalid, "Text and button elements require text or text_key", source, pointer));
    }
    if (has_text_key && (!version_0_6 || has_text)) {
        return std::unexpected(game_error(
            DiagnosticCode::game_localization_invalid,
            "text_key requires SceneSpec 0.6 and cannot be combined with text", source, pointer + "/text_key"));
    }
    if (has_text_key) {
        auto text_key = string_value(
            object, "text_key", source, pointer, DiagnosticCode::game_localization_invalid);
        if (!text_key) return std::unexpected(std::move(text_key.error()));
        auto key_index = index_by_name(
            localization_keys, *text_key, "localization key", source, pointer + "/text_key",
            DiagnosticCode::game_localization_invalid);
        if (!key_index) return std::unexpected(std::move(key_index.error()));
        plan.text = symbols.intern(std::move(*text_key));
        plan.localized = true;
        plan.localization_key_index = *key_index;
    } else {
        auto text = string_value(object, "text", source, pointer, DiagnosticCode::game_scene_invalid);
        if (!text) return std::unexpected(std::move(text.error()));
        plan.text = symbols.intern(std::move(*text));
    }
    auto font = string_value(object, "font", source, pointer, DiagnosticCode::game_scene_invalid);
    if (!font) return std::unexpected(std::move(font.error()));
    auto font_index = index_by_name(
        assets, *font, "font asset", source, pointer + "/font", DiagnosticCode::game_scene_invalid);
    if (!font_index) return std::unexpected(std::move(font_index.error()));
    if (asset_plans[*font_index].kind != GameAssetKind::font) {
        return std::unexpected(game_error(
            DiagnosticCode::game_scene_invalid, "UI font must reference a font asset", source, pointer + "/font"));
    }
    plan.font_asset = *font_index;
    if (plan.kind == UiElementKind::button) {
        auto action = string_value(object, "action", source, pointer, DiagnosticCode::game_scene_invalid);
        if (!action) return std::unexpected(std::move(action.error()));
        auto action_index = index_by_name(
            actions, *action, "action", source, pointer + "/action", DiagnosticCode::game_scene_invalid);
        if (!action_index) return std::unexpected(std::move(action_index.error()));
        plan.action = *action_index;
    } else if (optional(object, "action") != nullptr) {
        return std::unexpected(game_error(
            DiagnosticCode::game_scene_invalid, "Text element cannot declare an action", source, pointer + "/action"));
    }
    return plan;
}

Result<GameScenePlan> parse_scene(
    const Json& document,
    const std::filesystem::path& source_path,
    const std::string_view expected_id,
    const GameSchemaVersion expected_version,
    const std::unordered_map<std::string, std::uint32_t>& actions,
    const std::unordered_map<std::string, std::uint32_t>& states,
    const std::vector<IntStatePlan>& state_plans,
    const std::unordered_map<std::string, std::uint32_t>& assets,
    const std::vector<GameAssetPlan>& asset_plans,
    const std::unordered_map<std::string, std::uint32_t>& animations,
    const std::vector<GameAnimationPlan>& animation_plans,
    const std::unordered_map<std::string, std::uint32_t>& prefabs,
    const std::vector<PrefabSource>& prefab_sources,
    const std::unordered_map<std::string, std::uint32_t>& localization_keys,
    const std::unordered_map<std::string, std::uint32_t>& locales,
    const std::unordered_map<std::string, std::uint32_t>& input_profiles,
    const GameSavePlan& save_plan,
    const GameWindowPlan& window_plan,
    Symbols& symbols) {
    const auto source = source_path.string();
    const bool version_0_3 = expected_version != GameSchemaVersion::v0_2;
    const bool version_0_4 = expected_version == GameSchemaVersion::v0_4 ||
                             expected_version == GameSchemaVersion::v0_5 ||
                             expected_version == GameSchemaVersion::v0_6;
    const bool version_0_5 = expected_version == GameSchemaVersion::v0_5 ||
                             expected_version == GameSchemaVersion::v0_6;
    const bool version_0_6 = expected_version == GameSchemaVersion::v0_6;
    if (auto rejected = reject_unknown(
            document,
            version_0_6
                ? std::initializer_list<std::string_view>{
                      "schema_version", "id", "world", "camera", "collision", "grids", "spawn_groups", "pools",
                      "systems", "collision_rules", "rules", "ui", "ui_stacks", "tile_layers", "fields",
                      "particle_emitters", "persistent"}
                : version_0_4
                ? std::initializer_list<std::string_view>{
                      "schema_version", "id", "world", "camera", "collision", "grids", "spawn_groups", "pools",
                      "systems", "collision_rules", "rules", "ui"}
                : version_0_3
                ? std::initializer_list<std::string_view>{
                      "schema_version", "id", "world", "camera", "collision", "grids", "spawn_groups", "systems",
                      "collision_rules", "rules", "ui"}
                : std::initializer_list<std::string_view>{
                      "schema_version", "id", "world", "camera", "collision", "spawn_groups", "systems",
                      "collision_rules", "ui"},
            source,
            "/",
            DiagnosticCode::game_scene_invalid);
        !rejected) {
        return std::unexpected(std::move(rejected.error()));
    }
    auto version = string_value(document, "schema_version", source, "/", DiagnosticCode::game_scene_invalid);
    if (!version) return std::unexpected(std::move(version.error()));
    auto parsed_version = parse_game_schema_version(
        *version, source, "/schema_version", DiagnosticCode::game_scene_invalid);
    if (!parsed_version) return std::unexpected(std::move(parsed_version.error()));
    if (*parsed_version != expected_version) {
        return std::unexpected(game_error(
            DiagnosticCode::game_scene_invalid,
            "SceneSpec version must match its GameManifest version", source, "/schema_version"));
    }
    auto id = string_value(document, "id", source, "/", DiagnosticCode::game_scene_invalid);
    if (!id) return std::unexpected(std::move(id.error()));
    if (*id != expected_id) {
        return std::unexpected(game_error(
            DiagnosticCode::game_scene_invalid, "Scene id does not match the manifest declaration", source, "/id"));
    }
    GameScenePlan plan{};
    plan.symbol = symbols.intern(std::move(*id));
    plan.source_path = source_path;
    auto world = required(document, "world", source, "/", DiagnosticCode::game_scene_invalid);
    if (!world) return std::unexpected(std::move(world.error()));
    if (auto rejected = reject_unknown(**world, {"capacity"}, source, "/world", DiagnosticCode::game_scene_invalid);
        !rejected) {
        return std::unexpected(std::move(rejected.error()));
    }
    auto capacity = u32_value(
        **world, "capacity", 1U, GameScenePlan::max_world_capacity, source, "/world",
        DiagnosticCode::game_scene_invalid);
    if (!capacity) return std::unexpected(std::move(capacity.error()));
    plan.world_capacity = *capacity;
    auto camera = required(document, "camera", source, "/", DiagnosticCode::game_scene_invalid);
    if (!camera) return std::unexpected(std::move(camera.error()));
    if (auto rejected = reject_unknown(
            **camera,
            version_0_6
                ? std::initializer_list<std::string_view>{
                      "mode", "position", "half_extent", "target", "offset", "bounds", "pixel_snap"}
                : std::initializer_list<std::string_view>{"position", "half_extent"},
            source, "/camera", DiagnosticCode::game_scene_invalid);
        !rejected) {
        return std::unexpected(std::move(rejected.error()));
    }
    auto camera_position = optional_vec2(**camera, "position", {}, source, "/camera");
    if (!camera_position) return std::unexpected(std::move(camera_position.error()));
    auto camera_half = optional_vec2(**camera, "half_extent", {16.0F, 9.0F}, source, "/camera");
    if (!camera_half) return std::unexpected(std::move(camera_half.error()));
    if (camera_half->x <= 0.0F || camera_half->y <= 0.0F) {
        return std::unexpected(game_error(
            DiagnosticCode::game_scene_invalid, "Camera half extent must be positive", source,
            "/camera/half_extent"));
    }
    plan.camera_position = *camera_position;
    plan.camera_half_extent = *camera_half;
    plan.camera.position = *camera_position;
    plan.camera.half_extent = *camera_half;
    if (version_0_6) {
        const auto* mode_member = optional(**camera, "mode");
        const auto mode = mode_member == nullptr ? std::string{"fixed"}
                                                 : mode_member->is_string() ? mode_member->get<std::string>() : std::string{};
        if (mode == "fixed") plan.camera.mode = GameCameraMode::fixed;
        else if (mode == "follow") plan.camera.mode = GameCameraMode::follow;
        else {
            return std::unexpected(game_error(
                DiagnosticCode::game_presentation_invalid, "Camera mode must be fixed or follow", source,
                "/camera/mode"));
        }
        auto offset = optional_vec2(
            **camera, "offset", {}, source, "/camera", DiagnosticCode::game_presentation_invalid);
        if (!offset) return std::unexpected(std::move(offset.error()));
        plan.camera.follow_offset = *offset;
        auto pixel_snap = optional_bool(
            **camera, "pixel_snap", false, source, "/camera", DiagnosticCode::game_presentation_invalid);
        if (!pixel_snap) return std::unexpected(std::move(pixel_snap.error()));
        plan.camera.pixel_snap = *pixel_snap;
        if (const auto* bounds = optional(**camera, "bounds"); bounds != nullptr) {
            auto parsed = rect4(*bounds, source, "/camera/bounds", DiagnosticCode::game_presentation_invalid);
            if (!parsed) return std::unexpected(std::move(parsed.error()));
            if (parsed->max.x <= parsed->min.x || parsed->max.y <= parsed->min.y) {
                return std::unexpected(game_error(
                    DiagnosticCode::game_presentation_invalid, "Camera bounds are invalid", source,
                    "/camera/bounds"));
            }
            plan.camera.bounds = *parsed;
            plan.camera.has_bounds = true;
        }
    }

    if (const auto* collision = optional(document, "collision"); collision != nullptr) {
        if (auto rejected = reject_unknown(
                *collision,
                version_0_5
                    ? std::initializer_list<std::string_view>{
                          "bounds", "cell_size", "max_colliders", "max_grid_references", "max_candidate_pairs",
                          "max_contact_pairs", "max_impacts"}
                    : std::initializer_list<std::string_view>{
                          "bounds", "cell_size", "max_colliders", "max_grid_references", "max_candidate_pairs",
                          "max_impacts"},
                source,
                "/collision",
                DiagnosticCode::game_scene_invalid);
            !rejected) {
            return std::unexpected(std::move(rejected.error()));
        }
        if (const auto* bounds = optional(*collision, "bounds"); bounds != nullptr) {
            auto parsed = rect4(*bounds, source, "/collision/bounds");
            if (!parsed) return std::unexpected(std::move(parsed.error()));
            plan.collision_bounds = *parsed;
        }
        auto cell_size = optional_vec2(*collision, "cell_size", {1.0F, 1.0F}, source, "/collision");
        if (!cell_size) return std::unexpected(std::move(cell_size.error()));
        plan.collision_cell_size = *cell_size;
        const auto optional_capacity = [&](const std::string_view key, const std::uint32_t fallback,
                                           const std::uint32_t maximum) -> Result<std::uint32_t> {
            return optional(*collision, key) == nullptr
                       ? Result<std::uint32_t>{fallback}
                       : u32_value(*collision, key, 1U, maximum, source, "/collision",
                                   DiagnosticCode::game_scene_invalid);
        };
        auto max_colliders = optional_capacity(
            "max_colliders", 10'000U, GameScenePlan::max_world_capacity);
        if (!max_colliders) return std::unexpected(std::move(max_colliders.error()));
        auto max_references = optional_capacity(
            "max_grid_references", 80'000U, GameScenePlan::max_collision_capacity);
        if (!max_references) return std::unexpected(std::move(max_references.error()));
        auto max_candidates = optional_capacity(
            "max_candidate_pairs", 80'000U, GameScenePlan::max_collision_capacity);
        if (!max_candidates) return std::unexpected(std::move(max_candidates.error()));
        auto max_impacts = optional_capacity(
            "max_impacts", 4U, GameScenePlan::max_impacts_per_dynamic);
        if (!max_impacts) return std::unexpected(std::move(max_impacts.error()));
        plan.max_colliders = *max_colliders;
        plan.max_grid_references = *max_references;
        plan.max_candidate_pairs = *max_candidates;
        if (version_0_5 && optional(*collision, "max_contact_pairs") != nullptr) {
            auto max_contacts = u32_value(
                *collision, "max_contact_pairs", 1U, 100'000U, source, "/collision",
                DiagnosticCode::game_collision_interaction_invalid);
            if (!max_contacts) return std::unexpected(std::move(max_contacts.error()));
            if (*max_contacts > *max_candidates) {
                return std::unexpected(game_error(
                    DiagnosticCode::game_collision_interaction_invalid,
                    "max_contact_pairs must not exceed max_candidate_pairs", source,
                    "/collision/max_contact_pairs"));
            }
            plan.max_contact_pairs = *max_contacts;
        }
        plan.max_impacts = *max_impacts;
    }
    if (plan.collision_bounds.max.x <= plan.collision_bounds.min.x ||
        plan.collision_bounds.max.y <= plan.collision_bounds.min.y || plan.collision_cell_size.x <= 0.0F ||
        plan.collision_cell_size.y <= 0.0F) {
        return std::unexpected(game_error(
            DiagnosticCode::game_scene_invalid, "Collision bounds or cell size is invalid", source, "/collision"));
    }

    std::unordered_map<std::string, std::uint32_t> grid_indices{};
    if (version_0_3) {
        auto grids = required(document, "grids", source, "/", DiagnosticCode::game_scene_invalid);
        if (!grids) return std::unexpected(std::move(grids.error()));
        if (!(*grids)->is_array() || (*grids)->size() > GameScenePlan::max_grids) {
            return std::unexpected(game_error(
                DiagnosticCode::game_scene_invalid, "grids must be a bounded array", source, "/grids"));
        }
        plan.grids.reserve((*grids)->size());
        for (std::size_t index = 0U; index < (*grids)->size(); ++index) {
            auto grid = parse_grid((**grids)[index], index, symbols, source);
            if (!grid) return std::unexpected(std::move(grid.error()));
            const auto name = std::string{symbols.view(grid->symbol)};
            if (!grid_indices.emplace(name, static_cast<std::uint32_t>(plan.grids.size())).second) {
                return std::unexpected(game_error(
                    DiagnosticCode::game_scene_invalid, "Scene contains duplicate grid ids", source,
                    "/grids/" + std::to_string(index) + "/id"));
            }
            plan.grids.push_back(*grid);
        }
    }

    std::unordered_map<std::string, std::uint32_t> tile_layer_indices{};
    std::unordered_map<std::string, std::uint32_t> field_indices{};
    std::unordered_map<std::string, std::uint32_t> particle_emitter_indices{};
    if (version_0_6) {
        std::uint64_t aggregate_cells = 0U;
        if (const auto* layers = optional(document, "tile_layers"); layers != nullptr) {
            if (!layers->is_array() || layers->size() > 64U) {
                return std::unexpected(game_error(
                    DiagnosticCode::game_tile_field_invalid, "tile_layers must contain at most 64 items", source,
                    "/tile_layers"));
            }
            plan.tile_layers.reserve(layers->size());
            for (std::size_t index = 0U; index < layers->size(); ++index) {
                auto layer = parse_tile_layer(
                    (*layers)[index], index, grid_indices, plan.grids, assets, asset_plans, symbols, source);
                if (!layer) return std::unexpected(std::move(layer.error()));
                const auto name = std::string{symbols.view(layer->symbol)};
                if (!tile_layer_indices.emplace(name, static_cast<std::uint32_t>(plan.tile_layers.size())).second) {
                    return std::unexpected(game_error(
                        DiagnosticCode::game_tile_field_invalid, "Scene contains duplicate tile layer ids", source,
                        "/tile_layers/" + std::to_string(index) + "/id"));
                }
                aggregate_cells += layer->initial_cells.size();
                if (aggregate_cells > 4'000'000U) {
                    return std::unexpected(game_error(
                        DiagnosticCode::game_tile_field_invalid,
                        "Tile and field cells exceed the four-million scene limit", source, "/tile_layers"));
                }
                plan.tile_layers.push_back(std::move(*layer));
            }
        }
        if (const auto* fields = optional(document, "fields"); fields != nullptr) {
            if (!fields->is_array() || fields->size() > 64U) {
                return std::unexpected(game_error(
                    DiagnosticCode::game_tile_field_invalid, "fields must contain at most 64 items", source,
                    "/fields"));
            }
            plan.fields.reserve(fields->size());
            for (std::size_t index = 0U; index < fields->size(); ++index) {
                auto field = parse_field((*fields)[index], index, grid_indices, plan.grids, symbols, source);
                if (!field) return std::unexpected(std::move(field.error()));
                const auto name = std::string{symbols.view(field->symbol)};
                if (!field_indices.emplace(name, static_cast<std::uint32_t>(plan.fields.size())).second) {
                    return std::unexpected(game_error(
                        DiagnosticCode::game_tile_field_invalid, "Scene contains duplicate field ids", source,
                        "/fields/" + std::to_string(index) + "/id"));
                }
                aggregate_cells += field->initial_cells.size();
                if (aggregate_cells > 4'000'000U) {
                    return std::unexpected(game_error(
                        DiagnosticCode::game_tile_field_invalid,
                        "Tile and field cells exceed the four-million scene limit", source, "/fields"));
                }
                plan.fields.push_back(std::move(*field));
            }
        }
        if (const auto* emitters = optional(document, "particle_emitters"); emitters != nullptr) {
            if (!emitters->is_array() || emitters->size() > 64U) {
                return std::unexpected(game_error(
                    DiagnosticCode::game_presentation_invalid,
                    "particle_emitters must contain at most 64 items", source, "/particle_emitters"));
            }
            std::uint32_t aggregate_particles = 0U;
            plan.particle_emitters.reserve(emitters->size());
            for (std::size_t index = 0U; index < emitters->size(); ++index) {
                auto emitter = parse_particle_emitter(
                    (*emitters)[index], index, assets, asset_plans, symbols, source);
                if (!emitter) return std::unexpected(std::move(emitter.error()));
                if (aggregate_particles > 100'000U - emitter->capacity) {
                    return std::unexpected(game_error(
                        DiagnosticCode::game_presentation_invalid,
                        "Particle capacities exceed the one-hundred-thousand scene limit", source,
                        "/particle_emitters"));
                }
                aggregate_particles += emitter->capacity;
                const auto name = std::string{symbols.view(emitter->symbol)};
                if (!particle_emitter_indices.emplace(
                        name, static_cast<std::uint32_t>(plan.particle_emitters.size())).second) {
                    return std::unexpected(game_error(
                        DiagnosticCode::game_presentation_invalid,
                        "Scene contains duplicate particle emitter ids", source,
                        "/particle_emitters/" + std::to_string(index) + "/id"));
                }
                plan.particle_emitters.push_back(*emitter);
            }
        }
    }

    auto spawns = required(document, "spawn_groups", source, "/", DiagnosticCode::game_scene_invalid);
    if (!spawns) return std::unexpected(std::move(spawns.error()));
    if (!(*spawns)->is_array() || (*spawns)->size() > GameScenePlan::max_spawn_groups) {
        return std::unexpected(game_error(
            DiagnosticCode::game_scene_invalid, "spawn_groups must be a bounded array", source, "/spawn_groups"));
    }
    std::unordered_set<SymbolId> group_ids{};
    std::unordered_map<std::string, std::uint32_t> spawn_group_indices{};
    plan.spawn_groups.reserve((*spawns)->size());
    for (std::size_t index = 0U; index < (*spawns)->size(); ++index) {
        Json expanded_spawn = (**spawns)[index];
        if (version_0_6 && optional(expanded_spawn, "prefab") != nullptr) {
            auto prefab = string_value(
                expanded_spawn, "prefab", source, "/spawn_groups/" + std::to_string(index),
                DiagnosticCode::game_prefab_invalid);
            if (!prefab) return std::unexpected(std::move(prefab.error()));
            auto prefab_index = index_by_name(
                prefabs, *prefab, "prefab", source, "/spawn_groups/" + std::to_string(index) + "/prefab",
                DiagnosticCode::game_prefab_invalid);
            if (!prefab_index) return std::unexpected(std::move(prefab_index.error()));
            const auto& prefab_document = prefab_sources[*prefab_index].document;
            for (const auto field : {"transform", "velocity", "sprite", "collider", "animation"}) {
                if (optional(expanded_spawn, field) == nullptr && optional(prefab_document, field) != nullptr) {
                    expanded_spawn[field] = *optional(prefab_document, field);
                }
            }
            expanded_spawn.erase("prefab");
        }
        auto spawn = parse_spawn_group(
            expanded_spawn, index, version_0_3, version_0_5, version_0_6, assets, asset_plans,
            animations, animation_plans, symbols, source);
        if (!spawn) return std::unexpected(std::move(spawn.error()));
        if (!spawn_positions_representable(*spawn)) {
            return std::unexpected(game_error(
                DiagnosticCode::game_scene_invalid, "Derived spawn positions exceed the finite float range", source,
                "/spawn_groups/" + std::to_string(index) + "/placement"));
        }
        if (!group_ids.insert(spawn->symbol).second) {
            return std::unexpected(game_error(
                DiagnosticCode::game_scene_invalid, "Scene contains duplicate spawn group ids", source,
                "/spawn_groups/" + std::to_string(index) + "/id"));
        }
        if (plan.total_spawn_count > plan.world_capacity - spawn->count) {
            return std::unexpected(game_error(
                DiagnosticCode::game_scene_invalid, "Scene spawn count exceeds world capacity", source,
                "/spawn_groups/" + std::to_string(index) + "/count"));
        }
        plan.total_spawn_count += spawn->count;
        spawn_group_indices.emplace(
            std::string{symbols.view(spawn->symbol)}, static_cast<std::uint32_t>(plan.spawn_groups.size()));
        plan.spawn_groups.push_back(*spawn);
    }
    if (version_0_6) {
        auto persistent = optional_bool(
            document, "persistent", false, source, "/", DiagnosticCode::game_save_invalid);
        if (!persistent) return std::unexpected(std::move(persistent.error()));
        plan.persistent = *persistent;
        if (plan.camera.mode == GameCameraMode::follow) {
            auto target = required(**camera, "target", source, "/camera", DiagnosticCode::game_presentation_invalid);
            if (!target) return std::unexpected(std::move(target.error()));
            if (auto rejected = reject_unknown(
                    **target, {"group", "index"}, source, "/camera/target",
                    DiagnosticCode::game_presentation_invalid);
                !rejected) {
                return std::unexpected(std::move(rejected.error()));
            }
            auto group = string_value(
                **target, "group", source, "/camera/target", DiagnosticCode::game_presentation_invalid);
            if (!group) return std::unexpected(std::move(group.error()));
            auto group_index = index_by_name(
                spawn_group_indices, *group, "spawn group", source, "/camera/target/group",
                DiagnosticCode::game_presentation_invalid);
            if (!group_index) return std::unexpected(std::move(group_index.error()));
            auto item = u32_value(
                **target, "index", 0U, plan.spawn_groups[*group_index].count - 1U, source,
                "/camera/target", DiagnosticCode::game_presentation_invalid);
            if (!item) return std::unexpected(std::move(item.error()));
            plan.camera.follow_group_index = *group_index;
            plan.camera.follow_item_index = *item;
        } else if (optional(**camera, "target") != nullptr) {
            return std::unexpected(game_error(
                DiagnosticCode::game_presentation_invalid, "Fixed camera cannot declare a target", source,
                "/camera/target"));
        }
    }

    std::unordered_map<std::string, std::uint32_t> pool_indices{};
    if (version_0_4) {
        auto pools = required(document, "pools", source, "/", DiagnosticCode::game_pool_invalid);
        if (!pools) return std::unexpected(std::move(pools.error()));
        if (!(*pools)->is_array() || (*pools)->size() > GameScenePlan::max_pools) {
            return std::unexpected(game_error(
                DiagnosticCode::game_pool_invalid, "pools must contain at most 1024 items", source, "/pools"));
        }
        std::unordered_set<std::uint32_t> owned_groups{};
        plan.pools.reserve((*pools)->size());
        for (std::size_t index = 0U; index < (*pools)->size(); ++index) {
            auto pool = parse_pool((**pools)[index], index, spawn_group_indices, symbols, source);
            if (!pool) return std::unexpected(std::move(pool.error()));
            const auto name = std::string{symbols.view(pool->symbol)};
            if (!pool_indices.emplace(name, static_cast<std::uint32_t>(plan.pools.size())).second) {
                return std::unexpected(game_error(
                    DiagnosticCode::game_pool_invalid, "Scene contains duplicate pool ids", source,
                    "/pools/" + std::to_string(index) + "/id"));
            }
            if (!owned_groups.insert(pool->spawn_group_index).second) {
                return std::unexpected(game_error(
                    DiagnosticCode::game_pool_invalid, "A spawn group may belong to only one pool", source,
                    "/pools/" + std::to_string(index) + "/group"));
            }
            plan.pools.push_back(*pool);
        }
    }

    auto systems = required(document, "systems", source, "/", DiagnosticCode::game_scene_invalid);
    if (!systems) return std::unexpected(std::move(systems.error()));
    if (!(*systems)->is_array() || (*systems)->size() > GameScenePlan::max_systems) {
        return std::unexpected(game_error(
            DiagnosticCode::game_scene_invalid, "systems must be a bounded array", source, "/systems"));
    }
    std::unordered_set<SymbolId> system_ids{};
    std::unordered_map<std::string, std::uint32_t> system_indices{};
    plan.systems.reserve((*systems)->size());
    for (std::size_t index = 0U; index < (*systems)->size(); ++index) {
        auto system = parse_system(
            (**systems)[index], index, version_0_3, version_0_5, actions, spawn_group_indices, grid_indices, system_indices,
            plan.systems, symbols, source);
        if (!system) return std::unexpected(std::move(system.error()));
        if (!system_ids.insert(system->symbol).second) {
            return std::unexpected(game_error(
                DiagnosticCode::game_scene_invalid, "Scene contains duplicate system ids", source,
                "/systems/" + std::to_string(index) + "/id"));
        }
        system_indices.emplace(
            std::string{symbols.view(system->symbol)}, static_cast<std::uint32_t>(plan.systems.size()));
        plan.systems.push_back(*system);
    }

    std::unordered_map<std::string, std::uint32_t> collision_rule_indices{};
    if (const auto* collision_rules = optional(document, "collision_rules"); collision_rules != nullptr) {
        if (!collision_rules->is_array() || collision_rules->size() > GameScenePlan::max_collision_rules) {
            return std::unexpected(game_error(
                DiagnosticCode::game_scene_invalid, "collision_rules must be a bounded array", source,
                "/collision_rules"));
        }
        plan.collision_rules.reserve(collision_rules->size());
        for (std::size_t index = 0U; index < collision_rules->size(); ++index) {
            auto rule = parse_collision_rule(
                (*collision_rules)[index], index, version_0_3, version_0_5, states, assets, asset_plans, symbols, source);
            if (!rule) return std::unexpected(std::move(rule.error()));
            if (version_0_3) {
                const auto name = std::string{symbols.view(rule->symbol)};
                if (!collision_rule_indices.emplace(
                        name, static_cast<std::uint32_t>(plan.collision_rules.size())).second) {
                    return std::unexpected(game_error(
                        DiagnosticCode::game_scene_invalid, "Scene contains duplicate collision rule ids", source,
                        "/collision_rules/" + std::to_string(index) + "/id"));
                }
            }
            plan.collision_rules.push_back(std::move(*rule));
        }
    }
    if (version_0_3) {
        if (const auto* rules = optional(document, "rules"); rules != nullptr) {
            if (!rules->is_array() || rules->size() > GameScenePlan::max_rules) {
                return std::unexpected(game_error(
                    DiagnosticCode::game_scene_invalid, "rules must be a bounded array", source, "/rules"));
            }
            std::unordered_set<SymbolId> rule_ids{};
            plan.rules.reserve(rules->size());
            for (std::size_t index = 0U; index < rules->size(); ++index) {
                auto rule = parse_game_rule(
                    (*rules)[index], index, actions, states, state_plans, spawn_group_indices, plan.spawn_groups,
                    pool_indices, plan.pools, grid_indices, system_indices, plan.systems,
                    collision_rule_indices, plan.collision_rules,
                    version_0_5, version_0_6,
                    assets, asset_plans, animations, animation_plans,
                    tile_layer_indices, plan.tile_layers, field_indices, plan.fields,
                    locales, input_profiles, particle_emitter_indices, plan.particle_emitters,
                    plan.grids, save_plan,
                    symbols, source);
                if (!rule) return std::unexpected(std::move(rule.error()));
                if (!rule_ids.insert(rule->symbol).second) {
                    return std::unexpected(game_error(
                        DiagnosticCode::game_scene_invalid, "Scene contains duplicate rule ids", source,
                        "/rules/" + std::to_string(index) + "/id"));
                }
                plan.rules.push_back(std::move(*rule));
            }
        }
    }
    if (version_0_6 && optional(document, "ui_stacks") != nullptr && optional(document, "ui") == nullptr) {
        return std::unexpected(game_error(
            DiagnosticCode::game_presentation_invalid,
            "ui_stacks requires a ui array", source, "/ui_stacks"));
    }
    if (const auto* ui = optional(document, "ui"); ui != nullptr) {
        if (!ui->is_array() || ui->size() > GameScenePlan::max_ui_elements) {
            return std::unexpected(game_error(
                DiagnosticCode::game_scene_invalid, "ui must be a bounded array", source, "/ui"));
        }
        std::unordered_set<SymbolId> ui_ids{};
        std::unordered_map<std::string, std::uint32_t> ui_indices{};
        plan.ui.reserve(ui->size());
        for (std::size_t index = 0U; index < ui->size(); ++index) {
            auto element = parse_ui(
                (*ui)[index], index, actions, assets, asset_plans, version_0_6, localization_keys, window_plan,
                symbols, source);
            if (!element) return std::unexpected(std::move(element.error()));
            if (!ui_ids.insert(element->symbol).second) {
                return std::unexpected(game_error(
                    DiagnosticCode::game_scene_invalid, "Scene contains duplicate UI ids", source,
                    "/ui/" + std::to_string(index) + "/id"));
            }
            ui_indices.emplace(std::string{symbols.view(element->symbol)}, static_cast<std::uint32_t>(plan.ui.size()));
            plan.ui.push_back(*element);
        }
        if (version_0_6) {
            if (const auto* stacks = optional(document, "ui_stacks"); stacks != nullptr) {
                if (!stacks->is_array() || stacks->size() > 128U) {
                    return std::unexpected(game_error(
                        DiagnosticCode::game_presentation_invalid, "ui_stacks must contain at most 128 items", source,
                        "/ui_stacks"));
                }
                std::unordered_set<std::uint32_t> owned_children{};
                std::unordered_set<SymbolId> stack_ids{};
                plan.ui_stacks.reserve(stacks->size());
                for (std::size_t stack_index = 0U; stack_index < stacks->size(); ++stack_index) {
                    const auto& stack = (*stacks)[stack_index];
                    const auto stack_pointer = "/ui_stacks/" + std::to_string(stack_index);
                    if (auto rejected = reject_unknown(
                            stack, {"id", "direction", "position", "size", "padding", "spacing", "anchor", "children"},
                            source, stack_pointer, DiagnosticCode::game_presentation_invalid);
                        !rejected) {
                        return std::unexpected(std::move(rejected.error()));
                    }
                    auto stack_id = string_value(
                        stack, "id", source, stack_pointer, DiagnosticCode::game_presentation_invalid);
                    if (!stack_id) return std::unexpected(std::move(stack_id.error()));
                    GameUiStackPlan stack_plan{};
                    stack_plan.symbol = symbols.intern(std::move(*stack_id));
                    if (!stack_ids.insert(stack_plan.symbol).second) {
                        return std::unexpected(game_error(
                            DiagnosticCode::game_presentation_invalid, "Duplicate UI stack id", source,
                            stack_pointer + "/id"));
                    }
                    auto direction = string_value(
                        stack, "direction", source, stack_pointer, DiagnosticCode::game_presentation_invalid);
                    if (!direction) return std::unexpected(std::move(direction.error()));
                    if (*direction == "horizontal") stack_plan.direction = GameStackDirection::horizontal;
                    else if (*direction == "vertical") stack_plan.direction = GameStackDirection::vertical;
                    else {
                        return std::unexpected(game_error(
                            DiagnosticCode::game_presentation_invalid,
                            "UI stack direction must be horizontal or vertical", source,
                            stack_pointer + "/direction"));
                    }
                    auto position_member = required(
                        stack, "position", source, stack_pointer, DiagnosticCode::game_presentation_invalid);
                    if (!position_member) return std::unexpected(std::move(position_member.error()));
                    auto position = vec2(
                        **position_member, source, stack_pointer + "/position",
                        DiagnosticCode::game_presentation_invalid);
                    if (!position) return std::unexpected(std::move(position.error()));
                    auto size_member = required(
                        stack, "size", source, stack_pointer, DiagnosticCode::game_presentation_invalid);
                    if (!size_member) return std::unexpected(std::move(size_member.error()));
                    auto size = vec2(
                        **size_member, source, stack_pointer + "/size", DiagnosticCode::game_presentation_invalid);
                    if (!size) return std::unexpected(std::move(size.error()));
                    if (size->x <= 0.0F || size->y <= 0.0F) {
                        return std::unexpected(game_error(
                            DiagnosticCode::game_presentation_invalid, "UI stack size must be positive", source,
                            stack_pointer + "/size"));
                    }
                    auto padding = optional_vec2(
                        stack, "padding", {}, source, stack_pointer, DiagnosticCode::game_presentation_invalid);
                    if (!padding) return std::unexpected(std::move(padding.error()));
                    if (padding->x < 0.0F || padding->y < 0.0F) {
                        return std::unexpected(game_error(
                            DiagnosticCode::game_presentation_invalid, "UI stack padding cannot be negative", source,
                            stack_pointer + "/padding"));
                    }
                    auto spacing = optional_number(
                        stack, "spacing", 0.0F, source, stack_pointer,
                        DiagnosticCode::game_presentation_invalid);
                    if (!spacing) return std::unexpected(std::move(spacing.error()));
                    if (*spacing < 0.0F) {
                        return std::unexpected(game_error(
                            DiagnosticCode::game_presentation_invalid, "UI stack spacing cannot be negative", source,
                            stack_pointer + "/spacing"));
                    }
                    GameUiAnchor anchor = GameUiAnchor::top_left;
                    if (optional(stack, "anchor") != nullptr) {
                        auto anchor_name = string_value(
                            stack, "anchor", source, stack_pointer, DiagnosticCode::game_presentation_invalid);
                        if (!anchor_name) return std::unexpected(std::move(anchor_name.error()));
                        auto parsed_anchor = parse_ui_anchor(*anchor_name, source, stack_pointer + "/anchor");
                        if (!parsed_anchor) return std::unexpected(std::move(parsed_anchor.error()));
                        anchor = *parsed_anchor;
                    }
                    stack_plan.position = *position;
                    stack_plan.size = *size;
                    stack_plan.padding = *padding;
                    stack_plan.spacing = *spacing;
                    stack_plan.anchor = anchor;
                    const float window_width = static_cast<float>(window_plan.virtual_width);
                    const float window_height = static_cast<float>(window_plan.virtual_height);
                    const auto center = anchored_ui_center(anchor, *size, window_width, window_height);
                    const Vec2 container_center{center.x + position->x, center.y + position->y};
                    const Vec2 content_min{
                        container_center.x - size->x * 0.5F + padding->x,
                        container_center.y - size->y * 0.5F + padding->y,
                    };
                    const Vec2 content_max{
                        container_center.x + size->x * 0.5F - padding->x,
                        container_center.y + size->y * 0.5F - padding->y,
                    };
                    if (content_min.x > content_max.x || content_min.y > content_max.y) {
                        return std::unexpected(game_error(
                            DiagnosticCode::game_presentation_invalid,
                            "UI stack padding exceeds its content bounds", source, stack_pointer));
                    }
                    auto children = required(
                        stack, "children", source, stack_pointer, DiagnosticCode::game_presentation_invalid);
                    if (!children) return std::unexpected(std::move(children.error()));
                    if (!(*children)->is_array() || (*children)->empty() || (*children)->size() > 128U) {
                        return std::unexpected(game_error(
                            DiagnosticCode::game_presentation_invalid,
                            "UI stack children must be a non-empty bounded array", source,
                            stack_pointer + "/children"));
                    }
                    Vec2 cursor = content_min;
                    for (std::size_t child_index = 0U; child_index < (*children)->size(); ++child_index) {
                        if (!(**children)[child_index].is_string()) {
                            return std::unexpected(game_error(
                                DiagnosticCode::game_presentation_invalid, "UI stack child must be an id string", source,
                                stack_pointer + "/children/" + std::to_string(child_index)));
                        }
                        const auto& child_name = (**children)[child_index].get_ref<const std::string&>();
                        auto child = index_by_name(
                            ui_indices, child_name, "UI element", source,
                            stack_pointer + "/children/" + std::to_string(child_index),
                            DiagnosticCode::game_presentation_invalid);
                        if (!child) return std::unexpected(std::move(child.error()));
                        if (!owned_children.insert(*child).second) {
                            return std::unexpected(game_error(
                                DiagnosticCode::game_presentation_invalid,
                                "A UI element may belong to only one stack", source,
                                stack_pointer + "/children/" + std::to_string(child_index)));
                        }
                        auto& element = plan.ui[*child];
                        element.position = stack_plan.direction == GameStackDirection::horizontal
                                               ? Vec2{cursor.x + element.size.x * 0.5F,
                                                      content_min.y + element.size.y * 0.5F}
                                               : Vec2{content_min.x + element.size.x * 0.5F,
                                                      cursor.y + element.size.y * 0.5F};
                        stack_plan.child_indices.push_back(*child);
                        const Vec2 child_min{
                            element.position.x - element.size.x * 0.5F,
                            element.position.y - element.size.y * 0.5F,
                        };
                        const Vec2 child_max{
                            element.position.x + element.size.x * 0.5F,
                            element.position.y + element.size.y * 0.5F,
                        };
                        if (child_min.x < content_min.x - 0.0001F ||
                            child_min.y < content_min.y - 0.0001F ||
                            child_max.x > content_max.x + 0.0001F ||
                            child_max.y > content_max.y + 0.0001F) {
                            return std::unexpected(game_error(
                                DiagnosticCode::game_presentation_invalid,
                                "UI stack child rectangle exceeds its content bounds", source, stack_pointer));
                        }
                        if (stack_plan.direction == GameStackDirection::horizontal) {
                            cursor.x += element.size.x + *spacing;
                        } else {
                            cursor.y += element.size.y + *spacing;
                        }
                    }
                    plan.ui_stacks.push_back(std::move(stack_plan));
                }
            }
        }
    }

    std::unordered_set<SymbolId> collider_groups{};
    std::unordered_map<SymbolId, GameBodyMotion> group_motions{};
    std::unordered_map<SymbolId, bool> groups_all_have_velocity{};
    std::vector<std::int32_t> pool_for_spawn_group(plan.spawn_groups.size(), -1);
    for (std::size_t pool_index = 0U; pool_index < plan.pools.size(); ++pool_index) {
        pool_for_spawn_group[plan.pools[pool_index].spawn_group_index] = static_cast<std::int32_t>(pool_index);
    }
    const auto collider_group_contains_pool = [&](const SymbolId collider_group) {
        for (std::size_t group_index = 0U; group_index < plan.spawn_groups.size(); ++group_index) {
            const auto& spawn = plan.spawn_groups[group_index];
            if (pool_for_spawn_group[group_index] >= 0 && spawn.has_collider &&
                spawn.collider.group == collider_group) {
                return true;
            }
        }
        return false;
    };
    for (const auto& spawn : plan.spawn_groups) {
        if (!spawn.has_collider) continue;
        collider_groups.insert(spawn.collider.group);
        const auto velocity = groups_all_have_velocity.emplace(spawn.collider.group, spawn.has_velocity);
        if (!velocity.second) velocity.first->second = velocity.first->second && spawn.has_velocity;
        const auto inserted = group_motions.emplace(spawn.collider.group, spawn.collider.motion);
        if (!inserted.second && inserted.first->second != spawn.collider.motion) {
            return std::unexpected(game_error(
                DiagnosticCode::game_scene_invalid,
                "All colliders in a named group must use the same body motion",
                source,
                "/spawn_groups"));
        }
    }
    for (const auto& system : plan.systems) {
        const bool legacy_group_system = system.operation == GameOperationId::axis_control ||
                                         system.operation == GameOperationId::set_velocity_on_press;
        if (legacy_group_system && !collider_groups.contains(system.group)) {
            return std::unexpected(game_error(
                DiagnosticCode::game_scene_invalid, "System references a group without colliders", source,
                "/systems"));
        }
        if (system.operation == GameOperationId::axis_control &&
            (group_motions[system.group] != GameBodyMotion::kinematic_body ||
             !groups_all_have_velocity[system.group])) {
            return std::unexpected(game_error(
                DiagnosticCode::game_scene_invalid,
                "axis_control requires a kinematic collider group with Velocity2D",
                source,
                "/systems"));
        }
        if (system.operation == GameOperationId::set_velocity_on_press &&
            (group_motions[system.group] != GameBodyMotion::dynamic_body ||
             !groups_all_have_velocity[system.group])) {
            return std::unexpected(game_error(
                DiagnosticCode::game_scene_invalid,
                "set_velocity_on_press requires a dynamic collider group with Velocity2D",
                source,
                "/systems"));
        }
        if (system.operation == GameOperationId::grid_motion) {
            const auto& spawn = plan.spawn_groups[system.spawn_group_index];
            if (!spawn.has_velocity || !spawn.has_collider ||
                (spawn.collider.motion != GameBodyMotion::dynamic_body &&
                 (!version_0_5 || spawn.collider.motion != GameBodyMotion::kinematic_body))) {
                return std::unexpected(game_error(
                    DiagnosticCode::game_scene_invalid,
                    "grid_motion requires a dynamic collider, or a v0.5 kinematic collider, with Velocity2D",
                    source, "/systems"));
            }
        }
        if (system.operation == GameOperationId::linear_motion) {
            const auto& spawn = plan.spawn_groups[system.spawn_group_index];
            if (!spawn.has_velocity || (spawn.has_collider &&
                                         spawn.collider.motion != GameBodyMotion::kinematic_body)) {
                return std::unexpected(game_error(
                    DiagnosticCode::game_scene_invalid,
                    "linear_motion requires Velocity2D and only permits kinematic colliders",
                    source, "/systems"));
            }
        }
        if (system.operation == GameOperationId::follow_transform_chain) {
            const auto& leader = plan.spawn_groups[system.leader_group_index];
            if (leader.count != 1U || system.leader_group_index == system.follower_group_index) {
                return std::unexpected(game_error(
                    DiagnosticCode::game_scene_invalid,
                    "follow_transform_chain requires a one-entity leader and distinct followers",
                    source, "/systems"));
            }
            if (version_0_4 &&
                (pool_for_spawn_group[system.leader_group_index] >= 0 ||
                 pool_for_spawn_group[system.follower_group_index] >= 0)) {
                return std::unexpected(game_error(
                    DiagnosticCode::game_pool_invalid,
                    "Pooled groups cannot be owned by follow_transform_chain", source, "/systems"));
            }
        }
    }
    bool collision_system = false;
    std::uint32_t collision_system_count = 0U;
    std::size_t collision_system_index = 0U;
    for (std::size_t index = 0U; index < plan.systems.size(); ++index) {
        const auto& system = plan.systems[index];
        collision_system = collision_system || system.operation == GameOperationId::simulate_collisions;
        if (system.operation == GameOperationId::simulate_collisions) {
            ++collision_system_count;
            collision_system_index = index;
        }
    }
    if (!plan.collision_rules.empty() && !collision_system) {
        return std::unexpected(game_error(
            DiagnosticCode::game_scene_invalid, "Collision rules require a simulate_collisions system", source,
            "/collision_rules"));
    }
    if (version_0_3) {
        std::unordered_set<std::uint32_t> followed_groups{};
        std::unordered_set<std::uint32_t> linear_groups{};
        std::unordered_set<std::uint32_t> axis_groups{};
        std::vector<std::size_t> linear_index_by_group(plan.spawn_groups.size(), plan.systems.size());
        for (std::size_t index = 0U; index < plan.systems.size(); ++index) {
            const auto& system = plan.systems[index];
            if (system.operation == GameOperationId::linear_motion) {
                if (!linear_groups.insert(system.spawn_group_index).second) {
                    return std::unexpected(game_error(
                        DiagnosticCode::game_scene_invalid,
                        "A spawn group may have only one linear_motion system", source,
                        "/systems/" + std::to_string(index)));
                }
                linear_index_by_group[system.spawn_group_index] = index;
            }
            if (system.operation == GameOperationId::axis_control) {
                for (std::size_t group_index = 0U; group_index < plan.spawn_groups.size(); ++group_index) {
                    const auto& spawn = plan.spawn_groups[group_index];
                    if (spawn.has_collider && spawn.collider.group == system.group) {
                        axis_groups.insert(static_cast<std::uint32_t>(group_index));
                    }
                }
            }
        }
        for (std::size_t index = 0U; index < plan.systems.size(); ++index) {
            const auto& system = plan.systems[index];
            const bool kinematic_grid = system.operation == GameOperationId::grid_motion &&
                                        plan.spawn_groups[system.spawn_group_index].collider.motion ==
                                            GameBodyMotion::kinematic_body;
            if (system.operation == GameOperationId::grid_motion && !kinematic_grid &&
                (collision_system_count != 1U || index >= collision_system_index)) {
                return std::unexpected(game_error(
                    DiagnosticCode::game_scene_invalid,
                    "grid_motion requires exactly one later simulate_collisions system",
                    source, "/systems/" + std::to_string(index)));
            }
            if (kinematic_grid &&
                (collision_system_count != 1U || linear_index_by_group[system.spawn_group_index] >= plan.systems.size() ||
                 index >= linear_index_by_group[system.spawn_group_index] ||
                 linear_index_by_group[system.spawn_group_index] >= collision_system_index)) {
                return std::unexpected(game_error(
                    DiagnosticCode::game_scene_invalid,
                    "Kinematic grid_motion must precede linear_motion, which must precede simulate_collisions",
                    source, "/systems/" + std::to_string(index)));
            }
            if (system.operation == GameOperationId::follow_transform_chain &&
                (collision_system_count != 1U || system.motion_system_index >= collision_system_index ||
                 collision_system_index >= index)) {
                return std::unexpected(game_error(
                    DiagnosticCode::game_scene_invalid,
                    "Systems must order grid_motion before simulate_collisions before follow_transform_chain",
                    source, "/systems/" + std::to_string(index)));
            }
            if (system.operation == GameOperationId::follow_transform_chain &&
                !followed_groups.insert(system.follower_group_index).second) {
                return std::unexpected(game_error(
                    DiagnosticCode::game_scene_invalid,
                    "A follower spawn group may be owned by only one follow_transform_chain system",
                    source, "/systems/" + std::to_string(index) + "/followers"));
            }
            if (system.operation == GameOperationId::linear_motion && axis_groups.contains(system.spawn_group_index)) {
                return std::unexpected(game_error(
                    DiagnosticCode::game_scene_invalid,
                    "linear_motion cannot share a spawn group with axis_control", source,
                    "/systems/" + std::to_string(index)));
            }
            if (system.operation == GameOperationId::follow_transform_chain &&
                linear_groups.contains(system.follower_group_index)) {
                return std::unexpected(game_error(
                    DiagnosticCode::game_scene_invalid,
                    "linear_motion cannot own a follower spawn group", source,
                    "/systems/" + std::to_string(index)));
            }
        }
    }
    bool has_trigger_rule = false;
    for (const auto& rule : plan.collision_rules) {
        if (!collider_groups.contains(rule.group_a) || !collider_groups.contains(rule.group_b)) {
            return std::unexpected(game_error(
                DiagnosticCode::game_scene_invalid, "Collision rule references an unknown collider group", source,
                "/collision_rules"));
        }
        const auto motion_a = group_motions[rule.group_a];
        const auto motion_b = group_motions[rule.group_b];
        if (!version_0_5 && motion_a == GameBodyMotion::dynamic_body &&
            motion_b == GameBodyMotion::dynamic_body) {
            return std::unexpected(game_error(
                DiagnosticCode::game_scene_invalid,
                "SceneSpec does not support dynamic-versus-dynamic collision rules",
                source,
                "/collision_rules"));
        }
        if (version_0_5) {
            if (rule.interaction == GameCollisionInteraction::trigger) {
                has_trigger_rule = true;
                if (!rule.reactions.empty()) {
                    return std::unexpected(game_error(
                        DiagnosticCode::game_collision_interaction_invalid,
                        "Trigger collision rules cannot contain physical reactions", source, "/collision_rules"));
                }
            } else if (rule.interaction == GameCollisionInteraction::solid) {
                const bool exactly_one_dynamic =
                    (motion_a == GameBodyMotion::dynamic_body) != (motion_b == GameBodyMotion::dynamic_body);
                const bool has_reflect = std::any_of(
                    rule.reactions.begin(), rule.reactions.end(), [](const GameReactionPlan& reaction) {
                        return reaction.kind == GameReactionKind::reflect;
                    });
                if (!exactly_one_dynamic || !has_reflect) {
                    return std::unexpected(game_error(
                        DiagnosticCode::game_collision_interaction_invalid,
                        "Solid interaction requires exactly one dynamic endpoint and a reflect reaction",
                        source, "/collision_rules"));
                }
            } else {
                return std::unexpected(game_error(
                    DiagnosticCode::game_collision_interaction_invalid,
                    "SceneSpec 0.5 collision rules require an interaction", source, "/collision_rules"));
            }
        }
        for (const auto& reaction : rule.reactions) {
            if (reaction.kind == GameReactionKind::reset_group && !collider_groups.contains(reaction.group)) {
                return std::unexpected(game_error(
                    DiagnosticCode::game_scene_invalid, "reset_group references an unknown collider group", source,
                    "/collision_rules"));
            }
            if (reaction.kind == GameReactionKind::reflect) {
                const auto target_group = reaction.target == GameReactionTarget::a ? rule.group_a : rule.group_b;
                if (group_motions[target_group] != GameBodyMotion::dynamic_body ||
                    !groups_all_have_velocity[target_group]) {
                    return std::unexpected(game_error(
                        DiagnosticCode::game_scene_invalid,
                        "reflect target requires dynamic motion and Velocity2D on every collider group member",
                        source, "/collision_rules"));
                }
            }
            if (version_0_4 && reaction.kind == GameReactionKind::deactivate) {
                const auto target_group = reaction.target == GameReactionTarget::a ? rule.group_a : rule.group_b;
                if (collider_group_contains_pool(target_group)) {
                    return std::unexpected(game_error(
                        DiagnosticCode::game_pool_invalid,
                        "Legacy deactivate reactions cannot target pooled groups", source, "/collision_rules"));
                }
            }
            if (version_0_4 && reaction.kind == GameReactionKind::reset_group &&
                collider_group_contains_pool(reaction.group)) {
                return std::unexpected(game_error(
                    DiagnosticCode::game_pool_invalid,
                    "Legacy reset_group reactions cannot target pooled groups", source, "/collision_rules"));
            }
        }
    }
    if (has_trigger_rule && plan.max_contact_pairs == 0U) {
        return std::unexpected(game_error(
            DiagnosticCode::game_collision_interaction_invalid,
            "Trigger collision rules require collision.max_contact_pairs", source,
            "/collision/max_contact_pairs"));
    }
    if (version_0_4) {
        const auto target_collider_group = [&](const GameRulePlan& rule, const GameRuleTargetPlan& target) {
            const auto& collision_rule = plan.collision_rules[rule.event.collision_rule_index];
            return target.kind == GameRuleTargetKind::collision_a ? collision_rule.group_a : collision_rule.group_b;
        };
        const auto target_may_be_pooled = [&](const GameRulePlan& rule, const GameRuleTargetPlan& target) {
            if (target.kind == GameRuleTargetKind::spawn_group || target.kind == GameRuleTargetKind::spawn_index) {
                return pool_for_spawn_group[target.spawn_group_index] >= 0;
            }
            return collider_group_contains_pool(target_collider_group(rule, target));
        };
        const auto collision_target_is_owned_by_pool = [&](const GameRulePlan& rule,
                                                            const GameRuleTargetPlan& target,
                                                            const std::uint32_t pool_index) {
            const auto collider_group = target_collider_group(rule, target);
            const auto owned_group = plan.pools[pool_index].spawn_group_index;
            bool found = false;
            for (std::size_t group_index = 0U; group_index < plan.spawn_groups.size(); ++group_index) {
                const auto& spawn = plan.spawn_groups[group_index];
                if (!spawn.has_collider || spawn.collider.group != collider_group) continue;
                found = true;
                if (group_index != owned_group) return false;
            }
            return found;
        };
        for (const auto& rule : plan.rules) {
            for (const auto& action : rule.actions) {
                if ((action.kind == GameRuleActionKind::activate ||
                     action.kind == GameRuleActionKind::deactivate) &&
                    target_may_be_pooled(rule, action.target)) {
                    return std::unexpected(game_error(
                        DiagnosticCode::game_pool_invalid,
                        "Pool is the exclusive owner of its group active state", source, "/rules"));
                }
                if ((action.kind == GameRuleActionKind::set_group_active_count ||
                     action.kind == GameRuleActionKind::reset_group) &&
                    pool_for_spawn_group[action.spawn_group_index] >= 0) {
                    return std::unexpected(game_error(
                        DiagnosticCode::game_pool_invalid,
                        "Pool groups cannot use direct active-count or group reset actions", source, "/rules"));
                }
                if (action.kind == GameRuleActionKind::release_to_pool &&
                    (action.target.kind == GameRuleTargetKind::collision_a ||
                     action.target.kind == GameRuleTargetKind::collision_b) &&
                    !collision_target_is_owned_by_pool(rule, action.target, action.pool_index)) {
                    return std::unexpected(game_error(
                        DiagnosticCode::game_pool_invalid,
                        "release_to_pool collision target is not exclusively owned by the pool", source,
                        "/rules"));
                }
                if (action.kind == GameRuleActionKind::spawn_from_pool &&
                    action.pool_position_kind == GamePoolSpawnPositionKind::constant) {
                    const double x = static_cast<double>(action.pool_position.x) + action.pool_position_offset.x;
                    const double y = static_cast<double>(action.pool_position.y) + action.pool_position_offset.y;
                    if (!representable_float(x) || !representable_float(y)) {
                        return std::unexpected(game_error(
                            DiagnosticCode::game_pool_invalid,
                            "Pool spawn constant position plus offset exceeds the finite float range", source,
                        "/rules"));
                    }
                }
                if (action.kind == GameRuleActionKind::spawn_from_pool &&
                    (action.pool_position_kind == GamePoolSpawnPositionKind::initial ||
                     (action.pool_position_kind == GamePoolSpawnPositionKind::target &&
                      action.pool_position_target.kind == GameRuleTargetKind::spawn_index))) {
                    const auto group_index = action.pool_position_kind == GamePoolSpawnPositionKind::initial
                                                 ? plan.pools[action.pool_index].spawn_group_index
                                                 : action.pool_position_target.spawn_group_index;
                    const auto& group = plan.spawn_groups[group_index];
                    const auto first_item = action.pool_position_kind == GamePoolSpawnPositionKind::initial
                                                ? 0U
                                                : action.pool_position_target.item_index;
                    const auto item_count = action.pool_position_kind == GamePoolSpawnPositionKind::initial
                                                ? group.count
                                                : first_item + 1U;
                    for (std::uint32_t item = first_item; item < item_count; ++item) {
                        const auto column = item % group.placement.columns;
                        const auto row = item / group.placement.columns;
                        const double x = static_cast<double>(group.placement.origin.x) +
                                         static_cast<double>(column) * group.placement.spacing.x +
                                         group.transform.position_offset.x + action.pool_position_offset.x;
                        const double y = static_cast<double>(group.placement.origin.y) +
                                         static_cast<double>(row) * group.placement.spacing.y +
                                         group.transform.position_offset.y + action.pool_position_offset.y;
                        if (!representable_float(x) || !representable_float(y)) {
                            return std::unexpected(game_error(
                                DiagnosticCode::game_pool_invalid,
                                "Pool authored position plus offset exceeds the finite float range", source,
                                "/rules"));
                        }
                    }
                }
            }
        }
    }
    const auto group_aligned_to_grid = [&](const std::uint32_t group_index, const std::uint32_t grid_index) {
        const auto& group = plan.spawn_groups[group_index];
        const auto& grid = plan.grids[grid_index];
        constexpr double epsilon = 0.0001;
        for (std::uint32_t item = 0U; item < group.count; ++item) {
            const auto column = item % group.placement.columns;
            const auto row = item / group.placement.columns;
            const double x = static_cast<double>(group.placement.origin.x) +
                             static_cast<double>(column) * group.placement.spacing.x +
                             group.transform.position_offset.x;
            const double y = static_cast<double>(group.placement.origin.y) +
                             static_cast<double>(row) * group.placement.spacing.y +
                             group.transform.position_offset.y;
            const double grid_x = (x - grid.first_cell_center.x) / grid.cell_size.x;
            const double grid_y = (y - grid.first_cell_center.y) / grid.cell_size.y;
            const auto rounded_x = std::round(grid_x);
            const auto rounded_y = std::round(grid_y);
            if (std::abs(grid_x - rounded_x) > epsilon || std::abs(grid_y - rounded_y) > epsilon ||
                rounded_x < 0.0 || rounded_y < 0.0 || rounded_x >= grid.columns || rounded_y >= grid.rows) {
                return false;
            }
        }
        return true;
    };
    if (version_0_3) {
        for (const auto& system : plan.systems) {
            if (system.operation == GameOperationId::grid_motion &&
                !group_aligned_to_grid(system.spawn_group_index, system.grid_index)) {
                return std::unexpected(game_error(
                    DiagnosticCode::game_scene_invalid,
                    "All grid_motion reserve entities must be aligned to the logical grid",
                    source, "/systems"));
            }
            if (system.operation == GameOperationId::follow_transform_chain) {
                const auto grid_index = plan.systems[system.motion_system_index].grid_index;
                if (!group_aligned_to_grid(system.follower_group_index, grid_index)) {
                    return std::unexpected(game_error(
                        DiagnosticCode::game_scene_invalid,
                        "All follower reserve entities must be aligned to the motion grid",
                        source, "/systems"));
                }
            }
        }
        for (const auto& rule : plan.rules) {
            for (const auto& action : rule.actions) {
                if (action.kind != GameRuleActionKind::relocate_to_free_cell) continue;
                for (const auto group_index : action.occupancy_group_indices) {
                    if (!group_aligned_to_grid(group_index, action.grid_index)) {
                        return std::unexpected(game_error(
                            DiagnosticCode::game_scene_invalid,
                            "All occupancy reserve entities must be aligned to the relocation grid",
                            source, "/rules"));
                    }
                }
            }
        }
    }
    std::uint32_t collider_count = 0U;
    for (const auto& spawn : plan.spawn_groups) {
        if (spawn.has_collider) collider_count += spawn.count;
    }
    if (collider_count > plan.max_colliders) {
        return std::unexpected(game_error(
            DiagnosticCode::game_scene_invalid, "Configured max_colliders is too small", source, "/collision"));
    }
    return plan;
}

Result<TransitionConditionPlan> parse_transition_condition(
    const Json& object,
    const std::string_view pointer,
    const std::unordered_map<std::string, std::uint32_t>& actions,
    const std::unordered_map<std::string, std::uint32_t>& states,
    Symbols& symbols,
    const std::string_view source) {
    if (auto rejected = reject_unknown(
            object, {"kind", "action", "state", "value", "group"}, source, pointer,
            DiagnosticCode::game_transition_invalid);
        !rejected) {
        return std::unexpected(std::move(rejected.error()));
    }
    auto kind = string_value(object, "kind", source, pointer, DiagnosticCode::game_transition_invalid);
    if (!kind) return std::unexpected(std::move(kind.error()));
    TransitionConditionPlan plan{};
    if (*kind == "action_pressed") {
        plan.kind = TransitionConditionKind::action_pressed;
        auto action = string_value(object, "action", source, pointer, DiagnosticCode::game_transition_invalid);
        if (!action) return std::unexpected(std::move(action.error()));
        auto index = index_by_name(
            actions, *action, "action", source, std::string{pointer} + "/action",
            DiagnosticCode::game_transition_invalid);
        if (!index) return std::unexpected(std::move(index.error()));
        plan.action = *index;
    } else if (*kind == "int_state_at_most" || *kind == "int_state_at_least") {
        plan.kind = *kind == "int_state_at_most" ? TransitionConditionKind::int_state_at_most
                                                  : TransitionConditionKind::int_state_at_least;
        auto state = string_value(object, "state", source, pointer, DiagnosticCode::game_transition_invalid);
        if (!state) return std::unexpected(std::move(state.error()));
        auto index = index_by_name(
            states, *state, "integer state", source, std::string{pointer} + "/state",
            DiagnosticCode::game_transition_invalid);
        if (!index) return std::unexpected(std::move(index.error()));
        auto value = i32_value(
            object, "value", std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max(),
            source, pointer, DiagnosticCode::game_transition_invalid);
        if (!value) return std::unexpected(std::move(value.error()));
        plan.state_index = *index;
        plan.value = *value;
    } else if (*kind == "group_inactive") {
        plan.kind = TransitionConditionKind::group_inactive;
        auto group = string_value(object, "group", source, pointer, DiagnosticCode::game_transition_invalid);
        if (!group) return std::unexpected(std::move(group.error()));
        plan.group = symbols.intern(std::move(*group));
    } else {
        return std::unexpected(game_error(
            DiagnosticCode::game_transition_invalid, "Unsupported transition condition", source,
            std::string{pointer} + "/kind"));
    }
    const auto allowed_action = plan.kind == TransitionConditionKind::action_pressed;
    const auto allowed_state = plan.kind == TransitionConditionKind::int_state_at_most ||
                               plan.kind == TransitionConditionKind::int_state_at_least;
    const auto allowed_group = plan.kind == TransitionConditionKind::group_inactive;
    for (const auto& [field, allowed] : {
             std::pair<std::string_view, bool>{"action", allowed_action},
             {"state", allowed_state},
             {"value", allowed_state},
             {"group", allowed_group},
         }) {
        if (!allowed && optional(object, field) != nullptr) {
            return std::unexpected(game_error(
                DiagnosticCode::game_transition_invalid, "Field is not valid for this condition", source,
                std::string{pointer} + "/" + std::string{field}));
        }
    }
    return plan;
}

Result<std::uint32_t> decode_utf8_codepoint(
    const std::string_view text,
    std::size_t& offset,
    const std::string_view source,
    const std::string_view pointer) {
    const auto first = static_cast<std::uint8_t>(text[offset]);
    if (first < 0x80U) {
        ++offset;
        return first;
    }
    std::size_t length = 0U;
    std::uint32_t codepoint = 0U;
    if ((first & 0xE0U) == 0xC0U) {
        length = 2U;
        codepoint = first & 0x1FU;
    } else if ((first & 0xF0U) == 0xE0U) {
        length = 3U;
        codepoint = first & 0x0FU;
    } else if ((first & 0xF8U) == 0xF0U) {
        length = 4U;
        codepoint = first & 0x07U;
    } else {
        return std::unexpected(game_error(
            DiagnosticCode::asset_glyph_missing, "UI text contains invalid UTF-8", source, pointer));
    }
    if (offset + length > text.size()) {
        return std::unexpected(game_error(
            DiagnosticCode::asset_glyph_missing, "UI text contains truncated UTF-8", source, pointer));
    }
    for (std::size_t index = 1U; index < length; ++index) {
        const auto continuation = static_cast<std::uint8_t>(text[offset + index]);
        if ((continuation & 0xC0U) != 0x80U) {
            return std::unexpected(game_error(
                DiagnosticCode::asset_glyph_missing, "UI text contains invalid UTF-8", source, pointer));
        }
        codepoint = (codepoint << 6U) | (continuation & 0x3FU);
    }
    const bool overlong = (length == 2U && codepoint < 0x80U) || (length == 3U && codepoint < 0x800U) ||
                          (length == 4U && codepoint < 0x10000U);
    if (overlong || codepoint > 0x10FFFFU || (codepoint >= 0xD800U && codepoint <= 0xDFFFU)) {
        return std::unexpected(game_error(
            DiagnosticCode::asset_glyph_missing, "UI text contains an invalid Unicode codepoint", source, pointer));
    }
    offset += length;
    return codepoint;
}

Result<void> validate_ui_text(
    const GameScenePlan& scene,
    const std::vector<GameAssetPlan>& assets,
    const std::unordered_map<std::string, std::uint32_t>& states,
    const std::vector<IntStatePlan>& state_plans,
    const Symbols& symbols) {
    for (std::size_t element_index = 0U; element_index < scene.ui.size(); ++element_index) {
        const auto& element = scene.ui[element_index];
        // Localized elements store the key symbol here; every resolved locale
        // value is validated after the complete GamePlan has been assembled.
        if (element.kind == UiElementKind::panel || element.localized) continue;
        const auto text = symbols.view(element.text);
        const auto source = scene.source_path.string();
        const auto pointer = "/ui/" + std::to_string(element_index) + "/text";
        const auto& font = assets[element.font_asset];
        std::size_t offset = 0U;
        while (offset < text.size()) {
            if (text[offset] == '{') {
                const auto close = text.find('}', offset + 1U);
                if (close == std::string_view::npos || close == offset + 1U) {
                    return std::unexpected(game_error(
                        DiagnosticCode::game_scene_invalid, "UI state placeholder is malformed", source, pointer));
                }
                const auto name = text.substr(offset + 1U, close - offset - 1U);
                const auto state = states.find(std::string{name});
                if (state == states.end()) {
                    return std::unexpected(game_error(
                        DiagnosticCode::game_scene_invalid, "UI text references an unknown state", source, pointer));
                }
                const auto has_glyph = [&](const std::uint32_t codepoint) {
                    const auto found = std::lower_bound(
                        font.glyphs.begin(), font.glyphs.end(), codepoint,
                        [](const GlyphPlan& glyph, const std::uint32_t value) { return glyph.codepoint < value; });
                    return found != font.glyphs.end() && found->codepoint == codepoint;
                };
                for (std::uint32_t digit = static_cast<std::uint32_t>('0');
                     digit <= static_cast<std::uint32_t>('9');
                     ++digit) {
                    if (!has_glyph(digit)) {
                        return std::unexpected(game_error(
                            DiagnosticCode::asset_glyph_missing,
                            "Font atlas must contain decimal digits used by state placeholders",
                            source,
                            pointer));
                    }
                }
                if (state_plans[state->second].minimum < 0 && !has_glyph(static_cast<std::uint32_t>('-'))) {
                    return std::unexpected(game_error(
                        DiagnosticCode::asset_glyph_missing,
                        "Font atlas must contain a minus sign used by a signed state placeholder",
                        source,
                        pointer));
                }
                offset = close + 1U;
                continue;
            }
            auto codepoint = decode_utf8_codepoint(text, offset, source, pointer);
            if (!codepoint) return std::unexpected(std::move(codepoint.error()));
            if (*codepoint == '\n' || *codepoint == '\r') continue;
            const auto found = std::lower_bound(
                font.glyphs.begin(), font.glyphs.end(), *codepoint,
                [](const GlyphPlan& glyph, const std::uint32_t value) { return glyph.codepoint < value; });
            if (found == font.glyphs.end() || found->codepoint != *codepoint) {
                auto diagnostic = game_error(
                    DiagnosticCode::asset_glyph_missing, "Font atlas does not contain a required UI glyph", source,
                    pointer);
                diagnostic.context.push_back({"codepoint", std::to_string(*codepoint)});
                return std::unexpected(std::move(diagnostic));
            }
        }
    }
    return {};
}

void hash_vec2(StableHash& hash, const Vec2 value) noexcept {
    hash.scalar(value.x);
    hash.scalar(value.y);
}

void hash_rect(StableHash& hash, const Rect value) noexcept {
    hash_vec2(hash, value.min);
    hash_vec2(hash, value.max);
}

void hash_color(StableHash& hash, const Color value) noexcept {
    hash.scalar(value.r);
    hash.scalar(value.g);
    hash.scalar(value.b);
    hash.scalar(value.a);
}

} // namespace

std::string_view GamePlan::symbol(const SymbolId id) const noexcept {
    return id < symbols.size() ? std::string_view{symbols[id]} : std::string_view{};
}

std::string_view GamePlan::schema_version_text() const noexcept {
    switch (schema_version) {
    case GameSchemaVersion::v0_2: return legacy_schema_version;
    case GameSchemaVersion::v0_3: return event_action_schema_version;
    case GameSchemaVersion::v0_4: return object_pool_schema_version;
    case GameSchemaVersion::v0_5: return motion_contacts_schema_version;
    case GameSchemaVersion::v0_6: return supported_schema_version;
    }
    return {};
}

std::string_view to_string(const GameAssetKind value) noexcept {
    switch (value) {
    case GameAssetKind::png: return "png";
    case GameAssetKind::wav: return "wav";
    case GameAssetKind::font: return "font";
    case GameAssetKind::music: return "music";
    }
    return "png";
}

std::string_view to_string(const GameOperationId value) noexcept {
    switch (value) {
    case GameOperationId::axis_control: return "axis_control";
    case GameOperationId::set_velocity_on_press: return "set_velocity_on_press";
    case GameOperationId::simulate_collisions: return "simulate_collisions";
    case GameOperationId::grid_motion: return "grid_motion";
    case GameOperationId::follow_transform_chain: return "follow_transform_chain";
    case GameOperationId::linear_motion: return "linear_motion";
    }
    return "simulate_collisions";
}

std::string_view to_string(const GameDirection value) noexcept {
    switch (value) {
    case GameDirection::none: return "none";
    case GameDirection::up: return "up";
    case GameDirection::down: return "down";
    case GameDirection::left: return "left";
    case GameDirection::right: return "right";
    }
    return "none";
}

std::string_view to_string(const GameRuleEventKind value) noexcept {
    switch (value) {
    case GameRuleEventKind::scene_enter: return "scene_enter";
    case GameRuleEventKind::action_pressed: return "action_pressed";
    case GameRuleEventKind::action_released: return "action_released";
    case GameRuleEventKind::fixed_interval: return "fixed_interval";
    case GameRuleEventKind::collision: return "collision";
    case GameRuleEventKind::contact_begin: return "contact_begin";
    case GameRuleEventKind::contact_end: return "contact_end";
    case GameRuleEventKind::animation_finished: return "animation_finished";
    }
    return "scene_enter";
}

std::string_view to_string(const GameRuleActionKind value) noexcept {
    switch (value) {
    case GameRuleActionKind::set_int_state: return "set_int_state";
    case GameRuleActionKind::add_int_state: return "add_int_state";
    case GameRuleActionKind::activate: return "activate";
    case GameRuleActionKind::deactivate: return "deactivate";
    case GameRuleActionKind::set_group_active_count: return "set_group_active_count";
    case GameRuleActionKind::set_velocity: return "set_velocity";
    case GameRuleActionKind::queue_grid_direction: return "queue_grid_direction";
    case GameRuleActionKind::reset_group: return "reset_group";
    case GameRuleActionKind::play_sound: return "play_sound";
    case GameRuleActionKind::relocate_to_free_cell: return "relocate_to_free_cell";
    case GameRuleActionKind::spawn_from_pool: return "spawn_from_pool";
    case GameRuleActionKind::release_to_pool: return "release_to_pool";
    case GameRuleActionKind::reset_pool: return "reset_pool";
    case GameRuleActionKind::play_animation: return "play_animation";
    case GameRuleActionKind::stop_animation: return "stop_animation";
    case GameRuleActionKind::set_tile: return "set_tile";
    case GameRuleActionKind::set_field: return "set_field";
    case GameRuleActionKind::add_field: return "add_field";
    case GameRuleActionKind::save_slot: return "save_slot";
    case GameRuleActionKind::load_slot: return "load_slot";
    case GameRuleActionKind::delete_slot: return "delete_slot";
    case GameRuleActionKind::camera_shake: return "camera_shake";
    case GameRuleActionKind::set_camera_zoom: return "set_camera_zoom";
    case GameRuleActionKind::set_locale: return "set_locale";
    case GameRuleActionKind::set_input_profile: return "set_input_profile";
    case GameRuleActionKind::play_music: return "play_music";
    case GameRuleActionKind::stop_music: return "stop_music";
    case GameRuleActionKind::set_music_volume: return "set_music_volume";
    case GameRuleActionKind::emit_particles: return "emit_particles";
    }
    return "set_int_state";
}

std::string_view to_string(const GamePoolExhaustionPolicy value) noexcept {
    switch (value) {
    case GamePoolExhaustionPolicy::skip: return "skip";
    case GamePoolExhaustionPolicy::recycle_oldest: return "recycle_oldest";
    }
    return "skip";
}

std::string_view to_string(const GamePoolSpawnPositionKind value) noexcept {
    switch (value) {
    case GamePoolSpawnPositionKind::initial: return "initial";
    case GamePoolSpawnPositionKind::constant: return "constant";
    case GamePoolSpawnPositionKind::target: return "target";
    }
    return "initial";
}

std::string_view to_string(const GameCollisionInteraction value) noexcept {
    switch (value) {
    case GameCollisionInteraction::legacy: return "legacy";
    case GameCollisionInteraction::solid: return "solid";
    case GameCollisionInteraction::trigger: return "trigger";
    }
    return "legacy";
}

std::string_view to_string(const GameTestAssertionKind value) noexcept {
    switch (value) {
    case GameTestAssertionKind::current_scene: return "current_scene";
    case GameTestAssertionKind::int_state: return "int_state";
    case GameTestAssertionKind::group_active_count: return "group_active_count";
    case GameTestAssertionKind::entity_active: return "entity_active";
    case GameTestAssertionKind::position: return "position";
    case GameTestAssertionKind::velocity: return "velocity";
    case GameTestAssertionKind::runtime_metric: return "runtime_metric";
    case GameTestAssertionKind::animation_frame: return "animation_frame";
    case GameTestAssertionKind::tile_value: return "tile_value";
    case GameTestAssertionKind::field_value: return "field_value";
    case GameTestAssertionKind::camera_position: return "camera_position";
    }
    return "current_scene";
}

std::string_view to_string(const GameTestMetric value) noexcept {
    switch (value) {
    case GameTestMetric::rule_executions: return "rule_executions";
    case GameTestMetric::condition_evaluations: return "condition_evaluations";
    case GameTestMetric::action_executions: return "action_executions";
    case GameTestMetric::grid_steps: return "grid_steps";
    case GameTestMetric::rejected_direction_changes: return "rejected_direction_changes";
    case GameTestMetric::follower_updates: return "follower_updates";
    case GameTestMetric::active_state_changes: return "active_state_changes";
    case GameTestMetric::relocations: return "relocations";
    case GameTestMetric::relocation_cells_scanned: return "relocation_cells_scanned";
    case GameTestMetric::collision_contacts: return "collision_contacts";
    case GameTestMetric::trigger_narrowphase_tests: return "trigger_narrowphase_tests";
    case GameTestMetric::contact_begins: return "contact_begins";
    case GameTestMetric::contact_ends: return "contact_ends";
    case GameTestMetric::stale_contact_events: return "stale_contact_events";
    case GameTestMetric::active_contact_pairs: return "active_contact_pairs";
    case GameTestMetric::peak_contact_pairs: return "peak_contact_pairs";
    case GameTestMetric::motion_segments: return "motion_segments";
    case GameTestMetric::linear_motion_updates: return "linear_motion_updates";
    case GameTestMetric::pool_acquire_attempts: return "pool_acquire_attempts";
    case GameTestMetric::pool_acquire_successes: return "pool_acquire_successes";
    case GameTestMetric::pool_releases: return "pool_releases";
    case GameTestMetric::pool_release_misses: return "pool_release_misses";
    case GameTestMetric::pool_exhaustions: return "pool_exhaustions";
    case GameTestMetric::pool_recycled_slots: return "pool_recycled_slots";
    case GameTestMetric::pool_expirations: return "pool_expirations";
    case GameTestMetric::pool_resets: return "pool_resets";
    case GameTestMetric::pool_lifetime_checks: return "pool_lifetime_checks";
    case GameTestMetric::active_pooled_entities: return "active_pooled_entities";
    case GameTestMetric::peak_active_pooled_entities: return "peak_active_pooled_entities";
    case GameTestMetric::animation_frame_updates: return "animation_frame_updates";
    case GameTestMetric::animation_completions: return "animation_completions";
    case GameTestMetric::tile_reads: return "tile_reads";
    case GameTestMetric::tile_writes: return "tile_writes";
    case GameTestMetric::field_reads: return "field_reads";
    case GameTestMetric::field_writes: return "field_writes";
    case GameTestMetric::save_attempts: return "save_attempts";
    case GameTestMetric::save_successes: return "save_successes";
    case GameTestMetric::save_failures: return "save_failures";
    case GameTestMetric::particle_emits: return "particle_emits";
    case GameTestMetric::particle_updates: return "particle_updates";
    case GameTestMetric::particle_exhaustions: return "particle_exhaustions";
    case GameTestMetric::particle_slot_operations: return "particle_slot_operations";
    case GameTestMetric::peak_active_particles: return "peak_active_particles";
    case GameTestMetric::camera_follow_updates: return "camera_follow_updates";
    case GameTestMetric::camera_shake_updates: return "camera_shake_updates";
    case GameTestMetric::music_stream_bytes: return "music_stream_bytes";
    case GameTestMetric::music_underruns: return "music_underruns";
    case GameTestMetric::input_profile_switches: return "input_profile_switches";
    case GameTestMetric::locale_switches: return "locale_switches";
    }
    return "rule_executions";
}

std::string_view to_string(const GameAnimationMode value) noexcept {
    switch (value) {
    case GameAnimationMode::once: return "once";
    case GameAnimationMode::loop: return "loop";
    case GameAnimationMode::ping_pong: return "ping_pong";
    }
    return "loop";
}

std::string_view to_string(const GameReactionKind value) noexcept {
    switch (value) {
    case GameReactionKind::reflect: return "reflect";
    case GameReactionKind::deactivate: return "deactivate";
    case GameReactionKind::add_int_state: return "add_int_state";
    case GameReactionKind::reset_group: return "reset_group";
    case GameReactionKind::play_sound: return "play_sound";
    }
    return "reflect";
}

std::string_view to_string(const TransitionConditionKind value) noexcept {
    switch (value) {
    case TransitionConditionKind::action_pressed: return "action_pressed";
    case TransitionConditionKind::int_state_at_most: return "int_state_at_most";
    case TransitionConditionKind::int_state_at_least: return "int_state_at_least";
    case TransitionConditionKind::group_inactive: return "group_inactive";
    }
    return "action_pressed";
}

std::string_view to_string(const UiElementKind value) noexcept {
    switch (value) {
    case UiElementKind::panel: return "panel";
    case UiElementKind::text: return "text";
    case UiElementKind::button: return "button";
    }
    return "panel";
}

std::string_view to_string(const RenderFpsCap value) noexcept {
    switch (value) {
    case RenderFpsCap::fps_60: return "60";
    case RenderFpsCap::fps_120: return "120";
    case RenderFpsCap::fps_144: return "144";
    case RenderFpsCap::fps_240: return "240";
    case RenderFpsCap::unlimited: return "unlimited";
    }
    return "60";
}

std::uint64_t compute_game_plan_hash(const GamePlan& plan) {
    StableHash hash{};
    const bool version_0_3 = plan.schema_version != GameSchemaVersion::v0_2;
    const bool version_0_4 = plan.schema_version == GameSchemaVersion::v0_4 ||
                             plan.schema_version == GameSchemaVersion::v0_5 ||
                             plan.schema_version == GameSchemaVersion::v0_6;
    const bool version_0_5 = plan.schema_version == GameSchemaVersion::v0_5 ||
                             plan.schema_version == GameSchemaVersion::v0_6;
    const bool version_0_6 = plan.schema_version == GameSchemaVersion::v0_6;
    const auto hash_content_path = [&](const std::filesystem::path& path) {
        if (version_0_6) {
            hash.text(path.lexically_relative(plan.content_root).generic_string());
        } else {
            hash.text(path.generic_string());
        }
    };
    hash.text(plan.schema_version_text());
    hash.scalar(plan.source_hash);
    if (version_0_3) hash.scalar(plan.seed);
    for (const auto& symbol : plan.symbols) hash.text(symbol);
    hash.scalar(plan.name);
    hash.scalar(plan.organization);
    hash.scalar(plan.application);
    hash.scalar(plan.window.title);
    hash.scalar(plan.window.width);
    hash.scalar(plan.window.height);
    hash.scalar(plan.window.virtual_width);
    hash.scalar(plan.window.virtual_height);
    hash.scalar(plan.window.default_fps);
    for (const auto& asset : plan.assets) {
        hash.scalar(asset.symbol);
        hash.scalar(asset.kind);
        hash_content_path(asset.path);
        hash_content_path(asset.metadata_path);
        hash.scalar(asset.line_height);
        for (const auto& glyph : asset.glyphs) {
            hash.scalar(glyph.codepoint);
            hash_rect(hash, glyph.uv);
            hash_vec2(hash, glyph.size);
            hash_vec2(hash, glyph.bearing);
            hash.scalar(glyph.advance);
        }
    }
    if (version_0_6) {
        for (const auto& animation : plan.animations) {
            hash.scalar(animation.symbol);
            hash.scalar(animation.asset_index);
            hash.scalar(animation.mode);
            for (const auto& frame : animation.frames) {
                hash_rect(hash, frame.uv);
                hash.scalar(frame.duration_ticks);
            }
        }
        for (const auto& prefab : plan.prefabs) {
            hash.scalar(prefab.symbol);
            hash_content_path(prefab.path);
            hash.scalar(prefab.has_transform);
            hash_vec2(hash, prefab.transform.position_offset);
            hash.scalar(prefab.transform.rotation);
            hash_vec2(hash, prefab.transform.scale);
            hash.scalar(prefab.has_velocity);
            hash_vec2(hash, prefab.velocity.linear);
            hash.scalar(prefab.velocity.angular);
            hash.scalar(prefab.has_sprite);
            hash.scalar(prefab.sprite.asset_index);
            hash_vec2(hash, prefab.sprite.size);
            hash_vec2(hash, prefab.sprite.pivot);
            hash_rect(hash, prefab.sprite.uv);
            hash_color(hash, prefab.sprite.tint);
            hash.scalar(prefab.sprite.layer);
            hash.scalar(prefab.sprite.visible);
            hash.scalar(prefab.has_collider);
            hash_vec2(hash, prefab.collider.offset);
            hash_vec2(hash, prefab.collider.half_extent);
            hash.scalar(prefab.collider.group);
            hash.scalar(prefab.collider.motion);
            hash.scalar(prefab.collider.enabled);
            hash.scalar(prefab.has_animation);
            hash.scalar(prefab.animation_index);
            hash.scalar(prefab.animation_autoplay);
        }
        for (const auto& localization : plan.localizations) {
            hash.scalar(localization.locale);
            hash_content_path(localization.path);
            for (const auto& entry : localization.entries) {
                hash.scalar(entry.key);
                hash.scalar(entry.value);
            }
        }
        hash.scalar(plan.default_locale_index);
        for (const auto& profile : plan.input_profiles) {
            hash.scalar(profile.symbol);
            for (const auto& action : profile.actions) {
                hash.scalar(action.symbol);
                hash.scalar(action.key_count);
                for (std::uint32_t index = 0U; index < action.key_count; ++index) hash.scalar(action.keys[index]);
                hash.scalar(action.gamepad_button_count);
                for (std::uint32_t index = 0U; index < action.gamepad_button_count; ++index) {
                    hash.scalar(action.gamepad_buttons[index]);
                }
                hash.scalar(action.gamepad_axis_count);
                for (std::uint32_t index = 0U; index < action.gamepad_axis_count; ++index) {
                    hash.scalar(action.gamepad_axes[index].axis);
                    hash.scalar(action.gamepad_axes[index].direction);
                    hash.scalar(action.gamepad_axes[index].deadzone);
                }
                hash.scalar(action.mouse_left);
            }
        }
        hash.scalar(plan.default_input_profile_index);
        hash.scalar(plan.save.enabled);
        hash.scalar(plan.save.slot_count);
        for (const auto state_index : plan.save.state_indices) hash.scalar(state_index);
    }
    for (const auto& action : plan.actions) {
        hash.scalar(action.symbol);
        hash.scalar(action.key_count);
        hash.scalar(action.mouse_left);
        const auto safe_key_count = std::min<std::uint32_t>(
            action.key_count, static_cast<std::uint32_t>(ActionPlan::max_keys));
        for (std::uint32_t index = 0U; index < safe_key_count; ++index) hash.scalar(action.keys[index]);
    }
    for (const auto& state : plan.states) {
        hash.scalar(state.symbol);
        hash.scalar(state.initial);
        hash.scalar(state.minimum);
        hash.scalar(state.maximum);
    }
    for (const auto& scene : plan.scenes) {
        hash.scalar(scene.symbol);
        hash_content_path(scene.source_path);
        hash.scalar(scene.world_capacity);
        hash_vec2(hash, scene.camera_position);
        hash_vec2(hash, scene.camera_half_extent);
        if (version_0_6) {
            hash.scalar(scene.camera.mode);
            hash_vec2(hash, scene.camera.position);
            hash_vec2(hash, scene.camera.half_extent);
            hash.scalar(scene.camera.follow_group_index);
            hash.scalar(scene.camera.follow_item_index);
            hash_vec2(hash, scene.camera.follow_offset);
            hash_rect(hash, scene.camera.bounds);
            hash.scalar(scene.camera.has_bounds);
            hash.scalar(scene.camera.pixel_snap);
            hash.scalar(scene.persistent);
        }
        hash_rect(hash, scene.collision_bounds);
        hash_vec2(hash, scene.collision_cell_size);
        hash.scalar(scene.max_colliders);
        hash.scalar(scene.max_grid_references);
        hash.scalar(scene.max_candidate_pairs);
        if (version_0_5) hash.scalar(scene.max_contact_pairs);
        hash.scalar(scene.max_impacts);
        if (version_0_3) {
            for (const auto& grid : scene.grids) {
                hash.scalar(grid.symbol);
                hash_vec2(hash, grid.first_cell_center);
                hash_vec2(hash, grid.cell_size);
                hash.scalar(grid.columns);
                hash.scalar(grid.rows);
            }
        }
        for (const auto& spawn : scene.spawn_groups) {
            hash.scalar(spawn.symbol);
            hash.scalar(spawn.count);
            if (version_0_3) hash.scalar(spawn.active_count);
            hash.scalar(spawn.placement.kind);
            hash_vec2(hash, spawn.placement.origin);
            hash_vec2(hash, spawn.placement.spacing);
            hash.scalar(spawn.placement.columns);
            hash_vec2(hash, spawn.transform.position_offset);
            hash.scalar(spawn.transform.rotation);
            hash_vec2(hash, spawn.transform.scale);
            hash.scalar(spawn.has_velocity);
            hash_vec2(hash, spawn.velocity.linear);
            hash.scalar(spawn.velocity.angular);
            hash.scalar(spawn.has_sprite);
            hash.scalar(spawn.sprite.asset_index);
            hash_vec2(hash, spawn.sprite.size);
            hash_vec2(hash, spawn.sprite.pivot);
            hash_rect(hash, spawn.sprite.uv);
            hash_color(hash, spawn.sprite.tint);
            hash.scalar(spawn.sprite.layer);
            hash.scalar(spawn.sprite.visible);
            hash.scalar(spawn.has_collider);
            hash_vec2(hash, spawn.collider.offset);
            hash_vec2(hash, spawn.collider.half_extent);
            hash.scalar(spawn.collider.group);
            hash.scalar(spawn.collider.motion);
            hash.scalar(spawn.collider.trigger);
            hash.scalar(spawn.collider.enabled);
            if (version_0_6) {
                hash.scalar(spawn.initial_animation_index);
                hash.scalar(spawn.has_initial_animation);
                hash.scalar(spawn.animation_autoplay);
            }
        }
        if (version_0_4) {
            for (const auto& pool : scene.pools) {
                hash.scalar(pool.symbol);
                hash.scalar(pool.spawn_group_index);
                hash.scalar(pool.on_exhausted);
            }
        }
        if (version_0_6) {
            for (const auto& layer : scene.tile_layers) {
                hash.scalar(layer.symbol);
                hash.scalar(layer.grid_index);
                hash.scalar(layer.asset_index);
                hash.scalar(layer.atlas_columns);
                hash.scalar(layer.atlas_rows);
                for (const auto value : layer.initial_cells) hash.scalar(value);
                hash_color(hash, layer.tint);
                hash.scalar(layer.layer);
                hash.scalar(layer.visible);
            }
            for (const auto& field : scene.fields) {
                hash.scalar(field.symbol);
                hash.scalar(field.grid_index);
                hash.scalar(field.minimum);
                hash.scalar(field.maximum);
                for (const auto value : field.initial_cells) hash.scalar(value);
            }
            for (const auto& emitter : scene.particle_emitters) {
                hash.scalar(emitter.symbol);
                hash.scalar(emitter.capacity);
                hash.scalar(emitter.on_exhausted);
                hash.scalar(emitter.sprite.asset_index);
                hash_vec2(hash, emitter.sprite.size);
                hash_vec2(hash, emitter.sprite.pivot);
                hash_rect(hash, emitter.sprite.uv);
                hash_color(hash, emitter.sprite.tint);
                hash.scalar(emitter.sprite.layer);
                hash.scalar(emitter.sprite.visible);
                hash.scalar(emitter.lifetime_min_ticks);
                hash.scalar(emitter.lifetime_max_ticks);
                hash_vec2(hash, emitter.velocity_min);
                hash_vec2(hash, emitter.velocity_max);
            }
        }
        for (const auto& system : scene.systems) {
            hash.scalar(system.symbol);
            hash.scalar(system.operation);
            hash.scalar(system.group);
            hash.scalar(system.negative_action);
            hash.scalar(system.positive_action);
            hash.scalar(system.action);
            hash.scalar(system.speed);
            hash.scalar(system.minimum);
            hash.scalar(system.maximum);
            hash_vec2(hash, system.velocity);
            if (version_0_3) {
                hash.scalar(system.spawn_group_index);
                hash.scalar(system.grid_index);
                hash.scalar(system.step_interval_ticks);
                hash.scalar(system.initial_direction);
                hash.scalar(system.prevent_reverse);
                hash.scalar(system.leader_group_index);
                hash.scalar(system.follower_group_index);
                hash.scalar(system.motion_system_index);
            }
        }
        for (const auto& rule : scene.collision_rules) {
            if (version_0_3) hash.scalar(rule.symbol);
            hash.scalar(rule.group_a);
            hash.scalar(rule.group_b);
            if (version_0_5) hash.scalar(rule.interaction);
            for (const auto& reaction : rule.reactions) {
                hash.scalar(reaction.kind);
                hash.scalar(reaction.target);
                hash.scalar(reaction.state_index);
                hash.scalar(reaction.value);
                hash.scalar(reaction.group);
                hash.scalar(reaction.asset_index);
            }
        }
        if (version_0_3) {
            for (const auto& rule : scene.rules) {
                hash.scalar(rule.symbol);
                hash.scalar(rule.event.kind);
                hash.scalar(rule.event.action_index);
                hash.scalar(rule.event.interval_ticks);
                hash.scalar(rule.event.collision_rule_index);
                if (version_0_6) hash.scalar(rule.event.animation_index);
                for (const auto& condition : rule.conditions) {
                    hash.scalar(condition.kind);
                    hash.scalar(condition.comparison);
                    hash.scalar(condition.state_index);
                    hash.scalar(condition.spawn_group_index);
                    if (version_0_6) {
                        hash.scalar(condition.tile_layer_index);
                        hash.scalar(condition.field_index);
                        hash.scalar(condition.cell_source);
                        hash.scalar(condition.cell_x);
                        hash.scalar(condition.cell_y);
                        hash.scalar(condition.cell_target.kind);
                        hash.scalar(condition.cell_target.spawn_group_index);
                        hash.scalar(condition.cell_target.item_index);
                    }
                    hash.scalar(condition.value);
                }
                for (const auto& action : rule.actions) {
                    hash.scalar(action.kind);
                    hash.scalar(action.target.kind);
                    hash.scalar(action.target.spawn_group_index);
                    hash.scalar(action.target.item_index);
                    hash.scalar(action.state_index);
                    hash.scalar(action.value);
                    hash.scalar(action.spawn_group_index);
                    hash.scalar(action.count_from_state);
                    hash.scalar(action.count_state_index);
                    hash.scalar(action.count_constant);
                    hash_vec2(hash, action.velocity);
                    hash.scalar(action.system_index);
                    hash.scalar(action.direction);
                    hash.scalar(action.asset_index);
                    hash.scalar(action.grid_index);
                    for (const auto group : action.occupancy_group_indices) hash.scalar(group);
                    hash.scalar(action.has_result_state);
                    hash.scalar(action.result_state_index);
                    if (version_0_4) {
                        hash.scalar(action.pool_index);
                        hash.scalar(action.pool_position_kind);
                        hash_vec2(hash, action.pool_position);
                        hash.scalar(action.pool_position_target.kind);
                        hash.scalar(action.pool_position_target.spawn_group_index);
                        hash.scalar(action.pool_position_target.item_index);
                        hash_vec2(hash, action.pool_position_offset);
                        hash.scalar(action.has_velocity_override);
                        hash.scalar(action.has_rotation_override);
                        hash.scalar(action.rotation);
                        hash.scalar(action.has_lifetime);
                        hash.scalar(action.lifetime_ticks);
                    }
                    if (version_0_6) {
                        hash.scalar(action.animation_index);
                        hash.scalar(action.restart_animation);
                        hash.scalar(action.tile_layer_index);
                        hash.scalar(action.field_index);
                        hash.scalar(action.cell_source);
                        hash.scalar(action.cell_x);
                        hash.scalar(action.cell_y);
                        hash.scalar(action.cell_target.kind);
                        hash.scalar(action.cell_target.spawn_group_index);
                        hash.scalar(action.cell_target.item_index);
                        hash.scalar(action.tile_value);
                        hash.scalar(action.save_slot);
                        hash.scalar(action.scalar);
                        hash.scalar(action.duration_ticks);
                        hash.scalar(action.locale_index);
                        hash.scalar(action.input_profile_index);
                        hash.scalar(action.loop);
                        hash.scalar(action.fade_ticks);
                        hash.scalar(action.particle_emitter_index);
                        hash.scalar(action.particle_count);
                        hash.scalar(action.particle_position_kind);
                        hash_vec2(hash, action.particle_position);
                        hash.scalar(action.particle_position_target.kind);
                        hash.scalar(action.particle_position_target.spawn_group_index);
                        hash.scalar(action.particle_position_target.item_index);
                        hash_vec2(hash, action.particle_position_offset);
                    }
                }
            }
        }
        for (const auto& element : scene.ui) {
            hash.scalar(element.symbol);
            hash.scalar(element.kind);
            hash_vec2(hash, element.position);
            hash_vec2(hash, element.size);
            hash.scalar(element.text);
            hash.scalar(element.font_asset);
            hash.scalar(element.action);
            hash_color(hash, element.color);
            hash_color(hash, element.hover_color);
            hash_color(hash, element.text_color);
            hash.scalar(element.layer);
            hash.scalar(element.text_scale);
            if (version_0_6) {
                hash.scalar(element.anchor);
                hash.scalar(element.localized);
                hash.scalar(element.localization_key_index);
            }
        }
        if (version_0_6) {
            for (const auto& stack : scene.ui_stacks) {
                hash.scalar(stack.symbol);
                hash.scalar(stack.direction);
                hash_vec2(hash, stack.position);
                hash_vec2(hash, stack.size);
                hash_vec2(hash, stack.padding);
                hash.scalar(stack.spacing);
                hash.scalar(stack.anchor);
                for (const auto child : stack.child_indices) hash.scalar(child);
            }
        }
    }
    for (const auto& transition : plan.transitions) {
        hash.scalar(transition.from_scene);
        hash.scalar(transition.condition.kind);
        hash.scalar(transition.condition.action);
        hash.scalar(transition.condition.state_index);
        hash.scalar(transition.condition.value);
        hash.scalar(transition.condition.group);
        hash.scalar(transition.to_scene);
        hash.scalar(transition.quit);
        hash.scalar(transition.reset_scene);
        hash.scalar(transition.reset_session);
    }
    for (const auto& fps : plan.fps_actions) {
        hash.scalar(fps.action);
        hash.scalar(fps.cap);
    }
    hash.scalar(plan.start_scene);
    return hash.value();
}

Result<std::uint64_t> compute_game_save_capacity(const GamePlan& plan) {
    if (!plan.save.enabled) return std::uint64_t{0U};
    constexpr std::uint64_t maximum_save_bytes = 64ULL * 1024ULL * 1024ULL;
    std::uint64_t capacity =
        64U + static_cast<std::uint64_t>(plan.save.state_indices.size()) * 8U +
        static_cast<std::uint64_t>(plan.scenes.size());
    const auto add = [&](const std::uint64_t amount) -> bool {
        if (capacity > maximum_save_bytes || amount > maximum_save_bytes - capacity) {
            capacity = maximum_save_bytes + 1U;
            return false;
        }
        capacity += amount;
        return true;
    };
    for (const auto& scene : plan.scenes) {
        if (!scene.persistent) continue;
        if (!add(81U)) break;
        for (const auto& group : scene.spawn_groups) {
            std::uint64_t bytes_per_entity = 45U;
            if (group.has_velocity) bytes_per_entity += 12U;
            if (group.has_sprite) bytes_per_entity += 53U;
            if (group.has_collider) bytes_per_entity += 23U;
            if (!add(static_cast<std::uint64_t>(group.count) * bytes_per_entity)) break;
        }
        if (capacity > maximum_save_bytes) break;
        if (!add(static_cast<std::uint64_t>(scene.systems.size()) * 7U) ||
            !add(static_cast<std::uint64_t>(scene.rules.size()) * 8U) ||
            !add(static_cast<std::uint64_t>(scene.max_contact_pairs) * 12U)) {
            break;
        }
        for (const auto& layer : scene.tile_layers) {
            if (!add(4U + static_cast<std::uint64_t>(layer.initial_cells.size()) * 4U)) break;
        }
        if (capacity > maximum_save_bytes) break;
        for (const auto& field : scene.fields) {
            if (!add(4U + static_cast<std::uint64_t>(field.initial_cells.size()) * 4U)) break;
        }
        if (capacity > maximum_save_bytes) break;
        for (const auto& emitter : scene.particle_emitters) {
            if (!add(24U + static_cast<std::uint64_t>(emitter.capacity) * 41U)) break;
        }
        if (capacity > maximum_save_bytes) break;
        for (const auto& pool : scene.pools) {
            if (pool.spawn_group_index >= scene.spawn_groups.size() ||
                !add(24U + static_cast<std::uint64_t>(
                               scene.spawn_groups[pool.spawn_group_index].count) * 21U)) {
                capacity = maximum_save_bytes + 1U;
                break;
            }
        }
        if (capacity > maximum_save_bytes) break;
    }
    if (capacity > maximum_save_bytes) {
        return std::unexpected(game_error(
            DiagnosticCode::game_save_invalid,
            "Calculated save capacity exceeds 64 MiB",
            plan.manifest_path.string()));
    }
    return capacity;
}

Result<void> validate_game_plan(const GamePlan& plan) {
    const auto invalid = [&](const std::string_view message) -> Result<void> {
        return std::unexpected(game_error(
            DiagnosticCode::game_manifest_invalid, std::string{message}, plan.manifest_path.string()));
    };
    const auto pool_invalid = [&](const std::string_view message) -> Result<void> {
        return std::unexpected(game_error(
            DiagnosticCode::game_pool_invalid, std::string{message}, plan.manifest_path.string()));
    };
    const auto interaction_invalid = [&](const std::string_view message) -> Result<void> {
        return std::unexpected(game_error(
            DiagnosticCode::game_collision_interaction_invalid, std::string{message},
            plan.manifest_path.string()));
    };
    const auto animation_invalid = [&](const std::string_view message) -> Result<void> {
        return std::unexpected(game_error(
            DiagnosticCode::game_animation_invalid, std::string{message}, plan.manifest_path.string()));
    };
    const auto prefab_invalid = [&](const std::string_view message) -> Result<void> {
        return std::unexpected(game_error(
            DiagnosticCode::game_prefab_invalid, std::string{message}, plan.manifest_path.string()));
    };
    const auto save_invalid = [&](const std::string_view message) -> Result<void> {
        return std::unexpected(game_error(
            DiagnosticCode::game_save_invalid, std::string{message}, plan.manifest_path.string()));
    };
    const auto tile_field_invalid = [&](const std::string_view message) -> Result<void> {
        return std::unexpected(game_error(
            DiagnosticCode::game_tile_field_invalid, std::string{message}, plan.manifest_path.string()));
    };
    const auto input_profile_invalid = [&](const std::string_view message) -> Result<void> {
        return std::unexpected(game_error(
            DiagnosticCode::game_input_profile_invalid, std::string{message}, plan.manifest_path.string()));
    };
    const auto localization_invalid = [&](const std::string_view message) -> Result<void> {
        return std::unexpected(game_error(
            DiagnosticCode::game_localization_invalid, std::string{message}, plan.manifest_path.string()));
    };
    const auto presentation_invalid = [&](const std::string_view message) -> Result<void> {
        return std::unexpected(game_error(
            DiagnosticCode::game_presentation_invalid, std::string{message}, plan.manifest_path.string()));
    };
    const auto valid_symbol = [&](const SymbolId id) noexcept {
        return id < plan.symbols.size() && !plan.symbols[id].empty();
    };
    const auto finite_vec2 = [](const Vec2 value) noexcept {
        return std::isfinite(value.x) && std::isfinite(value.y);
    };
    const auto finite_rect = [&](const Rect& value) noexcept {
        return finite_vec2(value.min) && finite_vec2(value.max);
    };
    const auto valid_color = [](const Color& color) noexcept {
        return std::isfinite(color.r) && std::isfinite(color.g) && std::isfinite(color.b) &&
               std::isfinite(color.a) && color.r >= 0.0F && color.r <= 1.0F && color.g >= 0.0F &&
               color.g <= 1.0F && color.b >= 0.0F && color.b <= 1.0F && color.a >= 0.0F &&
               color.a <= 1.0F;
    };
    const auto valid_uv = [&](const Rect& uv, const bool require_positive_extent) noexcept {
        return finite_rect(uv) && uv.min.x >= 0.0F && uv.min.y >= 0.0F &&
               (require_positive_extent ? uv.max.x > 0.0F : uv.max.x >= 0.0F) &&
               (require_positive_extent ? uv.max.y > 0.0F : uv.max.y >= 0.0F) &&
               uv.min.x + uv.max.x <= 1.0001F && uv.min.y + uv.max.y <= 1.0001F;
    };
    const auto decimal_width = [](const std::int32_t value) noexcept {
        std::int64_t signed_value = value;
        std::size_t width = signed_value < 0 ? 1U : 0U;
        std::uint64_t magnitude = signed_value < 0
                                      ? static_cast<std::uint64_t>(-signed_value)
                                      : static_cast<std::uint64_t>(signed_value);
        do {
            ++width;
            magnitude /= 10U;
        } while (magnitude != 0U);
        return width;
    };
    const bool version_0_3 = plan.schema_version != GameSchemaVersion::v0_2;
    const bool version_0_4 = plan.schema_version == GameSchemaVersion::v0_4 ||
                             plan.schema_version == GameSchemaVersion::v0_5 ||
                             plan.schema_version == GameSchemaVersion::v0_6;
    const bool version_0_5 = plan.schema_version == GameSchemaVersion::v0_5 ||
                             plan.schema_version == GameSchemaVersion::v0_6;
    const bool version_0_6 = plan.schema_version == GameSchemaVersion::v0_6;
    if (static_cast<std::uint32_t>(plan.schema_version) > static_cast<std::uint32_t>(GameSchemaVersion::v0_6) ||
        (!version_0_3 && plan.seed != 0U)) {
        return invalid("GamePlan schema version or seed is invalid");
    }
    if (plan.symbols.empty() || !valid_symbol(plan.name) || !valid_symbol(plan.organization) ||
        !valid_symbol(plan.application) || !valid_symbol(plan.window.title) ||
        !safe_preference_component(plan.symbol(plan.organization)) ||
        !safe_preference_component(plan.symbol(plan.application))) {
        return invalid("GamePlan contains an invalid symbol reference");
    }
    std::unordered_set<std::string_view> unique_symbols{};
    unique_symbols.reserve(plan.symbols.size());
    for (const auto& symbol : plan.symbols) {
        if (symbol.empty() || !unique_symbols.insert(symbol).second) {
            return invalid("GamePlan symbol table is not canonical");
        }
    }
    if (plan.window.width == 0U || plan.window.height == 0U || plan.window.width > 16'384U ||
        plan.window.height > 16'384U || plan.window.virtual_width == 0U || plan.window.virtual_height == 0U ||
        plan.window.virtual_width > 16'384U || plan.window.virtual_height > 16'384U ||
        static_cast<std::uint32_t>(plan.window.default_fps) > static_cast<std::uint32_t>(RenderFpsCap::unlimited)) {
        return invalid("GamePlan window dimensions are invalid");
    }
    if (plan.assets.empty() || plan.assets.size() > GamePlan::max_assets || plan.actions.empty() ||
        plan.actions.size() > GamePlan::max_actions || plan.states.size() > GamePlan::max_states ||
        plan.scenes.empty() || plan.scenes.size() > GamePlan::max_scenes ||
        plan.transitions.size() > GamePlan::max_transitions ||
        plan.fps_actions.size() > GamePlan::max_fps_actions || plan.fps_actions.size() > plan.actions.size() ||
        plan.animations.size() > GamePlan::max_animations || plan.prefabs.size() > GamePlan::max_prefabs ||
        plan.localizations.size() > GamePlan::max_localizations ||
        plan.input_profiles.size() > GamePlan::max_input_profiles ||
        (!version_0_6 && (!plan.animations.empty() || !plan.prefabs.empty() || !plan.localizations.empty() ||
                         !plan.input_profiles.empty() || plan.save.enabled)) ||
        plan.start_scene >= plan.scenes.size()) {
        return invalid("GamePlan collection sizes are invalid");
    }
    std::error_code error{};
    const auto canonical_root = std::filesystem::weakly_canonical(plan.content_root, error);
    if (plan.content_root.empty() || error || canonical_root != plan.content_root.lexically_normal() ||
        !std::filesystem::is_directory(canonical_root, error) || error) {
        return invalid("GamePlan content root is invalid");
    }
    if (!canonical_content_file(plan.content_root, plan.manifest_path)) {
        return invalid("GamePlan manifest path is invalid");
    }
    std::unordered_set<SymbolId> asset_ids{};
    for (const auto& asset : plan.assets) {
        if (!valid_symbol(asset.symbol) ||
            static_cast<std::uint32_t>(asset.kind) >
                static_cast<std::uint32_t>(version_0_6 ? GameAssetKind::music : GameAssetKind::font) ||
            !canonical_content_file(plan.content_root, asset.path) ||
            !asset_ids.insert(asset.symbol).second) {
            return invalid("GamePlan contains an invalid asset");
        }
        if (asset.kind == GameAssetKind::font) {
            if (asset.glyphs.empty() || asset.glyphs.size() > GameAssetPlan::max_glyphs ||
                !std::isfinite(asset.line_height) || asset.line_height <= 0.0F ||
                !canonical_content_file(plan.content_root, asset.metadata_path)) {
                return invalid("GamePlan contains invalid font metadata");
            }
            std::uint32_t previous_codepoint = 0U;
            bool first_glyph = true;
            for (const auto& glyph : asset.glyphs) {
                if (glyph.codepoint == 0U || glyph.codepoint > 0x10FFFFU ||
                    (!first_glyph && glyph.codepoint <= previous_codepoint) || !valid_uv(glyph.uv, false) ||
                    !finite_vec2(glyph.size) || !finite_vec2(glyph.bearing) || !std::isfinite(glyph.advance) ||
                    glyph.size.x < 0.0F || glyph.size.y < 0.0F || glyph.advance < 0.0F) {
                    return invalid("GamePlan contains invalid font glyph metadata");
                }
                previous_codepoint = glyph.codepoint;
                first_glyph = false;
            }
        } else if (!asset.glyphs.empty() || !asset.metadata_path.empty() || asset.line_height != 0.0F) {
            return invalid("Non-font GamePlan asset contains font metadata");
        }
    }

    std::unordered_set<SymbolId> animation_ids{};
    for (const auto& animation : plan.animations) {
        if (!version_0_6 || !valid_symbol(animation.symbol) || !animation_ids.insert(animation.symbol).second ||
            animation.asset_index >= plan.assets.size() ||
            (plan.assets[animation.asset_index].kind != GameAssetKind::png &&
             plan.assets[animation.asset_index].kind != GameAssetKind::font) ||
            static_cast<std::uint32_t>(animation.mode) > static_cast<std::uint32_t>(GameAnimationMode::ping_pong) ||
            animation.frames.empty() || animation.frames.size() > GameAnimationPlan::max_frames) {
            return animation_invalid("GamePlan contains an invalid animation clip");
        }
        for (const auto& frame : animation.frames) {
            if (!valid_uv(frame.uv, true) || frame.duration_ticks == 0U || frame.duration_ticks > 1'000'000U) {
                return animation_invalid("GamePlan contains an invalid animation frame");
            }
        }
    }

    std::unordered_set<SymbolId> prefab_ids{};
    for (const auto& prefab : plan.prefabs) {
        if (!version_0_6 || !valid_symbol(prefab.symbol) || !prefab_ids.insert(prefab.symbol).second ||
            !canonical_content_file(plan.content_root, prefab.path) ||
            (!prefab.has_transform && !prefab.has_velocity && !prefab.has_sprite && !prefab.has_collider &&
             !prefab.has_animation) ||
            (prefab.has_transform &&
             (!finite_vec2(prefab.transform.position_offset) || !std::isfinite(prefab.transform.rotation) ||
              !finite_vec2(prefab.transform.scale) || prefab.transform.scale.x <= 0.0F ||
              prefab.transform.scale.y <= 0.0F)) ||
            (prefab.has_velocity && (!finite_vec2(prefab.velocity.linear) || !std::isfinite(prefab.velocity.angular))) ||
            (prefab.has_sprite &&
             (prefab.sprite.asset_index >= plan.assets.size() || !finite_vec2(prefab.sprite.size) ||
              (prefab.sprite.asset_index < plan.assets.size() &&
               plan.assets[prefab.sprite.asset_index].kind != GameAssetKind::png &&
               plan.assets[prefab.sprite.asset_index].kind != GameAssetKind::font) ||
              prefab.sprite.size.x <= 0.0F || prefab.sprite.size.y <= 0.0F || !finite_vec2(prefab.sprite.pivot) ||
              !valid_uv(prefab.sprite.uv, true) || !valid_color(prefab.sprite.tint))) ||
            (prefab.has_collider &&
             (!valid_symbol(prefab.collider.group) || !finite_vec2(prefab.collider.offset) ||
              !finite_vec2(prefab.collider.half_extent) || prefab.collider.half_extent.x <= 0.0F ||
              prefab.collider.half_extent.y <= 0.0F || prefab.collider.trigger ||
              static_cast<std::uint32_t>(prefab.collider.motion) >
                  static_cast<std::uint32_t>(GameBodyMotion::dynamic_body))) ||
            (prefab.has_animation && (!prefab.has_sprite || prefab.animation_index >= plan.animations.size()))) {
            return prefab_invalid("GamePlan contains an invalid prefab");
        }
    }

    std::unordered_set<SymbolId> locale_ids{};
    std::vector<SymbolId> localization_keys{};
    for (std::size_t locale_index = 0U; locale_index < plan.localizations.size(); ++locale_index) {
        const auto& locale = plan.localizations[locale_index];
        if (!version_0_6 || !valid_symbol(locale.locale) || !locale_ids.insert(locale.locale).second ||
            !canonical_content_file(plan.content_root, locale.path) || locale.entries.empty() ||
            locale.entries.size() > 4'096U ||
            (locale_index != 0U && locale.entries.size() != localization_keys.size())) {
            return localization_invalid("GamePlan contains an invalid localization table");
        }
        std::unordered_set<SymbolId> entry_ids{};
        for (std::size_t entry_index = 0U; entry_index < locale.entries.size(); ++entry_index) {
            const auto& entry = locale.entries[entry_index];
            if (!valid_symbol(entry.key) || !valid_symbol(entry.value) || !entry_ids.insert(entry.key).second ||
                (locale_index != 0U && entry.key != localization_keys[entry_index])) {
                return localization_invalid("GamePlan localization keys are invalid or inconsistent");
            }
            if (locale_index == 0U) localization_keys.push_back(entry.key);
        }
    }
    if ((plan.localizations.empty() && plan.default_locale_index != 0U) ||
        (!plan.localizations.empty() && plan.default_locale_index >= plan.localizations.size())) {
        return localization_invalid("GamePlan default locale is invalid");
    }
    std::unordered_set<SymbolId> action_ids{};
    const auto validate_action_bindings = [&](const ActionPlan& action) noexcept {
        if (!valid_symbol(action.symbol) || action.key_count > ActionPlan::max_keys ||
            action.gamepad_button_count > ActionPlan::max_gamepad_buttons ||
            action.gamepad_axis_count > ActionPlan::max_gamepad_axes) {
            return false;
        }
        std::uint32_t key_mask = 0U;
        for (std::uint32_t key_index = 0U; key_index < action.key_count; ++key_index) {
            const auto raw_key = static_cast<std::uint32_t>(action.keys[key_index]);
            if (raw_key > static_cast<std::uint32_t>(GameKey::left_control) ||
                (key_mask & (1U << raw_key)) != 0U) {
                return false;
            }
            key_mask |= 1U << raw_key;
        }
        std::uint32_t button_mask = 0U;
        for (std::uint32_t button_index = 0U; button_index < action.gamepad_button_count; ++button_index) {
            const auto raw_button = static_cast<std::uint32_t>(action.gamepad_buttons[button_index]);
            if (raw_button > static_cast<std::uint32_t>(GamepadButton::dpad_right) ||
                (button_mask & (1U << raw_button)) != 0U) {
                return false;
            }
            button_mask |= 1U << raw_button;
        }
        std::uint32_t axis_direction_mask = 0U;
        for (std::uint32_t axis_index = 0U; axis_index < action.gamepad_axis_count; ++axis_index) {
            const auto& binding = action.gamepad_axes[axis_index];
            const auto raw_axis = static_cast<std::uint32_t>(binding.axis);
            if (raw_axis > static_cast<std::uint32_t>(GamepadAxis::right_trigger) ||
                (binding.direction != -1 && binding.direction != 1) || !std::isfinite(binding.deadzone) ||
                binding.deadzone <= 0.0F || binding.deadzone > 0.95F) {
                return false;
            }
            const auto bit = raw_axis * 2U + (binding.direction > 0 ? 1U : 0U);
            if ((axis_direction_mask & (1U << bit)) != 0U) return false;
            axis_direction_mask |= 1U << bit;
        }
        return true;
    };
    for (const auto& action : plan.actions) {
        if (!validate_action_bindings(action) || action.gamepad_button_count != 0U ||
            action.gamepad_axis_count != 0U) {
            return invalid("GamePlan contains an invalid action");
        }
        if (!action_ids.insert(action.symbol).second) return invalid("GamePlan contains duplicate action ids");
    }
    std::unordered_set<SymbolId> profile_ids{};
    for (const auto& profile : plan.input_profiles) {
        if (!version_0_6 || !valid_symbol(profile.symbol) || !profile_ids.insert(profile.symbol).second ||
            profile.actions.size() != plan.actions.size()) {
            return input_profile_invalid("GamePlan contains an invalid input profile");
        }
        for (std::size_t action_index = 0U; action_index < profile.actions.size(); ++action_index) {
            if (profile.actions[action_index].symbol != plan.actions[action_index].symbol ||
                !validate_action_bindings(profile.actions[action_index])) {
                return input_profile_invalid("GamePlan input profile bindings are invalid");
            }
        }
    }
    if ((version_0_6 && (plan.input_profiles.empty() ||
                         plan.default_input_profile_index >= plan.input_profiles.size())) ||
        (!version_0_6 && plan.default_input_profile_index != 0U)) {
        return input_profile_invalid("GamePlan default input profile is invalid");
    }
    std::unordered_set<SymbolId> state_ids{};
    std::unordered_map<std::string, std::uint32_t> state_names{};
    state_names.reserve(plan.states.size());
    for (std::size_t state_index = 0U; state_index < plan.states.size(); ++state_index) {
        const auto& state = plan.states[state_index];
        if (!valid_symbol(state.symbol) || state.minimum > state.initial || state.initial > state.maximum) {
            return invalid("GamePlan contains an invalid integer state");
        }
        if (!state_ids.insert(state.symbol).second) return invalid("GamePlan contains duplicate state ids");
        state_names.emplace(std::string{plan.symbol(state.symbol)}, static_cast<std::uint32_t>(state_index));
    }
    if ((!version_0_6 && (plan.save.enabled || plan.save.slot_count != 0U || !plan.save.state_indices.empty())) ||
        (plan.save.enabled && (plan.save.slot_count == 0U || plan.save.slot_count > 16U)) ||
        (!plan.save.enabled && (plan.save.slot_count != 0U || !plan.save.state_indices.empty()))) {
        return save_invalid("GamePlan save configuration is invalid");
    }
    std::unordered_set<std::uint32_t> save_states{};
    for (const auto state_index : plan.save.state_indices) {
        if (state_index >= plan.states.size() || !save_states.insert(state_index).second) {
            return save_invalid("GamePlan save state list is invalid");
        }
    }
    std::unordered_set<SymbolId> scene_ids{};
    std::uint64_t aggregate_contact_pairs = 0U;
    std::uint64_t aggregate_game_tile_field_cells = 0U;
    std::uint64_t aggregate_game_particle_slots = 0U;
    for (const auto& scene : plan.scenes) {
        if (!valid_symbol(scene.symbol) || scene.world_capacity == 0U ||
            scene.world_capacity > GameScenePlan::max_world_capacity ||
            scene.total_spawn_count > scene.world_capacity || !finite_vec2(scene.camera_position) ||
            !finite_vec2(scene.camera_half_extent) || scene.camera_half_extent.x <= 0.0F ||
            scene.camera_half_extent.y <= 0.0F || !finite_rect(scene.collision_bounds) ||
            scene.collision_bounds.max.x <= scene.collision_bounds.min.x ||
            scene.collision_bounds.max.y <= scene.collision_bounds.min.y ||
            !finite_vec2(scene.collision_cell_size) || scene.collision_cell_size.x <= 0.0F ||
            scene.collision_cell_size.y <= 0.0F || scene.max_colliders == 0U ||
            scene.max_colliders > GameScenePlan::max_world_capacity || scene.max_grid_references == 0U ||
            scene.max_grid_references > GameScenePlan::max_collision_capacity ||
            scene.max_candidate_pairs == 0U ||
            scene.max_candidate_pairs > GameScenePlan::max_collision_capacity || scene.max_impacts == 0U ||
            scene.max_impacts > GameScenePlan::max_impacts_per_dynamic ||
            (!version_0_5 && scene.max_contact_pairs != 0U) ||
            scene.grids.size() > GameScenePlan::max_grids ||
            scene.pools.size() > GameScenePlan::max_pools ||
            scene.spawn_groups.size() > GameScenePlan::max_spawn_groups ||
            scene.systems.size() > GameScenePlan::max_systems ||
            scene.collision_rules.size() > GameScenePlan::max_collision_rules ||
            scene.ui.size() > GameScenePlan::max_ui_elements ||
            scene.rules.size() > GameScenePlan::max_rules ||
            scene.tile_layers.size() > 64U || scene.fields.size() > 64U ||
            scene.particle_emitters.size() > 64U || scene.ui_stacks.size() > 128U ||
            (!version_0_3 && (!scene.grids.empty() || !scene.rules.empty())) ||
            (!version_0_4 && !scene.pools.empty()) ||
            (!version_0_6 && (!scene.tile_layers.empty() || !scene.fields.empty() ||
                             !scene.particle_emitters.empty() || !scene.ui_stacks.empty() || scene.persistent))) {
            return invalid("GamePlan contains an invalid scene configuration");
        }
        if (version_0_6 &&
            (static_cast<std::uint32_t>(scene.camera.mode) > static_cast<std::uint32_t>(GameCameraMode::follow) ||
             !finite_vec2(scene.camera.position) || !finite_vec2(scene.camera.half_extent) ||
             scene.camera.half_extent.x <= 0.0F || scene.camera.half_extent.y <= 0.0F ||
             !finite_vec2(scene.camera.follow_offset) ||
             (scene.camera.mode == GameCameraMode::follow &&
              (scene.camera.follow_group_index >= scene.spawn_groups.size() ||
               scene.camera.follow_item_index >=
                   scene.spawn_groups[scene.camera.follow_group_index].count)) ||
             (scene.camera.has_bounds &&
              (!finite_rect(scene.camera.bounds) || scene.camera.bounds.max.x < scene.camera.bounds.min.x ||
               scene.camera.bounds.max.y < scene.camera.bounds.min.y)))) {
            return presentation_invalid("GamePlan contains an invalid camera configuration");
        }
        if (version_0_5 &&
            (scene.max_contact_pairs > 100'000U || scene.max_contact_pairs > scene.max_candidate_pairs)) {
            return interaction_invalid("GamePlan contains an invalid contact capacity");
        }
        aggregate_contact_pairs += scene.max_contact_pairs;
        if (aggregate_contact_pairs > 1'000'000U) {
            return interaction_invalid("GamePlan aggregate contact capacity exceeds one million pairs");
        }
        const double collision_width =
            static_cast<double>(scene.collision_bounds.max.x) - scene.collision_bounds.min.x;
        const double collision_height =
            static_cast<double>(scene.collision_bounds.max.y) - scene.collision_bounds.min.y;
        const double column_count = std::ceil(collision_width / scene.collision_cell_size.x);
        const double row_count = std::ceil(collision_height / scene.collision_cell_size.y);
        if (!std::isfinite(column_count) || !std::isfinite(row_count) || column_count < 1.0 || row_count < 1.0 ||
            column_count > static_cast<double>(GameScenePlan::max_collision_cells) ||
            row_count > static_cast<double>(GameScenePlan::max_collision_cells) ||
            column_count * row_count > static_cast<double>(GameScenePlan::max_collision_cells)) {
            return invalid("GamePlan collision grid dimensions are unsupported");
        }
        if (!scene_ids.insert(scene.symbol).second ||
            !canonical_content_file(plan.content_root, scene.source_path)) {
            return invalid("GamePlan contains a duplicate scene id or invalid scene path");
        }
        std::unordered_set<SymbolId> grid_ids{};
        for (const auto& grid : scene.grids) {
            if (!valid_symbol(grid.symbol) || !grid_ids.insert(grid.symbol).second ||
                !finite_vec2(grid.first_cell_center) || !finite_vec2(grid.cell_size) ||
                grid.cell_size.x <= 0.0F || grid.cell_size.y <= 0.0F || grid.columns == 0U || grid.rows == 0U ||
                static_cast<std::uint64_t>(grid.columns) * grid.rows > GameGridPlan::max_cells ||
                !grid_positions_representable(grid)) {
                return invalid("GamePlan contains an invalid logical grid");
            }
        }
        std::uint64_t aggregate_tile_field_cells = 0U;
        std::unordered_set<SymbolId> tile_layer_ids{};
        for (const auto& layer : scene.tile_layers) {
            if (!version_0_6 || !valid_symbol(layer.symbol) || !tile_layer_ids.insert(layer.symbol).second ||
                layer.grid_index >= scene.grids.size() || layer.asset_index >= plan.assets.size() ||
                (plan.assets[layer.asset_index].kind != GameAssetKind::png &&
                 plan.assets[layer.asset_index].kind != GameAssetKind::font) ||
                layer.atlas_columns == 0U || layer.atlas_columns > 256U || layer.atlas_rows == 0U ||
                layer.atlas_rows > 256U ||
                static_cast<std::uint64_t>(layer.atlas_columns) * layer.atlas_rows >
                    std::numeric_limits<std::uint16_t>::max() ||
                layer.initial_cells.size() !=
                    static_cast<std::size_t>(scene.grids[layer.grid_index].columns) *
                        scene.grids[layer.grid_index].rows ||
                !valid_color(layer.tint) || layer.layer < -1'000'000 || layer.layer > 1'000'000) {
                return tile_field_invalid("GamePlan contains an invalid tile layer");
            }
            const auto atlas_count = layer.atlas_columns * layer.atlas_rows;
            for (const auto value : layer.initial_cells) {
                if (value > atlas_count) return tile_field_invalid("GamePlan tile value exceeds its atlas");
            }
            aggregate_tile_field_cells += layer.initial_cells.size();
        }
        std::unordered_set<SymbolId> field_ids{};
        for (const auto& field : scene.fields) {
            if (!version_0_6 || !valid_symbol(field.symbol) || !field_ids.insert(field.symbol).second ||
                field.grid_index >= scene.grids.size() || field.minimum > field.maximum ||
                field.initial_cells.size() !=
                    static_cast<std::size_t>(scene.grids[field.grid_index].columns) *
                        scene.grids[field.grid_index].rows) {
                return tile_field_invalid("GamePlan contains an invalid integer field");
            }
            for (const auto value : field.initial_cells) {
                if (value < field.minimum || value > field.maximum) {
                    return tile_field_invalid("GamePlan field value is outside its declared range");
                }
            }
            aggregate_tile_field_cells += field.initial_cells.size();
        }
        if (aggregate_tile_field_cells > 4'000'000U) {
            return tile_field_invalid("GamePlan tile and field cells exceed the scene limit");
        }
        aggregate_game_tile_field_cells += aggregate_tile_field_cells;
        if (aggregate_game_tile_field_cells > GamePlan::max_total_tile_field_cells) {
            return tile_field_invalid("GamePlan tile and field cells exceed the game-wide limit");
        }
        std::uint32_t aggregate_particles = 0U;
        std::unordered_set<SymbolId> particle_ids{};
        for (const auto& emitter : scene.particle_emitters) {
            if (!version_0_6 || !valid_symbol(emitter.symbol) || !particle_ids.insert(emitter.symbol).second ||
                emitter.capacity == 0U || emitter.capacity > 10'000U ||
                aggregate_particles > 100'000U - emitter.capacity ||
                static_cast<std::uint32_t>(emitter.on_exhausted) >
                    static_cast<std::uint32_t>(GamePoolExhaustionPolicy::recycle_oldest) ||
                emitter.sprite.asset_index >= plan.assets.size() ||
                (plan.assets[emitter.sprite.asset_index].kind != GameAssetKind::png &&
                 plan.assets[emitter.sprite.asset_index].kind != GameAssetKind::font) ||
                !finite_vec2(emitter.sprite.size) || emitter.sprite.size.x <= 0.0F ||
                emitter.sprite.size.y <= 0.0F || !finite_vec2(emitter.sprite.pivot) ||
                !valid_uv(emitter.sprite.uv, true) || !valid_color(emitter.sprite.tint) ||
                emitter.lifetime_min_ticks == 0U || emitter.lifetime_min_ticks > emitter.lifetime_max_ticks ||
                emitter.lifetime_max_ticks > 1'000'000U || !finite_vec2(emitter.velocity_min) ||
                !finite_vec2(emitter.velocity_max) || emitter.velocity_min.x > emitter.velocity_max.x ||
                emitter.velocity_min.y > emitter.velocity_max.y) {
                return presentation_invalid("GamePlan contains an invalid particle emitter");
            }
            aggregate_particles += emitter.capacity;
        }
        aggregate_game_particle_slots += aggregate_particles;
        if (aggregate_game_particle_slots > GamePlan::max_total_particle_slots) {
            return presentation_invalid("GamePlan particle slots exceed the game-wide limit");
        }
        std::uint32_t spawn_total = 0U;
        std::uint32_t collider_total = 0U;
        std::unordered_set<SymbolId> spawn_ids{};
        std::unordered_map<SymbolId, GameBodyMotion> group_motions{};
        std::unordered_map<SymbolId, bool> groups_all_have_velocity{};
        std::uint64_t render_submission_count = 0U;
        for (const auto& spawn : scene.spawn_groups) {
            if (version_0_5 && spawn.has_collider && spawn.collider.trigger) {
                return interaction_invalid("GamePlan 0.5 collider contains the removed trigger field");
            }
            if (!valid_symbol(spawn.symbol) || spawn.count == 0U || spawn.count > GameSpawnGroupPlan::max_count ||
                spawn.active_count > spawn.count || (!version_0_3 && spawn.active_count != spawn.count) ||
                spawn.placement.columns == 0U || spawn.placement.columns > GamePlacementPlan::max_columns ||
                spawn.placement.kind != PlacementId::grid ||
                !finite_vec2(spawn.placement.origin) || !finite_vec2(spawn.placement.spacing) ||
                !finite_vec2(spawn.transform.position_offset) || !std::isfinite(spawn.transform.rotation) ||
                !finite_vec2(spawn.transform.scale) || spawn.transform.scale.x <= 0.0F ||
                spawn.transform.scale.y <= 0.0F || !spawn_positions_representable(spawn) ||
                (spawn.has_velocity &&
                 (!finite_vec2(spawn.velocity.linear) || !std::isfinite(spawn.velocity.angular))) ||
                 (spawn.has_sprite && spawn.sprite.asset_index >= plan.assets.size()) ||
                 (spawn.has_initial_animation &&
                  (!version_0_6 || spawn.initial_animation_index >= plan.animations.size() || !spawn.has_sprite)) ||
                (spawn.has_sprite &&
                 (!finite_vec2(spawn.sprite.size) || spawn.sprite.size.x <= 0.0F || spawn.sprite.size.y <= 0.0F ||
                  !finite_vec2(spawn.sprite.pivot) || !valid_uv(spawn.sprite.uv, true) ||
                  !valid_color(spawn.sprite.tint) || spawn.sprite.layer < -1'000'000 ||
                  spawn.sprite.layer > 1'000'000)) ||
                (spawn.has_collider &&
                 (!valid_symbol(spawn.collider.group) ||
                  static_cast<std::uint32_t>(spawn.collider.motion) >
                      static_cast<std::uint32_t>(GameBodyMotion::dynamic_body) ||
                  !finite_vec2(spawn.collider.offset) || !finite_vec2(spawn.collider.half_extent) ||
                  spawn.collider.half_extent.x <= 0.0F || spawn.collider.half_extent.y <= 0.0F ||
                  (version_0_5 && spawn.collider.trigger)))) {
                return invalid("GamePlan contains an invalid spawn group");
            }
            if (!spawn_ids.insert(spawn.symbol).second || spawn_total > scene.world_capacity - spawn.count) {
                return invalid("GamePlan contains duplicate spawn ids or an overflowing spawn count");
            }
            spawn_total += spawn.count;
            if (spawn.has_sprite && spawn.sprite.visible) render_submission_count += spawn.count;
            if (spawn.has_sprite &&
                (plan.assets[spawn.sprite.asset_index].kind == GameAssetKind::wav ||
                 plan.assets[spawn.sprite.asset_index].kind == GameAssetKind::music)) {
                return invalid("GamePlan sprite references an audio asset");
            }
            if (spawn.has_collider) {
                collider_total += spawn.count;
                const auto velocity = groups_all_have_velocity.emplace(spawn.collider.group, spawn.has_velocity);
                if (!velocity.second) velocity.first->second = velocity.first->second && spawn.has_velocity;
                const auto inserted = group_motions.emplace(spawn.collider.group, spawn.collider.motion);
                if (!inserted.second && inserted.first->second != spawn.collider.motion) {
                    return invalid("GamePlan collider group mixes body motion kinds");
                }
            }
        }
        if (spawn_total != scene.total_spawn_count || collider_total > scene.max_colliders) {
            return invalid("GamePlan scene count metadata is inconsistent");
        }
        render_submission_count += aggregate_particles;
        for (const auto& layer : scene.tile_layers) {
            if (layer.visible) render_submission_count += layer.initial_cells.size();
        }
        std::vector<std::int32_t> pool_for_spawn_group(scene.spawn_groups.size(), -1);
        std::unordered_set<SymbolId> pool_ids{};
        for (std::size_t pool_index = 0U; pool_index < scene.pools.size(); ++pool_index) {
            const auto& pool = scene.pools[pool_index];
            if (!version_0_4 || !valid_symbol(pool.symbol) || !pool_ids.insert(pool.symbol).second ||
                pool.spawn_group_index >= scene.spawn_groups.size() ||
                static_cast<std::uint32_t>(pool.on_exhausted) >
                    static_cast<std::uint32_t>(GamePoolExhaustionPolicy::recycle_oldest) ||
                pool_for_spawn_group[pool.spawn_group_index] >= 0) {
                return pool_invalid("GamePlan contains an invalid or multiply-owned pool");
            }
            pool_for_spawn_group[pool.spawn_group_index] = static_cast<std::int32_t>(pool_index);
        }
        const auto collider_group_contains_pool = [&](const SymbolId collider_group) {
            for (std::size_t group_index = 0U; group_index < scene.spawn_groups.size(); ++group_index) {
                const auto& spawn = scene.spawn_groups[group_index];
                if (pool_for_spawn_group[group_index] >= 0 && spawn.has_collider &&
                    spawn.collider.group == collider_group) {
                    return true;
                }
            }
            return false;
        };
        const auto group_aligned_to_grid = [&](const std::uint32_t group_index,
                                               const std::uint32_t grid_index) noexcept {
            if (group_index >= scene.spawn_groups.size() || grid_index >= scene.grids.size()) return false;
            const auto& group = scene.spawn_groups[group_index];
            const auto& grid = scene.grids[grid_index];
            constexpr double epsilon = 0.0001;
            for (std::uint32_t item = 0U; item < group.count; ++item) {
                const auto column = item % group.placement.columns;
                const auto row = item / group.placement.columns;
                const double x = static_cast<double>(group.placement.origin.x) +
                                 static_cast<double>(column) * group.placement.spacing.x +
                                 group.transform.position_offset.x;
                const double y = static_cast<double>(group.placement.origin.y) +
                                 static_cast<double>(row) * group.placement.spacing.y +
                                 group.transform.position_offset.y;
                const double grid_x = (x - grid.first_cell_center.x) / grid.cell_size.x;
                const double grid_y = (y - grid.first_cell_center.y) / grid.cell_size.y;
                const auto rounded_x = std::round(grid_x);
                const auto rounded_y = std::round(grid_y);
                if (!std::isfinite(grid_x) || !std::isfinite(grid_y) ||
                    std::abs(grid_x - rounded_x) > epsilon || std::abs(grid_y - rounded_y) > epsilon ||
                    rounded_x < 0.0 || rounded_y < 0.0 || rounded_x >= grid.columns || rounded_y >= grid.rows) {
                    return false;
                }
            }
            return true;
        };
        std::unordered_set<SymbolId> system_ids{};
        bool has_collision_system = false;
        std::size_t collision_system_index = scene.systems.size();
        std::uint32_t collision_system_count = 0U;
        std::unordered_set<std::uint32_t> followed_groups{};
        std::unordered_set<std::uint32_t> linear_groups{};
        std::vector<std::size_t> linear_index_by_group(scene.spawn_groups.size(), scene.systems.size());
        for (std::size_t system_index = 0U; system_index < scene.systems.size(); ++system_index) {
            const auto& system = scene.systems[system_index];
            if (!valid_symbol(system.symbol) ||
                static_cast<std::uint32_t>(system.operation) >
                    static_cast<std::uint32_t>(version_0_5 ? GameOperationId::linear_motion
                                                          : version_0_3 ? GameOperationId::follow_transform_chain
                                                          : GameOperationId::simulate_collisions) ||
                (system.operation == GameOperationId::axis_control &&
                 (system.negative_action >= plan.actions.size() || system.positive_action >= plan.actions.size())) ||
                (system.operation == GameOperationId::set_velocity_on_press && system.action >= plan.actions.size())) {
                return invalid("GamePlan contains an invalid system");
            }
            if (!system_ids.insert(system.symbol).second) return invalid("GamePlan contains duplicate system ids");
            has_collision_system = has_collision_system || system.operation == GameOperationId::simulate_collisions;
            if (system.operation == GameOperationId::simulate_collisions) {
                collision_system_index = system_index;
                ++collision_system_count;
            }
            if (system.operation == GameOperationId::axis_control &&
                (!group_motions.contains(system.group) || group_motions[system.group] != GameBodyMotion::kinematic_body ||
                 !groups_all_have_velocity[system.group] || !std::isfinite(system.speed) || system.speed <= 0.0F ||
                 !std::isfinite(system.minimum) || !std::isfinite(system.maximum) || system.minimum >= system.maximum)) {
                return invalid("GamePlan axis_control system is inconsistent with its group");
            }
            if (system.operation == GameOperationId::set_velocity_on_press &&
                (!group_motions.contains(system.group) || group_motions[system.group] != GameBodyMotion::dynamic_body ||
                 !groups_all_have_velocity[system.group] || !finite_vec2(system.velocity))) {
                return invalid("GamePlan velocity-on-press system is inconsistent with its group");
            }
            if (system.operation == GameOperationId::grid_motion &&
                (system.spawn_group_index >= scene.spawn_groups.size() || system.grid_index >= scene.grids.size() ||
                 system.step_interval_ticks == 0U || system.step_interval_ticks > 1'000'000U ||
                 system.initial_direction == GameDirection::none ||
                 static_cast<std::uint32_t>(system.initial_direction) >
                     static_cast<std::uint32_t>(GameDirection::right) ||
                 !scene.spawn_groups[system.spawn_group_index].has_velocity ||
                 !scene.spawn_groups[system.spawn_group_index].has_collider ||
                 (scene.spawn_groups[system.spawn_group_index].collider.motion != GameBodyMotion::dynamic_body &&
                  (!version_0_5 || scene.spawn_groups[system.spawn_group_index].collider.motion !=
                                        GameBodyMotion::kinematic_body)))) {
                return invalid("GamePlan grid_motion system is inconsistent");
            }
            if (system.operation == GameOperationId::grid_motion &&
                !group_aligned_to_grid(system.spawn_group_index, system.grid_index)) {
                return invalid("GamePlan grid_motion reserve entities are not aligned to the logical grid");
            }
            if (system.operation == GameOperationId::follow_transform_chain &&
                (system.leader_group_index >= scene.spawn_groups.size() ||
                 system.follower_group_index >= scene.spawn_groups.size() ||
                 system.motion_system_index >= system_index ||
                 scene.spawn_groups[system.leader_group_index].count != 1U ||
                  system.leader_group_index == system.follower_group_index ||
                 scene.systems[system.motion_system_index].operation != GameOperationId::grid_motion ||
                 scene.systems[system.motion_system_index].spawn_group_index != system.leader_group_index)) {
                return invalid("GamePlan follow_transform_chain system is inconsistent");
            }
            if (system.operation == GameOperationId::follow_transform_chain) {
                if (!followed_groups.insert(system.follower_group_index).second) {
                    return invalid("GamePlan follower group has multiple follow_transform_chain owners");
                }
                if (linear_groups.contains(system.follower_group_index)) {
                    return invalid("GamePlan linear_motion cannot own a follower group");
                }
                const auto grid_index = scene.systems[system.motion_system_index].grid_index;
                if (!group_aligned_to_grid(system.leader_group_index, grid_index) ||
                    !group_aligned_to_grid(system.follower_group_index, grid_index)) {
                    return invalid("GamePlan follow groups are not aligned to the motion grid");
                }
                if (version_0_4 &&
                    (pool_for_spawn_group[system.leader_group_index] >= 0 ||
                     pool_for_spawn_group[system.follower_group_index] >= 0)) {
                    return pool_invalid("GamePlan follow_transform_chain cannot own pooled groups");
                }
            }
            if (system.operation == GameOperationId::linear_motion) {
                if (system.spawn_group_index >= scene.spawn_groups.size() ||
                    !linear_groups.insert(system.spawn_group_index).second) {
                    return invalid("GamePlan linear_motion has an invalid or duplicate group owner");
                }
                const auto& spawn = scene.spawn_groups[system.spawn_group_index];
                if (!spawn.has_velocity ||
                    (spawn.has_collider && spawn.collider.motion != GameBodyMotion::kinematic_body)) {
                    return invalid("GamePlan linear_motion group is missing Velocity2D or is not kinematic");
                }
                linear_index_by_group[system.spawn_group_index] = system_index;
                if (followed_groups.contains(system.spawn_group_index)) {
                    return invalid("GamePlan linear_motion cannot own a follower group");
                }
                for (const auto& other : scene.systems) {
                    if (other.operation != GameOperationId::axis_control) continue;
                    if (spawn.has_collider && spawn.collider.group == other.group) {
                        return invalid("GamePlan linear_motion cannot overlap axis_control ownership");
                    }
                }
            }
        }
        if (version_0_3) {
            for (std::size_t system_index = 0U; system_index < scene.systems.size(); ++system_index) {
                const auto& system = scene.systems[system_index];
                const bool kinematic_grid = system.operation == GameOperationId::grid_motion &&
                                            scene.spawn_groups[system.spawn_group_index].collider.motion ==
                                                GameBodyMotion::kinematic_body;
                if (system.operation == GameOperationId::grid_motion && !kinematic_grid &&
                    (collision_system_count != 1U || system_index >= collision_system_index)) {
                    return invalid("GamePlan grid_motion must precede exactly one collision system");
                }
                if (kinematic_grid &&
                    (collision_system_count != 1U ||
                     linear_index_by_group[system.spawn_group_index] >= scene.systems.size() ||
                     system_index >= linear_index_by_group[system.spawn_group_index] ||
                     linear_index_by_group[system.spawn_group_index] >= collision_system_index)) {
                    return invalid("GamePlan kinematic grid/linear/collision order is invalid");
                }
                if (system.operation == GameOperationId::follow_transform_chain &&
                    (collision_system_count != 1U || system.motion_system_index >= collision_system_index ||
                     collision_system_index >= system_index)) {
                    return invalid("GamePlan follow system order is invalid");
                }
            }
        }
        if (!scene.collision_rules.empty() && !has_collision_system) {
            return invalid("GamePlan collision rules have no collision system");
        }
        std::unordered_set<std::uint64_t> collision_pairs{};
        std::unordered_set<SymbolId> collision_rule_ids{};
        bool has_trigger_rule = false;
        for (const auto& rule : scene.collision_rules) {
            if ((version_0_3 && (!valid_symbol(rule.symbol) || !collision_rule_ids.insert(rule.symbol).second)) ||
                (!version_0_3 && rule.symbol != 0U) ||
                !valid_symbol(rule.group_a) || !valid_symbol(rule.group_b) ||
                (plan.schema_version == GameSchemaVersion::v0_2 && rule.reactions.empty()) ||
                rule.reactions.size() > 8U || rule.group_a == rule.group_b ||
                !group_motions.contains(rule.group_a) || !group_motions.contains(rule.group_b)) {
                return invalid("GamePlan contains an invalid collision rule");
            }
            if (!version_0_5 && group_motions[rule.group_a] == GameBodyMotion::dynamic_body &&
                group_motions[rule.group_b] == GameBodyMotion::dynamic_body) {
                return invalid("GamePlan contains an unsupported dynamic-versus-dynamic rule");
            }
            const auto lower = std::min(rule.group_a, rule.group_b);
            const auto upper = std::max(rule.group_a, rule.group_b);
            const auto pair = (static_cast<std::uint64_t>(lower) << 32U) | upper;
            if (!collision_pairs.insert(pair).second && !version_0_5) {
                return invalid("GamePlan contains duplicate collision pairs");
            }
            if (version_0_5) {
                const auto motion_a = group_motions[rule.group_a];
                const auto motion_b = group_motions[rule.group_b];
                if (rule.interaction == GameCollisionInteraction::trigger) {
                    has_trigger_rule = true;
                    if (!rule.reactions.empty()) {
                        return interaction_invalid("GamePlan trigger rule contains reactions");
                    }
                } else if (rule.interaction == GameCollisionInteraction::solid) {
                    const bool exactly_one_dynamic =
                        (motion_a == GameBodyMotion::dynamic_body) != (motion_b == GameBodyMotion::dynamic_body);
                    const bool has_reflect = std::any_of(
                        rule.reactions.begin(), rule.reactions.end(), [](const GameReactionPlan& reaction) {
                            return reaction.kind == GameReactionKind::reflect;
                        });
                    if (!exactly_one_dynamic || !has_reflect) {
                        return interaction_invalid(
                            "GamePlan solid interaction body combination or reactions are invalid");
                    }
                } else {
                    return interaction_invalid("GamePlan v0.5 collision interaction is missing");
                }
            } else if (rule.interaction != GameCollisionInteraction::legacy) {
                return interaction_invalid("Legacy GamePlan contains a v0.5 collision interaction");
            }
            for (const auto& reaction : rule.reactions) {
                if (static_cast<std::uint32_t>(reaction.kind) > static_cast<std::uint32_t>(GameReactionKind::play_sound) ||
                    static_cast<std::uint32_t>(reaction.target) > static_cast<std::uint32_t>(GameReactionTarget::b) ||
                    (reaction.kind == GameReactionKind::add_int_state && reaction.state_index >= plan.states.size()) ||
                    (reaction.kind == GameReactionKind::play_sound && reaction.asset_index >= plan.assets.size()) ||
                    (reaction.kind == GameReactionKind::play_sound &&
                     plan.assets[reaction.asset_index].kind != GameAssetKind::wav) ||
                    (reaction.kind == GameReactionKind::reset_group &&
                     (!valid_symbol(reaction.group) || !group_motions.contains(reaction.group)))) {
                    return invalid("GamePlan contains an invalid collision reaction");
                }
                if (reaction.kind == GameReactionKind::reflect) {
                    const auto target_group = reaction.target == GameReactionTarget::a ? rule.group_a : rule.group_b;
                    if (group_motions[target_group] != GameBodyMotion::dynamic_body ||
                        !groups_all_have_velocity[target_group]) {
                        return invalid("GamePlan reflect target is not uniformly dynamic with Velocity2D");
                    }
                }
                if (version_0_4 && reaction.kind == GameReactionKind::deactivate) {
                    const auto target_group = reaction.target == GameReactionTarget::a ? rule.group_a : rule.group_b;
                    if (collider_group_contains_pool(target_group)) {
                        return pool_invalid("GamePlan legacy deactivate reaction targets a pooled group");
                    }
                }
                if (version_0_4 && reaction.kind == GameReactionKind::reset_group &&
                    collider_group_contains_pool(reaction.group)) {
                    return pool_invalid("GamePlan legacy reset_group reaction targets a pooled group");
                }
            }
        }
        if (has_trigger_rule && scene.max_contact_pairs == 0U) {
            return interaction_invalid("GamePlan trigger rules require max_contact_pairs");
        }
        std::unordered_set<SymbolId> game_rule_ids{};
        for (const auto& rule : scene.rules) {
            if (!version_0_3 || !valid_symbol(rule.symbol) || !game_rule_ids.insert(rule.symbol).second ||
                rule.conditions.size() > GameRulePlan::max_conditions || rule.actions.empty() ||
                rule.actions.size() > GameRulePlan::max_actions ||
                static_cast<std::uint32_t>(rule.event.kind) >
                    static_cast<std::uint32_t>(version_0_6 ? GameRuleEventKind::animation_finished
                                                          : version_0_5 ? GameRuleEventKind::contact_end
                                                                        : GameRuleEventKind::collision) ||
                ((rule.event.kind == GameRuleEventKind::action_pressed ||
                  rule.event.kind == GameRuleEventKind::action_released) &&
                 rule.event.action_index >= plan.actions.size()) ||
                (rule.event.kind == GameRuleEventKind::fixed_interval &&
                 (rule.event.interval_ticks == 0U || rule.event.interval_ticks > 1'000'000U)) ||
                ((rule.event.kind == GameRuleEventKind::collision ||
                  rule.event.kind == GameRuleEventKind::contact_begin ||
                  rule.event.kind == GameRuleEventKind::contact_end) &&
                 rule.event.collision_rule_index >= scene.collision_rules.size())) {
                return invalid("GamePlan contains an invalid event-action rule");
            }
            if (rule.event.kind == GameRuleEventKind::animation_finished &&
                (!version_0_6 || rule.event.animation_index >= plan.animations.size())) {
                return animation_invalid("GamePlan animation event references an invalid clip");
            }
            if (rule.event.kind == GameRuleEventKind::collision && version_0_5 &&
                scene.collision_rules[rule.event.collision_rule_index].interaction !=
                    GameCollisionInteraction::solid) {
                return interaction_invalid(
                    "GamePlan collision event does not reference a solid interaction");
            }
            if ((rule.event.kind == GameRuleEventKind::contact_begin ||
                 rule.event.kind == GameRuleEventKind::contact_end) &&
                (!version_0_5 ||
                 scene.collision_rules[rule.event.collision_rule_index].interaction !=
                     GameCollisionInteraction::trigger)) {
                return interaction_invalid(
                    "GamePlan contact event does not reference a trigger interaction");
            }
            for (const auto& condition : rule.conditions) {
                if (static_cast<std::uint32_t>(condition.kind) >
                        static_cast<std::uint32_t>(version_0_6 ? GameRuleConditionKind::field_value
                                                              : GameRuleConditionKind::group_active_count) ||
                    static_cast<std::uint32_t>(condition.comparison) >
                        static_cast<std::uint32_t>(GameComparison::greater_equal) ||
                    (condition.kind == GameRuleConditionKind::int_state &&
                     condition.state_index >= plan.states.size()) ||
                    (condition.kind == GameRuleConditionKind::group_active_count &&
                     (condition.spawn_group_index >= scene.spawn_groups.size() || condition.value < 0 ||
                       static_cast<std::uint32_t>(condition.value) >
                           scene.spawn_groups[condition.spawn_group_index].count)) ||
                    (condition.kind == GameRuleConditionKind::tile_value &&
                     (condition.tile_layer_index >= scene.tile_layers.size() || condition.value < 0 ||
                      static_cast<std::uint32_t>(condition.value) >
                          scene.tile_layers[condition.tile_layer_index].atlas_columns *
                              scene.tile_layers[condition.tile_layer_index].atlas_rows)) ||
                    (condition.kind == GameRuleConditionKind::field_value &&
                     (condition.field_index >= scene.fields.size() ||
                      condition.value < scene.fields[condition.field_index].minimum ||
                      condition.value > scene.fields[condition.field_index].maximum))) {
                    return invalid("GamePlan contains an invalid rule condition");
                }
            }
            const bool collision_context = rule.event.kind == GameRuleEventKind::collision ||
                                            rule.event.kind == GameRuleEventKind::contact_begin ||
                                            rule.event.kind == GameRuleEventKind::contact_end;
            const bool event_entity_context = rule.event.kind == GameRuleEventKind::animation_finished;
            const auto valid_single_target = [&](const GameRuleTargetPlan& target) {
                if (static_cast<std::uint32_t>(target.kind) >
                    static_cast<std::uint32_t>(version_0_6 ? GameRuleTargetKind::event_entity
                                                          : GameRuleTargetKind::collision_b)) {
                    return false;
                }
                if (target.kind == GameRuleTargetKind::spawn_group) return false;
                if (target.kind == GameRuleTargetKind::spawn_index) {
                    return target.spawn_group_index < scene.spawn_groups.size() &&
                           target.item_index < scene.spawn_groups[target.spawn_group_index].count;
                }
                if (target.kind == GameRuleTargetKind::collision_a ||
                    target.kind == GameRuleTargetKind::collision_b) {
                    return collision_context;
                }
                return target.kind == GameRuleTargetKind::event_entity && event_entity_context;
            };
            const auto validate_cell_source = [&](const GameCellSourceKind source_kind,
                                                   const std::uint32_t x,
                                                   const std::uint32_t y,
                                                   const GameRuleTargetPlan& target,
                                                   const std::uint32_t grid_index) {
                if (grid_index >= scene.grids.size() ||
                    static_cast<std::uint32_t>(source_kind) >
                        static_cast<std::uint32_t>(GameCellSourceKind::target)) {
                    return false;
                }
                const auto& grid = scene.grids[grid_index];
                return source_kind == GameCellSourceKind::constant
                           ? x < grid.columns && y < grid.rows
                           : valid_single_target(target);
            };
            for (const auto& condition : rule.conditions) {
                if (condition.kind == GameRuleConditionKind::tile_value) {
                    const auto grid_index = scene.tile_layers[condition.tile_layer_index].grid_index;
                    if (!validate_cell_source(condition.cell_source, condition.cell_x, condition.cell_y,
                                              condition.cell_target, grid_index)) {
                        return tile_field_invalid("GamePlan tile condition has an invalid cell source");
                    }
                } else if (condition.kind == GameRuleConditionKind::field_value) {
                    const auto grid_index = scene.fields[condition.field_index].grid_index;
                    if (!validate_cell_source(condition.cell_source, condition.cell_x, condition.cell_y,
                                              condition.cell_target, grid_index)) {
                        return tile_field_invalid("GamePlan field condition has an invalid cell source");
                    }
                }
            }
            const auto target_collider_group = [&](const GameRuleTargetPlan& target) {
                const auto& collision_rule = scene.collision_rules[rule.event.collision_rule_index];
                return target.kind == GameRuleTargetKind::collision_a ? collision_rule.group_a
                                                                      : collision_rule.group_b;
            };
            const auto target_may_be_pooled = [&](const GameRuleTargetPlan& target) {
                if (target.kind == GameRuleTargetKind::spawn_group ||
                    target.kind == GameRuleTargetKind::spawn_index) {
                    return target.spawn_group_index < pool_for_spawn_group.size() &&
                           pool_for_spawn_group[target.spawn_group_index] >= 0;
                }
                return collision_context && collider_group_contains_pool(target_collider_group(target));
            };
            const auto collision_target_owned_by_pool = [&](const GameRuleTargetPlan& target,
                                                             const std::uint32_t pool_index) {
                if (!collision_context || pool_index >= scene.pools.size()) return false;
                const auto endpoint = target_collider_group(target);
                const auto owned_group = scene.pools[pool_index].spawn_group_index;
                bool found = false;
                for (std::size_t group_index = 0U; group_index < scene.spawn_groups.size(); ++group_index) {
                    const auto& spawn = scene.spawn_groups[group_index];
                    if (!spawn.has_collider || spawn.collider.group != endpoint) continue;
                    found = true;
                    if (group_index != owned_group) return false;
                }
                return found;
            };
            for (const auto& action : rule.actions) {
                if (static_cast<std::uint32_t>(action.kind) >
                    static_cast<std::uint32_t>(version_0_6 ? GameRuleActionKind::emit_particles
                                                          : version_0_4 ? GameRuleActionKind::reset_pool
                                                                        : GameRuleActionKind::relocate_to_free_cell)) {
                    return invalid("GamePlan contains an invalid rule action kind");
                }
                const bool target_action = action.kind == GameRuleActionKind::activate ||
                                           action.kind == GameRuleActionKind::deactivate ||
                                            action.kind == GameRuleActionKind::set_velocity ||
                                            action.kind == GameRuleActionKind::relocate_to_free_cell ||
                                            action.kind == GameRuleActionKind::release_to_pool ||
                                            action.kind == GameRuleActionKind::play_animation ||
                                            action.kind == GameRuleActionKind::stop_animation;
                if (target_action &&
                     (static_cast<std::uint32_t>(action.target.kind) >
                          static_cast<std::uint32_t>(version_0_6 ? GameRuleTargetKind::event_entity
                                                                : GameRuleTargetKind::collision_b) ||
                     ((action.target.kind == GameRuleTargetKind::spawn_group ||
                       action.target.kind == GameRuleTargetKind::spawn_index) &&
                      (action.target.spawn_group_index >= scene.spawn_groups.size() ||
                       (action.target.kind == GameRuleTargetKind::spawn_index &&
                        action.target.item_index >= scene.spawn_groups[action.target.spawn_group_index].count))) ||
                      ((action.target.kind == GameRuleTargetKind::collision_a ||
                        action.target.kind == GameRuleTargetKind::collision_b) &&
                        !collision_context) ||
                       (action.target.kind == GameRuleTargetKind::event_entity && !event_entity_context))) {
                    return action.kind == GameRuleActionKind::release_to_pool
                               ? pool_invalid("GamePlan release_to_pool action target is invalid")
                               : invalid("GamePlan contains an invalid rule action target");
                }
                switch (action.kind) {
                case GameRuleActionKind::set_int_state:
                    if (action.state_index >= plan.states.size() ||
                        action.value < plan.states[action.state_index].minimum ||
                        action.value > plan.states[action.state_index].maximum) {
                        return invalid("GamePlan set_int_state action is invalid");
                    }
                    break;
                case GameRuleActionKind::add_int_state:
                    if (action.state_index >= plan.states.size()) return invalid("GamePlan add_int_state action is invalid");
                    break;
                case GameRuleActionKind::activate:
                case GameRuleActionKind::deactivate:
                    if (version_0_4 && target_may_be_pooled(action.target)) {
                        return pool_invalid("GamePlan direct active-state action targets a pooled group");
                    }
                    break;
                case GameRuleActionKind::set_group_active_count:
                    if (action.spawn_group_index >= scene.spawn_groups.size() ||
                        (!action.count_from_state &&
                         action.count_constant > scene.spawn_groups[action.spawn_group_index].count) ||
                        (action.count_from_state &&
                         (action.count_state_index >= plan.states.size() ||
                          plan.states[action.count_state_index].minimum < 0 ||
                          plan.states[action.count_state_index].maximum >
                              static_cast<std::int32_t>(scene.spawn_groups[action.spawn_group_index].count)))) {
                        return invalid("GamePlan set_group_active_count action is invalid");
                    }
                    if (version_0_4 && pool_for_spawn_group[action.spawn_group_index] >= 0) {
                        return pool_invalid("GamePlan active-count action targets a pooled group");
                    }
                    break;
                case GameRuleActionKind::set_velocity:
                    if (!finite_vec2(action.velocity) ||
                        ((action.target.kind == GameRuleTargetKind::spawn_group ||
                          action.target.kind == GameRuleTargetKind::spawn_index) &&
                         !scene.spawn_groups[action.target.spawn_group_index].has_velocity) ||
                        action.target.kind == GameRuleTargetKind::event_entity) {
                        return invalid("GamePlan set_velocity action is invalid");
                    }
                    if (action.target.kind == GameRuleTargetKind::collision_a ||
                        action.target.kind == GameRuleTargetKind::collision_b) {
                        const auto& collision_rule = scene.collision_rules[rule.event.collision_rule_index];
                        const auto endpoint_group = action.target.kind == GameRuleTargetKind::collision_a
                                                        ? collision_rule.group_a
                                                        : collision_rule.group_b;
                        for (const auto& spawn : scene.spawn_groups) {
                            if (spawn.has_collider && spawn.collider.group == endpoint_group && !spawn.has_velocity) {
                                return invalid("GamePlan set_velocity collision endpoint lacks Velocity2D");
                            }
                        }
                    }
                    break;
                case GameRuleActionKind::queue_grid_direction:
                    if (action.system_index >= scene.systems.size() ||
                        scene.systems[action.system_index].operation != GameOperationId::grid_motion ||
                        action.direction == GameDirection::none ||
                        static_cast<std::uint32_t>(action.direction) >
                            static_cast<std::uint32_t>(GameDirection::right)) {
                        return invalid("GamePlan queue_grid_direction action is invalid");
                    }
                    break;
                case GameRuleActionKind::reset_group:
                    if (action.spawn_group_index >= scene.spawn_groups.size()) return invalid("GamePlan reset_group action is invalid");
                    if (version_0_4 && pool_for_spawn_group[action.spawn_group_index] >= 0) {
                        return pool_invalid("GamePlan reset_group action targets a pooled group");
                    }
                    break;
                case GameRuleActionKind::play_sound:
                    if (action.asset_index >= plan.assets.size() ||
                        plan.assets[action.asset_index].kind != GameAssetKind::wav) {
                        return invalid("GamePlan play_sound action is invalid");
                    }
                    break;
                case GameRuleActionKind::relocate_to_free_cell:
                    if (!collision_context ||
                        (action.target.kind != GameRuleTargetKind::collision_a &&
                         action.target.kind != GameRuleTargetKind::collision_b) ||
                        action.grid_index >= scene.grids.size() || action.occupancy_group_indices.empty() ||
                        action.occupancy_group_indices.size() > GameRuleActionPlan::max_occupancy_groups ||
                        (action.has_result_state && action.result_state_index >= plan.states.size())) {
                        return invalid("GamePlan relocate_to_free_cell action is invalid");
                    }
                    {
                        std::unordered_set<std::uint32_t> occupied{};
                        for (const auto group_index : action.occupancy_group_indices) {
                            if (group_index >= scene.spawn_groups.size() || !occupied.insert(group_index).second) {
                                return invalid("GamePlan relocation occupancy groups are invalid");
                            }
                            if (!group_aligned_to_grid(group_index, action.grid_index)) {
                                return invalid("GamePlan relocation occupancy groups are not aligned to the grid");
                            }
                        }
                    }
                    if (action.has_result_state &&
                        (plan.states[action.result_state_index].minimum > 0 ||
                         plan.states[action.result_state_index].maximum < 1)) {
                        return invalid("GamePlan relocation result state is invalid");
                    }
                    break;
                case GameRuleActionKind::spawn_from_pool:
                    if (!version_0_4 || action.pool_index >= scene.pools.size() ||
                        static_cast<std::uint32_t>(action.pool_position_kind) >
                            static_cast<std::uint32_t>(GamePoolSpawnPositionKind::target) ||
                        !finite_vec2(action.pool_position) || !finite_vec2(action.pool_position_offset) ||
                        (action.has_velocity_override && !finite_vec2(action.velocity)) ||
                        (action.has_rotation_override && !std::isfinite(action.rotation)) ||
                        (action.has_lifetime &&
                         (action.lifetime_ticks == 0U || action.lifetime_ticks > 1'000'000U)) ||
                        (action.has_result_state && action.result_state_index >= plan.states.size())) {
                        return pool_invalid("GamePlan spawn_from_pool action is invalid");
                    }
                    if (action.has_velocity_override &&
                        !scene.spawn_groups[scene.pools[action.pool_index].spawn_group_index].has_velocity) {
                        return pool_invalid("GamePlan pool velocity override lacks Velocity2D");
                    }
                    if (action.pool_position_kind == GamePoolSpawnPositionKind::constant) {
                        const double x = static_cast<double>(action.pool_position.x) +
                                         action.pool_position_offset.x;
                        const double y = static_cast<double>(action.pool_position.y) +
                                         action.pool_position_offset.y;
                        if (!representable_float(x) || !representable_float(y)) {
                            return pool_invalid("GamePlan pool constant position plus offset is not representable");
                        }
                    }
                    if (action.pool_position_kind == GamePoolSpawnPositionKind::target) {
                        const auto& target = action.pool_position_target;
                        if (target.kind == GameRuleTargetKind::spawn_group ||
                            static_cast<std::uint32_t>(target.kind) >
                                static_cast<std::uint32_t>(GameRuleTargetKind::collision_b) ||
                            (target.kind == GameRuleTargetKind::spawn_index &&
                             (target.spawn_group_index >= scene.spawn_groups.size() ||
                              target.item_index >= scene.spawn_groups[target.spawn_group_index].count)) ||
                            ((target.kind == GameRuleTargetKind::collision_a ||
                              target.kind == GameRuleTargetKind::collision_b) && !collision_context)) {
                            return pool_invalid("GamePlan pool position target is invalid");
                        }
                    }
                    if (action.pool_position_kind == GamePoolSpawnPositionKind::initial ||
                        (action.pool_position_kind == GamePoolSpawnPositionKind::target &&
                         action.pool_position_target.kind == GameRuleTargetKind::spawn_index)) {
                        const auto group_index = action.pool_position_kind == GamePoolSpawnPositionKind::initial
                                                     ? scene.pools[action.pool_index].spawn_group_index
                                                     : action.pool_position_target.spawn_group_index;
                        const auto& group = scene.spawn_groups[group_index];
                        const auto first_item = action.pool_position_kind == GamePoolSpawnPositionKind::initial
                                                    ? 0U
                                                    : action.pool_position_target.item_index;
                        const auto item_count = action.pool_position_kind == GamePoolSpawnPositionKind::initial
                                                    ? group.count
                                                    : first_item + 1U;
                        for (std::uint32_t item = first_item; item < item_count; ++item) {
                            const auto column = item % group.placement.columns;
                            const auto row = item / group.placement.columns;
                            const double x = static_cast<double>(group.placement.origin.x) +
                                             static_cast<double>(column) * group.placement.spacing.x +
                                             group.transform.position_offset.x + action.pool_position_offset.x;
                            const double y = static_cast<double>(group.placement.origin.y) +
                                             static_cast<double>(row) * group.placement.spacing.y +
                                             group.transform.position_offset.y + action.pool_position_offset.y;
                            if (!representable_float(x) || !representable_float(y)) {
                                return pool_invalid("GamePlan pool authored position plus offset is not representable");
                            }
                        }
                    }
                    if (action.has_result_state &&
                        (plan.states[action.result_state_index].minimum > 0 ||
                         plan.states[action.result_state_index].maximum < 1)) {
                        return pool_invalid("GamePlan pool result state cannot represent zero and one");
                    }
                    break;
                case GameRuleActionKind::release_to_pool:
                    if (!version_0_4 || action.pool_index >= scene.pools.size() ||
                        action.target.kind == GameRuleTargetKind::spawn_group ||
                        (action.target.kind == GameRuleTargetKind::spawn_index &&
                         action.target.spawn_group_index != scene.pools[action.pool_index].spawn_group_index) ||
                        ((action.target.kind == GameRuleTargetKind::collision_a ||
                          action.target.kind == GameRuleTargetKind::collision_b) &&
                         !collision_target_owned_by_pool(action.target, action.pool_index)) ||
                        (action.has_result_state && action.result_state_index >= plan.states.size())) {
                        return pool_invalid("GamePlan release_to_pool action is invalid");
                    }
                    if (action.has_result_state &&
                        (plan.states[action.result_state_index].minimum > 0 ||
                         plan.states[action.result_state_index].maximum < 1)) {
                        return pool_invalid("GamePlan pool result state cannot represent zero and one");
                    }
                    break;
                case GameRuleActionKind::reset_pool:
                    if (!version_0_4 || action.pool_index >= scene.pools.size()) {
                        return pool_invalid("GamePlan reset_pool action is invalid");
                    }
                    break;
                case GameRuleActionKind::play_animation:
                case GameRuleActionKind::stop_animation: {
                    if (!version_0_6 ||
                        (action.kind == GameRuleActionKind::play_animation &&
                         action.animation_index >= plan.animations.size())) {
                        return animation_invalid("GamePlan animation action is invalid");
                    }
                    if ((action.target.kind == GameRuleTargetKind::spawn_group ||
                         action.target.kind == GameRuleTargetKind::spawn_index) &&
                        !scene.spawn_groups[action.target.spawn_group_index].has_sprite) {
                        return animation_invalid("GamePlan animation target lacks Sprite2D");
                    }
                    if (action.target.kind == GameRuleTargetKind::collision_a ||
                        action.target.kind == GameRuleTargetKind::collision_b) {
                        const auto endpoint = target_collider_group(action.target);
                        bool found = false;
                        for (const auto& spawn : scene.spawn_groups) {
                            if (!spawn.has_collider || spawn.collider.group != endpoint) continue;
                            found = true;
                            if (!spawn.has_sprite) {
                                return animation_invalid("GamePlan animation endpoint lacks Sprite2D");
                            }
                        }
                        if (!found) return animation_invalid("GamePlan animation endpoint is absent");
                    }
                    break;
                }
                case GameRuleActionKind::set_tile: {
                    if (!version_0_6 || action.tile_layer_index >= scene.tile_layers.size() ||
                        action.tile_value > scene.tile_layers[action.tile_layer_index].atlas_columns *
                                                scene.tile_layers[action.tile_layer_index].atlas_rows ||
                        !validate_cell_source(
                            action.cell_source, action.cell_x, action.cell_y, action.cell_target,
                            scene.tile_layers[action.tile_layer_index].grid_index)) {
                        return tile_field_invalid("GamePlan set_tile action is invalid");
                    }
                    if (action.has_result_state &&
                        (action.result_state_index >= plan.states.size() ||
                         plan.states[action.result_state_index].minimum > 0 ||
                         plan.states[action.result_state_index].maximum < 1)) {
                        return tile_field_invalid("GamePlan tile result state cannot represent zero and one");
                    }
                    break;
                }
                case GameRuleActionKind::set_field:
                case GameRuleActionKind::add_field: {
                    if (!version_0_6 || action.field_index >= scene.fields.size()) {
                        return tile_field_invalid("GamePlan field action is invalid");
                    }
                    const auto& field = scene.fields[action.field_index];
                    if ((action.kind == GameRuleActionKind::set_field &&
                         (action.value < field.minimum || action.value > field.maximum)) ||
                        !validate_cell_source(
                            action.cell_source, action.cell_x, action.cell_y, action.cell_target, field.grid_index)) {
                        return tile_field_invalid("GamePlan field action is invalid");
                    }
                    if (action.has_result_state &&
                        (action.result_state_index >= plan.states.size() ||
                         plan.states[action.result_state_index].minimum > 0 ||
                         plan.states[action.result_state_index].maximum < 1)) {
                        return tile_field_invalid("GamePlan field result state cannot represent zero and one");
                    }
                    break;
                }
                case GameRuleActionKind::save_slot:
                case GameRuleActionKind::load_slot:
                case GameRuleActionKind::delete_slot:
                    if (!version_0_6 || !plan.save.enabled || action.save_slot >= plan.save.slot_count ||
                        (action.has_result_state &&
                         (action.result_state_index >= plan.states.size() ||
                          plan.states[action.result_state_index].minimum > 0 ||
                          plan.states[action.result_state_index].maximum < 1))) {
                        return save_invalid("GamePlan save action is invalid");
                    }
                    break;
                case GameRuleActionKind::camera_shake:
                    if (!version_0_6 || !std::isfinite(action.scalar) || action.scalar < 0.0F ||
                        action.scalar > 1'000.0F || action.duration_ticks == 0U ||
                        action.duration_ticks > 1'000'000U) {
                        return presentation_invalid("GamePlan camera shake action is invalid");
                    }
                    break;
                case GameRuleActionKind::set_camera_zoom:
                    if (!version_0_6 || !std::isfinite(action.scalar) || action.scalar < 0.1F ||
                        action.scalar > 10.0F) {
                        return presentation_invalid("GamePlan camera zoom action is invalid");
                    }
                    break;
                case GameRuleActionKind::set_locale:
                    if (!version_0_6 || action.locale_index >= plan.localizations.size()) {
                        return localization_invalid("GamePlan locale action is invalid");
                    }
                    break;
                case GameRuleActionKind::set_input_profile:
                    if (!version_0_6 || action.input_profile_index >= plan.input_profiles.size()) {
                        return input_profile_invalid("GamePlan input profile action is invalid");
                    }
                    break;
                case GameRuleActionKind::play_music:
                    if (!version_0_6 || action.asset_index >= plan.assets.size() ||
                        plan.assets[action.asset_index].kind != GameAssetKind::music ||
                        !std::isfinite(action.scalar) || action.scalar < 0.0F || action.scalar > 1.0F ||
                        action.fade_ticks > 1'000'000U) {
                        return presentation_invalid("GamePlan play_music action is invalid");
                    }
                    break;
                case GameRuleActionKind::stop_music:
                    if (!version_0_6 || action.fade_ticks > 1'000'000U) {
                        return presentation_invalid("GamePlan stop_music action is invalid");
                    }
                    break;
                case GameRuleActionKind::set_music_volume:
                    if (!version_0_6 || !std::isfinite(action.scalar) || action.scalar < 0.0F ||
                        action.scalar > 1.0F || action.fade_ticks > 1'000'000U) {
                        return presentation_invalid("GamePlan music volume action is invalid");
                    }
                    break;
                case GameRuleActionKind::emit_particles:
                    if (!version_0_6 || action.particle_emitter_index >= scene.particle_emitters.size() ||
                        action.particle_count == 0U ||
                        action.particle_count > scene.particle_emitters[action.particle_emitter_index].capacity ||
                        static_cast<std::uint32_t>(action.particle_position_kind) >
                            static_cast<std::uint32_t>(GamePoolSpawnPositionKind::target) ||
                        action.particle_position_kind == GamePoolSpawnPositionKind::initial ||
                        !finite_vec2(action.particle_position) || !finite_vec2(action.particle_position_offset) ||
                        (action.particle_position_kind == GamePoolSpawnPositionKind::target &&
                         !valid_single_target(action.particle_position_target))) {
                        return presentation_invalid("GamePlan emit_particles action is invalid");
                    }
                    if (action.particle_position_kind == GamePoolSpawnPositionKind::constant) {
                        const double x = static_cast<double>(action.particle_position.x) +
                                         action.particle_position_offset.x;
                        const double y = static_cast<double>(action.particle_position.y) +
                                         action.particle_position_offset.y;
                        if (!representable_float(x) || !representable_float(y)) {
                            return presentation_invalid("GamePlan particle position is not representable");
                        }
                    }
                    break;
                }
            }
        }
        std::unordered_set<SymbolId> ui_ids{};
        for (const auto& element : scene.ui) {
            if (!valid_symbol(element.symbol) ||
                static_cast<std::uint32_t>(element.kind) > static_cast<std::uint32_t>(UiElementKind::button) ||
                !std::isfinite(element.position.x) || !std::isfinite(element.position.y) ||
                !std::isfinite(element.size.x) || !std::isfinite(element.size.y) ||
                element.size.x <= 0.0F || element.size.y <= 0.0F ||
                !valid_color(element.color) || !valid_color(element.hover_color) || !valid_color(element.text_color) ||
                (element.kind != UiElementKind::panel &&
                 (!valid_symbol(element.text) || element.font_asset >= plan.assets.size() ||
                   plan.assets[element.font_asset].kind != GameAssetKind::font)) ||
                 (element.kind == UiElementKind::button && element.action >= plan.actions.size()) ||
                 element.layer < -1'000'000 || element.layer > 1'000'000 ||
                 static_cast<std::uint32_t>(element.anchor) >
                     static_cast<std::uint32_t>(GameUiAnchor::bottom_right) ||
                 (element.localized &&
                  (!version_0_6 || plan.localizations.empty() ||
                   element.localization_key_index >= plan.localizations.front().entries.size()))) {
                return invalid("GamePlan contains an invalid UI element");
            }
            if (!ui_ids.insert(element.symbol).second || !std::isfinite(element.text_scale) || element.text_scale <= 0.0F ||
                element.text_scale > 16.0F) {
                return invalid("GamePlan contains duplicate or invalid UI data");
            }
            if (element.kind == UiElementKind::panel || element.kind == UiElementKind::button) {
                ++render_submission_count;
            }
            if (element.kind != UiElementKind::panel) {
                const auto& font = plan.assets[element.font_asset];
                const auto validate_text = [&](const std::string_view text, std::size_t& glyph_count) -> Result<void> {
                    std::size_t offset = 0U;
                    while (offset < text.size()) {
                        if (text[offset] == '{') {
                            const auto close = text.find('}', offset + 1U);
                            const auto state_name = close == std::string_view::npos
                                                        ? std::string{}
                                                        : std::string{text.substr(offset + 1U, close - offset - 1U)};
                            const auto state = state_names.find(state_name);
                            if (close == std::string_view::npos || close == offset + 1U ||
                                state == state_names.end()) {
                                return invalid("GamePlan UI contains an invalid state placeholder");
                            }
                            const auto has_glyph = [&](const std::uint32_t value) {
                                const auto glyph = std::lower_bound(
                                    font.glyphs.begin(), font.glyphs.end(), value,
                                    [](const GlyphPlan& item, const std::uint32_t codepoint) {
                                        return item.codepoint < codepoint;
                                    });
                                return glyph != font.glyphs.end() && glyph->codepoint == value;
                            };
                            for (std::uint32_t digit = static_cast<std::uint32_t>('0');
                                 digit <= static_cast<std::uint32_t>('9'); ++digit) {
                                if (!has_glyph(digit)) {
                                    return invalid("GamePlan placeholder font is missing decimal digits");
                                }
                            }
                            if (plan.states[state->second].minimum < 0 &&
                                !has_glyph(static_cast<std::uint32_t>('-'))) {
                                return invalid("GamePlan signed placeholder font is missing a minus sign");
                            }
                            glyph_count += std::max(
                                decimal_width(plan.states[state->second].minimum),
                                decimal_width(plan.states[state->second].maximum));
                            offset = close + 1U;
                            continue;
                        }
                        auto codepoint = decode_utf8_codepoint(text, offset, scene.source_path.string(), "/ui");
                        if (!codepoint) return invalid("GamePlan UI text contains invalid UTF-8");
                        if (*codepoint == '\n' || *codepoint == '\r') continue;
                        const auto glyph = std::lower_bound(
                            font.glyphs.begin(), font.glyphs.end(), *codepoint,
                            [](const GlyphPlan& item, const std::uint32_t value) { return item.codepoint < value; });
                        if (glyph == font.glyphs.end() || glyph->codepoint != *codepoint) {
                            return invalid("GamePlan UI font is missing a glyph");
                        }
                        ++glyph_count;
                    }
                    return {};
                };
                if (element.localized) {
                    std::size_t maximum_localized_glyphs = 0U;
                    for (const auto& locale : plan.localizations) {
                        const auto entry = locale.entries[element.localization_key_index];
                        std::size_t localized_glyphs = 0U;
                        if (auto checked = validate_text(plan.symbol(entry.value), localized_glyphs); !checked) {
                            return checked;
                        }
                        maximum_localized_glyphs = std::max(maximum_localized_glyphs, localized_glyphs);
                    }
                    render_submission_count += maximum_localized_glyphs;
                } else {
                    std::size_t glyph_count = 0U;
                    if (auto checked = validate_text(plan.symbol(element.text), glyph_count); !checked) {
                        return checked;
                    }
                    render_submission_count += glyph_count;
                }
            }
            if (render_submission_count > GamePlan::max_render_submissions) {
                return invalid("GamePlan scene exceeds the renderer submission capacity");
            }
        }
        std::unordered_set<SymbolId> stack_ids{};
        std::unordered_set<std::uint32_t> stack_children{};
        for (const auto& stack : scene.ui_stacks) {
            if (!version_0_6 || !valid_symbol(stack.symbol) || !stack_ids.insert(stack.symbol).second ||
                static_cast<std::uint32_t>(stack.direction) >
                    static_cast<std::uint32_t>(GameStackDirection::vertical) ||
                static_cast<std::uint32_t>(stack.anchor) >
                    static_cast<std::uint32_t>(GameUiAnchor::bottom_right) ||
                !finite_vec2(stack.position) || !finite_vec2(stack.size) || stack.size.x <= 0.0F ||
                stack.size.y <= 0.0F || !finite_vec2(stack.padding) || stack.padding.x < 0.0F ||
                stack.padding.y < 0.0F || !std::isfinite(stack.spacing) || stack.spacing < 0.0F ||
                stack.child_indices.empty() || stack.child_indices.size() > 128U) {
                return presentation_invalid("GamePlan contains an invalid UI stack");
            }
            const auto center = anchored_ui_center(
                stack.anchor, stack.size,
                static_cast<float>(plan.window.virtual_width),
                static_cast<float>(plan.window.virtual_height));
            const Vec2 container_center{center.x + stack.position.x, center.y + stack.position.y};
            const Vec2 content_min{
                container_center.x - stack.size.x * 0.5F + stack.padding.x,
                container_center.y - stack.size.y * 0.5F + stack.padding.y,
            };
            const Vec2 content_max{
                container_center.x + stack.size.x * 0.5F - stack.padding.x,
                container_center.y + stack.size.y * 0.5F - stack.padding.y,
            };
            if (content_min.x > content_max.x || content_min.y > content_max.y) {
                return presentation_invalid("GamePlan UI stack padding exceeds its content bounds");
            }
            for (const auto child : stack.child_indices) {
                if (child >= scene.ui.size() || !stack_children.insert(child).second) {
                    return presentation_invalid("GamePlan UI stack child ownership is invalid");
                }
                const auto& element = scene.ui[child];
                const Vec2 child_min{
                    element.position.x - element.size.x * 0.5F,
                    element.position.y - element.size.y * 0.5F,
                };
                const Vec2 child_max{
                    element.position.x + element.size.x * 0.5F,
                    element.position.y + element.size.y * 0.5F,
                };
                if (child_min.x < content_min.x - 0.0001F ||
                    child_min.y < content_min.y - 0.0001F ||
                    child_max.x > content_max.x + 0.0001F ||
                    child_max.y > content_max.y + 0.0001F) {
                    return presentation_invalid("GamePlan UI stack child rectangle exceeds its content bounds");
                }
            }
        }
        if (render_submission_count > GamePlan::max_render_submissions) {
            return presentation_invalid("GamePlan scene exceeds the renderer submission capacity");
        }
    }
    for (const auto& transition : plan.transitions) {
        if (transition.from_scene >= plan.scenes.size() ||
            static_cast<std::uint32_t>(transition.condition.kind) >
                static_cast<std::uint32_t>(TransitionConditionKind::group_inactive) ||
            (!transition.quit && transition.to_scene >= plan.scenes.size()) ||
            (transition.condition.kind == TransitionConditionKind::action_pressed &&
             transition.condition.action >= plan.actions.size()) ||
            ((transition.condition.kind == TransitionConditionKind::int_state_at_most ||
              transition.condition.kind == TransitionConditionKind::int_state_at_least) &&
             transition.condition.state_index >= plan.states.size()) ||
            (transition.condition.kind == TransitionConditionKind::group_inactive &&
             !valid_symbol(transition.condition.group))) {
            return invalid("GamePlan contains an invalid transition");
        }
        if (transition.condition.kind == TransitionConditionKind::group_inactive) {
            bool found_group = false;
            for (const auto& spawn : plan.scenes[transition.from_scene].spawn_groups) {
                found_group = found_group ||
                              (spawn.has_collider && spawn.collider.group == transition.condition.group);
            }
            if (!found_group) return invalid("GamePlan transition group is absent from its source scene");
        }
    }
    std::unordered_set<std::uint32_t> fps_action_ids{};
    for (const auto& fps : plan.fps_actions) {
        if (fps.action >= plan.actions.size() ||
            static_cast<std::uint32_t>(fps.cap) > static_cast<std::uint32_t>(RenderFpsCap::unlimited)) {
            return invalid("GamePlan contains an invalid FPS action");
        }
        if (!fps_action_ids.insert(fps.action).second) return invalid("GamePlan contains duplicate FPS actions");
    }
    if (auto save_capacity = compute_game_save_capacity(plan); !save_capacity) {
        return std::unexpected(std::move(save_capacity.error()));
    }
    if (plan.plan_hash == 0U || plan.plan_hash != compute_game_plan_hash(plan)) {
        return invalid("GamePlan hash does not match its contents");
    }
    return {};
}

Result<GamePlan> compile_game_file(const std::filesystem::path& manifest_path) {
    std::error_code error{};
    const auto canonical_manifest = std::filesystem::weakly_canonical(manifest_path, error);
    if (error || !std::filesystem::is_regular_file(canonical_manifest, error) || error) {
        return std::unexpected(game_error(
            DiagnosticCode::input_invalid, "GameManifest file could not be opened", manifest_path.string()));
    }
    const auto root = std::filesystem::weakly_canonical(canonical_manifest.parent_path(), error);
    if (error) {
        return std::unexpected(game_error(
            DiagnosticCode::asset_path_invalid, "GameManifest directory could not be resolved", manifest_path.string()));
    }
    auto source_text = read_text_file(canonical_manifest, 4U * 1024U * 1024U, DiagnosticCode::game_manifest_invalid);
    if (!source_text) return std::unexpected(std::move(source_text.error()));
    auto document = parse_json(*source_text, canonical_manifest.string(), DiagnosticCode::game_manifest_invalid);
    if (!document) return std::unexpected(std::move(document.error()));
    const auto source_name = canonical_manifest.string();
    auto version = string_value(*document, "schema_version", source_name, "/");
    if (!version) return std::unexpected(std::move(version.error()));
    auto schema_version = parse_game_schema_version(
        *version, source_name, "/schema_version", DiagnosticCode::game_manifest_invalid);
    if (!schema_version) return std::unexpected(std::move(schema_version.error()));
    const bool version_0_3 = *schema_version != GameSchemaVersion::v0_2;
    const bool version_0_6 = *schema_version == GameSchemaVersion::v0_6;
    StableHash source_hash{};
    source_hash.text(*source_text);
    if (auto rejected = reject_unknown(
            *document,
            version_0_6
                ? std::initializer_list<std::string_view>{
                      "schema_version", "seed", "name", "organization", "application", "window", "assets",
                      "animations", "prefabs", "localizations", "default_locale", "actions", "input_profiles",
                      "default_input_profile", "states", "save", "scenes", "start_scene", "transitions",
                      "fps_actions"}
                : version_0_3
                ? std::initializer_list<std::string_view>{
                      "schema_version", "seed", "name", "organization", "application", "window", "assets",
                      "actions", "states", "scenes", "start_scene", "transitions", "fps_actions"}
                : std::initializer_list<std::string_view>{
                      "schema_version", "name", "organization", "application", "window", "assets", "actions",
                      "states", "scenes", "start_scene", "transitions", "fps_actions"},
            source_name, "/");
        !rejected) {
        return std::unexpected(std::move(rejected.error()));
    }

    Symbols symbols{};
    GamePlan plan{};
    plan.schema_version = *schema_version;
    plan.manifest_path = canonical_manifest;
    plan.content_root = root;
    if (version_0_3 && optional(*document, "seed") != nullptr) {
        auto seed = u64_value(
            *document, "seed", 0U, std::numeric_limits<std::uint64_t>::max(), source_name, "/");
        if (!seed) return std::unexpected(std::move(seed.error()));
        plan.seed = *seed;
    }
    auto name = string_value(*document, "name", source_name, "/");
    if (!name) return std::unexpected(std::move(name.error()));
    auto organization = string_value(*document, "organization", source_name, "/");
    if (!organization) return std::unexpected(std::move(organization.error()));
    auto application = string_value(*document, "application", source_name, "/");
    if (!application) return std::unexpected(std::move(application.error()));
    if (!safe_preference_component(*organization) || !safe_preference_component(*application)) {
        return std::unexpected(game_error(
            DiagnosticCode::game_manifest_invalid, "Organization or application name is unsafe for preferences",
            source_name, "/organization"));
    }
    plan.name = symbols.intern(std::move(*name));
    plan.organization = symbols.intern(std::move(*organization));
    plan.application = symbols.intern(std::move(*application));

    auto window = required(*document, "window", source_name, "/");
    if (!window) return std::unexpected(std::move(window.error()));
    if (auto rejected = reject_unknown(
            **window, {"title", "width", "height", "virtual_width", "virtual_height", "default_fps"},
            source_name, "/window");
        !rejected) {
        return std::unexpected(std::move(rejected.error()));
    }
    auto title = string_value(**window, "title", source_name, "/window");
    if (!title) return std::unexpected(std::move(title.error()));
    auto width = u32_value(**window, "width", 1U, 16'384U, source_name, "/window");
    if (!width) return std::unexpected(std::move(width.error()));
    auto height = u32_value(**window, "height", 1U, 16'384U, source_name, "/window");
    if (!height) return std::unexpected(std::move(height.error()));
    auto virtual_width = u32_value(**window, "virtual_width", 1U, 16'384U, source_name, "/window");
    if (!virtual_width) return std::unexpected(std::move(virtual_width.error()));
    auto virtual_height = u32_value(**window, "virtual_height", 1U, 16'384U, source_name, "/window");
    if (!virtual_height) return std::unexpected(std::move(virtual_height.error()));
    auto fps_text = string_value(**window, "default_fps", source_name, "/window");
    if (!fps_text) return std::unexpected(std::move(fps_text.error()));
    auto fps = parse_fps(*fps_text, source_name, "/window/default_fps");
    if (!fps) return std::unexpected(std::move(fps.error()));
    plan.window = {symbols.intern(std::move(*title)), *width, *height, *virtual_width, *virtual_height, *fps};

    std::unordered_map<std::string, std::uint32_t> asset_indices{};
    auto assets = required(*document, "assets", source_name, "/");
    if (!assets) return std::unexpected(std::move(assets.error()));
    if (!(*assets)->is_array() || (*assets)->empty() || (*assets)->size() > GamePlan::max_assets) {
        return std::unexpected(game_error(
            DiagnosticCode::game_manifest_invalid, "assets must be a non-empty bounded array", source_name,
            "/assets"));
    }
    plan.assets.reserve((*assets)->size());
    for (std::size_t index = 0U; index < (*assets)->size(); ++index) {
        auto id = string_value((**assets)[index], "id", source_name, "/assets/" + std::to_string(index));
        if (!id) return std::unexpected(std::move(id.error()));
        if (asset_indices.contains(*id)) {
            return std::unexpected(game_error(
                DiagnosticCode::game_manifest_invalid, "Duplicate asset id", source_name,
                "/assets/" + std::to_string(index) + "/id"));
        }
        auto asset = parse_asset((**assets)[index], index, root, symbols, source_name, version_0_6);
        if (!asset) return std::unexpected(std::move(asset.error()));
        asset_indices.emplace(std::move(*id), static_cast<std::uint32_t>(plan.assets.size()));
        plan.assets.push_back(std::move(*asset));
    }

    std::unordered_map<std::string, std::uint32_t> animation_indices{};
    if (version_0_6) {
        if (const auto* animations = optional(*document, "animations"); animations != nullptr) {
            if (!animations->is_array() || animations->size() > GamePlan::max_animations) {
                return std::unexpected(game_error(
                    DiagnosticCode::game_animation_invalid,
                    "animations must contain at most 512 items", source_name, "/animations"));
            }
            plan.animations.reserve(animations->size());
            for (std::size_t index = 0U; index < animations->size(); ++index) {
                auto animation = parse_animation(
                    (*animations)[index], index, asset_indices, plan.assets, symbols, source_name);
                if (!animation) return std::unexpected(std::move(animation.error()));
                const auto id = std::string{symbols.view(animation->symbol)};
                if (!animation_indices.emplace(
                        id, static_cast<std::uint32_t>(plan.animations.size())).second) {
                    return std::unexpected(game_error(
                        DiagnosticCode::game_animation_invalid, "Duplicate animation id", source_name,
                        "/animations/" + std::to_string(index) + "/id"));
                }
                plan.animations.push_back(std::move(*animation));
            }
        }
    }

    std::unordered_map<std::string, std::uint32_t> prefab_indices{};
    std::vector<PrefabSource> prefab_sources{};
    if (version_0_6) {
        if (const auto* prefabs = optional(*document, "prefabs"); prefabs != nullptr) {
            if (!prefabs->is_array() || prefabs->size() > GamePlan::max_prefabs) {
                return std::unexpected(game_error(
                    DiagnosticCode::game_prefab_invalid,
                    "prefabs must contain at most 256 items", source_name, "/prefabs"));
            }
            plan.prefabs.reserve(prefabs->size());
            prefab_sources.reserve(prefabs->size());
            for (std::size_t index = 0U; index < prefabs->size(); ++index) {
                const auto pointer = "/prefabs/" + std::to_string(index);
                const auto& declaration = (*prefabs)[index];
                if (auto rejected = reject_unknown(
                        declaration, {"id", "path"}, source_name, pointer,
                        DiagnosticCode::game_prefab_invalid);
                    !rejected) {
                    return std::unexpected(std::move(rejected.error()));
                }
                auto id = string_value(
                    declaration, "id", source_name, pointer, DiagnosticCode::game_prefab_invalid);
                if (!id) return std::unexpected(std::move(id.error()));
                if (prefab_indices.contains(*id)) {
                    return std::unexpected(game_error(
                        DiagnosticCode::game_prefab_invalid, "Duplicate prefab id", source_name,
                        pointer + "/id"));
                }
                auto raw_path = string_value(
                    declaration, "path", source_name, pointer, DiagnosticCode::game_prefab_invalid);
                if (!raw_path) return std::unexpected(std::move(raw_path.error()));
                auto path = resolve_content_path(root, *raw_path, source_name, pointer + "/path");
                if (!path) return std::unexpected(std::move(path.error()));
                auto text = read_text_file(
                    *path, 4U * 1024U * 1024U, DiagnosticCode::game_prefab_invalid);
                if (!text) return std::unexpected(std::move(text.error()));
                source_hash.text(*text);
                auto prefab_document = parse_json(
                    *text, path->string(), DiagnosticCode::game_prefab_invalid);
                if (!prefab_document) return std::unexpected(std::move(prefab_document.error()));
                if (auto rejected = reject_unknown(
                        *prefab_document,
                        {"schema_version", "transform", "velocity", "sprite", "collider", "animation"},
                        path->string(), "/", DiagnosticCode::game_prefab_invalid);
                    !rejected) {
                    return std::unexpected(std::move(rejected.error()));
                }
                auto prefab_version = string_value(
                    *prefab_document, "schema_version", path->string(), "/",
                    DiagnosticCode::game_prefab_invalid);
                if (!prefab_version) return std::unexpected(std::move(prefab_version.error()));
                if (*prefab_version != "1") {
                    return std::unexpected(game_error(
                        DiagnosticCode::game_prefab_invalid,
                        "Unsupported prefab schema version", path->string(), "/schema_version"));
                }
                bool has_component = false;
                Json expanded = {
                    {"id", *id},
                    {"count", 1},
                    {"placement", {
                        {"kind", "grid"}, {"origin", {0.0, 0.0}},
                        {"spacing", {1.0, 1.0}}, {"columns", 1}}}
                };
                for (const auto field : {"transform", "velocity", "sprite", "collider", "animation"}) {
                    if (const auto* component = optional(*prefab_document, field); component != nullptr) {
                        expanded[field] = *component;
                        has_component = true;
                    }
                }
                if (!has_component) {
                    return std::unexpected(game_error(
                        DiagnosticCode::game_prefab_invalid,
                        "Prefab must declare at least one component", path->string(), "/"));
                }
                auto parsed = parse_spawn_group(
                    expanded, 0U, true, true, true, asset_indices, plan.assets,
                    animation_indices, plan.animations, symbols, path->string());
                if (!parsed) return std::unexpected(std::move(parsed.error()));
                GamePrefabPlan prefab{};
                prefab.symbol = symbols.intern(*id);
                prefab.path = *path;
                prefab.transform = parsed->transform;
                prefab.velocity = parsed->velocity;
                prefab.sprite = parsed->sprite;
                prefab.collider = parsed->collider;
                prefab.animation_index = parsed->initial_animation_index;
                prefab.has_transform = optional(*prefab_document, "transform") != nullptr;
                prefab.has_velocity = parsed->has_velocity;
                prefab.has_sprite = parsed->has_sprite;
                prefab.has_collider = parsed->has_collider;
                prefab.has_animation = parsed->has_initial_animation;
                prefab.animation_autoplay = parsed->animation_autoplay;
                prefab_indices.emplace(*id, static_cast<std::uint32_t>(plan.prefabs.size()));
                prefab_sources.push_back(PrefabSource{*id, *path, std::move(*prefab_document)});
                plan.prefabs.push_back(std::move(prefab));
            }
        }
    }

    std::unordered_map<std::string, std::uint32_t> locale_indices{};
    std::unordered_map<std::string, std::uint32_t> localization_key_indices{};
    if (version_0_6) {
        if (const auto* localizations = optional(*document, "localizations"); localizations != nullptr) {
            if (!localizations->is_array() || localizations->empty() ||
                localizations->size() > GamePlan::max_localizations) {
                return std::unexpected(game_error(
                    DiagnosticCode::game_localization_invalid,
                    "localizations must contain one to 32 items", source_name, "/localizations"));
            }
            plan.localizations.reserve(localizations->size());
            std::vector<SymbolId> reference_keys{};
            for (std::size_t index = 0U; index < localizations->size(); ++index) {
                std::string localization_text{};
                auto localization = parse_localization(
                    (*localizations)[index], index, root, symbols, source_name, localization_text);
                if (!localization) return std::unexpected(std::move(localization.error()));
                source_hash.text(localization_text);
                const auto locale = std::string{symbols.view(localization->locale)};
                if (!locale_indices.emplace(
                        locale, static_cast<std::uint32_t>(plan.localizations.size())).second) {
                    return std::unexpected(game_error(
                        DiagnosticCode::game_localization_invalid, "Duplicate locale", source_name,
                        "/localizations/" + std::to_string(index) + "/locale"));
                }
                if (index == 0U) {
                    reference_keys.reserve(localization->entries.size());
                    for (std::size_t key_index = 0U; key_index < localization->entries.size(); ++key_index) {
                        const auto key = localization->entries[key_index].key;
                        reference_keys.push_back(key);
                        localization_key_indices.emplace(
                            std::string{symbols.view(key)}, static_cast<std::uint32_t>(key_index));
                    }
                } else {
                    if (localization->entries.size() != reference_keys.size()) {
                        return std::unexpected(game_error(
                            DiagnosticCode::game_localization_invalid,
                            "Every locale must contain the same keys", localization->path.string(), "/strings"));
                    }
                    for (std::size_t key_index = 0U; key_index < reference_keys.size(); ++key_index) {
                        if (symbols.view(localization->entries[key_index].key) !=
                            symbols.view(reference_keys[key_index])) {
                            return std::unexpected(game_error(
                                DiagnosticCode::game_localization_invalid,
                                "Every locale must contain identical ordered keys", localization->path.string(),
                                "/strings"));
                        }
                    }
                }
                plan.localizations.push_back(std::move(*localization));
            }
            auto default_locale = string_value(
                *document, "default_locale", source_name, "/",
                DiagnosticCode::game_localization_invalid);
            if (!default_locale) return std::unexpected(std::move(default_locale.error()));
            auto locale_index = index_by_name(
                locale_indices, *default_locale, "locale", source_name, "/default_locale",
                DiagnosticCode::game_localization_invalid);
            if (!locale_index) return std::unexpected(std::move(locale_index.error()));
            plan.default_locale_index = *locale_index;
        } else if (optional(*document, "default_locale") != nullptr) {
            return std::unexpected(game_error(
                DiagnosticCode::game_localization_invalid,
                "default_locale requires localizations", source_name, "/default_locale"));
        }
    }

    std::unordered_map<std::string, std::uint32_t> action_indices{};
    auto actions = required(*document, "actions", source_name, "/");
    if (!actions) return std::unexpected(std::move(actions.error()));
    if (!(*actions)->is_array() || (*actions)->empty() || (*actions)->size() > GamePlan::max_actions) {
        return std::unexpected(game_error(
            DiagnosticCode::game_manifest_invalid, "actions must be a non-empty bounded array", source_name,
            "/actions"));
    }
    plan.actions.reserve((*actions)->size());
    for (std::size_t index = 0U; index < (*actions)->size(); ++index) {
        auto id = string_value((**actions)[index], "id", source_name, "/actions/" + std::to_string(index));
        if (!id) return std::unexpected(std::move(id.error()));
        if (action_indices.contains(*id)) {
            return std::unexpected(game_error(
                DiagnosticCode::game_manifest_invalid, "Duplicate action id", source_name,
                "/actions/" + std::to_string(index) + "/id"));
        }
        auto action = parse_action((**actions)[index], index, symbols, source_name);
        if (!action) return std::unexpected(std::move(action.error()));
        action_indices.emplace(std::move(*id), static_cast<std::uint32_t>(plan.actions.size()));
        plan.actions.push_back(*action);
    }

    std::unordered_map<std::string, std::uint32_t> input_profile_indices{};
    if (version_0_6) {
        auto profiles = required(
            *document, "input_profiles", source_name, "/",
            DiagnosticCode::game_input_profile_invalid);
        if (!profiles) return std::unexpected(std::move(profiles.error()));
        if (!(*profiles)->is_array() || (*profiles)->empty() ||
            (*profiles)->size() > GamePlan::max_input_profiles) {
            return std::unexpected(game_error(
                DiagnosticCode::game_input_profile_invalid,
                "input_profiles must contain one to 16 items", source_name, "/input_profiles"));
        }
        plan.input_profiles.reserve((*profiles)->size());
        for (std::size_t index = 0U; index < (*profiles)->size(); ++index) {
            auto profile = parse_input_profile(
                (**profiles)[index], index, action_indices, plan.actions, symbols, source_name);
            if (!profile) return std::unexpected(std::move(profile.error()));
            const auto id = std::string{symbols.view(profile->symbol)};
            if (!input_profile_indices.emplace(
                    id, static_cast<std::uint32_t>(plan.input_profiles.size())).second) {
                return std::unexpected(game_error(
                    DiagnosticCode::game_input_profile_invalid,
                    "Duplicate input profile id", source_name,
                    "/input_profiles/" + std::to_string(index) + "/id"));
            }
            plan.input_profiles.push_back(std::move(*profile));
        }
        auto default_profile = string_value(
            *document, "default_input_profile", source_name, "/",
            DiagnosticCode::game_input_profile_invalid);
        if (!default_profile) return std::unexpected(std::move(default_profile.error()));
        auto default_index = index_by_name(
            input_profile_indices, *default_profile, "input profile", source_name,
            "/default_input_profile", DiagnosticCode::game_input_profile_invalid);
        if (!default_index) return std::unexpected(std::move(default_index.error()));
        plan.default_input_profile_index = *default_index;
    }

    std::unordered_map<std::string, std::uint32_t> state_indices{};
    auto states = required(*document, "states", source_name, "/");
    if (!states) return std::unexpected(std::move(states.error()));
    if (!(*states)->is_array() || (*states)->size() > GamePlan::max_states) {
        return std::unexpected(game_error(
            DiagnosticCode::game_manifest_invalid, "states must be a bounded array", source_name, "/states"));
    }
    plan.states.reserve((*states)->size());
    for (std::size_t index = 0U; index < (*states)->size(); ++index) {
        auto id = string_value((**states)[index], "id", source_name, "/states/" + std::to_string(index));
        if (!id) return std::unexpected(std::move(id.error()));
        if (state_indices.contains(*id)) {
            return std::unexpected(game_error(
                DiagnosticCode::game_manifest_invalid, "Duplicate state id", source_name,
                "/states/" + std::to_string(index) + "/id"));
        }
        auto state = parse_state((**states)[index], index, symbols, source_name);
        if (!state) return std::unexpected(std::move(state.error()));
        state_indices.emplace(std::move(*id), static_cast<std::uint32_t>(plan.states.size()));
        plan.states.push_back(*state);
    }

    if (version_0_6 && optional(*document, "save") != nullptr) {
        const auto& save = *optional(*document, "save");
        if (auto rejected = reject_unknown(
                save, {"slots", "states"}, source_name, "/save", DiagnosticCode::game_save_invalid);
            !rejected) {
            return std::unexpected(std::move(rejected.error()));
        }
        auto slots = u32_value(
            save, "slots", 1U, 16U, source_name, "/save", DiagnosticCode::game_save_invalid);
        if (!slots) return std::unexpected(std::move(slots.error()));
        auto persistent_states = required(
            save, "states", source_name, "/save", DiagnosticCode::game_save_invalid);
        if (!persistent_states) return std::unexpected(std::move(persistent_states.error()));
        if (!(*persistent_states)->is_array() || (*persistent_states)->size() > plan.states.size()) {
            return std::unexpected(game_error(
                DiagnosticCode::game_save_invalid,
                "save states must be a bounded array", source_name, "/save/states"));
        }
        plan.save.enabled = true;
        plan.save.slot_count = *slots;
        plan.save.state_indices.reserve((*persistent_states)->size());
        std::unordered_set<std::uint32_t> seen_states{};
        for (std::size_t index = 0U; index < (*persistent_states)->size(); ++index) {
            if (!(**persistent_states)[index].is_string()) {
                return std::unexpected(game_error(
                    DiagnosticCode::game_save_invalid,
                    "save state must be a string", source_name,
                    "/save/states/" + std::to_string(index)));
            }
            const auto& persistent_name = (**persistent_states)[index].get_ref<const std::string&>();
            auto state_index = index_by_name(
                state_indices, persistent_name, "integer state", source_name,
                "/save/states/" + std::to_string(index), DiagnosticCode::game_save_invalid);
            if (!state_index) return std::unexpected(std::move(state_index.error()));
            if (!seen_states.insert(*state_index).second) {
                return std::unexpected(game_error(
                    DiagnosticCode::game_save_invalid,
                    "save states contains a duplicate", source_name,
                    "/save/states/" + std::to_string(index)));
            }
            plan.save.state_indices.push_back(*state_index);
        }
    }

    std::unordered_map<std::string, std::uint32_t> scene_indices{};
    auto scenes = required(*document, "scenes", source_name, "/");
    if (!scenes) return std::unexpected(std::move(scenes.error()));
    if (!(*scenes)->is_array() || (*scenes)->empty() || (*scenes)->size() > GamePlan::max_scenes) {
        return std::unexpected(game_error(
            DiagnosticCode::game_manifest_invalid, "scenes must be a non-empty bounded array", source_name,
            "/scenes"));
    }
    plan.scenes.reserve((*scenes)->size());
    for (std::size_t index = 0U; index < (*scenes)->size(); ++index) {
        const auto& declaration = (**scenes)[index];
        const auto pointer = "/scenes/" + std::to_string(index);
        if (auto rejected = reject_unknown(declaration, {"id", "path"}, source_name, pointer); !rejected) {
            return std::unexpected(std::move(rejected.error()));
        }
        auto id = string_value(declaration, "id", source_name, pointer);
        if (!id) return std::unexpected(std::move(id.error()));
        if (scene_indices.contains(*id)) {
            return std::unexpected(game_error(
                DiagnosticCode::game_manifest_invalid, "Duplicate scene id", source_name, pointer + "/id"));
        }
        auto raw_path = string_value(declaration, "path", source_name, pointer);
        if (!raw_path) return std::unexpected(std::move(raw_path.error()));
        auto path = resolve_content_path(root, *raw_path, source_name, pointer + "/path");
        if (!path) return std::unexpected(std::move(path.error()));
        auto scene_text = read_text_file(*path, 4U * 1024U * 1024U, DiagnosticCode::game_scene_invalid);
        if (!scene_text) return std::unexpected(std::move(scene_text.error()));
        source_hash.text(*scene_text);
        auto scene_document = parse_json(*scene_text, path->string(), DiagnosticCode::game_scene_invalid);
        if (!scene_document) return std::unexpected(std::move(scene_document.error()));
        auto scene = parse_scene(
            *scene_document, *path, *id, plan.schema_version, action_indices, state_indices, plan.states,
            asset_indices, plan.assets, animation_indices, plan.animations,
            prefab_indices, prefab_sources, localization_key_indices, locale_indices,
            input_profile_indices, plan.save, plan.window, symbols);
        if (!scene) return std::unexpected(std::move(scene.error()));
        scene_indices.emplace(std::move(*id), static_cast<std::uint32_t>(plan.scenes.size()));
        plan.scenes.push_back(std::move(*scene));
    }
    if (plan.schema_version == GameSchemaVersion::v0_5 ||
        plan.schema_version == GameSchemaVersion::v0_6) {
        std::uint64_t total_contact_pairs = 0U;
        for (const auto& scene : plan.scenes) {
            total_contact_pairs += scene.max_contact_pairs;
            if (total_contact_pairs > 1'000'000U) {
                return std::unexpected(game_error(
                    DiagnosticCode::game_collision_interaction_invalid,
                    "Game scenes exceed the aggregate one-million contact-pair capacity",
                    source_name, "/scenes"));
            }
        }
    }
    if (plan.schema_version == GameSchemaVersion::v0_6) {
        std::uint64_t total_tile_field_cells = 0U;
        std::uint64_t total_particle_slots = 0U;
        for (const auto& scene : plan.scenes) {
            for (const auto& layer : scene.tile_layers) {
                total_tile_field_cells += layer.initial_cells.size();
            }
            for (const auto& field : scene.fields) {
                total_tile_field_cells += field.initial_cells.size();
            }
            for (const auto& emitter : scene.particle_emitters) {
                total_particle_slots += emitter.capacity;
            }
            if (total_tile_field_cells > GamePlan::max_total_tile_field_cells) {
                return std::unexpected(game_error(
                    DiagnosticCode::game_tile_field_invalid,
                    "Game scenes exceed the aggregate four-million tile/field-cell capacity",
                    source_name, "/scenes"));
            }
            if (total_particle_slots > GamePlan::max_total_particle_slots) {
                return std::unexpected(game_error(
                    DiagnosticCode::game_presentation_invalid,
                    "Game scenes exceed the aggregate one-hundred-thousand particle-slot capacity",
                    source_name, "/scenes"));
            }
        }
    }

    auto start = string_value(*document, "start_scene", source_name, "/");
    if (!start) return std::unexpected(std::move(start.error()));
    auto start_index = index_by_name(
        scene_indices, *start, "scene", source_name, "/start_scene", DiagnosticCode::game_manifest_invalid);
    if (!start_index) return std::unexpected(std::move(start_index.error()));
    plan.start_scene = *start_index;

    auto transitions = required(*document, "transitions", source_name, "/");
    if (!transitions) return std::unexpected(std::move(transitions.error()));
    if (!(*transitions)->is_array() || (*transitions)->size() > GamePlan::max_transitions) {
        return std::unexpected(game_error(
            DiagnosticCode::game_manifest_invalid, "transitions must be a bounded array", source_name,
            "/transitions"));
    }
    plan.transitions.reserve((*transitions)->size());
    for (std::size_t index = 0U; index < (*transitions)->size(); ++index) {
        const auto& transition = (**transitions)[index];
        const auto pointer = "/transitions/" + std::to_string(index);
        if (auto rejected = reject_unknown(
                transition, {"from", "condition", "to", "reset_scene", "reset_session"}, source_name, pointer,
                DiagnosticCode::game_transition_invalid);
            !rejected) {
            return std::unexpected(std::move(rejected.error()));
        }
        auto from = string_value(transition, "from", source_name, pointer, DiagnosticCode::game_transition_invalid);
        if (!from) return std::unexpected(std::move(from.error()));
        auto from_index = index_by_name(
            scene_indices, *from, "scene", source_name, pointer + "/from", DiagnosticCode::game_transition_invalid);
        if (!from_index) return std::unexpected(std::move(from_index.error()));
        auto condition_member = required(
            transition, "condition", source_name, pointer, DiagnosticCode::game_transition_invalid);
        if (!condition_member) return std::unexpected(std::move(condition_member.error()));
        auto condition = parse_transition_condition(
            **condition_member, pointer + "/condition", action_indices, state_indices, symbols, source_name);
        if (!condition) return std::unexpected(std::move(condition.error()));
        auto to = string_value(transition, "to", source_name, pointer, DiagnosticCode::game_transition_invalid);
        if (!to) return std::unexpected(std::move(to.error()));
        SceneTransitionPlan item{};
        item.from_scene = *from_index;
        item.condition = *condition;
        if (*to == "$quit") {
            item.quit = true;
        } else {
            auto to_index = index_by_name(
                scene_indices, *to, "scene", source_name, pointer + "/to", DiagnosticCode::game_transition_invalid);
            if (!to_index) return std::unexpected(std::move(to_index.error()));
            item.to_scene = *to_index;
        }
        auto reset_scene = optional_bool(
            transition, "reset_scene", false, source_name, pointer, DiagnosticCode::game_transition_invalid);
        if (!reset_scene) return std::unexpected(std::move(reset_scene.error()));
        auto reset_session = optional_bool(
            transition, "reset_session", false, source_name, pointer, DiagnosticCode::game_transition_invalid);
        if (!reset_session) return std::unexpected(std::move(reset_session.error()));
        item.reset_scene = *reset_scene;
        item.reset_session = *reset_session;
        if (item.condition.kind == TransitionConditionKind::group_inactive) {
            bool found_group = false;
            for (const auto& spawn : plan.scenes[item.from_scene].spawn_groups) {
                found_group = found_group || (spawn.has_collider && spawn.collider.group == item.condition.group);
            }
            if (!found_group) {
                return std::unexpected(game_error(
                    DiagnosticCode::game_transition_invalid,
                    "group_inactive references a group absent from the source scene",
                    source_name,
                    pointer + "/condition/group"));
            }
        }
        plan.transitions.push_back(item);
    }

    if (const auto* fps_actions = optional(*document, "fps_actions"); fps_actions != nullptr) {
        if (!fps_actions->is_array() || fps_actions->size() > plan.actions.size()) {
            return std::unexpected(game_error(
                DiagnosticCode::game_manifest_invalid, "fps_actions must be a bounded array", source_name,
                "/fps_actions"));
        }
        std::unordered_set<std::uint32_t> bound_actions{};
        plan.fps_actions.reserve(fps_actions->size());
        for (std::size_t index = 0U; index < fps_actions->size(); ++index) {
            const auto& item = (*fps_actions)[index];
            const auto pointer = "/fps_actions/" + std::to_string(index);
            if (auto rejected = reject_unknown(item, {"action", "cap"}, source_name, pointer); !rejected) {
                return std::unexpected(std::move(rejected.error()));
            }
            auto action = string_value(item, "action", source_name, pointer);
            if (!action) return std::unexpected(std::move(action.error()));
            auto action_index = index_by_name(
                action_indices, *action, "action", source_name, pointer + "/action",
                DiagnosticCode::game_manifest_invalid);
            if (!action_index) return std::unexpected(std::move(action_index.error()));
            if (!bound_actions.insert(*action_index).second) {
                return std::unexpected(game_error(
                    DiagnosticCode::game_manifest_invalid, "Action has multiple FPS bindings", source_name,
                    pointer + "/action"));
            }
            auto cap_text = string_value(item, "cap", source_name, pointer);
            if (!cap_text) return std::unexpected(std::move(cap_text.error()));
            auto cap = parse_fps(*cap_text, source_name, pointer + "/cap");
            if (!cap) return std::unexpected(std::move(cap.error()));
            plan.fps_actions.push_back({*action_index, *cap});
        }
    }

    for (const auto& scene : plan.scenes) {
        if (auto validated = validate_ui_text(scene, plan.assets, state_indices, plan.states, symbols); !validated) {
            return std::unexpected(std::move(validated.error()));
        }
    }
    plan.source_hash = source_hash.value();
    plan.symbols = symbols.take();
    plan.plan_hash = compute_game_plan_hash(plan);
    if (auto validated = validate_game_plan(plan); !validated) {
        return std::unexpected(std::move(validated.error()));
    }
    return plan;
}

Result<GameInputScriptPlan> compile_game_input_file(
    const std::filesystem::path& input_path,
    const GamePlan& game_plan) {
    auto text = read_text_file(input_path, 64U * 1024U * 1024U, DiagnosticCode::input_invalid);
    if (!text) return std::unexpected(std::move(text.error()));
    auto document = parse_json(*text, input_path.string(), DiagnosticCode::input_invalid);
    if (!document) return std::unexpected(std::move(document.error()));
    const auto source = input_path.string();
    if (auto rejected = reject_unknown(
            *document, {"schema_version", "events"}, source, "/", DiagnosticCode::input_invalid);
        !rejected) {
        return std::unexpected(std::move(rejected.error()));
    }
    auto version = string_value(*document, "schema_version", source, "/", DiagnosticCode::input_invalid);
    if (!version) return std::unexpected(std::move(version.error()));
    if (*version != "1") {
        return std::unexpected(game_error(
            DiagnosticCode::input_invalid, "Unsupported game input schema version", source, "/schema_version"));
    }
    auto events = required(*document, "events", source, "/", DiagnosticCode::input_invalid);
    if (!events) return std::unexpected(std::move(events.error()));
    if (!(*events)->is_array() || (*events)->size() > GameInputScriptPlan::max_events) {
        return std::unexpected(game_error(
            DiagnosticCode::input_invalid, "events must be an array with at most one million items", source,
            "/events"));
    }
    std::unordered_map<std::string, std::uint32_t> action_indices{};
    action_indices.reserve(game_plan.actions.size());
    for (std::size_t index = 0U; index < game_plan.actions.size(); ++index) {
        action_indices.emplace(
            std::string{game_plan.symbol(game_plan.actions[index].symbol)}, static_cast<std::uint32_t>(index));
    }
    GameInputScriptPlan plan{};
    plan.events.reserve((*events)->size());
    std::uint64_t previous_tick = 0U;
    bool first = true;
    std::vector<std::uint32_t> actions_at_tick{};
    actions_at_tick.reserve(game_plan.actions.size());
    for (std::size_t index = 0U; index < (*events)->size(); ++index) {
        const auto& item = (**events)[index];
        const auto pointer = "/events/" + std::to_string(index);
        if (auto rejected = reject_unknown(
                item, {"tick", "action", "kind"}, source, pointer, DiagnosticCode::input_invalid);
            !rejected) {
            return std::unexpected(std::move(rejected.error()));
        }
        auto tick = u64_value(
            item, "tick", 0U, std::numeric_limits<std::uint64_t>::max(), source, pointer,
            DiagnosticCode::input_invalid);
        if (!tick) return std::unexpected(std::move(tick.error()));
        if (!first && *tick < previous_tick) {
            return std::unexpected(game_error(
                DiagnosticCode::input_invalid, "Input event ticks must be nondecreasing", source,
                pointer + "/tick"));
        }
        if (first || *tick != previous_tick) actions_at_tick.clear();
        auto action = string_value(item, "action", source, pointer, DiagnosticCode::input_invalid);
        if (!action) return std::unexpected(std::move(action.error()));
        auto action_index = index_by_name(
            action_indices, *action, "action", source, pointer + "/action", DiagnosticCode::input_invalid);
        if (!action_index) return std::unexpected(std::move(action_index.error()));
        if (std::find(actions_at_tick.begin(), actions_at_tick.end(), *action_index) != actions_at_tick.end()) {
            return std::unexpected(game_error(
                DiagnosticCode::input_invalid, "Duplicate action event at the same tick", source, pointer));
        }
        actions_at_tick.push_back(*action_index);
        auto kind = string_value(item, "kind", source, pointer, DiagnosticCode::input_invalid);
        if (!kind) return std::unexpected(std::move(kind.error()));
        GameInputEventKind parsed_kind{};
        if (*kind == "press") parsed_kind = GameInputEventKind::press;
        else if (*kind == "release") parsed_kind = GameInputEventKind::release;
        else if (*kind == "tap") parsed_kind = GameInputEventKind::tap;
        else {
            return std::unexpected(game_error(
                DiagnosticCode::input_invalid, "Input event kind must be press, release, or tap", source,
                pointer + "/kind"));
        }
        plan.events.push_back({*tick, *action_index, parsed_kind});
        plan.last_tick = *tick;
        previous_tick = *tick;
        first = false;
    }
    return plan;
}

Result<GameTestScriptPlan> compile_game_test_file(
    const std::filesystem::path& test_path,
    const GamePlan& game_plan) {
    auto text = read_text_file(test_path, 64U * 1024U * 1024U, DiagnosticCode::input_invalid);
    if (!text) return std::unexpected(std::move(text.error()));
    auto document = parse_json(*text, test_path.string(), DiagnosticCode::input_invalid);
    if (!document) return std::unexpected(std::move(document.error()));
    const auto source = test_path.string();
    if (auto rejected = reject_unknown(
            *document, {"schema_version", "frames", "events", "assertions"}, source, "/",
            DiagnosticCode::input_invalid); !rejected) {
        return std::unexpected(std::move(rejected.error()));
    }
    auto version = string_value(*document, "schema_version", source, "/", DiagnosticCode::input_invalid);
    if (!version) return std::unexpected(std::move(version.error()));
    if (*version != "1") {
        return std::unexpected(game_error(
            DiagnosticCode::input_invalid, "Unsupported game-test schema version", source, "/schema_version"));
    }
    auto frames = u32_value(
        *document, "frames", 1U, GameTestScriptPlan::max_frames, source, "/", DiagnosticCode::input_invalid);
    if (!frames) return std::unexpected(std::move(frames.error()));
    auto event_members = required(*document, "events", source, "/", DiagnosticCode::input_invalid);
    if (!event_members) return std::unexpected(std::move(event_members.error()));
    auto assertion_members = required(*document, "assertions", source, "/", DiagnosticCode::input_invalid);
    if (!assertion_members) return std::unexpected(std::move(assertion_members.error()));
    if (!(*event_members)->is_array() || (*event_members)->size() > GameTestScriptPlan::max_events ||
        !(*assertion_members)->is_array() || (*assertion_members)->size() > GameTestScriptPlan::max_assertions) {
        return std::unexpected(game_error(
            DiagnosticCode::input_invalid, "game-test events or assertions exceed their bounded limits", source,
            "/"));
    }
    std::unordered_map<std::string, std::uint32_t> actions{};
    std::unordered_map<std::string, std::uint32_t> states{};
    std::unordered_map<std::string, std::uint32_t> scenes{};
    actions.reserve(game_plan.actions.size());
    states.reserve(game_plan.states.size());
    scenes.reserve(game_plan.scenes.size());
    for (std::size_t index = 0U; index < game_plan.actions.size(); ++index) {
        actions.emplace(std::string{game_plan.symbol(game_plan.actions[index].symbol)}, static_cast<std::uint32_t>(index));
    }
    for (std::size_t index = 0U; index < game_plan.states.size(); ++index) {
        states.emplace(std::string{game_plan.symbol(game_plan.states[index].symbol)}, static_cast<std::uint32_t>(index));
    }
    for (std::size_t index = 0U; index < game_plan.scenes.size(); ++index) {
        scenes.emplace(std::string{game_plan.symbol(game_plan.scenes[index].symbol)}, static_cast<std::uint32_t>(index));
    }
    const auto parse_comparison_value = [&](const Json& object,
                                             const std::string_view pointer) -> Result<GameComparison> {
        auto value = string_value(object, "op", source, pointer, DiagnosticCode::input_invalid);
        if (!value) return std::unexpected(std::move(value.error()));
        if (*value == "eq") return GameComparison::equal;
        if (*value == "ne") return GameComparison::not_equal;
        if (*value == "lt") return GameComparison::less;
        if (*value == "le") return GameComparison::less_equal;
        if (*value == "gt") return GameComparison::greater;
        if (*value == "ge") return GameComparison::greater_equal;
        return std::unexpected(game_error(
            DiagnosticCode::input_invalid, "Assertion comparison must be eq, ne, lt, le, gt, or ge", source,
            std::string{pointer} + "/op"));
    };
    const auto find_group = [&](const std::uint32_t scene_index,
                                const std::string_view name,
                                const std::string_view pointer) -> Result<std::uint32_t> {
        const auto& groups = game_plan.scenes[scene_index].spawn_groups;
        const auto found = std::find_if(groups.begin(), groups.end(), [&](const GameSpawnGroupPlan& group) {
            return game_plan.symbol(group.symbol) == name;
        });
        if (found == groups.end()) {
            return std::unexpected(game_error(
                DiagnosticCode::input_invalid, "Assertion references an unknown spawn group", source, pointer));
        }
        return static_cast<std::uint32_t>(std::distance(groups.begin(), found));
    };
    GameTestScriptPlan plan{};
    plan.frames = *frames;
    plan.events.reserve((*event_members)->size());
    std::uint64_t previous_event_tick = 0U;
    bool first_event = true;
    std::vector<std::uint32_t> actions_at_tick{};
    actions_at_tick.reserve(game_plan.actions.size());
    for (std::size_t index = 0U; index < (*event_members)->size(); ++index) {
        const auto& item = (**event_members)[index];
        const auto pointer = "/events/" + std::to_string(index);
        if (auto rejected = reject_unknown(
                item, {"tick", "action", "kind"}, source, pointer, DiagnosticCode::input_invalid); !rejected) {
            return std::unexpected(std::move(rejected.error()));
        }
        auto tick = u64_value(item, "tick", 0U, plan.frames - 1U, source, pointer, DiagnosticCode::input_invalid);
        if (!tick) return std::unexpected(std::move(tick.error()));
        if (!first_event && *tick < previous_event_tick) {
            return std::unexpected(game_error(
                DiagnosticCode::input_invalid, "Test input ticks must be nondecreasing", source, pointer + "/tick"));
        }
        if (first_event || *tick != previous_event_tick) actions_at_tick.clear();
        auto action = string_value(item, "action", source, pointer, DiagnosticCode::input_invalid);
        if (!action) return std::unexpected(std::move(action.error()));
        auto action_index = index_by_name(
            actions, *action, "action", source, pointer + "/action", DiagnosticCode::input_invalid);
        if (!action_index) return std::unexpected(std::move(action_index.error()));
        if (std::find(actions_at_tick.begin(), actions_at_tick.end(), *action_index) != actions_at_tick.end()) {
            return std::unexpected(game_error(
                DiagnosticCode::input_invalid, "Duplicate test action at the same tick", source, pointer));
        }
        actions_at_tick.push_back(*action_index);
        auto kind = string_value(item, "kind", source, pointer, DiagnosticCode::input_invalid);
        if (!kind) return std::unexpected(std::move(kind.error()));
        GameInputEventKind event_kind{};
        if (*kind == "press") event_kind = GameInputEventKind::press;
        else if (*kind == "release") event_kind = GameInputEventKind::release;
        else if (*kind == "tap") event_kind = GameInputEventKind::tap;
        else return std::unexpected(game_error(
            DiagnosticCode::input_invalid, "Test input kind must be press, release, or tap", source,
            pointer + "/kind"));
        plan.events.push_back({*tick, *action_index, event_kind});
        previous_event_tick = *tick;
        first_event = false;
    }
    plan.assertions.reserve((*assertion_members)->size());
    std::uint64_t previous_assertion_tick = 0U;
    bool first_assertion = true;
    for (std::size_t index = 0U; index < (*assertion_members)->size(); ++index) {
        const auto& item = (**assertion_members)[index];
        const auto pointer = "/assertions/" + std::to_string(index);
        if (auto rejected = reject_unknown(
                item, {"tick", "kind", "scene", "state", "group", "index", "op", "value", "active",
                       "expected", "tolerance", "metric", "layer", "field", "x", "y"},
                source, pointer, DiagnosticCode::input_invalid); !rejected) {
            return std::unexpected(std::move(rejected.error()));
        }
        auto tick = u64_value(item, "tick", 0U, plan.frames - 1U, source, pointer, DiagnosticCode::input_invalid);
        if (!tick) return std::unexpected(std::move(tick.error()));
        if (!first_assertion && *tick < previous_assertion_tick) {
            return std::unexpected(game_error(
                DiagnosticCode::input_invalid, "Test assertion ticks must be nondecreasing", source,
                pointer + "/tick"));
        }
        auto kind = string_value(item, "kind", source, pointer, DiagnosticCode::input_invalid);
        if (!kind) return std::unexpected(std::move(kind.error()));
        GameTestAssertionPlan assertion{};
        assertion.tick = *tick;
        const auto parse_scene = [&]() -> Result<std::uint32_t> {
            auto name = string_value(item, "scene", source, pointer, DiagnosticCode::input_invalid);
            if (!name) return std::unexpected(std::move(name.error()));
            return index_by_name(scenes, *name, "scene", source, pointer + "/scene", DiagnosticCode::input_invalid);
        };
        if (*kind == "current_scene") {
            if (auto rejected = reject_unknown(
                    item, {"tick", "kind", "scene"}, source, pointer,
                    DiagnosticCode::input_invalid); !rejected) {
                return std::unexpected(std::move(rejected.error()));
            }
            assertion.kind = GameTestAssertionKind::current_scene;
            auto scene = parse_scene();
            if (!scene) return std::unexpected(std::move(scene.error()));
            assertion.scene_index = *scene;
        } else if (*kind == "int_state") {
            if (auto rejected = reject_unknown(
                    item, {"tick", "kind", "state", "op", "value"}, source, pointer,
                    DiagnosticCode::input_invalid); !rejected) {
                return std::unexpected(std::move(rejected.error()));
            }
            assertion.kind = GameTestAssertionKind::int_state;
            auto state = string_value(item, "state", source, pointer, DiagnosticCode::input_invalid);
            if (!state) return std::unexpected(std::move(state.error()));
            auto state_index = index_by_name(
                states, *state, "integer state", source, pointer + "/state", DiagnosticCode::input_invalid);
            if (!state_index) return std::unexpected(std::move(state_index.error()));
            auto comparison = parse_comparison_value(item, pointer);
            if (!comparison) return std::unexpected(std::move(comparison.error()));
            auto value = i32_value(
                item, "value", std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max(),
                source, pointer, DiagnosticCode::input_invalid);
            if (!value) return std::unexpected(std::move(value.error()));
            assertion.state_index = *state_index;
            assertion.comparison = *comparison;
            assertion.expected_integer = *value;
        } else if (*kind == "group_active_count" || *kind == "entity_active" ||
                   *kind == "position" || *kind == "velocity" || *kind == "animation_frame") {
            auto scene = parse_scene();
            if (!scene) return std::unexpected(std::move(scene.error()));
            auto group_name = string_value(item, "group", source, pointer, DiagnosticCode::input_invalid);
            if (!group_name) return std::unexpected(std::move(group_name.error()));
            auto group = find_group(*scene, *group_name, pointer + "/group");
            if (!group) return std::unexpected(std::move(group.error()));
            assertion.scene_index = *scene;
            assertion.spawn_group_index = *group;
            if (*kind == "group_active_count") {
                if (auto rejected = reject_unknown(
                        item, {"tick", "kind", "scene", "group", "op", "value"}, source, pointer,
                        DiagnosticCode::input_invalid); !rejected) {
                    return std::unexpected(std::move(rejected.error()));
                }
                assertion.kind = GameTestAssertionKind::group_active_count;
                auto comparison = parse_comparison_value(item, pointer);
                if (!comparison) return std::unexpected(std::move(comparison.error()));
                auto value = u32_value(
                    item, "value", 0U, game_plan.scenes[*scene].spawn_groups[*group].count,
                    source, pointer, DiagnosticCode::input_invalid);
                if (!value) return std::unexpected(std::move(value.error()));
                assertion.comparison = *comparison;
                assertion.expected_unsigned = *value;
            } else {
                auto entity_index = u32_value(
                    item, "index", 0U, game_plan.scenes[*scene].spawn_groups[*group].count - 1U,
                    source, pointer, DiagnosticCode::input_invalid);
                if (!entity_index) return std::unexpected(std::move(entity_index.error()));
                assertion.item_index = *entity_index;
                if (*kind == "entity_active") {
                    if (auto rejected = reject_unknown(
                            item, {"tick", "kind", "scene", "group", "index", "active"}, source,
                            pointer, DiagnosticCode::input_invalid); !rejected) {
                        return std::unexpected(std::move(rejected.error()));
                    }
                    assertion.kind = GameTestAssertionKind::entity_active;
                    auto active = required(item, "active", source, pointer, DiagnosticCode::input_invalid);
                    if (!active) return std::unexpected(std::move(active.error()));
                    if (!(**active).is_boolean()) return std::unexpected(game_error(
                        DiagnosticCode::input_invalid, "entity_active expected value must be boolean", source,
                        pointer + "/active"));
                    assertion.expected_active = (**active).get<bool>();
                } else if (*kind == "animation_frame") {
                    if (auto rejected = reject_unknown(
                            item, {"tick", "kind", "scene", "group", "index", "op", "value"},
                            source, pointer, DiagnosticCode::input_invalid); !rejected) {
                        return std::unexpected(std::move(rejected.error()));
                    }
                    if (!game_plan.scenes[*scene].spawn_groups[*group].has_sprite) {
                        return std::unexpected(game_error(
                            DiagnosticCode::input_invalid,
                            "Animation assertion target does not have Sprite2D", source, pointer + "/group"));
                    }
                    assertion.kind = GameTestAssertionKind::animation_frame;
                    auto comparison = parse_comparison_value(item, pointer);
                    if (!comparison) return std::unexpected(std::move(comparison.error()));
                    auto value = u32_value(
                        item, "value", 0U, static_cast<std::uint32_t>(GameAnimationPlan::max_frames - 1U),
                        source, pointer, DiagnosticCode::input_invalid);
                    if (!value) return std::unexpected(std::move(value.error()));
                    assertion.comparison = *comparison;
                    assertion.expected_unsigned = *value;
                } else {
                    if (auto rejected = reject_unknown(
                            item, {"tick", "kind", "scene", "group", "index", "expected", "tolerance"},
                            source, pointer, DiagnosticCode::input_invalid); !rejected) {
                        return std::unexpected(std::move(rejected.error()));
                    }
                    assertion.kind = *kind == "position" ? GameTestAssertionKind::position
                                                          : GameTestAssertionKind::velocity;
                    if (assertion.kind == GameTestAssertionKind::velocity &&
                        !game_plan.scenes[*scene].spawn_groups[*group].has_velocity) {
                        return std::unexpected(game_error(
                            DiagnosticCode::input_invalid,
                            "Velocity assertion target does not have Velocity2D",
                            source, pointer + "/group"));
                    }
                    auto expected = required(item, "expected", source, pointer, DiagnosticCode::input_invalid);
                    if (!expected) return std::unexpected(std::move(expected.error()));
                    auto vector = vec2(**expected, source, pointer + "/expected", DiagnosticCode::input_invalid);
                    if (!vector) return std::unexpected(std::move(vector.error()));
                    auto tolerance = optional_number(item, "tolerance", 1.0e-5F, source, pointer,
                                                     DiagnosticCode::input_invalid);
                    if (!tolerance) return std::unexpected(std::move(tolerance.error()));
                    if (*tolerance < 0.0F || *tolerance > 1.0F) return std::unexpected(game_error(
                        DiagnosticCode::input_invalid, "Assertion tolerance must be from zero to one", source,
                        pointer + "/tolerance"));
                    assertion.expected_vector = *vector;
                    assertion.tolerance = *tolerance;
                }
            }
        } else if (*kind == "tile_value" || *kind == "field_value") {
            const bool tile_assertion = *kind == "tile_value";
            if (auto rejected = reject_unknown(
                    item, tile_assertion
                              ? std::initializer_list<std::string_view>{
                                    "tick", "kind", "scene", "layer", "x", "y", "op", "value"}
                              : std::initializer_list<std::string_view>{
                                    "tick", "kind", "scene", "field", "x", "y", "op", "value"},
                    source, pointer, DiagnosticCode::input_invalid); !rejected) {
                return std::unexpected(std::move(rejected.error()));
            }
            auto scene = parse_scene();
            if (!scene) return std::unexpected(std::move(scene.error()));
            assertion.scene_index = *scene;
            const auto& scene_plan = game_plan.scenes[*scene];
            std::uint32_t grid_index = 0U;
            if (tile_assertion) {
                auto name = string_value(item, "layer", source, pointer, DiagnosticCode::input_invalid);
                if (!name) return std::unexpected(std::move(name.error()));
                const auto found = std::find_if(
                    scene_plan.tile_layers.begin(), scene_plan.tile_layers.end(), [&](const GameTileLayerPlan& layer) {
                        return game_plan.symbol(layer.symbol) == *name;
                    });
                if (found == scene_plan.tile_layers.end()) return std::unexpected(game_error(
                    DiagnosticCode::input_invalid, "Assertion references an unknown tile layer", source,
                    pointer + "/layer"));
                assertion.kind = GameTestAssertionKind::tile_value;
                assertion.tile_layer_index = static_cast<std::uint32_t>(
                    std::distance(scene_plan.tile_layers.begin(), found));
                grid_index = found->grid_index;
                auto value = u32_value(
                    item, "value", 0U, found->atlas_columns * found->atlas_rows,
                    source, pointer, DiagnosticCode::input_invalid);
                if (!value) return std::unexpected(std::move(value.error()));
                assertion.expected_unsigned = *value;
            } else {
                auto name = string_value(item, "field", source, pointer, DiagnosticCode::input_invalid);
                if (!name) return std::unexpected(std::move(name.error()));
                const auto found = std::find_if(
                    scene_plan.fields.begin(), scene_plan.fields.end(), [&](const GameFieldPlan& field) {
                        return game_plan.symbol(field.symbol) == *name;
                    });
                if (found == scene_plan.fields.end()) return std::unexpected(game_error(
                    DiagnosticCode::input_invalid, "Assertion references an unknown integer field", source,
                    pointer + "/field"));
                assertion.kind = GameTestAssertionKind::field_value;
                assertion.field_index = static_cast<std::uint32_t>(
                    std::distance(scene_plan.fields.begin(), found));
                grid_index = found->grid_index;
                auto value = i32_value(
                    item, "value", found->minimum, found->maximum, source, pointer,
                    DiagnosticCode::input_invalid);
                if (!value) return std::unexpected(std::move(value.error()));
                assertion.expected_integer = *value;
            }
            const auto& grid = scene_plan.grids[grid_index];
            auto x = u32_value(item, "x", 0U, grid.columns - 1U, source, pointer, DiagnosticCode::input_invalid);
            if (!x) return std::unexpected(std::move(x.error()));
            auto y = u32_value(item, "y", 0U, grid.rows - 1U, source, pointer, DiagnosticCode::input_invalid);
            if (!y) return std::unexpected(std::move(y.error()));
            auto comparison = parse_comparison_value(item, pointer);
            if (!comparison) return std::unexpected(std::move(comparison.error()));
            assertion.cell_x = *x;
            assertion.cell_y = *y;
            assertion.comparison = *comparison;
        } else if (*kind == "camera_position") {
            if (auto rejected = reject_unknown(
                    item, {"tick", "kind", "scene", "expected", "tolerance"}, source, pointer,
                    DiagnosticCode::input_invalid); !rejected) {
                return std::unexpected(std::move(rejected.error()));
            }
            assertion.kind = GameTestAssertionKind::camera_position;
            auto scene = parse_scene();
            if (!scene) return std::unexpected(std::move(scene.error()));
            auto expected = required(item, "expected", source, pointer, DiagnosticCode::input_invalid);
            if (!expected) return std::unexpected(std::move(expected.error()));
            auto vector = vec2(**expected, source, pointer + "/expected", DiagnosticCode::input_invalid);
            if (!vector) return std::unexpected(std::move(vector.error()));
            auto tolerance = optional_number(
                item, "tolerance", 1.0e-5F, source, pointer, DiagnosticCode::input_invalid);
            if (!tolerance) return std::unexpected(std::move(tolerance.error()));
            if (*tolerance < 0.0F || *tolerance > 1.0F) return std::unexpected(game_error(
                DiagnosticCode::input_invalid, "Assertion tolerance must be from zero to one", source,
                pointer + "/tolerance"));
            assertion.scene_index = *scene;
            assertion.expected_vector = *vector;
            assertion.tolerance = *tolerance;
        } else if (*kind == "runtime_metric") {
            if (auto rejected = reject_unknown(
                    item, {"tick", "kind", "metric", "op", "value"}, source, pointer,
                    DiagnosticCode::input_invalid); !rejected) {
                return std::unexpected(std::move(rejected.error()));
            }
            assertion.kind = GameTestAssertionKind::runtime_metric;
            auto metric = string_value(item, "metric", source, pointer, DiagnosticCode::input_invalid);
            if (!metric) return std::unexpected(std::move(metric.error()));
            const std::pair<std::string_view, GameTestMetric> metrics[] = {
                {"rule_executions", GameTestMetric::rule_executions},
                {"condition_evaluations", GameTestMetric::condition_evaluations},
                {"action_executions", GameTestMetric::action_executions},
                {"grid_steps", GameTestMetric::grid_steps},
                {"rejected_direction_changes", GameTestMetric::rejected_direction_changes},
                {"follower_updates", GameTestMetric::follower_updates},
                {"active_state_changes", GameTestMetric::active_state_changes},
                {"relocations", GameTestMetric::relocations},
                {"relocation_cells_scanned", GameTestMetric::relocation_cells_scanned},
                {"collision_contacts", GameTestMetric::collision_contacts},
                {"trigger_narrowphase_tests", GameTestMetric::trigger_narrowphase_tests},
                {"contact_begins", GameTestMetric::contact_begins},
                {"contact_ends", GameTestMetric::contact_ends},
                {"stale_contact_events", GameTestMetric::stale_contact_events},
                {"active_contact_pairs", GameTestMetric::active_contact_pairs},
                {"peak_contact_pairs", GameTestMetric::peak_contact_pairs},
                {"motion_segments", GameTestMetric::motion_segments},
                {"linear_motion_updates", GameTestMetric::linear_motion_updates},
                {"pool_acquire_attempts", GameTestMetric::pool_acquire_attempts},
                {"pool_acquire_successes", GameTestMetric::pool_acquire_successes},
                {"pool_releases", GameTestMetric::pool_releases},
                {"pool_release_misses", GameTestMetric::pool_release_misses},
                {"pool_exhaustions", GameTestMetric::pool_exhaustions},
                {"pool_recycled_slots", GameTestMetric::pool_recycled_slots},
                {"pool_expirations", GameTestMetric::pool_expirations},
                {"pool_resets", GameTestMetric::pool_resets},
                {"pool_lifetime_checks", GameTestMetric::pool_lifetime_checks},
                {"active_pooled_entities", GameTestMetric::active_pooled_entities},
                {"peak_active_pooled_entities", GameTestMetric::peak_active_pooled_entities},
                {"animation_frame_updates", GameTestMetric::animation_frame_updates},
                {"animation_completions", GameTestMetric::animation_completions},
                {"tile_reads", GameTestMetric::tile_reads},
                {"tile_writes", GameTestMetric::tile_writes},
                {"field_reads", GameTestMetric::field_reads},
                {"field_writes", GameTestMetric::field_writes},
                {"save_attempts", GameTestMetric::save_attempts},
                {"save_successes", GameTestMetric::save_successes},
                {"save_failures", GameTestMetric::save_failures},
                {"particle_emits", GameTestMetric::particle_emits},
                {"particle_updates", GameTestMetric::particle_updates},
                {"particle_exhaustions", GameTestMetric::particle_exhaustions},
                {"particle_slot_operations", GameTestMetric::particle_slot_operations},
                {"peak_active_particles", GameTestMetric::peak_active_particles},
                {"camera_follow_updates", GameTestMetric::camera_follow_updates},
                {"camera_shake_updates", GameTestMetric::camera_shake_updates},
                {"music_stream_bytes", GameTestMetric::music_stream_bytes},
                {"music_underruns", GameTestMetric::music_underruns},
                {"input_profile_switches", GameTestMetric::input_profile_switches},
                {"locale_switches", GameTestMetric::locale_switches},
            };
            const auto found = std::find_if(std::begin(metrics), std::end(metrics), [&](const auto& pair) {
                return pair.first == *metric;
            });
            if (found == std::end(metrics)) return std::unexpected(game_error(
                DiagnosticCode::input_invalid, "Assertion references an unsupported runtime metric", source,
                pointer + "/metric"));
            auto comparison = parse_comparison_value(item, pointer);
            if (!comparison) return std::unexpected(std::move(comparison.error()));
            auto value = u64_value(
                item, "value", 0U, std::numeric_limits<std::uint64_t>::max(), source, pointer,
                DiagnosticCode::input_invalid);
            if (!value) return std::unexpected(std::move(value.error()));
            assertion.metric = found->second;
            assertion.comparison = *comparison;
            assertion.expected_unsigned = *value;
        } else {
            return std::unexpected(game_error(
                DiagnosticCode::input_invalid, "Unsupported game-test assertion kind", source,
                pointer + "/kind"));
        }
        plan.assertions.push_back(assertion);
        previous_assertion_tick = *tick;
        first_assertion = false;
    }
    return plan;
}

void write_game_summary_json(JsonWriter& writer, const GamePlan& plan) {
    writer.begin_object();
    writer.key("schema_version");
    writer.value(plan.schema_version_text());
    writer.key("name");
    writer.value(plan.symbol(plan.name));
    writer.key("application");
    writer.value(plan.symbol(plan.application));
    if (plan.schema_version != GameSchemaVersion::v0_2) {
        writer.key("seed");
        writer.value(plan.seed);
    }
    writer.key("source_hash");
    writer.value(plan.source_hash);
    writer.key("plan_hash");
    writer.value(plan.plan_hash);
    writer.key("start_scene");
    writer.value(plan.symbol(plan.scenes[plan.start_scene].symbol));
    writer.key("default_fps");
    writer.value(to_string(plan.window.default_fps));
    writer.key("assets");
    writer.begin_array();
    for (const auto& asset : plan.assets) {
        writer.begin_object();
        writer.key("id");
        writer.value(plan.symbol(asset.symbol));
        writer.key("kind");
        writer.value(to_string(asset.kind));
        writer.key("path");
        writer.value(asset.path.generic_string());
        writer.end_object();
    }
    writer.end_array();
    writer.key("actions");
    writer.begin_array();
    for (const auto& action : plan.actions) writer.value(plan.symbol(action.symbol));
    writer.end_array();
    if (plan.schema_version == GameSchemaVersion::v0_6) {
        writer.key("animations");
        writer.begin_array();
        for (const auto& animation : plan.animations) {
            writer.begin_object();
            writer.key("id");
            writer.value(plan.symbol(animation.symbol));
            writer.key("texture");
            writer.value(plan.symbol(plan.assets[animation.asset_index].symbol));
            writer.key("mode");
            writer.value(to_string(animation.mode));
            writer.key("frames");
            writer.value(static_cast<std::uint64_t>(animation.frames.size()));
            writer.end_object();
        }
        writer.end_array();
        writer.key("prefabs");
        writer.begin_array();
        for (const auto& prefab : plan.prefabs) writer.value(plan.symbol(prefab.symbol));
        writer.end_array();
        writer.key("locales");
        writer.begin_array();
        for (const auto& locale : plan.localizations) writer.value(plan.symbol(locale.locale));
        writer.end_array();
        writer.key("default_locale");
        if (plan.localizations.empty()) writer.null_value();
        else writer.value(plan.symbol(plan.localizations[plan.default_locale_index].locale));
        writer.key("input_profiles");
        writer.begin_array();
        for (const auto& profile : plan.input_profiles) writer.value(plan.symbol(profile.symbol));
        writer.end_array();
        writer.key("default_input_profile");
        writer.value(plan.symbol(plan.input_profiles[plan.default_input_profile_index].symbol));
        writer.key("save");
        writer.begin_object();
        writer.key("enabled");
        writer.value(plan.save.enabled);
        writer.key("slots");
        writer.value(static_cast<std::uint64_t>(plan.save.slot_count));
        writer.key("state_count");
        writer.value(static_cast<std::uint64_t>(plan.save.state_indices.size()));
        writer.key("capacity_bytes");
        writer.value(compute_game_save_capacity(plan).value_or(0U));
        writer.end_object();
    }
    writer.key("states");
    writer.begin_array();
    for (const auto& state : plan.states) {
        writer.begin_object();
        writer.key("id");
        writer.value(plan.symbol(state.symbol));
        writer.key("initial");
        writer.value(static_cast<std::int64_t>(state.initial));
        writer.end_object();
    }
    writer.end_array();
    writer.key("scenes");
    writer.begin_array();
    for (const auto& scene : plan.scenes) {
        writer.begin_object();
        writer.key("id");
        writer.value(plan.symbol(scene.symbol));
        writer.key("world_capacity");
        writer.value(static_cast<std::uint64_t>(scene.world_capacity));
        writer.key("spawned_entities");
        writer.value(static_cast<std::uint64_t>(scene.total_spawn_count));
        writer.key("max_contact_pairs");
        writer.value(static_cast<std::uint64_t>(scene.max_contact_pairs));
        if (plan.schema_version == GameSchemaVersion::v0_6) {
            writer.key("persistent");
            writer.value(scene.persistent);
            writer.key("camera_mode");
            writer.value(scene.camera.mode == GameCameraMode::follow ? "follow" : "fixed");
            writer.key("tile_layers");
            writer.value(static_cast<std::uint64_t>(scene.tile_layers.size()));
            writer.key("fields");
            writer.value(static_cast<std::uint64_t>(scene.fields.size()));
            writer.key("particle_emitters");
            writer.value(static_cast<std::uint64_t>(scene.particle_emitters.size()));
            writer.key("ui_stacks");
            writer.value(static_cast<std::uint64_t>(scene.ui_stacks.size()));
        }
        writer.key("systems");
        writer.begin_array();
        for (const auto& system : scene.systems) writer.value(to_string(system.operation));
        writer.end_array();
        writer.key("motion_ownership");
        writer.begin_array();
        for (const auto& system : scene.systems) {
            if (system.operation != GameOperationId::linear_motion) continue;
            writer.begin_object();
            writer.key("system");
            writer.value(plan.symbol(system.symbol));
            writer.key("operation");
            writer.value(to_string(system.operation));
            writer.key("spawn_group");
            writer.value(plan.symbol(scene.spawn_groups[system.spawn_group_index].symbol));
            writer.end_object();
        }
        writer.end_array();
        writer.key("collision_rules");
        writer.begin_array();
        for (std::size_t collision_index = 0U; collision_index < scene.collision_rules.size(); ++collision_index) {
            const auto& collision_rule = scene.collision_rules[collision_index];
            writer.begin_object();
            writer.key("id");
            if (plan.schema_version == GameSchemaVersion::v0_2) writer.null_value();
            else writer.value(plan.symbol(collision_rule.symbol));
            writer.key("a");
            writer.value(plan.symbol(collision_rule.group_a));
            writer.key("b");
            writer.value(plan.symbol(collision_rule.group_b));
            writer.key("interaction");
            writer.value(to_string(collision_rule.interaction));
            writer.key("event_phases");
            writer.begin_array();
            bool collision_phase = false;
            bool begin_phase = false;
            bool end_phase = false;
            for (const auto& rule : scene.rules) {
                if (rule.event.collision_rule_index != collision_index) continue;
                collision_phase = collision_phase || rule.event.kind == GameRuleEventKind::collision;
                begin_phase = begin_phase || rule.event.kind == GameRuleEventKind::contact_begin;
                end_phase = end_phase || rule.event.kind == GameRuleEventKind::contact_end;
            }
            if (collision_phase) writer.value(to_string(GameRuleEventKind::collision));
            if (begin_phase) writer.value(to_string(GameRuleEventKind::contact_begin));
            if (end_phase) writer.value(to_string(GameRuleEventKind::contact_end));
            writer.end_array();
            writer.end_object();
        }
        writer.end_array();
        writer.key("pools");
        writer.begin_array();
        for (const auto& pool : scene.pools) {
            const auto& group = scene.spawn_groups[pool.spawn_group_index];
            writer.begin_object();
            writer.key("id");
            writer.value(plan.symbol(pool.symbol));
            writer.key("group");
            writer.value(plan.symbol(group.symbol));
            writer.key("capacity");
            writer.value(static_cast<std::uint64_t>(group.count));
            writer.key("initial_active");
            writer.value(static_cast<std::uint64_t>(group.active_count));
            writer.key("on_exhausted");
            writer.value(to_string(pool.on_exhausted));
            writer.end_object();
        }
        writer.end_array();
        writer.key("pool_actions");
        writer.begin_array();
        for (const auto& rule : scene.rules) {
            for (const auto& action : rule.actions) {
                if (action.kind != GameRuleActionKind::spawn_from_pool &&
                    action.kind != GameRuleActionKind::release_to_pool &&
                    action.kind != GameRuleActionKind::reset_pool) {
                    continue;
                }
                writer.begin_object();
                writer.key("rule");
                writer.value(plan.symbol(rule.symbol));
                writer.key("kind");
                writer.value(to_string(action.kind));
                writer.key("pool");
                writer.value(plan.symbol(scene.pools[action.pool_index].symbol));
                writer.end_object();
            }
        }
        writer.end_array();
        writer.end_object();
    }
    writer.end_array();
    writer.key("transition_count");
    writer.value(static_cast<std::uint64_t>(plan.transitions.size()));
    std::vector<std::string> dependencies{};
    dependencies.reserve(1U + plan.scenes.size() + plan.assets.size() * 2U);
    const auto add_dependency = [&](const std::filesystem::path& path) {
        std::error_code error{};
        const auto relative = std::filesystem::relative(path, plan.content_root, error);
        if (!error && !relative.empty() && !relative.is_absolute()) {
            dependencies.push_back(relative.generic_string());
        }
    };
    add_dependency(plan.manifest_path);
    for (const auto& scene : plan.scenes) add_dependency(scene.source_path);
    for (const auto& asset : plan.assets) {
        add_dependency(asset.path);
        if (!asset.metadata_path.empty()) add_dependency(asset.metadata_path);
    }
    for (const auto& prefab : plan.prefabs) add_dependency(prefab.path);
    for (const auto& locale : plan.localizations) add_dependency(locale.path);
    std::sort(dependencies.begin(), dependencies.end());
    dependencies.erase(std::unique(dependencies.begin(), dependencies.end()), dependencies.end());
    writer.key("dependencies");
    writer.begin_array();
    for (const auto& dependency : dependencies) writer.value(dependency);
    writer.end_array();
    writer.end_object();
}

} // namespace ai2d
