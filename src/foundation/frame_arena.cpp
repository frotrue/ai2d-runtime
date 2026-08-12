#include "ai2d/foundation/frame_arena.hpp"

#include <algorithm>
#include <limits>

namespace ai2d {

FrameArena::FrameArena(const std::size_t capacity_bytes) : storage_(capacity_bytes) {}

std::span<std::byte> FrameArena::try_allocate(const std::size_t bytes, const std::size_t alignment) noexcept {
    ++calls_;
    if (alignment == 0U || (alignment & (alignment - 1U)) != 0U) {
        ++overflows_;
        return {};
    }

    const auto mask = alignment - 1U;
    if (offset_ > std::numeric_limits<std::size_t>::max() - mask) {
        ++overflows_;
        return {};
    }
    const auto aligned_offset = (offset_ + mask) & ~mask;
    if (aligned_offset > storage_.size() || bytes > storage_.size() - aligned_offset) {
        ++overflows_;
        return {};
    }

    offset_ = aligned_offset + bytes;
    peak_ = std::max(peak_, offset_);
    return {storage_.data() + aligned_offset, bytes};
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
