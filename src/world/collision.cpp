#include "ai2d/world/collision.hpp"

#include "ai2d/world/components.hpp"
#include "ai2d/world/world.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

namespace ai2d {
namespace {

constexpr float collision_epsilon = 0.0001F;

Diagnostic collision_error(const DiagnosticCode code, const char* const message) {
    return Diagnostic::make(code, Severity::error, "collision", message);
}

bool finite_positive(const float value) noexcept { return std::isfinite(value) && value > 0.0F; }

bool finite_vec2(const Vec2 value) noexcept { return std::isfinite(value.x) && std::isfinite(value.y); }

bool representable_float(const double value) noexcept {
    return std::isfinite(value) &&
           value >= -static_cast<double>(std::numeric_limits<float>::max()) &&
           value <= static_cast<double>(std::numeric_limits<float>::max());
}

std::uint32_t clamped_cell_coordinate(
    const double world_coordinate,
    const float bounds_minimum,
    const float cell_size,
    const std::uint32_t count) noexcept {
    const double scaled = (world_coordinate - static_cast<double>(bounds_minimum)) /
                          static_cast<double>(cell_size);
    if (!std::isfinite(scaled) || scaled <= 0.0) return 0U;
    if (scaled >= static_cast<double>(count)) return count - 1U;
    return static_cast<std::uint32_t>(std::floor(scaled));
}

struct SweepHit final {
    bool hit{false};
    float time{0.0F};
    Vec2 normal{};
    float penetration{0.0F};
};

SweepHit swept_aabb(
    const Vec2 center_a,
    const Vec2 half_a,
    const Vec2 velocity_a,
    const Vec2 center_b,
    const Vec2 half_b,
    const Vec2 velocity_b,
    const float duration) noexcept {
    const Vec2 difference{center_a.x - center_b.x, center_a.y - center_b.y};
    const Vec2 extent{half_a.x + half_b.x, half_a.y + half_b.y};
    const Vec2 relative_velocity{velocity_a.x - velocity_b.x, velocity_a.y - velocity_b.y};

    if (std::abs(difference.x) <= extent.x && std::abs(difference.y) <= extent.y) {
        const float penetration_x = extent.x - std::abs(difference.x);
        const float penetration_y = extent.y - std::abs(difference.y);
        const bool use_x = penetration_x <= penetration_y;
        const Vec2 normal = use_x
                                ? Vec2{difference.x >= 0.0F ? 1.0F : -1.0F, 0.0F}
                                : Vec2{0.0F, difference.y >= 0.0F ? 1.0F : -1.0F};
        const float penetration = use_x ? penetration_x : penetration_y;
        const float normal_velocity = relative_velocity.x * normal.x + relative_velocity.y * normal.y;
        if (penetration <= collision_epsilon && normal_velocity >= 0.0F) {
            return {};
        }
        return {true, 0.0F, normal, penetration};
    }

    float entry_x = -std::numeric_limits<float>::infinity();
    float exit_x = std::numeric_limits<float>::infinity();
    float entry_y = -std::numeric_limits<float>::infinity();
    float exit_y = std::numeric_limits<float>::infinity();

    const auto axis_times = [](const float difference_axis,
                               const float extent_axis,
                               const float velocity_axis,
                               float& entry,
                               float& exit) noexcept {
        if (velocity_axis == 0.0F) {
            return std::abs(difference_axis) <= extent_axis;
        }
        const float first = (-extent_axis - difference_axis) / velocity_axis;
        const float second = (extent_axis - difference_axis) / velocity_axis;
        entry = std::min(first, second);
        exit = std::max(first, second);
        return true;
    };
    if (!axis_times(difference.x, extent.x, relative_velocity.x, entry_x, exit_x) ||
        !axis_times(difference.y, extent.y, relative_velocity.y, entry_y, exit_y)) {
        return {};
    }
    const float entry = std::max(entry_x, entry_y);
    const float exit = std::min(exit_x, exit_y);
    if (entry > exit || exit < 0.0F || entry < 0.0F || entry > duration) {
        return {};
    }
    if (entry_x >= entry_y) {
        return {true, entry, {relative_velocity.x > 0.0F ? -1.0F : 1.0F, 0.0F}, 0.0F};
    }
    return {true, entry, {0.0F, relative_velocity.y > 0.0F ? -1.0F : 1.0F}, 0.0F};
}

} // namespace

class CollisionGrid2D::Impl final {
public:
    struct Proxy final {
        EntityId entity{};
        Transform2D* transform{nullptr};
        Velocity2D* velocity{nullptr};
        Sprite2D* sprite{nullptr};
        Collider2D* collider{nullptr};
        EntityState2D* state{nullptr};
        Vec2 start_center{};
        Vec2 sweep_velocity{};
    };

