#include "ai2d/foundation/frame_arena.hpp"

#include <algorithm>
#include <memory>

namespace ai2d {

FrameArena::FrameArena(const std::size_t capacity_bytes) : storage_(capacity_bytes) {}

std::span<std::byte> FrameArena::try_allocate(const std::size_t bytes, const std::size_t alignment) noexcept {
    ++calls_;
    if (alignment == 0U || (alignment & (alignment - 1U)) != 0U) {
        ++overflows_;
        return {};
    }

    if (storage_.empty()) {
        if (bytes != 0U) {
            ++overflows_;
        }
        return {};
    }
    // The byte vector's base is not necessarily aligned for over-aligned types.
    // Align the actual address, charging any padding against the fixed budget.
    void* address = storage_.data() + offset_;
    auto space = storage_.size() - offset_;
    if (std::align(alignment, bytes, address, space) == nullptr) {
        ++overflows_;
        return {};
    }

    offset_ = storage_.size() - space + bytes;
    peak_ = std::max(peak_, offset_);
    return {static_cast<std::byte*>(address), bytes};
}

void FrameArena::reset() noexcept {
    offset_ = 0U;
    calls_ = 0U;
    overflows_ = 0U;
}

FrameArenaMetrics FrameArena::metrics() const noexcept {
    return {storage_.size(), offset_, peak_, calls_, overflows_};
}

} // namespace ai2d
