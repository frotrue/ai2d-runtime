#include "ai2d/platform/assets.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <exception>
#include <fstream>
#include <limits>
#include <memory>
#include <string>

namespace ai2d {
namespace {

Diagnostic asset_error(const DiagnosticCode code, const char* const action, const std::filesystem::path& path) {
    auto diagnostic = Diagnostic::make(code, Severity::error, "assets", action);
    diagnostic.context.push_back({"path", path.string()});
    diagnostic.context.push_back({"sdl_error", std::string{SDL_GetError()}});
    return diagnostic;
}

Diagnostic asset_validation_error(const char* const action, const std::filesystem::path& path) {
    auto diagnostic = Diagnostic::make(DiagnosticCode::asset_decode_failed, Severity::error, "assets", action);
    diagnostic.context.push_back({"path", path.string()});
    return diagnostic;
}

constexpr std::uint64_t maximum_source_bytes = 64ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t maximum_decoded_bytes = 256ULL * 1024ULL * 1024ULL;

std::uint16_t little_u16(const std::uint8_t* const bytes) noexcept {
    return static_cast<std::uint16_t>(bytes[0]) |
           static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[1]) << 8U);
}

std::uint32_t little_u32(const std::uint8_t* const bytes) noexcept {
    return static_cast<std::uint32_t>(bytes[0]) |
           (static_cast<std::uint32_t>(bytes[1]) << 8U) |
           (static_cast<std::uint32_t>(bytes[2]) << 16U) |
           (static_cast<std::uint32_t>(bytes[3]) << 24U);
}

std::uint32_t big_u32(const std::uint8_t* const bytes) noexcept {
    return (static_cast<std::uint32_t>(bytes[0]) << 24U) |
           (static_cast<std::uint32_t>(bytes[1]) << 16U) |
           (static_cast<std::uint32_t>(bytes[2]) << 8U) |
           static_cast<std::uint32_t>(bytes[3]);
}

Result<AssetDecodeInfo> preflight_png(const std::filesystem::path& path) {
    std::error_code error{};
    const auto size = std::filesystem::file_size(path, error);
    if (error || size < 24U || size > maximum_source_bytes) {
        return std::unexpected(asset_validation_error("PNG file size is outside the supported limit", path));
    }
    std::ifstream stream{path, std::ios::binary};
    std::array<std::uint8_t, 24U> header{};
    if (!stream.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()))) {
        return std::unexpected(asset_validation_error("PNG header could not be read", path));
    }
    constexpr std::array<std::uint8_t, 8U> signature{137U, 80U, 78U, 71U, 13U, 10U, 26U, 10U};
    if (!std::equal(signature.begin(), signature.end(), header.begin()) || big_u32(header.data() + 8U) != 13U ||
        std::memcmp(header.data() + 12U, "IHDR", 4U) != 0) {
        return std::unexpected(asset_validation_error("PNG signature or IHDR chunk is invalid", path));
    }
    const auto width = big_u32(header.data() + 16U);
    const auto height = big_u32(header.data() + 20U);
    const std::uint64_t decoded = static_cast<std::uint64_t>(width) * height * 4U;
    if (width == 0U || height == 0U || width > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
        height > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) || decoded > maximum_decoded_bytes) {
        return std::unexpected(asset_validation_error("PNG dimensions exceed the decoded-image limit", path));
    }
    return AssetDecodeInfo{size, decoded};
}

