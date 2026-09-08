#include "ai2d/foundation/allocation_tracker.hpp"
#include "ai2d/foundation/build_info.hpp"
#include "ai2d/foundation/diagnostic.hpp"
#include "ai2d/foundation/json_writer.hpp"

#if defined(AI2D_ENABLE_GPU)
#include "ai2d/platform/window.hpp"
#include "ai2d/renderer2d/renderer.hpp"
#endif

#include <charconv>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

#if defined(AI2D_ENABLE_GPU)
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
#endif

struct Arguments final {
    std::uint32_t frames{8U};
    bool json{false};
    bool hidden{false};
    bool offscreen{false};
    bool validation{false};
    bool synchronization_validation{false};
    bool resize_smoke{false};
    bool sprite_smoke{false};
    bool invalid_texture_smoke{false};
    bool fault_recovery_smoke{false};
    bool init_retry_smoke{false};
    bool resize_failure_smoke{false};
};

bool parse_arguments(const int count, const char* const* values, Arguments& arguments) {
    for (int index = 1; index < count; ++index) {
        const std::string_view option{values[index]};
        if (option == "--json") {
            arguments.json = true;
        } else if (option == "--hidden") {
            arguments.hidden = true;
        } else if (option == "--offscreen") {
            arguments.offscreen = true;
        } else if (option == "--validation") {
            arguments.validation = true;
        } else if (option == "--sync-validation") {
            arguments.validation = true;
            arguments.synchronization_validation = true;
        } else if (option == "--resize-smoke") {
            arguments.resize_smoke = true;
        } else if (option == "--sprite-smoke") {
            arguments.sprite_smoke = true;
        } else if (option == "--invalid-texture-smoke") {
            arguments.sprite_smoke = true;
            arguments.invalid_texture_smoke = true;
        } else if (option == "--fault-recovery-smoke") {
            arguments.fault_recovery_smoke = true;
        } else if (option == "--init-retry-smoke") {
            arguments.init_retry_smoke = true;
        } else if (option == "--resize-failure-smoke") {
            arguments.resize_failure_smoke = true;
        } else if (option == "--frames") {
            if (++index >= count) {
                return false;
            }
            const std::string_view value{values[index]};
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), arguments.frames);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || arguments.frames == 0U) {
                return false;
            }
        } else {
            return false;
        }
    }
    return true;
}

void write_build(ai2d::JsonWriter& writer) {
    const auto build = ai2d::current_build_info();
    writer.key("build");
    writer.begin_object();
    writer.key("version");
    writer.value(build.version);
    writer.key("compiler");
    writer.value(build.compiler);
    writer.key("configuration");
    writer.value(build.configuration);
    writer.key("os");
    writer.value(build.operating_system);
    writer.key("architecture");
    writer.value(build.architecture);
    writer.end_object();
}

int emit_failure(const Arguments& arguments, ai2d::Diagnostic diagnostic, const int exit_code) {
    if (!arguments.json) {
        std::cerr << ai2d::to_string(diagnostic.code) << ": " << diagnostic.message << '\n';
        return exit_code;
    }
    ai2d::JsonWriter writer{};
    writer.begin_object();
    writer.key("schema_version");
    writer.value(std::uint64_t{1U});
    writer.key("command");
    writer.value("sandbox");
    writer.key("status");
    writer.value(exit_code == 3 ? "unavailable" : "fail");
    write_build(writer);
    writer.key("environment");
    writer.begin_object();
    writer.end_object();
    writer.key("diagnostics");
    writer.begin_array();
    ai2d::write_json(writer, diagnostic);
    writer.end_array();
    writer.key("metrics");
    writer.begin_object();
    writer.end_object();
    writer.key("artifacts");
    writer.begin_array();
    writer.end_array();
    writer.end_object();
    std::cout << writer.str() << '\n';
    return exit_code;
}

#if defined(AI2D_ENABLE_GPU)
int emit_recovery_pass(const Arguments& arguments, const std::string_view recovery) {
    if (!arguments.json) {
        std::cout << recovery << ": PASS\n";
        return 0;
    }
    ai2d::JsonWriter writer{};
    writer.begin_object();
    writer.key("schema_version");
    writer.value(std::uint64_t{1U});
    writer.key("command");
    writer.value("sandbox");
    writer.key("status");
    writer.value("pass");
    write_build(writer);
    writer.key("environment");
    writer.begin_object();
    writer.end_object();
    writer.key("diagnostics");
    writer.begin_array();
    writer.end_array();
    writer.key("metrics");
    writer.begin_object();
    writer.key("recovery");
    writer.value(recovery);
    writer.end_object();
    writer.key("artifacts");
    writer.begin_array();
    writer.end_array();
    writer.end_object();
    std::cout << writer.str() << '\n';
    return 0;
}

