#include "ai2d/renderer2d/render_queue.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace ai2d {
namespace {

bool is_visible(const SpriteSubmission2D& sprite, const Camera2D& camera) noexcept {
    if (!sprite.visible) {
        return false;
    }
    const float absolute_cosine = std::abs(std::cos(sprite.rotation));
    const float absolute_sine = std::abs(std::sin(sprite.rotation));
    const float half_width = std::abs(sprite.size.x) * 0.5F;
    const float half_height = std::abs(sprite.size.y) * 0.5F;
    const float projected_x = half_width * absolute_cosine + half_height * absolute_sine;
    const float projected_y = half_width * absolute_sine + half_height * absolute_cosine;
    const float local_center_x = (0.5F - sprite.pivot.x) * sprite.size.x;
    const float local_center_y = (0.5F - sprite.pivot.y) * sprite.size.y;
    const float center_x = sprite.position.x +
                           local_center_x * std::cos(sprite.rotation) -
                           local_center_y * std::sin(sprite.rotation);
    const float center_y = sprite.position.y +
                           local_center_x * std::sin(sprite.rotation) +
                           local_center_y * std::cos(sprite.rotation);
    const float relative_x = center_x - camera.position.x;
    const float relative_y = center_y - camera.position.y;
    return relative_x + projected_x >= -camera.half_extent.x &&
           relative_x - projected_x <= camera.half_extent.x &&
           relative_y + projected_y >= -camera.half_extent.y &&
           relative_y - projected_y <= camera.half_extent.y;
}

bool less_for_batching(const PreparedSprite2D& left, const PreparedSprite2D& right) noexcept {
    if (left.layer != right.layer) {
        return left.layer < right.layer;
    }
    if (left.texture.index != right.texture.index) {
        return left.texture.index < right.texture.index;
    }
    if (left.texture.generation != right.texture.generation) {
        return left.texture.generation < right.texture.generation;
    }
    return left.source_order < right.source_order;
}

bool same_batch(const PreparedSprite2D& left, const PreparedSprite2D& right) noexcept {
    return left.layer == right.layer && left.texture == right.texture;
}

} // namespace

void RenderQueue2D::reserve(const std::size_t max_sprites) {
    instances_.reserve(max_sprites);
    batches_.reserve(max_sprites);
}

Result<RenderQueueMetrics> RenderQueue2D::build(const RenderFrame2D& frame) {
    RenderQueueMetrics metrics{};
    metrics.visited = frame.sprites.size();
    instances_.clear();
    batches_.clear();

    if (!(frame.camera.half_extent.x > 0.0F) || !(frame.camera.half_extent.y > 0.0F) ||
        !std::isfinite(frame.camera.half_extent.x) || !std::isfinite(frame.camera.half_extent.y)) {
        auto diagnostic = Diagnostic::make(
            DiagnosticCode::input_invalid,
            Severity::error,
            "renderer2d",
            "Camera half extents must be finite and positive");
        return std::unexpected(std::move(diagnostic));
    }

    for (std::size_t index = 0U; index < frame.sprites.size(); ++index) {
        const auto& sprite = frame.sprites[index];
        if (!std::isfinite(sprite.position.x) || !std::isfinite(sprite.position.y) ||
            !std::isfinite(sprite.size.x) || !std::isfinite(sprite.size.y) ||
            sprite.size.x <= 0.0F || sprite.size.y <= 0.0F ||
            !std::isfinite(sprite.pivot.x) || !std::isfinite(sprite.pivot.y) ||
            !std::isfinite(sprite.rotation)) {
            return std::unexpected(Diagnostic::make(
                DiagnosticCode::input_invalid,
                Severity::error,
                "renderer2d",
                "Sprite geometry must be finite with positive size"));
        }
        if (!is_visible(sprite, frame.camera)) {
            ++metrics.culled;
            continue;
        }
        if (instances_.size() == instances_.capacity() || index > std::numeric_limits<std::uint32_t>::max()) {
            auto diagnostic = Diagnostic::make(
                DiagnosticCode::render_upload_capacity_exceeded,
                Severity::error,
                "renderer2d",
                "Visible sprite count exceeds the reserved render queue capacity");
            diagnostic.context.push_back({"reserved_capacity", static_cast<std::uint64_t>(instances_.capacity())});
            diagnostic.context.push_back({"visited", static_cast<std::uint64_t>(index + 1U)});
            return std::unexpected(std::move(diagnostic));
        }
        PreparedSprite2D prepared{};
        prepared.clip_position[0] = (sprite.position.x - frame.camera.position.x) / frame.camera.half_extent.x;
        prepared.clip_position[1] = (sprite.position.y - frame.camera.position.y) / frame.camera.half_extent.y;
        prepared.clip_size[0] = sprite.size.x / frame.camera.half_extent.x;
        prepared.clip_size[1] = sprite.size.y / frame.camera.half_extent.y;
        prepared.rotation = sprite.rotation;
        prepared.pivot[0] = sprite.pivot.x;
        prepared.pivot[1] = sprite.pivot.y;
        prepared.uv_rect[0] = sprite.uv.u;
        prepared.uv_rect[1] = sprite.uv.v;
        prepared.uv_rect[2] = sprite.uv.width;
        prepared.uv_rect[3] = sprite.uv.height;
        prepared.tint[0] = sprite.tint.r;
        prepared.tint[1] = sprite.tint.g;
        prepared.tint[2] = sprite.tint.b;
        prepared.tint[3] = sprite.tint.a;
        prepared.texture = sprite.texture;
        prepared.layer = sprite.layer;
        prepared.source_order = static_cast<std::uint32_t>(index);
        instances_.push_back(prepared);
    }

    std::sort(instances_.begin(), instances_.end(), less_for_batching);
    std::size_t first = 0U;
    while (first < instances_.size()) {
        std::size_t end = first + 1U;
        while (end < instances_.size() && same_batch(instances_[first], instances_[end])) {
            ++end;
        }
        if (batches_.size() == batches_.capacity()) {
            auto diagnostic = Diagnostic::make(
                DiagnosticCode::render_upload_capacity_exceeded,
                Severity::error,
                "renderer2d",
                "Sprite batch count exceeds the reserved render queue capacity");
            return std::unexpected(std::move(diagnostic));
        }
        batches_.push_back({
            instances_[first].texture,
            instances_[first].layer,
            static_cast<std::uint32_t>(first),
            static_cast<std::uint32_t>(end - first),
        });
        first = end;
    }
    metrics.visible = instances_.size();
    metrics.batches = batches_.size();
    return metrics;
}

std::span<const PreparedSprite2D> RenderQueue2D::instances() const noexcept { return instances_; }
std::span<const SpriteBatch2D> RenderQueue2D::batches() const noexcept { return batches_; }
std::size_t RenderQueue2D::sprite_capacity() const noexcept { return instances_.capacity(); }
std::size_t RenderQueue2D::batch_capacity() const noexcept { return batches_.capacity(); }

static_assert(alignof(PreparedSprite2D) == 16U);
static_assert(sizeof(PreparedSprite2D) == 80U);

} // namespace ai2d
