#include "ai2d/foundation/frame_arena.hpp"
#include "ai2d/foundation/allocation_tracker.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>

TEST_CASE("FrameArena honors over-aligned addresses and includes padding in its budget") {
    struct alignas(4096) Page final { std::byte bytes[4096]; };
    ai2d::FrameArena arena{8192U};
    const auto prefix = arena.try_allocate(1U, 1U);
    REQUIRE(prefix.size() == 1U);
    const auto pages = arena.try_allocate<Page>();
    REQUIRE(pages.size() == 1U);
    const auto address = reinterpret_cast<std::uintptr_t>(pages.data());
    CHECK(address % alignof(Page) == 0U);
    CHECK(arena.metrics().used_bytes ==
        address - reinterpret_cast<std::uintptr_t>(prefix.data()) + sizeof(Page));
    CHECK(arena.metrics().overflow_count == 0U);

    const auto before = arena.metrics();
    CHECK(arena.try_allocate<Page>().empty());
    CHECK(arena.metrics().used_bytes == before.used_bytes);
    CHECK(arena.metrics().overflow_count == 1U);
}

TEST_CASE("FrameArena rejects alignment padding that cannot fit") {
    ai2d::FrameArena arena{128U};
    constexpr auto huge_alignment = std::size_t{1U} << (std::numeric_limits<std::size_t>::digits - 1U);
    CHECK(arena.try_allocate(1U, huge_alignment).empty());
    CHECK(arena.metrics().used_bytes == 0U);
    CHECK(arena.metrics().overflow_count == 1U);
}

TEST_CASE("FrameArena over-aligned steady-state allocations use no C++ heap") {
#if defined(AI2D_ENABLE_ALLOCATION_TRACKING)
    ai2d::FrameArena arena{8192U};
    ai2d::MeasuredAllocationScope scope{};
    const auto first = arena.try_allocate(64U, 64U);
    const auto second = arena.try_allocate(4096U, 4096U);
    const auto allocations = scope.finish();
    CHECK(first.size() == 64U);
    CHECK(second.size() == 4096U);
    CHECK(allocations.allocations == 0U);
    CHECK(allocations.bytes == 0U);
    CHECK(arena.metrics().overflow_count == 0U);
#else
    SKIP("allocation hook disabled by configuration");
#endif
}

TEST_CASE("FrameArena handles empty storage and failed requests without consuming space") {
    ai2d::FrameArena empty{0U};
    CHECK(empty.try_allocate(0U, 1U).empty());
    CHECK(empty.metrics().overflow_count == 0U);
    CHECK(empty.try_allocate(1U, 1U).empty());
    CHECK(empty.metrics().overflow_count == 1U);

    ai2d::FrameArena arena{64U};
    CHECK(arena.try_allocate(1U, 3U).empty());
    CHECK(arena.metrics().used_bytes == 0U);
    CHECK(arena.try_allocate(64U, 1U).size() == 64U);
    CHECK(arena.metrics().allocation_calls == 2U);
    CHECK(arena.metrics().overflow_count == 1U);
}

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
