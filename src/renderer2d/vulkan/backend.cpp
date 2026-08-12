#include "ai2d/renderer2d/renderer.hpp"

#include "ai2d/foundation/timer.hpp"
#include "ai2d_generated/embedded_spirv.hpp"
#include "sdl_surface.hpp"

#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ai2d {
namespace {

PresentMode2D requested_present_mode(const RenderFpsCap cap) noexcept {
    return cap == RenderFpsCap::fps_60 ? PresentMode2D::fifo : PresentMode2D::mailbox;
}

PresentMode2D present_mode_from_vk(const VkPresentModeKHR mode) noexcept {
    if (mode == VK_PRESENT_MODE_MAILBOX_KHR) return PresentMode2D::mailbox;
    if (mode == VK_PRESENT_MODE_IMMEDIATE_KHR) return PresentMode2D::immediate;
    return PresentMode2D::fifo;
}

constexpr std::uint32_t frames_in_flight = 2U;

Diagnostic vk_error(
    const DiagnosticCode code,
    const char* message,
    const VkResult result,
    const Severity severity = Severity::error) {
    auto diagnostic = Diagnostic::make(code, severity, "renderer2d", message);
    diagnostic.context.push_back({"vk_result", static_cast<std::int64_t>(result)});
    return diagnostic;
}

std::string_view device_type_name(const VkPhysicalDeviceType type) noexcept {
    switch (type) {
    case VK_PHYSICAL_DEVICE_TYPE_OTHER: return "other";
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated_gpu";
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return "discrete_gpu";
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return "virtual_gpu";
    case VK_PHYSICAL_DEVICE_TYPE_CPU: return "cpu";
    default: return "unknown";
    }
}

bool contains_name(const std::span<const VkExtensionProperties> properties, const char* name) noexcept {
    return std::ranges::any_of(properties, [name](const VkExtensionProperties& property) {
        return std::strcmp(property.extensionName, name) == 0;
    });
}

bool contains_layer(const std::span<const VkLayerProperties> properties, const char* name) noexcept {
    return std::ranges::any_of(properties, [name](const VkLayerProperties& property) {
        return std::strcmp(property.layerName, name) == 0;
    });
}

std::uint32_t first_set_bit(const VkCompositeAlphaFlagsKHR flags) noexcept {
    constexpr std::array<VkCompositeAlphaFlagBitsKHR, 4U> candidates{
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
    };
    for (const auto candidate : candidates) {
        if ((flags & candidate) != 0U) {
            return static_cast<std::uint32_t>(candidate);
        }
    }
    return static_cast<std::uint32_t>(VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR);
}

std::uint8_t color_byte(const float value) noexcept {
    const float clamped = std::clamp(value, 0.0F, 1.0F);
    return static_cast<std::uint8_t>(std::lround(clamped * 255.0F));
}

std::string environment_value(const char* const name) {
#if defined(_WIN32)
    char* buffer = nullptr;
    std::size_t length = 0U;
    if (_dupenv_s(&buffer, &length, name) != 0 || buffer == nullptr) {
        return {};
    }
    std::string value{buffer};
    std::free(buffer);
    return value;
#else
    const char* const value = std::getenv(name);
    return value != nullptr ? std::string{value} : std::string{};
#endif
}

} // namespace

class Renderer2D::Impl final {
  public:
#if defined(AI2D_ENABLE_TEST_HOOKS)
    enum class TestFault : std::uint8_t {
        none,
        acquire_out_of_date_once,
        reset_command_pool_once,
        begin_command_once,
        end_command_once,
        submit_once,
        initialize_offscreen_view_once,
        resize_offscreen_view_once,
    };
#endif
    struct AllocatedBuffer final {
        VkBuffer handle{VK_NULL_HANDLE};
        VmaAllocation allocation{VK_NULL_HANDLE};
        void* mapped{nullptr};
        VkDeviceSize size{0U};

        AllocatedBuffer() = default;
        AllocatedBuffer(const AllocatedBuffer&) = delete;
        AllocatedBuffer& operator=(const AllocatedBuffer&) = delete;
        AllocatedBuffer(AllocatedBuffer&& other) noexcept { *this = std::move(other); }
        AllocatedBuffer& operator=(AllocatedBuffer&& other) noexcept {
            if (this != &other) {
                handle = std::exchange(other.handle, VK_NULL_HANDLE);
                allocation = std::exchange(other.allocation, VK_NULL_HANDLE);
                mapped = std::exchange(other.mapped, nullptr);
                size = std::exchange(other.size, 0U);
            }
            return *this;
        }
        void reset(const VmaAllocator owner) noexcept {
            if (handle != VK_NULL_HANDLE) {
                vmaDestroyBuffer(owner, handle, allocation);
            }
            handle = VK_NULL_HANDLE;
            allocation = VK_NULL_HANDLE;
            mapped = nullptr;
            size = 0U;
        }
    };

    struct AllocatedImage final {
        VkImage handle{VK_NULL_HANDLE};
        VmaAllocation allocation{VK_NULL_HANDLE};
        VkImageView view{VK_NULL_HANDLE};

        AllocatedImage() = default;
        AllocatedImage(const AllocatedImage&) = delete;
        AllocatedImage& operator=(const AllocatedImage&) = delete;
        AllocatedImage(AllocatedImage&& other) noexcept { *this = std::move(other); }
        AllocatedImage& operator=(AllocatedImage&& other) noexcept {
            if (this != &other) {
                handle = std::exchange(other.handle, VK_NULL_HANDLE);
                allocation = std::exchange(other.allocation, VK_NULL_HANDLE);
                view = std::exchange(other.view, VK_NULL_HANDLE);
            }
            return *this;
        }
        void reset(const VmaAllocator owner, const VkDevice logical_device) noexcept {
            if (view != VK_NULL_HANDLE) {
                vkDestroyImageView(logical_device, view, nullptr);
            }
            if (handle != VK_NULL_HANDLE) {
                vmaDestroyImage(owner, handle, allocation);
            }
            handle = VK_NULL_HANDLE;
            allocation = VK_NULL_HANDLE;
            view = VK_NULL_HANDLE;
        }
    };

    struct TextureSlot final {
        AllocatedImage image{};
        VkDescriptorSet descriptor_set{VK_NULL_HANDLE};
        std::uint32_t generation{1U};
        std::uint32_t width{0U};
        std::uint32_t height{0U};
        bool occupied{false};
    };

    struct FrameSlot final {
        VkCommandPool command_pool{VK_NULL_HANDLE};
        VkCommandBuffer command_buffer{VK_NULL_HANDLE};
        VkFence fence{VK_NULL_HANDLE};
        VkSemaphore image_available{VK_NULL_HANDLE};
        VkQueryPool timestamp_queries{VK_NULL_HANDLE};
        AllocatedBuffer instance_upload{};
        bool timestamp_pending{false};
        double completed_gpu_ms{0.0};
        bool completed_gpu_valid{false};
    };

    struct DeviceCandidate final {
        VkPhysicalDevice device{VK_NULL_HANDLE};
        VkPhysicalDeviceProperties2 properties{};
        VkPhysicalDeviceDriverProperties driver_properties{};
        VkPhysicalDeviceVulkan11Features features11{};
        VkPhysicalDeviceVulkan13Features features13{};
        std::uint32_t queue_family{0U};
        std::uint32_t timestamp_valid_bits{0U};
        bool present_supported{false};
        std::int64_t score{0};
    };

    RendererOptions options{};
    RendererCapabilities capabilities{};
    VkInstance instance{VK_NULL_HANDLE};
    VkDebugUtilsMessengerEXT debug_messenger{VK_NULL_HANDLE};
    VkSurfaceKHR surface{VK_NULL_HANDLE};
    VkPhysicalDevice physical_device{VK_NULL_HANDLE};
    VkDevice device{VK_NULL_HANDLE};
    VkQueue graphics_queue{VK_NULL_HANDLE};
    std::uint32_t queue_family{0U};
    VmaAllocator allocator{VK_NULL_HANDLE};
    VkCommandPool setup_command_pool{VK_NULL_HANDLE};
    VkDescriptorSetLayout texture_set_layout{VK_NULL_HANDLE};
    VkDescriptorPool texture_descriptor_pool{VK_NULL_HANDLE};
    VkPipelineLayout pipeline_layout{VK_NULL_HANDLE};
    VkSampler sampler{VK_NULL_HANDLE};
    VkPipeline offscreen_pipeline{VK_NULL_HANDLE};
    VkPipeline present_pipeline{VK_NULL_HANDLE};
    VkSwapchainKHR swapchain{VK_NULL_HANDLE};
    VkFormat swapchain_format{VK_FORMAT_UNDEFINED};
    VkExtent2D swapchain_extent{};
    std::vector<VkImage> swapchain_images{};
    std::vector<VkImageView> swapchain_views{};
    std::vector<VkSemaphore> swapchain_present_semaphores{};
    VkImage offscreen_image{VK_NULL_HANDLE};
    VmaAllocation offscreen_allocation{VK_NULL_HANDLE};
    VkImageView offscreen_view{VK_NULL_HANDLE};
    VkExtent2D offscreen_extent{};
    VkImageLayout offscreen_layout{VK_IMAGE_LAYOUT_UNDEFINED};
    std::array<FrameSlot, frames_in_flight> frames{};
    std::vector<TextureSlot> textures{};
    RenderQueue2D render_queue{};
    std::uint64_t frame_index{0U};
    std::uint64_t vma_allocation_count{0U};
    std::uint64_t vma_allocation_bytes{0U};
    std::uint64_t texture_bytes{0U};
    std::uint64_t instance_buffer_capacity_bytes{0U};
    bool ready{false};
#if defined(AI2D_ENABLE_TEST_HOOKS)
    TestFault test_fault{TestFault::none};
    bool test_fault_triggered{false};
#endif
    std::vector<Diagnostic> validation_messages{};
    mutable std::mutex validation_mutex{};

    ~Impl() { shutdown(); }

#if defined(AI2D_ENABLE_TEST_HOOKS)
    bool inject_once(const TestFault expected) noexcept {
        if (test_fault != expected || test_fault_triggered) {
            return false;
        }
        test_fault_triggered = true;
        return true;
    }
#endif

