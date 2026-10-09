#pragma once
#include <cstdlib>
constexpr int MALLOC_CAP_SPIRAM = 1, MALLOC_CAP_8BIT = 2;
inline void *heap_caps_aligned_alloc(size_t, size_t bytes, int) { return malloc(bytes); }
inline void *heap_caps_malloc(size_t bytes, int) { return malloc(bytes); }
inline void heap_caps_free(void *p) { free(p); }
