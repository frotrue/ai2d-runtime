#include "ai2d/runtime/fixed_step.hpp"

#include <algorithm>
#include <cmath>

namespace ai2d {
namespace {

Diagnostic clock_error(const char* const message) {
    return Diagnostic::make(DiagnosticCode::input_invalid, Severity::error, "fixed_step", message);
}

} // namespace

Result<void> FixedStepClock::initialize(const FixedStepConfig& config) {
    if (initialized_) {
        return std::unexpected(clock_error("Fixed-step clock is already initialized"));
    }
    if (!std::isfinite(config.step_seconds) || config.step_seconds <= 0.0 ||
        !std::isfinite(config.maximum_frame_seconds) || config.maximum_frame_seconds < config.step_seconds ||
        config.maximum_catch_up_ticks == 0U || config.maximum_catch_up_ticks > 1'024U) {
        return std::unexpected(clock_error("Fixed-step clock configuration is invalid"));
    }
    config_ = config;
    initialized_ = true;
    return {};
}

Result<FixedStepAdvance> FixedStepClock::advance(const double elapsed_seconds) {
    if (!initialized_ || !std::isfinite(elapsed_seconds) || elapsed_seconds < 0.0) {
        return std::unexpected(clock_error("Fixed-step advance input is invalid"));
    }
    FixedStepAdvance result{};
    result.step_seconds = config_.step_seconds;
    const auto accepted = std::min(elapsed_seconds, config_.maximum_frame_seconds);
    result.frame_time_clamped = accepted != elapsed_seconds;
    result.dropped_seconds = elapsed_seconds - accepted;
    auto accumulator = accumulator_seconds_ + accepted;
    if (!std::isfinite(accumulator)) {
        return std::unexpected(clock_error("Fixed-step accumulator exceeds the finite range"));
    }
    // Bound the quotient before converting: valid finite durations can still
    // describe more ticks than any integer type can represent.
    const auto available_ticks = std::floor(accumulator / config_.step_seconds);
    result.tick_count = static_cast<std::uint32_t>(
        std::min(available_ticks, static_cast<double>(config_.maximum_catch_up_ticks)));
    result.catch_up_limited = available_ticks > config_.maximum_catch_up_ticks;
    accumulator -= static_cast<double>(result.tick_count) * config_.step_seconds;
    if (result.catch_up_limited) {
        const auto retained = std::fmod(accumulator, config_.step_seconds);
        result.dropped_seconds += accumulator - retained;
        accumulator = retained;
    }
    if (!std::isfinite(result.dropped_seconds)) {
        return std::unexpected(clock_error("Fixed-step dropped time exceeds the finite range"));
    }
    accumulator_seconds_ = accumulator;
    result.interpolation_alpha = std::clamp(accumulator / config_.step_seconds, 0.0, 1.0);
    return result;
}

Result<FixedStepAdvance> FixedStepClock::exact(const std::uint32_t tick_count) const {
    if (!initialized_ || tick_count == 0U || tick_count > 1'000'000U) {
        return std::unexpected(clock_error("Exact fixed-step request is invalid"));
    }
    return FixedStepAdvance{tick_count, config_.step_seconds, 0.0, 0.0, false, false};
}

void FixedStepClock::reset() noexcept { accumulator_seconds_ = 0.0; }

} // namespace ai2d
