#pragma once
#include <cstdlib>
#include <cstddef>

#define MALLOC_CAP_SPIRAM 0x01
#define MALLOC_CAP_8BIT 0x02

inline void* heap_caps_malloc(std::size_t size, int) {
    return std::malloc(size);
}

inline void heap_caps_free(void* pointer) {
    std::free(pointer);
}
