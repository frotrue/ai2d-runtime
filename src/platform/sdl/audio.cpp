#include "ai2d/platform/audio.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ai2d {
namespace {

Diagnostic audio_error(const DiagnosticCode code, const char* const message) {
    auto diagnostic = Diagnostic::make(code, Severity::error, "audio", message);
    diagnostic.context.push_back({"sdl_error", std::string{SDL_GetError()}});
    return diagnostic;
}

} // namespace

class AudioMixer::Impl final {
public:
    struct Clip final {
        std::vector<float> samples{};
        bool ready{false};
    };
    struct Voice final {
        std::uint32_t clip_id{0U};
        std::size_t cursor{0U};
        float gain{1.0F};
        bool active{false};
    };

    SDL_AudioStream* stream{nullptr};
    std::vector<Clip> clips{};
    std::vector<Voice> voices{};
    std::vector<float> mix_buffer{};
    std::atomic<std::uint32_t> active_voices{0U};
    float master_volume{1.0F};
    bool owns_audio{false};
    bool ready{false};

    ~Impl() {
        if (stream != nullptr) {
            SDL_DestroyAudioStream(stream);
        }
        if (owns_audio) {
            SDL_QuitSubSystem(SDL_INIT_AUDIO);
        }
    }

    static void SDLCALL audio_callback(
        void* userdata,
        SDL_AudioStream* callback_stream,
        const int additional_amount,
        const int total_amount) {
        (void)total_amount;
        static_cast<Impl*>(userdata)->mix(callback_stream, additional_amount);
    }

    void mix(SDL_AudioStream* callback_stream, const int additional_amount) noexcept {
        constexpr std::size_t frame_samples = 2U;
        constexpr std::size_t bytes_per_frame = sizeof(float) * frame_samples;
        std::size_t remaining_frames = additional_amount <= 0
                                           ? 0U
                                           : static_cast<std::size_t>(additional_amount) / bytes_per_frame;
        while (remaining_frames > 0U) {
            const auto buffer_frames = mix_buffer.size() / frame_samples;
            const auto frame_count = std::min(remaining_frames, buffer_frames);
            const auto sample_count = frame_count * frame_samples;
            std::fill_n(mix_buffer.data(), sample_count, 0.0F);
            for (auto& voice : voices) {
                if (!voice.active || voice.clip_id >= clips.size() || !clips[voice.clip_id].ready) continue;
                const auto& samples = clips[voice.clip_id].samples;
                const auto available = samples.size() - std::min(voice.cursor, samples.size());
                const auto copied = std::min(sample_count, available);
                for (std::size_t index = 0U; index < copied; ++index) {
                    mix_buffer[index] += samples[voice.cursor + index] * voice.gain * master_volume;
                }
                voice.cursor += copied;
                if (voice.cursor >= samples.size()) {
                    voice.active = false;
                    active_voices.fetch_sub(1U, std::memory_order_relaxed);
                }
            }
            for (std::size_t index = 0U; index < sample_count; ++index) {
                mix_buffer[index] = std::clamp(mix_buffer[index], -1.0F, 1.0F);
            }
            if (!SDL_PutAudioStreamData(
                    callback_stream, mix_buffer.data(), static_cast<int>(sample_count * sizeof(float)))) {
                return;
            }
            remaining_frames -= frame_count;
        }
    }
};

AudioMixer::AudioMixer() : impl_(std::make_unique<Impl>()) {}
AudioMixer::~AudioMixer() = default;
AudioMixer::AudioMixer(AudioMixer&&) noexcept = default;
AudioMixer& AudioMixer::operator=(AudioMixer&&) noexcept = default;

