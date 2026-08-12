#pragma once

#include <cstddef>
#include <span>

int ai2d_scenario_benchmark_main(
    std::span<const std::size_t> counts,
    std::size_t runs,
    std::size_t warmup_frames,
    std::size_t measurement_frames,
    bool json_mode);
