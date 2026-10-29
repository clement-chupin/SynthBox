#pragma once
// Vectoring CORDIC: magnitude and phase of a complex bin with shifts and adds only (no multiply, no divide,
// no atan2): about 6 cycles per iteration. Phase is a uint32 turn fraction (2^32 = 2 pi), the same unit as the
// oscillator phase, so it feeds sine() directly and phase differences wrap for free.
#include <cstdint>
#include "engine/dsp/fft.h"
#include "engine/dsp/phase.h"
#include "engine/dsp/q.h"
#include "engine/dsp/tables.h"

namespace sc {

constexpr int kCordicUse = 16;                       // iterations: phase error about 2^-15 rad

// x, y: int16-range components. mag has the same scale as x and y.
inline void cordic_polar(int32_t x, int32_t y, uint32_t &phase, uint32_t &mag) {
    uint32_t base = 0;
    if (x < 0) { x = -x; y = -y; base = 0x80000000u; }          // half-turn so that x >= 0
    int32_t xi = x * 4096, yi = y * 4096;                        // 12 fractional bits survive the shifts
    uint32_t ang = 0;
    for (int i = 0; i < kCordicUse; i++) {
        const int32_t dx = yi >> i, dy = xi >> i;
        if (yi >= 0) { xi += dx; yi -= dy; ang += kCordicAtan[i]; }
        else         { xi -= dx; yi += dy; ang -= kCordicAtan[i]; }
    }
    phase = base + ang;
    mag = static_cast<uint32_t>((((static_cast<uint64_t>(static_cast<uint32_t>(xi)) * 2608131496ull) >> 32) + 2048) >> 12);   // x 0.6072529 (1/K), undo prescale, rounded
}

#if SC_FFT_PIE
struct PieCordicArgs { const int16_t *re, *im; int32_t *xo, *ao; const uint32_t *atan; int n, iters; };
extern "C" void pie_cordic(const PieCordicArgs *a);       // fft_s3.S
#endif

// cordic_polar of bins 0..n-1 (n <= 260): fn(k, phase, mag) for each, the same values. On the S3 the iterations run on PIE,
// 4 bins at a time (re / im 16-byte aligned, as the Stft frame is).
template <typename Fn>
inline void cordic_polar_bins(const int16_t *re, const int16_t *im, int n, Fn &&fn) {
#if SC_FFT_PIE
    alignas(16) static int32_t xo[260], ao[260];             // the audio task only
    const PieCordicArgs a{re, im, xo, ao, kCordicAtan, (n + 3) & ~3, kCordicUse};
    pie_cordic(&a);
    for (int k = 0; k < n; k++) {
        const uint32_t phase = (re[k] < 0 ? 0x80000000u : 0u) + static_cast<uint32_t>(ao[k]);
        const uint32_t mag = static_cast<uint32_t>((((static_cast<uint64_t>(static_cast<uint32_t>(xo[k])) * 2608131496ull) >> 32) + 2048) >> 12);
        fn(k, phase, mag);
    }
#else
    for (int k = 0; k < n; k++) { uint32_t phase, mag; cordic_polar(re[k], im[k], phase, mag); fn(k, phase, mag); }
#endif
}

// Inverse: mag * (cos phase, sin phase), saturated to int16.
inline void polar_to_rect(uint32_t mag, uint32_t phase, int16_t &re, int16_t &im) {
    const int64_t m = static_cast<int64_t>(mag);
    re = sat16(static_cast<int32_t>((m * sine(phase + 0x40000000u) + (1 << 14)) >> 15));
    im = sat16(static_cast<int32_t>((m * sine(phase) + (1 << 14)) >> 15));
}

}  // namespace sc