    static VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(
        const VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        const VkDebugUtilsMessageTypeFlagsEXT type,
        const VkDebugUtilsMessengerCallbackDataEXT* data,
        void* user_data) noexcept {
        auto* self = static_cast<Impl*>(user_data);
        if (self == nullptr || data == nullptr) {
            return VK_FALSE;
        }
        try {
            Severity diagnostic_severity = Severity::info;
            if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0U) {
                diagnostic_severity = Severity::error;
            } else if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0U) {
                diagnostic_severity = Severity::warning;
            }
            auto diagnostic = Diagnostic::make(
                DiagnosticCode::vk_validation,
                diagnostic_severity,
                "renderer2d",
                data->pMessage != nullptr ? data->pMessage : "Vulkan validation message");
            diagnostic.context.push_back(
                {"message_id", std::string{data->pMessageIdName != nullptr ? data->pMessageIdName : "UNKNOWN"}});
            diagnostic.context.push_back({"message_id_number", static_cast<std::int64_t>(data->messageIdNumber)});
            diagnostic.context.push_back({"message_type", static_cast<std::uint64_t>(type)});
            if (data->objectCount > 0U && data->pObjects != nullptr) {
                diagnostic.context.push_back(
                    {"object_type", static_cast<std::uint64_t>(data->pObjects[0].objectType)});
                diagnostic.context.push_back({"object_handle", data->pObjects[0].objectHandle});
            }
            const std::scoped_lock lock{self->validation_mutex};
            self->validation_messages.push_back(std::move(diagnostic));
        } catch (...) {
            // The Vulkan callback must never allow a C++ exception to cross the C ABI.
        }
        return VK_FALSE;
    }

    Result<void> initialize(const RendererOptions& requested_options) {
        if (ready) {
            auto diagnostic = Diagnostic::make(
                DiagnosticCode::internal_error, Severity::error, "renderer2d", "Renderer is already initialized");
            return std::unexpected(std::move(diagnostic));
        }
        capabilities = {};
        validation_messages.clear();
        frame_index = 0U;
        texture_bytes = 0U;
        instance_buffer_capacity_bytes = 0U;
        vma_allocation_count = 0U;
        vma_allocation_bytes = 0U;
#if defined(AI2D_ENABLE_TEST_HOOKS)
        const std::string requested_fault = environment_value("AI2D_VK_TEST_FAULT");
        if (!requested_fault.empty()) {
            const std::string_view fault{requested_fault};
            if (fault == "acquire_out_of_date_once") {
                test_fault = TestFault::acquire_out_of_date_once;
            } else if (fault == "reset_command_pool_once") {
                test_fault = TestFault::reset_command_pool_once;
            } else if (fault == "begin_command_once") {
                test_fault = TestFault::begin_command_once;
            } else if (fault == "end_command_once") {
                test_fault = TestFault::end_command_once;
            } else if (fault == "submit_once") {
                test_fault = TestFault::submit_once;
            } else if (fault == "initialize_offscreen_view_once") {
                test_fault = TestFault::initialize_offscreen_view_once;
            } else if (fault == "resize_offscreen_view_once") {
                test_fault = TestFault::resize_offscreen_view_once;
            }
        }
#endif
        options = requested_options;
        if (options.max_sprites == 0U || options.max_textures == 0U || options.width == 0U ||
            options.height == 0U) {
            auto diagnostic = Diagnostic::make(
                DiagnosticCode::input_invalid,
                Severity::error,
                "renderer2d",
                "Renderer dimensions and resource capacities must be nonzero");
            return std::unexpected(std::move(diagnostic));
        }
#if defined(AI2D_DEFAULT_VULKAN_VALIDATION)
        options.enable_validation = true;
#endif
#if defined(AI2D_DEFAULT_SYNC_VALIDATION)
        options.enable_validation = true;
        options.enable_synchronization_validation = true;
#endif
        capabilities.validation_requested = options.enable_validation;
        capabilities.requested_fps_cap = options.requested_fps_cap;
        capabilities.requested_present_mode = requested_present_mode(options.requested_fps_cap);
        capabilities.effective_present_mode = PresentMode2D::fifo;

        if (auto result = create_instance(); !result) {
            return result;
        }
        if (options.require_present) {
            auto created_surface = vulkan_backend::create_sdl_surface(options.native_window, instance);
            if (!created_surface) {
                return std::unexpected(std::move(created_surface.error()));
            }
            surface = created_surface.value();
        }
        if (auto result = select_device(); !result) {
            return result;
        }
        if (auto result = create_device(); !result) {
            return result;
        }
        if (auto result = create_allocator(); !result) {
            return result;
        }
        if (auto result = create_frame_resources(); !result) {
            return result;
        }
        if (auto result = create_texture_infrastructure(); !result) {
            return result;
        }
        if (auto result = create_offscreen(options.width, options.height); !result) {
            return result;
        }
        if (options.require_present) {
            if (auto result = create_swapchain(options.width, options.height); !result) {
                return result;
            }
        }
        if (auto result = create_graphics_pipelines(); !result) {
            return result;
        }
        render_queue.reserve(options.max_sprites);
        instance_buffer_capacity_bytes = static_cast<std::uint64_t>(options.max_sprites) *
                                         sizeof(PreparedSprite2D) * frames_in_flight;
        refresh_memory_metrics();
        ready = true;
        return {};
    }

    Result<void> create_instance() {
        std::uint32_t instance_version = VK_API_VERSION_1_0;
        if (vkEnumerateInstanceVersion(&instance_version) != VK_SUCCESS || instance_version < VK_API_VERSION_1_3) {
            auto diagnostic = Diagnostic::make(
                DiagnosticCode::vk_device_unsupported,
                Severity::error,
                "renderer2d",
                "The Vulkan loader does not expose Vulkan 1.3");
            diagnostic.context.push_back({"instance_api_version", static_cast<std::uint64_t>(instance_version)});
            return std::unexpected(std::move(diagnostic));
        }

        std::vector<const char*> extensions{};
        if (options.require_present) {
            auto sdl_extensions = vulkan_backend::sdl_instance_extensions();
            if (!sdl_extensions) {
                return std::unexpected(std::move(sdl_extensions.error()));
            }
            extensions = std::move(sdl_extensions.value());
        }
        if (options.enable_validation) {
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        }

        std::uint32_t extension_count = 0U;
        VkResult result = vkEnumerateInstanceExtensionProperties(nullptr, &extension_count, nullptr);
        if (result != VK_SUCCESS) {
            return std::unexpected(vk_error(
                DiagnosticCode::vk_device_unsupported, "Could not enumerate Vulkan instance extensions", result));
        }
        std::vector<VkExtensionProperties> available_extensions(extension_count);
        result = vkEnumerateInstanceExtensionProperties(nullptr, &extension_count, available_extensions.data());
        if (result != VK_SUCCESS) {
            return std::unexpected(vk_error(
                DiagnosticCode::vk_device_unsupported, "Could not read Vulkan instance extensions", result));
        }
        for (const char* extension : extensions) {
            if (!contains_name(available_extensions, extension)) {
                auto diagnostic = Diagnostic::make(
                    DiagnosticCode::vk_device_unsupported,
                    Severity::error,
                    "renderer2d",
                    "A required Vulkan instance extension is unavailable");
                diagnostic.context.push_back({"extension", std::string{extension}});
                return std::unexpected(std::move(diagnostic));
            }
        }

        std::vector<const char*> layers{};
        if (options.enable_validation) {
            std::uint32_t layer_count = 0U;
            result = vkEnumerateInstanceLayerProperties(&layer_count, nullptr);
            if (result != VK_SUCCESS) {
                return std::unexpected(vk_error(
                    DiagnosticCode::tool_missing, "Could not enumerate Vulkan instance layers", result));
            }
            std::vector<VkLayerProperties> available_layers(layer_count);
            result = vkEnumerateInstanceLayerProperties(&layer_count, available_layers.data());
            if (result != VK_SUCCESS) {
                return std::unexpected(vk_error(
                    DiagnosticCode::tool_missing, "Could not read Vulkan instance layers", result));
            }
            constexpr const char* validation_layer = "VK_LAYER_KHRONOS_validation";
            if (!contains_layer(available_layers, validation_layer)) {
                auto diagnostic = Diagnostic::make(
                    DiagnosticCode::tool_missing,
                    Severity::error,
                    "renderer2d",
                    "VK_LAYER_KHRONOS_validation was requested but is unavailable");
                diagnostic.context.push_back({"layer", std::string{validation_layer}});
                diagnostic.suggestions.push_back("Run engine bootstrap or configure VK_ADD_LAYER_PATH.");
                return std::unexpected(std::move(diagnostic));
            }
            layers.push_back(validation_layer);
        }

        VkApplicationInfo application{};
        application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        application.pApplicationName = "game_runtime_dev";
        application.applicationVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
        application.pEngineName = "ai2d";
        application.engineVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
        application.apiVersion = VK_API_VERSION_1_3;

        VkDebugUtilsMessengerCreateInfoEXT debug_info{};
        debug_info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        debug_info.messageSeverity =
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        debug_info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                 VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                 VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        debug_info.pfnUserCallback = debug_callback;
        debug_info.pUserData = this;

        VkValidationFeatureEnableEXT synchronization_feature =
            VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
        VkValidationFeaturesEXT validation_features{};
        validation_features.sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT;
        validation_features.enabledValidationFeatureCount =
            options.enable_synchronization_validation ? 1U : 0U;
        validation_features.pEnabledValidationFeatures =
            options.enable_synchronization_validation ? &synchronization_feature : nullptr;
        debug_info.pNext = options.enable_synchronization_validation ? &validation_features : nullptr;

        VkInstanceCreateInfo create_info{};
        create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        create_info.pNext = options.enable_validation ? &debug_info : nullptr;
        create_info.pApplicationInfo = &application;
        create_info.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
        create_info.ppEnabledExtensionNames = extensions.data();
        create_info.enabledLayerCount = static_cast<std::uint32_t>(layers.size());
        create_info.ppEnabledLayerNames = layers.data();
        result = vkCreateInstance(&create_info, nullptr, &instance);
        if (result != VK_SUCCESS) {
            return std::unexpected(
                vk_error(DiagnosticCode::vk_device_unsupported, "Vulkan instance creation failed", result));
        }

        if (options.enable_validation) {
            const auto create_debug = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
            if (create_debug == nullptr) {
                return std::unexpected(vk_error(
                    DiagnosticCode::tool_missing,
                    "VK_EXT_debug_utils was enabled but its entry point is unavailable",
                    VK_ERROR_EXTENSION_NOT_PRESENT));
            }
            result = create_debug(instance, &debug_info, nullptr, &debug_messenger);
            if (result != VK_SUCCESS) {
                return std::unexpected(vk_error(
                    DiagnosticCode::tool_missing, "Vulkan debug messenger creation failed", result));
            }
            capabilities.validation_enabled = true;
            capabilities.synchronization_validation_enabled = options.enable_synchronization_validation;
        }
        return {};
    }

    std::optional<DeviceCandidate> inspect_device(const VkPhysicalDevice candidate_device) const {
        DeviceCandidate candidate{};
        candidate.device = candidate_device;
        candidate.properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        candidate.driver_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
        candidate.properties.pNext = &candidate.driver_properties;
        vkGetPhysicalDeviceProperties2(candidate_device, &candidate.properties);
        if (candidate.properties.properties.apiVersion < VK_API_VERSION_1_3) {
            return std::nullopt;
        }

        candidate.features11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
        candidate.features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        candidate.features11.pNext = &candidate.features13;
        VkPhysicalDeviceFeatures2 features{};
        features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        features.pNext = &candidate.features11;
        vkGetPhysicalDeviceFeatures2(candidate_device, &features);
        if (candidate.features11.shaderDrawParameters != VK_TRUE ||
            candidate.features13.dynamicRendering != VK_TRUE || candidate.features13.synchronization2 != VK_TRUE) {
            return std::nullopt;
        }

        std::uint32_t extension_count = 0U;
        if (vkEnumerateDeviceExtensionProperties(candidate_device, nullptr, &extension_count, nullptr) != VK_SUCCESS) {
            return std::nullopt;
        }
        std::vector<VkExtensionProperties> extensions(extension_count);
        if (vkEnumerateDeviceExtensionProperties(
                candidate_device, nullptr, &extension_count, extensions.data()) != VK_SUCCESS) {
            return std::nullopt;
        }
        if (options.require_present && !contains_name(extensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
            return std::nullopt;
        }

        std::uint32_t queue_count = 0U;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate_device, &queue_count, nullptr);
        std::vector<VkQueueFamilyProperties> queues(queue_count);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate_device, &queue_count, queues.data());
        bool found_queue = false;
        for (std::uint32_t index = 0U; index < queue_count; ++index) {
            if ((queues[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0U) {
                continue;
            }
            VkBool32 present = VK_TRUE;
            if (options.require_present &&
                vkGetPhysicalDeviceSurfaceSupportKHR(candidate_device, index, surface, &present) != VK_SUCCESS) {
                continue;
            }
            if (options.require_present && present != VK_TRUE) {
                continue;
            }
            candidate.queue_family = index;
            candidate.timestamp_valid_bits = queues[index].timestampValidBits;
            candidate.present_supported = present == VK_TRUE;
            found_queue = true;
            break;
        }
        if (!found_queue) {
            return std::nullopt;
        }

        candidate.score = candidate.properties.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU
                              ? 1000
                              : candidate.properties.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU
                                    ? 500
                                    : 100;
        candidate.score += candidate.timestamp_valid_bits > 0U ? 50 : 0;
        return candidate;
    }

    Result<void> select_device() {
        std::uint32_t device_count = 0U;
        VkResult result = vkEnumeratePhysicalDevices(instance, &device_count, nullptr);
        if (result != VK_SUCCESS || device_count == 0U) {
            return std::unexpected(vk_error(
                DiagnosticCode::vk_device_unsupported, "No Vulkan physical devices are available", result));
        }
        std::vector<VkPhysicalDevice> devices(device_count);
        result = vkEnumeratePhysicalDevices(instance, &device_count, devices.data());
        if (result != VK_SUCCESS) {
            return std::unexpected(vk_error(
                DiagnosticCode::vk_device_unsupported, "Could not enumerate Vulkan physical devices", result));
        }

        std::optional<DeviceCandidate> selected{};
        if (options.device_index >= 0) {
            const auto index = static_cast<std::uint32_t>(options.device_index);
            if (index < device_count) {
                selected = inspect_device(devices[index]);
            }
        } else {
            for (const auto candidate_device : devices) {
                auto candidate = inspect_device(candidate_device);
                if (candidate && (!selected || candidate->score > selected->score)) {
                    selected = std::move(candidate);
                }
            }
        }
        if (!selected) {
            auto diagnostic = Diagnostic::make(
                DiagnosticCode::vk_device_unsupported,
                Severity::error,
                "renderer2d",
                "No device satisfies Vulkan 1.3, dynamic rendering, synchronization2, and queue requirements");
            diagnostic.context.push_back({"physical_device_count", static_cast<std::uint64_t>(device_count)});
            diagnostic.context.push_back({"requested_device_index", static_cast<std::int64_t>(options.device_index)});
            return std::unexpected(std::move(diagnostic));
        }

        physical_device = selected->device;
        queue_family = selected->queue_family;
        const auto& properties = selected->properties.properties;
        capabilities.device_name = properties.deviceName;
        capabilities.device_type = device_type_name(properties.deviceType);
        capabilities.driver_name = selected->driver_properties.driverName;
        capabilities.driver_info = selected->driver_properties.driverInfo;
        capabilities.vendor_id = properties.vendorID;
        capabilities.device_id = properties.deviceID;
        capabilities.api_version = properties.apiVersion;
        capabilities.driver_version = properties.driverVersion;
        capabilities.graphics_queue_family = queue_family;
        capabilities.timestamp_period_nanoseconds = properties.limits.timestampPeriod;
        capabilities.timestamps_supported = selected->timestamp_valid_bits > 0U;
        capabilities.present_supported = selected->present_supported;
        capabilities.dynamic_rendering = true;
        capabilities.synchronization2 = true;
        capabilities.shader_draw_parameters = true;
        return {};
    }

    Result<void> create_device() {
        constexpr float priority = 1.0F;
        VkDeviceQueueCreateInfo queue_info{};
        queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue_info.queueFamilyIndex = queue_family;
        queue_info.queueCount = 1U;
        queue_info.pQueuePriorities = &priority;

        VkPhysicalDeviceVulkan13Features features13{};
        features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        features13.dynamicRendering = VK_TRUE;
        features13.synchronization2 = VK_TRUE;
        VkPhysicalDeviceVulkan11Features features11{};
        features11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
        features11.pNext = &features13;
        features11.shaderDrawParameters = VK_TRUE;

        constexpr std::array<const char*, 1U> swapchain_extension{VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        VkDeviceCreateInfo create_info{};
        create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        create_info.pNext = &features11;
        create_info.queueCreateInfoCount = 1U;
        create_info.pQueueCreateInfos = &queue_info;
        create_info.enabledExtensionCount = options.require_present ? 1U : 0U;
        create_info.ppEnabledExtensionNames = options.require_present ? swapchain_extension.data() : nullptr;
        const VkResult result = vkCreateDevice(physical_device, &create_info, nullptr, &device);
        if (result != VK_SUCCESS) {
            return std::unexpected(
                vk_error(DiagnosticCode::vk_device_unsupported, "Vulkan logical device creation failed", result));
        }
        vkGetDeviceQueue(device, queue_family, 0U, &graphics_queue);
        return {};
    }

    Result<void> create_allocator() {
        VmaAllocatorCreateInfo create_info{};
        create_info.flags = VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT;
        create_info.physicalDevice = physical_device;
        create_info.device = device;
        create_info.instance = instance;
        create_info.vulkanApiVersion = VK_API_VERSION_1_3;
        const VkResult result = vmaCreateAllocator(&create_info, &allocator);
        if (result != VK_SUCCESS) {
            return std::unexpected(
                vk_error(DiagnosticCode::vk_device_unsupported, "VMA allocator creation failed", result));
        }
        return {};
    }

    Result<void> create_frame_resources() {
        const VkDeviceSize upload_size =
            static_cast<VkDeviceSize>(options.max_sprites) * sizeof(PreparedSprite2D);
        for (auto& frame : frames) {
            VkCommandPoolCreateInfo pool_info{};
            pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            pool_info.queueFamilyIndex = queue_family;
            VkResult result = vkCreateCommandPool(device, &pool_info, nullptr, &frame.command_pool);
            if (result != VK_SUCCESS) {
                return std::unexpected(vk_error(
                    DiagnosticCode::vk_device_unsupported, "Frame command pool creation failed", result));
            }

            VkCommandBufferAllocateInfo command_info{};
            command_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            command_info.commandPool = frame.command_pool;
            command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            command_info.commandBufferCount = 1U;
            result = vkAllocateCommandBuffers(device, &command_info, &frame.command_buffer);
            if (result != VK_SUCCESS) {
                return std::unexpected(vk_error(
                    DiagnosticCode::vk_device_unsupported, "Frame command buffer allocation failed", result));
            }

            VkFenceCreateInfo fence_info{};
            fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            result = vkCreateFence(device, &fence_info, nullptr, &frame.fence);
            if (result != VK_SUCCESS) {
                return std::unexpected(vk_error(
                    DiagnosticCode::vk_device_unsupported, "Frame fence creation failed", result));
            }
            VkSemaphoreCreateInfo semaphore_info{};
            semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            result = vkCreateSemaphore(device, &semaphore_info, nullptr, &frame.image_available);
            if (result != VK_SUCCESS) {
                return std::unexpected(vk_error(
                    DiagnosticCode::vk_device_unsupported, "Frame semaphore creation failed", result));
            }

            if (capabilities.timestamps_supported) {
                VkQueryPoolCreateInfo query_info{};
                query_info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
                query_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
                query_info.queryCount = 2U;
                result = vkCreateQueryPool(device, &query_info, nullptr, &frame.timestamp_queries);
                if (result != VK_SUCCESS) {
                    capabilities.timestamps_supported = false;
                    frame.timestamp_queries = VK_NULL_HANDLE;
                }
            }

            VkBufferCreateInfo buffer_info{};
            buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            buffer_info.size = upload_size;
            buffer_info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
            buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            VmaAllocationCreateInfo allocation_info{};
            allocation_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                    VMA_ALLOCATION_CREATE_MAPPED_BIT;
            allocation_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
            VmaAllocationInfo result_info{};
            result = vmaCreateBuffer(
                allocator,
                &buffer_info,
                &allocation_info,
                &frame.instance_upload.handle,
                &frame.instance_upload.allocation,
                &result_info);
            if (result != VK_SUCCESS || result_info.pMappedData == nullptr) {
                return std::unexpected(vk_error(
                    DiagnosticCode::vk_device_unsupported,
                    "Persistently mapped per-frame instance buffer allocation failed",
                    result));
            }
            frame.instance_upload.mapped = result_info.pMappedData;
            frame.instance_upload.size = upload_size;
        }

        VkCommandPoolCreateInfo setup_pool_info{};
        setup_pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        setup_pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        setup_pool_info.queueFamilyIndex = queue_family;
        const VkResult result = vkCreateCommandPool(device, &setup_pool_info, nullptr, &setup_command_pool);
        if (result != VK_SUCCESS) {
            return std::unexpected(vk_error(
                DiagnosticCode::vk_device_unsupported, "Setup command pool creation failed", result));
        }
        return {};
    }

    Result<void> create_texture_infrastructure() {
        std::array<VkDescriptorSetLayoutBinding, 2U> bindings{};
        bindings[0].binding = 0U;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        bindings[0].descriptorCount = 1U;
        bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings[1].binding = 1U;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        bindings[1].descriptorCount = 1U;
        bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo layout_info{};
        layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layout_info.bindingCount = static_cast<std::uint32_t>(bindings.size());
        layout_info.pBindings = bindings.data();
        VkResult result = vkCreateDescriptorSetLayout(device, &layout_info, nullptr, &texture_set_layout);
        if (result != VK_SUCCESS) {
            return std::unexpected(vk_error(
                DiagnosticCode::vk_device_unsupported, "Texture descriptor set layout creation failed", result));
        }

        std::array<VkDescriptorPoolSize, 2U> pool_sizes{};
        pool_sizes[0] = {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, options.max_textures};
        pool_sizes[1] = {VK_DESCRIPTOR_TYPE_SAMPLER, options.max_textures};
        VkDescriptorPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        pool_info.maxSets = options.max_textures;
        pool_info.poolSizeCount = static_cast<std::uint32_t>(pool_sizes.size());
        pool_info.pPoolSizes = pool_sizes.data();
        result = vkCreateDescriptorPool(device, &pool_info, nullptr, &texture_descriptor_pool);
        if (result != VK_SUCCESS) {
            return std::unexpected(vk_error(
                DiagnosticCode::vk_device_unsupported, "Texture descriptor pool creation failed", result));
        }

        VkSamplerCreateInfo sampler_info{};
        sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sampler_info.magFilter = VK_FILTER_NEAREST;
        sampler_info.minFilter = VK_FILTER_NEAREST;
        sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_info.maxLod = 0.0F;
        result = vkCreateSampler(device, &sampler_info, nullptr, &sampler);
        if (result != VK_SUCCESS) {
            return std::unexpected(
                vk_error(DiagnosticCode::vk_device_unsupported, "Shared sprite sampler creation failed", result));
        }

        VkPipelineLayoutCreateInfo pipeline_layout_info{};
        pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipeline_layout_info.setLayoutCount = 1U;
        pipeline_layout_info.pSetLayouts = &texture_set_layout;
        result = vkCreatePipelineLayout(device, &pipeline_layout_info, nullptr, &pipeline_layout);
        if (result != VK_SUCCESS) {
            return std::unexpected(
                vk_error(DiagnosticCode::vk_device_unsupported, "Sprite pipeline layout creation failed", result));
        }

        textures.resize(options.max_textures);
        return {};
    }

    Result<VkPipeline> create_graphics_pipeline(const VkFormat color_format) {
        constexpr auto& vertex_words = embedded_shaders::sprite_vertex_spirv;
        constexpr auto& fragment_words = embedded_shaders::sprite_fragment_spirv;

        VkShaderModuleCreateInfo module_info{};
        module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        module_info.codeSize = vertex_words.size() * sizeof(std::uint32_t);
        module_info.pCode = vertex_words.data();
        VkShaderModule vertex_module = VK_NULL_HANDLE;
        VkResult result = vkCreateShaderModule(device, &module_info, nullptr, &vertex_module);
        if (result != VK_SUCCESS) {
            return std::unexpected(
                vk_error(DiagnosticCode::vk_device_unsupported, "Vertex shader module creation failed", result));
        }
        module_info.codeSize = fragment_words.size() * sizeof(std::uint32_t);
        module_info.pCode = fragment_words.data();
        VkShaderModule fragment_module = VK_NULL_HANDLE;
        result = vkCreateShaderModule(device, &module_info, nullptr, &fragment_module);
        if (result != VK_SUCCESS) {
            vkDestroyShaderModule(device, vertex_module, nullptr);
            return std::unexpected(
                vk_error(DiagnosticCode::vk_device_unsupported, "Fragment shader module creation failed", result));
        }

        std::array<VkPipelineShaderStageCreateInfo, 2U> stages{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vertex_module;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fragment_module;
        stages[1].pName = "main";

        VkVertexInputBindingDescription binding{};
        binding.binding = 0U;
        binding.stride = sizeof(PreparedSprite2D);
        binding.inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;
        std::array<VkVertexInputAttributeDescription, 6U> attributes{};
        attributes[0] = {0U, 0U, VK_FORMAT_R32G32_SFLOAT, offsetof(PreparedSprite2D, clip_position)};
        attributes[1] = {1U, 0U, VK_FORMAT_R32G32_SFLOAT, offsetof(PreparedSprite2D, clip_size)};
        attributes[2] = {2U, 0U, VK_FORMAT_R32_SFLOAT, offsetof(PreparedSprite2D, rotation)};
        attributes[3] = {3U, 0U, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(PreparedSprite2D, uv_rect)};
        attributes[4] = {4U, 0U, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(PreparedSprite2D, tint)};
        attributes[5] = {5U, 0U, VK_FORMAT_R32G32_SFLOAT, offsetof(PreparedSprite2D, pivot)};
        VkPipelineVertexInputStateCreateInfo vertex_input{};
        vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertex_input.vertexBindingDescriptionCount = 1U;
        vertex_input.pVertexBindingDescriptions = &binding;
        vertex_input.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(attributes.size());
        vertex_input.pVertexAttributeDescriptions = attributes.data();

        VkPipelineInputAssemblyStateCreateInfo assembly{};
        assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewport{};
        viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewport.viewportCount = 1U;
        viewport.scissorCount = 1U;
        VkPipelineRasterizationStateCreateInfo rasterization{};
        rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterization.polygonMode = VK_POLYGON_MODE_FILL;
        rasterization.cullMode = VK_CULL_MODE_NONE;
        rasterization.frontFace = VK_FRONT_FACE_CLOCKWISE;
        rasterization.lineWidth = 1.0F;
        VkPipelineMultisampleStateCreateInfo multisample{};
        multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState blend_attachment{};
        blend_attachment.blendEnable = VK_TRUE;
        blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend_attachment.colorBlendOp = VK_BLEND_OP_ADD;
        blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend_attachment.alphaBlendOp = VK_BLEND_OP_ADD;
        blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo blend{};
        blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        blend.attachmentCount = 1U;
        blend.pAttachments = &blend_attachment;
        constexpr std::array<VkDynamicState, 2U> dynamic_states{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{};
        dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic.dynamicStateCount = static_cast<std::uint32_t>(dynamic_states.size());
        dynamic.pDynamicStates = dynamic_states.data();
        VkPipelineRenderingCreateInfo rendering{};
        rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        rendering.colorAttachmentCount = 1U;
        rendering.pColorAttachmentFormats = &color_format;

        VkGraphicsPipelineCreateInfo pipeline_info{};
        pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipeline_info.pNext = &rendering;
        pipeline_info.stageCount = static_cast<std::uint32_t>(stages.size());
        pipeline_info.pStages = stages.data();
        pipeline_info.pVertexInputState = &vertex_input;
        pipeline_info.pInputAssemblyState = &assembly;
        pipeline_info.pViewportState = &viewport;
        pipeline_info.pRasterizationState = &rasterization;
        pipeline_info.pMultisampleState = &multisample;
        pipeline_info.pColorBlendState = &blend;
        pipeline_info.pDynamicState = &dynamic;
        pipeline_info.layout = pipeline_layout;
        VkPipeline pipeline = VK_NULL_HANDLE;
        result = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1U, &pipeline_info, nullptr, &pipeline);
        vkDestroyShaderModule(device, fragment_module, nullptr);
        vkDestroyShaderModule(device, vertex_module, nullptr);
        if (result != VK_SUCCESS) {
            return std::unexpected(
                vk_error(DiagnosticCode::vk_device_unsupported, "Instanced sprite pipeline creation failed", result));
        }
        return pipeline;
    }

    Result<void> create_graphics_pipelines() {
        auto offscreen = create_graphics_pipeline(VK_FORMAT_R8G8B8A8_UNORM);
        if (!offscreen) {
            return std::unexpected(std::move(offscreen.error()));
        }
        VkPipeline new_present_pipeline = VK_NULL_HANDLE;
        if (options.require_present && swapchain_format != VK_FORMAT_UNDEFINED) {
            auto presented = create_graphics_pipeline(swapchain_format);
            if (!presented) {
                vkDestroyPipeline(device, offscreen.value(), nullptr);
                return std::unexpected(std::move(presented.error()));
            }
            new_present_pipeline = presented.value();
        }
        offscreen_pipeline = offscreen.value();
        present_pipeline = new_present_pipeline;
        return {};
    }

    Result<VkCommandBuffer> begin_immediate_commands() {
        VkCommandBufferAllocateInfo allocate_info{};
        allocate_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocate_info.commandPool = setup_command_pool;
        allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate_info.commandBufferCount = 1U;
        VkCommandBuffer command_buffer = VK_NULL_HANDLE;
        VkResult result = vkAllocateCommandBuffers(device, &allocate_info, &command_buffer);
        if (result != VK_SUCCESS) {
            return std::unexpected(vk_error(
                DiagnosticCode::vk_device_unsupported, "Setup command buffer allocation failed", result));
        }
        VkCommandBufferBeginInfo begin_info{};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        result = vkBeginCommandBuffer(command_buffer, &begin_info);
        if (result != VK_SUCCESS) {
            vkFreeCommandBuffers(device, setup_command_pool, 1U, &command_buffer);
            return std::unexpected(vk_error(
                DiagnosticCode::vk_device_unsupported, "Setup command buffer begin failed", result));
        }
        return command_buffer;
    }

    Result<void> finish_immediate_commands(const VkCommandBuffer command_buffer) {
        VkResult result = vkEndCommandBuffer(command_buffer);
        if (result != VK_SUCCESS) {
            vkFreeCommandBuffers(device, setup_command_pool, 1U, &command_buffer);
            return std::unexpected(vk_error(
                DiagnosticCode::vk_device_unsupported, "Setup command buffer end failed", result));
        }
        VkFenceCreateInfo fence_info{};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VkFence fence = VK_NULL_HANDLE;
        result = vkCreateFence(device, &fence_info, nullptr, &fence);
        if (result != VK_SUCCESS) {
            vkFreeCommandBuffers(device, setup_command_pool, 1U, &command_buffer);
            return std::unexpected(vk_error(
                DiagnosticCode::vk_device_unsupported, "Setup submission fence creation failed", result));
        }
        VkCommandBufferSubmitInfo command_info{};
        command_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        command_info.commandBuffer = command_buffer;
        VkSubmitInfo2 submit_info{};
        submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        submit_info.commandBufferInfoCount = 1U;
        submit_info.pCommandBufferInfos = &command_info;
        result = vkQueueSubmit2(graphics_queue, 1U, &submit_info, fence);
        if (result == VK_SUCCESS) {
            result = vkWaitForFences(device, 1U, &fence, VK_TRUE, UINT64_MAX);
        }
        vkDestroyFence(device, fence, nullptr);
        vkFreeCommandBuffers(device, setup_command_pool, 1U, &command_buffer);
        if (result != VK_SUCCESS) {
            return std::unexpected(vk_error(
                DiagnosticCode::vk_device_unsupported, "Setup queue submission failed", result));
        }
        return {};
    }

    bool valid_texture(const TextureHandle handle) const noexcept {
        return handle.valid() && handle.index < textures.size() && textures[handle.index].occupied &&
               textures[handle.index].generation == handle.generation;
    }

    Diagnostic invalid_texture(const TextureHandle handle) const {
        auto diagnostic = Diagnostic::make(
            DiagnosticCode::render_invalid_texture_handle,
            Severity::error,
            "renderer2d",
            "A sprite references an invalid or stale texture handle");
        diagnostic.context.push_back({"texture_index", static_cast<std::uint64_t>(handle.index)});
        diagnostic.context.push_back({"texture_generation", static_cast<std::uint64_t>(handle.generation)});
        return diagnostic;
    }

    Result<TextureHandle> create_texture_rgba(
        const std::uint32_t width,
        const std::uint32_t height,
        const std::span<const std::uint8_t> rgba) {
        const std::uint64_t expected_size = static_cast<std::uint64_t>(width) *
                                            static_cast<std::uint64_t>(height) * 4U;
        if (!ready || width == 0U || height == 0U || width > 16384U || height > 16384U ||
            expected_size != rgba.size()) {
            auto diagnostic = Diagnostic::make(
                DiagnosticCode::input_invalid,
                Severity::error,
                "renderer2d",
                "RGBA texture dimensions and byte count are invalid");
            diagnostic.context.push_back({"width", static_cast<std::uint64_t>(width)});
            diagnostic.context.push_back({"height", static_cast<std::uint64_t>(height)});
            diagnostic.context.push_back({"provided_bytes", static_cast<std::uint64_t>(rgba.size())});
            diagnostic.context.push_back({"expected_bytes", expected_size});
            return std::unexpected(std::move(diagnostic));
        }

        auto free_slot = std::ranges::find_if(textures, [](const TextureSlot& slot) { return !slot.occupied; });
        if (free_slot == textures.end()) {
            auto diagnostic = Diagnostic::make(
                DiagnosticCode::render_upload_capacity_exceeded,
                Severity::error,
                "renderer2d",
                "Texture registry capacity is exhausted");
            diagnostic.context.push_back({"max_textures", static_cast<std::uint64_t>(textures.size())});
            return std::unexpected(std::move(diagnostic));
        }
        const auto slot_index = static_cast<std::uint32_t>(std::distance(textures.begin(), free_slot));

        AllocatedBuffer staging{};
        VkBufferCreateInfo buffer_info{};
        buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer_info.size = expected_size;
        buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo staging_allocation_info{};
        staging_allocation_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                        VMA_ALLOCATION_CREATE_MAPPED_BIT;
        staging_allocation_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        VmaAllocationInfo staging_result{};
        VkResult result = vmaCreateBuffer(
            allocator,
            &buffer_info,
            &staging_allocation_info,
            &staging.handle,
            &staging.allocation,
            &staging_result);
        if (result != VK_SUCCESS || staging_result.pMappedData == nullptr) {
            return std::unexpected(
                vk_error(DiagnosticCode::vk_device_unsupported, "Texture staging buffer allocation failed", result));
        }
        staging.mapped = staging_result.pMappedData;
        staging.size = expected_size;
        std::memcpy(staging.mapped, rgba.data(), rgba.size());
        result = vmaFlushAllocation(allocator, staging.allocation, 0U, expected_size);
        if (result != VK_SUCCESS) {
            staging.reset(allocator);
            return std::unexpected(
                vk_error(DiagnosticCode::vk_device_unsupported, "Texture staging buffer flush failed", result));
        }

        AllocatedImage image{};
        VkImageCreateInfo image_info{};
        image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image_info.imageType = VK_IMAGE_TYPE_2D;
        image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
        image_info.extent = {width, height, 1U};
        image_info.mipLevels = 1U;
        image_info.arrayLayers = 1U;
        image_info.samples = VK_SAMPLE_COUNT_1_BIT;
        image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        image_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VmaAllocationCreateInfo image_allocation_info{};
        image_allocation_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        result = vmaCreateImage(
            allocator,
            &image_info,
            &image_allocation_info,
            &image.handle,
            &image.allocation,
            nullptr);
        if (result != VK_SUCCESS) {
            staging.reset(allocator);
            return std::unexpected(
                vk_error(DiagnosticCode::vk_device_unsupported, "Texture image allocation failed", result));
        }

        VkImageViewCreateInfo view_info{};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = image.handle;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
        view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view_info.subresourceRange.levelCount = 1U;
        view_info.subresourceRange.layerCount = 1U;
        result = vkCreateImageView(device, &view_info, nullptr, &image.view);
        if (result != VK_SUCCESS) {
            image.reset(allocator, device);
            staging.reset(allocator);
            return std::unexpected(
                vk_error(DiagnosticCode::vk_device_unsupported, "Texture image view creation failed", result));
        }

        auto command = begin_immediate_commands();
        if (!command) {
            image.reset(allocator, device);
            staging.reset(allocator);
            return std::unexpected(std::move(command.error()));
        }
        VkImageMemoryBarrier2 to_transfer{};
        to_transfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        to_transfer.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
        to_transfer.srcAccessMask = VK_ACCESS_2_NONE;
        to_transfer.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        to_transfer.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        to_transfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        to_transfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_transfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_transfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_transfer.image = image.handle;
        to_transfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        to_transfer.subresourceRange.levelCount = 1U;
        to_transfer.subresourceRange.layerCount = 1U;
        VkDependencyInfo dependency{};
        dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.imageMemoryBarrierCount = 1U;
        dependency.pImageMemoryBarriers = &to_transfer;
        vkCmdPipelineBarrier2(command.value(), &dependency);
        VkBufferImageCopy copy{};
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1U;
        copy.imageExtent = {width, height, 1U};
        vkCmdCopyBufferToImage(
            command.value(), staging.handle, image.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1U, &copy);
        VkImageMemoryBarrier2 to_sampled = to_transfer;
        to_sampled.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        to_sampled.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        to_sampled.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        to_sampled.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        to_sampled.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_sampled.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        dependency.pImageMemoryBarriers = &to_sampled;
        vkCmdPipelineBarrier2(command.value(), &dependency);
        auto submitted = finish_immediate_commands(command.value());
        staging.reset(allocator);
        if (!submitted) {
            image.reset(allocator, device);
            return std::unexpected(std::move(submitted.error()));
        }

        VkDescriptorSetAllocateInfo descriptor_allocate{};
        descriptor_allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        descriptor_allocate.descriptorPool = texture_descriptor_pool;
        descriptor_allocate.descriptorSetCount = 1U;
        descriptor_allocate.pSetLayouts = &texture_set_layout;
        VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
        result = vkAllocateDescriptorSets(device, &descriptor_allocate, &descriptor_set);
        if (result != VK_SUCCESS) {
            image.reset(allocator, device);
            return std::unexpected(
                vk_error(DiagnosticCode::vk_device_unsupported, "Texture descriptor allocation failed", result));
        }
        VkDescriptorImageInfo image_descriptor{};
        image_descriptor.imageView = image.view;
        image_descriptor.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkDescriptorImageInfo sampler_descriptor{};
        sampler_descriptor.sampler = sampler;
        std::array<VkWriteDescriptorSet, 2U> writes{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = descriptor_set;
        writes[0].dstBinding = 0U;
        writes[0].descriptorCount = 1U;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        writes[0].pImageInfo = &image_descriptor;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = descriptor_set;
        writes[1].dstBinding = 1U;
        writes[1].descriptorCount = 1U;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        writes[1].pImageInfo = &sampler_descriptor;
        vkUpdateDescriptorSets(device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0U, nullptr);

        free_slot->image = std::move(image);
        free_slot->descriptor_set = descriptor_set;
        free_slot->width = width;
        free_slot->height = height;
        free_slot->occupied = true;
        texture_bytes += expected_size;
        refresh_memory_metrics();
        return TextureHandle{slot_index, free_slot->generation};
    }

    Result<TextureHandle> create_checker_texture(
        const std::uint32_t width,
        const std::uint32_t height,
        const Color first,
        const Color second,
        const std::uint32_t cell_size) {
        if (width == 0U || height == 0U || cell_size == 0U) {
            auto diagnostic = Diagnostic::make(
                DiagnosticCode::input_invalid,
                Severity::error,
                "renderer2d",
                "Checker texture dimensions and cell size must be nonzero");
            return std::unexpected(std::move(diagnostic));
        }
        const std::uint64_t byte_count = static_cast<std::uint64_t>(width) * height * 4U;
        if (byte_count > 1024U * 1024U * 256U) {
            auto diagnostic = Diagnostic::make(
                DiagnosticCode::input_invalid, Severity::error, "renderer2d", "Checker texture is too large");
            return std::unexpected(std::move(diagnostic));
        }
        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(byte_count));
        for (std::uint32_t y = 0U; y < height; ++y) {
            for (std::uint32_t x = 0U; x < width; ++x) {
                const Color color = ((x / cell_size) + (y / cell_size)) % 2U == 0U ? first : second;
                const auto offset = (static_cast<std::size_t>(y) * width + x) * 4U;
                pixels[offset] = color_byte(color.r);
                pixels[offset + 1U] = color_byte(color.g);
                pixels[offset + 2U] = color_byte(color.b);
                pixels[offset + 3U] = color_byte(color.a);
            }
        }
        return create_texture_rgba(width, height, pixels);
    }

    Result<void> destroy_texture(const TextureHandle handle) {
        if (!valid_texture(handle)) {
            return std::unexpected(invalid_texture(handle));
        }
        const VkResult idle_result = vkDeviceWaitIdle(device);
        if (idle_result != VK_SUCCESS) {
            return std::unexpected(vk_error(
                DiagnosticCode::vk_device_unsupported, "Device idle wait before texture destruction failed", idle_result));
        }
        auto& slot = textures[handle.index];
        if (slot.descriptor_set != VK_NULL_HANDLE) {
            const VkResult free_result =
                vkFreeDescriptorSets(device, texture_descriptor_pool, 1U, &slot.descriptor_set);
            if (free_result != VK_SUCCESS) {
                return std::unexpected(vk_error(
                    DiagnosticCode::vk_device_unsupported, "Texture descriptor release failed", free_result));
            }
        }
        texture_bytes -= static_cast<std::uint64_t>(slot.width) * slot.height * 4U;
        slot.image.reset(allocator, device);
        slot.descriptor_set = VK_NULL_HANDLE;
        slot.width = 0U;
        slot.height = 0U;
        slot.occupied = false;
        ++slot.generation;
        if (slot.generation == 0U) {
            slot.generation = 1U;
        }
        refresh_memory_metrics();
        return {};
    }

    void refresh_memory_metrics() noexcept {
        if (allocator == VK_NULL_HANDLE) {
            vma_allocation_count = 0U;
            vma_allocation_bytes = 0U;
            return;
        }
        VmaTotalStatistics statistics{};
        vmaCalculateStatistics(allocator, &statistics);
        vma_allocation_count = statistics.total.statistics.allocationCount;
        vma_allocation_bytes = statistics.total.statistics.allocationBytes;
    }

    Result<void> create_offscreen(const std::uint32_t width, const std::uint32_t height) {
        const auto safe_width = std::max(width, 1U);
        const auto safe_height = std::max(height, 1U);
        offscreen_extent = {safe_width, safe_height};

        VkImageCreateInfo image_info{};
        image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image_info.imageType = VK_IMAGE_TYPE_2D;
        image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
        image_info.extent = {safe_width, safe_height, 1U};
        image_info.mipLevels = 1U;
        image_info.arrayLayers = 1U;
        image_info.samples = VK_SAMPLE_COUNT_1_BIT;
        image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VmaAllocationCreateInfo allocation_info{};
        allocation_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        VkResult result = vmaCreateImage(
            allocator, &image_info, &allocation_info, &offscreen_image, &offscreen_allocation, nullptr);
        if (result != VK_SUCCESS) {
            return std::unexpected(
                vk_error(DiagnosticCode::vk_device_unsupported, "Offscreen image allocation failed", result));
        }
#if defined(AI2D_ENABLE_TEST_HOOKS)
        if ((!ready && inject_once(TestFault::initialize_offscreen_view_once)) ||
            (ready && inject_once(TestFault::resize_offscreen_view_once))) {
            return std::unexpected(vk_error(
                DiagnosticCode::vk_device_unsupported,
                "Injected offscreen image-view creation failure",
                VK_ERROR_OUT_OF_DEVICE_MEMORY));
        }
#endif

        VkImageViewCreateInfo view_info{};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = offscreen_image;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = image_info.format;
        view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view_info.subresourceRange.levelCount = 1U;
        view_info.subresourceRange.layerCount = 1U;
        result = vkCreateImageView(device, &view_info, nullptr, &offscreen_view);
        if (result != VK_SUCCESS) {
            return std::unexpected(
                vk_error(DiagnosticCode::vk_device_unsupported, "Offscreen image view creation failed", result));
        }
        offscreen_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        return {};
    }

    void destroy_offscreen() noexcept {
        if (offscreen_view != VK_NULL_HANDLE) {
            vkDestroyImageView(device, offscreen_view, nullptr);
            offscreen_view = VK_NULL_HANDLE;
        }
        if (offscreen_image != VK_NULL_HANDLE) {
            vmaDestroyImage(allocator, offscreen_image, offscreen_allocation);
            offscreen_image = VK_NULL_HANDLE;
            offscreen_allocation = VK_NULL_HANDLE;
        }
        offscreen_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    }

    Result<void> create_swapchain(const std::uint32_t width, const std::uint32_t height) {
        VkSurfaceCapabilitiesKHR surface_capabilities{};
        VkResult result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
            physical_device, surface, &surface_capabilities);
        if (result != VK_SUCCESS) {
            return std::unexpected(vk_error(
                DiagnosticCode::vk_swapchain_error, "Could not query surface capabilities", result));
        }

        std::uint32_t format_count = 0U;
        result = vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device, surface, &format_count, nullptr);
        if (result != VK_SUCCESS || format_count == 0U) {
            return std::unexpected(vk_error(
                DiagnosticCode::vk_swapchain_error, "The surface has no usable formats", result));
        }
        std::vector<VkSurfaceFormatKHR> formats(format_count);
        result = vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device, surface, &format_count, formats.data());
        if (result != VK_SUCCESS) {
            return std::unexpected(vk_error(
                DiagnosticCode::vk_swapchain_error, "Could not read surface formats", result));
        }
        auto chosen_format = formats.front();
        for (const auto& format : formats) {
            if (format.format == VK_FORMAT_B8G8R8A8_UNORM &&
                format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                chosen_format = format;
                break;
            }
        }

        VkExtent2D extent{};
        if (surface_capabilities.currentExtent.width != std::numeric_limits<std::uint32_t>::max()) {
            extent = surface_capabilities.currentExtent;
        } else {
            extent.width = std::clamp(
                width, surface_capabilities.minImageExtent.width, surface_capabilities.maxImageExtent.width);
            extent.height = std::clamp(
                height, surface_capabilities.minImageExtent.height, surface_capabilities.maxImageExtent.height);
        }
        if (extent.width == 0U || extent.height == 0U) {
            swapchain_extent = extent;
            return {};
        }

        std::uint32_t image_count = surface_capabilities.minImageCount + 1U;
        if (surface_capabilities.maxImageCount > 0U) {
            image_count = std::min(image_count, surface_capabilities.maxImageCount);
        }
        std::uint32_t present_mode_count = 0U;
        result = vkGetPhysicalDeviceSurfacePresentModesKHR(
            physical_device, surface, &present_mode_count, nullptr);
        if (result != VK_SUCCESS || present_mode_count == 0U) {
            return std::unexpected(vk_error(
                DiagnosticCode::vk_swapchain_error, "The surface has no usable present modes", result));
        }
        std::vector<VkPresentModeKHR> present_modes(present_mode_count);
        result = vkGetPhysicalDeviceSurfacePresentModesKHR(
            physical_device, surface, &present_mode_count, present_modes.data());
        if (result != VK_SUCCESS) {
            return std::unexpected(vk_error(
                DiagnosticCode::vk_swapchain_error, "Could not read surface present modes", result));
        }
        VkPresentModeKHR chosen_present_mode = VK_PRESENT_MODE_FIFO_KHR;
        if (options.requested_fps_cap != RenderFpsCap::fps_60) {
            const auto has_mode = [&](const VkPresentModeKHR candidate) {
                return std::find(present_modes.begin(), present_modes.end(), candidate) != present_modes.end();
            };
            if (has_mode(VK_PRESENT_MODE_MAILBOX_KHR)) chosen_present_mode = VK_PRESENT_MODE_MAILBOX_KHR;
            else if (has_mode(VK_PRESENT_MODE_IMMEDIATE_KHR)) chosen_present_mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
        }
        capabilities.effective_present_mode = present_mode_from_vk(chosen_present_mode);

        VkSwapchainCreateInfoKHR create_info{};
        create_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        create_info.surface = surface;
        create_info.minImageCount = image_count;
        create_info.imageFormat = chosen_format.format;
        create_info.imageColorSpace = chosen_format.colorSpace;
        create_info.imageExtent = extent;
        create_info.imageArrayLayers = 1U;
        create_info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        create_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        create_info.preTransform = surface_capabilities.currentTransform;
        create_info.compositeAlpha = static_cast<VkCompositeAlphaFlagBitsKHR>(
            first_set_bit(surface_capabilities.supportedCompositeAlpha));
        create_info.presentMode = chosen_present_mode;
        create_info.clipped = VK_TRUE;
        create_info.oldSwapchain = VK_NULL_HANDLE;
        result = vkCreateSwapchainKHR(device, &create_info, nullptr, &swapchain);
        if (result != VK_SUCCESS) {
            return std::unexpected(
                vk_error(DiagnosticCode::vk_swapchain_error, "Swapchain creation failed", result));
        }

        swapchain_format = chosen_format.format;
        swapchain_extent = extent;
        result = vkGetSwapchainImagesKHR(device, swapchain, &image_count, nullptr);
        if (result != VK_SUCCESS || image_count == 0U) {
            return std::unexpected(vk_error(
                DiagnosticCode::vk_swapchain_error, "Could not query swapchain images", result));
        }
        swapchain_images.resize(image_count);
        result = vkGetSwapchainImagesKHR(device, swapchain, &image_count, swapchain_images.data());
        if (result != VK_SUCCESS) {
            return std::unexpected(vk_error(
                DiagnosticCode::vk_swapchain_error, "Could not read swapchain images", result));
        }
        swapchain_views.reserve(image_count);
        swapchain_present_semaphores.reserve(image_count);
        for (const auto image : swapchain_images) {
            VkImageViewCreateInfo view_info{};
            view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            view_info.image = image;
            view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view_info.format = swapchain_format;
            view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            view_info.subresourceRange.levelCount = 1U;
            view_info.subresourceRange.layerCount = 1U;
            VkImageView view = VK_NULL_HANDLE;
            result = vkCreateImageView(device, &view_info, nullptr, &view);
            if (result != VK_SUCCESS) {
                return std::unexpected(vk_error(
                    DiagnosticCode::vk_swapchain_error, "Swapchain image view creation failed", result));
            }
            swapchain_views.push_back(view);

            VkSemaphoreCreateInfo semaphore_info{};
            semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            VkSemaphore present_semaphore = VK_NULL_HANDLE;
            result = vkCreateSemaphore(device, &semaphore_info, nullptr, &present_semaphore);
            if (result != VK_SUCCESS) {
                return std::unexpected(vk_error(
                    DiagnosticCode::vk_swapchain_error,
                    "Per-swapchain-image presentation semaphore creation failed",
                    result));
            }
            swapchain_present_semaphores.push_back(present_semaphore);
        }
        return {};
    }

    void destroy_swapchain() noexcept {
        for (const auto semaphore : swapchain_present_semaphores) {
            vkDestroySemaphore(device, semaphore, nullptr);
        }
        swapchain_present_semaphores.clear();
        for (const auto view : swapchain_views) {
            vkDestroyImageView(device, view, nullptr);
        }
        swapchain_views.clear();
        swapchain_images.clear();
        if (swapchain != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(device, swapchain, nullptr);
            swapchain = VK_NULL_HANDLE;
        }
        swapchain_extent = {};
        swapchain_format = VK_FORMAT_UNDEFINED;
    }

    Result<GpuFrameMetrics> submit_frame(
        const Color color,
        const std::span<const PreparedSprite2D> instances,
        const std::span<const SpriteBatch2D> batches,
        double* const upload_milliseconds) {
        if (!ready) {
            auto diagnostic = Diagnostic::make(
                DiagnosticCode::internal_error, Severity::error, "renderer2d", "Renderer is not initialized");
            return std::unexpected(std::move(diagnostic));
        }
        if (options.require_present &&
            (swapchain == VK_NULL_HANDLE || swapchain_extent.width == 0U || swapchain_extent.height == 0U)) {
            return GpuFrameMetrics{frame_index, 0.0, 0.0, false, false, 0U, 0U};
        }

        Stopwatch cpu_timer{};
        FrameSlot& frame = frames[frame_index % frames_in_flight];
        VkResult result = vkWaitForFences(device, 1U, &frame.fence, VK_TRUE, UINT64_MAX);
        if (result != VK_SUCCESS) {
            auto diagnostic =
                vk_error(DiagnosticCode::vk_device_unsupported, "Waiting for the frame fence failed", result);
            shutdown();
            return std::unexpected(std::move(diagnostic));
        }
        if (frame.timestamp_pending && frame.timestamp_queries != VK_NULL_HANDLE) {
            std::array<std::uint64_t, 2U> timestamps{};
            result = vkGetQueryPoolResults(
                device,
                frame.timestamp_queries,
                0U,
                2U,
                sizeof(timestamps),
                timestamps.data(),
                sizeof(std::uint64_t),
                VK_QUERY_RESULT_64_BIT);
            frame.completed_gpu_valid = result == VK_SUCCESS && timestamps[1] >= timestamps[0];
            if (frame.completed_gpu_valid) {
                frame.completed_gpu_ms = static_cast<double>(timestamps[1] - timestamps[0]) *
                                         static_cast<double>(capabilities.timestamp_period_nanoseconds) / 1'000'000.0;
            }
            frame.timestamp_pending = false;
        }

        const auto upload_bytes = instances.size_bytes();
        if (upload_bytes > frame.instance_upload.size) {
            auto diagnostic = Diagnostic::make(
                DiagnosticCode::render_upload_capacity_exceeded,
                Severity::error,
                "renderer2d",
                "Prepared sprite data exceeds the per-frame upload buffer");
            diagnostic.context.push_back({"required_bytes", static_cast<std::uint64_t>(upload_bytes)});
            diagnostic.context.push_back(
                {"reserved_bytes", static_cast<std::uint64_t>(frame.instance_upload.size)});
            return std::unexpected(std::move(diagnostic));
        }
        Stopwatch upload_timer{};
        if (upload_bytes > 0U) {
            std::memcpy(frame.instance_upload.mapped, instances.data(), upload_bytes);
            result = vmaFlushAllocation(allocator, frame.instance_upload.allocation, 0U, upload_bytes);
            if (result != VK_SUCCESS) {
                auto diagnostic = vk_error(
                    DiagnosticCode::vk_device_unsupported, "Instance upload buffer flush failed", result);
                shutdown();
                return std::unexpected(std::move(diagnostic));
            }
        }
        if (upload_milliseconds != nullptr) {
            *upload_milliseconds = upload_timer.elapsed_milliseconds();
        }

        std::uint32_t image_index = 0U;
        bool image_acquired = false;
        const auto fail_before_submit = [&](Diagnostic diagnostic) -> Result<GpuFrameMetrics> {
            if (image_acquired) shutdown();
            return std::unexpected(std::move(diagnostic));
        };
        if (options.require_present) {
#if defined(AI2D_ENABLE_TEST_HOOKS)
            if (inject_once(TestFault::acquire_out_of_date_once)) {
                result = VK_ERROR_OUT_OF_DATE_KHR;
            } else
#endif
            {
            result = vkAcquireNextImageKHR(
                device, swapchain, UINT64_MAX, frame.image_available, VK_NULL_HANDLE, &image_index);
            }
            if (result == VK_ERROR_OUT_OF_DATE_KHR) {
                if (auto recreated = resize(swapchain_extent.width, swapchain_extent.height); !recreated) {
                    return std::unexpected(std::move(recreated.error()));
                }
                return GpuFrameMetrics{
                    frame_index, cpu_timer.elapsed_milliseconds(), 0.0, false, false,
                    swapchain_extent.width, swapchain_extent.height};
            }
            if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
                auto diagnostic =
                    vk_error(DiagnosticCode::vk_swapchain_error, "Swapchain image acquisition failed", result);
                shutdown();
                return std::unexpected(std::move(diagnostic));
            }
            image_acquired = true;
        }

        #if defined(AI2D_ENABLE_TEST_HOOKS)
        if (inject_once(TestFault::reset_command_pool_once)) {
            result = VK_ERROR_UNKNOWN;
        } else
        #endif
        {
            result = vkResetCommandPool(device, frame.command_pool, 0U);
        }
        if (result != VK_SUCCESS) {
            return fail_before_submit(
                vk_error(DiagnosticCode::vk_device_unsupported, "Resetting the frame command pool failed", result));
        }

        VkCommandBufferBeginInfo begin_info{};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
