#pragma once

#include "ai2d/foundation/diagnostic.hpp"
#include "ai2d/platform/assets.hpp"

#include <cstdint>
#include <memory>

namespace ai2d {

class AudioMixer final {
public:
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
    [[nodiscard]] bool initialized() const noexcept;
    [[nodiscard]] std::uint32_t active_voice_count() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ai2d
