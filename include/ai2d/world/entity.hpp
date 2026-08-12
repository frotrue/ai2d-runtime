#pragma once

#include "ai2d/foundation/diagnostic.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace ai2d {

struct EntityId final {
    static constexpr std::uint32_t invalid_index = std::numeric_limits<std::uint32_t>::max();

    std::uint32_t index{invalid_index};
    std::uint32_t generation{0U};

    [[nodiscard]] constexpr bool valid() const noexcept { return index != invalid_index; }
    [[nodiscard]] static constexpr EntityId invalid() noexcept { return {}; }
    friend constexpr bool operator==(const EntityId&, const EntityId&) = default;
};

class EntityRegistry final {
public:
    EntityRegistry() = default;

    void reserve(std::size_t capacity);
    [[nodiscard]] Result<EntityId> create();
    [[nodiscard]] Result<void> destroy(EntityId entity);
    [[nodiscard]] bool is_alive(EntityId entity) const noexcept;

    [[nodiscard]] std::size_t alive_count() const noexcept { return alive_count_; }
    [[nodiscard]] std::size_t size_slots() const noexcept { return generations_.size(); }
    [[nodiscard]] std::size_t max_capacity() const noexcept { return max_capacity_; }
    [[nodiscard]] std::size_t capacity_growth_events() const noexcept { return capacity_growth_events_; }

private:
    std::vector<std::uint32_t> generations_{};
    std::vector<std::uint8_t> alive_{};
    std::vector<std::uint32_t> free_indices_{};
    std::size_t max_capacity_{0U};
    std::size_t alive_count_{0U};
    std::size_t capacity_growth_events_{0U};
};

} // namespace ai2d
