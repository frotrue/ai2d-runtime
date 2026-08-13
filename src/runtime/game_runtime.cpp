#include "ai2d/runtime/game_runtime.hpp"

#include "ai2d/foundation/timer.hpp"
#include "ai2d/runtime/fixed_step.hpp"
#include "ai2d/world/components.hpp"
#include "ai2d/world/world.hpp"

#if defined(AI2D_ENABLE_GPU)
#include "ai2d/platform/assets.hpp"
#include "ai2d/platform/audio.hpp"
#include "ai2d/platform/settings.hpp"
#include "ai2d/renderer2d/renderer.hpp"
#endif

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ai2d {
namespace {

Diagnostic runtime_error(const DiagnosticCode code, const char* const message) {
    return Diagnostic::make(code, Severity::error, "game_runtime", message);
}

struct TransparentStringHash final {
    using is_transparent = void;
    [[nodiscard]] std::size_t operator()(const std::string_view value) const noexcept {
        return std::hash<std::string_view>{}(value);
    }
};

std::size_t decimal_width(const std::int32_t value) noexcept {
    char buffer[16]{};
    const auto result = std::to_chars(std::begin(buffer), std::end(buffer), value);
    return result.ec == std::errc{} ? static_cast<std::size_t>(result.ptr - buffer) : 0U;
}

BodyMotion2D translate_motion(const GameBodyMotion motion) noexcept {
    switch (motion) {
    case GameBodyMotion::static_body: return BodyMotion2D::static_body;
    case GameBodyMotion::kinematic_body: return BodyMotion2D::kinematic_body;
    case GameBodyMotion::dynamic_body: return BodyMotion2D::dynamic_body;
    }
    return BodyMotion2D::static_body;
}

CollisionReactionKind2D translate_reaction(const GameReactionKind kind) noexcept {
    switch (kind) {
    case GameReactionKind::reflect: return CollisionReactionKind2D::reflect;
    case GameReactionKind::deactivate: return CollisionReactionKind2D::deactivate;
    case GameReactionKind::add_int_state: return CollisionReactionKind2D::add_int_state;
    case GameReactionKind::reset_group: return CollisionReactionKind2D::reset_group;
    case GameReactionKind::play_sound: return CollisionReactionKind2D::play_sound;
    }
    return CollisionReactionKind2D::reflect;
}

InputKey translate_key(const GameKey key) noexcept {
    return static_cast<InputKey>(static_cast<std::uint8_t>(key));
}

std::uint32_t decode_utf8(const std::string_view text, std::size_t& offset) noexcept {
    const auto first = static_cast<std::uint8_t>(text[offset++]);
    if (first < 0x80U) return first;
    std::size_t continuation_count = 0U;
    std::uint32_t codepoint = 0U;
    if ((first & 0xE0U) == 0xC0U) {
        continuation_count = 1U;
        codepoint = first & 0x1FU;
    } else if ((first & 0xF0U) == 0xE0U) {
        continuation_count = 2U;
        codepoint = first & 0x0FU;
    } else {
        continuation_count = 3U;
        codepoint = first & 0x07U;
    }
    for (std::size_t index = 0U; index < continuation_count && offset < text.size(); ++index) {
        codepoint = (codepoint << 6U) | (static_cast<std::uint8_t>(text[offset++]) & 0x3FU);
    }
    return codepoint;
}

bool directions_are_opposite(const GameDirection first, const GameDirection second) noexcept {
    return (first == GameDirection::up && second == GameDirection::down) ||
           (first == GameDirection::down && second == GameDirection::up) ||
           (first == GameDirection::left && second == GameDirection::right) ||
           (first == GameDirection::right && second == GameDirection::left);
}

Vec2 direction_vector(const GameDirection direction) noexcept {
    switch (direction) {
    case GameDirection::up: return {0.0F, 1.0F};
    case GameDirection::down: return {0.0F, -1.0F};
    case GameDirection::left: return {-1.0F, 0.0F};
    case GameDirection::right: return {1.0F, 0.0F};
    case GameDirection::none: return {};
    }
    return {};
}

bool compare_integer(const std::int64_t left, const GameComparison comparison, const std::int64_t right) noexcept {
    switch (comparison) {
    case GameComparison::equal: return left == right;
    case GameComparison::not_equal: return left != right;
    case GameComparison::less: return left < right;
    case GameComparison::less_equal: return left <= right;
    case GameComparison::greater: return left > right;
    case GameComparison::greater_equal: return left >= right;
    }
    return false;
}

bool representable_float(const double value) noexcept {
    return std::isfinite(value) &&
           value >= -static_cast<double>(std::numeric_limits<float>::max()) &&
           value <= static_cast<double>(std::numeric_limits<float>::max());
}

std::uint64_t mix_u64(std::uint64_t value) noexcept {
    value += 0x9E3779B97F4A7C15ULL;
    value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31U);
}

constexpr std::uint64_t maximum_resident_asset_bytes = 512ULL * 1024ULL * 1024ULL;

std::uint64_t stable_text_hash(const std::string_view value) noexcept {
    std::uint64_t hash = 14'695'981'039'346'656'037ULL;
    for (const unsigned char byte : value) {
        hash ^= byte;
        hash *= 1'099'511'628'211ULL;
    }
    return hash;
}

} // namespace

class GameRuntime::Impl final {
public:
    struct ActionState final {
        bool down{false};
        bool pressed{false};
        bool released{false};
    };

    struct EntityRecord final {
        EntityId entity{};
        std::uint32_t spawn_group_index{0U};
        SymbolId collider_group{0U};
        Transform2D initial_transform{};
        Velocity2D initial_velocity{};
        Sprite2D initial_sprite{};
        Collider2D initial_collider{};
        bool has_velocity{false};
        bool has_sprite{false};
        bool has_collider{false};
        bool initial_active{true};
        bool pooled{false};
        std::uint32_t pool_index{0U};
        std::uint32_t pool_slot_index{0U};
    };

    struct GroupRange final {
        std::uint32_t begin{0U};
        std::uint32_t count{0U};
    };

    struct SystemState final {
        GameDirection current_direction{GameDirection::none};
        GameDirection queued_direction{GameDirection::none};
        std::uint32_t phase{0U};
        bool stepped{false};
    };

    struct PoolRuntimeState final {
        static constexpr std::uint32_t invalid_slot = std::numeric_limits<std::uint32_t>::max();
        static constexpr std::uint64_t no_expiration = std::numeric_limits<std::uint64_t>::max();

        std::vector<std::uint32_t> free_ring{};
        std::vector<std::uint32_t> active_next{};
        std::vector<std::uint32_t> active_previous{};
        std::vector<std::uint64_t> expiration_ticks{};
        std::vector<std::uint8_t> acquired{};
        std::uint32_t free_head{0U};
        std::uint32_t free_count{0U};
        std::uint32_t active_head{invalid_slot};
        std::uint32_t active_tail{invalid_slot};
        std::uint32_t active_count{0U};
    };

    struct SceneSnapshot final {
        struct Contact final {
            std::uint32_t rule_index{0U};
            std::uint32_t entity_a_index{0U};
            std::uint32_t entity_b_index{0U};
        };

        std::vector<Transform2D> transforms{};
        std::vector<Velocity2D> velocities{};
        std::vector<Sprite2D> sprites{};
        std::vector<Collider2D> colliders{};
        std::vector<EntityState2D> entity_states{};
        std::vector<SystemState> system_states{};
        std::vector<std::uint64_t> rule_invocations{};
        std::vector<PoolRuntimeState> pool_states{};
        std::vector<Contact> contacts{};
        std::uint64_t scene_tick{0U};
        std::uint32_t focused_button{0U};
        bool retained{false};
    };

    struct TextToken final {
        std::string_view literal{};
        std::uint32_t state_index{0U};
        bool is_state{false};
    };

    GamePlan plan{};
    GameRuntimeOptions options{};
    FixedStepClock clock{};
    std::unique_ptr<World> world{};
    std::unique_ptr<CollisionGrid2D> collision{};
    std::vector<EntityRecord> entities{};
    std::vector<std::uint64_t> entity_lifecycle_epochs{};
    std::vector<std::uint64_t> collision_event_epoch_snapshot{};
    std::vector<GroupRange> group_ranges{};
    std::vector<CollisionRule2D> collision_rules{};
    std::vector<SystemState> system_states{};
    std::vector<std::uint64_t> rule_invocations{};
    std::vector<std::uint64_t> rule_random_keys{};
    std::vector<PoolRuntimeState> pool_states{};
    std::vector<std::uint32_t> scene_enter_rules{};
    std::vector<std::uint32_t> pre_tick_rules{};
    std::vector<std::vector<std::uint32_t>> collision_rule_dispatch{};
    std::vector<std::vector<std::uint8_t>> grid_occupancy{};
    std::vector<ActionState> actions{};
    std::vector<ActionState> tick_actions{};
    std::vector<ActionState> pending_tick_edges{};
    std::vector<std::int32_t> states{};
    std::vector<SceneSnapshot> scene_snapshots{};
    std::unordered_map<std::string, std::uint32_t, TransparentStringHash, std::equal_to<>> state_indices{};
    std::vector<std::vector<std::vector<TextToken>>> ui_text_tokens{};
    std::vector<Diagnostic> warnings{};
    std::uint32_t active_scene{0U};
    std::uint32_t focused_button{0U};
    std::uint64_t frame_index{0U};
    std::uint64_t simulation_tick{0U};
    std::uint64_t scene_tick{0U};
    std::uint32_t active_pooled_entities{0U};
    std::uint32_t peak_active_pooled_entities{0U};
    RenderFpsCap requested_fps{RenderFpsCap::fps_60};
    RenderFpsCap effective_fps{RenderFpsCap::fps_60};
    float master_volume{1.0F};
    float virtual_mouse_x{0.0F};
    float virtual_mouse_y{0.0F};
    bool audio_available{false};
    bool ready{false};
    std::chrono::steady_clock::time_point last_frame_time{};

#if defined(AI2D_ENABLE_GPU)
    PlatformWindow window{};
    Renderer2D renderer{};
    AudioMixer audio{};
    std::vector<TextureHandle> textures{};
    TextureHandle white_texture{};
    std::vector<SpriteSubmission2D> extracted{};
    std::string formatted_text{};
#endif

    void warn_once(Diagnostic diagnostic) {
        const auto exists = std::find_if(warnings.begin(), warnings.end(), [&](const Diagnostic& existing) {
            return existing.code == diagnostic.code;
        });
        if (exists == warnings.end()) {
            diagnostic.severity = Severity::warning;
            warnings.push_back(std::move(diagnostic));
        }
    }

    void reset_states() {
        for (std::size_t index = 0U; index < plan.states.size(); ++index) states[index] = plan.states[index].initial;
    }

    void synchronize_interpolation_history() noexcept {
        if (world == nullptr) return;
        auto transforms = world->query<Transform2D>();
        for (auto item : transforms) item.component.previous_position = item.component.position;
        world->record_query(transforms.candidate_count(), transforms.candidate_count());
    }

    Result<void> capture_active_scene() {
        if (active_scene >= scene_snapshots.size() || entities.size() != scene_snapshots[active_scene].transforms.size()) {
            return std::unexpected(runtime_error(
                DiagnosticCode::internal_error, "Active scene snapshot storage is inconsistent"));
        }
        auto& snapshot = scene_snapshots[active_scene];
        for (std::size_t index = 0U; index < entities.size(); ++index) {
            const auto& record = entities[index];
            const auto* transform = world->transform(record.entity);
            if (transform == nullptr) {
                return std::unexpected(runtime_error(
                    DiagnosticCode::internal_error, "Active scene entity is missing its transform"));
            }
            snapshot.transforms[index] = *transform;
            if (record.has_velocity) {
                const auto* velocity = world->velocity(record.entity);
                if (velocity == nullptr) {
                    return std::unexpected(runtime_error(
                        DiagnosticCode::internal_error, "Active scene entity is missing its velocity"));
                }
                snapshot.velocities[index] = *velocity;
            }
            if (record.has_sprite) {
                const auto* sprite = world->sprite(record.entity);
                if (sprite == nullptr) {
                    return std::unexpected(runtime_error(
                        DiagnosticCode::internal_error, "Active scene entity is missing its sprite"));
                }
                snapshot.sprites[index] = *sprite;
            }
            if (record.has_collider) {
                const auto* collider_component = world->collider(record.entity);
                if (collider_component == nullptr) {
                    return std::unexpected(runtime_error(
                        DiagnosticCode::internal_error, "Active scene entity is missing collision state"));
                }
                snapshot.colliders[index] = *collider_component;
            }
            const auto* entity_state = world->entity_state(record.entity);
            if (entity_state == nullptr) {
                return std::unexpected(runtime_error(
                    DiagnosticCode::internal_error, "Active scene entity is missing active state"));
            }
            snapshot.entity_states[index] = *entity_state;
        }
        snapshot.system_states = system_states;
        snapshot.rule_invocations = rule_invocations;
        if (snapshot.pool_states.size() != pool_states.size()) {
            return std::unexpected(pool_state_error("Active scene pool snapshot storage is inconsistent"));
        }
        for (std::size_t pool_index = 0U; pool_index < pool_states.size(); ++pool_index) {
            const auto& source = pool_states[pool_index];
            auto& destination = snapshot.pool_states[pool_index];
            if (source.free_ring.size() != destination.free_ring.size() ||
                source.active_next.size() != destination.active_next.size() ||
                source.active_previous.size() != destination.active_previous.size() ||
                source.expiration_ticks.size() != destination.expiration_ticks.size() ||
                source.acquired.size() != destination.acquired.size()) {
                return std::unexpected(pool_state_error("Active scene pool snapshot capacity is inconsistent"));
            }
            std::copy(source.free_ring.begin(), source.free_ring.end(), destination.free_ring.begin());
            std::copy(source.active_next.begin(), source.active_next.end(), destination.active_next.begin());
            std::copy(
                source.active_previous.begin(), source.active_previous.end(), destination.active_previous.begin());
            std::copy(
                source.expiration_ticks.begin(), source.expiration_ticks.end(),
                destination.expiration_ticks.begin());
            std::copy(source.acquired.begin(), source.acquired.end(), destination.acquired.begin());
            destination.free_head = source.free_head;
            destination.free_count = source.free_count;
            destination.active_head = source.active_head;
            destination.active_tail = source.active_tail;
            destination.active_count = source.active_count;
        }
        snapshot.contacts.clear();
        for (const auto& pair : collision->active_contact_pairs()) {
            const auto find_local_index = [&](const EntityId entity) -> std::uint32_t {
                const auto found = std::find_if(
                    entities.begin(), entities.end(), [&](const EntityRecord& record) {
                        return record.entity == entity;
                    });
                return found == entities.end()
                           ? std::numeric_limits<std::uint32_t>::max()
                           : static_cast<std::uint32_t>(std::distance(entities.begin(), found));
            };
            const auto a = find_local_index(pair.entity_a);
            const auto b = find_local_index(pair.entity_b);
            if (a >= entities.size() || b >= entities.size() ||
                snapshot.contacts.size() >= snapshot.contacts.capacity()) {
                return std::unexpected(runtime_error(
                    DiagnosticCode::runtime_contact_state_invalid,
                    "Active contact state cannot be represented by the retained scene snapshot"));
            }
            snapshot.contacts.push_back({pair.rule_index, a, b});
        }
        snapshot.scene_tick = scene_tick;
        snapshot.focused_button = focused_button;
        snapshot.retained = true;
        return {};
    }

