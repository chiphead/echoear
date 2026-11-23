#pragma once
#include <cstdlib>

inline void heap_caps_free(void* pointer) {
    std::free(pointer);
}