void write_capabilities(ai2d::JsonWriter& writer, const ai2d::RendererCapabilities& capabilities) {
    writer.begin_object();
    writer.key("device_name");
    writer.value(capabilities.device_name);
    writer.key("device_type");
    writer.value(capabilities.device_type);
    writer.key("driver_name");
    writer.value(capabilities.driver_name);
    writer.key("driver_info");
    writer.value(capabilities.driver_info);
    writer.key("vendor_id");
    writer.value(static_cast<std::uint64_t>(capabilities.vendor_id));
    writer.key("device_id");
    writer.value(static_cast<std::uint64_t>(capabilities.device_id));
    writer.key("api_version");
    writer.value(static_cast<std::uint64_t>(capabilities.api_version));
    writer.key("driver_version");
    writer.value(static_cast<std::uint64_t>(capabilities.driver_version));
    writer.key("graphics_queue_family");
    writer.value(static_cast<std::uint64_t>(capabilities.graphics_queue_family));
    writer.key("dynamic_rendering");
    writer.value(capabilities.dynamic_rendering);
    writer.key("synchronization2");
    writer.value(capabilities.synchronization2);
    writer.key("shader_draw_parameters");
    writer.value(capabilities.shader_draw_parameters);
    writer.key("present_supported");
    writer.value(capabilities.present_supported);
    writer.key("timestamps_supported");
    writer.value(capabilities.timestamps_supported);
    writer.key("timestamp_period_nanoseconds");
    writer.value(static_cast<double>(capabilities.timestamp_period_nanoseconds));
    writer.key("validation_requested");
    writer.value(capabilities.validation_requested);
    writer.key("validation_enabled");
    writer.value(capabilities.validation_enabled);
    writer.key("synchronization_validation_enabled");
    writer.value(capabilities.synchronization_validation_enabled);
    writer.end_object();
}

