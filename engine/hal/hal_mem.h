#pragma once
// HAL: memory for large application data that the audio thread never touches (stored patterns ...). Implemented once per platform.
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// `bytes` of zeroed memory that is never given back: the slow, large memory when the platform has one (PSRAM on the board; the
// internal heap when that has no room left), the heap on the desktop. NULL when there is none: the caller does without.
void *mem_bulk_alloc(size_t bytes);

#ifdef __cplusplus
}
#endif
