#pragma once

#include <cstddef>
#include <cstdint>

namespace ai2d {

struct AllocationSnapshot final {
    std::uint64_t allocations{0U};
    std::uint64_t bytes{0U};
};

void reset_allocation_counters() noexcept;
void set_allocation_tracking(bool enabled) noexcept;
[[nodiscard]] bool allocation_tracking_enabled() noexcept;
[[nodiscard]] AllocationSnapshot allocation_snapshot() noexcept;
void record_cpp_allocation(std::size_t bytes) noexcept;

class MeasuredAllocationScope final {
public:
    MeasuredAllocationScope() noexcept;
    ~MeasuredAllocationScope();

    MeasuredAllocationScope(const MeasuredAllocationScope&) = delete;
    MeasuredAllocationScope& operator=(const MeasuredAllocationScope&) = delete;

    [[nodiscard]] AllocationSnapshot finish() noexcept;

private:
    bool active_{true};
};

} // namespace ai2d
