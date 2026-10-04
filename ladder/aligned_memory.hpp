#pragma once
// C++17 aligned new/delete, including MSVC (which lacks std::aligned_alloc).
#include <cstddef>
#include <new>
namespace ladder {
inline void* allocate_aligned(std::size_t bytes) {
    return ::operator new(bytes ? bytes : 64, std::align_val_t{64});
}
inline void free_aligned(void* p) noexcept {
    ::operator delete(p, std::align_val_t{64});
}
} // namespace ladder
