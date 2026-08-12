#include "ai2d/foundation/statistics.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>

TEST_CASE("Distribution summary uses interpolated percentiles and MAD") {
    constexpr std::array<double, 4> samples{4.0, 1.0, 3.0, 2.0};
    const auto summary = ai2d::summarize(samples);

    REQUIRE(summary.sample_count == 4U);
    REQUIRE(summary.average == Catch::Approx(2.5));
    REQUIRE(summary.median == Catch::Approx(2.5));
    REQUIRE(summary.p95 == Catch::Approx(3.85));
    REQUIRE(summary.p99 == Catch::Approx(3.97));
    REQUIRE(summary.minimum == Catch::Approx(1.0));
    REQUIRE(summary.maximum == Catch::Approx(4.0));
    REQUIRE(summary.mad == Catch::Approx(1.0));
}

TEST_CASE("Empty distribution is explicit zero sample data") {
    const auto summary = ai2d::summarize({});
    REQUIRE(summary.sample_count == 0U);
    REQUIRE(summary.average == 0.0);
}
