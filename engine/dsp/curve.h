#pragma once
// Segment shapes with tension (envelopes). A curve value c in -1..+1 (q15) maps a segment phase p in 0..1 to a progress s in 0..1:
//   s(p) = (1 - exp(-k p)) / (1 - exp(-k)),  k = 8 c
//   c = 0: a straight line.  c > 0: fast start, slow end (an analog capacitor charging / discharging: the natural decay).
//   c < 0: slow start, fast end (a swelling, accelerating segment).  |c| = 1 is a very strong bend (k = 8: 99.97 % after the first half... of the time constant).
// The shape is a 128-interval table in q15 with linear interpolation, built at control time (uses exp()); evaluating it is two reads and a
// multiply, so envelopes cost the same whatever the curve.
#include <cmath>
#include <cstdint>
#include "engine/dsp/q.h"

namespace sc {

struct CurveLut {
    static constexpr int kBits = 7;                         // 128 intervals
    static constexpr int kN = 1 << kBits;
    int16_t t[kN + 1] = {};

    void build(q15 curve) {
        const double k = 8.0 * static_cast<double>(curve) / 32767.0;
        for (int i = 0; i <= kN; i++) {
            const double p = static_cast<double>(i) / kN;
            const double s = std::fabs(k) < 1e-3 ? p : (1.0 - std::exp(-k * p)) / (1.0 - std::exp(-k));
            t[i] = static_cast<int16_t>(std::lround(std::fmax(0.0, std::fmin(1.0, s)) * 32767.0));
        }
    }
    // p: phase as an unsigned Q32 (0 .. 2^32-1). Returns the progress as q15 (0 .. 32767).
    int32_t at(uint32_t p) const {
        const uint32_t idx = p >> (32 - kBits);
        const int32_t frac = static_cast<int32_t>((p >> (32 - kBits - 15)) & 0x7FFF);        // 15 bits of the position inside the interval
        const int32_t a = t[idx], b = t[idx + 1];
        return a + (((b - a) * frac) >> 15);
    }
};

}  // namespace sc
