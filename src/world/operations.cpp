#include "ai2d/world/operations.hpp"

#include "ai2d/world/components.hpp"
#include "ai2d/world/world.hpp"

#include <cmath>
#include <limits>

namespace ai2d {
namespace {

Diagnostic numeric_error(const char* const message, const EntityId entity = EntityId::invalid()) {
    auto diagnostic = Diagnostic::make(
        DiagnosticCode::runtime_numeric_state_invalid,
        Severity::error,
        "runtime",
        message);
    if (entity.valid()) {
        diagnostic.context.push_back({"entity_index", static_cast<std::uint64_t>(entity.index)});
    }
    return diagnostic;
}

bool fits_finite_float(const double value) noexcept {
    constexpr auto limit = static_cast<double>(std::numeric_limits<float>::max());
    return std::isfinite(value) && value >= -limit && value <= limit;
}

} // namespace

Result<void> integrate_velocity(World& world, const float delta_seconds) {
    if (!std::isfinite(delta_seconds) || delta_seconds <= 0.0F) {
        return std::unexpected(numeric_error("Velocity integration delta must be finite and positive"));
    }
    auto query = world.query<Transform2D, Velocity2D>();
    const auto visited = query.candidate_count();
    std::size_t matched = 0U;
    for (auto item : query) {
        const auto next_x = static_cast<double>(item.first.position.x) +
                            static_cast<double>(item.second.linear.x) * delta_seconds;
        const auto next_y = static_cast<double>(item.first.position.y) +
                            static_cast<double>(item.second.linear.y) * delta_seconds;
        const auto next_rotation = static_cast<double>(item.first.rotation) +
                                   static_cast<double>(item.second.angular) * delta_seconds;
        if (!fits_finite_float(next_x) || !fits_finite_float(next_y) ||
            !fits_finite_float(next_rotation)) {
            world.record_query(visited, matched);
            world.record_system_invocation();
            return std::unexpected(numeric_error(
                "Velocity integration produced a non-finite or out-of-range transform",
                item.entity));
        }
        item.first.position.x = static_cast<float>(next_x);
        item.first.position.y = static_cast<float>(next_y);
        item.first.rotation = static_cast<float>(next_rotation);
        ++matched;
    }
    world.record_query(visited, matched);
    world.record_system_invocation();
    return {};
}

Result<void> wrap_bounds(World& world, const Rect bounds) {
    const auto finite_bounds = std::isfinite(bounds.min.x) && std::isfinite(bounds.min.y) &&
                               std::isfinite(bounds.max.x) && std::isfinite(bounds.max.y);
    const auto width = static_cast<double>(bounds.max.x) - bounds.min.x;
    const auto height = static_cast<double>(bounds.max.y) - bounds.min.y;
    if (!finite_bounds || !(width > 0.0) || !(height > 0.0) ||
        !std::isfinite(width) || !std::isfinite(height)) {
        return std::unexpected(numeric_error("Wrap bounds must be finite with positive extents"));
    }

    auto query = world.query<Transform2D>();
    const auto visited = query.candidate_count();
    std::size_t matched = 0U;
    for (auto item : query) {
        auto& position = item.component.position;
        if (!std::isfinite(position.x) || !std::isfinite(position.y)) {
            world.record_query(visited, matched);
            world.record_system_invocation();
            return std::unexpected(numeric_error("Wrap received a non-finite position", item.entity));
        }

        const auto wrap_axis = [](const float value, const float minimum, const float maximum, const double extent) {
            auto wrapped = std::fmod(static_cast<double>(value) - minimum, extent);
            if (wrapped < 0.0) {
                wrapped += extent;
            }
            auto result = static_cast<double>(minimum) + wrapped;
            if (result >= static_cast<double>(maximum)) {
                result = minimum;
            }
            return static_cast<float>(result);
        };
        position.x = wrap_axis(position.x, bounds.min.x, bounds.max.x, width);
        position.y = wrap_axis(position.y, bounds.min.y, bounds.max.y, height);
        ++matched;
    }
    world.record_query(visited, matched);
    world.record_system_invocation();
    return {};
}

} // namespace ai2d
