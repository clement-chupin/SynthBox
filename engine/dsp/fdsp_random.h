#pragma once
// Ported from stmlib (Mutable Instruments, (c) 2012-2017 Emilie Gillet, MIT licence: see the notice in src/engine/mi/LICENSE) into the
// framework so the Braids / Plaits algorithms (src/engine/mi) depend on our code only. Namespace sc::fdsp ("float DSP" helpers, plus the
// integer table helpers of Braids), kept bit-identical to the originals: replacing a helper by one of our own (svf.h, util.h ...) is done
// under an A/B test (ENGINE_DESIGN.md ADR-039).
#include "engine/dsp/fdsp.h"

namespace sc {
namespace fdsp {

/* ---- the shared random generator (stmlib/utils/random.h) ---- */
class Random {
 public:
  static inline uint32_t state() { return rng_state_; }

  static inline void Seed(uint32_t seed) {
    rng_state_ = seed;
  }

  static inline uint32_t GetWord() {
    rng_state_ = rng_state_ * 1664525L + 1013904223L;
    return state();
  }
  
  static inline int16_t GetSample() {
    return static_cast<int16_t>(GetWord() >> 16);
  }

  static inline float GetFloat() {
    return static_cast<float>(GetWord()) / 4294967296.0f;
  }

 private:
  static uint32_t rng_state_;

  DISALLOW_COPY_AND_ASSIGN(Random);
};

}  // namespace fdsp
}  // namespace sc
