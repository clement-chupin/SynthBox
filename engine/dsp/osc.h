#pragma once
// Band-limited oscillator waveforms (ADR-015): PolyBLEP for steps (saw, pulse) and PolyBLAMP for slope
// corners (triangle). `t` is the phase (2^32 = one cycle) and `dt` the per-sample increment, both unsigned,
// so audio-rate pitch modulation just changes dt every sample. Outputs are q15.
// The correction is a 2-sample-wide polynomial around each discontinuity; the only division happens inside
// that window (at most twice per cycle).
#include <cstdint>
#include "engine/dsp/q.h"

namespace sc {

// Residual of a unit-height step at phase 0 (to subtract for a falling step of size 2, add for a rising one),
// Q15 in [-1, 0] after the step and [0, 1] before it.
inline int32_t blep_q15(uint32_t t, uint32_t dt) {
    if (dt == 0) return 0;
    if (t < dt) {                                                        // just after the step
        int64_t x = (static_cast<int64_t>(t) << 15) / dt;                // Q15 0..1
        return static_cast<int32_t>(2 * x - ((x * x) >> 15) - 32768);
    }
    if (t > 0u - dt) {                                                   // just before the step
        // t viewed as signed is the (negative) distance to the step, |distance| < dt
        int64_t x = (static_cast<int64_t>(static_cast<int32_t>(t)) << 15) / static_cast<int64_t>(dt);   // Q15 -1..0
        return static_cast<int32_t>(((x * x) >> 15) + 2 * x + 32768);
    }
    return 0;
}

// Rising saw (-1 .. +1): naive minus the step residual.
inline q15 osc_saw(uint32_t t, uint32_t dt) {
    int32_t naive = static_cast<int32_t>(t >> 16) - 32768;
    return sat16(naive - blep_q15(t, dt));
}

// Pulse with the falling edge at phase `pw` (threshold, 2^32 * duty). Levels are +-1 like an analog PWM, so
// the output carries a DC offset of 2*duty - 1 (removing it would push a narrow pulse past full scale).
inline q15 osc_pulse(uint32_t t, uint32_t dt, uint32_t pw) {
    int32_t naive = t < pw ? 32767 : -32768;
    return sat16(naive + blep_q15(t, dt) - blep_q15(t - pw, dt));
}

// Corner residual (Q15) for a slope change at phase 0: s' * (1-|x|)^3 / 6 where s' is the slope change per
// sample and x the distance to the corner in samples (the integral of the BLEP residual).
inline int32_t blamp_q15(uint32_t t, uint32_t dt) {
    if (dt == 0) return 0;
    uint32_t d;
    if (t < dt) d = t;
    else if (t > 0u - dt) d = 0u - t;
    else return 0;
    int32_t x = static_cast<int32_t>((static_cast<uint64_t>(d) << 15) / dt);       // Q15 0..1
    int32_t u = 32768 - x;
    int32_t u3 = static_cast<int32_t>((static_cast<int64_t>((u * u) >> 15) * u) >> 15);
    int64_t dt15 = dt >> 17;                                                       // dt in Q15 of a cycle
    return static_cast<int32_t>((8 * dt15 * u3 / 6) >> 15);                        // slope change of 8 per cycle
}

// Triangle (+1 at phase 0, -1 at phase 0.5) with corner correction.
inline q15 osc_tri(uint32_t t, uint32_t dt) {
    int32_t naive = t < 0x80000000u ? 32767 - static_cast<int32_t>(t >> 15) : static_cast<int32_t>(t >> 15) - 98303;
    int32_t v = naive - blamp_q15(t, dt) + blamp_q15(t + 0x80000000u, dt);
    return sat16(v);
}

}  // namespace sc