    void set_entity_active(EntityRecord& record, const bool active, GameRuntimeFrameMetrics* const metrics = nullptr) {
        auto* state = world->entity_state(record.entity);
        if (state == nullptr) return;
        if (state->active != active && metrics != nullptr) ++metrics->active_state_changes;
        if (state->active && !active) {
            if (record.entity.index < entity_lifecycle_epochs.size()) {
                ++entity_lifecycle_epochs[record.entity.index];
            }
            if (collision != nullptr) collision->discard_contacts_for(record.entity);
        }
        state->active = active;
        if (record.has_collider) {
            if (auto* collider_component = world->collider(record.entity); collider_component != nullptr) {
                collider_component->enabled = active && record.initial_collider.enabled;
            }
        }
        if (record.has_sprite) {
            if (auto* sprite = world->sprite(record.entity); sprite != nullptr) {
                sprite->visible = active && record.initial_sprite.visible;
            }
        }
    }

    [[nodiscard]] Diagnostic pool_state_error(const char* const message) const {
        return runtime_error(DiagnosticCode::runtime_pool_state_invalid, message);
    }

    void initialize_pool_storage(
        PoolRuntimeState& state,
        const std::uint32_t capacity,
        const std::uint32_t initially_active) {
        state.free_ring.resize(capacity);
        state.active_next.assign(capacity, PoolRuntimeState::invalid_slot);
        state.active_previous.assign(capacity, PoolRuntimeState::invalid_slot);
        state.expiration_ticks.assign(capacity, PoolRuntimeState::no_expiration);
        state.acquired.assign(capacity, std::uint8_t{0U});
        state.free_head = 0U;
        state.free_count = 0U;
        state.active_head = PoolRuntimeState::invalid_slot;
        state.active_tail = PoolRuntimeState::invalid_slot;
        state.active_count = 0U;
        for (std::uint32_t slot = 0U; slot < initially_active; ++slot) {
            state.acquired[slot] = 1U;
            state.active_previous[slot] = state.active_tail;
            if (state.active_tail != PoolRuntimeState::invalid_slot) {
                state.active_next[state.active_tail] = slot;
            } else {
                state.active_head = slot;
            }
            state.active_tail = slot;
            ++state.active_count;
        }
        for (std::uint32_t slot = initially_active; slot < capacity; ++slot) {
            state.free_ring[state.free_count++] = slot;
        }
    }

    [[nodiscard]] bool pool_storage_valid(
        const std::uint32_t pool_index,
        const PoolRuntimeState& state) const noexcept {
        if (pool_index >= plan.scenes[active_scene].pools.size()) return false;
        const auto group_index = plan.scenes[active_scene].pools[pool_index].spawn_group_index;
        if (group_index >= group_ranges.size()) return false;
        const auto capacity = group_ranges[group_index].count;
        return capacity != 0U && state.free_ring.size() == capacity &&
               state.active_next.size() == capacity && state.active_previous.size() == capacity &&
               state.expiration_ticks.size() == capacity && state.acquired.size() == capacity &&
               state.free_count <= capacity && state.active_count <= capacity &&
               state.free_count + state.active_count == capacity &&
               (state.free_count == 0U || state.free_head < capacity) &&
               (state.active_head == PoolRuntimeState::invalid_slot || state.active_head < capacity) &&
               (state.active_tail == PoolRuntimeState::invalid_slot || state.active_tail < capacity);
    }

    [[nodiscard]] EntityRecord& pool_record(const std::uint32_t pool_index, const std::uint32_t slot) {
        const auto group_index = plan.scenes[active_scene].pools[pool_index].spawn_group_index;
        return entities[group_ranges[group_index].begin + slot];
    }

    Result<void> restore_record_components(EntityRecord& record) {
        auto* transform = world->transform(record.entity);
        if (transform == nullptr) {
            return std::unexpected(pool_state_error("Pooled entity is missing Transform2D"));
        }
        *transform = record.initial_transform;
        if (record.has_velocity) {
            auto* velocity = world->velocity(record.entity);
            if (velocity == nullptr) {
                return std::unexpected(pool_state_error("Pooled entity is missing Velocity2D"));
            }
            *velocity = record.initial_velocity;
        }
        if (record.has_sprite) {
            auto* sprite = world->sprite(record.entity);
            if (sprite == nullptr) {
                return std::unexpected(pool_state_error("Pooled entity is missing Sprite2D"));
            }
            *sprite = record.initial_sprite;
        }
        if (record.has_collider) {
            auto* collider_component = world->collider(record.entity);
            if (collider_component == nullptr) {
                return std::unexpected(pool_state_error("Pooled entity is missing Collider2D"));
            }
            *collider_component = record.initial_collider;
        }
        return {};
    }

    Result<void> detach_active_slot(PoolRuntimeState& state, const std::uint32_t slot) {
        if (slot >= state.acquired.size() || state.acquired[slot] == 0U || state.active_count == 0U) {
            return std::unexpected(pool_state_error("Pool active list contains an invalid slot"));
        }
        const auto previous = state.active_previous[slot];
        const auto next = state.active_next[slot];
        if ((previous != PoolRuntimeState::invalid_slot && previous >= state.acquired.size()) ||
            (next != PoolRuntimeState::invalid_slot && next >= state.acquired.size())) {
            return std::unexpected(pool_state_error("Pool active list link is out of range"));
        }
        if (previous == PoolRuntimeState::invalid_slot) state.active_head = next;
        else state.active_next[previous] = next;
        if (next == PoolRuntimeState::invalid_slot) state.active_tail = previous;
        else state.active_previous[next] = previous;
        state.active_previous[slot] = PoolRuntimeState::invalid_slot;
        state.active_next[slot] = PoolRuntimeState::invalid_slot;
        state.expiration_ticks[slot] = PoolRuntimeState::no_expiration;
        state.acquired[slot] = 0U;
        --state.active_count;
        return {};
    }

    Result<void> append_active_slot(
        PoolRuntimeState& state,
        const std::uint32_t slot,
        const std::uint64_t expiration_tick) {
        if (slot >= state.acquired.size() || state.acquired[slot] != 0U ||
            (state.active_tail != PoolRuntimeState::invalid_slot &&
             state.active_tail >= state.acquired.size())) {
            return std::unexpected(pool_state_error("Pool acquisition would corrupt the active list"));
        }
        state.acquired[slot] = 1U;
        state.active_previous[slot] = state.active_tail;
        state.active_next[slot] = PoolRuntimeState::invalid_slot;
        if (state.active_tail == PoolRuntimeState::invalid_slot) state.active_head = slot;
        else state.active_next[state.active_tail] = slot;
        state.active_tail = slot;
        state.expiration_ticks[slot] = expiration_tick;
        ++state.active_count;
        return {};
    }

    Result<void> append_free_slot(PoolRuntimeState& state, const std::uint32_t slot) {
        const auto capacity = static_cast<std::uint32_t>(state.free_ring.size());
        if (capacity == 0U || state.free_count >= capacity || state.free_head >= capacity) {
            return std::unexpected(pool_state_error("Pool available-slot FIFO is full or invalid"));
        }
        state.free_ring[(state.free_head + state.free_count) % capacity] = slot;
        ++state.free_count;
        return {};
    }

    Result<std::uint32_t> pop_free_slot(PoolRuntimeState& state) {
        const auto capacity = static_cast<std::uint32_t>(state.free_ring.size());
        if (capacity == 0U || state.free_count == 0U || state.free_head >= capacity) {
            return std::unexpected(pool_state_error("Pool available-slot FIFO is empty or invalid"));
        }
        const auto slot = state.free_ring[state.free_head];
        if (slot >= capacity || state.acquired[slot] != 0U) {
            return std::unexpected(pool_state_error("Pool available-slot FIFO references an acquired slot"));
        }
        state.free_head = (state.free_head + 1U) % capacity;
        --state.free_count;
        return slot;
    }

    Result<EntityRecord*> resolve_single_target(
        const GameRuleTargetPlan& target,
        const CollisionEvent2D* const collision_event) {
        EntityRecord* resolved = nullptr;
        if (auto result = for_each_target(target, collision_event, [&](EntityRecord& record) -> Result<void> {
                if (resolved != nullptr) {
                    return std::unexpected(pool_state_error("Pool action target resolved to multiple entities"));
                }
                resolved = &record;
                return {};
            });
            !result) {
            return std::unexpected(std::move(result.error()));
        }
        if (resolved == nullptr) {
            return std::unexpected(pool_state_error("Pool action target did not resolve to an entity"));
        }
        return resolved;
    }

    Result<void> release_pool_slot(
        const std::uint32_t pool_index,
        const std::uint32_t slot,
        GameRuntimeFrameMetrics& metrics,
        const bool expiration) {
        if (pool_index >= pool_states.size()) {
            return std::unexpected(pool_state_error("Pool release references an invalid pool"));
        }
        auto& state = pool_states[pool_index];
        if (!pool_storage_valid(pool_index, state) || slot >= state.acquired.size()) {
            return std::unexpected(pool_state_error("Pool release found invalid runtime storage"));
        }
        if (state.acquired[slot] == 0U) return {};
        if (auto detached = detach_active_slot(state, slot); !detached) return detached;
        if (auto appended = append_free_slot(state, slot); !appended) return appended;
        set_entity_active(pool_record(pool_index, slot), false, &metrics);
        if (active_pooled_entities == 0U) {
            return std::unexpected(pool_state_error("Pool active entity count underflowed"));
        }
        --active_pooled_entities;
        if (expiration) ++metrics.pool_expirations;
        else ++metrics.pool_releases;
        return {};
    }

    Result<void> reset_pool(const std::uint32_t pool_index, GameRuntimeFrameMetrics* const metrics) {
        if (pool_index >= pool_states.size()) {
            return std::unexpected(pool_state_error("Pool reset references an invalid pool"));
        }
        const auto& pool = plan.scenes[active_scene].pools[pool_index];
        const auto& group = plan.scenes[active_scene].spawn_groups[pool.spawn_group_index];
        auto& state = pool_states[pool_index];
        if (!pool_storage_valid(pool_index, state)) {
            return std::unexpected(pool_state_error("Pool reset found invalid runtime storage"));
        }
        const auto previous_active = state.active_count;
        for (std::uint32_t slot = 0U; slot < group.count; ++slot) {
            auto& record = pool_record(pool_index, slot);
            collision->discard_contacts_for(record.entity);
            if (auto restored = restore_record_components(record); !restored) return restored;
            set_entity_active(record, slot < group.active_count, metrics);
        }
        initialize_pool_storage(state, group.count, group.active_count);
        if (active_pooled_entities < previous_active) {
            return std::unexpected(pool_state_error("Pool reset active entity count underflowed"));
        }
        active_pooled_entities = active_pooled_entities - previous_active + group.active_count;
        peak_active_pooled_entities = std::max(peak_active_pooled_entities, active_pooled_entities);
        if (metrics != nullptr) ++metrics->pool_resets;
        return {};
    }

    [[nodiscard]] std::uint32_t group_active_count(const std::uint32_t group_index) const noexcept {
        if (group_index >= group_ranges.size()) return 0U;
        const auto range = group_ranges[group_index];
        std::uint32_t count = 0U;
        for (std::uint32_t offset = 0U; offset < range.count; ++offset) {
            const auto* state = world->entity_state(entities[range.begin + offset].entity);
            if (state != nullptr && state->active) ++count;
        }
        return count;
    }

    Result<void> reset_spawn_group(
        const std::uint32_t group_index,
        GameRuntimeFrameMetrics* const metrics = nullptr) {
        if (group_index >= group_ranges.size()) {
            return std::unexpected(runtime_error(DiagnosticCode::internal_error, "Spawn group index is invalid"));
        }
        const auto range = group_ranges[group_index];
        for (std::uint32_t offset = 0U; offset < range.count; ++offset) {
            auto& record = entities[range.begin + offset];
            auto* transform = world->transform(record.entity);
            if (transform == nullptr) {
                return std::unexpected(runtime_error(
                    DiagnosticCode::internal_error, "Entity reset metadata does not match the world"));
            }
            *transform = record.initial_transform;
            if (record.has_collider) {
                auto* collider_component = world->collider(record.entity);
                if (collider_component == nullptr) {
                    return std::unexpected(runtime_error(
                        DiagnosticCode::internal_error, "Reset entity is missing its collider"));
                }
                *collider_component = record.initial_collider;
            }
            if (record.has_velocity) {
                auto* velocity = world->velocity(record.entity);
                if (velocity == nullptr) {
                    return std::unexpected(runtime_error(
                        DiagnosticCode::internal_error, "Reset entity is missing its velocity"));
                }
                *velocity = record.initial_velocity;
            }
            if (record.has_sprite) {
                auto* sprite = world->sprite(record.entity);
                if (sprite == nullptr) {
                    return std::unexpected(runtime_error(
                        DiagnosticCode::internal_error, "Reset entity is missing its sprite"));
                }
                *sprite = record.initial_sprite;
            }
            set_entity_active(record, record.initial_active, metrics);
        }
        return {};
    }

