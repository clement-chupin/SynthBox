#pragma once
// Zero-delay-feedback (TPT) state-variable filter, fixed point (ADR-014). One 2-pole section gives LP, BP,
// HP and notch from the same two integrators; N sections cascade into 12..48 dB/oct.
//
// Number formats:
//   signal / state   int32 Q28 (1.0 = 2^28, range +-8: resonance headroom; beyond that the state saturates,
//                    which acts as a limiter instead of wrapping)
//   g = tan(pi fc/fs) unsigned Q4.28   (table, no float)
//   k = 1/Q          Q3.29, k in (0, 4)
//   a1,a2,a3         Q31 (all < 1 for g > 0, k >= 0.25)
//   1/(1 + g(g+k)) uses a division-free Newton reciprocal, so a coefficient update costs about 25 multiplies
//   and can run per sample when the cutoff is audio-rate modulated.
#include <cstdint>
#include <cstring>
#include "engine/dsp/q.h"
#include "engine/dsp/tables.h"

namespace sc {

constexpr uint32_t kSvfMaxInc = 1932735283u;        // 0.45 * 2^32: highest cutoff (fc = 0.45 fs)
constexpr int kSvfHeadroomBits = 3;                  // Q28 signal: 3 bits above full scale

constexpr int32_t svf_in(q15 x) { return static_cast<int32_t>(x) * (1 << 13); }                 // q15 -> Q28
inline q15 svf_out(int32_t y) { int32_t v = (y + (1 << 12)) >> 13; return sat16(v); }             // Q28 -> q15

// g = tan(pi * fc / fs) from the cutoff phase increment (fc/fs * 2^32). Table + linear interpolation.
inline uint32_t svf_g(uint32_t inc) {
    if (inc > kSvfMaxInc) inc = kSvfMaxInc;
    uint32_t idx = inc >> 23;
    uint32_t frac = (inc >> 7) & 0xFFFFu;
    uint32_t a = kTanTab[idx], b = kTanTab[idx + 1];
    return a + static_cast<uint32_t>((static_cast<uint64_t>(b - a) * frac) >> 16);
}

// 1/D for D = d / 2^26 in [1, 64): returns Q31 (clamped to 2^31 - 1). Newton-Raphson, no division.
inline uint32_t svf_recip_q26(uint32_t d) {
    int n = __builtin_clz(d);
    uint32_t dn = d << n;                                            // [2^31, 2^32): M = dn / 2^32 in [0.5, 1)
    // Seed from a 256-entry table with linear interpolation (error ~1e-5), then one Newton step (error ~1e-10, below Q30). Every product is
    // 32 x 32 -> 64, a single multiply instruction pair; the former three steps from a straight-line seed used 64 x 64 products.
    const uint32_t idx = (dn >> 23) & 0xFFu, frac = (dn >> 7) & 0xFFFFu;
    const uint32_t ta = kRecipTab[idx], tb = kRecipTab[idx + 1];
    uint32_t r = ta - static_cast<uint32_t>((static_cast<uint64_t>(ta - tb) * frac) >> 16);      // 1/M in Q30
    const uint32_t e = static_cast<uint32_t>((static_cast<uint64_t>(dn) * r) >> 32);             // M * R in Q30 (about 1.0)
    const uint32_t t = (1u << 31) - e;                                                           // 2 - M R in Q30
    r = static_cast<uint32_t>((static_cast<uint64_t>(r) * t) >> 30);
    int sh = n - 5;                                                  // 1/D = R * 2^(n-6), R in Q30 -> Q31
    uint64_t a = sh >= 0 ? (static_cast<uint64_t>(r) << sh) : (static_cast<uint64_t>(r) >> -sh);
    return a > 0x7FFFFFFFull ? 0x7FFFFFFFu : static_cast<uint32_t>(a);
}

struct SvfCoef { int32_t a1 = 0x7FFFFFFF, a2 = 0, a3 = 0; int32_t k = 1 << 29; };

inline SvfCoef svf_coef(uint32_t g, int32_t k) {
    SvfCoef c;
    uint32_t g2 = static_cast<uint32_t>((static_cast<uint64_t>(g) * g) >> 30);            // Q26
    uint32_t gk = static_cast<uint32_t>((static_cast<uint64_t>(g) * static_cast<uint32_t>(k)) >> 31);   // Q26
    uint32_t d = (1u << 26) + g2 + gk;
    uint32_t a1 = svf_recip_q26(d);
    c.a1 = static_cast<int32_t>(a1);
    c.a2 = static_cast<int32_t>((static_cast<uint64_t>(g) * a1) >> 28);
    c.a3 = static_cast<int32_t>((static_cast<uint64_t>(g) * static_cast<uint32_t>(c.a2)) >> 28);
    c.k = k;
    return c;
}

struct SvfState { int32_t ic1 = 0, ic2 = 0; };

constexpr int32_t sat_q28(int64_t v) {
    return static_cast<int32_t>(v > 0x7FFFFFFFLL ? 0x7FFFFFFFLL : (v < -0x80000000LL ? -0x80000000LL : v));
}

// k * x for k in Q3.29 and x in Q28 -> Q28 (saturated).
constexpr int32_t svf_kmul(int32_t k, int32_t x) { return shl_sat(mulh(k, x), 3); }

// One sample. x, lp, bp, hp are Q28. [AUDIO]
// All in 32 bits: a Q31 coefficient times a Q28 value through mulh() is Q27, doubled back to Q28. Each product keeps 27 bits, 11 more than
// the 16-bit output needs (the 64-bit version this replaces cost 264 cycles per sample on the ESP32-S3, this one far less).
// v3 = x - ic2 saturates at 32 bits (+-8.0 in Q28), which only matters when the filter is already 8x overloaded.
inline void svf_tick(const SvfCoef &c, SvfState &s, int32_t x, int32_t &lp, int32_t &bp, int32_t &hp) {
    const int32_t v3 = sub31(x, s.ic2);
    const int32_t v1 = shl_sat(mulh(c.a1, s.ic1) + mulh(c.a2, v3), 1);
    const int32_t v2 = add31(s.ic2, shl_sat(mulh(c.a2, s.ic1) + mulh(c.a3, v3), 1));
    s.ic1 = sub31(add31(v1, v1), s.ic1);
    s.ic2 = sub31(add31(v2, v2), s.ic2);
    lp = v2;
    bp = v1;
    hp = sub31(sub31(x, svf_kmul(c.k, v1)), v2);
}


// ---- single-precision float version (ESP32-S3 has a hardware FPU; used only if it measures faster, see bench_esp32.cpp) ----
// State and signal in float with 1.0 = full scale (Q28 value / 2^28), same TPT structure. 24-bit mantissa.
struct SvfCoefF { float a1 = 1.0f, a2 = 0.0f, a3 = 0.0f, k = 1.0f; };
struct SvfStateF { float ic1 = 0.0f, ic2 = 0.0f; };

inline SvfCoefF svf_coef_f(uint32_t g_q28, int32_t k_q29) {
    const float g = static_cast<float>(g_q28) * (1.0f / 268435456.0f);
    const float k = static_cast<float>(k_q29) * (1.0f / 536870912.0f);
    SvfCoefF c;
    c.a1 = 1.0f / (1.0f + g * (g + k));
    c.a2 = g * c.a1;
    c.a3 = g * c.a2;
    c.k = k;
    return c;
}

// Float coefficients for n sections that share one g (tan, 0..~6) and have their own 1/Q (k[]): the per-sample path of the filter module.
// On the ESP32-S3 a float divide costs 68 cycles and a bit cast 62, but a multiply-add 6 and an int->float convert 4, so one divide serves all the
// sections: with d_i = 1 + g (g + k_i) and a1_i = 1 / d_i, the batch inversion r = 1 / (d_1 d_2 ...) gives each a1_i as r times the others' product.
inline void svf_coef_batch(float g, const float *k, int n, SvfCoefF *c) {
    float d[4];
    for (int s = 0; s < n; s++) d[s] = 1.0f + g * (g + k[s]);
    float a1[4];
    if (n == 1) {
        a1[0] = 1.0f / d[0];
    } else if (n == 2) {
        const float r = 1.0f / (d[0] * d[1]);
        a1[0] = d[1] * r; a1[1] = d[0] * r;
    } else if (n == 3) {
        const float p12 = d[0] * d[1], r = 1.0f / (p12 * d[2]), t = d[2] * r;
        a1[2] = p12 * r; a1[0] = d[1] * t; a1[1] = d[0] * t;
    } else {
        const float p12 = d[0] * d[1], p34 = d[2] * d[3], r = 1.0f / (p12 * p34), u = p34 * r, v = p12 * r;     // u = 1/p12, v = 1/p34
        a1[0] = d[1] * u; a1[1] = d[0] * u; a1[2] = d[3] * v; a1[3] = d[2] * v;
    }
    for (int s = 0; s < n; s++) {
        c[s].a1 = a1[s];
        c[s].a2 = g * a1[s];
        c[s].a3 = g * c[s].a2;
        c[s].k = k[s];
    }
}

// Float copy of an integer coefficient set (the integer routine stays the one that computes them: cheaper than a float divide on the ESP32).
inline SvfCoefF svf_to_float(const SvfCoef &c) {
    SvfCoefF f;
    f.a1 = static_cast<float>(c.a1) * (1.0f / 2147483648.0f);
    f.a2 = static_cast<float>(c.a2) * (1.0f / 2147483648.0f);
    f.a3 = static_cast<float>(c.a3) * (1.0f / 2147483648.0f);
    f.k = static_cast<float>(c.k) * (1.0f / 536870912.0f);
    return f;
}

// float (1.0 = full scale) -> q15, rounded and saturated, without a library call.
inline q15 svf_out_f(float y) {
    y = y * 32768.0f;
    y = y > 32767.0f ? 32767.0f : (y < -32768.0f ? -32768.0f : y);
    return static_cast<q15>(static_cast<int32_t>(y + 32768.5f) - 32768);
}

inline void svf_tick_f(const SvfCoefF &c, SvfStateF &s, float x, float &lp, float &bp, float &hp) {
    const float v3 = x - s.ic2;
    const float v1 = c.a1 * s.ic1 + c.a2 * v3;
    const float v2 = s.ic2 + c.a2 * s.ic1 + c.a3 * v3;
    s.ic1 = 2.0f * v1 - s.ic1;
    s.ic2 = 2.0f * v2 - s.ic2;
    lp = v2;
    bp = v1;
    hp = x - c.k * v1 - v2;
}

}  // namespace sc