int gpu_main(const Arguments& arguments) {
    ai2d::PlatformWindow window{};
    const auto opened = window.open({"ai2d Vulkan 1.3 sandbox", 640U, 360U, arguments.hidden, true});
    if (!opened) {
        return emit_failure(arguments, opened.error(), 3);
    }

    ai2d::Renderer2D renderer{};
    ai2d::RendererOptions options{};
    options.native_window = window.native_handle();
    options.width = 640U;
    options.height = 360U;
    options.enable_validation = arguments.validation;
    options.enable_synchronization_validation = arguments.synchronization_validation;
    options.require_present = !arguments.offscreen;
    auto initialized = renderer.initialize(options);
    if (arguments.init_retry_smoke) {
        if (initialized || renderer.initialized()) {
            return emit_failure(
                arguments,
                ai2d::Diagnostic::make(
                    ai2d::DiagnosticCode::internal_error,
                    ai2d::Severity::error,
                    "sandbox",
                    "Injected renderer initialization failure did not roll back"),
                1);
        }
        initialized = renderer.initialize(options);
    }
    if (!initialized) {
        const auto code = initialized.error().code == ai2d::DiagnosticCode::vk_device_unsupported ||
                                  initialized.error().code == ai2d::DiagnosticCode::tool_missing
                              ? 3
                              : 1;
        return emit_failure(arguments, initialized.error(), code);
    }

    if (arguments.resize_failure_smoke) {
        auto resized = renderer.resize(800U, 450U);
        auto retried = renderer.clear({});
        if (resized || renderer.initialized() || retried) {
            return emit_failure(
                arguments,
                ai2d::Diagnostic::make(
                    ai2d::DiagnosticCode::internal_error,
                    ai2d::Severity::error,
                    "sandbox",
                    "Injected resize failure did not leave a deterministic terminal renderer"),
                1);
        }
        return emit_recovery_pass(arguments, "resize_failure_terminal_state");
    }

    if (arguments.fault_recovery_smoke) {
        const std::string fault = environment_value("AI2D_VK_TEST_FAULT");
        auto first = renderer.clear({});
        if (first) {
            return emit_failure(
                arguments,
                ai2d::Diagnostic::make(
                    ai2d::DiagnosticCode::internal_error,
                    ai2d::Severity::error,
                    "sandbox",
                    "Injected frame submission fault was not observed"),
                1);
        }
        auto second = renderer.clear({});
        const bool terminal_submission = fault == "submit_once";
        const bool terminal_presented_recording = options.require_present &&
                                                  (fault == "reset_command_pool_once" ||
                                                   fault == "begin_command_once" || fault == "end_command_once");
        const bool terminal = terminal_submission || terminal_presented_recording;
        const bool recovered = terminal
                                   ? !renderer.initialized() && !second
                                   : renderer.initialized() && second.has_value();
        if (!recovered) {
            return emit_failure(
                arguments,
                ai2d::Diagnostic::make(
                    ai2d::DiagnosticCode::internal_error,
                    ai2d::Severity::error,
                    "sandbox",
                    "Frame submission fault recovery state was inconsistent"),
                1);
        }
        if (second) {
            if (auto idle = renderer.wait_idle(); !idle) {
                return emit_failure(arguments, idle.error(), 1);
            }
        }
        return emit_recovery_pass(
            arguments,
            terminal ? "submission_or_presented_recording_failure_terminal_state" : "command_failure_recovered");
    }

    ai2d::TextureHandle sprite_texture = ai2d::TextureHandle::invalid();
    std::array<ai2d::SpriteSubmission2D, 1U> sprite_submissions{};
    if (arguments.sprite_smoke) {
        auto texture = renderer.create_checker_texture(
            2U, 2U, {1.0F, 0.0F, 0.0F, 1.0F}, {1.0F, 0.0F, 0.0F, 1.0F}, 1U);
        if (!texture) {
            return emit_failure(arguments, texture.error(), 1);
        }
        sprite_texture = texture.value();
        sprite_submissions[0].position = {-1.0F, -1.0F};
        sprite_submissions[0].size = {2.0F, 2.0F};
        sprite_submissions[0].pivot = {0.0F, 0.0F};
        sprite_submissions[0].texture = sprite_texture;
        sprite_submissions[0].tint = {1.0F, 1.0F, 1.0F, 1.0F};
    }
    bool invalid_texture_rejected = false;
    if (arguments.invalid_texture_smoke) {
        auto destroyed = renderer.destroy_texture(sprite_texture);
        if (!destroyed) {
            return emit_failure(arguments, destroyed.error(), 1);
        }
        const ai2d::RenderFrame2D invalid_frame{
            {{0.0F, 0.0F}, {4.0F, 2.25F}}, sprite_submissions, {0.0F, 0.0F, 0.0F, 1.0F}};
        auto rejected = renderer.render(invalid_frame);
        invalid_texture_rejected = !rejected &&
                                   rejected.error().code == ai2d::DiagnosticCode::render_invalid_texture_handle;
        if (!invalid_texture_rejected) {
            auto diagnostic = ai2d::Diagnostic::make(
                ai2d::DiagnosticCode::internal_error,
                ai2d::Severity::error,
                "sandbox",
                "Stale texture handle did not produce RENDER_INVALID_TEXTURE_HANDLE");
            return emit_failure(arguments, std::move(diagnostic), 1);
        }
        auto replacement = renderer.create_checker_texture(
            2U, 2U, {1.0F, 0.0F, 0.0F, 1.0F}, {1.0F, 0.0F, 0.0F, 1.0F}, 1U);
        if (!replacement) {
            return emit_failure(arguments, replacement.error(), 1);
        }
        sprite_texture = replacement.value();
        sprite_submissions[0].texture = sprite_texture;
    }

    std::vector<ai2d::GpuFrameMetrics> frame_metrics{};
    frame_metrics.reserve(arguments.frames);
    ai2d::RendererMetrics last_renderer_metrics{};
    ai2d::AllocationSnapshot measured_allocations{};
    std::uint64_t measured_frames = 0U;
    for (std::uint32_t frame = 0U; frame < arguments.frames; ++frame) {
        auto input = window.poll_input();
        if (!input) {
            return emit_failure(arguments, input.error(), 1);
        }
        if (input->quit_requested || input->down(ai2d::InputKey::escape)) {
            break;
        }
        if (input->resized && input->drawable_extent.width > 0U && input->drawable_extent.height > 0U) {
            auto resized = renderer.resize(input->drawable_extent.width, input->drawable_extent.height);
            if (!resized) {
                return emit_failure(arguments, resized.error(), 1);
            }
        }
        if (arguments.resize_smoke && frame == arguments.frames / 2U) {
            auto window_resize = window.set_extent(800U, 450U);
            if (!window_resize) {
                return emit_failure(arguments, window_resize.error(), 1);
            }
            auto renderer_resize = renderer.resize(800U, 450U);
            if (!renderer_resize) {
                return emit_failure(arguments, renderer_resize.error(), 1);
            }
        }
        const float phase = static_cast<float>(frame % 8U) / 8.0F;
        if (arguments.sprite_smoke) {
            const ai2d::RenderFrame2D render_frame{
                {{0.0F, 0.0F}, {4.0F, 2.25F}},
                sprite_submissions,
                {0.0F, 0.0F, 0.0F, 1.0F},
            };
            if (frame >= 2U) {
                ai2d::MeasuredAllocationScope measured{};
                auto rendered = renderer.render(render_frame);
                const auto allocation_snapshot = measured.finish();
                measured_allocations.allocations += allocation_snapshot.allocations;
                measured_allocations.bytes += allocation_snapshot.bytes;
                ++measured_frames;
                if (!rendered) {
                    return emit_failure(arguments, rendered.error(), 1);
                }
                last_renderer_metrics = rendered.value();
                frame_metrics.push_back(rendered->gpu);
            } else {
                auto rendered = renderer.render(render_frame);
                if (!rendered) {
                    return emit_failure(arguments, rendered.error(), 1);
                }
                last_renderer_metrics = rendered.value();
                frame_metrics.push_back(rendered->gpu);
            }
        } else {
            auto rendered = renderer.clear({0.04F + phase * 0.1F, 0.08F, 0.16F, 1.0F});
            if (!rendered) {
                return emit_failure(arguments, rendered.error(), 1);
            }
            frame_metrics.push_back(rendered.value());
        }
    }
    auto idle = renderer.wait_idle();
    if (!idle) {
        return emit_failure(arguments, idle.error(), 1);
    }

    ai2d::Color sampled_pixel{};
    bool pixel_checked = false;
    bool pixel_correct = true;
    if (arguments.sprite_smoke && arguments.offscreen) {
        // World-space (0.5, 0.5) lies inside a [-1,-1] sprite only when
        // the authored [0,0] pivot reaches the shader.
        auto pixel = renderer.read_offscreen_pixel(360U, 220U);
        if (!pixel) {
            return emit_failure(arguments, pixel.error(), 1);
        }
        sampled_pixel = pixel.value();
        pixel_checked = true;
        pixel_correct = sampled_pixel.r > 0.90F && sampled_pixel.g < 0.10F &&
                        sampled_pixel.b < 0.10F && sampled_pixel.a > 0.90F;
    }

    const auto& validation = renderer.validation_diagnostics();
    const bool validation_clean = validation.empty();
    const bool allocation_clean = !arguments.sprite_smoke || measured_frames == 0U ||
                                  measured_allocations.allocations == 0U;
    const bool all_pass = validation_clean && pixel_correct && allocation_clean &&
                          (!arguments.invalid_texture_smoke || invalid_texture_rejected);
    if (!arguments.json) {
        std::cout << "Vulkan sandbox: " << (all_pass ? "PASS" : "FAIL") << " ("
                  << frame_metrics.size() << " frames on " << renderer.capabilities().device_name << ")\n";
        return all_pass ? 0 : 1;
    }

    ai2d::JsonWriter writer{};
    writer.begin_object();
    writer.key("schema_version");
    writer.value(std::uint64_t{1U});
    writer.key("command");
    writer.value("sandbox");
    writer.key("status");
    writer.value(all_pass ? "pass" : "fail");
    write_build(writer);
    writer.key("environment");
    writer.begin_object();
    writer.key("renderer");
    writer.value("Vulkan 1.3");
    writer.end_object();
    writer.key("diagnostics");
    writer.begin_array();
    for (const auto& diagnostic : validation) {
        ai2d::write_json(writer, diagnostic);
    }
    writer.end_array();
    writer.key("metrics");
    writer.begin_object();
    writer.key("target");
    writer.value(arguments.offscreen ? "offscreen" : "presented");
    writer.key("requested_frames");
    writer.value(static_cast<std::uint64_t>(arguments.frames));
    writer.key("completed_frames");
    writer.value(static_cast<std::uint64_t>(frame_metrics.size()));
    writer.key("resize_smoke");
    writer.value(arguments.resize_smoke);
    writer.key("sprite_smoke");
    writer.value(arguments.sprite_smoke);
    writer.key("invalid_texture_smoke");
    writer.value(arguments.invalid_texture_smoke);
    writer.key("capabilities");
    write_capabilities(writer, renderer.capabilities());
    writer.key("last_frame");
    writer.begin_object();
    if (!frame_metrics.empty()) {
        const auto& last = frame_metrics.back();
        writer.key("frame_index");
        writer.value(last.frame_index);
        writer.key("cpu_submit_ms");
        writer.value(last.cpu_submit_ms);
        writer.key("gpu_pass_ms");
        writer.value(last.gpu_pass_ms);
        writer.key("gpu_timing_available");
        writer.value(last.gpu_timing_available);
        writer.key("presented");
        writer.value(last.presented);
        writer.key("width");
        writer.value(static_cast<std::uint64_t>(last.width));
        writer.key("height");
        writer.value(static_cast<std::uint64_t>(last.height));
    }
    writer.end_object();
    writer.key("renderer2d");
    writer.begin_object();
    writer.key("visited_sprites");
    writer.value(last_renderer_metrics.queue.visited);
    writer.key("visible_sprites");
    writer.value(last_renderer_metrics.queue.visible);
    writer.key("culled_sprites");
    writer.value(last_renderer_metrics.queue.culled);
    writer.key("batches");
    writer.value(last_renderer_metrics.queue.batches);
    writer.key("sprite_draw_calls");
    writer.value(last_renderer_metrics.sprite_draw_calls);
    writer.key("texture_binds");
    writer.value(last_renderer_metrics.texture_binds);
    writer.key("instance_upload_bytes");
    writer.value(last_renderer_metrics.instance_upload_bytes);
    writer.key("capacity_growth_events");
    writer.value(last_renderer_metrics.capacity_growth_events);
    writer.key("measured_frames");
    writer.value(measured_frames);
    writer.key("tracked_cpp_heap_allocations");
    writer.value(measured_allocations.allocations);
    writer.key("tracked_cpp_heap_bytes");
    writer.value(measured_allocations.bytes);
    writer.key("frame_arena_overflows");
    writer.null_value();
    writer.key("frame_arena_overflows_unavailable_reason");
    writer.value("The sandbox does not use or instrument a FrameArena");
    writer.key("invalid_texture_rejected");
    writer.value(invalid_texture_rejected);
    writer.key("pixel_checked");
    writer.value(pixel_checked);
    writer.key("pixel_correct");
    writer.value(pixel_correct);
    writer.key("sampled_pixel");
    writer.begin_object();
    writer.key("r");
    writer.value(static_cast<double>(sampled_pixel.r));
    writer.key("g");
    writer.value(static_cast<double>(sampled_pixel.g));
    writer.key("b");
    writer.value(static_cast<double>(sampled_pixel.b));
    writer.key("a");
    writer.value(static_cast<double>(sampled_pixel.a));
    writer.end_object();
    writer.end_object();
    writer.end_object();
    writer.key("artifacts");
    writer.begin_array();
    writer.end_array();
    writer.end_object();
    std::cout << writer.str() << '\n';
    return all_pass ? 0 : 1;
}
#endif

} // namespace

int main(const int argument_count, const char* const* const arguments) {
    Arguments parsed{};
    if (!parse_arguments(argument_count, arguments, parsed)) {
        auto diagnostic = ai2d::Diagnostic::make(
            ai2d::DiagnosticCode::input_invalid,
            ai2d::Severity::error,
            "sandbox",
            "Invalid sandbox arguments");
        return emit_failure(parsed, std::move(diagnostic), 2);
    }
#if defined(AI2D_ENABLE_GPU)
    return gpu_main(parsed);
#else
    auto diagnostic = ai2d::Diagnostic::make(
        ai2d::DiagnosticCode::command_unavailable,
        ai2d::Severity::error,
        "sandbox",
        "GPU support was disabled at configure time");
    return emit_failure(parsed, std::move(diagnostic), 3);
#endif
}
