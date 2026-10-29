#pragma once
// Effect modules (global scope, stereo or mono). Type ids and parameter indexes.
#include "engine/core/module.h"

#ifndef ENGINE_DELAY_MAX_MS
#define ENGINE_DELAY_MAX_MS 1000        // longest delay time; two lines of this length live in the bulk heap
#endif

// Section cycle counters (ESP32 with -DENGINE_PROFILE): the reverb and delay loops add the cycles of each stage to g_sec_prof[]; the audio task prints
// and clears them with the [PROF] line. No effect (and no code) otherwise.
#if defined(ENGINE_PROFILE) && defined(ARDUINO_ARCH_ESP32)
#include <esp_cpu.h>
extern uint32_t g_sec_prof[16];
#define SEC_BEGIN() uint32_t sec_t = esp_cpu_get_cycle_count()
#define SEC_MARK(n) do { const uint32_t sec_n = esp_cpu_get_cycle_count(); g_sec_prof[n] += sec_n - sec_t; sec_t = sec_n; } while (0)
#else
#define SEC_BEGIN() do {} while (0)
#define SEC_MARK(n) do {} while (0)
#endif

namespace sc {

enum FxType : int { T_DELAY = 48, T_SPECTRAL, T_VOCODER, /* 51 = global oscillator */ T_CHORUS = 52, T_REVERB };

// Delay (stereo): ins L, R; outs L, R.   time in 1/16 ms.  mod: time (+-time_mod)
// Hermite-interpolated reads, a one-pole low-pass in the feedback path, optional ping-pong. Time changes glide
// (tape-like) instead of jumping, so there are no clicks.
enum { DLY_TIME, DLY_FEEDBACK, DLY_DAMP, DLY_MIX, DLY_PINGPONG, DLY_TIME_MOD, DLY_N };

// SpectralFx (mono): in 0, out 0.  STFT 512, 75 % overlap: 512 samples of latency (the dry path in the mix is delayed
// to match). One phase-vocoder core serves freeze and pitch shift. `stereo` = 1 (the FX rack): ins L, R and outs L, R; the
// STFT runs on (L + R) / 2 and each output mixes its own delayed dry channel with the shared wet signal.
enum { SPX_MODE, SPX_AMOUNT, SPX_LO, SPX_HI, SPX_SHIFT, SPX_FREEZE, SPX_MIX, SPX_STEREO, SPX_N };
enum { SPXM_THRU, SPXM_FREEZE, SPXM_GATE, SPXM_ROBOT, SPXM_WHISPER, SPXM_PITCH };
//   THRU     untouched (reconstruction test, latency compensation)
//   FREEZE   holds the spectrum with its phase advance when `freeze` = 1; `shift` still applies
//   GATE     zeroes bins below amount * frame peak (amplitude ratio, q15) and outside lo..hi (pitch units)
//   ROBOT    all phases to zero: a buzz at fs / (N / 4)
//   WHISPER  random phases: breathy, keeps the spectral envelope
//   PITCH    phase-vocoder pitch shift by `shift` (1/256 semitone, +-24 semitones)

// Vocoder (dual STFT): ins modulator, carrier; out.  The modulator's band envelope shapes the carrier's spectrum.
enum { VOC_BAND_LOG2, VOC_GAIN, VOC_N };

// Chorus (stereo): ins L, R; outs L, R. Juno-like fixed modes, one modulated line per channel with opposite LFO
// phases: I (0.51 Hz), II (0.86 Hz) and I+II (9.75 Hz, shallow). Wet is added to the dry signal (`mix` scales it).
enum { CHR_MODE, CHR_MIX, CHR_N };
enum { CHRM_OFF, CHRM_I, CHRM_II, CHRM_I_II };

// Reverb (Dattorro plate, stereo): ins L, R (summed); outs L, R. Pre-delay (ms) lives in the bulk heap, the tank in
// the fast heap. size: q15 -> tank length x0.5 .. x1.25 (changes glide); decay q15; damp / bandwidth are cutoffs in
// pitch units (MIDI * 256); mod q15 scales the 16-sample excursion of the two modulated all-passes; mix crossfades
// dry / wet.
enum { RVB_PREDELAY, RVB_DECAY, RVB_DAMP, RVB_BANDWIDTH, RVB_SIZE, RVB_MOD, RVB_MIX, RVB_N };

void register_fx_modules(Registry &reg);

}  // namespace sc
