#include "render_benchmark.hpp"

#include "ai2d/foundation/build_info.hpp"
#include "ai2d/foundation/diagnostic.hpp"
#include "ai2d/foundation/json_writer.hpp"

#include <cstdint>
#include <iostream>

int ai2d_render_benchmark_main(
    const std::span<const std::size_t> counts,
    const std::size_t runs,
    const std::size_t warmup_frames,
    const std::size_t measurement_frames,
    const bool json_mode) {
    (void)counts;
    (void)runs;
    (void)warmup_frames;
    (void)measurement_frames;
    auto diagnostic = ai2d::Diagnostic::make(
        ai2d::DiagnosticCode::command_unavailable,
        ai2d::Severity::error,
        "renderer2d",
        "The render benchmark is unavailable because GPU support was disabled at configure time");
    if (!json_mode) {
        std::cerr << diagnostic.message << '\n';
        return 3;
    }

    const auto build = ai2d::current_build_info();
    ai2d::JsonWriter writer{};
    writer.begin_object();
    writer.key("schema_version");
    writer.value(std::uint64_t{1U});
    writer.key("command");
    writer.value("benchmark");
    writer.key("status");
    writer.value("unavailable");
    writer.key("build");
    writer.begin_object();
    writer.key("version");
    writer.value(build.version);
    writer.key("compiler");
    writer.value(build.compiler);
    writer.key("configuration");
    writer.value(build.configuration);
    writer.end_object();
    writer.key("environment");
    writer.begin_object();
    writer.end_object();
    writer.key("diagnostics");
    writer.begin_array();
    ai2d::write_json(writer, diagnostic);
    writer.end_array();
    writer.key("metrics");
    writer.begin_object();
    writer.key("suite");
    writer.value("render");
    writer.key("results");
    writer.begin_array();
    writer.end_array();
    writer.end_object();
    writer.key("artifacts");
    writer.begin_array();
    writer.end_array();
    writer.end_object();
    std::cout << writer.str() << '\n';
    return 3;
}
