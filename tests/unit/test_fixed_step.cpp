#include "ai2d/runtime/fixed_step.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>

TEST_CASE("Fixed-step clock bounds work even when the tick quotient overflows") {
    ai2d::FixedStepClock clock{};
    REQUIRE(clock.initialize({std::numeric_limits<double>::min(), 1.0, 8U}));
    const auto advance = clock.advance(1.0);
    REQUIRE(advance);
    CHECK(advance->tick_count == 8U);
    CHECK(advance->catch_up_limited);
    CHECK(std::isfinite(advance->dropped_seconds));
    CHECK(advance->interpolation_alpha >= 0.0);
    CHECK(advance->interpolation_alpha < 1.0);
}

TEST_CASE("Fixed-step clock rejects accumulator overflow without changing its state") {
    ai2d::FixedStepClock clock{};
    const auto maximum = std::numeric_limits<double>::max();
    REQUIRE(clock.initialize({maximum, maximum, 8U}));
    REQUIRE(clock.advance(maximum * 0.75));
    const auto before = clock.accumulator_seconds();
    const auto advance = clock.advance(maximum * 0.75);
    REQUIRE_FALSE(advance);
    CHECK(advance.error().code == ai2d::DiagnosticCode::input_invalid);
    CHECK(clock.accumulator_seconds() == before);
}

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
