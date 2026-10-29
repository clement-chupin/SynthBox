#pragma once
// Phase, pitch and exponential mapping (ADR-006).
//   phase      : uint32, 2^32 = one cycle, wraps naturally
//   pitch      : int32 in 1/256 semitone, MIDI note n = n * 256 (A4 = 69 * 256)
//   exp domain : int32 in Q16 octaves (65536 = one octave), the common "log" parameter space
#include <cstdint>
#include <cmath>
#include "engine/dsp/config.h"
#include "engine/dsp/q.h"
#include "engine/dsp/tables.h"

namespace sc {

constexpr int32_t kPitchUnit = 256;                 // 1 semitone
constexpr int32_t kPitchA4   = 69 * kPitchUnit;

// Phase increment for a frequency (control rate / init: uses float).
inline uint32_t hz_to_inc(float hz) {
    double inc = static_cast<double>(hz) / kSampleRate * 4294967296.0;
    return inc <= 0 ? 0u : (inc >= 2147483648.0 ? 2147483648u : static_cast<uint32_t>(inc));
}
inline float inc_to_hz(uint32_t inc) { return static_cast<float>(static_cast<double>(inc) * kSampleRate / 4294967296.0); }

// A4 (440 Hz) increment: the base for pitch_to_inc.
inline uint32_t inc_a4() { return hz_to_inc(440.0f); }

// base * 2^(x / 65536), x in Q16 octaves. Saturates at 2^32-1. 256-entry table + linear interpolation
// (error below 0.1 cent). The shared primitive for pitch, cutoff, envelope times and LFO rates. [CONTROL/AUDIO]
inline uint32_t exp2_scale(uint32_t base, int32_t x) {
    int32_t oct = x >> 16;                                         // floor
    uint32_t frac = static_cast<uint32_t>(x) & 0xFFFFu;
    uint32_t idx = frac >> 8, r = frac & 0xFFu;
    uint32_t a = kExp2Tab[idx], b = kExp2Tab[idx + 1];
    uint32_t mant = a + static_cast<uint32_t>((static_cast<uint64_t>(b - a) * r) >> 8);   // Q2.30 in [1, 2)
    uint64_t v = (static_cast<uint64_t>(base) * mant) >> 30;
    if (oct >= 0) {
        if (oct >= 32 || v > (0xFFFFFFFFull >> oct)) return 0xFFFFFFFFu;
        return static_cast<uint32_t>(v << oct);
    }
    return oct <= -32 ? 0u : static_cast<uint32_t>(v >> (-oct));
}

// Phase increment of a pitch given in 1/256 semitone. [CONTROL]
inline uint32_t pitch_to_inc(int32_t pitch, uint32_t a4_inc) {
    // 65536 / (12 * 256) = 64 / 3. Clamped first so the product stays in 32 bits: a 64-bit divide is a library call on the ESP32 (~100+ cycles).
    const int32_t d = pitch - kPitchA4 < -196608 ? -196608 : (pitch - kPitchA4 > 196608 ? 196608 : pitch - kPitchA4);    // +-768 semitones = +-64 octaves, as before
    int32_t x = d * 64 / 3;
    x = x < -(1 << 22) ? -(1 << 22) : (x > (1 << 22) ? (1 << 22) : x);
    uint32_t inc = exp2_scale(a4_inc, x);
    return inc > 2147483648u ? 2147483648u : inc;                             // clamp at Nyquist
}

// One cycle of sine, q15, 1024-entry table + linear interpolation (about -90 dB distortion). [AUDIO]
inline q15 sine(uint32_t phase) {
    uint32_t idx = phase >> (32 - kSineBits);
    int32_t frac = static_cast<int32_t>((phase >> (16 - kSineBits)) & 0xFFFFu);   // 16-bit fraction
    int32_t a = kSineTab[idx], b = kSineTab[idx + 1];
    return static_cast<q15>(a + (((b - a) * frac) >> 16));
}

}  // namespace sc
