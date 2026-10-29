#pragma once
// Oscillator engines (voice scope): one module, eight sound generators chosen by `engine`, each with the same three controls
// (pitch, timbre, morph) so a rack oscillator can switch between them. Inspired by the macro-oscillator idea of Mutable Instruments
// Plaits (parameters named by effect, not by algorithm), implemented from scratch in fixed point.
//
//   engine        timbre                              morph
//   KARP  string  brightness of the pluck / filter    sustain (decay time of the string)
//   MODAL struck  material: harmonic string -> bell   decay time of the modes (higher modes die first)
//   FM2   2-op FM modulation index (0..8 rad)         frequency ratio (0.5 .. 12, stepped)
//   FOLD  folder  fold gain (1x .. 9x)                waveform that is folded: sine -> saw
//   SSAW  supersaw detune spread (up to +-4.5 %)      centre voice -> full unison
//   VOWEL formant vowel position A E I O U            resonance (bandwidth) of the formants
//   ADD   additive (12 harmonics) brightness (slope)  odd / even balance (1 = odd only)
//   DUST  crackle  density (2 Hz .. 4 kHz)            resonance of the pitched pings
//   STR   strings  detune of a pair of oscillators    wave: saw -> pulse -> triangle (thirds)          (ADR-037: the Strings voice's oscillators)
//
// quality (STR, and nothing else here): 0 = Blep (means Mip for STR), 1 = Mip (band-limited tables), 2 = Naive (cheapest, aliases).
// STR follows the pitch at block rate.
//
// in 0 pitch CV (like Osc), in 1 gate (strikes KARP / MODAL, retriggers); mod: pitch (+-pitch_mod), timbre, morph (q15, block rate).
#include "engine/core/module.h"

namespace sc {

enum OscxEngine : int { OSCX_KARP, OSCX_MODAL, OSCX_FM2, OSCX_FOLD, OSCX_SSAW, OSCX_VOWEL, OSCX_ADD, OSCX_DUST, OSCX_STR, OSCX_ENGINES };
enum { OSCX_ENGINE, OSCX_PITCH, OSCX_TIMBRE, OSCX_MORPH, OSCX_LEVEL, OSCX_PITCH_MOD, OSCX_QUAL, OSCX_N };

constexpr int T_OSCX = 60;

// Per-engine cycle counters (ESP32 with -DENGINE_PROFILE): process() adds its cycles to g_osc_prof[engine]; the audio task prints and clears them.
#if defined(ENGINE_PROFILE) && defined(ARDUINO_ARCH_ESP32)
extern uint32_t g_osc_prof[OSCX_ENGINES];
#endif
void register_osc_engines(Registry &reg);

}  // namespace sc