    Result<void> reset_legacy_collider_group(
        const SymbolId group,
        GameRuntimeFrameMetrics* const metrics = nullptr) {
        for (std::uint32_t group_index = 0U; group_index < group_ranges.size(); ++group_index) {
            const auto range = group_ranges[group_index];
            bool matches = false;
            for (std::uint32_t offset = 0U; offset < range.count; ++offset) {
                const auto& record = entities[range.begin + offset];
                if (record.has_collider && record.collider_group == group) {
                    matches = true;
                    break;
                }
            }
            if (matches) {
                if (auto reset = reset_spawn_group(group_index, metrics); !reset) return reset;
            }
        }
        return {};
    }

    Result<void> load_scene(const std::uint32_t scene_index, const bool restore_retained = false) {
        if (scene_index >= plan.scenes.size()) {
            return std::unexpected(runtime_error(DiagnosticCode::game_transition_invalid, "Scene index is invalid"));
        }
        const auto& scene = plan.scenes[scene_index];
        auto next_world = std::make_unique<World>();
        if (auto reserved = next_world->reserve(scene.world_capacity); !reserved) {
            return std::unexpected(std::move(reserved.error()));
        }
        auto next_collision = std::make_unique<CollisionGrid2D>();
        if (auto initialized = next_collision->initialize({
                scene.collision_bounds,
                scene.collision_cell_size,
                scene.max_colliders,
                scene.max_grid_references,
                scene.max_candidate_pairs,
                scene.max_contact_pairs,
                scene.max_impacts,
            });
            !initialized) {
            return std::unexpected(std::move(initialized.error()));
        }
        std::vector<EntityRecord> next_entities{};
        next_entities.reserve(scene.total_spawn_count);
        std::vector<std::uint64_t> next_entity_lifecycle_epochs(scene.total_spawn_count, 0U);
        std::vector<std::uint64_t> next_collision_event_epoch_snapshot(scene.total_spawn_count, 0U);
        std::vector<GroupRange> next_group_ranges{};
        next_group_ranges.reserve(scene.spawn_groups.size());
        std::vector<std::int32_t> next_pool_for_group(scene.spawn_groups.size(), -1);
        for (std::size_t pool_index = 0U; pool_index < scene.pools.size(); ++pool_index) {
            next_pool_for_group[scene.pools[pool_index].spawn_group_index] = static_cast<std::int32_t>(pool_index);
        }
        for (std::uint32_t group_index = 0U; group_index < scene.spawn_groups.size(); ++group_index) {
            const auto& group = scene.spawn_groups[group_index];
            next_group_ranges.push_back({static_cast<std::uint32_t>(next_entities.size()), group.count});
            for (std::uint32_t item = 0U; item < group.count; ++item) {
                const auto column = item % group.placement.columns;
                const auto row = item / group.placement.columns;
                const double position_x = static_cast<double>(group.placement.origin.x) +
                                          static_cast<double>(column) * group.placement.spacing.x +
                                          group.transform.position_offset.x;
                const double position_y = static_cast<double>(group.placement.origin.y) +
                                          static_cast<double>(row) * group.placement.spacing.y +
                                          group.transform.position_offset.y;
                if (!representable_float(position_x) || !representable_float(position_y)) {
                    return std::unexpected(runtime_error(
                        DiagnosticCode::runtime_numeric_state_invalid,
                        "Spawn placement exceeds the supported numeric range"));
                }
                Transform2D transform{};
                transform.position = {
                    static_cast<float>(position_x),
                    static_cast<float>(position_y),
                };
                transform.previous_position = transform.position;
                transform.rotation = group.transform.rotation;
                transform.scale = group.transform.scale;
                std::optional<Velocity2D> velocity{};
                if (group.has_velocity) velocity = Velocity2D{group.velocity.linear, group.velocity.angular};
                std::optional<Sprite2D> sprite{};
                if (group.has_sprite) {
#if defined(AI2D_ENABLE_GPU)
                    const auto texture = textures[group.sprite.asset_index];
#else
                    const TextureHandle texture{group.sprite.asset_index, 1U};
#endif
                    sprite = Sprite2D{
                        texture,
                        group.sprite.size,
                        group.sprite.pivot,
                        group.sprite.tint,
                        group.sprite.layer,
                        group.sprite.visible,
                        group.sprite.uv,
                    };
                }
                auto entity = next_world->create_entity(transform, velocity, sprite);
                if (!entity) return std::unexpected(std::move(entity.error()));
                EntityRecord record{};
                record.entity = *entity;
                record.spawn_group_index = group_index;
                record.initial_transform = transform;
                record.has_velocity = group.has_velocity;
                record.has_sprite = group.has_sprite;
                if (velocity) record.initial_velocity = *velocity;
                if (sprite) record.initial_sprite = *sprite;
                if (group.has_collider) {
                    const Collider2D collider_component{
                        group.collider.offset,
                        group.collider.half_extent,
                        group.collider.group,
                        translate_motion(group.collider.motion),
                        group.collider.trigger,
                        group.collider.enabled,
                    };
                    if (auto added = next_world->add(*entity, collider_component); !added) {
                        return std::unexpected(std::move(added.error()));
                    }
                    record.collider_group = group.collider.group;
                    record.initial_collider = collider_component;
                    record.has_collider = true;
                }
                record.initial_active = item < group.active_count;
                if (next_pool_for_group[group_index] >= 0) {
                    record.pooled = true;
                    record.pool_index = static_cast<std::uint32_t>(next_pool_for_group[group_index]);
                    record.pool_slot_index = item;
                }
                if (auto added = next_world->add(*entity, EntityState2D{record.initial_active}); !added) {
                    return std::unexpected(std::move(added.error()));
                }
                next_entities.push_back(record);
            }
        }
        std::vector<CollisionRule2D> next_rules{};
        next_rules.reserve(scene.collision_rules.size());
        for (const auto& source_rule : scene.collision_rules) {
            CollisionRule2D rule{};
            rule.group_a = source_rule.group_a;
            rule.group_b = source_rule.group_b;
            rule.interaction = source_rule.interaction == GameCollisionInteraction::solid
                                   ? CollisionInteraction2D::solid
                               : source_rule.interaction == GameCollisionInteraction::trigger
                                   ? CollisionInteraction2D::trigger
                                   : CollisionInteraction2D::legacy;
            rule.reaction_count = static_cast<std::uint32_t>(source_rule.reactions.size());
            for (std::size_t index = 0U; index < source_rule.reactions.size(); ++index) {
                const auto& source_reaction = source_rule.reactions[index];
                rule.reactions[index] = {
                    translate_reaction(source_reaction.kind),
                    source_reaction.target == GameReactionTarget::a ? CollisionTarget2D::a : CollisionTarget2D::b,
                    source_reaction.state_index,
                    source_reaction.value,
                    source_reaction.group,
                    source_reaction.asset_index,
                };
            }
            next_rules.push_back(rule);
        }
        std::vector<SystemState> next_system_states(scene.systems.size());
        for (std::size_t system_index = 0U; system_index < scene.systems.size(); ++system_index) {
            if (scene.systems[system_index].operation == GameOperationId::grid_motion) {
                next_system_states[system_index].current_direction = scene.systems[system_index].initial_direction;
            }
        }
        std::vector<std::uint64_t> next_rule_invocations(scene.rules.size(), 0U);
        std::vector<PoolRuntimeState> next_pool_states(scene.pools.size());
        for (std::size_t pool_index = 0U; pool_index < scene.pools.size(); ++pool_index) {
            const auto group_index = scene.pools[pool_index].spawn_group_index;
            initialize_pool_storage(
                next_pool_states[pool_index], scene.spawn_groups[group_index].count,
                scene.spawn_groups[group_index].active_count);
        }
        std::vector<std::uint64_t> next_rule_random_keys{};
        next_rule_random_keys.reserve(scene.rules.size());
        for (const auto& rule : scene.rules) {
            next_rule_random_keys.push_back(stable_text_hash(plan.symbol(rule.symbol)));
        }
        std::vector<std::uint32_t> next_scene_enter_rules{};
        std::vector<std::uint32_t> next_pre_tick_rules{};
        next_scene_enter_rules.reserve(scene.rules.size());
        next_pre_tick_rules.reserve(scene.rules.size());
        std::vector<std::vector<std::uint32_t>> next_collision_dispatch(scene.collision_rules.size());
        for (std::uint32_t rule_index = 0U; rule_index < scene.rules.size(); ++rule_index) {
            const auto& rule = scene.rules[rule_index];
            if (rule.event.kind == GameRuleEventKind::scene_enter) {
                next_scene_enter_rules.push_back(rule_index);
            } else if (rule.event.kind == GameRuleEventKind::collision ||
                       rule.event.kind == GameRuleEventKind::contact_begin ||
                       rule.event.kind == GameRuleEventKind::contact_end) {
                next_collision_dispatch[rule.event.collision_rule_index].push_back(rule_index);
            } else {
                next_pre_tick_rules.push_back(rule_index);
            }
        }
        std::vector<std::vector<std::uint8_t>> next_grid_occupancy(scene.grids.size());
        for (std::size_t grid_index = 0U; grid_index < scene.grids.size(); ++grid_index) {
            const auto& grid = scene.grids[grid_index];
            next_grid_occupancy[grid_index].resize(
                static_cast<std::size_t>(grid.columns) * static_cast<std::size_t>(grid.rows));
        }
        std::uint64_t next_scene_tick = 0U;
        if (restore_retained && scene_snapshots[scene_index].retained) {
            const auto& snapshot = scene_snapshots[scene_index];
            if (snapshot.transforms.size() != next_entities.size()) {
                return std::unexpected(runtime_error(
                    DiagnosticCode::internal_error, "Retained scene snapshot does not match its entity plan"));
            }
            for (std::size_t index = 0U; index < next_entities.size(); ++index) {
                const auto& record = next_entities[index];
                *next_world->transform(record.entity) = snapshot.transforms[index];
                if (record.has_velocity) *next_world->velocity(record.entity) = snapshot.velocities[index];
                if (record.has_sprite) *next_world->sprite(record.entity) = snapshot.sprites[index];
                if (record.has_collider) *next_world->collider(record.entity) = snapshot.colliders[index];
                *next_world->entity_state(record.entity) = snapshot.entity_states[index];
            }
            next_system_states = snapshot.system_states;
            next_rule_invocations = snapshot.rule_invocations;
            if (snapshot.pool_states.size() != next_pool_states.size()) {
                return std::unexpected(pool_state_error("Retained pool snapshot does not match its scene plan"));
            }
            next_pool_states = snapshot.pool_states;
            next_scene_tick = snapshot.scene_tick;
            std::vector<CollisionContactPair2D> restored_contacts{};
            restored_contacts.reserve(snapshot.contacts.size());
            for (const auto& contact : snapshot.contacts) {
                if (contact.rule_index >= scene.collision_rules.size() ||
                    contact.entity_a_index >= next_entities.size() ||
                    contact.entity_b_index >= next_entities.size()) {
                    return std::unexpected(runtime_error(
                        DiagnosticCode::runtime_contact_state_invalid,
                        "Retained contact snapshot contains an invalid local reference"));
                }
                restored_contacts.push_back({
                    contact.rule_index,
                    next_entities[contact.entity_a_index].entity,
                    next_entities[contact.entity_b_index].entity,
                });
            }
            if (auto restored = next_collision->restore_contact_pairs(restored_contacts); !restored) {
                return std::unexpected(std::move(restored.error()));
            }
        }
        world = std::move(next_world);
        collision = std::move(next_collision);
        entities = std::move(next_entities);
        entity_lifecycle_epochs = std::move(next_entity_lifecycle_epochs);
        collision_event_epoch_snapshot = std::move(next_collision_event_epoch_snapshot);
        group_ranges = std::move(next_group_ranges);
        collision_rules = std::move(next_rules);
        system_states = std::move(next_system_states);
        rule_invocations = std::move(next_rule_invocations);
        pool_states = std::move(next_pool_states);
        rule_random_keys = std::move(next_rule_random_keys);
        scene_enter_rules = std::move(next_scene_enter_rules);
        pre_tick_rules = std::move(next_pre_tick_rules);
        collision_rule_dispatch = std::move(next_collision_dispatch);
        grid_occupancy = std::move(next_grid_occupancy);
        scene_tick = next_scene_tick;
        active_pooled_entities = 0U;
        for (const auto& pool_state : pool_states) active_pooled_entities += pool_state.active_count;
        peak_active_pooled_entities = std::max(peak_active_pooled_entities, active_pooled_entities);
        active_scene = scene_index;
        focused_button = restore_retained && scene_snapshots[scene_index].retained
                             ? scene_snapshots[scene_index].focused_button
                             : 0U;
        return {};
    }

    [[nodiscard]] bool group_inactive(const SymbolId group) const noexcept {
        bool found = false;
        for (const auto& record : entities) {
            if (!record.has_collider || record.collider_group != group) continue;
            found = true;
            const auto* state = world->entity_state(record.entity);
            const auto* collider_component = world->collider(record.entity);
            if ((state == nullptr || state->active) && (collider_component == nullptr || collider_component->enabled)) {
                return false;
            }
        }
        return found;
    }

    bool point_in_ui(const UiElementPlan& element) const noexcept {
        return virtual_mouse_x >= element.position.x - element.size.x * 0.5F &&
               virtual_mouse_x <= element.position.x + element.size.x * 0.5F &&
               virtual_mouse_y >= element.position.y - element.size.y * 0.5F &&
               virtual_mouse_y <= element.position.y + element.size.y * 0.5F;
    }

