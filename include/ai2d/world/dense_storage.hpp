#pragma once

#include "ai2d/foundation/diagnostic.hpp"
#include "ai2d/world/entity.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace ai2d {

template <class Component>
class DenseStorage final {
public:
    static constexpr std::uint32_t missing = std::numeric_limits<std::uint32_t>::max();

    [[nodiscard]] Result<void> reserve(
        const std::size_t entity_capacity,
        const std::size_t component_capacity) {
        if (!components_.empty()) {
            return std::unexpected(Diagnostic::make(
                DiagnosticCode::input_invalid,
                Severity::error,
                "world",
                "Dense component capacity cannot change after population"));
        }
        const auto components_before = components_.capacity();
        const auto entities_before = entities_.capacity();
        const auto sparse_before = sparse_.capacity();
        components_.reserve(component_capacity);
        entities_.reserve(component_capacity);
        sparse_.reserve(entity_capacity);
        sparse_.resize(entity_capacity, missing);
        capacity_limit_ = component_capacity;
        if (components_.capacity() != components_before || entities_.capacity() != entities_before ||
            sparse_.capacity() != sparse_before) {
            ++setup_capacity_changes_;
        }
        return {};
    }

    [[nodiscard]] Result<void> insert(const EntityId entity, const Component& component) {
        if (!entity.valid() || entity.index >= sparse_.size()) {
            return std::unexpected(Diagnostic::make(
                DiagnosticCode::world_stale_entity,
                Severity::error,
                "world",
                "Component insertion received an out-of-range entity"));
        }
        if (contains(entity)) {
            return std::unexpected(Diagnostic::make(
                DiagnosticCode::input_invalid,
                Severity::error,
                "world",
                "The entity already owns this component"));
        }
        if (components_.size() >= capacity_limit_ || components_.size() >= components_.capacity() ||
            entities_.size() >= entities_.capacity()) {
            return std::unexpected(Diagnostic::make(
                DiagnosticCode::world_capacity_exceeded,
                Severity::error,
                "world",
                "Dense component capacity was exhausted"));
        }

        const auto component_capacity = components_.capacity();
        const auto entity_capacity = entities_.capacity();
        const auto dense_index = static_cast<std::uint32_t>(components_.size());
        components_.push_back(component);
        entities_.push_back(entity);
        sparse_[entity.index] = dense_index;
        if (components_.capacity() != component_capacity || entities_.capacity() != entity_capacity) {
            ++runtime_capacity_growth_events_;
        }
        return {};
    }

    [[nodiscard]] Result<void> remove(const EntityId entity) {
        if (!contains(entity)) {
            return std::unexpected(Diagnostic::make(
                DiagnosticCode::world_stale_entity,
                Severity::error,
                "world",
                "Component removal received a stale or missing entity"));
        }

        const auto dense_index = static_cast<std::size_t>(sparse_[entity.index]);
        const auto last_index = components_.size() - 1U;
        if (dense_index != last_index) {
            components_[dense_index] = std::move(components_[last_index]);
            entities_[dense_index] = entities_[last_index];
            sparse_[entities_[dense_index].index] = static_cast<std::uint32_t>(dense_index);
        }
        components_.pop_back();
        entities_.pop_back();
        sparse_[entity.index] = missing;
        return {};
    }

    [[nodiscard]] bool contains(const EntityId entity) const noexcept {
        if (!entity.valid() || entity.index >= sparse_.size()) {
            return false;
        }
        const auto dense_index = sparse_[entity.index];
        return dense_index != missing && dense_index < entities_.size() && entities_[dense_index] == entity;
    }

    [[nodiscard]] Component* get(const EntityId entity) noexcept {
        return contains(entity) ? &components_[sparse_[entity.index]] : nullptr;
    }

    [[nodiscard]] const Component* get(const EntityId entity) const noexcept {
        return contains(entity) ? &components_[sparse_[entity.index]] : nullptr;
    }

    [[nodiscard]] Component& component_at(const std::size_t dense_index) noexcept {
        return components_[dense_index];
    }

    [[nodiscard]] const Component& component_at(const std::size_t dense_index) const noexcept {
        return components_[dense_index];
    }

    [[nodiscard]] EntityId entity_at(const std::size_t dense_index) const noexcept {
        return entities_[dense_index];
    }

    [[nodiscard]] std::size_t size() const noexcept { return components_.size(); }
    [[nodiscard]] std::size_t capacity() const noexcept { return components_.capacity(); }
    [[nodiscard]] std::size_t sparse_capacity() const noexcept { return sparse_.size(); }
    [[nodiscard]] std::size_t setup_capacity_changes() const noexcept { return setup_capacity_changes_; }
    [[nodiscard]] std::size_t runtime_capacity_growth_events() const noexcept {
        return runtime_capacity_growth_events_;
    }

private:
    std::vector<Component> components_{};
    std::vector<EntityId> entities_{};
    std::vector<std::uint32_t> sparse_{};
    std::size_t capacity_limit_{0U};
    std::size_t setup_capacity_changes_{0U};
    std::size_t runtime_capacity_growth_events_{0U};
};

} // namespace ai2d
