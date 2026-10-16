#pragma once
// Parameter smoothers (ADR-007: both kinds, chosen per parameter type).
//   Ramp    : linear, reaches the target exactly after n samples. For gains, mixes, pitch glides, and
//             anything the control rate retargets once per block. One add per sample.
//   OnePole : exponential. For knobs and values with no natural deadline; snaps when the step rounds to 0.
#include <cmath>
#include <cstdint>
#include "engine/dsp/config.h"
#include "engine/dsp/q.h"

namespace sc {

// q31 value, linear ramp. Typical use: ramp.set_target(gain31, kBlock) once per block, then next() per sample.
class Ramp {
public:
    void reset(q31 v) { cur_ = v; step_ = 0; left_ = 0; target_ = v; }
    void set_target(q31 target, int samples) {
        target_ = target;
        if (samples <= 0) { cur_ = target; step_ = 0; left_ = 0; return; }
        left_ = samples;
        step_ = (static_cast<int64_t>(target) - cur_) / samples;
    }
    // [AUDIO] advance one sample, returns the new value
    q31 next() {
        if (left_ > 0) {
            if (--left_ == 0) cur_ = target_;
            else cur_ = static_cast<q31>(cur_ + step_);
        }
        return cur_;
    }
    q31 value() const { return cur_; }
    bool active() const { return left_ > 0; }

private:
    q31 cur_ = 0, target_ = 0;
    int64_t step_ = 0;
    int left_ = 0;
};

class OnePole {
public:
    // time constant in milliseconds to reach ~63 % of a step (control rate / init)
    static q31 coef_from_ms(float ms) {
        double k = 1.0 - std::exp(-1.0 / (static_cast<double>(ms) * 0.001 * kSampleRate));
        return q31_from_float(k > 0.999999 ? 0.999999 : k);
    }
    void reset(q31 v) { y_ = v; }
    void set_coef(q31 k) { k_ = k; }
    // [AUDIO]
    q31 next(q31 target) {
        int64_t diff = static_cast<int64_t>(target) - y_;
        int64_t inc = (diff * k_ + (1LL << 30)) >> 31;
        if (inc == 0) y_ = target; else y_ = static_cast<q31>(y_ + inc);
        return y_;
    }
    q31 value() const { return y_; }

private:
    q31 y_ = 0, k_ = 0;
};

}  // namespace sc