    void update_actions(const InputSnapshot& input) {
        const auto drawable_width = input.drawable_extent.width == 0U ? plan.window.width : input.drawable_extent.width;
        const auto drawable_height = input.drawable_extent.height == 0U ? plan.window.height : input.drawable_extent.height;
        virtual_mouse_x = input.mouse_x * static_cast<float>(plan.window.virtual_width) /
                          static_cast<float>(std::max(1U, drawable_width));
        virtual_mouse_y = input.mouse_y * static_cast<float>(plan.window.virtual_height) /
                          static_cast<float>(std::max(1U, drawable_height));
        for (std::size_t action_index = 0U; action_index < plan.actions.size(); ++action_index) {
            const auto& binding = plan.actions[action_index];
            ActionState state{};
            for (std::uint32_t key_index = 0U; key_index < binding.key_count; ++key_index) {
                const auto key = translate_key(binding.keys[key_index]);
                state.down = state.down || input.down(key);
                state.pressed = state.pressed || input.pressed(key);
                state.released = state.released || input.released(key);
            }
            if (binding.mouse_left) {
                state.down = state.down || input.mouse_left;
                state.pressed = state.pressed || input.mouse_left_pressed;
                state.released = state.released || input.mouse_left_released;
            }
            actions[action_index] = state;
            pending_tick_edges[action_index].pressed = pending_tick_edges[action_index].pressed || state.pressed;
            pending_tick_edges[action_index].released = pending_tick_edges[action_index].released || state.released;
        }
        const auto& ui = plan.scenes[active_scene].ui;
        const auto button_count = static_cast<std::uint32_t>(std::count_if(
            ui.begin(), ui.end(), [](const UiElementPlan& element) { return element.kind == UiElementKind::button; }));
        if (button_count > 0U && input.pressed(InputKey::tab)) {
            focused_button = (focused_button + 1U) % button_count;
        }
        std::uint32_t button = 0U;
        for (const auto& element : ui) {
            if (element.kind != UiElementKind::button) continue;
            const bool mouse_activation = input.mouse_left_pressed && point_in_ui(element);
            const bool keyboard_activation = button == focused_button &&
                                             (input.pressed(InputKey::enter) || input.pressed(InputKey::space));
            if (mouse_activation || keyboard_activation) {
                actions[element.action].pressed = true;
                actions[element.action].down = true;
                pending_tick_edges[element.action].pressed = true;
            }
            ++button;
        }
    }

    Result<void> set_fps(const RenderFpsCap cap, const bool persist) {
        requested_fps = cap;
        effective_fps = cap;
#if defined(AI2D_ENABLE_GPU)
        if (!options.headless && renderer.initialized()) {
            if (auto changed = renderer.set_fps_cap(cap); !changed) {
                return std::unexpected(std::move(changed.error()));
            }
        }
        if (persist && !options.headless) {
            auto saved = save_game_settings(
                plan.symbol(plan.organization), plan.symbol(plan.application), {requested_fps, master_volume});
            if (!saved) warn_once(std::move(saved.error()));
        }
#else
        (void)persist;
#endif
        return {};
    }

    void set_state_value(const std::uint32_t state_index, const std::int64_t value) noexcept {
        const auto& state_plan = plan.states[state_index];
        states[state_index] = static_cast<std::int32_t>(
            std::clamp<std::int64_t>(value, state_plan.minimum, state_plan.maximum));
    }

    template <class Function>
    Result<void> for_each_target(
        const GameRuleTargetPlan& target,
        const CollisionEvent2D* const collision_event,
        Function&& function) {
        if (target.kind == GameRuleTargetKind::collision_a || target.kind == GameRuleTargetKind::collision_b) {
            if (collision_event == nullptr) {
                return std::unexpected(runtime_error(
                    DiagnosticCode::internal_error, "Collision target used outside a collision event"));
            }
            const auto entity = target.kind == GameRuleTargetKind::collision_a
                                    ? collision_event->entity_a
                                    : collision_event->entity_b;
            if (entity.index >= entities.size() || entities[entity.index].entity != entity) {
                return std::unexpected(runtime_error(
                    DiagnosticCode::internal_error, "Collision target does not map to a scene entity"));
            }
            return function(entities[entity.index]);
        }
        if (target.spawn_group_index >= group_ranges.size()) {
            return std::unexpected(runtime_error(DiagnosticCode::internal_error, "Rule target group is invalid"));
        }
        const auto range = group_ranges[target.spawn_group_index];
        if (target.kind == GameRuleTargetKind::spawn_index) {
            if (target.item_index >= range.count) {
                return std::unexpected(runtime_error(DiagnosticCode::internal_error, "Rule target index is invalid"));
            }
            return function(entities[range.begin + target.item_index]);
        }
        for (std::uint32_t offset = 0U; offset < range.count; ++offset) {
            if (auto applied = function(entities[range.begin + offset]); !applied) return applied;
        }
        return {};
    }

    Result<void> spawn_from_pool(
        const GameRuleActionPlan& action,
        const CollisionEvent2D* const collision_event,
        GameRuntimeFrameMetrics& metrics) {
        ++metrics.pool_acquire_attempts;
        if (action.pool_index >= pool_states.size()) {
            return std::unexpected(pool_state_error("Pool acquisition references an invalid pool"));
        }
        auto& state = pool_states[action.pool_index];
        if (!pool_storage_valid(action.pool_index, state)) {
            return std::unexpected(pool_state_error("Pool acquisition found invalid runtime storage"));
        }
        const auto& pool = plan.scenes[active_scene].pools[action.pool_index];
        bool recycle = false;
        std::uint32_t slot = PoolRuntimeState::invalid_slot;
        if (state.free_count != 0U) {
            slot = state.free_ring[state.free_head];
        } else {
            ++metrics.pool_exhaustions;
            if (pool.on_exhausted == GamePoolExhaustionPolicy::skip) {
                if (action.has_result_state) set_state_value(action.result_state_index, 0);
                return {};
            }
            if (state.active_head == PoolRuntimeState::invalid_slot) {
                return std::unexpected(pool_state_error("Exhausted recycle pool has no oldest active slot"));
            }
            slot = state.active_head;
            recycle = true;
        }
        if (slot >= state.acquired.size()) {
            return std::unexpected(pool_state_error("Pool acquisition selected an out-of-range slot"));
        }
        auto& record = pool_record(action.pool_index, slot);
        Vec2 base_position{};
        if (action.pool_position_kind == GamePoolSpawnPositionKind::initial) {
            base_position = record.initial_transform.position;
        } else if (action.pool_position_kind == GamePoolSpawnPositionKind::constant) {
            base_position = action.pool_position;
        } else {
            auto target = resolve_single_target(action.pool_position_target, collision_event);
            if (!target) return std::unexpected(std::move(target.error()));
            const bool captured_contact_target =
                collision_event != nullptr && plan.schema_version == GameSchemaVersion::v0_5 &&
                (action.pool_position_target.kind == GameRuleTargetKind::collision_a ||
                 action.pool_position_target.kind == GameRuleTargetKind::collision_b);
            if (captured_contact_target) {
                base_position = action.pool_position_target.kind == GameRuleTargetKind::collision_a
                                    ? collision_event->position_a
                                    : collision_event->position_b;
            } else {
                const auto* transform = world->transform((*target)->entity);
                if (transform == nullptr) {
                    return std::unexpected(pool_state_error("Pool position target is missing Transform2D"));
                }
                base_position = transform->position;
            }
        }
        const double position_x = static_cast<double>(base_position.x) + action.pool_position_offset.x;
        const double position_y = static_cast<double>(base_position.y) + action.pool_position_offset.y;
        if (!representable_float(position_x) || !representable_float(position_y)) {
            return std::unexpected(runtime_error(
                DiagnosticCode::runtime_numeric_state_invalid,
                "Pool spawn position exceeds the supported numeric range"));
        }
        std::uint64_t expiration_tick = PoolRuntimeState::no_expiration;
        if (action.has_lifetime) {
            if (scene_tick > std::numeric_limits<std::uint64_t>::max() - action.lifetime_ticks) {
                return std::unexpected(pool_state_error("Pool lifetime tick overflowed"));
            }
            expiration_tick = scene_tick + action.lifetime_ticks;
        }
        if (recycle) {
            if (auto detached = detach_active_slot(state, slot); !detached) return detached;
            set_entity_active(record, false, &metrics);
            if (active_pooled_entities == 0U) {
                return std::unexpected(pool_state_error("Pool recycle active entity count underflowed"));
            }
            --active_pooled_entities;
            ++metrics.pool_recycled_slots;
        } else {
            auto popped = pop_free_slot(state);
            if (!popped) return std::unexpected(std::move(popped.error()));
            if (*popped != slot) {
                return std::unexpected(pool_state_error("Pool FIFO head changed during acquisition"));
            }
        }
        if (auto restored = restore_record_components(record); !restored) return restored;
        auto* transform = world->transform(record.entity);
        transform->position = {static_cast<float>(position_x), static_cast<float>(position_y)};
        transform->previous_position = transform->position;
        if (action.has_rotation_override) transform->rotation = action.rotation;
        if (action.has_velocity_override) world->velocity(record.entity)->linear = action.velocity;
        if (auto appended = append_active_slot(state, slot, PoolRuntimeState::no_expiration); !appended) {
            return appended;
        }
        set_entity_active(record, true, &metrics);
        state.expiration_ticks[slot] = expiration_tick;
        ++active_pooled_entities;
        peak_active_pooled_entities = std::max(peak_active_pooled_entities, active_pooled_entities);
        ++metrics.pool_acquire_successes;
        if (action.has_result_state) set_state_value(action.result_state_index, 1);
        return {};
    }

    Result<void> release_to_pool(
        const GameRuleActionPlan& action,
        const CollisionEvent2D* const collision_event,
        GameRuntimeFrameMetrics& metrics) {
        if (action.pool_index >= pool_states.size()) {
            return std::unexpected(pool_state_error("Pool release references an invalid pool"));
        }
        auto target = resolve_single_target(action.target, collision_event);
        if (!target) return std::unexpected(std::move(target.error()));
        auto& record = **target;
        if (!record.pooled || record.pool_index != action.pool_index) {
            return std::unexpected(pool_state_error("Pool release target is not owned by the requested pool"));
        }
        auto& state = pool_states[action.pool_index];
        if (!pool_storage_valid(action.pool_index, state) || record.pool_slot_index >= state.acquired.size()) {
            return std::unexpected(pool_state_error("Pool release target metadata is inconsistent"));
        }
        if (state.acquired[record.pool_slot_index] == 0U) {
            ++metrics.pool_release_misses;
            if (action.has_result_state) set_state_value(action.result_state_index, 0);
            return {};
        }
        if (auto released = release_pool_slot(
                action.pool_index, record.pool_slot_index, metrics, false);
            !released) {
            return released;
        }
        if (action.has_result_state) set_state_value(action.result_state_index, 1);
        return {};
    }

    Result<void> expire_pool_lifetimes(GameRuntimeFrameMetrics& metrics) {
        for (std::uint32_t pool_index = 0U; pool_index < pool_states.size(); ++pool_index) {
            auto& state = pool_states[pool_index];
            if (!pool_storage_valid(pool_index, state)) {
                return std::unexpected(pool_state_error("Pool lifetime scan found invalid runtime storage"));
            }
            auto slot = state.active_head;
            std::uint32_t visited = 0U;
            while (slot != PoolRuntimeState::invalid_slot) {
                if (slot >= state.acquired.size() || state.acquired[slot] == 0U ||
                    visited >= state.acquired.size()) {
                    return std::unexpected(pool_state_error("Pool lifetime scan found a corrupt active list"));
                }
                const auto next = state.active_next[slot];
                ++metrics.pool_lifetime_checks;
                if (state.expiration_ticks[slot] != PoolRuntimeState::no_expiration &&
                    state.expiration_ticks[slot] <= scene_tick) {
                    if (auto released = release_pool_slot(pool_index, slot, metrics, true); !released) {
                        return released;
                    }
                }
                slot = next;
                ++visited;
            }
        }
        return {};
    }

    [[nodiscard]] bool rule_conditions_match(
        const GameRulePlan& rule,
        GameRuntimeFrameMetrics& metrics) const noexcept {
        for (const auto& condition : rule.conditions) {
            ++metrics.condition_evaluations;
            const std::int64_t left = condition.kind == GameRuleConditionKind::int_state
                                          ? states[condition.state_index]
                                          : group_active_count(condition.spawn_group_index);
            if (!compare_integer(left, condition.comparison, condition.value)) return false;
        }
        return true;
    }

    Result<void> play_sound(const std::uint32_t asset_index) {
#if defined(AI2D_ENABLE_GPU)
        if (audio_available) {
            if (auto played = audio.play(asset_index); !played) warn_once(std::move(played.error()));
        }
#else
        (void)asset_index;
#endif
        return {};
    }

