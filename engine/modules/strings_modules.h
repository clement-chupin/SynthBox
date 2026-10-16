#pragma once
// The Strings engine (ADR-037): thin voices for pads and big chords, up to 32 of them, and the string Ensemble effect.
//
//   Strings  (voice)  in 0 pitch CV, in 1 gate, out 0. One complete voice in one module (no per-module overhead): two detuned oscillators
//                     (saw / pulse / triangle; naive or band-limited mipmap tables), a linear ADSR computed once per block (the envelope of the
//                     ESP32Synth library), an optional one-pole lowpass whose cutoff follows the key and the envelope, velocity and level.
//                     Times in ms, sustain / mix / pw / key / level q15, detune / cutoffs in 1/256 semitone (cutoff as a MIDI note * 256).
//   Ensemble (global) ins L, R; outs L, R. A string ensemble (Solina style): three taps on one short delay line, modulated by a slow and a
//                     fast LFO at 0 / 120 / 240 degrees. rate in hundredths of Hz (slow LFO; the fast one runs at 10.5 x), depth / shimmer
//                     (fast LFO amount) / mix q15.
#include "engine/core/module.h"

namespace sc {

constexpr int T_STRINGS = 69;
constexpr int T_ENSEMBLE = 70;

enum {
    STR_WAVE, STR_OSC, STR_DETUNE, STR_MIX, STR_PW,
    STR_ATTACK, STR_DECAY, STR_SUSTAIN, STR_RELEASE,
    STR_LP_ON, STR_LP_CUT, STR_LP_ENV, STR_LP_KEY, STR_LEVEL,
    STR_N
};
enum { STRW_SAW, STRW_PULSE, STRW_TRI, STRW_N };
enum { STRO_NAIVE, STRO_MIP, STRO_N };

enum { ENS_RATE, ENS_DEPTH, ENS_SHIMMER, ENS_MIX, ENS_N };

void register_strings_modules(Registry &reg);

}  // namespace sc
