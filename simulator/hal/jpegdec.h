#pragma once
// Simulator / android-native image decode shim — backs the cross-target
// imgDecodeGray() used by loadOrConvertBvid() (src/main.cpp) to auto-convert
// photos to .bvid. Desktop/mobile side, so no memory-bounded scratch decoding
// is needed: just decode straight to grayscale via stb_image (vendored, public
// domain, single header, alongside the other HAL shims in this dir) for both
// JPEG and PNG — unlike the ESP32 side (include/jpegdec.h), there's no PNG
// pixel-count safety cap here since desktop/phone RAM dwarfs a typical photo's
// decoded size; the two headers are never compiled together.

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

// Decodes JPEG or PNG bytes to 8-bit grayscale (row-major, 1 byte/pixel). On
// success, *outGray is a buffer the caller must free() (plain free() works —
// stb_image uses malloc internally); dimensions come back via *outW/*outH.
// Returns false on failure (nothing to free).
static inline bool imgDecodeGray(const uint8_t* buf, size_t len, uint8_t** outGray, int* outW, int* outH) {
    int w = 0, h = 0, channels = 0;
    uint8_t* gray = stbi_load_from_memory(buf, (int)len, &w, &h, &channels, 1);
    if (!gray || w <= 0 || h <= 0) { if (gray) stbi_image_free(gray); return false; }
    *outGray = gray;
    *outW    = w;
    *outH    = h;
    return true;
}