    Result<void> relocate_to_free_cell(
        const std::uint32_t rule_index,
        const GameRuleActionPlan& action,
        const CollisionEvent2D* const collision_event,
        GameRuntimeFrameMetrics& metrics) {
        const auto& grid = plan.scenes[active_scene].grids[action.grid_index];
        auto& occupied = grid_occupancy[action.grid_index];
        std::fill(occupied.begin(), occupied.end(), std::uint8_t{0U});
        constexpr float alignment_epsilon = 0.0001F;
        for (const auto occupied_group : action.occupancy_group_indices) {
            const auto range = group_ranges[occupied_group];
            const GameSystemPlan* pending_follow = nullptr;
            for (const auto& system : plan.scenes[active_scene].systems) {
                if (system.operation == GameOperationId::follow_transform_chain &&
                    system.follower_group_index == occupied_group &&
                    system.motion_system_index < system_states.size() &&
                    system_states[system.motion_system_index].stepped) {
                    pending_follow = &system;
                    break;
                }
            }
            for (std::uint32_t offset = 0U; offset < range.count; ++offset) {
                const auto& record = entities[range.begin + offset];
                const auto* active = world->entity_state(record.entity);
                const auto* transform = world->transform(record.entity);
                if (active == nullptr || !active->active || transform == nullptr) continue;
                Vec2 occupied_position = transform->position;
                if (pending_follow != nullptr) {
                    const auto predecessor_entity = offset == 0U
                                                        ? entities[group_ranges[pending_follow->leader_group_index].begin].entity
                                                        : entities[range.begin + offset - 1U].entity;
                    const auto* predecessor = world->transform(predecessor_entity);
                    if (predecessor == nullptr) {
                        return std::unexpected(runtime_error(
                            DiagnosticCode::internal_error,
                            "Projected transform-chain occupancy is missing Transform2D"));
                    }
                    occupied_position = predecessor->previous_position;
                }
                const double column_value =
                    (static_cast<double>(occupied_position.x) - grid.first_cell_center.x) / grid.cell_size.x;
                const double row_value =
                    (static_cast<double>(occupied_position.y) - grid.first_cell_center.y) / grid.cell_size.y;
                if (!std::isfinite(column_value) || !std::isfinite(row_value) ||
                    column_value < -alignment_epsilon || row_value < -alignment_epsilon ||
                    column_value > static_cast<double>(grid.columns - 1U) + alignment_epsilon ||
                    row_value > static_cast<double>(grid.rows - 1U) + alignment_epsilon) {
                    return std::unexpected(runtime_error(
                        DiagnosticCode::internal_error, "Active occupancy entity is not aligned to its logical grid"));
                }
                const double rounded_column = std::round(column_value);
                const double rounded_row = std::round(row_value);
                if (std::abs(column_value - rounded_column) > alignment_epsilon ||
                    std::abs(row_value - rounded_row) > alignment_epsilon) {
                    return std::unexpected(runtime_error(
                        DiagnosticCode::internal_error, "Active occupancy entity is not aligned to its logical grid"));
                }
                const auto column = static_cast<std::uint32_t>(rounded_column);
                const auto row = static_cast<std::uint32_t>(rounded_row);
                occupied[static_cast<std::size_t>(row) * grid.columns + column] = 1U;
            }
        }
        auto& invocation = rule_invocations[rule_index];
        const auto random = mix_u64(plan.seed ^ rule_random_keys[rule_index] ^ invocation++);
        const auto cell_count = static_cast<std::uint64_t>(occupied.size());
        const auto start = cell_count == 0U ? 0U : random % cell_count;
        std::uint64_t selected = cell_count;
        for (std::uint64_t scanned = 0U; scanned < cell_count; ++scanned) {
            const auto candidate = (start + scanned) % cell_count;
            ++metrics.relocation_cells_scanned;
            if (occupied[static_cast<std::size_t>(candidate)] == 0U) {
                selected = candidate;
                break;
            }
        }
        ++metrics.relocations;
        if (selected == cell_count) {
            if (auto applied = for_each_target(action.target, collision_event, [&](EntityRecord& record) -> Result<void> {
                    set_entity_active(record, false, &metrics);
                    return {};
                });
                !applied) {
                return applied;
            }
            if (action.has_result_state) set_state_value(action.result_state_index, 0);
            return {};
        }
        const auto column = static_cast<std::uint32_t>(selected % grid.columns);
        const auto row = static_cast<std::uint32_t>(selected / grid.columns);
        const double position_x = static_cast<double>(grid.first_cell_center.x) +
                                  static_cast<double>(column) * grid.cell_size.x;
        const double position_y = static_cast<double>(grid.first_cell_center.y) +
                                  static_cast<double>(row) * grid.cell_size.y;
        if (!representable_float(position_x) || !representable_float(position_y)) {
            return std::unexpected(runtime_error(
                DiagnosticCode::runtime_numeric_state_invalid,
                "Relocation cell position exceeds the supported numeric range"));
        }
        const Vec2 position{static_cast<float>(position_x), static_cast<float>(position_y)};
        if (auto applied = for_each_target(action.target, collision_event, [&](EntityRecord& record) -> Result<void> {
                auto* transform = world->transform(record.entity);
                if (transform == nullptr) {
                    return std::unexpected(runtime_error(
                        DiagnosticCode::internal_error, "Relocation target is missing Transform2D"));
                }
                transform->position = position;
                transform->previous_position = position;
                set_entity_active(record, true, &metrics);
                return {};
            });
            !applied) {
            return applied;
        }
        if (action.has_result_state) set_state_value(action.result_state_index, 1);
        return {};
    }

    Result<void> apply_rule_action(
        const std::uint32_t rule_index,
        const GameRuleActionPlan& action,
        const CollisionEvent2D* const collision_event,
        GameRuntimeFrameMetrics& metrics) {
        switch (action.kind) {
        case GameRuleActionKind::set_int_state:
            set_state_value(action.state_index, action.value);
            return {};
        case GameRuleActionKind::add_int_state:
            set_state_value(action.state_index, static_cast<std::int64_t>(states[action.state_index]) + action.value);
            return {};
        case GameRuleActionKind::activate:
        case GameRuleActionKind::deactivate:
            return for_each_target(action.target, collision_event, [&](EntityRecord& record) -> Result<void> {
                set_entity_active(record, action.kind == GameRuleActionKind::activate, &metrics);
                return {};
            });
        case GameRuleActionKind::set_group_active_count: {
            const auto range = group_ranges[action.spawn_group_index];
            const auto requested = action.count_from_state
                                       ? std::clamp<std::int32_t>(states[action.count_state_index], 0, static_cast<std::int32_t>(range.count))
                                       : static_cast<std::int32_t>(std::min(action.count_constant, range.count));
            const auto requested_count = static_cast<std::uint32_t>(requested);
            const auto& scene = plan.scenes[active_scene];
            for (const auto& system : scene.systems) {
                if (system.operation != GameOperationId::follow_transform_chain ||
                    system.follower_group_index != action.spawn_group_index ||
                    system.motion_system_index >= system_states.size() ||
                    !system_states[system.motion_system_index].stepped) {
                    continue;
                }
                for (std::uint32_t offset = 0U; offset < requested_count; ++offset) {
                    const auto& record = entities[range.begin + offset];
                    const auto* active = world->entity_state(record.entity);
                    if (active == nullptr) {
                        return std::unexpected(runtime_error(
                            DiagnosticCode::internal_error,
                            "Activated transform-chain entity is missing active state"));
                    }
                    if (active->active) continue;
                    const auto predecessor_entity = offset == 0U
                                                        ? entities[group_ranges[system.leader_group_index].begin].entity
                                                        : entities[range.begin + offset - 1U].entity;
                    const auto* predecessor = world->transform(predecessor_entity);
                    auto* follower = world->transform(record.entity);
                    if (predecessor == nullptr || follower == nullptr) {
                        return std::unexpected(runtime_error(
                            DiagnosticCode::internal_error,
                            "Activated transform-chain entity is missing Transform2D"));
                    }
                    follower->position = predecessor->previous_position;
                    follower->previous_position = predecessor->previous_position;
                    follower->rotation = predecessor->rotation;
                    follower->scale = predecessor->scale;
                }
            }
            for (std::uint32_t offset = 0U; offset < range.count; ++offset) {
                set_entity_active(entities[range.begin + offset], offset < requested_count, &metrics);
            }
            return {};
        }
        case GameRuleActionKind::set_velocity:
            return for_each_target(action.target, collision_event, [&](EntityRecord& record) -> Result<void> {
                auto* velocity = world->velocity(record.entity);
                if (velocity == nullptr) {
                    return std::unexpected(runtime_error(
                        DiagnosticCode::internal_error, "Velocity action target is missing Velocity2D"));
                }
                velocity->linear = action.velocity;
                return {};
            });
        case GameRuleActionKind::queue_grid_direction: {
            auto& state = system_states[action.system_index];
            const auto& system = plan.scenes[active_scene].systems[action.system_index];
            if (system.prevent_reverse && directions_are_opposite(state.current_direction, action.direction)) {
                ++metrics.rejected_direction_changes;
            } else {
                state.queued_direction = action.direction;
            }
            return {};
        }
        case GameRuleActionKind::reset_group: return reset_spawn_group(action.spawn_group_index, &metrics);
        case GameRuleActionKind::play_sound: return play_sound(action.asset_index);
        case GameRuleActionKind::relocate_to_free_cell:
            return relocate_to_free_cell(rule_index, action, collision_event, metrics);
        case GameRuleActionKind::spawn_from_pool:
            return spawn_from_pool(action, collision_event, metrics);
        case GameRuleActionKind::release_to_pool:
            return release_to_pool(action, collision_event, metrics);
        case GameRuleActionKind::reset_pool:
            return reset_pool(action.pool_index, &metrics);
        }
        return {};
    }

    Result<void> execute_rule(
        const std::uint32_t rule_index,
        const CollisionEvent2D* const collision_event,
        GameRuntimeFrameMetrics& metrics) {
        const auto& rule = plan.scenes[active_scene].rules[rule_index];
        if (!rule_conditions_match(rule, metrics)) return {};
        ++metrics.rule_executions;
        for (const auto& action : rule.actions) {
            ++metrics.action_executions;
            if (auto applied = apply_rule_action(rule_index, action, collision_event, metrics); !applied) return applied;
        }
        return {};
    }

    Result<void> execute_scene_enter_rules(GameRuntimeFrameMetrics* const metrics) {
        GameRuntimeFrameMetrics ignored{};
        auto& destination = metrics == nullptr ? ignored : *metrics;
        for (const auto rule_index : scene_enter_rules) {
            if (auto executed = execute_rule(rule_index, nullptr, destination); !executed) return executed;
        }
        return {};
    }

    Result<void> execute_pre_tick_rules(GameRuntimeFrameMetrics& metrics) {
        const auto& rules = plan.scenes[active_scene].rules;
        for (const auto rule_index : pre_tick_rules) {
            const auto& event = rules[rule_index].event;
            bool fired = false;
            if (event.kind == GameRuleEventKind::action_pressed) {
                fired = tick_actions[event.action_index].pressed;
            } else if (event.kind == GameRuleEventKind::action_released) {
                fired = tick_actions[event.action_index].released;
            } else if (event.kind == GameRuleEventKind::fixed_interval) {
                fired = ((scene_tick + 1U) % event.interval_ticks) == 0U;
            }
            if (fired) {
                if (auto executed = execute_rule(rule_index, nullptr, metrics); !executed) return executed;
            }
        }
        return {};
    }

    Result<void> apply_collision_events(
        const std::span<const CollisionEvent2D> events,
        GameRuntimeFrameMetrics& metrics) {
        if (collision_event_epoch_snapshot.size() != entity_lifecycle_epochs.size()) {
            return std::unexpected(runtime_error(
                DiagnosticCode::runtime_contact_state_invalid,
                "Collision event lifecycle storage does not match the active scene"));
        }
        std::copy(
            entity_lifecycle_epochs.begin(), entity_lifecycle_epochs.end(),
            collision_event_epoch_snapshot.begin());
        const auto endpoint_index = [&](const EntityId entity) -> std::optional<std::size_t> {
            if (entity.index < entities.size() && entities[entity.index].entity == entity) {
                return static_cast<std::size_t>(entity.index);
            }
            const auto found = std::find_if(
                entities.begin(), entities.end(), [&](const EntityRecord& record) {
                    return record.entity == entity;
                });
            if (found == entities.end()) return std::nullopt;
            return static_cast<std::size_t>(std::distance(entities.begin(), found));
        };
        for (const auto& event : events) {
            if (event.rule_index >= plan.scenes[active_scene].collision_rules.size()) {
                return std::unexpected(runtime_error(
                    DiagnosticCode::internal_error, "Collision event references an invalid rule"));
            }
            if (plan.schema_version == GameSchemaVersion::v0_5) {
                const auto a_index = endpoint_index(event.entity_a);
                const auto b_index = endpoint_index(event.entity_b);
                if (!a_index || !b_index ||
                    entity_lifecycle_epochs[*a_index] != collision_event_epoch_snapshot[*a_index] ||
                    entity_lifecycle_epochs[*b_index] != collision_event_epoch_snapshot[*b_index]) {
                    ++metrics.stale_contact_events;
                    continue;
                }
            }
            const auto& rule = plan.scenes[active_scene].collision_rules[event.rule_index];
            for (const auto& reaction : rule.reactions) {
                if (reaction.kind == GameReactionKind::add_int_state) {
                    set_state_value(
                        reaction.state_index,
                        static_cast<std::int64_t>(states[reaction.state_index]) + reaction.value);
                } else if (reaction.kind == GameReactionKind::reset_group) {
                    if (auto reset = reset_legacy_collider_group(reaction.group, &metrics); !reset) return reset;
                } else if (reaction.kind == GameReactionKind::play_sound) {
                    if (auto played = play_sound(reaction.asset_index); !played) return played;
                }
            }
            const auto expected_event = event.phase == CollisionEventPhase2D::contact_begin
                                            ? GameRuleEventKind::contact_begin
                                        : event.phase == CollisionEventPhase2D::contact_end
                                            ? GameRuleEventKind::contact_end
                                            : GameRuleEventKind::collision;
            for (const auto event_rule_index : collision_rule_dispatch[event.rule_index]) {
                if (plan.scenes[active_scene].rules[event_rule_index].event.kind != expected_event) continue;
                if (auto executed = execute_rule(event_rule_index, &event, metrics); !executed) return executed;
            }
        }
        return {};
    }

