#pragma once

#include <cstddef>
#include <span>

namespace ai2d {

struct DistributionSummary final {
    std::size_t sample_count{0U};
    double average{0.0};
    double median{0.0};
    double p95{0.0};
    double p99{0.0};
    double minimum{0.0};
    double maximum{0.0};
    double mad{0.0};
};

[[nodiscard]] double percentile_sorted(std::span<const double> sorted_samples, double percentile) noexcept;
[[nodiscard]] DistributionSummary summarize(std::span<const double> samples);

} // namespace ai2d
