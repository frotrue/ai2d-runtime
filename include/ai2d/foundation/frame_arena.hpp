#pragma once

#include <cstddef>
#include <limits>
#include <span>
#include <vector>

namespace ai2d {

struct FrameArenaMetrics final {
    std::size_t capacity_bytes{0U};
    std::size_t used_bytes{0U};
    std::size_t peak_bytes{0U};
    std::size_t allocation_calls{0U};
    std::size_t overflow_count{0U};
};

class FrameArena final {
public:
    explicit FrameArena(std::size_t capacity_bytes);

    [[nodiscard]] std::span<std::byte> try_allocate(std::size_t bytes, std::size_t alignment) noexcept;

    template <class T>
    [[nodiscard]] std::span<T> try_allocate(const std::size_t count = 1U) noexcept {
        if (count > std::numeric_limits<std::size_t>::max() / sizeof(T)) {
            ++calls_;
            ++overflows_;
            return {};
        }
        const auto bytes = try_allocate(sizeof(T) * count, alignof(T));
        if (bytes.empty() && count != 0U) {
            return {};
        }
        return {reinterpret_cast<T*>(bytes.data()), count};
    }

    void reset() noexcept;
    [[nodiscard]] FrameArenaMetrics metrics() const noexcept;

private:
    std::vector<std::byte> storage_{};
    std::size_t offset_{0U};
    std::size_t peak_{0U};
    std::size_t calls_{0U};
    std::size_t overflows_{0U};
};

} // namespace ai2d