    Result<CollisionMetrics2D> fixed_tick(
        const float delta_seconds,
        GameRuntimeFrameMetrics& frame_metrics) {
        if (auto expired = expire_pool_lifetimes(frame_metrics); !expired) {
            return std::unexpected(std::move(expired.error()));
        }
        {
            auto transforms = world->query<Transform2D>();
            for (auto item : transforms) item.component.previous_position = item.component.position;
            world->record_query(transforms.candidate_count(), transforms.candidate_count());
        }
        for (auto& state : system_states) state.stepped = false;
        if (auto rules = execute_pre_tick_rules(frame_metrics); !rules) {
            return std::unexpected(std::move(rules.error()));
        }
        CollisionMetrics2D collision_metrics{};
        const auto& scene = plan.scenes[active_scene];
        for (std::size_t system_index = 0U; system_index < scene.systems.size(); ++system_index) {
            const auto& system = scene.systems[system_index];
            if (system.operation == GameOperationId::axis_control) {
                const float direction = (tick_actions[system.positive_action].down ? 1.0F : 0.0F) -
                                        (tick_actions[system.negative_action].down ? 1.0F : 0.0F);
                for (const auto& record : entities) {
                    if (!record.has_collider || record.collider_group != system.group) continue;
                    auto* transform = world->transform(record.entity);
                    auto* velocity = world->velocity(record.entity);
                    auto* state = world->entity_state(record.entity);
                    if (transform == nullptr || velocity == nullptr || (state != nullptr && !state->active)) continue;
                    velocity->linear.x = direction * system.speed;
                    transform->position.x = std::clamp(
                        transform->position.x + velocity->linear.x * delta_seconds, system.minimum, system.maximum);
                }
                world->record_system_invocation();
            } else if (system.operation == GameOperationId::set_velocity_on_press) {
                if (tick_actions[system.action].pressed) {
                    for (const auto& record : entities) {
                        if (!record.has_collider || record.collider_group != system.group) continue;
                        auto* velocity = world->velocity(record.entity);
                        auto* state = world->entity_state(record.entity);
                        if (velocity != nullptr && (state == nullptr || state->active)) velocity->linear = system.velocity;
                    }
                }
                world->record_system_invocation();
            } else if (system.operation == GameOperationId::simulate_collisions) {
                auto simulated = collision->simulate(*world, collision_rules, delta_seconds);
                if (!simulated) return std::unexpected(std::move(simulated.error()));
                collision_metrics = *simulated;
                frame_metrics.active_state_changes += collision_metrics.active_state_changes;
                frame_metrics.trigger_narrowphase_tests += collision_metrics.trigger_narrowphase_tests;
                frame_metrics.contact_begins += collision_metrics.contact_begins;
                frame_metrics.contact_ends += collision_metrics.contact_ends;
                frame_metrics.motion_segments += collision_metrics.motion_segments;
                frame_metrics.peak_contact_pairs = std::max(
                    frame_metrics.peak_contact_pairs, collision_metrics.peak_contact_pairs);
                if (collision_metrics.iteration_limit_hits > 0U) {
                    warn_once(runtime_error(
                        DiagnosticCode::collision_iteration_limit, "Collision solver reached its impact limit"));
                }
                if (auto applied = apply_collision_events(collision->events(), frame_metrics); !applied) {
                    return std::unexpected(std::move(applied.error()));
                }
                frame_metrics.active_contact_pairs =
                    static_cast<std::uint32_t>(collision->active_contact_pairs().size());
            } else if (system.operation == GameOperationId::grid_motion) {
                auto& runtime_state = system_states[system_index];
                const auto range = group_ranges[system.spawn_group_index];
                for (std::uint32_t offset = 0U; offset < range.count; ++offset) {
                    auto* velocity = world->velocity(entities[range.begin + offset].entity);
                    if (velocity != nullptr) velocity->linear = {};
                }
                ++runtime_state.phase;
                if (runtime_state.phase >= system.step_interval_ticks) {
                    runtime_state.phase = 0U;
                    runtime_state.stepped = true;
                    ++frame_metrics.grid_steps;
                    if (runtime_state.queued_direction != GameDirection::none) {
                        if (system.prevent_reverse && directions_are_opposite(
                                                          runtime_state.current_direction,
                                                          runtime_state.queued_direction)) {
                            ++frame_metrics.rejected_direction_changes;
                        } else {
                            runtime_state.current_direction = runtime_state.queued_direction;
                        }
                        runtime_state.queued_direction = GameDirection::none;
                    }
                    const auto unit = direction_vector(runtime_state.current_direction);
                    const auto& grid = scene.grids[system.grid_index];
                    const double velocity_x = static_cast<double>(unit.x) * grid.cell_size.x / delta_seconds;
                    const double velocity_y = static_cast<double>(unit.y) * grid.cell_size.y / delta_seconds;
                    if (!std::isfinite(velocity_x) || !std::isfinite(velocity_y) ||
                        std::abs(velocity_x) > std::numeric_limits<float>::max() ||
                        std::abs(velocity_y) > std::numeric_limits<float>::max()) {
                        return std::unexpected(runtime_error(
                            DiagnosticCode::runtime_numeric_state_invalid,
                            "Grid motion velocity exceeds the supported numeric range"));
                    }
                    const Vec2 velocity_value{
                        static_cast<float>(velocity_x),
                        static_cast<float>(velocity_y),
                    };
                    for (std::uint32_t offset = 0U; offset < range.count; ++offset) {
                        auto& record = entities[range.begin + offset];
                        auto* velocity = world->velocity(record.entity);
                        const auto* active = world->entity_state(record.entity);
                        if (velocity != nullptr && active != nullptr && active->active) velocity->linear = velocity_value;
                    }
                }
                world->record_system_invocation();
            } else if (system.operation == GameOperationId::linear_motion) {
                const auto range = group_ranges[system.spawn_group_index];
                for (std::uint32_t offset = 0U; offset < range.count; ++offset) {
                    auto& record = entities[range.begin + offset];
                    const auto* active = world->entity_state(record.entity);
                    if (active == nullptr || !active->active) continue;
                    auto* transform = world->transform(record.entity);
                    const auto* velocity = world->velocity(record.entity);
                    if (transform == nullptr || velocity == nullptr) {
                        return std::unexpected(runtime_error(
                            DiagnosticCode::internal_error,
                            "linear_motion entity is missing Transform2D or Velocity2D"));
                    }
                    const double x = static_cast<double>(transform->position.x) +
                                     static_cast<double>(velocity->linear.x) * delta_seconds;
                    const double y = static_cast<double>(transform->position.y) +
                                     static_cast<double>(velocity->linear.y) * delta_seconds;
                    if (!representable_float(x) || !representable_float(y)) {
                        return std::unexpected(runtime_error(
                            DiagnosticCode::runtime_numeric_state_invalid,
                            "linear_motion position exceeds the supported numeric range"));
                    }
                    transform->position = {static_cast<float>(x), static_cast<float>(y)};
                    ++frame_metrics.linear_motion_updates;
                }
                world->record_system_invocation();
            } else if (system.operation == GameOperationId::follow_transform_chain) {
                if (system.motion_system_index >= system_states.size() ||
                    !system_states[system.motion_system_index].stepped) {
                    world->record_system_invocation();
                    continue;
                }
                const auto leader_range = group_ranges[system.leader_group_index];
                const auto follower_range = group_ranges[system.follower_group_index];
                for (std::uint32_t reverse = follower_range.count; reverse > 0U; --reverse) {
                    const auto follower_offset = reverse - 1U;
                    const auto predecessor_entity = follower_offset == 0U
                                                        ? entities[leader_range.begin].entity
                                                        : entities[follower_range.begin + follower_offset - 1U].entity;
                    auto* predecessor = world->transform(predecessor_entity);
                    auto* follower = world->transform(entities[follower_range.begin + follower_offset].entity);
                    if (predecessor == nullptr || follower == nullptr) {
                        return std::unexpected(runtime_error(
                            DiagnosticCode::internal_error, "Transform chain entity is missing Transform2D"));
                    }
                    follower->position = predecessor->previous_position;
                    follower->rotation = predecessor->rotation;
                    follower->scale = predecessor->scale;
                    ++frame_metrics.follower_updates;
                }
                world->record_system_invocation();
            }
        }
        ++simulation_tick;
        ++scene_tick;
        return collision_metrics;
    }

    bool transition_matches(const SceneTransitionPlan& transition) const noexcept {
        const auto& condition = transition.condition;
        switch (condition.kind) {
        case TransitionConditionKind::action_pressed: return actions[condition.action].pressed;
        case TransitionConditionKind::int_state_at_most: return states[condition.state_index] <= condition.value;
        case TransitionConditionKind::int_state_at_least: return states[condition.state_index] >= condition.value;
        case TransitionConditionKind::group_inactive: return group_inactive(condition.group);
        }
        return false;
    }

    Result<bool> apply_transition(bool& quit_requested, GameRuntimeFrameMetrics& metrics) {
        for (const auto& transition : plan.transitions) {
            if (transition.from_scene != active_scene || !transition_matches(transition)) continue;
            if (transition.quit) {
                quit_requested = true;
                return false;
            }
            if (transition.to_scene == active_scene && !transition.reset_scene) {
                if (transition.reset_session) reset_states();
                if (auto entered = execute_scene_enter_rules(&metrics); !entered) {
                    return std::unexpected(std::move(entered.error()));
                }
                clock.reset();
                return true;
            }
            if (transition.to_scene != active_scene) {
                if (auto captured = capture_active_scene(); !captured) {
                    return std::unexpected(std::move(captured.error()));
                }
            }
            if (transition.reset_scene) scene_snapshots[transition.to_scene].retained = false;
            if (auto loaded = load_scene(transition.to_scene, !transition.reset_scene); !loaded) {
                return std::unexpected(std::move(loaded.error()));
            }
            if (transition.reset_session) reset_states();
            if (auto entered = execute_scene_enter_rules(&metrics); !entered) {
                return std::unexpected(std::move(entered.error()));
            }
            clock.reset();
            return true;
        }
        return false;
    }

#if defined(AI2D_ENABLE_GPU)
    Vec2 virtual_to_world(const Vec2 point) const noexcept {
        const auto& scene = plan.scenes[active_scene];
        return {
            scene.camera_position.x +
                (point.x / static_cast<float>(plan.window.virtual_width) * 2.0F - 1.0F) * scene.camera_half_extent.x,
            -scene.camera_position.y +
                (point.y / static_cast<float>(plan.window.virtual_height) * 2.0F - 1.0F) * scene.camera_half_extent.y,
        };
    }

    Vec2 virtual_size_to_world(const Vec2 size) const noexcept {
        const auto& scene = plan.scenes[active_scene];
        return {
            size.x / static_cast<float>(plan.window.virtual_width) * scene.camera_half_extent.x * 2.0F,
            size.y / static_cast<float>(plan.window.virtual_height) * scene.camera_half_extent.y * 2.0F,
        };
    }

    void format_text(const std::span<const TextToken> tokens) {
        formatted_text.clear();
        for (const auto& token : tokens) {
            if (!token.is_state) {
                formatted_text.append(token.literal);
            } else {
                char buffer[16]{};
                const auto converted = std::to_chars(
                    std::begin(buffer), std::end(buffer), states[token.state_index]);
                if (converted.ec == std::errc{}) {
                    formatted_text.append(buffer, converted.ptr);
                }
            }
        }
    }

    Result<void> append_text(const UiElementPlan& element, const std::span<const TextToken> tokens) {
        const auto& font = plan.assets[element.font_asset];
        format_text(tokens);
        std::size_t line_count = 1U;
        for (const auto byte : formatted_text) {
            if (byte == '\n') ++line_count;
        }
        const auto line_height = font.line_height * element.text_scale;
        float line_center_y = element.position.y - static_cast<float>(line_count - 1U) * line_height * 0.5F;
        std::size_t line_begin = 0U;
        while (line_begin <= formatted_text.size()) {
            const auto newline = formatted_text.find('\n', line_begin);
            const auto line_end = newline == std::string::npos ? formatted_text.size() : newline;
            float line_width = 0.0F;
            std::size_t measure_offset = line_begin;
            while (measure_offset < line_end) {
                const auto codepoint = decode_utf8(formatted_text, measure_offset);
                if (codepoint == '\r') continue;
                const auto glyph = std::lower_bound(
                    font.glyphs.begin(), font.glyphs.end(), codepoint,
                    [](const GlyphPlan& item, const std::uint32_t value) { return item.codepoint < value; });
                if (glyph != font.glyphs.end() && glyph->codepoint == codepoint) {
                    line_width += glyph->advance * element.text_scale;
                }
            }
            float cursor_x = element.position.x - line_width * 0.5F;
            std::size_t offset = line_begin;
            while (offset < line_end) {
                const auto codepoint = decode_utf8(formatted_text, offset);
                if (codepoint == '\r') continue;
                const auto glyph = std::lower_bound(
                    font.glyphs.begin(), font.glyphs.end(), codepoint,
                    [](const GlyphPlan& item, const std::uint32_t value) { return item.codepoint < value; });
                if (glyph == font.glyphs.end() || glyph->codepoint != codepoint) continue;
                if (extracted.size() >= GamePlan::max_render_submissions) {
                    return std::unexpected(runtime_error(
                        DiagnosticCode::render_upload_capacity_exceeded,
                        "UI text exceeds the validated renderer submission capacity"));
                }
                const Vec2 pixel_size{glyph->size.x * element.text_scale, glyph->size.y * element.text_scale};
                const Vec2 pixel_center{
                    cursor_x + (glyph->bearing.x + glyph->size.x * 0.5F) * element.text_scale,
                    line_center_y - glyph->bearing.y * element.text_scale,
                };
                extracted.push_back({
                    virtual_to_world(pixel_center),
                    virtual_size_to_world(pixel_size),
                    {0.5F, 0.5F},
                    0.0F,
                    {glyph->uv.min.x, glyph->uv.min.y, glyph->uv.max.x, glyph->uv.max.y},
                    element.text_color,
                    textures[element.font_asset],
                    element.layer + 1,
                    true,
                });
                cursor_x += glyph->advance * element.text_scale;
            }
            if (newline == std::string::npos) break;
            line_begin = newline + 1U;
            line_center_y += line_height;
        }
        return {};
    }

    Result<bool> render(const double interpolation_alpha, std::uint32_t& extracted_count) {
        if (options.headless) return false;
        extracted.clear();
        auto sprites = world->query<Transform2D, Sprite2D>();
        for (auto item : sprites) {
            const auto* state = world->entity_state(item.entity);
            if (!item.second.visible || (state != nullptr && !state->active)) continue;
            const auto alpha = static_cast<float>(interpolation_alpha);
            const Vec2 position{
                item.first.previous_position.x + (item.first.position.x - item.first.previous_position.x) * alpha,
                -(item.first.previous_position.y + (item.first.position.y - item.first.previous_position.y) * alpha),
            };
            if (extracted.size() >= GamePlan::max_render_submissions) {
                return std::unexpected(runtime_error(
                    DiagnosticCode::render_upload_capacity_exceeded,
                    "World sprites exceed the validated renderer submission capacity"));
            }
            extracted.push_back({
                position,
                {item.second.size.x * item.first.scale.x, item.second.size.y * item.first.scale.y},
                item.second.pivot,
                -item.first.rotation,
                {item.second.uv.min.x, item.second.uv.min.y, item.second.uv.max.x, item.second.uv.max.y},
                item.second.tint,
                item.second.texture,
                item.second.layer,
                true,
            });
        }
        world->record_query(sprites.candidate_count(), extracted.size());
        for (std::size_t element_index = 0U; element_index < plan.scenes[active_scene].ui.size(); ++element_index) {
            const auto& element = plan.scenes[active_scene].ui[element_index];
            if (element.kind == UiElementKind::panel || element.kind == UiElementKind::button) {
                const auto color = element.kind == UiElementKind::button && point_in_ui(element)
                                       ? element.hover_color
                                       : element.color;
                if (extracted.size() >= GamePlan::max_render_submissions) {
                    return std::unexpected(runtime_error(
                        DiagnosticCode::render_upload_capacity_exceeded,
                        "UI geometry exceeds the validated renderer submission capacity"));
                }
                extracted.push_back({
                    virtual_to_world(element.position),
                    virtual_size_to_world(element.size),
                    {0.5F, 0.5F},
                    0.0F,
                    {},
                    color,
                    white_texture,
                    element.layer,
                    true,
                });
            }
            if (element.kind == UiElementKind::text || element.kind == UiElementKind::button) {
                if (auto appended = append_text(element, ui_text_tokens[active_scene][element_index]); !appended) {
                    return std::unexpected(std::move(appended.error()));
                }
            }
        }
        RenderFrame2D frame{};
        frame.camera = {
            {plan.scenes[active_scene].camera_position.x, -plan.scenes[active_scene].camera_position.y},
            plan.scenes[active_scene].camera_half_extent,
        };
        frame.sprites = extracted;
        auto rendered = renderer.render(frame);
        if (!rendered) return std::unexpected(std::move(rendered.error()));
        extracted_count = static_cast<std::uint32_t>(extracted.size());
        return rendered->gpu.presented;
    }
#endif

