#pragma once

#include "ai2d/foundation/diagnostic.hpp"

#include <vulkan/vulkan.h>

#include <vector>

namespace ai2d::vulkan_backend {

[[nodiscard]] Result<std::vector<const char*>> sdl_instance_extensions();
[[nodiscard]] Result<VkSurfaceKHR> create_sdl_surface(void* native_window, VkInstance instance);

} // namespace ai2d::vulkan_backend
