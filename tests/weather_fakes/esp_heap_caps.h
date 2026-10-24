#pragma once
#include <stdlib.h>
constexpr int MALLOC_CAP_SPIRAM = 1;
inline void *heap_caps_malloc(size_t size, int) { return malloc(size); }
inline void *heap_caps_calloc(size_t count, size_t size, int) { return calloc(count, size); }