    Result<GameRuntimeFrameMetrics> execute(
        const InputSnapshot& input,
        const FixedStepAdvance& advance_result,
        const bool actions_prepared = false) {
        if (!ready) {
            return std::unexpected(runtime_error(
                DiagnosticCode::input_invalid, "Game frame requested before initialization"));
        }
        Stopwatch timer{};
        if (!actions_prepared) update_actions(input);
        for (const auto& binding : plan.fps_actions) {
            if (actions[binding.action].pressed) {
                if (auto changed = set_fps(binding.cap, true); !changed) {
                    return std::unexpected(std::move(changed.error()));
                }
            }
        }
        GameRuntimeFrameMetrics metrics{};
        metrics.frame_index = frame_index;
        metrics.fixed_ticks = advance_result.tick_count;
        metrics.interpolation_alpha = advance_result.interpolation_alpha;
        metrics.dropped_seconds = advance_result.dropped_seconds;
        metrics.frame_time_clamped = advance_result.frame_time_clamped;
        metrics.catch_up_limited = advance_result.catch_up_limited;
        metrics.quit_requested = input.quit_requested;
        for (std::uint32_t tick = 0U; tick < advance_result.tick_count; ++tick) {
            for (std::size_t action_index = 0U; action_index < actions.size(); ++action_index) {
                tick_actions[action_index] = {
                    actions[action_index].down,
                    pending_tick_edges[action_index].pressed,
                    pending_tick_edges[action_index].released,
                };
            }
            if (std::any_of(tick_actions.begin(), tick_actions.end(), [](const ActionState& state) {
                    return state.pressed || state.released;
                })) {
                ++metrics.input_edge_ticks;
            }
            auto collision_result = fixed_tick(static_cast<float>(advance_result.step_seconds), metrics);
            if (!collision_result) return std::unexpected(std::move(collision_result.error()));
            for (auto& pending : pending_tick_edges) {
                pending.pressed = false;
                pending.released = false;
            }
            metrics.collision.active_colliders = std::max(
                metrics.collision.active_colliders, collision_result->active_colliders);
            metrics.collision.active_state_changes += collision_result->active_state_changes;
            metrics.collision.grid_references += collision_result->grid_references;
            metrics.collision.candidate_pairs += collision_result->candidate_pairs;
            metrics.collision.narrowphase_tests += collision_result->narrowphase_tests;
            metrics.collision.contacts += collision_result->contacts;
            metrics.collision.trigger_narrowphase_tests += collision_result->trigger_narrowphase_tests;
            metrics.collision.contact_begins += collision_result->contact_begins;
            metrics.collision.contact_ends += collision_result->contact_ends;
            metrics.collision.active_contact_pairs = collision_result->active_contact_pairs;
            metrics.collision.peak_contact_pairs = std::max(
                metrics.collision.peak_contact_pairs, collision_result->peak_contact_pairs);
            metrics.collision.motion_segments += collision_result->motion_segments;
            metrics.collision.toi_iterations += collision_result->toi_iterations;
            metrics.collision.iteration_limit_hits += collision_result->iteration_limit_hits;
            metrics.collision.grid_reference_capacity = collision_result->grid_reference_capacity;
            metrics.collision.candidate_capacity = collision_result->candidate_capacity;
            metrics.collision.contact_capacity = collision_result->contact_capacity;
        }
        auto transitioned = apply_transition(metrics.quit_requested, metrics);
        if (!transitioned) return std::unexpected(std::move(transitioned.error()));
        metrics.scene_changed = *transitioned;
        if (*transitioned) {
            synchronize_interpolation_history();
            metrics.interpolation_alpha = 0.0;
            for (auto& pending : pending_tick_edges) pending = {};
        }
#if defined(AI2D_ENABLE_GPU)
        auto rendered = render(metrics.interpolation_alpha, metrics.extracted_sprites);
        if (!rendered) return std::unexpected(std::move(rendered.error()));
        metrics.presented = *rendered;
#endif
        metrics.scene_index = active_scene;
        metrics.simulation_tick = simulation_tick;
        for (std::size_t index = 0U; index < states.size(); ++index) {
            metrics.state_checksum += static_cast<double>(states[index]) * static_cast<double>(index + 1U);
        }
        for (std::size_t index = 0U; index < entities.size(); ++index) {
            const auto& record = entities[index];
            const auto* transform = world->transform(record.entity);
            if (transform != nullptr) {
                const double weight = static_cast<double>(index + 1U);
                metrics.scene_state_checksum +=
                    weight * (static_cast<double>(transform->position.x) * 3.0 +
                              static_cast<double>(transform->position.y) * 5.0 +
                              static_cast<double>(transform->rotation) * 7.0 +
                              static_cast<double>(transform->previous_position.x) * 23.0 +
                              static_cast<double>(transform->previous_position.y) * 29.0);
            }
            if (const auto* velocity = world->velocity(record.entity); velocity != nullptr) {
                const double weight = static_cast<double>(index + 1U);
                metrics.scene_state_checksum +=
                    weight * (static_cast<double>(velocity->linear.x) * 11.0 +
                              static_cast<double>(velocity->linear.y) * 13.0 +
                              static_cast<double>(velocity->angular) * 17.0);
            }
            if (const auto* state = world->entity_state(record.entity); state != nullptr && state->active) {
                metrics.scene_state_checksum += static_cast<double>(index + 1U) * 19.0;
            }
        }
        for (std::size_t pool_index = 0U; pool_index < pool_states.size(); ++pool_index) {
            const auto& pool_state = pool_states[pool_index];
            const double pool_weight = static_cast<double>(pool_index + 1U);
            metrics.scene_state_checksum += pool_weight *
                                            (static_cast<double>(pool_state.active_count) * 31.0 +
                                             static_cast<double>(pool_state.free_count) * 37.0);
            const auto capacity = static_cast<std::uint32_t>(pool_state.free_ring.size());
            for (std::uint32_t order = 0U; order < pool_state.free_count; ++order) {
                const auto slot = pool_state.free_ring[(pool_state.free_head + order) % capacity];
                metrics.scene_state_checksum += pool_weight * static_cast<double>(order + 1U) *
                                                static_cast<double>(slot + 1U) * 41.0;
            }
            for (std::uint32_t slot = 0U; slot < pool_state.acquired.size(); ++slot) {
                if (pool_state.acquired[slot] == 0U) continue;
                metrics.scene_state_checksum += pool_weight * static_cast<double>(slot + 1U) * 43.0;
                if (pool_state.expiration_ticks[slot] != PoolRuntimeState::no_expiration) {
                    metrics.scene_state_checksum +=
                        pool_weight * static_cast<double>(pool_state.expiration_ticks[slot] & 0xFFFF'FFFFULL) * 47.0;
                }
            }
        }
        metrics.active_pooled_entities = active_pooled_entities;
        metrics.peak_active_pooled_entities = peak_active_pooled_entities;
        metrics.frame_cpu_ms = timer.elapsed_milliseconds();
        ++frame_index;
        return metrics;
    }

    Result<GameRuntimeFrameMetrics> execute_numeric_actions(
        const std::span<const GameActionInput> input_actions,
        const FixedStepAdvance& advance_result) {
        if (!ready || input_actions.size() != actions.size()) {
            return std::unexpected(runtime_error(
                DiagnosticCode::input_invalid,
                "Numeric action input count must exactly match the compiled game action count"));
        }
        for (std::size_t index = 0U; index < actions.size(); ++index) {
            actions[index] = {input_actions[index].down, input_actions[index].pressed, input_actions[index].released};
            pending_tick_edges[index].pressed = pending_tick_edges[index].pressed || input_actions[index].pressed;
            pending_tick_edges[index].released = pending_tick_edges[index].released || input_actions[index].released;
        }
        return execute({}, advance_result, true);
    }

    Result<GameRuntimeLoadMetrics> initialize(const GamePlan& requested_plan, const GameRuntimeOptions& requested_options) {
        if (ready) {
            return std::unexpected(runtime_error(DiagnosticCode::input_invalid, "Game runtime is already initialized"));
        }
        if (auto validated = validate_game_plan(requested_plan); !validated) {
            return std::unexpected(std::move(validated.error()));
        }
        Stopwatch timer{};
        plan = requested_plan;
        options = requested_options;
        warnings.reserve(32U);
        actions.resize(plan.actions.size());
        tick_actions.resize(plan.actions.size());
        pending_tick_edges.resize(plan.actions.size());
        states.resize(plan.states.size());
        scene_snapshots.resize(plan.scenes.size());
        for (std::size_t scene_index = 0U; scene_index < plan.scenes.size(); ++scene_index) {
            const auto count = plan.scenes[scene_index].total_spawn_count;
            scene_snapshots[scene_index].transforms.resize(count);
            scene_snapshots[scene_index].velocities.resize(count);
            scene_snapshots[scene_index].sprites.resize(count);
            scene_snapshots[scene_index].colliders.resize(count);
            scene_snapshots[scene_index].entity_states.resize(count);
            scene_snapshots[scene_index].system_states.resize(plan.scenes[scene_index].systems.size());
            scene_snapshots[scene_index].rule_invocations.resize(plan.scenes[scene_index].rules.size());
            scene_snapshots[scene_index].pool_states.resize(plan.scenes[scene_index].pools.size());
            scene_snapshots[scene_index].contacts.reserve(plan.scenes[scene_index].max_contact_pairs);
            for (std::size_t pool_index = 0U; pool_index < plan.scenes[scene_index].pools.size(); ++pool_index) {
                const auto group_index = plan.scenes[scene_index].pools[pool_index].spawn_group_index;
                const auto& group = plan.scenes[scene_index].spawn_groups[group_index];
                initialize_pool_storage(
                    scene_snapshots[scene_index].pool_states[pool_index], group.count, group.active_count);
            }
        }
        state_indices.reserve(plan.states.size());
        for (std::size_t index = 0U; index < plan.states.size(); ++index) {
            state_indices.emplace(std::string{plan.symbol(plan.states[index].symbol)}, static_cast<std::uint32_t>(index));
        }
        ui_text_tokens.resize(plan.scenes.size());
        std::size_t maximum_formatted_bytes = 0U;
        for (std::size_t scene_index = 0U; scene_index < plan.scenes.size(); ++scene_index) {
            const auto& ui = plan.scenes[scene_index].ui;
            ui_text_tokens[scene_index].resize(ui.size());
            for (std::size_t element_index = 0U; element_index < ui.size(); ++element_index) {
                const auto& element = ui[element_index];
                if (element.kind == UiElementKind::panel) continue;
                const auto source = plan.symbol(element.text);
                auto& tokens = ui_text_tokens[scene_index][element_index];
                std::size_t offset = 0U;
                std::size_t formatted_bytes = 0U;
                while (offset < source.size()) {
                    const auto open = source.find('{', offset);
                    if (open == std::string_view::npos) {
                        tokens.push_back({source.substr(offset), 0U, false});
                        formatted_bytes += source.size() - offset;
                        break;
                    }
                    if (open > offset) {
                        tokens.push_back({source.substr(offset, open - offset), 0U, false});
                        formatted_bytes += open - offset;
                    }
                    const auto close = source.find('}', open + 1U);
                    if (close == std::string_view::npos) {
                        return std::unexpected(runtime_error(
                            DiagnosticCode::internal_error,
                            "Validated UI placeholder could not be compiled"));
                    }
                    const auto found = state_indices.find(source.substr(open + 1U, close - open - 1U));
                    if (found == state_indices.end()) {
                        return std::unexpected(runtime_error(
                            DiagnosticCode::internal_error,
                            "Validated UI placeholder references a missing state"));
                    }
                    tokens.push_back({{}, found->second, true});
                    const auto& state = plan.states[found->second];
                    formatted_bytes += std::max(decimal_width(state.minimum), decimal_width(state.maximum));
                    offset = close + 1U;
                }
                maximum_formatted_bytes = std::max(maximum_formatted_bytes, formatted_bytes);
            }
        }
        reset_states();
        if (auto initialized = clock.initialize(); !initialized) {
            return std::unexpected(std::move(initialized.error()));
        }
        requested_fps = plan.window.default_fps;
        master_volume = 1.0F;
#if defined(AI2D_ENABLE_GPU)
        if (!options.headless && options.load_saved_settings) {
            auto saved = load_game_settings(plan.symbol(plan.organization), plan.symbol(plan.application));
            if (!saved) {
                warn_once(std::move(saved.error()));
            } else if (saved->has_value()) {
                requested_fps = saved->value().fps_cap;
                master_volume = saved->value().master_volume;
            }
        }
#endif
        if (options.has_fps_override) requested_fps = options.fps_override;
        effective_fps = requested_fps;
        std::uint32_t loaded_textures = 0U;
        std::uint32_t loaded_audio = 0U;
#if defined(AI2D_ENABLE_GPU)
        textures.resize(plan.assets.size());
        if (!options.headless) {
            std::uint64_t resident_asset_bytes = 0U;
            for (const auto& asset : plan.assets) {
                const bool image_asset = asset.kind == GameAssetKind::png || asset.kind == GameAssetKind::font;
                auto inspected = image_asset ? preflight_png_asset(asset.path) : preflight_wav_asset(asset.path);
                if (!inspected) return std::unexpected(std::move(inspected.error()));
                if (!image_asset && !options.enable_audio) continue;
                if (inspected->decoded_bytes > maximum_resident_asset_bytes - resident_asset_bytes) {
                    auto diagnostic = runtime_error(
                        DiagnosticCode::asset_decode_failed,
                        "Aggregate decoded game assets exceed the resident-memory limit");
                    diagnostic.context.push_back({"path", asset.path.string()});
                    diagnostic.context.push_back({"limit_bytes", std::to_string(maximum_resident_asset_bytes)});
                    return std::unexpected(std::move(diagnostic));
                }
                resident_asset_bytes += inspected->decoded_bytes;
            }
            const auto width = options.width == 0U ? plan.window.width : options.width;
            const auto height = options.height == 0U ? plan.window.height : options.height;
            if (!options.offscreen) {
                WindowOptions window_options{};
                const auto title = plan.symbol(plan.window.title);
                window_options.title = title.data();
                window_options.width = width;
                window_options.height = height;
                window_options.hidden = options.hidden;
                if (auto opened = window.open(window_options); !opened) {
                    return std::unexpected(std::move(opened.error()));
                }
            }
            RendererOptions renderer_options{};
            renderer_options.native_window = window.native_handle();
            renderer_options.width = width;
            renderer_options.height = height;
            renderer_options.max_sprites = GamePlan::max_render_submissions;
            renderer_options.max_textures = static_cast<std::uint32_t>(plan.assets.size() + 1U);
            renderer_options.enable_validation = options.enable_validation;
            renderer_options.enable_synchronization_validation = options.enable_synchronization_validation;
            renderer_options.require_present = !options.offscreen;
            renderer_options.requested_fps_cap = requested_fps;
            if (auto initialized = renderer.initialize(renderer_options); !initialized) {
                return std::unexpected(std::move(initialized.error()));
            }
            constexpr std::uint8_t white_pixel[4]{255U, 255U, 255U, 255U};
            auto white = renderer.create_texture_rgba(1U, 1U, white_pixel);
            if (!white) return std::unexpected(std::move(white.error()));
            white_texture = *white;
            ++loaded_textures;
            extracted.reserve(GamePlan::max_render_submissions);
            formatted_text.reserve(maximum_formatted_bytes);
            if (options.enable_audio) {
                auto initialized_audio = audio.initialize(static_cast<std::uint32_t>(plan.assets.size()));
                if (initialized_audio) {
                    audio_available = true;
                    if (auto volume = audio.set_master_volume(master_volume); !volume) {
                        return std::unexpected(std::move(volume.error()));
                    }
                } else {
                    warn_once(std::move(initialized_audio.error()));
                }
            }
            for (std::size_t index = 0U; index < plan.assets.size(); ++index) {
                const auto& asset = plan.assets[index];
                if (asset.kind == GameAssetKind::png || asset.kind == GameAssetKind::font) {
                    auto image = load_png_rgba8(asset.path);
                    if (!image) return std::unexpected(std::move(image.error()));
                    auto texture = renderer.create_texture_rgba(image->width, image->height, image->pixels);
                    if (!texture) return std::unexpected(std::move(texture.error()));
                    textures[index] = *texture;
                    ++loaded_textures;
                } else if (audio_available) {
                    auto wave = load_wav_f32_stereo(asset.path);
                    if (!wave) return std::unexpected(std::move(wave.error()));
                    if (auto registered = audio.register_clip(static_cast<std::uint32_t>(index), std::move(*wave));
                        !registered) {
                        return std::unexpected(std::move(registered.error()));
                    }
                    ++loaded_audio;
                }
            }
        } else {
            for (std::size_t index = 0U; index < textures.size(); ++index) {
                textures[index] = {static_cast<std::uint32_t>(index), 1U};
            }
        }
#else
        if (!options.headless) {
            return std::unexpected(runtime_error(
                DiagnosticCode::command_unavailable, "This build requires headless mode for GameRuntime"));
        }
#endif
        if (auto loaded = load_scene(plan.start_scene); !loaded) {
            return std::unexpected(std::move(loaded.error()));
        }
        if (auto entered = execute_scene_enter_rules(nullptr); !entered) {
            return std::unexpected(std::move(entered.error()));
        }
        ready = true;
        last_frame_time = std::chrono::steady_clock::now();
        return GameRuntimeLoadMetrics{
            timer.elapsed_milliseconds(),
            static_cast<std::uint32_t>(plan.assets.size()),
            loaded_textures,
            loaded_audio,
            plan.scenes[active_scene].total_spawn_count,
            audio_available,
            requested_fps,
            effective_fps,
        };
    }
};

GameRuntime::GameRuntime() : impl_(std::make_unique<Impl>()) {}
GameRuntime::~GameRuntime() = default;
GameRuntime::GameRuntime(GameRuntime&&) noexcept = default;
GameRuntime& GameRuntime::operator=(GameRuntime&&) noexcept = default;

Result<GameRuntimeLoadMetrics> GameRuntime::initialize(const GamePlan& plan, const GameRuntimeOptions& options) {
    const bool was_ready = impl_->ready;
    try {
        auto result = impl_->initialize(plan, options);
        if (!result && !was_ready) impl_ = std::make_unique<Impl>();
        return result;
    } catch (const std::exception& exception) {
        if (!was_ready) impl_ = std::make_unique<Impl>();
        auto diagnostic = runtime_error(
            DiagnosticCode::internal_error,
            "Game runtime initialization failed with a standard-library exception");
        diagnostic.context.push_back({"exception", std::string{exception.what()}});
        return std::unexpected(std::move(diagnostic));
    } catch (...) {
        if (!was_ready) impl_ = std::make_unique<Impl>();
        return std::unexpected(runtime_error(
            DiagnosticCode::internal_error,
            "Game runtime initialization failed with an unknown exception"));
    }
}

Result<GameRuntimeFrameMetrics> GameRuntime::run_frame() {
    if (!impl_->ready || impl_->options.headless) {
        return std::unexpected(runtime_error(
            DiagnosticCode::input_invalid, "run_frame requires an initialized non-headless game runtime"));
    }
    const auto frame_begin = std::chrono::steady_clock::now();
    const auto elapsed = std::chrono::duration<double>(frame_begin - impl_->last_frame_time).count();
    impl_->last_frame_time = frame_begin;
    InputSnapshot input{};
#if defined(AI2D_ENABLE_GPU)
    if (!impl_->options.offscreen) {
        auto polled = impl_->window.poll_input();
        if (!polled) return std::unexpected(std::move(polled.error()));
        input = *polled;
        if (input.resized && input.drawable_extent.width > 0U && input.drawable_extent.height > 0U) {
            if (auto resized = impl_->renderer.resize(input.drawable_extent.width, input.drawable_extent.height); !resized) {
                return std::unexpected(std::move(resized.error()));
            }
        }
    }
#endif
    auto advance_result = impl_->clock.advance(elapsed);
    if (!advance_result) return std::unexpected(std::move(advance_result.error()));
    auto result = impl_->execute(input, *advance_result);
    if (!result) return result;
    const auto fps = frames_per_second(impl_->requested_fps);
    if (fps > 0U) {
        const auto deadline = frame_begin + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                              std::chrono::duration<double>{1.0 / static_cast<double>(fps)});
        std::this_thread::sleep_until(deadline);
    }
    return result;
}

