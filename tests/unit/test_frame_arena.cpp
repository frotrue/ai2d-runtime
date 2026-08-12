#include "ai2d/foundation/frame_arena.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>

TEST_CASE("FrameArena aligns, records peak, and resets without fallback") {
    ai2d::FrameArena arena{128U};
    const auto bytes = arena.try_allocate(3U, 1U);
    const auto values = arena.try_allocate<std::uint64_t>(2U);

    REQUIRE(bytes.size() == 3U);
    REQUIRE(values.size() == 2U);
    REQUIRE(reinterpret_cast<std::uintptr_t>(values.data()) % alignof(std::uint64_t) == 0U);
    const auto before_reset = arena.metrics();
    REQUIRE(before_reset.allocation_calls == 2U);
    REQUIRE(before_reset.peak_bytes >= 19U);
    REQUIRE(before_reset.overflow_count == 0U);

    arena.reset();
    const auto after_reset = arena.metrics();
    REQUIRE(after_reset.used_bytes == 0U);
    REQUIRE(after_reset.allocation_calls == 0U);
    REQUIRE(after_reset.peak_bytes == before_reset.peak_bytes);
}

TEST_CASE("FrameArena reports overflow and never silently falls back") {
    ai2d::FrameArena arena{16U};
    const auto allocation = arena.try_allocate(32U, 8U);
    REQUIRE(allocation.empty());
    REQUIRE(arena.metrics().overflow_count == 1U);
}

TEST_CASE("FrameArena rejects typed allocation byte-count overflow") {
    ai2d::FrameArena arena{64U};
    constexpr auto overflowing_count =
        std::numeric_limits<std::size_t>::max() / sizeof(std::uint64_t) + 2U;
    const auto allocation = arena.try_allocate<std::uint64_t>(overflowing_count);
    REQUIRE(allocation.empty());
    CHECK(arena.metrics().allocation_calls == 1U);
    CHECK(arena.metrics().overflow_count == 1U);
    CHECK(arena.metrics().used_bytes == 0U);
}
