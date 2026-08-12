#include "ai2d/runtime/fixed_step.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("Fixed-step clock clamps stalls and bounds catch-up work") {
    ai2d::FixedStepClock clock{};
    REQUIRE(clock.initialize());

    const auto advance = clock.advance(0.5);
    REQUIRE(advance);
    CHECK(advance->tick_count == 8U);
    CHECK(advance->frame_time_clamped);
    CHECK(advance->catch_up_limited);
    CHECK(advance->dropped_seconds > 0.25);
    CHECK(advance->interpolation_alpha >= 0.0);
    CHECK(advance->interpolation_alpha < 1.0);
}

TEST_CASE("Fixed-step clock exposes exact deterministic headless ticks") {
    ai2d::FixedStepClock clock{};
    REQUIRE(clock.initialize());
    const auto exact = clock.exact(120U);
    REQUIRE(exact);
    CHECK(exact->tick_count == 120U);
    CHECK(exact->step_seconds == 1.0 / 60.0);
    CHECK(exact->interpolation_alpha == 0.0);
    CHECK_FALSE(exact->frame_time_clamped);
    CHECK_FALSE(exact->catch_up_limited);
}
