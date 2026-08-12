#include "sdl_surface.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <string>

namespace ai2d::vulkan_backend {
namespace {

Diagnostic surface_error(const char* message) {
    auto diagnostic = Diagnostic::make(
        DiagnosticCode::vk_swapchain_error, Severity::error, "renderer2d", message);
    diagnostic.context.push_back({"sdl_error", std::string{SDL_GetError()}});
    return diagnostic;
}

} // namespace

Result<std::vector<const char*>> sdl_instance_extensions() {
    Uint32 count = 0U;
    const char* const* extensions = SDL_Vulkan_GetInstanceExtensions(&count);
    if (extensions == nullptr || count == 0U) {
        return std::unexpected(surface_error("SDL did not provide Vulkan instance extensions"));
    }
    return std::vector<const char*>{extensions, extensions + count};
}

Result<VkSurfaceKHR> create_sdl_surface(void* native_window, const VkInstance instance) {
    if (native_window == nullptr) {
        return std::unexpected(surface_error("A native SDL window is required for presentation"));
    }
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (!SDL_Vulkan_CreateSurface(static_cast<SDL_Window*>(native_window), instance, nullptr, &surface)) {
        return std::unexpected(surface_error("SDL Vulkan surface creation failed"));
    }
    return surface;
}

} // namespace ai2d::vulkan_backend