    struct GridReference final {
        std::uint32_t proxy{0U};
        std::int32_t next{-1};
    };

    CollisionGridConfig2D config{};
    std::uint32_t columns{0U};
    std::uint32_t rows{0U};
    std::vector<Proxy> proxies{};
    std::vector<std::int32_t> cell_heads{};
    std::vector<GridReference> references{};
    std::vector<std::uint64_t> candidate_pairs{};
    std::vector<CollisionEvent2D> collision_events{};
    bool ready{false};

    [[nodiscard]] bool active(const Proxy& proxy) const noexcept {
        return proxy.collider != nullptr && proxy.collider->enabled &&
               (proxy.state == nullptr || proxy.state->active);
    }

    [[nodiscard]] Vec2 current_center(const Proxy& proxy) const noexcept {
        return {
            proxy.transform->position.x + proxy.collider->offset.x,
            proxy.transform->position.y + proxy.collider->offset.y,
        };
    }

    [[nodiscard]] Vec2 velocity(const Proxy& proxy) const noexcept {
        if (proxy.collider->motion == BodyMotion2D::kinematic_body) return proxy.sweep_velocity;
        return proxy.velocity == nullptr || proxy.collider->motion != BodyMotion2D::dynamic_body
                   ? Vec2{}
                   : proxy.velocity->linear;
    }

    [[nodiscard]] Vec2 center_at(const Proxy& proxy, const float elapsed) const noexcept {
        if (proxy.collider->motion == BodyMotion2D::kinematic_body) {
            return {
                proxy.start_center.x + proxy.sweep_velocity.x * elapsed,
                proxy.start_center.y + proxy.sweep_velocity.y * elapsed,
            };
        }
        return current_center(proxy);
    }

    [[nodiscard]] const CollisionRule2D* find_rule(
        const Proxy& first,
        const Proxy& second,
        const std::span<const CollisionRule2D> rules,
        std::uint32_t& rule_index,
        bool& first_is_a) const noexcept {
        for (std::size_t index = 0U; index < rules.size(); ++index) {
            const auto& rule = rules[index];
            if (first.collider->group == rule.group_a && second.collider->group == rule.group_b) {
                rule_index = static_cast<std::uint32_t>(index);
                first_is_a = true;
                return &rule;
            }
            if (first.collider->group == rule.group_b && second.collider->group == rule.group_a) {
                rule_index = static_cast<std::uint32_t>(index);
                first_is_a = false;
                return &rule;
            }
        }
        return nullptr;
    }

    [[nodiscard]] bool deactivate(Proxy& proxy) noexcept {
        const bool was_active = active(proxy);
        proxy.collider->enabled = false;
        if (proxy.state != nullptr) {
            proxy.state->active = false;
        }
        if (proxy.sprite != nullptr) {
            proxy.sprite->visible = false;
        }
        return was_active;
    }

