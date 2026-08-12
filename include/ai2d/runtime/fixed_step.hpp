#pragma once

#include "ai2d/foundation/diagnostic.hpp"

#include <cstdint>

namespace ai2d {

struct FixedStepConfig final {
    double step_seconds{1.0 / 60.0};
    double maximum_frame_seconds{0.250};
    std::uint32_t maximum_catch_up_ticks{8U};
};

struct FixedStepAdvance final {
    std::uint32_t tick_count{0U};
    double step_seconds{1.0 / 60.0};
    double interpolation_alpha{0.0};
    double dropped_seconds{0.0};
    bool frame_time_clamped{false};
    bool catch_up_limited{false};
};

class FixedStepClock final {
public:
    [[nodiscard]] Result<void> initialize(const FixedStepConfig& config = {});
    [[nodiscard]] Result<FixedStepAdvance> advance(double elapsed_seconds);
    [[nodiscard]] Result<FixedStepAdvance> exact(std::uint32_t tick_count) const;
    void reset() noexcept;
    [[nodiscard]] bool initialized() const noexcept { return initialized_; }
    [[nodiscard]] double accumulator_seconds() const noexcept { return accumulator_seconds_; }

private:
    FixedStepConfig config_{};
    double accumulator_seconds_{0.0};
    bool initialized_{false};
};

} // namespace ai2d
