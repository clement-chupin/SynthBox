#pragma once
// Block (vector) kernels on q15 buffers. Portable reference implementations: the loops are written so
// that a platform layer can later replace them with SIMD (ESP32-S3 PIE / esp-dsp) behind the same names.
// All buffers are kBlock frames unless `n` is given. dst may alias a source (in-place is allowed).
#include "engine/dsp/config.h"
#include "engine/dsp/q.h"

namespace sc {

inline void block_clear(q15 *dst, int n = kBlock) { for (int i = 0; i < n; i++) dst[i] = 0; }
inline void block_copy(q15 *dst, const q15 *src, int n = kBlock) { for (int i = 0; i < n; i++) dst[i] = src[i]; }

inline void block_add(q15 *dst, const q15 *a, const q15 *b, int n = kBlock) { for (int i = 0; i < n; i++) dst[i] = add15(a[i], b[i]); }
inline void block_mul(q15 *dst, const q15 *a, const q15 *b, int n = kBlock) { for (int i = 0; i < n; i++) dst[i] = mul15(a[i], b[i]); }

// dst += src (mixing bus accumulate)
inline void block_acc(q15 *dst, const q15 *src, int n = kBlock) { for (int i = 0; i < n; i++) dst[i] = add15(dst[i], src[i]); }

// dst = src * g (constant gain, q15)
inline void block_gain(q15 *dst, const q15 *src, q15 g, int n = kBlock) { for (int i = 0; i < n; i++) dst[i] = mul15(src[i], g); }

// dst += src * g
inline void block_mac(q15 *dst, const q15 *src, q15 g, int n = kBlock) { for (int i = 0; i < n; i++) dst[i] = add15(dst[i], mul15(src[i], g)); }

// Peak absolute value (metering, tests).
inline q15 block_peak(const q15 *src, int n = kBlock) {
    int32_t p = 0;
    for (int i = 0; i < n; i++) { int32_t v = src[i] < 0 ? -static_cast<int32_t>(src[i]) : src[i]; if (v > p) p = v; }
    return sat16(p);
}

}  // namespace sc
