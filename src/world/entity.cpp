#include "ai2d/world/entity.hpp"

#include <limits>

namespace ai2d {

void EntityRegistry::reserve(const std::size_t capacity) {
    const auto generations_before = generations_.capacity();
    const auto alive_before = alive_.capacity();
    const auto free_before = free_indices_.capacity();
    generations_.reserve(capacity);
    alive_.reserve(capacity);
    free_indices_.reserve(capacity);
    max_capacity_ = capacity;
    (void)generations_before;
    (void)alive_before;
    (void)free_before;
}

Result<EntityId> EntityRegistry::create() {
    if (!free_indices_.empty()) {
        const auto index = free_indices_.back();
        free_indices_.pop_back();
        alive_[index] = 1U;
        ++alive_count_;
        return EntityId{index, generations_[index]};
    }
    if (generations_.size() >= max_capacity_ || generations_.size() >= generations_.capacity() ||
        alive_.size() >= alive_.capacity()) {
        return std::unexpected(Diagnostic::make(
            DiagnosticCode::world_capacity_exceeded,
            Severity::error,
            "world",
            "Entity capacity was exhausted"));
    }

    const auto generation_capacity = generations_.capacity();
    const auto alive_capacity = alive_.capacity();
    const auto index = static_cast<std::uint32_t>(generations_.size());
    generations_.push_back(1U);
    alive_.push_back(1U);
    ++alive_count_;
    if (generations_.capacity() != generation_capacity || alive_.capacity() != alive_capacity) {
        ++capacity_growth_events_;
    }
    return EntityId{index, 1U};
}

Result<void> EntityRegistry::destroy(const EntityId entity) {
    if (!is_alive(entity)) {
        return std::unexpected(Diagnostic::make(
            DiagnosticCode::world_stale_entity,
            Severity::error,
            "world",
            "Entity destroy received a stale handle"));
    }
    alive_[entity.index] = 0U;
    auto& generation = generations_[entity.index];
    generation = generation == std::numeric_limits<std::uint32_t>::max() ? 1U : generation + 1U;
    free_indices_.push_back(entity.index);
    --alive_count_;
    return {};
}

bool EntityRegistry::is_alive(const EntityId entity) const noexcept {
    return entity.valid() && entity.index < generations_.size() && alive_[entity.index] != 0U &&
           generations_[entity.index] == entity.generation;
}

} // namespace ai2d
