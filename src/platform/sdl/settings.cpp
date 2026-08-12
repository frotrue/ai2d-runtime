#include "ai2d/platform/settings.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#else
#include <unistd.h>
#endif

namespace ai2d {
namespace {

Diagnostic settings_error(const DiagnosticCode code, const char* const message) {
    return Diagnostic::make(code, Severity::error, "settings", message);
}

Result<RenderFpsCap> parse_cap(const std::string_view text) {
    if (text == "60") return RenderFpsCap::fps_60;
    if (text == "120") return RenderFpsCap::fps_120;
    if (text == "144") return RenderFpsCap::fps_144;
    if (text == "240") return RenderFpsCap::fps_240;
    if (text == "unlimited") return RenderFpsCap::unlimited;
    return std::unexpected(settings_error(DiagnosticCode::settings_invalid, "Saved FPS cap is invalid"));
}

std::string_view cap_text(const RenderFpsCap cap) noexcept {
    switch (cap) {
    case RenderFpsCap::fps_60: return "60";
    case RenderFpsCap::fps_120: return "120";
    case RenderFpsCap::fps_144: return "144";
    case RenderFpsCap::fps_240: return "240";
    case RenderFpsCap::unlimited: return "unlimited";
    }
    return "60";
}

std::uint64_t process_identifier() noexcept {
#if defined(_WIN32)
    return static_cast<std::uint64_t>(GetCurrentProcessId());
#else
    return static_cast<std::uint64_t>(getpid());
#endif
}

class SettingsJsonParser final {
public:
    explicit SettingsJsonParser(const std::string_view source) noexcept : source_(source) {}

    Result<GameSettings> parse() {
        skip_space();
        if (!consume('{')) return failure("Saved settings must be a JSON object");
        GameSettings settings{};
        bool has_version = false;
        bool has_fps = false;
        bool has_volume = false;
        skip_space();
        if (consume('}')) return failure("Saved settings fields are missing");
        while (true) {
            auto key = parse_string();
            if (!key) return std::unexpected(std::move(key.error()));
            skip_space();
            if (!consume(':')) return failure("Saved setting is missing a colon");
            skip_space();
            if (*key == "schema_version") {
                if (has_version) return failure("Saved settings contain a duplicate schema_version");
                auto version = parse_u64();
                if (!version) return std::unexpected(std::move(version.error()));
                if (*version != 1U) return failure("Unsupported settings version");
                has_version = true;
            } else if (*key == "fps_cap") {
                if (has_fps) return failure("Saved settings contain a duplicate fps_cap");
                auto text = parse_string();
                if (!text) return std::unexpected(std::move(text.error()));
                auto cap = parse_cap(*text);
                if (!cap) return std::unexpected(std::move(cap.error()));
                settings.fps_cap = *cap;
                has_fps = true;
            } else if (*key == "master_volume") {
                if (has_volume) return failure("Saved settings contain a duplicate master_volume");
                auto volume = parse_float();
                if (!volume) return std::unexpected(std::move(volume.error()));
                settings.master_volume = *volume;
                has_volume = true;
            } else {
                return failure("Saved settings contain an unknown field");
            }
            skip_space();
            if (consume('}')) break;
            if (!consume(',')) return failure("Saved settings object is malformed");
            skip_space();
        }
        skip_space();
        if (offset_ != source_.size()) return failure("Saved settings contain trailing data");
        if (!has_version || !has_fps || !has_volume) return failure("Saved setting field is missing");
        if (!std::isfinite(settings.master_volume) || settings.master_volume < 0.0F ||
            settings.master_volume > 1.0F) {
            return failure("Saved master volume is invalid");
        }
        return settings;
    }

private:
    Result<GameSettings> failure(const char* const message) const {
        return std::unexpected(settings_error(DiagnosticCode::settings_invalid, message));
    }

    Result<std::string> parse_string() {
        if (!consume('"')) {
            return std::unexpected(settings_error(
                DiagnosticCode::settings_invalid, "Saved setting string is malformed"));
        }
        std::string value{};
        while (offset_ < source_.size()) {
            const char character = source_[offset_++];
            if (character == '"') return value;
            if (static_cast<unsigned char>(character) < 0x20U) {
                return std::unexpected(settings_error(
                    DiagnosticCode::settings_invalid, "Saved setting string contains a control character"));
            }
            if (character != '\\') {
                value.push_back(character);
                continue;
            }
            if (offset_ >= source_.size()) {
                return std::unexpected(settings_error(
                    DiagnosticCode::settings_invalid, "Saved setting escape is incomplete"));
            }
            const char escape = source_[offset_++];
            switch (escape) {
            case '"': value.push_back('"'); break;
            case '\\': value.push_back('\\'); break;
            case '/': value.push_back('/'); break;
            case 'b': value.push_back('\b'); break;
            case 'f': value.push_back('\f'); break;
            case 'n': value.push_back('\n'); break;
            case 'r': value.push_back('\r'); break;
            case 't': value.push_back('\t'); break;
            default:
                return std::unexpected(settings_error(
                    DiagnosticCode::settings_invalid, "Saved setting escape is unsupported"));
            }
        }
        return std::unexpected(settings_error(
            DiagnosticCode::settings_invalid, "Saved setting string is unterminated"));
    }

    Result<std::uint64_t> parse_u64() {
        const auto begin = source_.data() + offset_;
        std::uint64_t value = 0U;
        const auto parsed = std::from_chars(begin, source_.data() + source_.size(), value);
        if (parsed.ec != std::errc{} || parsed.ptr == begin) {
            return std::unexpected(settings_error(
                DiagnosticCode::settings_invalid, "Saved settings version is invalid"));
        }
        offset_ = static_cast<std::size_t>(parsed.ptr - source_.data());
        return value;
    }

