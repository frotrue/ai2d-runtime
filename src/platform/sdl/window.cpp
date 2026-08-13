#include "ai2d/platform/window.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

namespace ai2d {
namespace {

constexpr std::array<SDL_Scancode, static_cast<std::uint8_t>(InputKey::count)> input_scancodes{
    SDL_SCANCODE_ESCAPE,
    SDL_SCANCODE_LEFT,
    SDL_SCANCODE_RIGHT,
    SDL_SCANCODE_UP,
    SDL_SCANCODE_DOWN,
    SDL_SCANCODE_A,
    SDL_SCANCODE_D,
    SDL_SCANCODE_W,
    SDL_SCANCODE_S,
    SDL_SCANCODE_Q,
    SDL_SCANCODE_E,
    SDL_SCANCODE_R,
    SDL_SCANCODE_F,
    SDL_SCANCODE_LSHIFT,
    SDL_SCANCODE_LCTRL,
    SDL_SCANCODE_SPACE,
    SDL_SCANCODE_RETURN,
    SDL_SCANCODE_TAB,
};

constexpr std::array<SDL_GamepadButton, static_cast<std::uint8_t>(InputGamepadButton::count)> input_gamepad_buttons{
    SDL_GAMEPAD_BUTTON_SOUTH,
    SDL_GAMEPAD_BUTTON_EAST,
    SDL_GAMEPAD_BUTTON_WEST,
    SDL_GAMEPAD_BUTTON_NORTH,
    SDL_GAMEPAD_BUTTON_BACK,
    SDL_GAMEPAD_BUTTON_START,
    SDL_GAMEPAD_BUTTON_LEFT_STICK,
    SDL_GAMEPAD_BUTTON_RIGHT_STICK,
    SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,
    SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
    SDL_GAMEPAD_BUTTON_DPAD_UP,
    SDL_GAMEPAD_BUTTON_DPAD_DOWN,
    SDL_GAMEPAD_BUTTON_DPAD_LEFT,
    SDL_GAMEPAD_BUTTON_DPAD_RIGHT,
};

constexpr std::array<SDL_GamepadAxis, static_cast<std::uint8_t>(InputGamepadAxis::count)> input_gamepad_axes{
    SDL_GAMEPAD_AXIS_LEFTX,
    SDL_GAMEPAD_AXIS_LEFTY,
    SDL_GAMEPAD_AXIS_RIGHTX,
    SDL_GAMEPAD_AXIS_RIGHTY,
    SDL_GAMEPAD_AXIS_LEFT_TRIGGER,
    SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,
};

Diagnostic sdl_error(const char* action) {
    auto diagnostic = Diagnostic::make(
        DiagnosticCode::internal_error, Severity::error, "platform", action);
    diagnostic.context.push_back({"sdl_error", std::string{SDL_GetError()}});
    return diagnostic;
}

std::uint32_t checked_extent(const int value) noexcept {
    if (value <= 0) {
        return 0U;
    }
    const auto unsigned_value = static_cast<unsigned int>(value);
    return unsigned_value > std::numeric_limits<std::uint32_t>::max()
               ? std::numeric_limits<std::uint32_t>::max()
               : static_cast<std::uint32_t>(unsigned_value);
}

} // namespace

class PlatformWindow::Impl final {
  public:
    SDL_Window* window{nullptr};
    SDL_Gamepad* gamepad{nullptr};
    bool owns_video{false};
    bool owns_gamepad{false};
    std::array<bool, static_cast<std::uint8_t>(InputKey::count)> previous_keys{};
    std::array<bool, static_cast<std::uint8_t>(InputGamepadButton::count)> previous_gamepad_buttons{};
    bool mouse_left{false};
    float mouse_x{0.0F};
    float mouse_y{0.0F};

    ~Impl() {
        if (window != nullptr) {
            SDL_DestroyWindow(window);
        }
        if (gamepad != nullptr) {
            SDL_CloseGamepad(gamepad);
        }
        if (owns_video) {
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
        }
        if (owns_gamepad) {
            SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
        }
    }
};

PlatformWindow::PlatformWindow() : impl_(std::make_unique<Impl>()) {}
PlatformWindow::~PlatformWindow() = default;
PlatformWindow::PlatformWindow(PlatformWindow&&) noexcept = default;
PlatformWindow& PlatformWindow::operator=(PlatformWindow&&) noexcept = default;

Result<void> PlatformWindow::open(const WindowOptions& options) {
    if (impl_->window != nullptr) {
        return std::unexpected(sdl_error("SDL window is already open"));
    }
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        return std::unexpected(sdl_error("SDL video initialization failed"));
    }
    impl_->owns_video = true;
    if (SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        impl_->owns_gamepad = true;
        int gamepad_count = 0;
        SDL_JoystickID* gamepads = SDL_GetGamepads(&gamepad_count);
        if (gamepads != nullptr && gamepad_count > 0) {
            impl_->gamepad = SDL_OpenGamepad(gamepads[0]);
        }
        SDL_free(gamepads);
    }

    SDL_WindowFlags flags = SDL_WINDOW_VULKAN;
    if (options.hidden) {
        flags |= SDL_WINDOW_HIDDEN;
    }
    if (options.resizable) {
        flags |= SDL_WINDOW_RESIZABLE;
    }
    impl_->window = SDL_CreateWindow(
        options.title,
        static_cast<int>(options.width),
        static_cast<int>(options.height),
        flags);
    if (impl_->window == nullptr) {
        return std::unexpected(sdl_error("SDL Vulkan window creation failed"));
    }
    return {};
}