Result<void> AudioMixer::initialize(const std::uint32_t max_clips, const std::uint32_t max_voices) {
    if (impl_->ready || max_clips == 0U || max_clips > 256U || max_voices == 0U || max_voices > 64U) {
        return std::unexpected(audio_error(DiagnosticCode::input_invalid, "Audio mixer configuration is invalid"));
    }
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        return std::unexpected(audio_error(
            DiagnosticCode::audio_device_unavailable, "SDL audio initialization failed"));
    }
    impl_->owns_audio = true;
    impl_->clips.resize(max_clips);
    impl_->voices.resize(max_voices);
    impl_->mix_buffer.resize(8'192U, 0.0F);
    constexpr SDL_AudioSpec target_spec{SDL_AUDIO_F32, 2, 48'000};
    impl_->stream = SDL_OpenAudioDeviceStream(
        SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &target_spec, &Impl::audio_callback, impl_.get());
    if (impl_->stream == nullptr) {
        return std::unexpected(audio_error(
            DiagnosticCode::audio_device_unavailable, "Default audio playback device could not be opened"));
    }
    if (!SDL_ResumeAudioStreamDevice(impl_->stream)) {
        return std::unexpected(audio_error(
            DiagnosticCode::audio_device_unavailable, "Audio playback device could not be resumed"));
    }
    impl_->ready = true;
    return {};
}

Result<void> AudioMixer::register_clip(const std::uint32_t clip_id, WavePcmF32 clip) {
    if (!impl_->ready || clip_id >= impl_->clips.size() || clip.samples.empty() || clip.samples.size() % 2U != 0U) {
        return std::unexpected(audio_error(DiagnosticCode::input_invalid, "Audio clip registration input is invalid"));
    }
    if (!SDL_LockAudioStream(impl_->stream)) {
        return std::unexpected(audio_error(DiagnosticCode::internal_error, "Audio stream lock failed"));
    }
    impl_->clips[clip_id].samples = std::move(clip.samples);
    impl_->clips[clip_id].ready = true;
    (void)SDL_UnlockAudioStream(impl_->stream);
    return {};
}

Result<void> AudioMixer::play(const std::uint32_t clip_id, const float gain) {
    if (!impl_->ready || clip_id >= impl_->clips.size() || !impl_->clips[clip_id].ready || !std::isfinite(gain) ||
        gain < 0.0F || gain > 4.0F) {
        return std::unexpected(audio_error(DiagnosticCode::input_invalid, "Audio playback input is invalid"));
    }
    if (!SDL_LockAudioStream(impl_->stream)) {
        return std::unexpected(audio_error(DiagnosticCode::internal_error, "Audio stream lock failed"));
    }
    auto voice = std::find_if(impl_->voices.begin(), impl_->voices.end(), [](const Impl::Voice& item) {
        return !item.active;
    });
    if (voice == impl_->voices.end()) {
        (void)SDL_UnlockAudioStream(impl_->stream);
        return std::unexpected(audio_error(
            DiagnosticCode::audio_voice_capacity_exceeded, "All configured audio voices are active"));
    }
    *voice = {clip_id, 0U, gain, true};
    impl_->active_voices.fetch_add(1U, std::memory_order_relaxed);
    (void)SDL_UnlockAudioStream(impl_->stream);
    return {};
}

Result<void> AudioMixer::set_master_volume(const float gain) {
    if (!impl_->ready || !std::isfinite(gain) || gain < 0.0F || gain > 1.0F) {
        return std::unexpected(audio_error(DiagnosticCode::input_invalid, "Master volume is invalid"));
    }
    if (!SDL_LockAudioStream(impl_->stream)) {
        return std::unexpected(audio_error(DiagnosticCode::internal_error, "Audio stream lock failed"));
    }
    impl_->master_volume = gain;
    (void)SDL_UnlockAudioStream(impl_->stream);
    return {};
}

bool AudioMixer::initialized() const noexcept { return impl_->ready; }
std::uint32_t AudioMixer::active_voice_count() const noexcept {
    return impl_->active_voices.load(std::memory_order_relaxed);
}

} // namespace ai2d
