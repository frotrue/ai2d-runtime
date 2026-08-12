#pragma once

#include "ai2d/foundation/diagnostic.hpp"
#include "ai2d/world/components.hpp"
#include "ai2d/world/dense_storage.hpp"
#include "ai2d/world/entity.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>
#include <utility>

namespace ai2d {

template <class... Components>
class QueryView;

struct WorldMetrics final {
    std::size_t alive_entities{0U};
    std::size_t transform_count{0U};
    std::size_t velocity_count{0U};
    std::size_t sprite_count{0U};
    std::size_t collider_count{0U};
    std::size_t entity_state_count{0U};
    std::uint64_t query_visited{0U};
    std::uint64_t query_matched{0U};
    std::uint64_t system_invocations{0U};
    std::uint64_t stale_handle_errors{0U};
    std::uint64_t structural_mutation_errors{0U};
    std::uint64_t capacity_growth_events{0U};
};

struct WorldCapacitySnapshot final {
    std::size_t entity_slots{0U};
    std::size_t transform_capacity{0U};
    std::size_t velocity_capacity{0U};
    std::size_t sprite_capacity{0U};
    std::size_t collider_capacity{0U};
    std::size_t entity_state_capacity{0U};
};

class World final {
public:
    World() = default;
    World(const World&) = delete;
    World& operator=(const World&) = delete;
    World(World&&) = delete;
    World& operator=(World&&) = delete;

    [[nodiscard]] Result<void> reserve(std::size_t entity_capacity);

    [[nodiscard]] Result<EntityId> create_entity();
    [[nodiscard]] Result<EntityId> create_entity(
        const Transform2D& transform,
        const std::optional<Velocity2D>& velocity,
        const std::optional<Sprite2D>& sprite);
    [[nodiscard]] Result<void> destroy_entity(EntityId entity);

    [[nodiscard]] Result<void> add(EntityId entity, const Transform2D& component);
    [[nodiscard]] Result<void> add(EntityId entity, const Velocity2D& component);
    [[nodiscard]] Result<void> add(EntityId entity, const Sprite2D& component);
    [[nodiscard]] Result<void> add(EntityId entity, const Collider2D& component);
    [[nodiscard]] Result<void> add(EntityId entity, const EntityState2D& component);
    [[nodiscard]] Result<void> remove_transform(EntityId entity);
    [[nodiscard]] Result<void> remove_velocity(EntityId entity);
    [[nodiscard]] Result<void> remove_sprite(EntityId entity);
    [[nodiscard]] Result<void> remove_collider(EntityId entity);
    [[nodiscard]] Result<void> remove_entity_state(EntityId entity);

    [[nodiscard]] bool is_alive(EntityId entity) const noexcept { return entities_.is_alive(entity); }
    [[nodiscard]] Transform2D* transform(EntityId entity) noexcept { return transforms_.get(entity); }
    [[nodiscard]] Velocity2D* velocity(EntityId entity) noexcept { return velocities_.get(entity); }
    [[nodiscard]] Sprite2D* sprite(EntityId entity) noexcept { return sprites_.get(entity); }
    [[nodiscard]] Collider2D* collider(EntityId entity) noexcept { return colliders_.get(entity); }
    [[nodiscard]] EntityState2D* entity_state(EntityId entity) noexcept { return entity_states_.get(entity); }
    [[nodiscard]] const Transform2D* transform(EntityId entity) const noexcept { return transforms_.get(entity); }
    [[nodiscard]] const Velocity2D* velocity(EntityId entity) const noexcept { return velocities_.get(entity); }
    [[nodiscard]] const Sprite2D* sprite(EntityId entity) const noexcept { return sprites_.get(entity); }
    [[nodiscard]] const Collider2D* collider(EntityId entity) const noexcept { return colliders_.get(entity); }
    [[nodiscard]] const EntityState2D* entity_state(EntityId entity) const noexcept { return entity_states_.get(entity); }

    template <class... Components>
    [[nodiscard]] QueryView<Components...> query();

    [[nodiscard]] WorldMetrics metrics() const noexcept;
    [[nodiscard]] WorldCapacitySnapshot capacity_snapshot() const noexcept;
    void record_query(std::size_t visited, std::size_t matched) noexcept;
    void record_system_invocation() noexcept { ++system_invocations_; }

private:
    template <class...>
    friend class QueryView;

    [[nodiscard]] Result<void> check_structural_mutation();
    void begin_query() noexcept { ++active_queries_; }
    void end_query() noexcept { --active_queries_; }

    template <class Component>
    [[nodiscard]] DenseStorage<Component>& storage() noexcept {
        if constexpr (std::is_same_v<Component, Transform2D>) {
            return transforms_;
        } else if constexpr (std::is_same_v<Component, Velocity2D>) {
            return velocities_;
        } else if constexpr (std::is_same_v<Component, Sprite2D>) {
            return sprites_;
        } else if constexpr (std::is_same_v<Component, Collider2D>) {
            return colliders_;
        } else {
            static_assert(std::is_same_v<Component, EntityState2D>);
            return entity_states_;
        }
    }

