#include "ai2d/platform/window.hpp"

#include <SDL3/SDL.h>

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
    SDL_SCANCODE_SPACE,
    SDL_SCANCODE_RETURN,
    SDL_SCANCODE_TAB,
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
    bool owns_video{false};
    std::array<bool, static_cast<std::uint8_t>(InputKey::count)> previous_keys{};
    bool mouse_left{false};
    float mouse_x{0.0F};
    float mouse_y{0.0F};

    ~Impl() {
        if (window != nullptr) {
            SDL_DestroyWindow(window);
        }
        if (owns_video) {
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
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
        default: break;
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
