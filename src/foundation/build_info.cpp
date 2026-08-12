#include "ai2d/foundation/build_info.hpp"

#define AI2D_STRINGIFY_IMPL(value) #value
#define AI2D_STRINGIFY(value) AI2D_STRINGIFY_IMPL(value)

namespace ai2d {

BuildInfo current_build_info() {
    BuildInfo info{};
    info.version = AI2D_VERSION;
#if defined(_MSC_VER)
    info.compiler = "MSVC " AI2D_STRINGIFY(_MSC_FULL_VER);
#elif defined(__clang__)
    info.compiler = "Clang " __clang_version__;
#elif defined(__GNUC__)
    info.compiler = "GCC " __VERSION__;
#else
    info.compiler = "unknown";
#endif

#if defined(NDEBUG)
    info.configuration = "release-like";
#else
    info.configuration = "debug";
#endif

#if defined(_WIN32)
    info.operating_system = "Windows";
#elif defined(__linux__)
    info.operating_system = "Linux";
#elif defined(__APPLE__)
    info.operating_system = "macOS";
#else
    info.operating_system = "unknown";
#endif

#if defined(_M_X64) || defined(__x86_64__)
    info.architecture = "x86_64";
#elif defined(_M_ARM64) || defined(__aarch64__)
    info.architecture = "arm64";
#else
    info.architecture = "unknown";
#endif
    return info;
}

} // namespace ai2d
