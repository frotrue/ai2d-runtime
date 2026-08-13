#pragma once

#include "ai2d/foundation/diagnostic.hpp"
#include "ai2d/platform/assets.hpp"

#include <cstdint>
#include <memory>
#include <span>

namespace ai2d {

class AudioMixer final {
public:
    static constexpr std::uint32_t music_buffer_capacity_frames = 32'768U;

    AudioMixer();
    ~AudioMixer();
    AudioMixer(const AudioMixer&) = delete;
    AudioMixer& operator=(const AudioMixer&) = delete;
    AudioMixer(AudioMixer&&) noexcept;
    AudioMixer& operator=(AudioMixer&&) noexcept;

    [[nodiscard]] Result<void> initialize(std::uint32_t max_clips, std::uint32_t max_voices = 16U);
    [[nodiscard]] Result<void> register_clip(std::uint32_t clip_id, WavePcmF32 clip);
    [[nodiscard]] Result<void> play(std::uint32_t clip_id, float gain = 1.0F);
    [[nodiscard]] Result<void> set_master_volume(float gain);
    [[nodiscard]] Result<std::uint32_t> queue_music(std::span<const float> interleaved_stereo);
    [[nodiscard]] Result<void> start_music(float gain = 1.0F);
    [[nodiscard]] Result<void> stop_music();
    [[nodiscard]] Result<void> set_music_volume(float gain);
    [[nodiscard]] std::uint32_t music_free_frames();
    [[nodiscard]] std::uint64_t music_underruns() const noexcept;
    [[nodiscard]] bool initialized() const noexcept;
    [[nodiscard]] std::uint32_t active_voice_count() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ai2d
