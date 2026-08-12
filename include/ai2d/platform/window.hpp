#pragma once

#include "ai2d/foundation/diagnostic.hpp"

#include <cstdint>
#include <memory>

namespace ai2d {

struct WindowExtent final {
    std::uint32_t width{0U};
    std::uint32_t height{0U};
};

enum class InputKey : std::uint8_t { escape, left, right, up, down, a, d, space, enter, tab, count };

struct InputSnapshot final {
    bool quit_requested{false};
    bool resized{false};
    bool minimized{false};
    bool restored{false};
    bool mouse_left{false};
    bool mouse_left_pressed{false};
    bool mouse_left_released{false};
    float mouse_x{0.0F};
    float mouse_y{0.0F};
    bool keys[static_cast<std::uint8_t>(InputKey::count)]{};
    bool pressed_keys[static_cast<std::uint8_t>(InputKey::count)]{};
    bool released_keys[static_cast<std::uint8_t>(InputKey::count)]{};
    WindowExtent drawable_extent{};

    [[nodiscard]] bool down(InputKey key) const noexcept {
        return keys[static_cast<std::uint8_t>(key)];
    }
    [[nodiscard]] bool pressed(InputKey key) const noexcept {
        return pressed_keys[static_cast<std::uint8_t>(key)];
    }
    [[nodiscard]] bool released(InputKey key) const noexcept {
        return released_keys[static_cast<std::uint8_t>(key)];
    }
};

struct WindowOptions final {
    const char* title{"game_runtime_dev"};
    std::uint32_t width{1280U};
    std::uint32_t height{720U};
    bool hidden{false};
    bool resizable{true};
};

class PlatformWindow final {
  public:
    PlatformWindow();
    ~PlatformWindow();

    PlatformWindow(const PlatformWindow&) = delete;
    PlatformWindow& operator=(const PlatformWindow&) = delete;
    PlatformWindow(PlatformWindow&&) noexcept;
    PlatformWindow& operator=(PlatformWindow&&) noexcept;

    [[nodiscard]] Result<void> open(const WindowOptions& options);
    [[nodiscard]] Result<InputSnapshot> poll_input();
    [[nodiscard]] WindowExtent drawable_extent() const noexcept;
    [[nodiscard]] void* native_handle() const noexcept;
    [[nodiscard]] bool is_open() const noexcept;
    void show() noexcept;
    [[nodiscard]] Result<void> set_extent(std::uint32_t width, std::uint32_t height);

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ai2d