Result<GameRuntimeFrameMetrics> GameRuntime::advance(const InputSnapshot& input, const double elapsed_seconds) {
    if (!impl_->ready) {
        return std::unexpected(runtime_error(DiagnosticCode::input_invalid, "Game runtime is not initialized"));
    }
    auto advance_result = impl_->clock.advance(elapsed_seconds);
    if (!advance_result) return std::unexpected(std::move(advance_result.error()));
    return impl_->execute(input, *advance_result);
}

Result<GameRuntimeFrameMetrics> GameRuntime::run_exact(
    const InputSnapshot& input,
    const std::uint32_t fixed_ticks) {
    if (!impl_->ready) {
        return std::unexpected(runtime_error(DiagnosticCode::input_invalid, "Game runtime is not initialized"));
    }
    auto exact = impl_->clock.exact(fixed_ticks);
    if (!exact) return std::unexpected(std::move(exact.error()));
    return impl_->execute(input, *exact);
}

Result<GameRuntimeFrameMetrics> GameRuntime::run_exact_actions(
    const std::span<const GameActionInput> actions,
    const std::uint32_t fixed_ticks) {
    if (!impl_->ready) {
        return std::unexpected(runtime_error(DiagnosticCode::input_invalid, "Game runtime is not initialized"));
    }
    if (actions.size() != impl_->actions.size()) {
        return std::unexpected(runtime_error(
            DiagnosticCode::input_invalid,
            "Numeric action input count must exactly match the compiled game action count"));
    }
    auto exact = impl_->clock.exact(fixed_ticks);
    if (!exact) return std::unexpected(std::move(exact.error()));
    return impl_->execute_numeric_actions(actions, *exact);
}

Result<void> GameRuntime::wait_idle() {
    if (!impl_->ready) return {};
#if defined(AI2D_ENABLE_GPU)
    if (!impl_->options.headless) return impl_->renderer.wait_idle();
#endif
    return {};
}

Result<std::int32_t> GameRuntime::state_value(const std::string_view state_name) const {
    if (!impl_->ready) {
        return std::unexpected(runtime_error(DiagnosticCode::input_invalid, "Game runtime is not initialized"));
    }
    const auto found = impl_->state_indices.find(state_name);
    if (found == impl_->state_indices.end()) {
        return std::unexpected(runtime_error(DiagnosticCode::input_invalid, "Requested game state does not exist"));
    }
    return impl_->states[found->second];
}

Result<std::int32_t> GameRuntime::state_value(const std::uint32_t state_index) const {
    if (!impl_->ready || state_index >= impl_->states.size()) {
        return std::unexpected(runtime_error(
            DiagnosticCode::input_invalid, "Numeric game state index is invalid"));
    }
    return impl_->states[state_index];
}

Result<std::uint32_t> GameRuntime::group_active_count(const std::uint32_t spawn_group_index) const {
    if (!impl_->ready || spawn_group_index >= impl_->group_ranges.size()) {
        return std::unexpected(runtime_error(
            DiagnosticCode::input_invalid, "Numeric spawn group index is invalid for the active scene"));
    }
    return impl_->group_active_count(spawn_group_index);
}

Result<bool> GameRuntime::entity_active(
    const std::uint32_t spawn_group_index,
    const std::uint32_t item_index) const {
    if (!impl_->ready || spawn_group_index >= impl_->group_ranges.size() ||
        item_index >= impl_->group_ranges[spawn_group_index].count) {
        return std::unexpected(runtime_error(DiagnosticCode::input_invalid, "Numeric entity target is invalid"));
    }
    const auto& range = impl_->group_ranges[spawn_group_index];
    const auto* state = impl_->world->entity_state(impl_->entities[range.begin + item_index].entity);
    if (state == nullptr) {
        return std::unexpected(runtime_error(DiagnosticCode::internal_error, "Entity is missing active state"));
    }
    return state->active;
}

Result<Vec2> GameRuntime::entity_position(
    const std::uint32_t spawn_group_index,
    const std::uint32_t item_index) const {
    if (!impl_->ready || spawn_group_index >= impl_->group_ranges.size() ||
        item_index >= impl_->group_ranges[spawn_group_index].count) {
        return std::unexpected(runtime_error(DiagnosticCode::input_invalid, "Numeric entity target is invalid"));
    }
    const auto& range = impl_->group_ranges[spawn_group_index];
    const auto* transform = impl_->world->transform(impl_->entities[range.begin + item_index].entity);
    if (transform == nullptr) {
        return std::unexpected(runtime_error(DiagnosticCode::internal_error, "Entity is missing Transform2D"));
    }
    return transform->position;
}

Result<Vec2> GameRuntime::entity_velocity(
    const std::uint32_t spawn_group_index,
    const std::uint32_t item_index) const {
    if (!impl_->ready || spawn_group_index >= impl_->group_ranges.size() ||
        item_index >= impl_->group_ranges[spawn_group_index].count) {
        return std::unexpected(runtime_error(DiagnosticCode::input_invalid, "Numeric entity target is invalid"));
    }
    const auto& range = impl_->group_ranges[spawn_group_index];
    const auto* velocity = impl_->world->velocity(impl_->entities[range.begin + item_index].entity);
    if (velocity == nullptr) {
        return std::unexpected(runtime_error(DiagnosticCode::input_invalid, "Entity has no Velocity2D"));
    }
    return velocity->linear;
}

std::uint32_t GameRuntime::current_scene_index() const noexcept {
    return impl_->ready ? impl_->active_scene : std::numeric_limits<std::uint32_t>::max();
}

std::uint64_t GameRuntime::contact_state_checksum() const noexcept {
    return impl_->ready && impl_->collision != nullptr ? impl_->collision->contact_state_checksum() : 0U;
}

std::string_view GameRuntime::current_scene() const noexcept {
    return impl_->ready ? impl_->plan.symbol(impl_->plan.scenes[impl_->active_scene].symbol) : std::string_view{};
}
RenderFpsCap GameRuntime::requested_fps_cap() const noexcept { return impl_->requested_fps; }
RenderFpsCap GameRuntime::effective_fps_cap() const noexcept { return impl_->effective_fps; }
std::string_view GameRuntime::requested_present_mode() const noexcept {
#if defined(AI2D_ENABLE_GPU)
    if (impl_->ready && !impl_->options.headless) {
        return present_mode_name(impl_->renderer.capabilities().requested_present_mode);
    }
#endif
    return "unavailable";
}
std::string_view GameRuntime::effective_present_mode() const noexcept {
#if defined(AI2D_ENABLE_GPU)
    if (impl_->ready && !impl_->options.headless) {
        return present_mode_name(impl_->renderer.capabilities().effective_present_mode);
    }
#endif
    return "unavailable";
}
std::span<const Diagnostic> GameRuntime::diagnostics() const noexcept { return impl_->warnings; }
bool GameRuntime::initialized() const noexcept { return impl_->ready; }

} // namespace ai2d
