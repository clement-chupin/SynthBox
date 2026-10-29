#pragma once
// Ported from stmlib (Mutable Instruments, (c) 2012-2017 Emilie Gillet, MIT licence: see the notice in src/engine/mi/LICENSE) into the
// framework so the Braids / Plaits algorithms (src/engine/mi) depend on our code only. Namespace sc::fdsp ("float DSP" helpers, plus the
// integer table helpers of Braids), kept bit-identical to the originals: replacing a helper by one of our own (svf.h, util.h ...) is done
// under an A/B test (ENGINE_DESIGN.md ADR-039).
#include "engine/dsp/fdsp.h"

namespace sc {
namespace fdsp {

/* ---- semitones -> frequency ratio (stmlib/dsp/units.h) ---- */
extern const float lut_pitch_ratio_high[257];
extern const float lut_pitch_ratio_low[257];

inline float SemitonesToRatio(float semitones) {
  float pitch = semitones + 128.0f;
  MAKE_INTEGRAL_FRACTIONAL(pitch)

  return lut_pitch_ratio_high[pitch_integral] * \
      lut_pitch_ratio_low[static_cast<int32_t>(pitch_fractional * 256.0f)];
}

inline float SemitonesToRatioSafe(float semitones) {
  float scale = 1.0f;
  while (semitones > 120.0f) {
    semitones -= 120.0f;
    scale *= 1024.0f;
  }
  while (semitones < -120.0f) {
    semitones += 120.0f;
    scale *= 1.0f / 1024.0f;
  }
  return scale * SemitonesToRatio(semitones);
}


inline float Exp2Safe(float value) {
  return SemitonesToRatioSafe(value * 12.0f);
}

}  // namespace fdsp
}  // namespace sc
