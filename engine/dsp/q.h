#pragma once
// Fixed-point primitives (ADR-002): audio signals are q15 (int16), internal state/accumulators are
// q31 / int64. All helpers saturate where noted and round to nearest. Pure integer, no FPU needed.
//
// Formats used by the engine:
//   q15  : Q0.15, range [-1, 1)           audio buffers, levels, gains
//   q31  : Q0.31, range [-1, 1)           filter state, envelopes, delay feedback paths
//   u32 phase: 2^32 = one cycle           oscillators / LFOs (wrap naturally)
//   coefficients with range > 1 use "mulq(a, b, shift)" with an explicit Q format (see mulq).
#include <cstdint>

namespace sc {

using q15 = int16_t;
using q31 = int32_t;

constexpr q15 kQ15Max = 32767;
constexpr q15 kQ15Min = -32768;
constexpr q31 kQ31Max = 2147483647;
constexpr q31 kQ31Min = -2147483647 - 1;

/* ---- saturation ---- */
constexpr q15 sat16(int32_t x) { return static_cast<q15>(x > kQ15Max ? kQ15Max : (x < kQ15Min ? kQ15Min : x)); }
constexpr q31 sat32(int64_t x) { return static_cast<q31>(x > kQ31Max ? kQ31Max : (x < kQ31Min ? kQ31Min : x)); }

/* ---- q15 ---- */
constexpr q15 add15(q15 a, q15 b) { return sat16(static_cast<int32_t>(a) + b); }
constexpr q15 sub15(q15 a, q15 b) { return sat16(static_cast<int32_t>(a) - b); }
constexpr q15 neg15(q15 a)        { return sat16(-static_cast<int32_t>(a)); }
// mul15 without the saturation, for a gain b in [-32767, 32767] (any |gain| < 1): the product cannot leave the q15 range, so the two compares go.
constexpr q15 mul15n(q15 a, q15 b) { return static_cast<q15>((static_cast<int32_t>(a) * b + (1 << 14)) >> 15); }
constexpr q15 mul15(q15 a, q15 b) { return sat16((static_cast<int32_t>(a) * b + (1 << 14)) >> 15); }   // -1 * -1 saturates

/* ---- 32-bit multiply helpers ---- */
// High 32 bits of a 32 x 32 signed product: one multiply instruction (MULSH on Xtensa, 5 cycles measured), no 64-bit shifts or adds.
constexpr int32_t mulh(int32_t a, int32_t b) { return static_cast<int32_t>((static_cast<int64_t>(a) * b) >> 32); }
// v << n, saturated to 32 bits.
constexpr int32_t shl_sat(int32_t v, int n) {
    const int32_t lim = 1 << (31 - n);
    return v >= lim ? 0x7FFFFFFF : (v < -lim ? static_cast<int32_t>(0x80000000u) : v * (1 << n));
}

/* ---- q31 ---- */
// 32-bit saturating add / sub: the overflow builtin and a select, instead of a 64-bit add and two 64-bit compares.
constexpr q31 add31(q31 a, q31 b) {
    q31 r = 0;
    return __builtin_add_overflow(a, b, &r) ? (a < 0 ? kQ31Min : kQ31Max) : r;
}
constexpr q31 sub31(q31 a, q31 b) {
    q31 r = 0;
    return __builtin_sub_overflow(a, b, &r) ? (a < 0 ? kQ31Min : kQ31Max) : r;
}
constexpr q31 neg31(q31 a)        { return sat32(-static_cast<int64_t>(a)); }
// Truncating (floor) rather than rounded: half a Q31 LSB of bias, far below anything a 16-bit signal can show; saves the 64-bit add and compares.
constexpr q31 mul31(q31 a, q31 b) { return shl_sat(mulh(a, b), 1); }

/* ---- conversions ---- */
constexpr q31 to31(q15 x) { return static_cast<q31>(static_cast<uint32_t>(static_cast<int32_t>(x)) << 16); }
// Round to nearest: (x + 2^15) >> 16 without the 64-bit add (x >> 16 plus the bit just below it); saturates only for x within 2^15 of the top.
constexpr q15 to15(q31 x) { return sat16((x >> 16) + ((x >> 15) & 1)); }

// q15 x q31 -> q31 (state update with an audio-rate input)
constexpr q31 mul31_15(q31 a, q15 b) { return sat32((static_cast<int64_t>(a) * b + (1 << 14)) >> 15); }

/* ---- general fixed multiply: (a * b) >> shift, rounded, saturated to q31 ----
 * Use for coefficients with an integer part, e.g. a Q2.30 coefficient (range [-2, 2)) times a q31
 * state is mulq(state, coef, 30). Keep the Q format of every coefficient in its name or comment. */
constexpr q31 mulq(q31 a, q31 b, int shift) {
    return sat32((static_cast<int64_t>(a) * b + (1LL << (shift - 1))) >> shift);
}

/* ---- float bridges: control-rate / init / tests ONLY (never in the audio path) ---- */
constexpr q15 q15_from_float(float f) { return sat16(static_cast<int32_t>(f * 32768.0f + (f >= 0 ? 0.5f : -0.5f))); }
constexpr q31 q31_from_float(double f) {
    return sat32(static_cast<int64_t>(f * 2147483648.0 + (f >= 0 ? 0.5 : -0.5)));
}
constexpr float q15_to_float(q15 x) { return static_cast<float>(x) * (1.0f / 32768.0f); }
constexpr double q31_to_double(q31 x) { return static_cast<double>(x) * (1.0 / 2147483648.0); }

/* ---- misc ---- */
constexpr int32_t clamp_i32(int32_t x, int32_t lo, int32_t hi) { return x < lo ? lo : (x > hi ? hi : x); }
constexpr q15 abs15(q15 a) { return a < 0 ? neg15(a) : a; }

}  // namespace sc
