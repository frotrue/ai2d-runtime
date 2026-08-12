#include "ai2d/foundation/statistics.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>

namespace ai2d {

double percentile_sorted(const std::span<const double> sorted_samples, const double percentile) noexcept {
    if (sorted_samples.empty()) {
        return 0.0;
    }
    const auto clamped = std::clamp(percentile, 0.0, 100.0);
    const auto last_index = static_cast<double>(sorted_samples.size() - 1U);
    const auto rank = (clamped / 100.0) * last_index;
    const auto lower = static_cast<std::size_t>(std::floor(rank));
    const auto upper = static_cast<std::size_t>(std::ceil(rank));
    if (lower == upper) {
        return sorted_samples[lower];
    }
    const auto weight = rank - static_cast<double>(lower);
    return sorted_samples[lower] + ((sorted_samples[upper] - sorted_samples[lower]) * weight);
}

DistributionSummary summarize(const std::span<const double> samples) {
    if (samples.empty()) {
        return {};
    }
    std::vector<double> sorted(samples.begin(), samples.end());
    std::sort(sorted.begin(), sorted.end());
    const auto median = percentile_sorted(sorted, 50.0);

    std::vector<double> deviations{};
    deviations.reserve(sorted.size());
    for (const auto sample : sorted) {
        deviations.push_back(std::abs(sample - median));
    }
    std::sort(deviations.begin(), deviations.end());

    DistributionSummary summary{};
    summary.sample_count = sorted.size();
    summary.average = std::accumulate(sorted.begin(), sorted.end(), 0.0) / static_cast<double>(sorted.size());
    summary.median = median;
    summary.p95 = percentile_sorted(sorted, 95.0);
    summary.p99 = percentile_sorted(sorted, 99.0);
    summary.minimum = sorted.front();
    summary.maximum = sorted.back();
    summary.mad = percentile_sorted(deviations, 50.0);
    return summary;
}

} // namespace ai2d
