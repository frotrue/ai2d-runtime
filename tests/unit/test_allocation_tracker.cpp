#include "ai2d/foundation/allocation_tracker.hpp"

#include <catch2/catch_test_macros.hpp>

#include <vector>

TEST_CASE("Measured allocation hook detects intentional C++ heap allocation") {
#if defined(AI2D_ENABLE_ALLOCATION_TRACKING)
    std::vector<int> values{};
    ai2d::MeasuredAllocationScope scope{};
    values.reserve(64U);
    const auto snapshot = scope.finish();
    REQUIRE(snapshot.allocations >= 1U);
    REQUIRE(snapshot.bytes >= 64U * sizeof(int));
#else
    SKIP("allocation hook disabled by configuration");
#endif
}

TEST_CASE("Reserved vector steady-state mutation has no measured allocation") {
#if defined(AI2D_ENABLE_ALLOCATION_TRACKING)
    std::vector<int> values{};
    values.reserve(64U);
    ai2d::MeasuredAllocationScope scope{};
    for (int value = 0; value < 64; ++value) {
        values.push_back(value);
    }
    const auto snapshot = scope.finish();
    REQUIRE(snapshot.allocations == 0U);
    REQUIRE(snapshot.bytes == 0U);
#else
    SKIP("allocation hook disabled by configuration");
#endif
}
