#define NOMINMAX
#include <windows.h>
#include <shellapi.h>

#include "ai2d/runtime/game_runtime.hpp"
#include "ai2d/scenario/game.hpp"

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>

namespace {

std::wstring widen(const std::string_view text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) return L"The runtime reported an error that could not be converted to Unicode.";
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), size);
    return result;
}

int fail(const std::string_view message) {
    const auto wide = widen(message);
    MessageBoxW(nullptr, wide.c_str(), L"AI2D Player", MB_OK | MB_ICONERROR | MB_TASKMODAL);
    return 1;
}

std::filesystem::path executable_path() {
    std::array<wchar_t, 32'768U> buffer{};
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0U || length >= buffer.size()) return {};
    return std::filesystem::path{std::wstring_view{buffer.data(), length}};
}

struct PlayerArguments final {
    std::uint32_t frames{0U};
    bool hidden{false};
    bool audio{true};
    bool saved_settings{true};
};

bool parse_arguments(PlayerArguments& output) {
    int count = 0;
    wchar_t** values = CommandLineToArgvW(GetCommandLineW(), &count);
    if (values == nullptr) return false;
    bool valid = true;
    for (int index = 1; index < count && valid; ++index) {
        const std::wstring_view option{values[index]};
        if (option == L"--hidden") output.hidden = true;
        else if (option == L"--no-audio") output.audio = false;
        else if (option == L"--no-saved-settings") output.saved_settings = false;
        else if (option == L"--frames" && index + 1 < count) {
            wchar_t* end = nullptr;
            errno = 0;
            const auto parsed = std::wcstoul(values[++index], &end, 10);
            valid = errno == 0 && end != values[index] && *end == L'\0' && parsed > 0UL && parsed <= 10'000'000UL;
            if (valid) output.frames = static_cast<std::uint32_t>(parsed);
        } else {
            valid = false;
        }
    }
    LocalFree(values);
    return valid;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    PlayerArguments arguments{};
    if (!parse_arguments(arguments)) {
        return fail("usage: <game>.exe [--hidden] [--frames N] [--no-audio] [--no-saved-settings]");
    }
    const auto executable = executable_path();
    if (executable.empty()) return fail("The executable path could not be resolved.");
    const auto manifest = executable.parent_path() / L"content" / L"game.json";
    auto plan = ai2d::compile_game_file(manifest);
    if (!plan) return fail(plan.error().message);

    ai2d::GameRuntime runtime{};
    ai2d::GameRuntimeOptions options{};
    options.hidden = arguments.hidden;
    options.enable_audio = arguments.audio;
    options.load_saved_settings = arguments.saved_settings;
    auto loaded = runtime.initialize(*plan, options);
    if (!loaded) return fail(loaded.error().message);

    std::uint32_t frames = 0U;
    while (arguments.frames == 0U || frames < arguments.frames) {
        auto frame = runtime.run_frame();
        if (!frame) return fail(frame.error().message);
        ++frames;
        if (frame->quit_requested) break;
    }
    if (auto idle = runtime.wait_idle(); !idle) return fail(idle.error().message);
    return 0;
}