    Result<float> parse_float() {
        const auto begin = source_.data() + offset_;
        float value = 0.0F;
        const auto parsed = std::from_chars(begin, source_.data() + source_.size(), value);
        if (parsed.ec != std::errc{} || parsed.ptr == begin || !std::isfinite(value)) {
            return std::unexpected(settings_error(
                DiagnosticCode::settings_invalid, "Saved setting number is invalid"));
        }
        offset_ = static_cast<std::size_t>(parsed.ptr - source_.data());
        return value;
    }

    void skip_space() noexcept {
        while (offset_ < source_.size() &&
               (source_[offset_] == ' ' || source_[offset_] == '\t' || source_[offset_] == '\r' ||
                source_[offset_] == '\n')) {
            ++offset_;
        }
    }

    bool consume(const char expected) noexcept {
        if (offset_ >= source_.size() || source_[offset_] != expected) return false;
        ++offset_;
        return true;
    }

    std::string_view source_{};
    std::size_t offset_{0U};
};

} // namespace

Result<std::filesystem::path> settings_file_path(
    const std::string_view organization,
    const std::string_view application) {
    if (organization.empty() || application.empty() || organization.find('\0') != std::string_view::npos ||
        application.find('\0') != std::string_view::npos) {
        return std::unexpected(settings_error(DiagnosticCode::settings_invalid, "Preference names are invalid"));
    }
    const std::string organization_text{organization};
    const std::string application_text{application};
    char* raw_path = SDL_GetPrefPath(organization_text.c_str(), application_text.c_str());
    if (raw_path == nullptr) {
        return std::unexpected(settings_error(DiagnosticCode::settings_invalid, "SDL preference path is unavailable"));
    }
    const std::u8string utf8_path{reinterpret_cast<const char8_t*>(raw_path)};
    const std::filesystem::path path = std::filesystem::path{utf8_path} / "settings.json";
    SDL_free(raw_path);
    return path;
}

Result<std::optional<GameSettings>> load_game_settings(
    const std::string_view organization,
    const std::string_view application) {
    auto path = settings_file_path(organization, application);
    if (!path) return std::unexpected(std::move(path.error()));
    std::error_code error{};
    if (!std::filesystem::exists(*path, error) && !error) return std::optional<GameSettings>{};
    if (error || std::filesystem::file_size(*path, error) > 16U * 1024U || error) {
        return std::unexpected(settings_error(DiagnosticCode::settings_invalid, "Settings file is unreadable or too large"));
    }
    std::ifstream stream{*path, std::ios::binary};
    if (!stream) {
        return std::unexpected(settings_error(DiagnosticCode::settings_invalid, "Settings file could not be opened"));
    }
    const std::string source{std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
    auto settings = SettingsJsonParser{source}.parse();
    if (!settings) return std::unexpected(std::move(settings.error()));
    return std::optional<GameSettings>{*settings};
}

Result<void> save_game_settings(
    const std::string_view organization,
    const std::string_view application,
    const GameSettings& settings) {
    if (!std::isfinite(settings.master_volume) || settings.master_volume < 0.0F || settings.master_volume > 1.0F) {
        return std::unexpected(settings_error(DiagnosticCode::settings_invalid, "Master volume is invalid"));
    }
    // Settings replacement is process-global state. Serialize local writers so a
    // transient sharing violation cannot turn a valid concurrent save into a failure.
    static std::mutex settings_write_mutex{};
    const std::scoped_lock write_lock{settings_write_mutex};
    auto path = settings_file_path(organization, application);
    if (!path) return std::unexpected(std::move(path.error()));
    auto temporary = *path;
    static std::atomic<std::uint64_t> temporary_counter{0U};
    temporary += ".tmp." + std::to_string(process_identifier()) + "." +
                 std::to_string(temporary_counter.fetch_add(1U, std::memory_order_relaxed));
    {
        std::ofstream stream{temporary, std::ios::binary | std::ios::trunc};
        if (!stream) {
            return std::unexpected(settings_error(DiagnosticCode::settings_write_failed, "Temporary settings file could not be opened"));
        }
        stream << "{\"schema_version\":1,\"fps_cap\":\"" << cap_text(settings.fps_cap)
               << "\",\"master_volume\":" << settings.master_volume << "}\n";
        stream.flush();
        if (!stream) {
            return std::unexpected(settings_error(DiagnosticCode::settings_write_failed, "Temporary settings file could not be written"));
        }
    }
#if defined(_WIN32)
    bool replaced = false;
    for (std::uint32_t attempt = 0U; attempt < 16U; ++attempt) {
        if (MoveFileExW(
                temporary.c_str(), path->c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            replaced = true;
            break;
        }
        const auto failure = GetLastError();
        if (failure != ERROR_ACCESS_DENIED && failure != ERROR_SHARING_VIOLATION) break;
        Sleep(1U << std::min(attempt, 4U));
    }
    if (!replaced) {
        std::error_code remove_error{};
        std::filesystem::remove(temporary, remove_error);
        return std::unexpected(settings_error(DiagnosticCode::settings_write_failed, "Atomic settings replacement failed"));
    }
#else
    std::error_code rename_error{};
    std::filesystem::rename(temporary, *path, rename_error);
    if (rename_error) {
        std::filesystem::remove(temporary, rename_error);
        return std::unexpected(settings_error(DiagnosticCode::settings_write_failed, "Atomic settings replacement failed"));
    }
#endif
    return {};
}

} // namespace ai2d