#if defined(AI2D_ENABLE_TEST_HOOKS)
        if (inject_once(TestFault::begin_command_once)) {
            return fail_before_submit(vk_error(
                DiagnosticCode::vk_device_unsupported,
                "Injected frame command-buffer begin failure",
                VK_ERROR_UNKNOWN));
        }
#endif
        result = vkBeginCommandBuffer(frame.command_buffer, &begin_info);
        if (result != VK_SUCCESS) {
            return fail_before_submit(vk_error(
                DiagnosticCode::vk_device_unsupported, "Beginning the frame command buffer failed", result));
        }
        if (frame.timestamp_queries != VK_NULL_HANDLE) {
            vkCmdResetQueryPool(frame.command_buffer, frame.timestamp_queries, 0U, 2U);
            vkCmdWriteTimestamp2(
                frame.command_buffer,
                VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                frame.timestamp_queries,
                0U);
        }

        const VkImage target_image = options.require_present ? swapchain_images[image_index] : offscreen_image;
        const VkImageView target_view = options.require_present ? swapchain_views[image_index] : offscreen_view;
        const VkExtent2D target_extent = options.require_present ? swapchain_extent : offscreen_extent;
        const VkImageLayout old_layout = options.require_present ? VK_IMAGE_LAYOUT_UNDEFINED : offscreen_layout;

        VkImageMemoryBarrier2 to_color{};
        to_color.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        // The presented-image transition itself must be in the acquire
        // semaphore's wait scope. For a private offscreen image, NONE remains
        // valid on the first undefined-to-color transition.
        to_color.srcStageMask = options.require_present
                                    ? VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT
                                    : old_layout == VK_IMAGE_LAYOUT_UNDEFINED
                                          ? VK_PIPELINE_STAGE_2_NONE
                                          : VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        to_color.srcAccessMask = old_layout == VK_IMAGE_LAYOUT_UNDEFINED
                                     ? VK_ACCESS_2_NONE
                                     : VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        to_color.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        to_color.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        to_color.oldLayout = old_layout;
        to_color.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        to_color.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_color.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_color.image = target_image;
        to_color.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        to_color.subresourceRange.levelCount = 1U;
        to_color.subresourceRange.layerCount = 1U;
        VkDependencyInfo dependency{};
        dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.imageMemoryBarrierCount = 1U;
        dependency.pImageMemoryBarriers = &to_color;
        vkCmdPipelineBarrier2(frame.command_buffer, &dependency);

        VkClearValue clear_value{};
        clear_value.color = {{color.r, color.g, color.b, color.a}};
        VkRenderingAttachmentInfo color_attachment{};
        color_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        color_attachment.imageView = target_view;
        color_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color_attachment.clearValue = clear_value;
        VkRenderingInfo rendering_info{};
        rendering_info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        rendering_info.renderArea.extent = target_extent;
        rendering_info.layerCount = 1U;
        rendering_info.colorAttachmentCount = 1U;
        rendering_info.pColorAttachments = &color_attachment;
        vkCmdBeginRendering(frame.command_buffer, &rendering_info);
        if (!batches.empty()) {
            const VkPipeline pipeline = options.require_present ? present_pipeline : offscreen_pipeline;
            VkViewport viewport{};
            viewport.width = static_cast<float>(target_extent.width);
            viewport.height = static_cast<float>(target_extent.height);
            viewport.minDepth = 0.0F;
            viewport.maxDepth = 1.0F;
            VkRect2D scissor{};
            scissor.extent = target_extent;
            vkCmdSetViewport(frame.command_buffer, 0U, 1U, &viewport);
            vkCmdSetScissor(frame.command_buffer, 0U, 1U, &scissor);
            vkCmdBindPipeline(frame.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            const VkDeviceSize vertex_offset = 0U;
            vkCmdBindVertexBuffers(
                frame.command_buffer, 0U, 1U, &frame.instance_upload.handle, &vertex_offset);
            for (const auto& batch : batches) {
                const auto& texture = textures[batch.texture.index];
                vkCmdBindDescriptorSets(
                    frame.command_buffer,
                    VK_PIPELINE_BIND_POINT_GRAPHICS,
                    pipeline_layout,
                    0U,
                    1U,
                    &texture.descriptor_set,
                    0U,
                    nullptr);
                vkCmdDraw(
                    frame.command_buffer,
                    6U,
                    batch.instance_count,
                    0U,
                    batch.first_instance);
            }
        }
        vkCmdEndRendering(frame.command_buffer);

        VkImageMemoryBarrier2 after_color{};
        if (options.require_present) {
            after_color.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            after_color.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            after_color.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
            after_color.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
            after_color.dstAccessMask = VK_ACCESS_2_NONE;
            after_color.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            after_color.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            after_color.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            after_color.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            after_color.image = target_image;
            after_color.subresourceRange = to_color.subresourceRange;
            dependency.pImageMemoryBarriers = &after_color;
            vkCmdPipelineBarrier2(frame.command_buffer, &dependency);
        }
        if (frame.timestamp_queries != VK_NULL_HANDLE) {
            vkCmdWriteTimestamp2(
                frame.command_buffer,
                VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
                frame.timestamp_queries,
                1U);
        }
#if defined(AI2D_ENABLE_TEST_HOOKS)
        if (inject_once(TestFault::end_command_once)) {
            return fail_before_submit(vk_error(
                DiagnosticCode::vk_device_unsupported,
                "Injected frame command-buffer end failure",
                VK_ERROR_UNKNOWN));
        }
#endif
        result = vkEndCommandBuffer(frame.command_buffer);
        if (result != VK_SUCCESS) {
            return fail_before_submit(vk_error(
                DiagnosticCode::vk_device_unsupported, "Ending the frame command buffer failed", result));
        }

        VkSemaphoreSubmitInfo wait_info{};
        wait_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        wait_info.semaphore = frame.image_available;
        wait_info.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSemaphoreSubmitInfo signal_info{};
        signal_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        signal_info.semaphore = options.require_present
                                    ? swapchain_present_semaphores[image_index]
                                    : VK_NULL_HANDLE;
        // Include the final color-to-present layout transition in the signal
        // scope, not only the rendering commands before it.
        signal_info.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        VkCommandBufferSubmitInfo command_submit{};
        command_submit.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        command_submit.commandBuffer = frame.command_buffer;
        VkSubmitInfo2 submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        submit.waitSemaphoreInfoCount = options.require_present ? 1U : 0U;
        submit.pWaitSemaphoreInfos = options.require_present ? &wait_info : nullptr;
        submit.commandBufferInfoCount = 1U;
        submit.pCommandBufferInfos = &command_submit;
        submit.signalSemaphoreInfoCount = options.require_present ? 1U : 0U;
        submit.pSignalSemaphoreInfos = options.require_present ? &signal_info : nullptr;
        result = vkResetFences(device, 1U, &frame.fence);
        if (result != VK_SUCCESS) {
            auto diagnostic = vk_error(
                DiagnosticCode::vk_device_unsupported,
                "Resetting the frame fence before submission failed",
                result);
            shutdown();
            return std::unexpected(std::move(diagnostic));
        }
#if defined(AI2D_ENABLE_TEST_HOOKS)
        if (inject_once(TestFault::submit_once)) {
            result = VK_ERROR_DEVICE_LOST;
        } else
#endif
        {
            result = vkQueueSubmit2(graphics_queue, 1U, &submit, frame.fence);
        }
        if (result != VK_SUCCESS) {
            auto diagnostic = vk_error(
                DiagnosticCode::vk_device_unsupported,
                "Graphics queue submission failed; renderer entered a terminal uninitialized state",
                result);
            shutdown();
            return std::unexpected(std::move(diagnostic));
        }
        if (!options.require_present) {
            offscreen_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        }
        frame.timestamp_pending = frame.timestamp_queries != VK_NULL_HANDLE;

        bool presented = false;
        if (options.require_present) {
            VkPresentInfoKHR present_info{};
            present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
            present_info.waitSemaphoreCount = 1U;
            present_info.pWaitSemaphores = &swapchain_present_semaphores[image_index];
            present_info.swapchainCount = 1U;
            present_info.pSwapchains = &swapchain;
            present_info.pImageIndices = &image_index;
            result = vkQueuePresentKHR(graphics_queue, &present_info);
            if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR && result != VK_ERROR_OUT_OF_DATE_KHR) {
                auto diagnostic =
                    vk_error(DiagnosticCode::vk_swapchain_error, "Swapchain presentation failed", result);
                shutdown();
                return std::unexpected(std::move(diagnostic));
            }
            presented = result != VK_ERROR_OUT_OF_DATE_KHR;
        }

        const GpuFrameMetrics metrics{
            frame_index,
            cpu_timer.elapsed_milliseconds(),
            frame.completed_gpu_ms,
            frame.completed_gpu_valid,
            presented,
            target_extent.width,
            target_extent.height,
        };
        ++frame_index;
        return metrics;
    }

    Result<GpuFrameMetrics> clear(const Color color) {
        return submit_frame(color, {}, {}, nullptr);
    }

    Result<RendererMetrics> render(const RenderFrame2D& frame) {
        Stopwatch queue_timer{};
        auto built = render_queue.build(frame);
        if (!built) {
            return std::unexpected(std::move(built.error()));
        }
        const double queue_build_ms = queue_timer.elapsed_milliseconds();
        for (const auto& batch : render_queue.batches()) {
            if (!valid_texture(batch.texture)) {
                return std::unexpected(invalid_texture(batch.texture));
            }
        }
        double upload_ms = 0.0;
        auto submitted = submit_frame(
            frame.clear_color, render_queue.instances(), render_queue.batches(), &upload_ms);
        if (!submitted) {
            return std::unexpected(std::move(submitted.error()));
        }
        RendererMetrics metrics{};
        metrics.gpu = submitted.value();
        metrics.queue = built.value();
        metrics.sprite_draw_calls = render_queue.batches().size();
        metrics.texture_binds = render_queue.batches().size();
        metrics.instance_upload_bytes = render_queue.instances().size_bytes();
        metrics.capacity_growth_events = built->capacity_growth_events;
        metrics.vma_allocation_count = vma_allocation_count;
        metrics.vma_allocation_bytes = vma_allocation_bytes;
        metrics.texture_bytes = texture_bytes;
        metrics.instance_buffer_capacity_bytes = instance_buffer_capacity_bytes;
        metrics.queue_build_ms = queue_build_ms;
        metrics.upload_ms = upload_ms;
        return metrics;
    }

    Result<Color> read_offscreen_pixel(const std::uint32_t x, const std::uint32_t y) {
        if (!ready || options.require_present || offscreen_layout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
            auto diagnostic = Diagnostic::make(
                DiagnosticCode::input_invalid,
                Severity::error,
                "renderer2d",
                "Pixel readback requires a completed frame from an offscreen renderer");
            return std::unexpected(std::move(diagnostic));
        }
        if (x >= offscreen_extent.width || y >= offscreen_extent.height) {
            auto diagnostic = Diagnostic::make(
                DiagnosticCode::input_invalid,
                Severity::error,
                "renderer2d",
                "Offscreen pixel coordinate is outside the target extent");
            diagnostic.context.push_back({"x", static_cast<std::uint64_t>(x)});
            diagnostic.context.push_back({"y", static_cast<std::uint64_t>(y)});
            return std::unexpected(std::move(diagnostic));
        }
        const VkResult idle_result = vkDeviceWaitIdle(device);
        if (idle_result != VK_SUCCESS) {
            return std::unexpected(vk_error(
                DiagnosticCode::vk_device_unsupported, "Device idle wait before pixel readback failed", idle_result));
        }

        AllocatedBuffer readback{};
        VkBufferCreateInfo buffer_info{};
        buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer_info.size = 4U;
        buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VmaAllocationCreateInfo allocation_info{};
        allocation_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        allocation_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        VmaAllocationInfo allocation_result{};
        VkResult result = vmaCreateBuffer(
            allocator,
            &buffer_info,
            &allocation_info,
            &readback.handle,
            &readback.allocation,
            &allocation_result);
        if (result != VK_SUCCESS || allocation_result.pMappedData == nullptr) {
            return std::unexpected(
                vk_error(DiagnosticCode::vk_device_unsupported, "Pixel readback buffer allocation failed", result));
        }
        readback.mapped = allocation_result.pMappedData;
        readback.size = 4U;

        auto command = begin_immediate_commands();
        if (!command) {
            readback.reset(allocator);
            return std::unexpected(std::move(command.error()));
        }
        VkImageMemoryBarrier2 to_transfer{};
        to_transfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        to_transfer.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        to_transfer.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        to_transfer.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        to_transfer.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        to_transfer.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        to_transfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        to_transfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_transfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_transfer.image = offscreen_image;
        to_transfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        to_transfer.subresourceRange.levelCount = 1U;
        to_transfer.subresourceRange.layerCount = 1U;
        VkDependencyInfo dependency{};
        dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.imageMemoryBarrierCount = 1U;
        dependency.pImageMemoryBarriers = &to_transfer;
        vkCmdPipelineBarrier2(command.value(), &dependency);
        VkBufferImageCopy copy{};
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1U;
        copy.imageOffset = {static_cast<std::int32_t>(x), static_cast<std::int32_t>(y), 0};
        copy.imageExtent = {1U, 1U, 1U};
        vkCmdCopyImageToBuffer(
            command.value(), offscreen_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.handle, 1U, &copy);
        VkImageMemoryBarrier2 to_color = to_transfer;
        to_color.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        to_color.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        to_color.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        to_color.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        to_color.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        to_color.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        dependency.pImageMemoryBarriers = &to_color;
        vkCmdPipelineBarrier2(command.value(), &dependency);
        auto submitted = finish_immediate_commands(command.value());
        if (!submitted) {
            readback.reset(allocator);
            return std::unexpected(std::move(submitted.error()));
        }
        result = vmaInvalidateAllocation(allocator, readback.allocation, 0U, 4U);
        if (result != VK_SUCCESS) {
            readback.reset(allocator);
            return std::unexpected(
                vk_error(DiagnosticCode::vk_device_unsupported, "Pixel readback invalidation failed", result));
        }
        std::array<std::uint8_t, 4U> pixel{};
        std::memcpy(pixel.data(), readback.mapped, pixel.size());
        readback.reset(allocator);
        constexpr float inverse_byte = 1.0F / 255.0F;
        return Color{
            pixel[0] * inverse_byte,
            pixel[1] * inverse_byte,
            pixel[2] * inverse_byte,
            pixel[3] * inverse_byte,
        };
    }

    Result<void> resize(const std::uint32_t width, const std::uint32_t height) {
        const auto terminal_failure = [this](Diagnostic diagnostic) -> Result<void> {
            shutdown();
            return std::unexpected(std::move(diagnostic));
        };
        options.width = width;
        options.height = height;
        if (!ready || width == 0U || height == 0U) {
            swapchain_extent = {width, height};
            return {};
        }
        const VkResult idle_result = vkDeviceWaitIdle(device);
        if (idle_result != VK_SUCCESS) {
            return terminal_failure(vk_error(
                DiagnosticCode::vk_device_unsupported, "Device idle wait before resize failed", idle_result));
        }
        destroy_offscreen();
        if (auto result = create_offscreen(width, height); !result) {
            return terminal_failure(std::move(result.error()));
        }
        if (options.require_present) {
            const VkFormat previous_format = swapchain_format;
            destroy_swapchain();
            if (auto result = create_swapchain(width, height); !result) {
                return terminal_failure(std::move(result.error()));
            }
            if (swapchain == VK_NULL_HANDLE) {
                if (present_pipeline != VK_NULL_HANDLE) {
                    vkDestroyPipeline(device, present_pipeline, nullptr);
                    present_pipeline = VK_NULL_HANDLE;
                }
                refresh_memory_metrics();
                return {};
            }
            if (present_pipeline == VK_NULL_HANDLE || swapchain_format != previous_format) {
                auto recreated_pipeline = create_graphics_pipeline(swapchain_format);
                if (!recreated_pipeline) {
                    return terminal_failure(std::move(recreated_pipeline.error()));
                }
                if (present_pipeline != VK_NULL_HANDLE) {
                    vkDestroyPipeline(device, present_pipeline, nullptr);
                }
                present_pipeline = recreated_pipeline.value();
            }
        }
        refresh_memory_metrics();
        return {};
    }

    Result<void> set_fps_cap(const RenderFpsCap cap) {
        if (!ready) {
            return std::unexpected(Diagnostic::make(
                DiagnosticCode::input_invalid,
                Severity::error,
                "renderer2d",
                "Present mode cannot change before renderer initialization"));
        }
        if (options.requested_fps_cap == cap) return {};
        options.requested_fps_cap = cap;
        capabilities.requested_fps_cap = cap;
        capabilities.requested_present_mode = requested_present_mode(cap);
        if (!options.require_present) return {};
        return resize(options.width, options.height);
    }

    Result<void> wait_idle() {
        if (device == VK_NULL_HANDLE) {
            return {};
        }
        const VkResult result = vkDeviceWaitIdle(device);
        if (result != VK_SUCCESS) {
            return std::unexpected(
                vk_error(DiagnosticCode::vk_device_unsupported, "Vulkan device idle wait failed", result));
        }
        return {};
    }

    void shutdown() noexcept {
        if (device != VK_NULL_HANDLE) {
            static_cast<void>(vkDeviceWaitIdle(device));
            destroy_swapchain();
            if (present_pipeline != VK_NULL_HANDLE) {
                vkDestroyPipeline(device, present_pipeline, nullptr);
                present_pipeline = VK_NULL_HANDLE;
            }
            if (offscreen_pipeline != VK_NULL_HANDLE) {
                vkDestroyPipeline(device, offscreen_pipeline, nullptr);
                offscreen_pipeline = VK_NULL_HANDLE;
            }
            if (allocator != VK_NULL_HANDLE) {
                destroy_offscreen();
            }
            for (auto& texture : textures) {
                if (allocator != VK_NULL_HANDLE) {
                    texture.image.reset(allocator, device);
                }
                texture.descriptor_set = VK_NULL_HANDLE;
                texture.occupied = false;
            }
            textures.clear();
            if (texture_descriptor_pool != VK_NULL_HANDLE) {
                vkDestroyDescriptorPool(device, texture_descriptor_pool, nullptr);
                texture_descriptor_pool = VK_NULL_HANDLE;
            }
            if (sampler != VK_NULL_HANDLE) {
                vkDestroySampler(device, sampler, nullptr);
                sampler = VK_NULL_HANDLE;
            }
            if (pipeline_layout != VK_NULL_HANDLE) {
                vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
                pipeline_layout = VK_NULL_HANDLE;
            }
            if (texture_set_layout != VK_NULL_HANDLE) {
                vkDestroyDescriptorSetLayout(device, texture_set_layout, nullptr);
                texture_set_layout = VK_NULL_HANDLE;
            }
            for (auto& frame : frames) {
                if (frame.timestamp_queries != VK_NULL_HANDLE) {
                    vkDestroyQueryPool(device, frame.timestamp_queries, nullptr);
                }
                if (frame.image_available != VK_NULL_HANDLE) {
                    vkDestroySemaphore(device, frame.image_available, nullptr);
                }
                if (frame.fence != VK_NULL_HANDLE) {
                    vkDestroyFence(device, frame.fence, nullptr);
                }
                if (frame.command_pool != VK_NULL_HANDLE) {
                    vkDestroyCommandPool(device, frame.command_pool, nullptr);
                }
                if (allocator != VK_NULL_HANDLE) {
                    frame.instance_upload.reset(allocator);
                }
                frame = {};
            }
            if (setup_command_pool != VK_NULL_HANDLE) {
                vkDestroyCommandPool(device, setup_command_pool, nullptr);
                setup_command_pool = VK_NULL_HANDLE;
            }
            if (allocator != VK_NULL_HANDLE) {
                vmaDestroyAllocator(allocator);
                allocator = VK_NULL_HANDLE;
            }
            vkDestroyDevice(device, nullptr);
            device = VK_NULL_HANDLE;
        }
        if (surface != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(instance, surface, nullptr);
            surface = VK_NULL_HANDLE;
        }
        if (debug_messenger != VK_NULL_HANDLE) {
            const auto destroy_debug = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
            if (destroy_debug != nullptr) {
                destroy_debug(instance, debug_messenger, nullptr);
            }
            debug_messenger = VK_NULL_HANDLE;
        }
        if (instance != VK_NULL_HANDLE) {
            vkDestroyInstance(instance, nullptr);
            instance = VK_NULL_HANDLE;
        }
        ready = false;
    }
};