    EntityRegistry entities_{};
    DenseStorage<Transform2D> transforms_{};
    DenseStorage<Velocity2D> velocities_{};
    DenseStorage<Sprite2D> sprites_{};
    DenseStorage<Collider2D> colliders_{};
    DenseStorage<EntityState2D> entity_states_{};
    std::size_t active_queries_{0U};
    std::uint64_t query_visited_{0U};
    std::uint64_t query_matched_{0U};
    std::uint64_t system_invocations_{0U};
    std::uint64_t stale_handle_errors_{0U};
    std::uint64_t structural_mutation_errors_{0U};
};

template <class Component>
struct QueryItem final {
    EntityId entity{};
    Component& component;
};

template <class First, class Second>
struct QueryItem2 final {
    EntityId entity{};
    First& first;
    Second& second;
};

template <class Component>
class QueryView<Component> final {
public:
    class Iterator final {
    public:
        Iterator(QueryView* owner, const std::size_t index) noexcept : owner_(owner), index_(index) {}
        Iterator& operator++() noexcept {
            ++index_;
            return *this;
        }
        [[nodiscard]] bool operator!=(const Iterator& other) const noexcept { return index_ != other.index_; }
        [[nodiscard]] QueryItem<Component> operator*() const noexcept {
            return {owner_->storage_->entity_at(index_), owner_->storage_->component_at(index_)};
        }

    private:
        QueryView* owner_{nullptr};
        std::size_t index_{0U};
    };

    QueryView(const QueryView&) = delete;
    QueryView& operator=(const QueryView&) = delete;
    QueryView(QueryView&& other) noexcept
        : world_(std::exchange(other.world_, nullptr)), storage_(other.storage_) {}
    QueryView& operator=(QueryView&&) = delete;
    ~QueryView() {
        if (world_ != nullptr) {
            world_->end_query();
        }
    }

    [[nodiscard]] Iterator begin() noexcept { return {this, 0U}; }
    [[nodiscard]] Iterator end() noexcept { return {this, storage_->size()}; }
    [[nodiscard]] std::size_t candidate_count() const noexcept { return storage_->size(); }

private:
    friend class World;
    QueryView(World& world, DenseStorage<Component>& storage) noexcept : world_(&world), storage_(&storage) {}
    World* world_{nullptr};
    DenseStorage<Component>* storage_{nullptr};
};

template <class First, class Second>
class QueryView<First, Second> final {
public:
    class Iterator final {
    public:
        Iterator(QueryView* owner, const std::size_t index) noexcept : owner_(owner), index_(index) { seek_match(); }
        Iterator& operator++() noexcept {
            ++index_;
            seek_match();
            return *this;
        }
        [[nodiscard]] bool operator!=(const Iterator& other) const noexcept { return index_ != other.index_; }
        [[nodiscard]] QueryItem2<First, Second> operator*() const noexcept {
            if (owner_->first_is_primary_) {
                const auto entity = owner_->first_->entity_at(index_);
                return {entity, owner_->first_->component_at(index_), *owner_->second_->get(entity)};
            }
            const auto entity = owner_->second_->entity_at(index_);
            return {entity, *owner_->first_->get(entity), owner_->second_->component_at(index_)};
        }

    private:
        void seek_match() noexcept {
            const auto end_index = owner_->candidate_count();
            while (index_ < end_index) {
                const auto entity = owner_->first_is_primary_ ? owner_->first_->entity_at(index_)
                                                               : owner_->second_->entity_at(index_);
                const auto present = owner_->first_is_primary_ ? owner_->second_->contains(entity)
                                                                : owner_->first_->contains(entity);
                if (present) {
                    break;
                }
                ++index_;
            }
        }

        QueryView* owner_{nullptr};
        std::size_t index_{0U};
    };

    QueryView(const QueryView&) = delete;
    QueryView& operator=(const QueryView&) = delete;
    QueryView(QueryView&& other) noexcept
        : world_(std::exchange(other.world_, nullptr)),
          first_(other.first_),
          second_(other.second_),
          first_is_primary_(other.first_is_primary_) {}
    QueryView& operator=(QueryView&&) = delete;
    ~QueryView() {
        if (world_ != nullptr) {
            world_->end_query();
        }
    }

    [[nodiscard]] Iterator begin() noexcept { return {this, 0U}; }
    [[nodiscard]] Iterator end() noexcept { return {this, candidate_count()}; }
    [[nodiscard]] std::size_t candidate_count() const noexcept {
        return first_is_primary_ ? first_->size() : second_->size();
    }

private:
    friend class World;
    QueryView(World& world, DenseStorage<First>& first, DenseStorage<Second>& second) noexcept
        : world_(&world), first_(&first), second_(&second), first_is_primary_(first.size() <= second.size()) {}

    World* world_{nullptr};
    DenseStorage<First>* first_{nullptr};
    DenseStorage<Second>* second_{nullptr};
    bool first_is_primary_{true};
};

template <class... Components>
QueryView<Components...> World::query() {
    static_assert(sizeof...(Components) == 1U || sizeof...(Components) == 2U);
    begin_query();
    return QueryView<Components...>{*this, storage<Components>()...};
}

} // namespace ai2d