Result<AssetDecodeInfo> preflight_wav(const std::filesystem::path& path) {
    std::error_code error{};
    const auto size = std::filesystem::file_size(path, error);
    if (error || size < 12U || size > maximum_source_bytes) {
        return std::unexpected(asset_validation_error("WAV file size is outside the supported limit", path));
    }
    std::ifstream stream{path, std::ios::binary};
    std::array<std::uint8_t, 12U> riff{};
    if (!stream.read(reinterpret_cast<char*>(riff.data()), static_cast<std::streamsize>(riff.size())) ||
        std::memcmp(riff.data(), "RIFF", 4U) != 0 || std::memcmp(riff.data() + 8U, "WAVE", 4U) != 0) {
        return std::unexpected(asset_validation_error("WAV RIFF header is invalid", path));
    }
    const std::uint64_t declared_riff_size = static_cast<std::uint64_t>(little_u32(riff.data() + 4U)) + 8U;
    if (declared_riff_size > size || declared_riff_size < 12U) {
        return std::unexpected(asset_validation_error("WAV RIFF size exceeds the file boundary", path));
    }
    bool found_format = false;
    bool found_data = false;
    std::uint16_t format = 0U;
    std::uint16_t channels = 0U;
    std::uint16_t block_alignment = 0U;
    std::uint16_t bits_per_sample = 0U;
    std::uint32_t sample_rate = 0U;
    std::uint32_t byte_rate = 0U;
    std::uint32_t data_size = 0U;
    std::uint64_t offset = 12U;
    while (offset + 8U <= size) {
        stream.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        std::array<std::uint8_t, 8U> chunk{};
        if (!stream.read(reinterpret_cast<char*>(chunk.data()), static_cast<std::streamsize>(chunk.size()))) {
            return std::unexpected(asset_validation_error("WAV chunk header could not be read", path));
        }
        const auto chunk_size = little_u32(chunk.data() + 4U);
        const std::uint64_t payload = offset + 8U;
        if (payload + chunk_size > size) {
            return std::unexpected(asset_validation_error("WAV chunk exceeds the file boundary", path));
        }
        if (std::memcmp(chunk.data(), "fmt ", 4U) == 0) {
            if (found_format || chunk_size < 16U) {
                return std::unexpected(asset_validation_error("WAV format chunk is too small", path));
            }
            stream.seekg(static_cast<std::streamoff>(payload), std::ios::beg);
            std::array<std::uint8_t, 16U> description{};
            if (!stream.read(reinterpret_cast<char*>(description.data()), static_cast<std::streamsize>(description.size()))) {
                return std::unexpected(asset_validation_error("WAV format chunk could not be read", path));
            }
            format = little_u16(description.data());
            channels = little_u16(description.data() + 2U);
            sample_rate = little_u32(description.data() + 4U);
            byte_rate = little_u32(description.data() + 8U);
            block_alignment = little_u16(description.data() + 12U);
            bits_per_sample = little_u16(description.data() + 14U);
            found_format = true;
        } else if (std::memcmp(chunk.data(), "data", 4U) == 0) {
            if (found_data) {
                return std::unexpected(asset_validation_error("WAV contains multiple data chunks", path));
            }
            data_size = chunk_size;
            found_data = true;
        }
        offset = payload + chunk_size + (chunk_size & 1U);
    }
    const bool supported_format = format == 1U || format == 3U;
    const bool supported_depth = format == 1U
                                     ? (bits_per_sample == 8U || bits_per_sample == 16U ||
                                        bits_per_sample == 24U || bits_per_sample == 32U)
                                     : (bits_per_sample == 32U || bits_per_sample == 64U);
    const std::uint64_t expected_alignment =
        static_cast<std::uint64_t>(channels) * ((bits_per_sample + 7U) / 8U);
    const std::uint64_t expected_byte_rate = static_cast<std::uint64_t>(sample_rate) * expected_alignment;
    if (!found_format || !found_data || !supported_format || channels == 0U || channels > 8U ||
        sample_rate < 8'000U || sample_rate > 384'000U || block_alignment == 0U || bits_per_sample == 0U ||
        !supported_depth || expected_alignment != block_alignment || expected_byte_rate != byte_rate ||
        data_size == 0U || data_size > maximum_source_bytes || data_size % block_alignment != 0U) {
        return std::unexpected(asset_validation_error("WAV format or sample data is unsupported", path));
    }
    const std::uint64_t source_frames = data_size / block_alignment;
    const std::uint64_t converted_frames =
        (source_frames * WavePcmF32::sample_rate + sample_rate - 1U) / sample_rate;
    // Leave bounded headroom for the resampler's filter delay so SDL cannot
    // cross the decoded-data ceiling before we receive its output length.
    constexpr std::uint64_t resampler_headroom_frames = 4'096U;
    const std::uint64_t converted_bytes =
        (converted_frames + resampler_headroom_frames) * WavePcmF32::channel_count * sizeof(float);
    if (converted_bytes == 0U || converted_bytes > maximum_decoded_bytes) {
        return std::unexpected(asset_validation_error("WAV converted PCM exceeds the supported limit", path));
    }
    return AssetDecodeInfo{size, converted_bytes};
}

struct SurfaceDeleter final {
    void operator()(SDL_Surface* surface) const noexcept { SDL_DestroySurface(surface); }
};

} // namespace

Result<AssetDecodeInfo> preflight_png_asset(const std::filesystem::path& path) {
    try {
        return preflight_png(path);
    } catch (const std::exception& exception) {
        auto diagnostic = asset_validation_error("PNG preflight raised a standard-library exception", path);
        diagnostic.context.push_back({"exception", std::string{exception.what()}});
        return std::unexpected(std::move(diagnostic));
    }
}

Result<AssetDecodeInfo> preflight_wav_asset(const std::filesystem::path& path) {
    try {
        return preflight_wav(path);
    } catch (const std::exception& exception) {
        auto diagnostic = asset_validation_error("WAV preflight raised a standard-library exception", path);
        diagnostic.context.push_back({"exception", std::string{exception.what()}});
        return std::unexpected(std::move(diagnostic));
    }
}