std::string_view present_mode_name(const PresentMode2D mode) noexcept {
    switch (mode) {
    case PresentMode2D::fifo: return "fifo";
    case PresentMode2D::mailbox: return "mailbox";
    case PresentMode2D::immediate: return "immediate";
    }
    return "fifo";
}

Renderer2D::Renderer2D() : impl_(std::make_unique<Impl>()) {}
Renderer2D::~Renderer2D() = default;
Renderer2D::Renderer2D(Renderer2D&&) noexcept = default;
Renderer2D& Renderer2D::operator=(Renderer2D&&) noexcept = default;

Result<void> Renderer2D::initialize(const RendererOptions& options) {
    const bool was_initialized = impl_->ready;
    auto result = impl_->initialize(options);
    if (!result && !was_initialized) {
        impl_->shutdown();
    }
    return result;
}
Result<GpuFrameMetrics> Renderer2D::clear(const Color color) { return impl_->clear(color); }
Result<TextureHandle> Renderer2D::create_texture_rgba(
    const std::uint32_t width,
    const std::uint32_t height,
    const std::span<const std::uint8_t> rgba) {
    return impl_->create_texture_rgba(width, height, rgba);
}
Result<TextureHandle> Renderer2D::create_checker_texture(
    const std::uint32_t width,
    const std::uint32_t height,
    const Color first,
    const Color second,
    const std::uint32_t cell_size) {
    return impl_->create_checker_texture(width, height, first, second, cell_size);
}
Result<void> Renderer2D::destroy_texture(const TextureHandle texture) {
    return impl_->destroy_texture(texture);
}
Result<RendererMetrics> Renderer2D::render(const RenderFrame2D& frame) { return impl_->render(frame); }
Result<Color> Renderer2D::read_offscreen_pixel(const std::uint32_t x, const std::uint32_t y) {
    return impl_->read_offscreen_pixel(x, y);
}
Result<void> Renderer2D::resize(const std::uint32_t width, const std::uint32_t height) {
    return impl_->resize(width, height);
}
Result<void> Renderer2D::set_fps_cap(const RenderFpsCap cap) { return impl_->set_fps_cap(cap); }
Result<void> Renderer2D::wait_idle() { return impl_->wait_idle(); }
const RendererCapabilities& Renderer2D::capabilities() const noexcept { return impl_->capabilities; }
const std::vector<Diagnostic>& Renderer2D::validation_diagnostics() const noexcept {
    return impl_->validation_messages;
}
bool Renderer2D::initialized() const noexcept { return impl_->ready; }

} // namespace ai2d
