#pragma once

#include "ai2d/foundation/diagnostic.hpp"
#include "ai2d/foundation/types.hpp"

namespace ai2d {

class World;

[[nodiscard]] Result<void> integrate_velocity(World& world, float delta_seconds);
[[nodiscard]] Result<void> wrap_bounds(World& world, Rect bounds);

} // namespace ai2d
