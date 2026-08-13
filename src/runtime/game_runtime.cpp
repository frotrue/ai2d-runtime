#include "ai2d/runtime/game_runtime.hpp"

#include "ai2d/foundation/timer.hpp"
#include "ai2d/foundation/preference_path.hpp"
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
#include <array>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iterator>
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

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#endif

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
    switch (key) {
    case GameKey::escape: return InputKey::escape;
    case GameKey::left: return InputKey::left;
    case GameKey::right: return InputKey::right;
    case GameKey::up: return InputKey::up;
    case GameKey::down: return InputKey::down;
    case GameKey::a: return InputKey::a;
    case GameKey::d: return InputKey::d;
    case GameKey::space: return InputKey::space;
    case GameKey::enter: return InputKey::enter;
    case GameKey::tab: return InputKey::tab;
    case GameKey::w: return InputKey::w;
    case GameKey::s: return InputKey::s;
    case GameKey::q: return InputKey::q;
    case GameKey::e: return InputKey::e;
    case GameKey::r: return InputKey::r;
    case GameKey::f: return InputKey::f;
    case GameKey::left_shift: return InputKey::left_shift;
    case GameKey::left_control: return InputKey::left_control;
    }
    return InputKey::escape;
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

std::uint64_t process_identifier() noexcept {
#if defined(_WIN32)
    return static_cast<std::uint64_t>(GetCurrentProcessId());
#else
    return static_cast<std::uint64_t>(getpid());
#endif
}