Result<ImageRgba8> load_png_rgba8(const std::filesystem::path& path) {
    try {
    if (auto checked = preflight_png(path); !checked) return std::unexpected(std::move(checked.error()));
    const auto utf8_path = path.u8string();
    const auto* path_text = reinterpret_cast<const char*>(utf8_path.c_str());
    std::unique_ptr<SDL_Surface, SurfaceDeleter> loaded{SDL_LoadPNG(path_text)};
    if (!loaded) {
        return std::unexpected(asset_error(DiagnosticCode::asset_decode_failed, "SDL_LoadPNG failed", path));
    }
    std::unique_ptr<SDL_Surface, SurfaceDeleter> converted{SDL_ConvertSurface(loaded.get(), SDL_PIXELFORMAT_RGBA32)};
    if (!converted || converted->w <= 0 || converted->h <= 0 || converted->pixels == nullptr || converted->pitch < converted->w * 4) {
        return std::unexpected(asset_error(
            DiagnosticCode::asset_decode_failed, "PNG could not be converted to RGBA8", path));
    }
    const auto width = static_cast<std::uint32_t>(converted->w);
    const auto height = static_cast<std::uint32_t>(converted->h);
    const auto byte_count = static_cast<std::uint64_t>(width) * height * 4U;
    if (byte_count > 256ULL * 1024ULL * 1024ULL || byte_count > std::numeric_limits<std::size_t>::max()) {
        return std::unexpected(asset_error(
            DiagnosticCode::asset_decode_failed, "PNG dimensions exceed the supported limit", path));
    }
    ImageRgba8 image{};
    image.width = width;
    image.height = height;
    image.pixels.resize(static_cast<std::size_t>(byte_count));
    const auto* source = static_cast<const std::uint8_t*>(converted->pixels);
    const auto row_bytes = static_cast<std::size_t>(width) * 4U;
    for (std::uint32_t row = 0U; row < height; ++row) {
        std::memcpy(
            image.pixels.data() + static_cast<std::size_t>(row) * row_bytes,
            source + static_cast<std::size_t>(row) * static_cast<std::size_t>(converted->pitch),
            row_bytes);
    }
    return image;
    } catch (const std::exception& exception) {
        auto diagnostic = asset_validation_error("PNG loading raised a standard-library exception", path);
        diagnostic.context.push_back({"exception", std::string{exception.what()}});
        return std::unexpected(std::move(diagnostic));
    }
}

Result<WavePcmF32> load_wav_f32_stereo(const std::filesystem::path& path) {
    try {
    if (auto checked = preflight_wav(path); !checked) return std::unexpected(std::move(checked.error()));
    const auto utf8_path = path.u8string();
    const auto* path_text = reinterpret_cast<const char*>(utf8_path.c_str());
    SDL_AudioSpec source_spec{};
    std::uint8_t* source_data = nullptr;
    std::uint32_t source_length = 0U;
    if (!SDL_LoadWAV(path_text, &source_spec, &source_data, &source_length)) {
        return std::unexpected(asset_error(DiagnosticCode::asset_decode_failed, "SDL_LoadWAV failed", path));
    }
    struct AudioBuffer final {
        std::uint8_t* pointer;
        ~AudioBuffer() { SDL_free(pointer); }
    } source{source_data};
    if (source_length == 0U || source_length > maximum_source_bytes) {
        return std::unexpected(asset_error(
            DiagnosticCode::asset_decode_failed, "WAV sample data exceeds the supported limit", path));
    }
    constexpr SDL_AudioSpec target_spec{SDL_AUDIO_F32, 2, 48'000};
    std::uint8_t* converted_data = nullptr;
    int converted_length = 0;
    if (!SDL_ConvertAudioSamples(
            &source_spec,
            source_data,
            static_cast<int>(source_length),
            &target_spec,
            &converted_data,
            &converted_length)) {
        return std::unexpected(asset_error(
            DiagnosticCode::asset_decode_failed, "WAV conversion to stereo float PCM failed", path));
    }
    AudioBuffer converted{converted_data};
    if (converted_length <= 0 || converted_length % static_cast<int>(sizeof(float) * 2U) != 0 ||
        static_cast<std::uint64_t>(converted_length) > maximum_decoded_bytes) {
        return std::unexpected(asset_error(
            DiagnosticCode::asset_decode_failed, "Converted WAV sample data is invalid", path));
    }
    WavePcmF32 wave{};
    const auto sample_count = static_cast<std::size_t>(converted_length) / sizeof(float);
    wave.samples.resize(sample_count);
    std::memcpy(wave.samples.data(), converted_data, static_cast<std::size_t>(converted_length));
    return wave;
    } catch (const std::exception& exception) {
        auto diagnostic = asset_validation_error("WAV loading raised a standard-library exception", path);
        diagnostic.context.push_back({"exception", std::string{exception.what()}});
        return std::unexpected(std::move(diagnostic));
    }
}

} // namespace ai2d
