#include "ai2d/foundation/allocation_tracker.hpp"

#include <cstdlib>
#include <new>

#if defined(_MSC_VER)
#include <malloc.h>
#endif

#if defined(AI2D_ENABLE_ALLOCATION_TRACKING)

namespace {

[[nodiscard]] void* allocate_unaligned(const std::size_t bytes) {
    const auto actual_bytes = bytes == 0U ? 1U : bytes;
    void* const memory = std::malloc(actual_bytes);
    if (memory == nullptr) {
        throw std::bad_alloc{};
    }
    ai2d::record_cpp_allocation(bytes);
    return memory;
}

[[nodiscard]] void* allocate_aligned(const std::size_t bytes, const std::size_t alignment) {
    const auto actual_bytes = bytes == 0U ? 1U : bytes;
#if defined(_MSC_VER)
    void* const memory = _aligned_malloc(actual_bytes, alignment);
#else
    void* const memory = std::aligned_alloc(alignment, ((actual_bytes + alignment - 1U) / alignment) * alignment);
#endif
    if (memory == nullptr) {
        throw std::bad_alloc{};
    }
    ai2d::record_cpp_allocation(bytes);
    return memory;
}

void free_aligned(void* const memory) noexcept {
#if defined(_MSC_VER)
    _aligned_free(memory);
#else
    std::free(memory);
#endif
}

} // namespace

void* operator new(const std::size_t bytes) { return allocate_unaligned(bytes); }
void* operator new[](const std::size_t bytes) { return allocate_unaligned(bytes); }
void operator delete(void* const memory) noexcept { std::free(memory); }
void operator delete[](void* const memory) noexcept { std::free(memory); }
void operator delete(void* const memory, const std::size_t bytes) noexcept { (void)bytes; std::free(memory); }
void operator delete[](void* const memory, const std::size_t bytes) noexcept { (void)bytes; std::free(memory); }

void* operator new(const std::size_t bytes, const std::nothrow_t&) noexcept {
    try { return allocate_unaligned(bytes); } catch (...) { return nullptr; }
}
void* operator new[](const std::size_t bytes, const std::nothrow_t&) noexcept {
    try { return allocate_unaligned(bytes); } catch (...) { return nullptr; }
}
void operator delete(void* const memory, const std::nothrow_t&) noexcept { std::free(memory); }
void operator delete[](void* const memory, const std::nothrow_t&) noexcept { std::free(memory); }

void* operator new(const std::size_t bytes, const std::align_val_t alignment) {
    return allocate_aligned(bytes, static_cast<std::size_t>(alignment));
}
void* operator new[](const std::size_t bytes, const std::align_val_t alignment) {
    return allocate_aligned(bytes, static_cast<std::size_t>(alignment));
}
void operator delete(void* const memory, const std::align_val_t alignment) noexcept { (void)alignment; free_aligned(memory); }
void operator delete[](void* const memory, const std::align_val_t alignment) noexcept { (void)alignment; free_aligned(memory); }
void operator delete(void* const memory, const std::size_t bytes, const std::align_val_t alignment) noexcept {
    (void)bytes; (void)alignment; free_aligned(memory);
}
void operator delete[](void* const memory, const std::size_t bytes, const std::align_val_t alignment) noexcept {
    (void)bytes; (void)alignment; free_aligned(memory);
}

void* operator new(const std::size_t bytes, const std::align_val_t alignment, const std::nothrow_t&) noexcept {
    try { return allocate_aligned(bytes, static_cast<std::size_t>(alignment)); } catch (...) { return nullptr; }
}
void* operator new[](const std::size_t bytes, const std::align_val_t alignment, const std::nothrow_t&) noexcept {
    try { return allocate_aligned(bytes, static_cast<std::size_t>(alignment)); } catch (...) { return nullptr; }
}
void operator delete(void* const memory, const std::align_val_t alignment, const std::nothrow_t&) noexcept {
    (void)alignment; free_aligned(memory);
}
void operator delete[](void* const memory, const std::align_val_t alignment, const std::nothrow_t&) noexcept {
    (void)alignment; free_aligned(memory);
}

#endif
