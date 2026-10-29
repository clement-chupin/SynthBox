#pragma once
// Small audio utilities: noise, DC blocker, soft clipper.
#include <cmath>
#include <cstdint>
#include "engine/dsp/config.h"
#include "engine/dsp/q.h"

namespace sc {

// White noise, xorshift32. [AUDIO]
class Noise {
public:
    explicit Noise(uint32_t seed = 0x1234567u) : s_(seed ? seed : 1u) {}
    uint32_t next_u32() { s_ ^= s_ << 13; s_ ^= s_ >> 17; s_ ^= s_ << 5; return s_; }
    q15 next() { return static_cast<q15>(next_u32() >> 16); }
private:
    uint32_t s_;
};

// First-order DC blocker: y[n] = x[n] - x[n-1] + R * y[n-1]. q31 state keeps the low corner clean.
class DcBlocker {
public:
    // corner frequency in Hz (init/control rate)
    void set_corner(float hz, int fs = kSampleRate) {
        double r = 1.0 - 2.0 * 3.14159265358979 * static_cast<double>(hz) / fs;
        r_ = q31_from_float(r < 0.0 ? 0.0 : (r > 0.999999 ? 0.999999 : r));
    }
    void reset() { x1_ = 0; y1_ = 0; }
    // [AUDIO]
    q15 process(q15 x) {
        q31 xs = to31(x);
        q31 y = add31(sub31(xs, x1_), mul31(r_, y1_));
        x1_ = xs;
        y1_ = y;
        return to15(y);
    }
private:
    q31 r_ = q31_from_float(0.995), x1_ = 0, y1_ = 0;
};

// Cubic soft clipper y = 1.5 x - 0.5 x^3 on [-1, 1], flat (unity limit) outside. Input is the already
// driven signal: scale with a gain first. Continuous first derivative at the rails. [AUDIO]
constexpr q15 softclip15(q15 x) {
    const int32_t xi = x;
    const int32_t x2 = (xi * xi) >> 15;                         // Q15
    const int32_t factor = 49152 - (x2 >> 1);                   // 1.5 - 0.5 x^2, Q15
    return sat16((xi * factor) >> 15);
}

// Clamp to the q15 range (a "hard clip" on a wider accumulator).
constexpr q15 hardclip15(int32_t x) { return sat16(x); }

}  // namespace sc
