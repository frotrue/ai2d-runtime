#pragma once

#include <string>

namespace ai2d {

struct BuildInfo final {
    std::string version{};
    std::string compiler{};
    std::string configuration{};
    std::string operating_system{};
    std::string architecture{};
};

[[nodiscard]] BuildInfo current_build_info();

} // namespace ai2d