    [[nodiscard]] bool reflect(Proxy& proxy, const Proxy& other, const Vec2 normal) const noexcept {
        if (proxy.velocity == nullptr || proxy.collider->motion != BodyMotion2D::dynamic_body) return false;
        const auto other_velocity = velocity(other);
        const Vec2 relative{
            proxy.velocity->linear.x - other_velocity.x,
            proxy.velocity->linear.y - other_velocity.y,
        };
        const float projection = relative.x * normal.x + relative.y * normal.y;
        if (projection >= -collision_epsilon) return false;
        proxy.velocity->linear.x = other_velocity.x + relative.x - 2.0F * projection * normal.x;
        proxy.velocity->linear.y = other_velocity.y + relative.y - 2.0F * projection * normal.y;
        return true;
    }
};

CollisionGrid2D::CollisionGrid2D() : impl_(std::make_unique<Impl>()) {}
CollisionGrid2D::~CollisionGrid2D() = default;
CollisionGrid2D::CollisionGrid2D(CollisionGrid2D&&) noexcept = default;
CollisionGrid2D& CollisionGrid2D::operator=(CollisionGrid2D&&) noexcept = default;

Result<void> CollisionGrid2D::initialize(const CollisionGridConfig2D& config) {
    if (impl_->ready) {
        return std::unexpected(collision_error(DiagnosticCode::input_invalid, "Collision grid is already initialized"));
    }
    const double width = static_cast<double>(config.bounds.max.x) - config.bounds.min.x;
    const double height = static_cast<double>(config.bounds.max.y) - config.bounds.min.y;
    if (!(width > 0.0) || !(height > 0.0) || !std::isfinite(width) || !std::isfinite(height) ||
        !finite_positive(config.cell_size.x) || !finite_positive(config.cell_size.y) ||
        config.max_colliders == 0U || config.max_colliders > 10'000U ||
        config.max_grid_references == 0U || config.max_grid_references > 10'000'000U ||
        config.max_candidate_pairs == 0U || config.max_candidate_pairs > 10'000'000U ||
        config.max_impacts_per_dynamic == 0U || config.max_impacts_per_dynamic > 16U) {
        return std::unexpected(collision_error(DiagnosticCode::input_invalid, "Collision grid configuration is invalid"));
    }
    const double column_count = std::ceil(width / config.cell_size.x);
    const double row_count = std::ceil(height / config.cell_size.y);
    if (!std::isfinite(column_count) || !std::isfinite(row_count) || column_count < 1.0 || row_count < 1.0 ||
        column_count > 1'000'000.0 || row_count > 1'000'000.0 || column_count * row_count > 1'000'000.0) {
        return std::unexpected(collision_error(DiagnosticCode::collision_grid_capacity_exceeded, "Collision grid cell count is unsupported"));
    }
    const auto columns = static_cast<std::uint64_t>(column_count);
    const auto rows = static_cast<std::uint64_t>(row_count);
    impl_->config = config;
    impl_->columns = static_cast<std::uint32_t>(columns);
    impl_->rows = static_cast<std::uint32_t>(rows);
    impl_->proxies.reserve(config.max_colliders);
    impl_->cell_heads.resize(static_cast<std::size_t>(columns * rows), -1);
    impl_->references.reserve(config.max_grid_references);
    impl_->candidate_pairs.reserve(config.max_candidate_pairs);
    impl_->collision_events.reserve(
        static_cast<std::size_t>(config.max_colliders) * config.max_impacts_per_dynamic);
    impl_->ready = true;
    return {};
}

Result<CollisionMetrics2D> CollisionGrid2D::simulate(
    World& world,
    const std::span<const CollisionRule2D> rules,
    const float delta_seconds) {
    if (!impl_->ready || !std::isfinite(delta_seconds) || delta_seconds <= 0.0F) {
        return std::unexpected(collision_error(DiagnosticCode::input_invalid, "Collision simulation input is invalid"));
    }
    for (const auto& rule : rules) {
        if (rule.group_a == rule.group_b || rule.reaction_count > CollisionRule2D::max_reactions) {
            return std::unexpected(collision_error(
                DiagnosticCode::input_invalid,
                "Collision rule count or group pairing is invalid"));
        }
        for (std::uint32_t reaction_index = 0U; reaction_index < rule.reaction_count; ++reaction_index) {
            const auto& reaction = rule.reactions[reaction_index];
            if (static_cast<std::uint32_t>(reaction.kind) >
                    static_cast<std::uint32_t>(CollisionReactionKind2D::play_sound) ||
                static_cast<std::uint32_t>(reaction.target) >
                    static_cast<std::uint32_t>(CollisionTarget2D::b)) {
                return std::unexpected(collision_error(
                    DiagnosticCode::input_invalid,
                    "Collision rule contains an invalid reaction"));
            }
        }
    }
    CollisionMetrics2D metrics{};
    metrics.grid_reference_capacity = impl_->config.max_grid_references;
    metrics.candidate_capacity = impl_->config.max_candidate_pairs;
    impl_->proxies.clear();
    impl_->references.clear();
    impl_->candidate_pairs.clear();
    impl_->collision_events.clear();
    std::fill(impl_->cell_heads.begin(), impl_->cell_heads.end(), -1);

    {
        auto colliders = world.query<Collider2D>();
        for (auto item : colliders) {
            auto* transform = world.transform(item.entity);
            if (transform == nullptr) {
                continue;
            }
            if (impl_->proxies.size() >= impl_->proxies.capacity()) {
                return std::unexpected(collision_error(
                    DiagnosticCode::collision_grid_capacity_exceeded,
                    "Active colliders exceed the configured grid capacity"));
            }
            impl_->proxies.push_back({
                item.entity,
                transform,
                world.velocity(item.entity),
                world.sprite(item.entity),
                &item.component,
                world.entity_state(item.entity),
                {},
                {},
            });
        }
        world.record_query(colliders.candidate_count(), impl_->proxies.size());
    }
    std::sort(impl_->proxies.begin(), impl_->proxies.end(), [](const Impl::Proxy& left, const Impl::Proxy& right) {
        if (left.entity.index != right.entity.index) {
            return left.entity.index < right.entity.index;
        }
        return left.entity.generation < right.entity.generation;
    });

    for (auto& proxy : impl_->proxies) {
        if (!finite_vec2(proxy.transform->position) || !finite_vec2(proxy.transform->previous_position) ||
            !finite_vec2(proxy.collider->offset) || !finite_vec2(proxy.collider->half_extent) ||
            !finite_positive(proxy.collider->half_extent.x) || !finite_positive(proxy.collider->half_extent.y) ||
            (proxy.velocity != nullptr &&
             (!finite_vec2(proxy.velocity->linear) || !std::isfinite(proxy.velocity->angular)))) {
            return std::unexpected(collision_error(
                DiagnosticCode::runtime_numeric_state_invalid,
                "Collision state contains a non-finite numeric value"));
        }
        const double current_x = static_cast<double>(proxy.transform->position.x) + proxy.collider->offset.x;
        const double current_y = static_cast<double>(proxy.transform->position.y) + proxy.collider->offset.y;
        const double previous_x = static_cast<double>(proxy.transform->previous_position.x) + proxy.collider->offset.x;
        const double previous_y = static_cast<double>(proxy.transform->previous_position.y) + proxy.collider->offset.y;
        if (!representable_float(current_x) || !representable_float(current_y) ||
            !representable_float(previous_x) || !representable_float(previous_y)) {
            return std::unexpected(collision_error(
                DiagnosticCode::runtime_numeric_state_invalid,
                "Collision center exceeds the supported numeric range"));
        }
        proxy.start_center = {static_cast<float>(current_x), static_cast<float>(current_y)};
        if (proxy.collider->motion == BodyMotion2D::kinematic_body) {
            const double velocity_x = (current_x - previous_x) / delta_seconds;
            const double velocity_y = (current_y - previous_y) / delta_seconds;
            if (!representable_float(velocity_x) || !representable_float(velocity_y)) {
                return std::unexpected(collision_error(
                    DiagnosticCode::runtime_numeric_state_invalid,
                    "Kinematic collision velocity exceeds the supported numeric range"));
            }
            proxy.start_center = {static_cast<float>(previous_x), static_cast<float>(previous_y)};
            proxy.sweep_velocity = {static_cast<float>(velocity_x), static_cast<float>(velocity_y)};
        } else if (proxy.collider->motion == BodyMotion2D::dynamic_body && proxy.velocity != nullptr) {
            proxy.sweep_velocity = proxy.velocity->linear;
        }
        const auto velocity = impl_->velocity(proxy);
        if (!representable_float(current_x + static_cast<double>(velocity.x) * delta_seconds) ||
            !representable_float(current_y + static_cast<double>(velocity.y) * delta_seconds)) {
            return std::unexpected(collision_error(
                DiagnosticCode::runtime_numeric_state_invalid,
                "Collision motion exceeds the supported numeric range"));
        }
    }
    for (std::size_t proxy_index = 0U; proxy_index < impl_->proxies.size(); ++proxy_index) {
        const auto& proxy = impl_->proxies[proxy_index];
        if (!impl_->active(proxy)) {
            continue;
        }
        ++metrics.active_colliders;
        const auto center = proxy.start_center;
        const auto velocity = impl_->velocity(proxy);
        const double travel_x = static_cast<double>(velocity.x) * delta_seconds;
        const double travel_y = static_cast<double>(velocity.y) * delta_seconds;
        const bool may_reflect = proxy.collider->motion == BodyMotion2D::dynamic_body;
        const double minimum_x = (may_reflect ? center.x - std::abs(travel_x) :
                                               std::min<double>(center.x, center.x + travel_x)) -
                                 proxy.collider->half_extent.x;
        const double maximum_x = (may_reflect ? center.x + std::abs(travel_x) :
                                               std::max<double>(center.x, center.x + travel_x)) +
                                 proxy.collider->half_extent.x;
        const double minimum_y = (may_reflect ? center.y - std::abs(travel_y) :
                                               std::min<double>(center.y, center.y + travel_y)) -
                                 proxy.collider->half_extent.y;
        const double maximum_y = (may_reflect ? center.y + std::abs(travel_y) :
                                               std::max<double>(center.y, center.y + travel_y)) +
                                 proxy.collider->half_extent.y;
        const auto cell_x0 = clamped_cell_coordinate(
            minimum_x, impl_->config.bounds.min.x, impl_->config.cell_size.x, impl_->columns);
        const auto cell_x1 = clamped_cell_coordinate(
            maximum_x, impl_->config.bounds.min.x, impl_->config.cell_size.x, impl_->columns);
        const auto cell_y0 = clamped_cell_coordinate(
            minimum_y, impl_->config.bounds.min.y, impl_->config.cell_size.y, impl_->rows);
        const auto cell_y1 = clamped_cell_coordinate(
            maximum_y, impl_->config.bounds.min.y, impl_->config.cell_size.y, impl_->rows);
        for (std::uint32_t y = cell_y0; y <= cell_y1; ++y) {
            for (std::uint32_t x = cell_x0; x <= cell_x1; ++x) {
                if (impl_->references.size() >= impl_->references.capacity()) {
                    return std::unexpected(collision_error(
                        DiagnosticCode::collision_grid_capacity_exceeded,
                        "Collision grid references exceed the configured capacity"));
                }
                const auto cell = static_cast<std::size_t>(y) * impl_->columns + x;
                impl_->references.push_back({
                    static_cast<std::uint32_t>(proxy_index),
                    impl_->cell_heads[cell],
                });
                impl_->cell_heads[cell] = static_cast<std::int32_t>(impl_->references.size() - 1U);
            }
        }
    }
    metrics.grid_references = static_cast<std::uint32_t>(impl_->references.size());

    bool candidate_pairs_sorted = true;
    const auto canonicalize_candidate_pairs = [&]() noexcept {
        if (candidate_pairs_sorted) return;
        std::sort(impl_->candidate_pairs.begin(), impl_->candidate_pairs.end());
        impl_->candidate_pairs.erase(
            std::unique(impl_->candidate_pairs.begin(), impl_->candidate_pairs.end()),
            impl_->candidate_pairs.end());
        candidate_pairs_sorted = true;
    };
    const auto add_candidate_pair = [&](std::uint32_t first, std::uint32_t second) -> Result<void> {
        if (first == second) return {};
        if (first > second) std::swap(first, second);
        const auto& first_proxy = impl_->proxies[first];
        const auto& second_proxy = impl_->proxies[second];
        if (first_proxy.collider->motion != BodyMotion2D::dynamic_body &&
            second_proxy.collider->motion != BodyMotion2D::dynamic_body) {
            return {};
        }
        std::uint32_t ignored_index = 0U;
        bool ignored_orientation = false;
        if (impl_->find_rule(first_proxy, second_proxy, rules, ignored_index, ignored_orientation) == nullptr) {
            return {};
        }
        if (first_proxy.collider->motion == BodyMotion2D::dynamic_body &&
            second_proxy.collider->motion == BodyMotion2D::dynamic_body) {
            return std::unexpected(collision_error(
                DiagnosticCode::input_invalid,
                "Dynamic-versus-dynamic collision rules are unsupported"));
        }
        const auto packed = (static_cast<std::uint64_t>(first) << 32U) | static_cast<std::uint64_t>(second);
        if (candidate_pairs_sorted &&
            std::binary_search(impl_->candidate_pairs.begin(), impl_->candidate_pairs.end(), packed)) {
            return {};
        }
        if (impl_->candidate_pairs.size() >= impl_->config.max_candidate_pairs) {
            canonicalize_candidate_pairs();
            if (std::binary_search(impl_->candidate_pairs.begin(), impl_->candidate_pairs.end(), packed)) return {};
        }
        if (impl_->candidate_pairs.size() >= impl_->config.max_candidate_pairs) {
            return std::unexpected(collision_error(
                DiagnosticCode::collision_candidate_capacity_exceeded,
                "Collision candidate pairs exceed the configured capacity"));
        }
        impl_->candidate_pairs.push_back(packed);
        candidate_pairs_sorted = false;
        return {};
    };

    for (const auto head : impl_->cell_heads) {
        for (std::int32_t left = head; left >= 0; left = impl_->references[static_cast<std::size_t>(left)].next) {
            for (std::int32_t right = impl_->references[static_cast<std::size_t>(left)].next;
                 right >= 0;
                 right = impl_->references[static_cast<std::size_t>(right)].next) {
                auto first = impl_->references[static_cast<std::size_t>(left)].proxy;
                auto second = impl_->references[static_cast<std::size_t>(right)].proxy;
                if (first == second) {
                    continue;
                }
                if (auto added = add_candidate_pair(first, second); !added) return std::unexpected(std::move(added.error()));
            }
        }
    }
    canonicalize_candidate_pairs();
    metrics.candidate_pairs = static_cast<std::uint32_t>(impl_->candidate_pairs.size());

    for (std::size_t moving_index = 0U; moving_index < impl_->proxies.size(); ++moving_index) {
        auto& moving = impl_->proxies[moving_index];
        if (!impl_->active(moving) || moving.collider->motion != BodyMotion2D::dynamic_body ||
            moving.velocity == nullptr) {
            continue;
        }
        float remaining = delta_seconds;
        std::uint32_t impact = 0U;
        std::array<std::uint64_t, 16U> ignored_candidates{};
        std::size_t ignored_count = 0U;
        for (; impact < impl_->config.max_impacts_per_dynamic && remaining > 0.0F && impl_->active(moving); ++impact) {
            ++metrics.toi_iterations;
            // Reflections can send a dynamic body into cells that its original
            // forward sweep did not overlap. Query the grid again for every
            // remaining segment; static and kinematic proxies already cover their
            // complete tick paths in the cell-reference table.
            const auto moving_center = impl_->current_center(moving);
            const auto moving_velocity = impl_->velocity(moving);
            const double segment_end_x = static_cast<double>(moving_center.x) +
                                         static_cast<double>(moving_velocity.x) * remaining;
            const double segment_end_y = static_cast<double>(moving_center.y) +
                                         static_cast<double>(moving_velocity.y) * remaining;
            if (!finite_vec2(moving_center) || !finite_vec2(moving_velocity) ||
                !representable_float(segment_end_x) || !representable_float(segment_end_y)) {
                return std::unexpected(collision_error(
                    DiagnosticCode::runtime_numeric_state_invalid,
                    "Collision segment exceeds the supported numeric range"));
            }
            const auto segment_end = Vec2{static_cast<float>(segment_end_x), static_cast<float>(segment_end_y)};
            const double minimum_x = std::min(moving_center.x, segment_end.x) - moving.collider->half_extent.x;
            const double maximum_x = std::max(moving_center.x, segment_end.x) + moving.collider->half_extent.x;
            const double minimum_y = std::min(moving_center.y, segment_end.y) - moving.collider->half_extent.y;
            const double maximum_y = std::max(moving_center.y, segment_end.y) + moving.collider->half_extent.y;
            const auto cell_x0 = clamped_cell_coordinate(
                minimum_x, impl_->config.bounds.min.x, impl_->config.cell_size.x, impl_->columns);
            const auto cell_x1 = clamped_cell_coordinate(
                maximum_x, impl_->config.bounds.min.x, impl_->config.cell_size.x, impl_->columns);
            const auto cell_y0 = clamped_cell_coordinate(
                minimum_y, impl_->config.bounds.min.y, impl_->config.cell_size.y, impl_->rows);
            const auto cell_y1 = clamped_cell_coordinate(
                maximum_y, impl_->config.bounds.min.y, impl_->config.cell_size.y, impl_->rows);
            for (std::uint32_t y = cell_y0; y <= cell_y1; ++y) {
                for (std::uint32_t x = cell_x0; x <= cell_x1; ++x) {
                    const auto cell = static_cast<std::size_t>(y) * impl_->columns + x;
                    for (std::int32_t reference = impl_->cell_heads[cell]; reference >= 0;
                         reference = impl_->references[static_cast<std::size_t>(reference)].next) {
                        const auto other_index = impl_->references[static_cast<std::size_t>(reference)].proxy;
                        if (auto added = add_candidate_pair(
                                static_cast<std::uint32_t>(moving_index), other_index);
                            !added) {
                            return std::unexpected(std::move(added.error()));
                        }
                    }
                }
            }
            canonicalize_candidate_pairs();
            float earliest = remaining + 1.0F;
            Vec2 earliest_normal{};
            float earliest_penetration = 0.0F;
            std::uint32_t earliest_other = std::numeric_limits<std::uint32_t>::max();
            std::uint32_t earliest_rule = 0U;
            bool earliest_moving_is_a = true;
            std::uint64_t earliest_candidate = std::numeric_limits<std::uint64_t>::max();
            for (std::size_t candidate_index = 0U; candidate_index < impl_->candidate_pairs.size(); ++candidate_index) {
                const auto packed = impl_->candidate_pairs[candidate_index];
                if (std::find(
                        ignored_candidates.begin(), ignored_candidates.begin() + static_cast<std::ptrdiff_t>(ignored_count),
                        packed) != ignored_candidates.begin() + static_cast<std::ptrdiff_t>(ignored_count)) {
                    continue;
                }
                const auto first = static_cast<std::uint32_t>(packed >> 32U);
                const auto second = static_cast<std::uint32_t>(packed & 0xFFFFFFFFU);
                if (first != moving_index && second != moving_index) {
                    continue;
                }
                const auto other_index = first == moving_index ? second : first;
                auto& other = impl_->proxies[other_index];
                if (!impl_->active(other)) {
                    continue;
                }
                std::uint32_t rule_index = 0U;
                bool moving_is_a = true;
                const auto* rule = impl_->find_rule(moving, other, rules, rule_index, moving_is_a);
                if (rule == nullptr) {
                    continue;
                }
                (void)rule;
                ++metrics.narrowphase_tests;
                const float elapsed = delta_seconds - remaining;
                const auto hit = swept_aabb(
                    impl_->center_at(moving, elapsed),
                    moving.collider->half_extent,
                    impl_->velocity(moving),
                    impl_->center_at(other, elapsed),
                    other.collider->half_extent,
                    impl_->velocity(other),
                    remaining);
                if (hit.hit &&
                    (hit.time < earliest ||
                     (hit.time == earliest && other.entity.index < impl_->proxies[earliest_other].entity.index))) {
                    earliest = hit.time;
                    earliest_normal = hit.normal;
                    earliest_penetration = hit.penetration;
                    earliest_other = other_index;
                    earliest_rule = rule_index;
                    earliest_moving_is_a = moving_is_a;
                    earliest_candidate = packed;
                }
            }
            if (earliest_other == std::numeric_limits<std::uint32_t>::max()) {
                moving.transform->position.x += moving.velocity->linear.x * remaining;
                moving.transform->position.y += moving.velocity->linear.y * remaining;
                remaining = 0.0F;
                break;
            }

            moving.transform->position.x += moving.velocity->linear.x * earliest;
            moving.transform->position.y += moving.velocity->linear.y * earliest;
            remaining -= earliest;
            auto& other = impl_->proxies[earliest_other];
            const auto& rule = rules[earliest_rule];
            const Vec2 normal_for_rule_a = earliest_moving_is_a
                                               ? earliest_normal
                                               : Vec2{-earliest_normal.x, -earliest_normal.y};
            if (impl_->collision_events.size() >= impl_->collision_events.capacity()) {
                return std::unexpected(collision_error(
                    DiagnosticCode::collision_candidate_capacity_exceeded,
                    "Collision event capacity was exceeded"));
            }
            impl_->collision_events.push_back({
                earliest_rule,
                earliest_moving_is_a ? moving.entity : other.entity,
                earliest_moving_is_a ? other.entity : moving.entity,
                normal_for_rule_a,
                delta_seconds - remaining,
            });
            ++metrics.contacts;

            bool reflected_moving = false;
            for (std::size_t reaction_index = 0U; reaction_index < rule.reaction_count; ++reaction_index) {
                const auto& reaction = rule.reactions[reaction_index];
                Impl::Proxy& target = reaction.target == CollisionTarget2D::a
                                          ? (earliest_moving_is_a ? moving : other)
                                          : (earliest_moving_is_a ? other : moving);
                const Vec2 target_normal = reaction.target == CollisionTarget2D::a
                                               ? normal_for_rule_a
                                               : Vec2{-normal_for_rule_a.x, -normal_for_rule_a.y};
                if (reaction.kind == CollisionReactionKind2D::reflect) {
                    Impl::Proxy& counterpart = target.entity == moving.entity ? other : moving;
                    const bool reflected = impl_->reflect(target, counterpart, target_normal);
                    reflected_moving = reflected_moving || (reflected && target.entity == moving.entity);
                } else if (reaction.kind == CollisionReactionKind2D::deactivate) {
                    if (impl_->deactivate(target)) {
                        ++metrics.active_state_changes;
                    }
                }
            }
            if (!impl_->active(moving)) {
                remaining = 0.0F;
                break;
            }
            if (!reflected_moving) {
                if (ignored_count < ignored_candidates.size()) {
                    ignored_candidates[ignored_count++] = earliest_candidate;
                    continue;
                }
                ++metrics.iteration_limit_hits;
                break;
            }
            const float separation = std::max(collision_epsilon, earliest_penetration + collision_epsilon);
            moving.transform->position.x += earliest_normal.x * separation;
            moving.transform->position.y += earliest_normal.y * separation;
        }
        if (remaining > 0.0F && impact >= impl_->config.max_impacts_per_dynamic) {
            ++metrics.iteration_limit_hits;
        }
    }
    canonicalize_candidate_pairs();
    metrics.candidate_pairs = static_cast<std::uint32_t>(impl_->candidate_pairs.size());
    world.record_system_invocation();
    return metrics;
}

std::span<const CollisionEvent2D> CollisionGrid2D::events() const noexcept { return impl_->collision_events; }
bool CollisionGrid2D::initialized() const noexcept { return impl_->ready; }

} // namespace ai2d