bool path_is_descendant(
    const std::filesystem::path& root,
    const std::filesystem::path& candidate) {
    std::error_code error{};
    const auto relative = std::filesystem::relative(candidate, root, error);
    return !error && !relative.empty() && !relative.is_absolute() && *relative.begin() != "..";
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
        std::uint32_t initial_animation_index{0U};
        bool has_initial_animation{false};
        bool initial_animation_playing{false};
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

    struct AnimationRuntimeState final {
        std::uint32_t clip_index{0U};
        std::uint32_t frame_index{0U};
        std::uint32_t ticks_remaining{0U};
        std::int8_t direction{1};
        bool has_clip{false};
        bool playing{false};
        bool completion_emitted{false};
    };

    struct ParticleSlot final {
        Vec2 position{};
        Vec2 previous_position{};
        Vec2 velocity{};
        std::uint64_t expiration_tick{0U};
        std::uint64_t acquisition_order{0U};
        bool active{false};
    };

    struct ParticleEmitterRuntimeState final {
        static constexpr std::uint32_t invalid_slot = std::numeric_limits<std::uint32_t>::max();

        std::vector<ParticleSlot> slots{};
        std::vector<std::uint32_t> free_heap{};
        std::vector<std::uint32_t> active_next{};
        std::vector<std::uint32_t> active_previous{};
        std::uint32_t active_head{invalid_slot};
        std::uint32_t active_tail{invalid_slot};
        std::uint64_t next_acquisition_order{0U};
        std::uint64_t emission_count{0U};
        std::uint32_t active_count{0U};
    };

    struct CameraRuntimeState final {
        Vec2 position{};
        Vec2 half_extent{16.0F, 9.0F};
        float zoom{1.0F};
        float shake_amplitude{0.0F};
        std::uint32_t shake_ticks_remaining{0U};
        std::uint64_t shake_invocation{0U};
    };

    enum class SaveRequestKind : std::uint8_t { none, save, load, erase };

    struct PendingSaveRequest final {
        SaveRequestKind kind{SaveRequestKind::none};
        std::uint32_t slot{0U};
        bool has_result_state{false};
        std::uint32_t result_state_index{0U};
    };

    struct MusicRequest final {
        enum class Kind : std::uint8_t { none, play, stop, volume };
        Kind kind{Kind::none};
        std::uint32_t asset_index{0U};
        std::uint32_t fade_ticks{0U};
        float volume{1.0F};
        bool loop{false};
    };

    struct MusicStreamState final {
        std::ifstream file{};
        std::vector<std::uint8_t> source_buffer{};
        std::vector<float> sample_buffer{};
        std::uint64_t data_offset{0U};
        std::uint64_t data_bytes{0U};
        std::uint64_t cursor_bytes{0U};
        std::uint64_t last_underruns{0U};
        std::uint32_t asset_index{0U};
        std::uint16_t format{0U};
        std::uint16_t channels{0U};
        std::uint16_t bits_per_sample{0U};
        float current_volume{1.0F};
        float fade_start_volume{1.0F};
        float target_volume{1.0F};
        std::uint32_t fade_total_ticks{0U};
        std::uint32_t fade_ticks_remaining{0U};
        bool playing{false};
        bool loop{false};
        bool stop_after_fade{false};
        bool source_exhausted{false};
        bool output_started{false};
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
        std::vector<AnimationRuntimeState> animations{};
        std::vector<std::vector<std::uint16_t>> tile_layers{};
        std::vector<std::vector<std::int32_t>> fields{};
        std::vector<ParticleEmitterRuntimeState> particle_emitters{};
        CameraRuntimeState camera{};
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
    std::vector<AnimationRuntimeState> animation_states{};
    std::vector<std::uint32_t> animation_finished_entities{};
    std::vector<std::vector<std::uint16_t>> tile_layers{};
    std::vector<std::vector<std::int32_t>> fields{};
    std::vector<ParticleEmitterRuntimeState> particle_emitters{};
    CameraRuntimeState camera_state{};
    PendingSaveRequest pending_save{};
    MusicRequest pending_music{};
    MusicStreamState music_stream{};
    std::vector<std::uint8_t> save_buffer{};
    std::vector<std::uint8_t> load_buffer{};
    std::size_t save_capacity_limit{0U};
    std::vector<std::uint8_t> save_validation_slots{};
    std::vector<std::uint32_t> save_validation_entity_groups{};
    std::vector<std::uint32_t> save_validation_particle_slots{};
    std::filesystem::path save_directory{};
    std::uint64_t save_attempt_sequence{0U};
    std::vector<std::uint32_t> scene_enter_rules{};
    std::vector<std::uint32_t> pre_tick_rules{};
    std::vector<std::vector<std::uint32_t>> collision_rule_dispatch{};
    std::vector<std::vector<std::uint8_t>> grid_occupancy{};
    std::vector<ActionState> actions{};
    std::vector<ActionState> tick_actions{};
    std::vector<ActionState> pending_tick_edges{};
    std::vector<std::int32_t> states{};
    std::vector<SceneSnapshot> scene_snapshots{};
    std::vector<SceneSnapshot> load_scene_snapshots{};
    std::vector<std::int32_t> load_state_scratch{};
    std::unordered_map<std::string, std::uint32_t, TransparentStringHash, std::equal_to<>> state_indices{};
    std::vector<std::vector<std::vector<std::vector<TextToken>>>> localized_ui_text_tokens{};
    std::vector<std::vector<std::vector<TextToken>>> ui_text_tokens{};
    std::vector<Diagnostic> warnings{};
    std::uint32_t active_scene{0U};
    std::uint32_t focused_button{0U};
    std::uint64_t frame_index{0U};
    std::uint64_t simulation_tick{0U};
    std::uint64_t scene_tick{0U};
    std::uint32_t active_pooled_entities{0U};
    std::uint32_t peak_active_pooled_entities{0U};
    std::uint32_t peak_active_particles{0U};
    std::uint32_t active_locale_index{0U};
    std::uint32_t active_input_profile_index{0U};
    EntityRecord* current_rule_event_entity{nullptr};
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
        if (snapshot.animations.size() != animation_states.size() ||
            snapshot.tile_layers.size() != tile_layers.size() ||
            snapshot.fields.size() != fields.size() ||
            snapshot.particle_emitters.size() != particle_emitters.size()) {
            return std::unexpected(runtime_error(
                DiagnosticCode::internal_error,
                "Active scene content snapshot storage is inconsistent"));
        }
        std::copy(animation_states.begin(), animation_states.end(), snapshot.animations.begin());
        for (std::size_t layer = 0U; layer < tile_layers.size(); ++layer) {
            if (snapshot.tile_layers[layer].size() != tile_layers[layer].size()) {
                return std::unexpected(runtime_error(
                    DiagnosticCode::runtime_tile_field_state_invalid,
                    "Active tile snapshot capacity is inconsistent"));
            }
            std::copy(tile_layers[layer].begin(), tile_layers[layer].end(), snapshot.tile_layers[layer].begin());
        }
        for (std::size_t field = 0U; field < fields.size(); ++field) {
            if (snapshot.fields[field].size() != fields[field].size()) {
                return std::unexpected(runtime_error(
                    DiagnosticCode::runtime_tile_field_state_invalid,
                    "Active field snapshot capacity is inconsistent"));
            }
            std::copy(fields[field].begin(), fields[field].end(), snapshot.fields[field].begin());
        }
        for (std::size_t emitter = 0U; emitter < particle_emitters.size(); ++emitter) {
            if (snapshot.particle_emitters[emitter].slots.size() != particle_emitters[emitter].slots.size() ||
                snapshot.particle_emitters[emitter].free_heap.capacity() <
                    particle_emitters[emitter].free_heap.size() ||
                snapshot.particle_emitters[emitter].active_next.size() !=
                    particle_emitters[emitter].active_next.size() ||
                snapshot.particle_emitters[emitter].active_previous.size() !=
                    particle_emitters[emitter].active_previous.size()) {
                return std::unexpected(runtime_error(
                    DiagnosticCode::internal_error,
                    "Active particle snapshot capacity is inconsistent"));
            }
            std::copy(
                particle_emitters[emitter].slots.begin(), particle_emitters[emitter].slots.end(),
                snapshot.particle_emitters[emitter].slots.begin());
            snapshot.particle_emitters[emitter].free_heap.assign(
                particle_emitters[emitter].free_heap.begin(),
                particle_emitters[emitter].free_heap.end());
            std::copy(
                particle_emitters[emitter].active_next.begin(),
                particle_emitters[emitter].active_next.end(),
                snapshot.particle_emitters[emitter].active_next.begin());
            std::copy(
                particle_emitters[emitter].active_previous.begin(),
                particle_emitters[emitter].active_previous.end(),
                snapshot.particle_emitters[emitter].active_previous.begin());
            snapshot.particle_emitters[emitter].active_head =
                particle_emitters[emitter].active_head;
            snapshot.particle_emitters[emitter].active_tail =
                particle_emitters[emitter].active_tail;
            snapshot.particle_emitters[emitter].next_acquisition_order =
                particle_emitters[emitter].next_acquisition_order;
            snapshot.particle_emitters[emitter].emission_count = particle_emitters[emitter].emission_count;
            snapshot.particle_emitters[emitter].active_count = particle_emitters[emitter].active_count;
        }
        snapshot.camera = camera_state;
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

    [[nodiscard]] bool saved_pool_storage_valid(
        const GameScenePlan& scene,
        const std::uint32_t pool_index,
        const PoolRuntimeState& state,
        const std::span<const EntityState2D> entity_states,
        const std::uint64_t saved_scene_tick) {
        if (pool_index >= scene.pools.size()) return false;
        const auto group_index = scene.pools[pool_index].spawn_group_index;
        if (group_index >= scene.spawn_groups.size()) return false;
        const auto capacity = scene.spawn_groups[group_index].count;
        if (capacity == 0U || save_validation_slots.size() < capacity ||
            state.free_ring.size() != capacity || state.active_next.size() != capacity ||
            state.active_previous.size() != capacity || state.expiration_ticks.size() != capacity ||
            state.acquired.size() != capacity || state.free_count > capacity ||
            state.active_count > capacity || state.free_count + state.active_count != capacity ||
            (state.free_count != 0U && state.free_head >= capacity)) {
            return false;
        }
        std::uint32_t group_begin = 0U;
        for (std::uint32_t index = 0U; index < group_index; ++index) {
            group_begin += scene.spawn_groups[index].count;
        }
        if (static_cast<std::uint64_t>(group_begin) + capacity > entity_states.size()) return false;

        std::fill(save_validation_slots.begin(), save_validation_slots.begin() + capacity, std::uint8_t{0U});
        for (std::uint32_t order = 0U; order < state.free_count; ++order) {
            const auto slot = state.free_ring[(state.free_head + order) % capacity];
            if (slot >= capacity || save_validation_slots[slot] != 0U || state.acquired[slot] != 0U ||
                state.active_previous[slot] != PoolRuntimeState::invalid_slot ||
                state.active_next[slot] != PoolRuntimeState::invalid_slot ||
                state.expiration_ticks[slot] != PoolRuntimeState::no_expiration ||
                entity_states[group_begin + slot].active) {
                return false;
            }
            save_validation_slots[slot] = 1U;
        }

        if ((state.active_count == 0U &&
             (state.active_head != PoolRuntimeState::invalid_slot ||
              state.active_tail != PoolRuntimeState::invalid_slot)) ||
            (state.active_count != 0U &&
             (state.active_head >= capacity || state.active_tail >= capacity))) {
            return false;
        }
        auto slot = state.active_head;
        auto previous = PoolRuntimeState::invalid_slot;
        for (std::uint32_t order = 0U; order < state.active_count; ++order) {
            if (slot >= capacity || save_validation_slots[slot] != 0U || state.acquired[slot] != 1U ||
                state.active_previous[slot] != previous ||
                (state.expiration_ticks[slot] != PoolRuntimeState::no_expiration &&
                 state.expiration_ticks[slot] < saved_scene_tick) ||
                !entity_states[group_begin + slot].active) {
                return false;
            }
            save_validation_slots[slot] = 2U;
            previous = slot;
            slot = state.active_next[slot];
        }
        if (slot != PoolRuntimeState::invalid_slot ||
            (state.active_count != 0U && previous != state.active_tail)) {
            return false;
        }
        return std::none_of(
            save_validation_slots.begin(), save_validation_slots.begin() + capacity,
            [](const std::uint8_t value) { return value == 0U; });
    }

    [[nodiscard]] EntityRecord& pool_record(const std::uint32_t pool_index, const std::uint32_t slot) {
        const auto group_index = plan.scenes[active_scene].pools[pool_index].spawn_group_index;
        return entities[group_ranges[group_index].begin + slot];
    }

    void initialize_particle_storage(
        ParticleEmitterRuntimeState& state,
        const std::uint32_t capacity) {
        state.slots.assign(capacity, ParticleSlot{});
        state.free_heap.resize(capacity);
        state.active_next.assign(capacity, ParticleEmitterRuntimeState::invalid_slot);
        state.active_previous.assign(capacity, ParticleEmitterRuntimeState::invalid_slot);
        for (std::uint32_t slot = 0U; slot < capacity; ++slot) state.free_heap[slot] = slot;
        state.active_head = ParticleEmitterRuntimeState::invalid_slot;
        state.active_tail = ParticleEmitterRuntimeState::invalid_slot;
        state.next_acquisition_order = 0U;
        state.emission_count = 0U;
        state.active_count = 0U;
    }

    Result<std::uint32_t> pop_particle_free(
        ParticleEmitterRuntimeState& state,
        GameRuntimeFrameMetrics* const metrics) {
        if (state.free_heap.empty()) {
            return std::unexpected(runtime_error(
                DiagnosticCode::internal_error, "Particle available-slot heap is empty"));
        }
        const auto selected = state.free_heap.front();
        const auto replacement = state.free_heap.back();
        state.free_heap.pop_back();
        if (!state.free_heap.empty()) {
            state.free_heap.front() = replacement;
            std::size_t index = 0U;
            while (true) {
                const auto left = index * 2U + 1U;
                if (left >= state.free_heap.size()) break;
                const auto right = left + 1U;
                auto smallest = left;
                if (right < state.free_heap.size()) {
                    if (metrics != nullptr) ++metrics->particle_slot_operations;
                    if (state.free_heap[right] < state.free_heap[left]) smallest = right;
                }
                if (metrics != nullptr) ++metrics->particle_slot_operations;
                if (state.free_heap[index] <= state.free_heap[smallest]) break;
                std::swap(state.free_heap[index], state.free_heap[smallest]);
                index = smallest;
            }
        }
        return selected;
    }

    Result<void> push_particle_free(
        ParticleEmitterRuntimeState& state,
        const std::uint32_t slot,
        GameRuntimeFrameMetrics* const metrics) {
        if (slot >= state.slots.size() || state.free_heap.size() >= state.free_heap.capacity()) {
            return std::unexpected(runtime_error(
                DiagnosticCode::internal_error, "Particle available-slot heap capacity is invalid"));
        }
        state.free_heap.push_back(slot);
        auto index = state.free_heap.size() - 1U;
        while (index != 0U) {
            const auto parent = (index - 1U) / 2U;
            if (metrics != nullptr) ++metrics->particle_slot_operations;
            if (state.free_heap[parent] <= state.free_heap[index]) break;
            std::swap(state.free_heap[parent], state.free_heap[index]);
            index = parent;
        }
        return {};
    }

    Result<void> detach_active_particle(
        ParticleEmitterRuntimeState& state,
        const std::uint32_t slot) {
        if (slot >= state.slots.size() || !state.slots[slot].active || state.active_count == 0U) {
            return std::unexpected(runtime_error(
                DiagnosticCode::internal_error, "Particle active list is inconsistent"));
        }
        const auto previous = state.active_previous[slot];
        const auto next = state.active_next[slot];
        if (previous == ParticleEmitterRuntimeState::invalid_slot) state.active_head = next;
        else state.active_next[previous] = next;
        if (next == ParticleEmitterRuntimeState::invalid_slot) state.active_tail = previous;
        else state.active_previous[next] = previous;
        state.active_previous[slot] = ParticleEmitterRuntimeState::invalid_slot;
        state.active_next[slot] = ParticleEmitterRuntimeState::invalid_slot;
        --state.active_count;
        return {};
    }

    Result<void> append_active_particle(
        ParticleEmitterRuntimeState& state,
        const std::uint32_t slot) {
        if (slot >= state.slots.size() || state.slots[slot].active ||
            state.active_count >= state.slots.size()) {
            return std::unexpected(runtime_error(
                DiagnosticCode::internal_error, "Particle active append is inconsistent"));
        }
        state.active_previous[slot] = state.active_tail;
        state.active_next[slot] = ParticleEmitterRuntimeState::invalid_slot;
        if (state.active_tail == ParticleEmitterRuntimeState::invalid_slot) state.active_head = slot;
        else state.active_next[state.active_tail] = slot;
        state.active_tail = slot;
        ++state.active_count;
        return {};
    }

    Result<void> restore_record_animation(const EntityRecord& record) {
        if (record.entity.index >= animation_states.size() ||
            entities[record.entity.index].entity != record.entity) {
            return std::unexpected(runtime_error(
                DiagnosticCode::runtime_animation_state_invalid,
                "Entity animation state does not map to the active scene"));
        }
        AnimationRuntimeState state{};
        if (record.has_initial_animation) {
            if (record.initial_animation_index >= plan.animations.size() ||
                plan.animations[record.initial_animation_index].frames.empty()) {
                return std::unexpected(runtime_error(
                    DiagnosticCode::runtime_animation_state_invalid,
                    "Entity initial animation is invalid"));
            }
            state.clip_index = record.initial_animation_index;
            state.frame_index = 0U;
            state.ticks_remaining = plan.animations[state.clip_index].frames.front().duration_ticks;
            state.has_clip = true;
            state.playing = record.initial_animation_playing;
            if (record.has_sprite) {
                auto* sprite = world->sprite(record.entity);
                if (sprite == nullptr) {
                    return std::unexpected(runtime_error(
                        DiagnosticCode::runtime_animation_state_invalid,
                        "Animated entity is missing Sprite2D"));
                }
                sprite->texture =
#if defined(AI2D_ENABLE_GPU)
                    textures[plan.animations[state.clip_index].asset_index];
#else
                    TextureHandle{plan.animations[state.clip_index].asset_index, 1U};
#endif
                sprite->uv = plan.animations[state.clip_index].frames.front().uv;
            }
        }
        animation_states[record.entity.index] = state;
        return {};
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
        if (auto animation = restore_record_animation(record); !animation) return animation;
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
            if (auto animation = restore_record_animation(record); !animation) return animation;
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
        std::vector<AnimationRuntimeState> next_animation_states{};
        next_animation_states.reserve(scene.total_spawn_count);
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
                    if (group.has_initial_animation) {
                        if (group.initial_animation_index >= plan.animations.size() ||
                            plan.animations[group.initial_animation_index].frames.empty()) {
                            return std::unexpected(runtime_error(
                                DiagnosticCode::runtime_animation_state_invalid,
                                "Initial animation references an invalid clip"));
                        }
                        const auto& clip = plan.animations[group.initial_animation_index];
#if defined(AI2D_ENABLE_GPU)
                        sprite->texture = textures[clip.asset_index];
#else
                        sprite->texture = {clip.asset_index, 1U};
#endif
                        sprite->uv = clip.frames.front().uv;
                    }
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
                AnimationRuntimeState animation_state{};
                if (group.has_initial_animation) {
                    if (group.initial_animation_index >= plan.animations.size() ||
                        plan.animations[group.initial_animation_index].frames.empty()) {
                        return std::unexpected(runtime_error(
                            DiagnosticCode::runtime_animation_state_invalid,
                            "Initial animation references an invalid clip"));
                    }
                    animation_state.clip_index = group.initial_animation_index;
                    animation_state.frame_index = 0U;
                    animation_state.ticks_remaining =
                        plan.animations[group.initial_animation_index].frames.front().duration_ticks;
                    animation_state.has_clip = true;
                    animation_state.playing = group.animation_autoplay;
                    record.initial_animation_index = group.initial_animation_index;
                    record.has_initial_animation = true;
                    record.initial_animation_playing = group.animation_autoplay;
                }
                if (next_pool_for_group[group_index] >= 0) {
                    record.pooled = true;
                    record.pool_index = static_cast<std::uint32_t>(next_pool_for_group[group_index]);
                    record.pool_slot_index = item;
                }
                if (auto added = next_world->add(*entity, EntityState2D{record.initial_active}); !added) {
                    return std::unexpected(std::move(added.error()));
                }
                next_entities.push_back(record);
                next_animation_states.push_back(animation_state);
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
        std::vector<std::vector<std::uint16_t>> next_tile_layers{};
        next_tile_layers.reserve(scene.tile_layers.size());
        for (const auto& layer : scene.tile_layers) next_tile_layers.push_back(layer.initial_cells);
        std::vector<std::vector<std::int32_t>> next_fields{};
        next_fields.reserve(scene.fields.size());
        for (const auto& field : scene.fields) next_fields.push_back(field.initial_cells);
        std::vector<ParticleEmitterRuntimeState> next_particle_emitters(scene.particle_emitters.size());
        for (std::size_t emitter = 0U; emitter < scene.particle_emitters.size(); ++emitter) {
            initialize_particle_storage(
                next_particle_emitters[emitter], scene.particle_emitters[emitter].capacity);
        }
        CameraRuntimeState next_camera{};
        next_camera.position = scene.camera.position;
        next_camera.half_extent = scene.camera.half_extent;
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
            if (snapshot.animations.size() != next_animation_states.size() ||
                snapshot.tile_layers.size() != next_tile_layers.size() ||
                snapshot.fields.size() != next_fields.size() ||
                snapshot.particle_emitters.size() != next_particle_emitters.size()) {
                return std::unexpected(runtime_error(
                    DiagnosticCode::internal_error,
                    "Retained content snapshot does not match its scene plan"));
            }
            next_animation_states = snapshot.animations;
            next_tile_layers = snapshot.tile_layers;
            next_fields = snapshot.fields;
            next_particle_emitters = snapshot.particle_emitters;
            next_camera = snapshot.camera;
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
        animation_states = std::move(next_animation_states);
        tile_layers = std::move(next_tile_layers);
        fields = std::move(next_fields);
        particle_emitters = std::move(next_particle_emitters);
        camera_state = next_camera;
        animation_finished_entities.clear();
        animation_finished_entities.reserve(scene.total_spawn_count);
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
        const auto& bindings = plan.input_profiles.empty()
                                   ? plan.actions
                                   : plan.input_profiles[active_input_profile_index].actions;
        for (std::size_t action_index = 0U; action_index < bindings.size(); ++action_index) {
            const auto& binding = bindings[action_index];
            const bool previous_down = actions[action_index].down;
            ActionState state{};
            bool raw_pressed = false;
            bool raw_released = false;
            for (std::uint32_t key_index = 0U; key_index < binding.key_count; ++key_index) {
                const auto key = translate_key(binding.keys[key_index]);
                state.down = state.down || input.down(key);
                raw_pressed = raw_pressed || input.pressed(key);
                raw_released = raw_released || input.released(key);
            }
            if (binding.mouse_left) {
                state.down = state.down || input.mouse_left;
                raw_pressed = raw_pressed || input.mouse_left_pressed;
                raw_released = raw_released || input.mouse_left_released;
            }
            for (std::uint32_t button_index = 0U;
                 button_index < binding.gamepad_button_count; ++button_index) {
                const auto button = static_cast<InputGamepadButton>(
                    static_cast<std::uint8_t>(binding.gamepad_buttons[button_index]));
                state.down = state.down || input.down(button);
                raw_pressed = raw_pressed || input.pressed(button);
                raw_released = raw_released || input.released(button);
            }
            for (std::uint32_t axis_index = 0U; axis_index < binding.gamepad_axis_count; ++axis_index) {
                const auto& axis_binding = binding.gamepad_axes[axis_index];
                const auto axis = static_cast<InputGamepadAxis>(
                    static_cast<std::uint8_t>(axis_binding.axis));
                const float signed_value = input.axis(axis) * static_cast<float>(axis_binding.direction);
                state.down = state.down || signed_value >= axis_binding.deadzone;
            }
            // A logical action is the aggregate of all of its physical bindings.
            // A second constituent press must not retrigger an aggregate that is
            // already held. Preserve a complete between-polls tap only when both
            // the previous and resulting aggregate states are up.
            const bool aggregate_tap = !previous_down && !state.down && raw_pressed && raw_released;
            state.pressed = (!previous_down && state.down) || aggregate_tap;
            state.released = (previous_down && !state.down) || aggregate_tap;
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
                const bool was_down = actions[element.action].down;
                actions[element.action].pressed = actions[element.action].pressed || !was_down;
                actions[element.action].down = true;
                pending_tick_edges[element.action].pressed =
                    pending_tick_edges[element.action].pressed || !was_down;
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
        if (target.kind == GameRuleTargetKind::event_entity) {
            if (current_rule_event_entity == nullptr) {
                return std::unexpected(runtime_error(
                    DiagnosticCode::runtime_animation_state_invalid,
                    "event_entity target used outside an animation event"));
            }
            return function(*current_rule_event_entity);
        }
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

    Result<std::optional<std::size_t>> resolve_cell_index(
        const std::uint32_t grid_index,
        const GameCellSourceKind source_kind,
        const std::uint32_t x,
        const std::uint32_t y,
        const GameRuleTargetPlan& target,
        const CollisionEvent2D* const collision_event) {
        const auto& scene = plan.scenes[active_scene];
        if (grid_index >= scene.grids.size()) {
            return std::unexpected(runtime_error(
                DiagnosticCode::runtime_tile_field_state_invalid,
                "Cell source references an invalid logical grid"));
        }
        const auto& grid = scene.grids[grid_index];
        if (source_kind == GameCellSourceKind::constant) {
            if (x >= grid.columns || y >= grid.rows) return std::optional<std::size_t>{};
            return std::optional<std::size_t>{static_cast<std::size_t>(y) * grid.columns + x};
        }
        auto entity = resolve_single_target(target, collision_event);
        if (!entity) return std::unexpected(std::move(entity.error()));
        Vec2 position{};
        if (collision_event != nullptr &&
            (target.kind == GameRuleTargetKind::collision_a ||
             target.kind == GameRuleTargetKind::collision_b)) {
            position = target.kind == GameRuleTargetKind::collision_a
                           ? collision_event->position_a
                           : collision_event->position_b;
        } else {
            const auto* transform = world->transform((*entity)->entity);
            if (transform == nullptr) {
                return std::unexpected(runtime_error(
                    DiagnosticCode::runtime_tile_field_state_invalid,
                    "Cell target is missing Transform2D"));
            }
            position = transform->position;
        }
        const double origin_x = static_cast<double>(grid.first_cell_center.x) - grid.cell_size.x * 0.5;
        const double origin_y = static_cast<double>(grid.first_cell_center.y) - grid.cell_size.y * 0.5;
        const double column_value = (static_cast<double>(position.x) - origin_x) / grid.cell_size.x;
        const double row_value = (static_cast<double>(position.y) - origin_y) / grid.cell_size.y;
        if (!std::isfinite(column_value) || !std::isfinite(row_value) || column_value < 0.0 || row_value < 0.0 ||
            column_value >= static_cast<double>(grid.columns) ||
            row_value >= static_cast<double>(grid.rows)) {
            return std::optional<std::size_t>{};
        }
        const auto column = static_cast<std::uint32_t>(column_value);
        const auto row = static_cast<std::uint32_t>(row_value);
        return std::optional<std::size_t>{static_cast<std::size_t>(row) * grid.columns + column};
    }

    Result<bool> rule_conditions_match(
        const GameRulePlan& rule,
        const CollisionEvent2D* const collision_event,
        GameRuntimeFrameMetrics& metrics) {
        for (const auto& condition : rule.conditions) {
            ++metrics.condition_evaluations;
            std::int64_t left = 0;
            if (condition.kind == GameRuleConditionKind::int_state) {
                left = states[condition.state_index];
            } else if (condition.kind == GameRuleConditionKind::group_active_count) {
                left = group_active_count(condition.spawn_group_index);
            } else if (condition.kind == GameRuleConditionKind::tile_value) {
                const auto& layer = plan.scenes[active_scene].tile_layers[condition.tile_layer_index];
                auto cell = resolve_cell_index(
                    layer.grid_index, condition.cell_source, condition.cell_x, condition.cell_y,
                    condition.cell_target, collision_event);
                if (!cell) return std::unexpected(std::move(cell.error()));
                ++metrics.tile_reads;
                if (!cell->has_value()) return false;
                left = tile_layers[condition.tile_layer_index][**cell];
            } else if (condition.kind == GameRuleConditionKind::field_value) {
                const auto& field = plan.scenes[active_scene].fields[condition.field_index];
                auto cell = resolve_cell_index(
                    field.grid_index, condition.cell_source, condition.cell_x, condition.cell_y,
                    condition.cell_target, collision_event);
                if (!cell) return std::unexpected(std::move(cell.error()));
                ++metrics.field_reads;
                if (!cell->has_value()) return false;
                left = fields[condition.field_index][**cell];
            }
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

    Result<void> apply_animation_frame(EntityRecord& record, AnimationRuntimeState& state) {
        if (!record.has_sprite || !state.has_clip || state.clip_index >= plan.animations.size()) {
            return std::unexpected(runtime_error(
                DiagnosticCode::runtime_animation_state_invalid,
                "Animated entity state is inconsistent"));
        }
        const auto& clip = plan.animations[state.clip_index];
        if (state.frame_index >= clip.frames.size()) {
            return std::unexpected(runtime_error(
                DiagnosticCode::runtime_animation_state_invalid,
                "Animation frame index is out of range"));
        }
        auto* sprite = world->sprite(record.entity);
        if (sprite == nullptr) {
            return std::unexpected(runtime_error(
                DiagnosticCode::runtime_animation_state_invalid,
                "Animated entity is missing Sprite2D"));
        }
#if defined(AI2D_ENABLE_GPU)
        sprite->texture = textures[clip.asset_index];
#else
        sprite->texture = {clip.asset_index, 1U};
#endif
        sprite->uv = clip.frames[state.frame_index].uv;
        return {};
    }

    Result<void> play_animation_action(
        const GameRuleActionPlan& action,
        const CollisionEvent2D* const collision_event,
        const bool play) {
        return for_each_target(action.target, collision_event, [&](EntityRecord& record) -> Result<void> {
            if (record.entity.index >= animation_states.size() || !record.has_sprite) {
                return std::unexpected(runtime_error(
                    DiagnosticCode::runtime_animation_state_invalid,
                    "Animation action target is invalid"));
            }
            auto& state = animation_states[record.entity.index];
            if (!play) {
                state.playing = false;
                return {};
            }
            if (action.animation_index >= plan.animations.size() ||
                plan.animations[action.animation_index].frames.empty()) {
                return std::unexpected(runtime_error(
                    DiagnosticCode::runtime_animation_state_invalid,
                    "Animation action references an invalid clip"));
            }
            const bool same_clip = state.has_clip && state.clip_index == action.animation_index;
            if (!same_clip || action.restart_animation || state.completion_emitted) {
                state.clip_index = action.animation_index;
                state.frame_index = 0U;
                state.direction = 1;
                state.ticks_remaining = plan.animations[action.animation_index].frames.front().duration_ticks;
                state.completion_emitted = false;
            }
            state.has_clip = true;
            state.playing = true;
            return apply_animation_frame(record, state);
        });
    }

    Result<void> update_animations(GameRuntimeFrameMetrics& metrics) {
        animation_finished_entities.clear();
        for (std::uint32_t entity_index = 0U; entity_index < animation_states.size(); ++entity_index) {
            auto& state = animation_states[entity_index];
            if (!state.has_clip || !state.playing) continue;
            const auto* active = world->entity_state(entities[entity_index].entity);
            if (active == nullptr || !active->active) continue;
            if (state.clip_index >= plan.animations.size()) {
                return std::unexpected(runtime_error(
                    DiagnosticCode::runtime_animation_state_invalid,
                    "Animation clip index is out of range"));
            }
            const auto& clip = plan.animations[state.clip_index];
            if (clip.frames.empty() || state.frame_index >= clip.frames.size() || state.ticks_remaining == 0U) {
                return std::unexpected(runtime_error(
                    DiagnosticCode::runtime_animation_state_invalid,
                    "Animation playback state is corrupt"));
            }
            --state.ticks_remaining;
            if (state.ticks_remaining != 0U) continue;
            bool completed = false;
            if (clip.mode == GameAnimationMode::once) {
                if (state.frame_index + 1U < clip.frames.size()) {
                    ++state.frame_index;
                } else {
                    state.playing = false;
                    if (!state.completion_emitted) {
                        state.completion_emitted = true;
                        completed = true;
                    }
                }
            } else if (clip.mode == GameAnimationMode::loop) {
                state.frame_index = (state.frame_index + 1U) % static_cast<std::uint32_t>(clip.frames.size());
            } else if (clip.frames.size() > 1U) {
                if (state.direction > 0 && state.frame_index + 1U >= clip.frames.size()) state.direction = -1;
                else if (state.direction < 0 && state.frame_index == 0U) state.direction = 1;
                state.frame_index = static_cast<std::uint32_t>(
                    static_cast<std::int64_t>(state.frame_index) + state.direction);
            }
            if (state.playing) {
                state.ticks_remaining = clip.frames[state.frame_index].duration_ticks;
                if (auto applied = apply_animation_frame(entities[entity_index], state); !applied) return applied;
                ++metrics.animation_frame_updates;
            }
            if (completed) {
                if (animation_finished_entities.size() >= animation_finished_entities.capacity()) {
                    return std::unexpected(runtime_error(
                        DiagnosticCode::runtime_animation_state_invalid,
                        "Animation completion queue exceeded its reserved capacity"));
                }
                animation_finished_entities.push_back(entity_index);
                ++metrics.animation_completions;
            }
        }
        const auto& rules = plan.scenes[active_scene].rules;
        for (const auto entity_index : animation_finished_entities) {
            const auto clip_index = animation_states[entity_index].clip_index;
            for (std::uint32_t rule_index = 0U; rule_index < rules.size(); ++rule_index) {
                const auto& event = rules[rule_index].event;
                if (event.kind != GameRuleEventKind::animation_finished ||
                    event.animation_index != clip_index) {
                    continue;
                }
                if (auto executed = execute_rule(
                        rule_index, nullptr, metrics, &entities[entity_index]);
                    !executed) {
                    return executed;
                }
            }
        }
        return {};
    }

    Result<Vec2> resolve_position_source(
        const GamePoolSpawnPositionKind kind,
        const Vec2 constant,
        const GameRuleTargetPlan& target,
        const Vec2 offset,
        const CollisionEvent2D* const collision_event) {
        Vec2 base = constant;
        if (kind == GamePoolSpawnPositionKind::target) {
            auto entity = resolve_single_target(target, collision_event);
            if (!entity) return std::unexpected(std::move(entity.error()));
            if (collision_event != nullptr &&
                (target.kind == GameRuleTargetKind::collision_a ||
                 target.kind == GameRuleTargetKind::collision_b)) {
                base = target.kind == GameRuleTargetKind::collision_a
                           ? collision_event->position_a
                           : collision_event->position_b;
            } else {
                const auto* transform = world->transform((*entity)->entity);
                if (transform == nullptr) {
                    return std::unexpected(runtime_error(
                        DiagnosticCode::game_presentation_invalid,
                        "Presentation target is missing Transform2D"));
                }
                base = transform->position;
            }
        }
        const double x = static_cast<double>(base.x) + offset.x;
        const double y = static_cast<double>(base.y) + offset.y;
        if (!representable_float(x) || !representable_float(y)) {
            return std::unexpected(runtime_error(
                DiagnosticCode::runtime_numeric_state_invalid,
                "Presentation position exceeds the supported numeric range"));
        }
        return Vec2{static_cast<float>(x), static_cast<float>(y)};
    }

    Result<void> emit_particles(
        const GameRuleActionPlan& action,
        const CollisionEvent2D* const collision_event,
        GameRuntimeFrameMetrics& metrics) {
        if (action.particle_emitter_index >= particle_emitters.size()) {
            return std::unexpected(runtime_error(
                DiagnosticCode::game_presentation_invalid,
                "Particle action references an invalid emitter"));
        }
        auto position = resolve_position_source(
            action.particle_position_kind, action.particle_position,
            action.particle_position_target, action.particle_position_offset, collision_event);
        if (!position) return std::unexpected(std::move(position.error()));
        auto& state = particle_emitters[action.particle_emitter_index];
        const auto& plan_emitter = plan.scenes[active_scene].particle_emitters[action.particle_emitter_index];
        if (state.emission_count > std::numeric_limits<std::uint64_t>::max() - action.particle_count ||
            state.next_acquisition_order >
                std::numeric_limits<std::uint64_t>::max() - action.particle_count ||
            scene_tick > std::numeric_limits<std::uint64_t>::max() - plan_emitter.lifetime_max_ticks) {
            return std::unexpected(runtime_error(
                DiagnosticCode::runtime_numeric_state_invalid,
                "Particle counters or lifetime lack bounded headroom"));
        }
        for (std::uint32_t emission = 0U; emission < action.particle_count; ++emission) {
            bool recycle = false;
            std::uint32_t selected = ParticleEmitterRuntimeState::invalid_slot;
            if (!state.free_heap.empty()) {
                auto popped = pop_particle_free(state, &metrics);
                if (!popped) return std::unexpected(std::move(popped.error()));
                selected = *popped;
            } else {
                ++metrics.particle_exhaustions;
                if (plan_emitter.on_exhausted == GamePoolExhaustionPolicy::skip) continue;
                selected = state.active_head;
                ++metrics.particle_slot_operations;
                if (selected == ParticleEmitterRuntimeState::invalid_slot) {
                    return std::unexpected(runtime_error(
                        DiagnosticCode::internal_error,
                        "Particle recycle policy has no oldest active slot"));
                }
                recycle = true;
            }
            const auto random_a = mix_u64(
                plan.seed ^ (static_cast<std::uint64_t>(active_scene + 1U) << 48U) ^
                (static_cast<std::uint64_t>(action.particle_emitter_index + 1U) << 32U) ^
                state.emission_count);
            const auto random_b = mix_u64(random_a);
            const auto unit = [](const std::uint64_t value) {
                return static_cast<float>(value & 0x00FF'FFFFULL) /
                       static_cast<float>(0x0100'0000ULL);
            };
            const Vec2 velocity{
                plan_emitter.velocity_min.x +
                    (plan_emitter.velocity_max.x - plan_emitter.velocity_min.x) * unit(random_a),
                plan_emitter.velocity_min.y +
                    (plan_emitter.velocity_max.y - plan_emitter.velocity_min.y) * unit(random_b),
            };
            const auto lifetime_span = plan_emitter.lifetime_max_ticks - plan_emitter.lifetime_min_ticks + 1U;
            const auto lifetime = plan_emitter.lifetime_min_ticks +
                                  static_cast<std::uint32_t>((random_b >> 32U) % lifetime_span);
            if (recycle) {
                if (auto detached = detach_active_particle(state, selected); !detached) return detached;
                state.slots[selected].active = false;
            }
            auto& slot = state.slots[selected];
            slot.position = *position;
            slot.previous_position = *position;
            slot.velocity = velocity;
            slot.expiration_tick = scene_tick + lifetime;
            slot.acquisition_order = state.next_acquisition_order;
            if (auto appended = append_active_particle(state, selected); !appended) return appended;
            slot.active = true;
            ++state.next_acquisition_order;
            ++state.emission_count;
            ++metrics.particle_emits;
        }
        peak_active_particles = std::max(peak_active_particles, state.active_count);
        metrics.peak_active_particles = std::max(metrics.peak_active_particles, peak_active_particles);
        return {};
    }

    Result<void> update_particles(const float delta_seconds, GameRuntimeFrameMetrics& metrics) {
        for (auto& emitter : particle_emitters) {
            for (std::uint32_t slot_index = 0U; slot_index < emitter.slots.size(); ++slot_index) {
                auto& slot = emitter.slots[slot_index];
                if (!slot.active) continue;
                if (slot.expiration_tick <= scene_tick) {
                    if (auto detached = detach_active_particle(emitter, slot_index); !detached) return detached;
                    slot.active = false;
                    if (auto released = push_particle_free(emitter, slot_index, &metrics); !released) return released;
                    continue;
                }
                slot.previous_position = slot.position;
                const double x = static_cast<double>(slot.position.x) + slot.velocity.x * delta_seconds;
                const double y = static_cast<double>(slot.position.y) + slot.velocity.y * delta_seconds;
                if (!representable_float(x) || !representable_float(y)) {
                    return std::unexpected(runtime_error(
                        DiagnosticCode::runtime_numeric_state_invalid,
                        "Particle position exceeds the supported numeric range"));
                }
                slot.position = {static_cast<float>(x), static_cast<float>(y)};
                ++metrics.particle_updates;
            }
        }
        metrics.peak_active_particles = std::max(metrics.peak_active_particles, peak_active_particles);
        return {};
    }

    Result<void> update_camera(GameRuntimeFrameMetrics& metrics) {
        const auto& scene = plan.scenes[active_scene];
        Vec2 base = scene.camera.position;
        if (scene.camera.mode == GameCameraMode::follow) {
            if (scene.camera.follow_group_index >= group_ranges.size()) {
                return std::unexpected(runtime_error(
                    DiagnosticCode::game_presentation_invalid,
                    "Camera follow group is invalid"));
            }
            const auto range = group_ranges[scene.camera.follow_group_index];
            if (scene.camera.follow_item_index >= range.count) {
                return std::unexpected(runtime_error(
                    DiagnosticCode::game_presentation_invalid,
                    "Camera follow entity is invalid"));
            }
            const auto* transform = world->transform(
                entities[range.begin + scene.camera.follow_item_index].entity);
            if (transform == nullptr) {
                return std::unexpected(runtime_error(
                    DiagnosticCode::game_presentation_invalid,
                    "Camera follow entity is missing Transform2D"));
            }
            base = {transform->position.x + scene.camera.follow_offset.x,
                    transform->position.y + scene.camera.follow_offset.y};
            if (scene.camera.has_bounds) {
                base.x = std::clamp(base.x, scene.camera.bounds.min.x, scene.camera.bounds.max.x);
                base.y = std::clamp(base.y, scene.camera.bounds.min.y, scene.camera.bounds.max.y);
            }
            ++metrics.camera_follow_updates;
        }
        camera_state.half_extent = {
            scene.camera.half_extent.x / camera_state.zoom,
            scene.camera.half_extent.y / camera_state.zoom,
        };
        if (scene.camera.pixel_snap) {
            const float step_x = camera_state.half_extent.x * 2.0F /
                                 static_cast<float>(plan.window.virtual_width);
            const float step_y = camera_state.half_extent.y * 2.0F /
                                 static_cast<float>(plan.window.virtual_height);
            if (step_x > 0.0F && step_y > 0.0F) {
                base.x = std::round(base.x / step_x) * step_x;
                base.y = std::round(base.y / step_y) * step_y;
            }
        }
        if (camera_state.shake_ticks_remaining != 0U) {
            const auto random = mix_u64(
                plan.seed ^ scene_tick ^ (camera_state.shake_invocation * 0x9E3779B97F4A7C15ULL));
            const auto signed_unit = [](const std::uint32_t value) {
                return static_cast<float>(value) /
                           static_cast<float>(std::numeric_limits<std::uint32_t>::max()) * 2.0F - 1.0F;
            };
            base.x += signed_unit(static_cast<std::uint32_t>(random)) * camera_state.shake_amplitude;
            base.y += signed_unit(static_cast<std::uint32_t>(random >> 32U)) * camera_state.shake_amplitude;
            --camera_state.shake_ticks_remaining;
            ++metrics.camera_shake_updates;
        }
        camera_state.position = base;
        return {};
    }

    static void append_u8(std::vector<std::uint8_t>& output, const std::uint8_t value) {
        output.push_back(value);
    }

    static void append_u32(std::vector<std::uint8_t>& output, const std::uint32_t value) {
        for (std::uint32_t shift = 0U; shift < 32U; shift += 8U) {
            output.push_back(static_cast<std::uint8_t>(value >> shift));
        }
    }

    static void append_i32(std::vector<std::uint8_t>& output, const std::int32_t value) {
        append_u32(output, std::bit_cast<std::uint32_t>(value));
    }

    static void append_u64(std::vector<std::uint8_t>& output, const std::uint64_t value) {
        for (std::uint32_t shift = 0U; shift < 64U; shift += 8U) {
            output.push_back(static_cast<std::uint8_t>(value >> shift));
        }
    }

    static void append_float(std::vector<std::uint8_t>& output, const float value) {
        append_u32(output, std::bit_cast<std::uint32_t>(value));
    }

    static std::uint64_t checksum_bytes(const std::span<const std::uint8_t> bytes) noexcept {
        std::uint64_t hash = 14'695'981'039'346'656'037ULL;
        for (const auto byte : bytes) {
            hash ^= byte;
            hash *= 1'099'511'628'211ULL;
        }
        return hash;
    }

    struct SaveReader final {
        std::span<const std::uint8_t> bytes{};
        std::size_t offset{0U};

        Result<std::uint8_t> u8() {
            if (offset >= bytes.size()) return std::unexpected(failure_diagnostic());
            return bytes[offset++];
        }
        Result<std::uint32_t> u32() {
            if (bytes.size() - offset < 4U) return std::unexpected(failure_diagnostic());
            std::uint32_t value = 0U;
            for (std::uint32_t index = 0U; index < 4U; ++index) {
                value |= static_cast<std::uint32_t>(bytes[offset++]) << (index * 8U);
            }
            return value;
        }
        Result<std::int32_t> i32() {
            auto value = u32();
            if (!value) return std::unexpected(std::move(value.error()));
            return std::bit_cast<std::int32_t>(*value);
        }
        Result<std::uint64_t> u64() {
            if (bytes.size() - offset < 8U) return std::unexpected(failure_diagnostic());
            std::uint64_t value = 0U;
            for (std::uint32_t index = 0U; index < 8U; ++index) {
                value |= static_cast<std::uint64_t>(bytes[offset++]) << (index * 8U);
            }
            return value;
        }
        Result<float> number() {
            auto value = u32();
            if (!value) return std::unexpected(std::move(value.error()));
            const auto parsed = std::bit_cast<float>(*value);
            if (!std::isfinite(parsed)) return std::unexpected(failure_diagnostic());
            return parsed;
        }
        Result<void> expect_count(const std::size_t expected) {
            auto value = u32();
            if (!value) return std::unexpected(std::move(value.error()));
            if (*value != expected) return std::unexpected(failure_diagnostic());
            return {};
        }
        Result<std::uint8_t> boolean() {
            auto value = u8();
            if (!value) return std::unexpected(std::move(value.error()));
            if (*value > 1U) return std::unexpected(failure_diagnostic());
            return *value;
        }
        static Diagnostic failure_diagnostic() {
            return runtime_error(
                DiagnosticCode::runtime_save_state_invalid,
                "Save payload is truncated or malformed");
        }
    };

    void serialize_vec2(std::vector<std::uint8_t>& output, const Vec2 value) {
        append_float(output, value.x);
        append_float(output, value.y);
    }

    Result<Vec2> deserialize_vec2(SaveReader& reader) {
        auto x = reader.number();
        if (!x) return std::unexpected(std::move(x.error()));
        auto y = reader.number();
        if (!y) return std::unexpected(std::move(y.error()));
        return Vec2{*x, *y};
    }

    Result<void> serialize_scene_snapshot(
        std::vector<std::uint8_t>& output,
        const std::uint32_t scene_index,
        const SceneSnapshot& snapshot) {
        const auto& scene = plan.scenes[scene_index];
        append_u8(output, snapshot.retained ? 1U : 0U);
        append_u64(output, snapshot.scene_tick);
        append_u32(output, snapshot.focused_button);
        append_u32(output, static_cast<std::uint32_t>(snapshot.transforms.size()));
        std::uint32_t entity_offset = 0U;
        for (std::size_t group_index = 0U; group_index < scene.spawn_groups.size(); ++group_index) {
            const auto& group = scene.spawn_groups[group_index];
            for (std::uint32_t item = 0U; item < group.count; ++item, ++entity_offset) {
                const auto& transform = snapshot.transforms[entity_offset];
                serialize_vec2(output, transform.position);
                serialize_vec2(output, transform.previous_position);
                append_float(output, transform.rotation);
                serialize_vec2(output, transform.scale);
                append_u8(output, snapshot.entity_states[entity_offset].active ? 1U : 0U);
                if (group.has_velocity) {
                    serialize_vec2(output, snapshot.velocities[entity_offset].linear);
                    append_float(output, snapshot.velocities[entity_offset].angular);
                }
                if (group.has_sprite) {
                    const auto& sprite = snapshot.sprites[entity_offset];
                    serialize_vec2(output, sprite.size);
                    serialize_vec2(output, sprite.pivot);
                    append_float(output, sprite.tint.r);
                    append_float(output, sprite.tint.g);
                    append_float(output, sprite.tint.b);
                    append_float(output, sprite.tint.a);
                    append_i32(output, sprite.layer);
                    append_u8(output, sprite.visible ? 1U : 0U);
                    serialize_vec2(output, sprite.uv.min);
                    serialize_vec2(output, sprite.uv.max);
                }
                if (group.has_collider) {
                    const auto& collider_component = snapshot.colliders[entity_offset];
                    serialize_vec2(output, collider_component.offset);
                    serialize_vec2(output, collider_component.half_extent);
                    append_u32(output, collider_component.group);
                    append_u8(output, static_cast<std::uint8_t>(collider_component.motion));
                    append_u8(output, collider_component.trigger ? 1U : 0U);
                    append_u8(output, collider_component.enabled ? 1U : 0U);
                }
                const auto& animation = snapshot.animations[entity_offset];
                append_u32(output, animation.clip_index);
                append_u32(output, animation.frame_index);
                append_u32(output, animation.ticks_remaining);
                append_u8(output, static_cast<std::uint8_t>(animation.direction));
                append_u8(output, animation.has_clip ? 1U : 0U);
                append_u8(output, animation.playing ? 1U : 0U);
                append_u8(output, animation.completion_emitted ? 1U : 0U);
            }
        }
        append_u32(output, static_cast<std::uint32_t>(snapshot.system_states.size()));
        for (const auto& system : snapshot.system_states) {
            append_u8(output, static_cast<std::uint8_t>(system.current_direction));
            append_u8(output, static_cast<std::uint8_t>(system.queued_direction));
            append_u32(output, system.phase);
            append_u8(output, system.stepped ? 1U : 0U);
        }
        append_u32(output, static_cast<std::uint32_t>(snapshot.rule_invocations.size()));
        for (const auto invocation : snapshot.rule_invocations) append_u64(output, invocation);
        append_u32(output, static_cast<std::uint32_t>(snapshot.pool_states.size()));
        for (const auto& pool : snapshot.pool_states) {
            append_u32(output, static_cast<std::uint32_t>(pool.free_ring.size()));
            for (const auto value : pool.free_ring) append_u32(output, value);
            for (const auto value : pool.active_next) append_u32(output, value);
            for (const auto value : pool.active_previous) append_u32(output, value);
            for (const auto value : pool.expiration_ticks) append_u64(output, value);
            for (const auto value : pool.acquired) append_u8(output, value);
            append_u32(output, pool.free_head);
            append_u32(output, pool.free_count);
            append_u32(output, pool.active_head);
            append_u32(output, pool.active_tail);
            append_u32(output, pool.active_count);
        }
        append_u32(output, static_cast<std::uint32_t>(snapshot.contacts.size()));
        for (const auto& contact : snapshot.contacts) {
            append_u32(output, contact.rule_index);
            append_u32(output, contact.entity_a_index);
            append_u32(output, contact.entity_b_index);
        }
        append_u32(output, static_cast<std::uint32_t>(snapshot.tile_layers.size()));
        for (const auto& layer : snapshot.tile_layers) {
            append_u32(output, static_cast<std::uint32_t>(layer.size()));
            for (const auto value : layer) append_u32(output, value);
        }
        append_u32(output, static_cast<std::uint32_t>(snapshot.fields.size()));
        for (const auto& field : snapshot.fields) {
            append_u32(output, static_cast<std::uint32_t>(field.size()));
            for (const auto value : field) append_i32(output, value);
        }
        append_u32(output, static_cast<std::uint32_t>(snapshot.particle_emitters.size()));
        for (const auto& emitter : snapshot.particle_emitters) {
            append_u32(output, static_cast<std::uint32_t>(emitter.slots.size()));
            append_u64(output, emitter.next_acquisition_order);
            append_u64(output, emitter.emission_count);
            append_u32(output, emitter.active_count);
            for (const auto& particle : emitter.slots) {
                serialize_vec2(output, particle.position);
                serialize_vec2(output, particle.previous_position);
                serialize_vec2(output, particle.velocity);
                append_u64(output, particle.expiration_tick);
                append_u64(output, particle.acquisition_order);
                append_u8(output, particle.active ? 1U : 0U);
            }
        }
        serialize_vec2(output, snapshot.camera.position);
        serialize_vec2(output, snapshot.camera.half_extent);
        append_float(output, snapshot.camera.zoom);
        append_float(output, snapshot.camera.shake_amplitude);
        append_u32(output, snapshot.camera.shake_ticks_remaining);
        append_u64(output, snapshot.camera.shake_invocation);
        return {};
    }

    Result<void> deserialize_scene_snapshot(
        SaveReader& reader,
        const std::uint32_t scene_index,
        SceneSnapshot& snapshot) {
        const auto& scene = plan.scenes[scene_index];
        auto retained = reader.boolean();
        if (!retained) return std::unexpected(std::move(retained.error()));
        auto scene_tick_value = reader.u64();
        if (!scene_tick_value) return std::unexpected(std::move(scene_tick_value.error()));
        if (*scene_tick_value == std::numeric_limits<std::uint64_t>::max()) {
            return std::unexpected(SaveReader::failure_diagnostic());
        }
        auto focus = reader.u32();
        if (!focus) return std::unexpected(std::move(focus.error()));
        const auto button_count = static_cast<std::uint32_t>(std::count_if(
            scene.ui.begin(), scene.ui.end(),
            [](const UiElementPlan& element) { return element.kind == UiElementKind::button; }));
        if ((button_count == 0U && *focus != 0U) || (button_count != 0U && *focus >= button_count)) {
            return std::unexpected(SaveReader::failure_diagnostic());
        }
        if (auto count = reader.expect_count(snapshot.transforms.size()); !count) return count;
        snapshot.retained = *retained != 0U;
        snapshot.scene_tick = *scene_tick_value;
        snapshot.focused_button = *focus;
        if (save_validation_entity_groups.size() < snapshot.transforms.size()) {
            return std::unexpected(SaveReader::failure_diagnostic());
        }
        std::uint32_t entity_offset = 0U;
        for (std::uint32_t group_index = 0U; group_index < scene.spawn_groups.size(); ++group_index) {
            const auto& group = scene.spawn_groups[group_index];
            for (std::uint32_t item = 0U; item < group.count; ++item, ++entity_offset) {
                save_validation_entity_groups[entity_offset] = group_index;
                auto position = deserialize_vec2(reader);
                if (!position) return std::unexpected(std::move(position.error()));
                auto previous = deserialize_vec2(reader);
                if (!previous) return std::unexpected(std::move(previous.error()));
                auto rotation = reader.number();
                if (!rotation) return std::unexpected(std::move(rotation.error()));
                auto scale = deserialize_vec2(reader);
                if (!scale) return std::unexpected(std::move(scale.error()));
                if (scale->x <= 0.0F || scale->y <= 0.0F) return std::unexpected(SaveReader::failure_diagnostic());
                snapshot.transforms[entity_offset] = {*position, *rotation, *scale, *previous};
                auto active = reader.boolean();
                if (!active) return std::unexpected(std::move(active.error()));
                snapshot.entity_states[entity_offset].active = *active != 0U;
                if (group.has_velocity) {
                    auto linear = deserialize_vec2(reader);
                    if (!linear) return std::unexpected(std::move(linear.error()));
                    auto angular = reader.number();
                    if (!angular) return std::unexpected(std::move(angular.error()));
                    snapshot.velocities[entity_offset] = {*linear, *angular};
                }
                if (group.has_sprite) {
                    auto size = deserialize_vec2(reader);
                    if (!size) return std::unexpected(std::move(size.error()));
                    auto pivot = deserialize_vec2(reader);
                    if (!pivot) return std::unexpected(std::move(pivot.error()));
                    auto red = reader.number();
                    if (!red) return std::unexpected(std::move(red.error()));
                    auto green = reader.number();
                    if (!green) return std::unexpected(std::move(green.error()));
                    auto blue = reader.number();
                    if (!blue) return std::unexpected(std::move(blue.error()));
                    auto alpha = reader.number();
                    if (!alpha) return std::unexpected(std::move(alpha.error()));
                    auto layer = reader.i32();
                    if (!layer) return std::unexpected(std::move(layer.error()));
                    auto visible = reader.boolean();
                    if (!visible) return std::unexpected(std::move(visible.error()));
                    auto uv_min = deserialize_vec2(reader);
                    if (!uv_min) return std::unexpected(std::move(uv_min.error()));
                    auto uv_size = deserialize_vec2(reader);
                    if (!uv_size) return std::unexpected(std::move(uv_size.error()));
                    if (size->x <= 0.0F || size->y <= 0.0F || uv_min->x < 0.0F || uv_min->y < 0.0F ||
                        uv_size->x <= 0.0F || uv_size->y <= 0.0F || uv_min->x + uv_size->x > 1.0001F ||
                        uv_min->y + uv_size->y > 1.0001F || *red < 0.0F || *red > 1.0F ||
                        *green < 0.0F || *green > 1.0F || *blue < 0.0F || *blue > 1.0F ||
                        *alpha < 0.0F || *alpha > 1.0F) {
                        return std::unexpected(SaveReader::failure_diagnostic());
                    }
                    auto& sprite = snapshot.sprites[entity_offset];
                    sprite.size = *size;
                    sprite.pivot = *pivot;
                    sprite.tint = {*red, *green, *blue, *alpha};
                    sprite.layer = *layer;
                    sprite.visible = *visible != 0U;
                    sprite.uv = {*uv_min, *uv_size};
                    const bool expected_visible = snapshot.entity_states[entity_offset].active && group.sprite.visible;
                    if (sprite.size.x != group.sprite.size.x || sprite.size.y != group.sprite.size.y ||
                        sprite.pivot.x != group.sprite.pivot.x || sprite.pivot.y != group.sprite.pivot.y ||
                        sprite.tint.r != group.sprite.tint.r || sprite.tint.g != group.sprite.tint.g ||
                        sprite.tint.b != group.sprite.tint.b || sprite.tint.a != group.sprite.tint.a ||
                        sprite.layer != group.sprite.layer || sprite.visible != expected_visible) {
                        return std::unexpected(SaveReader::failure_diagnostic());
                    }
                }
                if (group.has_collider) {
                    auto offset = deserialize_vec2(reader);
                    if (!offset) return std::unexpected(std::move(offset.error()));
                    auto half_extent = deserialize_vec2(reader);
                    if (!half_extent) return std::unexpected(std::move(half_extent.error()));
                    auto collider_group = reader.u32();
                    if (!collider_group) return std::unexpected(std::move(collider_group.error()));
                    auto motion = reader.u8();
                    if (!motion) return std::unexpected(std::move(motion.error()));
                    auto trigger = reader.boolean();
                    if (!trigger) return std::unexpected(std::move(trigger.error()));
                    auto enabled = reader.boolean();
                    if (!enabled) return std::unexpected(std::move(enabled.error()));
                    if (half_extent->x <= 0.0F || half_extent->y <= 0.0F ||
                        *motion > static_cast<std::uint8_t>(BodyMotion2D::dynamic_body)) {
                        return std::unexpected(SaveReader::failure_diagnostic());
                    }
                    snapshot.colliders[entity_offset] = {
                        *offset, *half_extent, *collider_group, static_cast<BodyMotion2D>(*motion),
                        *trigger != 0U, *enabled != 0U};
                    const auto& collider = snapshot.colliders[entity_offset];
                    const bool expected_enabled =
                        snapshot.entity_states[entity_offset].active && group.collider.enabled;
                    if (collider.offset.x != group.collider.offset.x ||
                        collider.offset.y != group.collider.offset.y ||
                        collider.half_extent.x != group.collider.half_extent.x ||
                        collider.half_extent.y != group.collider.half_extent.y ||
                        collider.group != group.collider.group ||
                        static_cast<std::uint8_t>(collider.motion) !=
                            static_cast<std::uint8_t>(group.collider.motion) ||
                        collider.trigger != group.collider.trigger || collider.enabled != expected_enabled) {
                        return std::unexpected(SaveReader::failure_diagnostic());
                    }
                }
                auto clip = reader.u32();
                if (!clip) return std::unexpected(std::move(clip.error()));
                auto frame = reader.u32();
                if (!frame) return std::unexpected(std::move(frame.error()));
                auto remaining = reader.u32();
                if (!remaining) return std::unexpected(std::move(remaining.error()));
                auto direction = reader.u8();
                if (!direction) return std::unexpected(std::move(direction.error()));
                auto has_clip = reader.boolean();
                if (!has_clip) return std::unexpected(std::move(has_clip.error()));
                auto playing = reader.boolean();
                if (!playing) return std::unexpected(std::move(playing.error()));
                auto completion = reader.boolean();
                if (!completion) return std::unexpected(std::move(completion.error()));
                auto& animation = snapshot.animations[entity_offset];
                animation = {*clip, *frame, *remaining, std::bit_cast<std::int8_t>(*direction),
                             *has_clip != 0U, *playing != 0U, *completion != 0U};
                if (animation.has_clip) {
                    if (!group.has_sprite || animation.clip_index >= plan.animations.size() ||
                        animation.frame_index >= plan.animations[animation.clip_index].frames.size() ||
                        (animation.direction != 1 && animation.direction != -1)) {
                        return std::unexpected(SaveReader::failure_diagnostic());
                    }
                    const auto& animation_plan = plan.animations[animation.clip_index];
                    const auto frame_duration = animation_plan.frames[animation.frame_index].duration_ticks;
                    const bool completed_once =
                        animation.completion_emitted && !animation.playing &&
                        animation_plan.mode == GameAnimationMode::once &&
                        animation.frame_index + 1U == animation_plan.frames.size() &&
                        animation.ticks_remaining == 0U;
                    if ((!completed_once &&
                         (animation.completion_emitted || animation.ticks_remaining == 0U ||
                          animation.ticks_remaining > frame_duration)) ||
                        (animation_plan.mode != GameAnimationMode::ping_pong && animation.direction != 1)) {
                        return std::unexpected(SaveReader::failure_diagnostic());
                    }
#if defined(AI2D_ENABLE_GPU)
                    snapshot.sprites[entity_offset].texture =
                        textures[plan.animations[animation.clip_index].asset_index];
#else
                    snapshot.sprites[entity_offset].texture =
                        {plan.animations[animation.clip_index].asset_index, 1U};
#endif
                    snapshot.sprites[entity_offset].uv =
                        animation_plan.frames[animation.frame_index].uv;
                } else if (group.has_sprite) {
                    if (animation.clip_index != 0U || animation.frame_index != 0U ||
                        animation.ticks_remaining != 0U || animation.direction != 1 ||
                        animation.playing || animation.completion_emitted) {
                        return std::unexpected(SaveReader::failure_diagnostic());
                    }
#if defined(AI2D_ENABLE_GPU)
                    snapshot.sprites[entity_offset].texture = textures[group.sprite.asset_index];
#else
                    snapshot.sprites[entity_offset].texture = {group.sprite.asset_index, 1U};
#endif
                    snapshot.sprites[entity_offset].uv = group.sprite.uv;
                } else if (animation.clip_index != 0U || animation.frame_index != 0U ||
                           animation.ticks_remaining != 0U || animation.direction != 1 ||
                           animation.playing || animation.completion_emitted) {
                    return std::unexpected(SaveReader::failure_diagnostic());
                }
            }
        }
        if (auto count = reader.expect_count(snapshot.system_states.size()); !count) return count;
        for (std::size_t system_index = 0U; system_index < snapshot.system_states.size(); ++system_index) {
            auto& system = snapshot.system_states[system_index];
            auto current = reader.u8();
            if (!current) return std::unexpected(std::move(current.error()));
            auto queued = reader.u8();
            if (!queued) return std::unexpected(std::move(queued.error()));
            auto phase = reader.u32();
            if (!phase) return std::unexpected(std::move(phase.error()));
            auto stepped = reader.boolean();
            if (!stepped) return std::unexpected(std::move(stepped.error()));
            if (*current > static_cast<std::uint8_t>(GameDirection::right) ||
                *queued > static_cast<std::uint8_t>(GameDirection::right)) {
                return std::unexpected(SaveReader::failure_diagnostic());
            }
            system = {static_cast<GameDirection>(*current), static_cast<GameDirection>(*queued),
                      *phase, *stepped != 0U};
            const auto& system_plan = scene.systems[system_index];
            if (system_plan.operation == GameOperationId::grid_motion) {
                if (system.current_direction == GameDirection::none ||
                    system.phase >= system_plan.step_interval_ticks ||
                    (system.stepped && system.phase != 0U)) {
                    return std::unexpected(SaveReader::failure_diagnostic());
                }
            } else if (system.current_direction != GameDirection::none ||
                       system.queued_direction != GameDirection::none || system.phase != 0U ||
                       system.stepped) {
                return std::unexpected(SaveReader::failure_diagnostic());
            }
        }
        if (auto count = reader.expect_count(snapshot.rule_invocations.size()); !count) return count;
        for (auto& invocation : snapshot.rule_invocations) {
            auto value = reader.u64();
            if (!value) return std::unexpected(std::move(value.error()));
            if (*value == std::numeric_limits<std::uint64_t>::max()) {
                return std::unexpected(SaveReader::failure_diagnostic());
            }
            invocation = *value;
        }
        if (auto count = reader.expect_count(snapshot.pool_states.size()); !count) return count;
        for (std::uint32_t pool_index = 0U; pool_index < snapshot.pool_states.size(); ++pool_index) {
            auto& pool = snapshot.pool_states[pool_index];
            if (auto count = reader.expect_count(pool.free_ring.size()); !count) return count;
            for (auto& value : pool.free_ring) {
                auto parsed = reader.u32();
                if (!parsed) return std::unexpected(std::move(parsed.error()));
                value = *parsed;
            }
            for (auto& value : pool.active_next) {
                auto parsed = reader.u32();
                if (!parsed) return std::unexpected(std::move(parsed.error()));
                value = *parsed;
            }
            for (auto& value : pool.active_previous) {
                auto parsed = reader.u32();
                if (!parsed) return std::unexpected(std::move(parsed.error()));
                value = *parsed;
            }
            for (auto& value : pool.expiration_ticks) {
                auto parsed = reader.u64();
                if (!parsed) return std::unexpected(std::move(parsed.error()));
                value = *parsed;
            }
            for (auto& value : pool.acquired) {
                auto parsed = reader.boolean();
                if (!parsed) return std::unexpected(std::move(parsed.error()));
                value = *parsed;
            }
            auto free_head = reader.u32();
            if (!free_head) return std::unexpected(std::move(free_head.error()));
            auto free_count = reader.u32();
            if (!free_count) return std::unexpected(std::move(free_count.error()));
            auto active_head = reader.u32();
            if (!active_head) return std::unexpected(std::move(active_head.error()));
            auto active_tail = reader.u32();
            if (!active_tail) return std::unexpected(std::move(active_tail.error()));
            auto active_count = reader.u32();
            if (!active_count) return std::unexpected(std::move(active_count.error()));
            pool.free_head = *free_head;
            pool.free_count = *free_count;
            pool.active_head = *active_head;
            pool.active_tail = *active_tail;
            pool.active_count = *active_count;
            if (!saved_pool_storage_valid(
                    scene, pool_index, pool, snapshot.entity_states, snapshot.scene_tick)) {
                return std::unexpected(SaveReader::failure_diagnostic());
            }
        }
        auto contact_count = reader.u32();
        if (!contact_count) return std::unexpected(std::move(contact_count.error()));
        if (*contact_count > snapshot.contacts.capacity()) return std::unexpected(SaveReader::failure_diagnostic());
        snapshot.contacts.clear();
        for (std::uint32_t index = 0U; index < *contact_count; ++index) {
            auto rule = reader.u32();
            if (!rule) return std::unexpected(std::move(rule.error()));
            auto a = reader.u32();
            if (!a) return std::unexpected(std::move(a.error()));
            auto b = reader.u32();
            if (!b) return std::unexpected(std::move(b.error()));
            if (*rule >= scene.collision_rules.size() || *a >= scene.total_spawn_count ||
                *b >= scene.total_spawn_count || *a == *b) {
                return std::unexpected(SaveReader::failure_diagnostic());
            }
            const auto& rule_plan = scene.collision_rules[*rule];
            const auto group_a_index = save_validation_entity_groups[*a];
            const auto group_b_index = save_validation_entity_groups[*b];
            if (rule_plan.interaction != GameCollisionInteraction::trigger ||
                group_a_index >= scene.spawn_groups.size() || group_b_index >= scene.spawn_groups.size() ||
                !scene.spawn_groups[group_a_index].has_collider ||
                !scene.spawn_groups[group_b_index].has_collider ||
                !snapshot.entity_states[*a].active || !snapshot.entity_states[*b].active ||
                !snapshot.colliders[*a].enabled || !snapshot.colliders[*b].enabled ||
                snapshot.colliders[*a].group != rule_plan.group_a ||
                snapshot.colliders[*b].group != rule_plan.group_b) {
                return std::unexpected(SaveReader::failure_diagnostic());
            }
            const auto center_a = Vec2{
                snapshot.transforms[*a].position.x + snapshot.colliders[*a].offset.x,
                snapshot.transforms[*a].position.y + snapshot.colliders[*a].offset.y,
            };
            const auto center_b = Vec2{
                snapshot.transforms[*b].position.x + snapshot.colliders[*b].offset.x,
                snapshot.transforms[*b].position.y + snapshot.colliders[*b].offset.y,
            };
            if (std::abs(center_a.x - center_b.x) >
                    snapshot.colliders[*a].half_extent.x + snapshot.colliders[*b].half_extent.x ||
                std::abs(center_a.y - center_b.y) >
                    snapshot.colliders[*a].half_extent.y + snapshot.colliders[*b].half_extent.y) {
                return std::unexpected(SaveReader::failure_diagnostic());
            }
            if (!snapshot.contacts.empty()) {
                const auto& previous = snapshot.contacts.back();
                const bool ordered = previous.rule_index < *rule ||
                    (previous.rule_index == *rule && previous.entity_a_index < *a) ||
                    (previous.rule_index == *rule && previous.entity_a_index == *a &&
                     previous.entity_b_index < *b);
                if (!ordered) return std::unexpected(SaveReader::failure_diagnostic());
            }
            snapshot.contacts.push_back({*rule, *a, *b});
        }
        if (auto count = reader.expect_count(snapshot.tile_layers.size()); !count) return count;
        for (std::size_t layer_index = 0U; layer_index < snapshot.tile_layers.size(); ++layer_index) {
            auto& layer = snapshot.tile_layers[layer_index];
            if (auto count = reader.expect_count(layer.size()); !count) return count;
            const auto atlas_capacity =
                scene.tile_layers[layer_index].atlas_columns * scene.tile_layers[layer_index].atlas_rows;
            for (auto& value : layer) {
                auto parsed = reader.u32();
                if (!parsed || *parsed > atlas_capacity) {
                    return std::unexpected(SaveReader::failure_diagnostic());
                }
                value = static_cast<std::uint16_t>(*parsed);
            }
        }
        if (auto count = reader.expect_count(snapshot.fields.size()); !count) return count;
        for (std::size_t field_index = 0U; field_index < snapshot.fields.size(); ++field_index) {
            auto& field = snapshot.fields[field_index];
            if (auto count = reader.expect_count(field.size()); !count) return count;
            for (auto& value : field) {
                auto parsed = reader.i32();
                if (!parsed || *parsed < scene.fields[field_index].minimum ||
                    *parsed > scene.fields[field_index].maximum) return std::unexpected(SaveReader::failure_diagnostic());
                value = *parsed;
            }
        }
        if (auto count = reader.expect_count(snapshot.particle_emitters.size()); !count) return count;
        for (auto& emitter : snapshot.particle_emitters) {
            if (auto count = reader.expect_count(emitter.slots.size()); !count) return count;
            auto order = reader.u64();
            if (!order) return std::unexpected(std::move(order.error()));
            auto emissions = reader.u64();
            if (!emissions) return std::unexpected(std::move(emissions.error()));
            auto active_count = reader.u32();
            if (!active_count || *active_count > emitter.slots.size()) return std::unexpected(SaveReader::failure_diagnostic());
            emitter.next_acquisition_order = *order;
            emitter.emission_count = *emissions;
            emitter.active_count = *active_count;
            std::uint32_t observed_active = 0U;
            std::uint64_t maximum_acquisition_order = 0U;
            for (auto& particle : emitter.slots) {
                auto position = deserialize_vec2(reader);
                if (!position) return std::unexpected(std::move(position.error()));
                auto previous = deserialize_vec2(reader);
                if (!previous) return std::unexpected(std::move(previous.error()));
                auto velocity = deserialize_vec2(reader);
                if (!velocity) return std::unexpected(std::move(velocity.error()));
                auto expiration = reader.u64();
                if (!expiration) return std::unexpected(std::move(expiration.error()));
                auto acquisition = reader.u64();
                if (!acquisition) return std::unexpected(std::move(acquisition.error()));
                auto active = reader.boolean();
                if (!active) return std::unexpected(std::move(active.error()));
                particle = {*position, *previous, *velocity, *expiration, *acquisition, *active != 0U};
                if (particle.active) {
                    if (particle.expiration_tick < snapshot.scene_tick) {
                        return std::unexpected(SaveReader::failure_diagnostic());
                    }
                    maximum_acquisition_order = std::max(maximum_acquisition_order, particle.acquisition_order);
                    ++observed_active;
                }
            }
            if (observed_active != emitter.active_count ||
                emitter.next_acquisition_order == std::numeric_limits<std::uint64_t>::max() ||
                emitter.emission_count == std::numeric_limits<std::uint64_t>::max() ||
                emitter.emission_count != emitter.next_acquisition_order ||
                (observed_active != 0U && emitter.next_acquisition_order <= maximum_acquisition_order) ||
                save_validation_particle_slots.size() < emitter.slots.size()) {
                return std::unexpected(SaveReader::failure_diagnostic());
            }
            emitter.free_heap.clear();
            std::fill(
                emitter.active_next.begin(), emitter.active_next.end(),
                ParticleEmitterRuntimeState::invalid_slot);
            std::fill(
                emitter.active_previous.begin(), emitter.active_previous.end(),
                ParticleEmitterRuntimeState::invalid_slot);
            emitter.active_head = ParticleEmitterRuntimeState::invalid_slot;
            emitter.active_tail = ParticleEmitterRuntimeState::invalid_slot;
            std::uint32_t active_slot_count = 0U;
            for (std::uint32_t slot = 0U; slot < emitter.slots.size(); ++slot) {
                if (emitter.slots[slot].active) {
                    save_validation_particle_slots[active_slot_count++] = slot;
                } else if (auto released = push_particle_free(emitter, slot, nullptr); !released) {
                    return std::unexpected(SaveReader::failure_diagnostic());
                }
            }
            std::sort(
                save_validation_particle_slots.begin(),
                save_validation_particle_slots.begin() + active_slot_count,
                [&](const std::uint32_t left, const std::uint32_t right) {
                    return emitter.slots[left].acquisition_order < emitter.slots[right].acquisition_order;
                });
            for (std::uint32_t order_index = 0U; order_index < active_slot_count; ++order_index) {
                const auto slot = save_validation_particle_slots[order_index];
                if (order_index != 0U &&
                    emitter.slots[save_validation_particle_slots[order_index - 1U]].acquisition_order ==
                        emitter.slots[slot].acquisition_order) {
                    return std::unexpected(SaveReader::failure_diagnostic());
                }
                emitter.active_previous[slot] = emitter.active_tail;
                if (emitter.active_tail == ParticleEmitterRuntimeState::invalid_slot) {
                    emitter.active_head = slot;
                } else {
                    emitter.active_next[emitter.active_tail] = slot;
                }
                emitter.active_tail = slot;
            }
        }
        auto camera_position = deserialize_vec2(reader);
        if (!camera_position) return std::unexpected(std::move(camera_position.error()));
        auto camera_extent = deserialize_vec2(reader);
        if (!camera_extent) return std::unexpected(std::move(camera_extent.error()));
        auto zoom = reader.number();
        if (!zoom) return std::unexpected(std::move(zoom.error()));
        auto shake = reader.number();
        if (!shake) return std::unexpected(std::move(shake.error()));
        auto shake_ticks = reader.u32();
        if (!shake_ticks) return std::unexpected(std::move(shake_ticks.error()));
        auto shake_invocation = reader.u64();
        if (!shake_invocation) return std::unexpected(std::move(shake_invocation.error()));
        float maximum_shake_amplitude = 0.0F;
        std::uint32_t maximum_shake_ticks = 0U;
        for (const auto& rule : scene.rules) {
            for (const auto& action : rule.actions) {
                if (action.kind != GameRuleActionKind::camera_shake) continue;
                maximum_shake_amplitude = std::max(maximum_shake_amplitude, action.scalar);
                maximum_shake_ticks = std::max(maximum_shake_ticks, action.duration_ticks);
            }
        }
        if (camera_extent->x <= 0.0F || camera_extent->y <= 0.0F || *zoom < 0.1F || *zoom > 10.0F ||
            *shake < 0.0F || *shake > maximum_shake_amplitude ||
            *shake_ticks > maximum_shake_ticks ||
            *shake_invocation == std::numeric_limits<std::uint64_t>::max()) {
            return std::unexpected(SaveReader::failure_diagnostic());
        }
        snapshot.camera = {*camera_position, *camera_extent, *zoom, *shake, *shake_ticks, *shake_invocation};
        return {};
    }

    [[nodiscard]] std::filesystem::path save_file_path(const std::uint32_t slot) const {
        return save_directory / ("slot-" + std::to_string(slot) + ".ai2dsave");
    }

    Result<std::uint64_t> write_save_file(const std::uint32_t slot) {
        if (!plan.save.enabled || slot >= plan.save.slot_count ||
            active_scene >= plan.scenes.size() || !plan.scenes[active_scene].persistent) {
            return std::unexpected(runtime_error(
                DiagnosticCode::game_save_invalid,
                "Save request requires a persistent current scene and a valid slot"));
        }
        if (auto captured = capture_active_scene(); !captured) {
            return std::unexpected(std::move(captured.error()));
        }
        save_buffer.clear();
        constexpr std::array<std::uint8_t, 8U> magic{'A', 'I', '2', 'D', 'S', 'V', '0', '6'};
        for (const auto byte : magic) append_u8(save_buffer, byte);
        append_u32(save_buffer, 1U);
        append_u64(save_buffer, plan.plan_hash);
        const auto payload_length_offset = save_buffer.size();
        append_u64(save_buffer, 0U);
        const auto checksum_offset = save_buffer.size();
        append_u64(save_buffer, 0U);
        const auto payload_offset = save_buffer.size();
        append_u32(save_buffer, static_cast<std::uint32_t>(plan.save.state_indices.size()));
        for (const auto state_index : plan.save.state_indices) {
            append_u32(save_buffer, state_index);
            append_i32(save_buffer, states[state_index]);
        }
        append_u32(save_buffer, active_scene);
        append_u32(save_buffer, active_locale_index);
        append_u32(save_buffer, active_input_profile_index);
        append_u64(save_buffer, simulation_tick);
        append_u32(save_buffer, static_cast<std::uint32_t>(plan.scenes.size()));
        for (std::uint32_t scene_index = 0U; scene_index < plan.scenes.size(); ++scene_index) {
            const bool included = plan.scenes[scene_index].persistent && scene_snapshots[scene_index].retained;
            append_u8(save_buffer, included ? 1U : 0U);
            if (included) {
                if (auto serialized = serialize_scene_snapshot(
                        save_buffer, scene_index, scene_snapshots[scene_index]);
                    !serialized) {
                    return std::unexpected(std::move(serialized.error()));
                }
            }
            if (save_buffer.size() > save_capacity_limit) {
                return std::unexpected(runtime_error(
                    DiagnosticCode::runtime_save_state_invalid,
                    "Save payload exceeded its precomputed capacity"));
            }
        }
        const auto payload_size = save_buffer.size() - payload_offset;
        const auto checksum = checksum_bytes(std::span<const std::uint8_t>{save_buffer}.subspan(payload_offset));
        const auto overwrite_u64 = [&](const std::size_t offset, const std::uint64_t value) {
            for (std::uint32_t byte = 0U; byte < 8U; ++byte) {
                save_buffer[offset + byte] = static_cast<std::uint8_t>(value >> (byte * 8U));
            }
        };
        overwrite_u64(payload_length_offset, payload_size);
        overwrite_u64(checksum_offset, checksum);
        std::error_code error{};
        std::filesystem::create_directories(save_directory, error);
        if (error) {
            return std::unexpected(runtime_error(
                DiagnosticCode::runtime_save_state_invalid,
                "Save directory could not be created"));
        }
        if (save_attempt_sequence == std::numeric_limits<std::uint64_t>::max()) {
            return std::unexpected(runtime_error(
                DiagnosticCode::runtime_save_state_invalid,
                "Save transaction sequence was exhausted"));
        }
        const auto destination = save_file_path(slot);
        auto temporary = destination;
        temporary += ".tmp." + std::to_string(process_identifier()) + "." +
                     std::to_string(reinterpret_cast<std::uintptr_t>(this)) + "." +
                     std::to_string(save_attempt_sequence++);
        {
            std::ofstream stream{temporary, std::ios::binary | std::ios::trunc};
            if (!stream) {
                return std::unexpected(runtime_error(
                    DiagnosticCode::runtime_save_state_invalid,
                    "Temporary save file could not be opened"));
            }
            stream.write(
                reinterpret_cast<const char*>(save_buffer.data()),
                static_cast<std::streamsize>(save_buffer.size()));
            stream.flush();
            if (!stream) {
                std::filesystem::remove(temporary, error);
                return std::unexpected(runtime_error(
                    DiagnosticCode::runtime_save_state_invalid,
                    "Temporary save file could not be written"));
            }
        }
#if defined(_WIN32)
        bool replaced = false;
        for (std::uint32_t attempt = 0U; attempt < 16U; ++attempt) {
            if (MoveFileExW(
                    temporary.c_str(), destination.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                replaced = true;
                break;
            }
            const auto failure = GetLastError();
            if (failure != ERROR_ACCESS_DENIED && failure != ERROR_SHARING_VIOLATION) break;
            Sleep(1U << std::min(attempt, 4U));
        }
        if (!replaced) {
            std::filesystem::remove(temporary, error);
            return std::unexpected(runtime_error(
                DiagnosticCode::runtime_save_state_invalid,
                "Atomic save replacement failed"));
        }
#else
        std::filesystem::rename(temporary, destination, error);
        if (error) {
            std::filesystem::remove(temporary, error);
            return std::unexpected(runtime_error(
                DiagnosticCode::runtime_save_state_invalid,
                "Atomic save replacement failed"));
        }
#endif
        return static_cast<std::uint64_t>(save_buffer.size());
    }

    Result<std::uint64_t> load_save_file(const std::uint32_t slot) {
        if (!plan.save.enabled || slot >= plan.save.slot_count) {
            return std::unexpected(runtime_error(
                DiagnosticCode::game_save_invalid, "Load request slot is invalid"));
        }
        const auto path = save_file_path(slot);
        std::error_code error{};
        if (!std::filesystem::is_regular_file(path, error) || error) {
            return std::unexpected(runtime_error(
                DiagnosticCode::runtime_save_state_invalid, "Save file does not exist"));
        }
        const auto size = std::filesystem::file_size(path, error);
        if (error || size < 36U || size > load_buffer.capacity()) {
            return std::unexpected(runtime_error(
                DiagnosticCode::runtime_save_state_invalid,
                "Save file size is invalid"));
        }
        load_buffer.resize(static_cast<std::size_t>(size));
        {
            std::ifstream stream{path, std::ios::binary};
            if (!stream) {
                return std::unexpected(runtime_error(
                    DiagnosticCode::runtime_save_state_invalid,
                    "Save file could not be opened"));
            }
            stream.read(
                reinterpret_cast<char*>(load_buffer.data()),
                static_cast<std::streamsize>(load_buffer.size()));
            if (!stream || stream.peek() != std::ifstream::traits_type::eof()) {
                return std::unexpected(runtime_error(
                    DiagnosticCode::runtime_save_state_invalid,
                    "Save file could not be read completely"));
            }
        }
        SaveReader reader{load_buffer};
        constexpr std::array<std::uint8_t, 8U> magic{'A', 'I', '2', 'D', 'S', 'V', '0', '6'};
        for (const auto expected : magic) {
            auto actual = reader.u8();
            if (!actual) return std::unexpected(std::move(actual.error()));
            if (*actual != expected) return std::unexpected(SaveReader::failure_diagnostic());
        }
        auto version = reader.u32();
        if (!version) return std::unexpected(std::move(version.error()));
        auto plan_hash = reader.u64();
        if (!plan_hash) return std::unexpected(std::move(plan_hash.error()));
        auto payload_size = reader.u64();
        if (!payload_size) return std::unexpected(std::move(payload_size.error()));
        auto expected_checksum = reader.u64();
        if (!expected_checksum) return std::unexpected(std::move(expected_checksum.error()));
        if (*version != 1U || *plan_hash != plan.plan_hash || *payload_size != load_buffer.size() - reader.offset ||
            *expected_checksum != checksum_bytes(std::span<const std::uint8_t>{load_buffer}.subspan(reader.offset))) {
            return std::unexpected(runtime_error(
                DiagnosticCode::runtime_save_state_invalid,
                "Save header, plan hash, length, or checksum is invalid"));
        }
        load_state_scratch = states;
        auto persistent_state_count = reader.u32();
        if (!persistent_state_count || *persistent_state_count != plan.save.state_indices.size()) {
            return std::unexpected(SaveReader::failure_diagnostic());
        }
        for (std::size_t index = 0U; index < plan.save.state_indices.size(); ++index) {
            auto state_index = reader.u32();
            if (!state_index) return std::unexpected(std::move(state_index.error()));
            auto value = reader.i32();
            if (!value) return std::unexpected(std::move(value.error()));
            if (*state_index != plan.save.state_indices[index] || *value < plan.states[*state_index].minimum ||
                *value > plan.states[*state_index].maximum) {
                return std::unexpected(SaveReader::failure_diagnostic());
            }
            load_state_scratch[*state_index] = *value;
        }
        auto saved_scene = reader.u32();
        if (!saved_scene || *saved_scene >= plan.scenes.size() || !plan.scenes[*saved_scene].persistent) {
            return std::unexpected(SaveReader::failure_diagnostic());
        }
        auto saved_locale = reader.u32();
        if (!saved_locale || (!plan.localizations.empty() && *saved_locale >= plan.localizations.size()) ||
            (plan.localizations.empty() && *saved_locale != 0U)) {
            return std::unexpected(SaveReader::failure_diagnostic());
        }
        auto saved_profile = reader.u32();
        if (!saved_profile || (!plan.input_profiles.empty() && *saved_profile >= plan.input_profiles.size()) ||
            (plan.input_profiles.empty() && *saved_profile != 0U)) {
            return std::unexpected(SaveReader::failure_diagnostic());
        }
        auto saved_simulation_tick = reader.u64();
        if (!saved_simulation_tick) return std::unexpected(std::move(saved_simulation_tick.error()));
        if (*saved_simulation_tick == std::numeric_limits<std::uint64_t>::max()) {
            return std::unexpected(SaveReader::failure_diagnostic());
        }
        if (auto count = reader.expect_count(plan.scenes.size()); !count) {
            return std::unexpected(std::move(count.error()));
        }
        bool saved_scene_included = false;
        for (std::uint32_t scene_index = 0U; scene_index < plan.scenes.size(); ++scene_index) {
            auto included = reader.boolean();
            if (!included) return std::unexpected(std::move(included.error()));
            load_scene_snapshots[scene_index].retained = false;
            if (*included != 0U) {
                if (!plan.scenes[scene_index].persistent) return std::unexpected(SaveReader::failure_diagnostic());
                if (auto parsed = deserialize_scene_snapshot(
                        reader, scene_index, load_scene_snapshots[scene_index]);
                    !parsed) {
                    return std::unexpected(std::move(parsed.error()));
                }
                if (!load_scene_snapshots[scene_index].retained) return std::unexpected(SaveReader::failure_diagnostic());
                saved_scene_included = saved_scene_included || scene_index == *saved_scene;
            }
        }
        if (!saved_scene_included || reader.offset != load_buffer.size()) {
            return std::unexpected(SaveReader::failure_diagnostic());
        }
        for (std::size_t scene_index = 0U; scene_index < load_scene_snapshots.size(); ++scene_index) {
            if (load_scene_snapshots[scene_index].retained &&
                load_scene_snapshots[scene_index].scene_tick > *saved_simulation_tick) {
                return std::unexpected(SaveReader::failure_diagnostic());
            }
        }
        const auto previous_locale = active_locale_index;
        const auto previous_profile = active_input_profile_index;
        const auto previous_simulation_tick = simulation_tick;
        states.swap(load_state_scratch);
        scene_snapshots.swap(load_scene_snapshots);
        active_locale_index = *saved_locale;
        active_input_profile_index = *saved_profile;
        simulation_tick = *saved_simulation_tick;
        if (auto loaded = load_scene(*saved_scene, true); !loaded) {
            scene_snapshots.swap(load_scene_snapshots);
            states.swap(load_state_scratch);
            active_locale_index = previous_locale;
            active_input_profile_index = previous_profile;
            simulation_tick = previous_simulation_tick;
            return std::unexpected(std::move(loaded.error()));
        }
        return static_cast<std::uint64_t>(load_buffer.size());
    }

    Result<void> delete_save_file(const std::uint32_t slot) {
        if (!plan.save.enabled || slot >= plan.save.slot_count) {
            return std::unexpected(runtime_error(
                DiagnosticCode::game_save_invalid, "Delete request slot is invalid"));
        }
        std::error_code error{};
        const bool removed = std::filesystem::remove(save_file_path(slot), error);
        if (error || !removed) {
            return std::unexpected(runtime_error(
                DiagnosticCode::runtime_save_state_invalid,
                "Save file could not be deleted"));
        }
        return {};
    }

    Result<void> process_pending_save(GameRuntimeFrameMetrics& metrics) {
        if (pending_save.kind == SaveRequestKind::none) return {};
        const auto request = pending_save;
        pending_save = {};
        ++metrics.save_attempts;
        Result<std::uint64_t> result = std::uint64_t{0U};
        if (request.kind == SaveRequestKind::save) result = write_save_file(request.slot);
        else if (request.kind == SaveRequestKind::load) result = load_save_file(request.slot);
        else {
            auto erased = delete_save_file(request.slot);
            if (!erased) result = std::unexpected(std::move(erased.error()));
        }
        if (!result) {
            ++metrics.save_failures;
            if (request.has_result_state) set_state_value(request.result_state_index, 0);
            warn_once(std::move(result.error()));
            return {};
        }
        ++metrics.save_successes;
        metrics.save_bytes += *result;
        if (request.has_result_state) set_state_value(request.result_state_index, 1);
        return {};
    }

    void advance_music_fade() noexcept {
        if (!music_stream.playing || music_stream.fade_ticks_remaining == 0U) return;
        const auto elapsed = music_stream.fade_total_ticks - music_stream.fade_ticks_remaining + 1U;
        const auto alpha = static_cast<float>(elapsed) /
                           static_cast<float>(std::max(1U, music_stream.fade_total_ticks));
        music_stream.current_volume =
            music_stream.fade_start_volume +
            (music_stream.target_volume - music_stream.fade_start_volume) * alpha;
        --music_stream.fade_ticks_remaining;
    }

#if defined(AI2D_ENABLE_GPU)
    static std::uint16_t music_u16(const std::uint8_t* const bytes) noexcept {
        return static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(bytes[0]) |
            static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[1]) << 8U));
    }

    static std::uint32_t music_u32(const std::uint8_t* const bytes) noexcept {
        return static_cast<std::uint32_t>(bytes[0]) |
               (static_cast<std::uint32_t>(bytes[1]) << 8U) |
               (static_cast<std::uint32_t>(bytes[2]) << 16U) |
               (static_cast<std::uint32_t>(bytes[3]) << 24U);
    }

    Result<void> open_music_stream(const MusicRequest& request) {
        if (request.asset_index >= plan.assets.size() ||
            plan.assets[request.asset_index].kind != GameAssetKind::music) {
            return std::unexpected(runtime_error(
                DiagnosticCode::game_presentation_invalid,
                "Music request references an invalid asset"));
        }
        const auto& path = plan.assets[request.asset_index].path;
        auto inspected = preflight_wav_asset(path);
        if (!inspected) return std::unexpected(std::move(inspected.error()));
        music_stream.file.close();
        music_stream.file.clear();
        music_stream.file.open(path, std::ios::binary);
        if (!music_stream.file) {
            auto diagnostic = runtime_error(
                DiagnosticCode::asset_decode_failed, "Music stream could not be opened");
            diagnostic.context.push_back({"path", path.string()});
            return std::unexpected(std::move(diagnostic));
        }
        const auto source_size = inspected->source_bytes;
        std::array<std::uint8_t, 12U> riff{};
        if (!music_stream.file.read(
                reinterpret_cast<char*>(riff.data()), static_cast<std::streamsize>(riff.size())) ||
            std::string_view{reinterpret_cast<const char*>(riff.data()), 4U} != "RIFF" ||
            std::string_view{reinterpret_cast<const char*>(riff.data() + 8U), 4U} != "WAVE") {
            return std::unexpected(runtime_error(
                DiagnosticCode::asset_decode_failed, "Music stream has an invalid RIFF header"));
        }
        bool found_format = false;
        bool found_data = false;
        std::uint16_t block_alignment = 0U;
        std::uint32_t sample_rate = 0U;
        std::uint64_t offset = 12U;
        while (offset + 8U <= source_size) {
            music_stream.file.clear();
            music_stream.file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
            std::array<std::uint8_t, 8U> chunk{};
            if (!music_stream.file.read(
                    reinterpret_cast<char*>(chunk.data()), static_cast<std::streamsize>(chunk.size()))) {
                return std::unexpected(runtime_error(
                    DiagnosticCode::asset_decode_failed, "Music chunk header could not be read"));
            }
            const auto chunk_size = music_u32(chunk.data() + 4U);
            const auto payload = offset + 8U;
            if (payload > source_size || chunk_size > source_size - payload) {
                return std::unexpected(runtime_error(
                    DiagnosticCode::asset_decode_failed, "Music chunk exceeds the source boundary"));
            }
            const auto chunk_id = std::string_view{reinterpret_cast<const char*>(chunk.data()), 4U};
            if (chunk_id == "fmt ") {
                if (found_format || chunk_size < 16U) {
                    return std::unexpected(runtime_error(
                        DiagnosticCode::asset_decode_failed, "Music format chunk is invalid"));
                }
                music_stream.file.seekg(static_cast<std::streamoff>(payload), std::ios::beg);
                std::array<std::uint8_t, 16U> format{};
                if (!music_stream.file.read(
                        reinterpret_cast<char*>(format.data()), static_cast<std::streamsize>(format.size()))) {
                    return std::unexpected(runtime_error(
                        DiagnosticCode::asset_decode_failed, "Music format chunk could not be read"));
                }
                music_stream.format = music_u16(format.data());
                music_stream.channels = music_u16(format.data() + 2U);
                sample_rate = music_u32(format.data() + 4U);
                block_alignment = music_u16(format.data() + 12U);
                music_stream.bits_per_sample = music_u16(format.data() + 14U);
                found_format = true;
            } else if (chunk_id == "data") {
                if (found_data) {
                    return std::unexpected(runtime_error(
                        DiagnosticCode::asset_decode_failed, "Music contains multiple data chunks"));
                }
                music_stream.data_offset = payload;
                music_stream.data_bytes = chunk_size;
                found_data = true;
            }
            offset = payload + chunk_size + (chunk_size & 1U);
        }
        const auto sample_bytes = music_stream.bits_per_sample / 8U;
        const auto expected_alignment = static_cast<std::uint32_t>(music_stream.channels) * sample_bytes;
        const bool format_supported =
            (music_stream.format == 1U && music_stream.bits_per_sample == 16U) ||
            (music_stream.format == 3U && music_stream.bits_per_sample == 32U);
        if (!found_format || !found_data || !format_supported ||
            (music_stream.channels != 1U && music_stream.channels != 2U) ||
            sample_rate != WavePcmF32::sample_rate || block_alignment != expected_alignment ||
            music_stream.data_bytes == 0U || music_stream.data_bytes % block_alignment != 0U) {
            auto diagnostic = runtime_error(
                DiagnosticCode::asset_decode_failed,
                "Streaming music must be 48 kHz mono/stereo PCM16 or float32 WAV");
            diagnostic.context.push_back({"path", path.string()});
            return std::unexpected(std::move(diagnostic));
        }
        music_stream.asset_index = request.asset_index;
        music_stream.cursor_bytes = 0U;
        music_stream.current_volume = request.fade_ticks == 0U ? request.volume : 0.0F;
        music_stream.fade_start_volume = music_stream.current_volume;
        music_stream.target_volume = request.volume;
        music_stream.fade_total_ticks = request.fade_ticks;
        music_stream.fade_ticks_remaining = request.fade_ticks;
        music_stream.playing = true;
        music_stream.loop = request.loop;
        music_stream.stop_after_fade = false;
        music_stream.source_exhausted = false;
        music_stream.output_started = false;
        music_stream.last_underruns = audio.music_underruns();
        return {};
    }

    Result<void> pump_music_stream(GameRuntimeFrameMetrics& metrics) {
        if (!music_stream.playing || !audio_available) return {};
        if (music_stream.source_exhausted) {
            if (audio.music_free_frames() == AudioMixer::music_buffer_capacity_frames) {
                if (auto stopped = audio.stop_music(); !stopped) return stopped;
                music_stream.playing = false;
                music_stream.output_started = false;
            }
            return {};
        }
        const auto sample_bytes = music_stream.bits_per_sample / 8U;
        const auto frame_bytes = static_cast<std::uint32_t>(music_stream.channels) * sample_bytes;
        while (music_stream.playing) {
            const auto free_frames = audio.music_free_frames();
            const auto buffer_frames = static_cast<std::uint32_t>(music_stream.sample_buffer.size() / 2U);
            if (free_frames == 0U || buffer_frames == 0U) break;
            if (music_stream.cursor_bytes >= music_stream.data_bytes) {
                if (music_stream.loop) {
                    music_stream.cursor_bytes = 0U;
                } else {
                    music_stream.source_exhausted = true;
                    break;
                }
            }
            const auto remaining_frames =
                (music_stream.data_bytes - music_stream.cursor_bytes) / frame_bytes;
            const auto frame_count = static_cast<std::uint32_t>(
                std::min<std::uint64_t>({remaining_frames, free_frames, buffer_frames}));
            if (frame_count == 0U) break;
            const auto byte_count = static_cast<std::size_t>(frame_count) * frame_bytes;
            if (byte_count > music_stream.source_buffer.size()) {
                return std::unexpected(runtime_error(
                    DiagnosticCode::internal_error, "Music source buffer capacity is invalid"));
            }
            music_stream.file.clear();
            music_stream.file.seekg(
                static_cast<std::streamoff>(music_stream.data_offset + music_stream.cursor_bytes),
                std::ios::beg);
            if (!music_stream.file.read(
                    reinterpret_cast<char*>(music_stream.source_buffer.data()),
                    static_cast<std::streamsize>(byte_count))) {
                return std::unexpected(runtime_error(
                    DiagnosticCode::asset_decode_failed, "Music stream data could not be read"));
            }
            for (std::uint32_t frame = 0U; frame < frame_count; ++frame) {
                for (std::uint32_t output_channel = 0U; output_channel < 2U; ++output_channel) {
                    const auto source_channel = std::min<std::uint32_t>(output_channel, music_stream.channels - 1U);
                    const auto source_offset = static_cast<std::size_t>(frame) * frame_bytes +
                                               static_cast<std::size_t>(source_channel) * sample_bytes;
                    float sample = 0.0F;
                    if (music_stream.format == 1U) {
                        const auto raw = music_u16(music_stream.source_buffer.data() + source_offset);
                        sample = static_cast<float>(std::bit_cast<std::int16_t>(raw)) / 32768.0F;
                    } else {
                        const auto raw = music_u32(music_stream.source_buffer.data() + source_offset);
                        sample = std::bit_cast<float>(raw);
                        if (!std::isfinite(sample)) {
                            return std::unexpected(runtime_error(
                                DiagnosticCode::asset_decode_failed,
                                "Music stream contains a non-finite sample"));
                        }
                    }
                    music_stream.sample_buffer[static_cast<std::size_t>(frame) * 2U + output_channel] =
                        std::clamp(sample, -1.0F, 1.0F);
                }
            }
            auto queued = audio.queue_music(std::span<const float>{
                music_stream.sample_buffer.data(), static_cast<std::size_t>(frame_count) * 2U});
            if (!queued) return std::unexpected(std::move(queued.error()));
            if (*queued != frame_count) {
                return std::unexpected(runtime_error(
                    DiagnosticCode::internal_error, "Music ring accepted an incomplete decoded block"));
            }
            music_stream.cursor_bytes += static_cast<std::uint64_t>(frame_count) * frame_bytes;
            metrics.music_stream_bytes += byte_count;
        }
        if (!music_stream.output_started &&
            audio.music_free_frames() < AudioMixer::music_buffer_capacity_frames) {
            if (auto started = audio.start_music(music_stream.current_volume); !started) return started;
            music_stream.output_started = true;
        }
        return {};
    }
#endif

    Result<void> process_pending_music(GameRuntimeFrameMetrics& metrics) {
#if defined(AI2D_ENABLE_GPU)
        if (!audio_available) {
            pending_music = {};
            return {};
        }
        if (pending_music.kind != MusicRequest::Kind::none) {
            const auto request = pending_music;
            pending_music = {};
            if (request.kind == MusicRequest::Kind::play) {
                if (music_stream.output_started) {
                    if (auto stopped = audio.stop_music(); !stopped) return stopped;
                }
                if (auto opened = open_music_stream(request); !opened) {
                    warn_once(std::move(opened.error()));
                    music_stream.playing = false;
                    music_stream.output_started = false;
                }
            } else if (request.kind == MusicRequest::Kind::stop && music_stream.playing) {
                if (request.fade_ticks == 0U) {
                    if (auto stopped = audio.stop_music(); !stopped) return stopped;
                    music_stream.playing = false;
                    music_stream.output_started = false;
                } else {
                    music_stream.fade_start_volume = music_stream.current_volume;
                    music_stream.target_volume = 0.0F;
                    music_stream.fade_total_ticks = request.fade_ticks;
                    music_stream.fade_ticks_remaining = request.fade_ticks;
                    music_stream.stop_after_fade = true;
                }
            } else if (request.kind == MusicRequest::Kind::volume && music_stream.playing) {
                music_stream.fade_start_volume = music_stream.current_volume;
                music_stream.target_volume = request.volume;
                music_stream.fade_total_ticks = request.fade_ticks;
                music_stream.fade_ticks_remaining = request.fade_ticks;
                music_stream.stop_after_fade = false;
                if (request.fade_ticks == 0U) music_stream.current_volume = request.volume;
            }
        }
        if (music_stream.playing && music_stream.stop_after_fade &&
            music_stream.fade_ticks_remaining == 0U && music_stream.current_volume <= 0.0F) {
            if (auto stopped = audio.stop_music(); !stopped) return stopped;
            music_stream.playing = false;
            music_stream.output_started = false;
        }
        if (music_stream.playing && music_stream.output_started) {
            if (auto volume = audio.set_music_volume(music_stream.current_volume); !volume) return volume;
        }
        if (auto pumped = pump_music_stream(metrics); !pumped) return pumped;
        const auto underruns = audio.music_underruns();
        if (underruns >= music_stream.last_underruns) {
            metrics.music_underruns += underruns - music_stream.last_underruns;
        }
        music_stream.last_underruns = underruns;
#else
        (void)metrics;
        pending_music = {};
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
        if (invocation == std::numeric_limits<std::uint64_t>::max()) {
            return std::unexpected(runtime_error(
                DiagnosticCode::runtime_numeric_state_invalid,
                "Rule invocation counter was exhausted"));
        }
        const auto random = mix_u64(plan.seed ^ rule_random_keys[rule_index] ^ invocation);
        ++invocation;
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
        case GameRuleActionKind::play_animation:
            return play_animation_action(action, collision_event, true);
        case GameRuleActionKind::stop_animation:
            return play_animation_action(action, collision_event, false);
        case GameRuleActionKind::set_tile: {
            const auto& layer = plan.scenes[active_scene].tile_layers[action.tile_layer_index];
            auto cell = resolve_cell_index(
                layer.grid_index, action.cell_source, action.cell_x, action.cell_y,
                action.cell_target, collision_event);
            if (!cell) return std::unexpected(std::move(cell.error()));
            if (!cell->has_value()) {
                if (action.has_result_state) set_state_value(action.result_state_index, 0);
                return {};
            }
            tile_layers[action.tile_layer_index][**cell] = static_cast<std::uint16_t>(action.tile_value);
            ++metrics.tile_writes;
            if (action.has_result_state) set_state_value(action.result_state_index, 1);
            return {};
        }
        case GameRuleActionKind::set_field:
        case GameRuleActionKind::add_field: {
            const auto& field_plan = plan.scenes[active_scene].fields[action.field_index];
            auto cell = resolve_cell_index(
                field_plan.grid_index, action.cell_source, action.cell_x, action.cell_y,
                action.cell_target, collision_event);
            if (!cell) return std::unexpected(std::move(cell.error()));
            if (!cell->has_value()) {
                if (action.has_result_state) set_state_value(action.result_state_index, 0);
                return {};
            }
            auto& destination = fields[action.field_index][**cell];
            const std::int64_t requested = action.kind == GameRuleActionKind::set_field
                                               ? action.value
                                               : static_cast<std::int64_t>(destination) + action.value;
            destination = static_cast<std::int32_t>(std::clamp<std::int64_t>(
                requested, field_plan.minimum, field_plan.maximum));
            ++metrics.field_writes;
            if (action.has_result_state) set_state_value(action.result_state_index, 1);
            return {};
        }
        case GameRuleActionKind::save_slot:
        case GameRuleActionKind::load_slot:
        case GameRuleActionKind::delete_slot:
            if (pending_save.kind != SaveRequestKind::none) {
                if (action.has_result_state) set_state_value(action.result_state_index, 0);
                ++metrics.save_failures;
                return {};
            }
            pending_save = {
                action.kind == GameRuleActionKind::save_slot ? SaveRequestKind::save
                    : action.kind == GameRuleActionKind::load_slot ? SaveRequestKind::load
                                                                  : SaveRequestKind::erase,
                action.save_slot,
                action.has_result_state,
                action.result_state_index,
            };
            return {};
        case GameRuleActionKind::camera_shake:
            if (camera_state.shake_invocation == std::numeric_limits<std::uint64_t>::max()) {
                return std::unexpected(runtime_error(
                    DiagnosticCode::runtime_numeric_state_invalid,
                    "Camera shake invocation counter was exhausted"));
            }
            camera_state.shake_amplitude = action.scalar;
            camera_state.shake_ticks_remaining = action.duration_ticks;
            ++camera_state.shake_invocation;
            return {};
        case GameRuleActionKind::set_camera_zoom:
            camera_state.zoom = action.scalar;
            return {};
        case GameRuleActionKind::set_locale:
            if (active_locale_index != action.locale_index) {
                active_locale_index = action.locale_index;
                ++metrics.locale_switches;
            }
            return {};
        case GameRuleActionKind::set_input_profile:
            if (active_input_profile_index != action.input_profile_index) {
                active_input_profile_index = action.input_profile_index;
                ++metrics.input_profile_switches;
            }
            return {};
        case GameRuleActionKind::play_music:
            pending_music = {
                MusicRequest::Kind::play, action.asset_index, action.fade_ticks,
                action.scalar, action.loop};
            return {};
        case GameRuleActionKind::stop_music:
            pending_music = {MusicRequest::Kind::stop, 0U, action.fade_ticks, 0.0F, false};
            return {};
        case GameRuleActionKind::set_music_volume:
            if (pending_music.kind == MusicRequest::Kind::play) {
                pending_music.fade_ticks = action.fade_ticks;
                pending_music.volume = action.scalar;
            } else {
                pending_music = {MusicRequest::Kind::volume, 0U, action.fade_ticks, action.scalar, false};
            }
            return {};
        case GameRuleActionKind::emit_particles:
            return emit_particles(action, collision_event, metrics);
        }
        return {};
    }

    Result<void> execute_rule(
        const std::uint32_t rule_index,
        const CollisionEvent2D* const collision_event,
        GameRuntimeFrameMetrics& metrics,
        EntityRecord* const event_entity = nullptr) {
        const auto& rule = plan.scenes[active_scene].rules[rule_index];
        auto* const previous_event_entity = current_rule_event_entity;
        current_rule_event_entity = event_entity;
        auto conditions = rule_conditions_match(rule, collision_event, metrics);
        if (!conditions) {
            current_rule_event_entity = previous_event_entity;
            return std::unexpected(std::move(conditions.error()));
        }
        if (!*conditions) {
            current_rule_event_entity = previous_event_entity;
            return {};
        }
        ++metrics.rule_executions;
        for (const auto& action : rule.actions) {
            ++metrics.action_executions;
            if (auto applied = apply_rule_action(rule_index, action, collision_event, metrics); !applied) {
                current_rule_event_entity = previous_event_entity;
                return applied;
            }
        }
        current_rule_event_entity = previous_event_entity;
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
            if (plan.schema_version == GameSchemaVersion::v0_5 ||
                plan.schema_version == GameSchemaVersion::v0_6) {
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
        if (simulation_tick == std::numeric_limits<std::uint64_t>::max() ||
            scene_tick == std::numeric_limits<std::uint64_t>::max()) {
            return std::unexpected(runtime_error(
                DiagnosticCode::runtime_numeric_state_invalid,
                "Fixed-tick counters were exhausted"));
        }
        if (auto expired = expire_pool_lifetimes(frame_metrics); !expired) {
            return std::unexpected(std::move(expired.error()));
        }
        if (auto particles = update_particles(delta_seconds, frame_metrics); !particles) {
            return std::unexpected(std::move(particles.error()));
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
        if (auto animations = update_animations(frame_metrics); !animations) {
            return std::unexpected(std::move(animations.error()));
        }
        if (auto camera = update_camera(frame_metrics); !camera) {
            return std::unexpected(std::move(camera.error()));
        }
        advance_music_fade();
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
        return {
            camera_state.position.x +
                (point.x / static_cast<float>(plan.window.virtual_width) * 2.0F - 1.0F) * camera_state.half_extent.x,
            -camera_state.position.y +
                (point.y / static_cast<float>(plan.window.virtual_height) * 2.0F - 1.0F) * camera_state.half_extent.y,
        };
    }

    Vec2 virtual_size_to_world(const Vec2 size) const noexcept {
        return {
            size.x / static_cast<float>(plan.window.virtual_width) * camera_state.half_extent.x * 2.0F,
            size.y / static_cast<float>(plan.window.virtual_height) * camera_state.half_extent.y * 2.0F,
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

    Result<bool> render(
        const double interpolation_alpha,
        std::uint32_t& extracted_count,
        GameRuntimeFrameMetrics& metrics) {
        if (options.headless) return false;
        extracted.clear();
        const auto& scene = plan.scenes[active_scene];
        for (std::size_t layer_index = 0U; layer_index < scene.tile_layers.size(); ++layer_index) {
            const auto& layer = scene.tile_layers[layer_index];
            if (!layer.visible) continue;
            const auto& grid = scene.grids[layer.grid_index];
            const auto& cells = tile_layers[layer_index];
            for (std::uint32_t row = 0U; row < grid.rows; ++row) {
                for (std::uint32_t column = 0U; column < grid.columns; ++column) {
                    const auto tile = cells[static_cast<std::size_t>(row) * grid.columns + column];
                    if (tile == 0U) continue;
                    if (extracted.size() >= GamePlan::max_render_submissions) {
                        return std::unexpected(runtime_error(
                            DiagnosticCode::render_upload_capacity_exceeded,
                            "Tile layers exceed the validated renderer submission capacity"));
                    }
                    const auto atlas_index = static_cast<std::uint32_t>(tile - 1U);
                    const auto atlas_column = atlas_index % layer.atlas_columns;
                    const auto atlas_row = atlas_index / layer.atlas_columns;
                    const Vec2 uv_size{
                        1.0F / static_cast<float>(layer.atlas_columns),
                        1.0F / static_cast<float>(layer.atlas_rows),
                    };
                    extracted.push_back({
                        {grid.first_cell_center.x + static_cast<float>(column) * grid.cell_size.x,
                         -(grid.first_cell_center.y + static_cast<float>(row) * grid.cell_size.y)},
                        grid.cell_size,
                        {0.5F, 0.5F},
                        0.0F,
                        {static_cast<float>(atlas_column) * uv_size.x,
                         static_cast<float>(atlas_row) * uv_size.y, uv_size.x, uv_size.y},
                        layer.tint,
                        textures[layer.asset_index],
                        layer.layer,
                        true,
                    });
                }
            }
        }
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
        for (std::size_t emitter_index = 0U; emitter_index < scene.particle_emitters.size(); ++emitter_index) {
            const auto& emitter_plan = scene.particle_emitters[emitter_index];
            for (const auto& particle : particle_emitters[emitter_index].slots) {
                if (!particle.active || !emitter_plan.sprite.visible) continue;
                if (extracted.size() >= GamePlan::max_render_submissions) {
                    return std::unexpected(runtime_error(
                        DiagnosticCode::render_upload_capacity_exceeded,
                        "Particles exceed the validated renderer submission capacity"));
                }
                const auto alpha = static_cast<float>(interpolation_alpha);
                const Vec2 position{
                    particle.previous_position.x + (particle.position.x - particle.previous_position.x) * alpha,
                    -(particle.previous_position.y + (particle.position.y - particle.previous_position.y) * alpha),
                };
                extracted.push_back({
                    position,
                    emitter_plan.sprite.size,
                    emitter_plan.sprite.pivot,
                    0.0F,
                    {emitter_plan.sprite.uv.min.x, emitter_plan.sprite.uv.min.y,
                     emitter_plan.sprite.uv.max.x, emitter_plan.sprite.uv.max.y},
                    emitter_plan.sprite.tint,
                    textures[emitter_plan.sprite.asset_index],
                    emitter_plan.sprite.layer,
                    true,
                });
            }
        }
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
                const auto& tokens = element.localized
                    ? localized_ui_text_tokens[active_locale_index][active_scene][element_index]
                    : ui_text_tokens[active_scene][element_index];
                if (element.localized) ++metrics.localized_text_resolutions;
                if (auto appended = append_text(element, tokens); !appended) {
                    return std::unexpected(std::move(appended.error()));
                }
            }
        }
        RenderFrame2D frame{};
        frame.camera = {
            {camera_state.position.x, -camera_state.position.y},
            camera_state.half_extent,
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
        InputSnapshot held_input = input;
        std::fill(std::begin(held_input.pressed_keys), std::end(held_input.pressed_keys), false);
        std::fill(std::begin(held_input.released_keys), std::end(held_input.released_keys), false);
        std::fill(
            std::begin(held_input.pressed_gamepad_buttons),
            std::end(held_input.pressed_gamepad_buttons), false);
        std::fill(
            std::begin(held_input.released_gamepad_buttons),
            std::end(held_input.released_gamepad_buttons), false);
        held_input.mouse_left_pressed = false;
        held_input.mouse_left_released = false;
        auto sampled_profile_index = active_input_profile_index;
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
            // Save-file and music-stream I/O are structural work, never part of
            // fixed_tick. Processing at each completed tick boundary preserves
            // identical semantics whether a caller batches exact ticks or
            // advances them one at a time.
            if (auto saved = process_pending_save(metrics); !saved) {
                return std::unexpected(std::move(saved.error()));
            }
            if (auto music = process_pending_music(metrics); !music) {
                return std::unexpected(std::move(music.error()));
            }
            for (auto& pending : pending_tick_edges) {
                pending.pressed = false;
                pending.released = false;
            }
            if (!actions_prepared && tick + 1U < advance_result.tick_count &&
                sampled_profile_index != active_input_profile_index) {
                update_actions(held_input);
                sampled_profile_index = active_input_profile_index;
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
        if (advance_result.tick_count == 0U) {
            if (auto saved = process_pending_save(metrics); !saved) {
                return std::unexpected(std::move(saved.error()));
            }
            if (auto music = process_pending_music(metrics); !music) {
                return std::unexpected(std::move(music.error()));
            }
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
        auto rendered = render(metrics.interpolation_alpha, metrics.extracted_sprites, metrics);
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
            if (plan.schema_version == GameSchemaVersion::v0_6) {
                if (const auto* sprite = world->sprite(record.entity); sprite != nullptr) {
                    const double weight = static_cast<double>(index + 1U);
                    metrics.scene_state_checksum +=
                        weight * (static_cast<double>(sprite->uv.min.x) * 149.0 +
                                  static_cast<double>(sprite->uv.min.y) * 151.0 +
                                  static_cast<double>(sprite->uv.max.x) * 157.0 +
                                  static_cast<double>(sprite->uv.max.y) * 163.0);
                }
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
        for (std::size_t index = 0U; index < animation_states.size(); ++index) {
            const auto& animation = animation_states[index];
            const double weight = static_cast<double>(index + 1U);
            metrics.scene_state_checksum += weight *
                (static_cast<double>(animation.clip_index + 1U) * 53.0 +
                 static_cast<double>(animation.frame_index + 1U) * 59.0 +
                 static_cast<double>(animation.ticks_remaining) * 61.0 +
                 static_cast<double>(animation.direction) * 67.0 +
                 (animation.has_clip ? 71.0 : 0.0) +
                 (animation.playing ? 73.0 : 0.0) +
                 (animation.completion_emitted ? 79.0 : 0.0));
        }
        for (std::size_t layer = 0U; layer < tile_layers.size(); ++layer) {
            for (std::size_t cell = 0U; cell < tile_layers[layer].size(); ++cell) {
                metrics.scene_state_checksum += static_cast<double>(layer + 1U) *
                                                static_cast<double>(cell + 1U) *
                                                static_cast<double>(tile_layers[layer][cell]) * 83.0;
            }
        }
        for (std::size_t field = 0U; field < fields.size(); ++field) {
            for (std::size_t cell = 0U; cell < fields[field].size(); ++cell) {
                metrics.scene_state_checksum += static_cast<double>(field + 1U) *
                                                static_cast<double>(cell + 1U) *
                                                static_cast<double>(fields[field][cell]) * 89.0;
            }
        }
        for (std::size_t emitter = 0U; emitter < particle_emitters.size(); ++emitter) {
            const auto& emitter_state = particle_emitters[emitter];
            metrics.scene_state_checksum +=
                static_cast<double>(emitter + 1U) *
                (static_cast<double>(emitter_state.next_acquisition_order & 0xFFFF'FFFFULL) * 167.0 +
                 static_cast<double>(emitter_state.emission_count & 0xFFFF'FFFFULL) * 173.0 +
                 static_cast<double>(emitter_state.active_count) * 179.0);
            for (std::size_t slot = 0U; slot < particle_emitters[emitter].slots.size(); ++slot) {
                const auto& particle = particle_emitters[emitter].slots[slot];
                if (!particle.active) continue;
                const double weight = static_cast<double>(emitter + 1U) * static_cast<double>(slot + 1U);
                metrics.scene_state_checksum += weight *
                    (particle.position.x * 97.0 + particle.position.y * 101.0 +
                     particle.velocity.x * 103.0 + particle.velocity.y * 107.0 +
                     static_cast<double>(particle.expiration_tick & 0xFFFF'FFFFULL) * 109.0 +
                     static_cast<double>(particle.acquisition_order & 0xFFFF'FFFFULL) * 181.0);
            }
        }
        metrics.scene_state_checksum += camera_state.position.x * 113.0 + camera_state.position.y * 127.0 +
                                        camera_state.zoom * 131.0 +
                                        static_cast<double>(active_locale_index) * 137.0 +
                                        static_cast<double>(active_input_profile_index) * 139.0;
        metrics.active_pooled_entities = active_pooled_entities;
        metrics.peak_active_pooled_entities = peak_active_pooled_entities;
        metrics.peak_active_particles = std::max(metrics.peak_active_particles, peak_active_particles);
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
            scene_snapshots[scene_index].animations.resize(count);
            scene_snapshots[scene_index].tile_layers.resize(plan.scenes[scene_index].tile_layers.size());
            for (std::size_t layer = 0U; layer < plan.scenes[scene_index].tile_layers.size(); ++layer) {
                scene_snapshots[scene_index].tile_layers[layer].resize(
                    plan.scenes[scene_index].tile_layers[layer].initial_cells.size());
            }
            scene_snapshots[scene_index].fields.resize(plan.scenes[scene_index].fields.size());
            for (std::size_t field = 0U; field < plan.scenes[scene_index].fields.size(); ++field) {
                scene_snapshots[scene_index].fields[field].resize(
                    plan.scenes[scene_index].fields[field].initial_cells.size());
            }
            scene_snapshots[scene_index].particle_emitters.resize(
                plan.scenes[scene_index].particle_emitters.size());
            for (std::size_t emitter = 0U; emitter < plan.scenes[scene_index].particle_emitters.size(); ++emitter) {
                initialize_particle_storage(
                    scene_snapshots[scene_index].particle_emitters[emitter],
                    plan.scenes[scene_index].particle_emitters[emitter].capacity);
            }
            scene_snapshots[scene_index].contacts.reserve(plan.scenes[scene_index].max_contact_pairs);
            for (std::size_t pool_index = 0U; pool_index < plan.scenes[scene_index].pools.size(); ++pool_index) {
                const auto group_index = plan.scenes[scene_index].pools[pool_index].spawn_group_index;
                const auto& group = plan.scenes[scene_index].spawn_groups[group_index];
                initialize_pool_storage(
                    scene_snapshots[scene_index].pool_states[pool_index], group.count, group.active_count);
            }
        }
        load_state_scratch.resize(plan.states.size());
        if (plan.save.enabled) {
            load_scene_snapshots = scene_snapshots;
            for (std::size_t scene_index = 0U; scene_index < plan.scenes.size(); ++scene_index) {
                load_scene_snapshots[scene_index].contacts.reserve(plan.scenes[scene_index].max_contact_pairs);
            }
            auto calculated_save_capacity = compute_game_save_capacity(plan);
            if (!calculated_save_capacity) {
                return std::unexpected(std::move(calculated_save_capacity.error()));
            }
            std::uint32_t maximum_pool_capacity = 0U;
            std::uint32_t maximum_persistent_entities = 0U;
            std::uint32_t maximum_particle_capacity = 0U;
            for (std::size_t scene_index = 0U; scene_index < plan.scenes.size(); ++scene_index) {
                if (!plan.scenes[scene_index].persistent) continue;
                const auto& scene = plan.scenes[scene_index];
                maximum_persistent_entities = std::max(maximum_persistent_entities, scene.total_spawn_count);
                for (const auto& emitter : scene.particle_emitters) {
                    maximum_particle_capacity = std::max(maximum_particle_capacity, emitter.capacity);
                }
                for (const auto& pool : scene.pools) {
                    const auto pool_capacity = scene.spawn_groups[pool.spawn_group_index].count;
                    maximum_pool_capacity = std::max(maximum_pool_capacity, pool_capacity);
                }
            }
            save_capacity_limit = static_cast<std::size_t>(*calculated_save_capacity);
            save_buffer.reserve(save_capacity_limit);
            load_buffer.reserve(save_capacity_limit);
            save_validation_slots.resize(maximum_pool_capacity);
            save_validation_entity_groups.resize(maximum_persistent_entities);
            save_validation_particle_slots.resize(maximum_particle_capacity);
            if (!options.save_directory_override.empty()) {
                save_directory = options.save_directory_override;
            } else {
                std::filesystem::path preference_base{};
#if defined(AI2D_ENABLE_GPU)
                auto settings_path = settings_file_path(
                    plan.symbol(plan.organization), plan.symbol(plan.application));
                if (settings_path) preference_base = settings_path->parent_path();
                else preference_base = std::filesystem::temp_directory_path() / "ai2d" /
                                       std::string{plan.symbol(plan.organization)} /
                                       std::string{plan.symbol(plan.application)};
#else
                preference_base = std::filesystem::temp_directory_path() / "ai2d" /
                                  std::string{plan.symbol(plan.organization)} /
                                  std::string{plan.symbol(plan.application)};
#endif
                if (!safe_preference_component(plan.symbol(plan.organization)) ||
                    !safe_preference_component(plan.symbol(plan.application))) {
                    return std::unexpected(runtime_error(
                        DiagnosticCode::game_save_invalid,
                        "Save preference components are unsafe"));
                }
                save_directory = preference_base / "saves";
                std::error_code path_error{};
                std::filesystem::create_directories(save_directory, path_error);
                const auto canonical_base = std::filesystem::weakly_canonical(preference_base, path_error);
                const auto canonical_save = std::filesystem::weakly_canonical(save_directory, path_error);
                if (path_error || !path_is_descendant(canonical_base, canonical_save)) {
                    return std::unexpected(runtime_error(
                        DiagnosticCode::game_save_invalid,
                        "Save preference directory escapes its engine-owned root"));
                }
                save_directory = canonical_save;
            }
        }
        if (std::any_of(plan.assets.begin(), plan.assets.end(), [](const GameAssetPlan& asset) {
                return asset.kind == GameAssetKind::music;
            })) {
            constexpr std::size_t music_block_frames = 4'096U;
            music_stream.source_buffer.resize(music_block_frames * 2U * sizeof(float));
            music_stream.sample_buffer.resize(music_block_frames * 2U);
        }
        state_indices.reserve(plan.states.size());
        for (std::size_t index = 0U; index < plan.states.size(); ++index) {
            state_indices.emplace(std::string{plan.symbol(plan.states[index].symbol)}, static_cast<std::uint32_t>(index));
        }
        ui_text_tokens.resize(plan.scenes.size());
        localized_ui_text_tokens.resize(plan.localizations.size());
        for (auto& locale : localized_ui_text_tokens) locale.resize(plan.scenes.size());
        std::size_t maximum_formatted_bytes = 0U;
        const auto compile_text_tokens = [&](const std::string_view source,
                                             std::vector<TextToken>& tokens) -> Result<std::size_t> {
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
            return formatted_bytes;
        };
        for (std::size_t scene_index = 0U; scene_index < plan.scenes.size(); ++scene_index) {
            const auto& ui = plan.scenes[scene_index].ui;
            ui_text_tokens[scene_index].resize(ui.size());
            for (auto& locale : localized_ui_text_tokens) locale[scene_index].resize(ui.size());
            for (std::size_t element_index = 0U; element_index < ui.size(); ++element_index) {
                const auto& element = ui[element_index];
                if (element.kind == UiElementKind::panel) continue;
                if (!element.localized) {
                    auto compiled = compile_text_tokens(
                        plan.symbol(element.text), ui_text_tokens[scene_index][element_index]);
                    if (!compiled) return std::unexpected(std::move(compiled.error()));
                    maximum_formatted_bytes = std::max(maximum_formatted_bytes, *compiled);
                    continue;
                }
                if (element.localization_key_index >= plan.localizations.front().entries.size()) {
                    return std::unexpected(runtime_error(
                        DiagnosticCode::game_localization_invalid,
                        "Localized UI key index is invalid"));
                }
                for (std::size_t locale_index = 0U; locale_index < plan.localizations.size(); ++locale_index) {
                    const auto value_symbol =
                        plan.localizations[locale_index].entries[element.localization_key_index].value;
                    auto compiled = compile_text_tokens(
                        plan.symbol(value_symbol),
                        localized_ui_text_tokens[locale_index][scene_index][element_index]);
                    if (!compiled) return std::unexpected(std::move(compiled.error()));
                    maximum_formatted_bytes = std::max(maximum_formatted_bytes, *compiled);
                }
            }
        }
        active_locale_index = plan.default_locale_index;
        active_input_profile_index = plan.default_input_profile_index;
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
                if (asset.kind == GameAssetKind::music) continue;
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
                } else if (asset.kind == GameAssetKind::wav && audio_available) {
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

Result<std::uint32_t> GameRuntime::animation_frame(
    const std::uint32_t spawn_group_index,
    const std::uint32_t item_index) const {
    if (!impl_->ready || spawn_group_index >= impl_->group_ranges.size()) {
        return std::unexpected(runtime_error(
            DiagnosticCode::input_invalid, "Animation observation group index is invalid"));
    }
    const auto range = impl_->group_ranges[spawn_group_index];
    if (item_index >= range.count || range.begin + item_index >= impl_->animation_states.size()) {
        return std::unexpected(runtime_error(
            DiagnosticCode::input_invalid, "Animation observation entity index is invalid"));
    }
    const auto& state = impl_->animation_states[range.begin + item_index];
    if (!state.has_clip) {
        return std::unexpected(runtime_error(
            DiagnosticCode::input_invalid, "Observed entity does not have an active animation clip"));
    }
    return state.frame_index;
}

Result<std::uint32_t> GameRuntime::tile_value(
    const std::uint32_t tile_layer_index,
    const std::uint32_t x,
    const std::uint32_t y) const {
    if (!impl_->ready || tile_layer_index >= impl_->tile_layers.size()) {
        return std::unexpected(runtime_error(
            DiagnosticCode::input_invalid, "Tile observation layer index is invalid"));
    }
    const auto& layer = impl_->plan.scenes[impl_->active_scene].tile_layers[tile_layer_index];
    const auto& grid = impl_->plan.scenes[impl_->active_scene].grids[layer.grid_index];
    if (x >= grid.columns || y >= grid.rows) {
        return std::unexpected(runtime_error(
            DiagnosticCode::input_invalid, "Tile observation cell is out of range"));
    }
    return impl_->tile_layers[tile_layer_index][static_cast<std::size_t>(y) * grid.columns + x];
}

Result<std::int32_t> GameRuntime::field_value(
    const std::uint32_t field_index,
    const std::uint32_t x,
    const std::uint32_t y) const {
    if (!impl_->ready || field_index >= impl_->fields.size()) {
        return std::unexpected(runtime_error(
            DiagnosticCode::input_invalid, "Field observation index is invalid"));
    }
    const auto& field = impl_->plan.scenes[impl_->active_scene].fields[field_index];
    const auto& grid = impl_->plan.scenes[impl_->active_scene].grids[field.grid_index];
    if (x >= grid.columns || y >= grid.rows) {
        return std::unexpected(runtime_error(
            DiagnosticCode::input_invalid, "Field observation cell is out of range"));
    }
    return impl_->fields[field_index][static_cast<std::size_t>(y) * grid.columns + x];
}

Vec2 GameRuntime::camera_position() const noexcept {
    return impl_->ready ? impl_->camera_state.position : Vec2{};
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