Result<InputSnapshot> PlatformWindow::poll_input() {
    if (impl_->window == nullptr) {
        return std::unexpected(sdl_error("Cannot poll input before opening the SDL window"));
    }

    InputSnapshot snapshot{};
    snapshot.mouse_left = impl_->mouse_left;
    snapshot.mouse_x = impl_->mouse_x;
    snapshot.mouse_y = impl_->mouse_y;
    SDL_Event event{};
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
        case SDL_EVENT_QUIT: snapshot.quit_requested = true; break;
        case SDL_EVENT_WINDOW_RESIZED:
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: snapshot.resized = true; break;
        case SDL_EVENT_WINDOW_MINIMIZED: snapshot.minimized = true; break;
        case SDL_EVENT_WINDOW_RESTORED: snapshot.restored = true; break;
        case SDL_EVENT_MOUSE_MOTION:
            impl_->mouse_x = event.motion.x;
            impl_->mouse_y = event.motion.y;
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (event.button.button == SDL_BUTTON_LEFT) {
                const bool next = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
                snapshot.mouse_left_pressed = snapshot.mouse_left_pressed || (next && !impl_->mouse_left);
                snapshot.mouse_left_released = snapshot.mouse_left_released || (!next && impl_->mouse_left);
                impl_->mouse_left = next;
                impl_->mouse_x = event.button.x;
                impl_->mouse_y = event.button.y;
            }
            break;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
            for (std::size_t index = 0U; index < input_scancodes.size(); ++index) {
                if (event.key.scancode != input_scancodes[index]) continue;
                const bool next = event.type == SDL_EVENT_KEY_DOWN;
                snapshot.pressed_keys[index] = snapshot.pressed_keys[index] ||
                                               (next && !impl_->previous_keys[index]);
                snapshot.released_keys[index] = snapshot.released_keys[index] ||
                                                (!next && impl_->previous_keys[index]);
                impl_->previous_keys[index] = next;
                break;
            }
            break;
        case SDL_EVENT_GAMEPAD_ADDED:
            if (impl_->gamepad == nullptr) impl_->gamepad = SDL_OpenGamepad(event.gdevice.which);
            break;
        case SDL_EVENT_GAMEPAD_REMOVED:
            if (impl_->gamepad != nullptr && SDL_GetGamepadID(impl_->gamepad) == event.gdevice.which) {
                SDL_CloseGamepad(impl_->gamepad);
                impl_->gamepad = nullptr;
                for (std::size_t index = 0U; index < impl_->previous_gamepad_buttons.size(); ++index) {
                    snapshot.released_gamepad_buttons[index] = impl_->previous_gamepad_buttons[index];
                }
                impl_->previous_gamepad_buttons.fill(false);
            }
            break;
        default: break;
        }
    }
    snapshot.gamepad_connected = impl_->gamepad != nullptr;
    if (impl_->gamepad != nullptr) {
        constexpr float axis_scale = 1.0F / 32767.0F;
        for (std::size_t index = 0U; index < input_gamepad_buttons.size(); ++index) {
            const bool current = SDL_GetGamepadButton(impl_->gamepad, input_gamepad_buttons[index]);
            snapshot.gamepad_buttons[index] = current;
            snapshot.pressed_gamepad_buttons[index] = current && !impl_->previous_gamepad_buttons[index];
            snapshot.released_gamepad_buttons[index] = !current && impl_->previous_gamepad_buttons[index];
            impl_->previous_gamepad_buttons[index] = current;
        }
        for (std::size_t index = 0U; index < input_gamepad_axes.size(); ++index) {
            const auto raw = SDL_GetGamepadAxis(impl_->gamepad, input_gamepad_axes[index]);
            snapshot.gamepad_axes[index] = std::clamp(static_cast<float>(raw) * axis_scale, -1.0F, 1.0F);
        }
    }

    const bool* keyboard = SDL_GetKeyboardState(nullptr);
    for (std::size_t index = 0U; index < impl_->previous_keys.size(); ++index) {
        const bool current = keyboard[input_scancodes[index]];
        snapshot.keys[index] = current;
        snapshot.pressed_keys[index] = snapshot.pressed_keys[index] || (current && !impl_->previous_keys[index]);
        snapshot.released_keys[index] = snapshot.released_keys[index] || (!current && impl_->previous_keys[index]);
        impl_->previous_keys[index] = current;
    }
    snapshot.mouse_left = impl_->mouse_left;
    snapshot.mouse_x = impl_->mouse_x;
    snapshot.mouse_y = impl_->mouse_y;
    snapshot.drawable_extent = drawable_extent();
    snapshot.minimized = snapshot.minimized || snapshot.drawable_extent.width == 0U ||
                         snapshot.drawable_extent.height == 0U;
    return snapshot;
}

WindowExtent PlatformWindow::drawable_extent() const noexcept {
    int width = 0;
    int height = 0;
    if (impl_->window != nullptr) {
        SDL_GetWindowSizeInPixels(impl_->window, &width, &height);
    }
    return {checked_extent(width), checked_extent(height)};
}

void* PlatformWindow::native_handle() const noexcept { return impl_->window; }
bool PlatformWindow::is_open() const noexcept { return impl_->window != nullptr; }

void PlatformWindow::show() noexcept {
    if (impl_->window != nullptr) {
        SDL_ShowWindow(impl_->window);
    }
}

Result<void> PlatformWindow::set_extent(const std::uint32_t width, const std::uint32_t height) {
    if (impl_->window == nullptr ||
        !SDL_SetWindowSize(impl_->window, static_cast<int>(width), static_cast<int>(height))) {
        return std::unexpected(sdl_error("SDL window resize failed"));
    }
    return {};
}

} // namespace ai2d
