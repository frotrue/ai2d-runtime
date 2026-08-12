#include "ai2d/foundation/allocation_tracker.hpp"

#include <atomic>

namespace ai2d {
namespace {

std::atomic_bool tracking_enabled{false};
std::atomic<std::uint64_t> allocation_count{0U};
std::atomic<std::uint64_t> allocation_bytes{0U};

} // namespace

void reset_allocation_counters() noexcept {
    allocation_count.store(0U, std::memory_order_relaxed);
    allocation_bytes.store(0U, std::memory_order_relaxed);
}

void set_allocation_tracking(const bool enabled) noexcept {
    tracking_enabled.store(enabled, std::memory_order_release);
}

bool allocation_tracking_enabled() noexcept {
    return tracking_enabled.load(std::memory_order_acquire);
}

AllocationSnapshot allocation_snapshot() noexcept {
    return {
        allocation_count.load(std::memory_order_relaxed),
        allocation_bytes.load(std::memory_order_relaxed),
    };
}

void record_cpp_allocation(const std::size_t bytes) noexcept {
    if (tracking_enabled.load(std::memory_order_relaxed)) {
        allocation_count.fetch_add(1U, std::memory_order_relaxed);
        allocation_bytes.fetch_add(static_cast<std::uint64_t>(bytes), std::memory_order_relaxed);
    }
}

MeasuredAllocationScope::MeasuredAllocationScope() noexcept {
    reset_allocation_counters();
    set_allocation_tracking(true);
}

MeasuredAllocationScope::~MeasuredAllocationScope() {
    if (active_) {
        set_allocation_tracking(false);
    }
}

AllocationSnapshot MeasuredAllocationScope::finish() noexcept {
    if (active_) {
        set_allocation_tracking(false);
        active_ = false;
    }
    return allocation_snapshot();
}

} // namespace ai2d
