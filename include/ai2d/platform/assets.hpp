#pragma once

#include "ai2d/foundation/diagnostic.hpp"

#include <cstdint>
#include <filesystem>
#include <vector>

namespace ai2d {

struct ImageRgba8 final {
    std::uint32_t width{0U};
    std::uint32_t height{0U};
    std::vector<std::uint8_t> pixels{};
};

struct WavePcmF32 final {
    static constexpr std::uint32_t channel_count = 2U;
    static constexpr std::uint32_t sample_rate = 48'000U;
    std::vector<float> samples{};
};

struct AssetDecodeInfo final {
    std::uint64_t source_bytes{0U};
    std::uint64_t decoded_bytes{0U};
};

[[nodiscard]] Result<AssetDecodeInfo> preflight_png_asset(const std::filesystem::path& path);
[[nodiscard]] Result<AssetDecodeInfo> preflight_wav_asset(const std::filesystem::path& path);
[[nodiscard]] Result<ImageRgba8> load_png_rgba8(const std::filesystem::path& path);
[[nodiscard]] Result<WavePcmF32> load_wav_f32_stereo(const std::filesystem::path& path);

} // namespace ai2d
