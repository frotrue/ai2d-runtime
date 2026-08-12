#pragma once

#include "ai2d/foundation/diagnostic.hpp"
#include "ai2d/foundation/types.hpp"

#include <filesystem>
#include <optional>
#include <string_view>

namespace ai2d {

struct GameSettings final {
    RenderFpsCap fps_cap{RenderFpsCap::fps_60};
    float master_volume{1.0F};
};

[[nodiscard]] Result<std::filesystem::path> settings_file_path(
    std::string_view organization,
    std::string_view application);
[[nodiscard]] Result<std::optional<GameSettings>> load_game_settings(
    std::string_view organization,
    std::string_view application);
[[nodiscard]] Result<void> save_game_settings(
    std::string_view organization,
    std::string_view application,
    const GameSettings& settings);

} // namespace ai2d
