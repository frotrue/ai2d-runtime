#include "ai2d/world/world.hpp"

namespace ai2d {

Result<void> World::reserve(const std::size_t entity_capacity) {
    if (auto guard = check_structural_mutation(); !guard) {
        return guard;
    }
    if (entities_.size_slots() != 0U) {
        return std::unexpected(Diagnostic::make(
            DiagnosticCode::input_invalid,
            Severity::error,
            "world",
            "World capacity cannot change after any entity slot has been created"));
    }
    entities_.reserve(entity_capacity);
    if (auto reserved = transforms_.reserve(entity_capacity, entity_capacity); !reserved) {
        return reserved;
    }
    if (auto reserved = velocities_.reserve(entity_capacity, entity_capacity); !reserved) {
        return reserved;
    }
    if (auto reserved = sprites_.reserve(entity_capacity, entity_capacity); !reserved) {
        return reserved;
    }
    if (auto reserved = colliders_.reserve(entity_capacity, entity_capacity); !reserved) {
        return reserved;
    }
    if (auto reserved = entity_states_.reserve(entity_capacity, entity_capacity); !reserved) {
        return reserved;
    }
    return {};
}

Result<void> World::check_structural_mutation() {
    if (active_queries_ == 0U) {
        return {};
    }
    ++structural_mutation_errors_;
    return std::unexpected(Diagnostic::make(
        DiagnosticCode::world_structural_mutation_during_query,
        Severity::error,
        "world",
        "World structure cannot change while a borrowed query is alive"));
}

Result<EntityId> World::create_entity() {
    if (auto guard = check_structural_mutation(); !guard) {
        return std::unexpected(std::move(guard.error()));
    }
    return entities_.create();
}

Result<EntityId> World::create_entity(
    const Transform2D& transform_component,
    const std::optional<Velocity2D>& velocity_component,
    const std::optional<Sprite2D>& sprite_component) {
    auto entity_result = create_entity();
    if (!entity_result) {
        return entity_result;
    }
    const auto entity = *entity_result;
    if (auto result = transforms_.insert(entity, transform_component); !result) {
        (void)entities_.destroy(entity);
        return std::unexpected(std::move(result.error()));
    }
    if (velocity_component) {
        if (auto result = velocities_.insert(entity, *velocity_component); !result) {
            (void)transforms_.remove(entity);
            (void)entities_.destroy(entity);
            return std::unexpected(std::move(result.error()));
        }
    }
    if (sprite_component) {
        if (auto result = sprites_.insert(entity, *sprite_component); !result) {
            if (velocity_component) {
                (void)velocities_.remove(entity);
            }
            (void)transforms_.remove(entity);
            (void)entities_.destroy(entity);
            return std::unexpected(std::move(result.error()));
        }
    }
    return entity;
}

Result<void> World::destroy_entity(const EntityId entity) {
    if (auto guard = check_structural_mutation(); !guard) {
        return guard;
    }
    if (!entities_.is_alive(entity)) {
        ++stale_handle_errors_;
        return std::unexpected(Diagnostic::make(
            DiagnosticCode::world_stale_entity,
            Severity::error,
            "world",
            "World destroy received a stale entity"));
    }
    if (transforms_.contains(entity)) {
        (void)transforms_.remove(entity);
    }
    if (velocities_.contains(entity)) {
        (void)velocities_.remove(entity);
    }
    if (sprites_.contains(entity)) {
        (void)sprites_.remove(entity);
    }
    if (colliders_.contains(entity)) {
        (void)colliders_.remove(entity);
    }
    if (entity_states_.contains(entity)) {
        (void)entity_states_.remove(entity);
    }
    return entities_.destroy(entity);
}

Result<void> World::add(const EntityId entity, const Transform2D& component) {
    if (auto guard = check_structural_mutation(); !guard) {
        return guard;
    }
    if (!entities_.is_alive(entity)) {
        ++stale_handle_errors_;
        return std::unexpected(Diagnostic::make(
            DiagnosticCode::world_stale_entity, Severity::error, "world", "Transform add received a stale entity"));
    }
    return transforms_.insert(entity, component);
}

Result<void> World::add(const EntityId entity, const Velocity2D& component) {
    if (auto guard = check_structural_mutation(); !guard) {
        return guard;
    }
    if (!entities_.is_alive(entity)) {
        ++stale_handle_errors_;
        return std::unexpected(Diagnostic::make(
            DiagnosticCode::world_stale_entity, Severity::error, "world", "Velocity add received a stale entity"));
    }
    return velocities_.insert(entity, component);
}

Result<void> World::add(const EntityId entity, const Sprite2D& component) {
    if (auto guard = check_structural_mutation(); !guard) {
        return guard;
    }
    if (!entities_.is_alive(entity)) {
        ++stale_handle_errors_;
        return std::unexpected(Diagnostic::make(
            DiagnosticCode::world_stale_entity, Severity::error, "world", "Sprite add received a stale entity"));
    }
    return sprites_.insert(entity, component);
}

Result<void> World::add(const EntityId entity, const Collider2D& component) {
    if (auto guard = check_structural_mutation(); !guard) {
        return guard;
    }
    if (!entities_.is_alive(entity)) {
        ++stale_handle_errors_;
        return std::unexpected(Diagnostic::make(
            DiagnosticCode::world_stale_entity, Severity::error, "world", "Collider add received a stale entity"));
    }
    return colliders_.insert(entity, component);
}

Result<void> World::add(const EntityId entity, const EntityState2D& component) {
    if (auto guard = check_structural_mutation(); !guard) {
        return guard;
    }
    if (!entities_.is_alive(entity)) {
        ++stale_handle_errors_;
        return std::unexpected(Diagnostic::make(
            DiagnosticCode::world_stale_entity, Severity::error, "world", "Entity state add received a stale entity"));
    }
    return entity_states_.insert(entity, component);
}

Result<void> World::remove_transform(const EntityId entity) {
    if (auto guard = check_structural_mutation(); !guard) {
        return guard;
    }
    return transforms_.remove(entity);
}

Result<void> World::remove_velocity(const EntityId entity) {
    if (auto guard = check_structural_mutation(); !guard) {
        return guard;
    }
    return velocities_.remove(entity);
}

Result<void> World::remove_sprite(const EntityId entity) {
    if (auto guard = check_structural_mutation(); !guard) {
        return guard;
    }
    return sprites_.remove(entity);
}

Result<void> World::remove_collider(const EntityId entity) {
    if (auto guard = check_structural_mutation(); !guard) {
        return guard;
    }
    return colliders_.remove(entity);
}

Result<void> World::remove_entity_state(const EntityId entity) {
    if (auto guard = check_structural_mutation(); !guard) {
        return guard;
    }
    return entity_states_.remove(entity);
}

WorldMetrics World::metrics() const noexcept {
    return {
        entities_.alive_count(),
        transforms_.size(),
        velocities_.size(),
        sprites_.size(),
        colliders_.size(),
        entity_states_.size(),
        query_visited_,
        query_matched_,
        system_invocations_,
        stale_handle_errors_,
        structural_mutation_errors_,
        static_cast<std::uint64_t>(
            entities_.capacity_growth_events() + transforms_.runtime_capacity_growth_events() +
            velocities_.runtime_capacity_growth_events() + sprites_.runtime_capacity_growth_events() +
            colliders_.runtime_capacity_growth_events() + entity_states_.runtime_capacity_growth_events()),
    };
}

WorldCapacitySnapshot World::capacity_snapshot() const noexcept {
    return {
        entities_.max_capacity(),
        transforms_.capacity(),
        velocities_.capacity(),
        sprites_.capacity(),
        colliders_.capacity(),
        entity_states_.capacity(),
    };
}

void World::record_query(const std::size_t visited, const std::size_t matched) noexcept {
    query_visited_ += static_cast<std::uint64_t>(visited);
    query_matched_ += static_cast<std::uint64_t>(matched);
}

} // namespace ai2d
