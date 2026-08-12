#pragma once

#include <chrono>

namespace ai2d {

class Stopwatch final {
public:
    using Clock = std::chrono::steady_clock;

    Stopwatch() noexcept : start_(Clock::now()) {}
    void reset() noexcept { start_ = Clock::now(); }

    [[nodiscard]] double elapsed_milliseconds() const noexcept {
        return std::chrono::duration<double, std::milli>(Clock::now() - start_).count();
    }

private:
    Clock::time_point start_{};
};

} // namespace ai2d
