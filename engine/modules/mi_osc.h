#pragma once
// Macro oscillator models from Mutable Instruments Braids and Plaits (MIT, (c) Emilie Gillet; sources in src/engine/mi, their helpers ported to
// src/engine/dsp/fdsp*.h): one voice-scope module, the model chosen by `model`, the same controls as OscEngines (pitch, timbre, morph) plus
// `harm` (Plaits' third control; Braids ignores it). ADR-039.
//
// Tiers 1 and 2 of the selection (the cheap ones in CPU and RAM); the models close to our own engines are set aside for a later comparison
// (Braids VOWEL / VOWEL_FOF / FM / SAW_SWARM / PARTICLE, Plaits FM / Waveshaping / Particle). Braids runs at our sample rate with its tables
// regenerated (framework/tools/gen_mi_tables.py); its per-sample time constants (kick decay, noise clocks ...) were written for 96 kHz and run slower here.
//
// in 0 pitch CV (like Osc), in 1 gate (strikes the percussive models); mod: pitch (+-pitch_mod), timbre, morph, harm (q15, block rate).
#include "engine/core/module.h"

namespace sc {

enum MiModel : int {
    // Braids, tier 1
    MI_CSAW, MI_MORPH, MI_SAW_SQUARE, MI_SINE_TRI, MI_BUZZ, MI_SQUARE_SUB, MI_SAW_SUB, MI_SQUARE_SYNC, MI_SAW_SYNC, MI_TOY,
    MI_ZLP, MI_ZPK, MI_ZBP, MI_ZHP, MI_VOSIM, MI_FB_FM, MI_CHAOS_FM, MI_KICK, MI_FILT_NOISE, MI_TWIN_PEAKS, MI_CLOCK_NOISE,
    MI_DIGI_MOD, MI_MORSE,
    // Braids, tier 2
    MI_TRI_SAW, MI_TRI_SQUARE, MI_TRI_TRIANGLE, MI_TRI_SINE, MI_RING_MOD, MI_WAVETABLE, MI_WAVE_MAP, MI_WAVE_LINE, MI_GRAIN_CLOUD,
    MI_CYMBAL, MI_SNARE,
    // Plaits, tier 1
    MI_P_NOISE, MI_P_CHIP, MI_P_PHASE_DIST,
    // Plaits, tier 2
    MI_P_VA, MI_P_VA_VCF, MI_P_GRAIN, MI_P_TERRAIN, MI_P_BASS_DRUM, MI_P_BASS_SYN, MI_P_SNARE, MI_P_SNARE_SYN, MI_P_HIHAT, MI_P_HIHAT_2,
    MI_MODELS
};
enum { MI_MODEL, MI_PITCH, MI_TIMBRE, MI_MORPH_P, MI_HARM, MI_LEVEL, MI_PITCH_MOD, MI_N };

constexpr int T_MIOSC = 96;                 // (61..68: the Fx2 effects; the registry refuses a duplicate id silently)

const char *mi_model_name(int model);        // a short name ("CSaw"), for tests and the serial commands
bool mi_model_percussive(int model);         // struck by the gate (decays by itself)

// Renders a model outside the engine, for the UI's preview: `n` samples at `pitch` (1/256 semitone), after `settle` samples (the oscillator's
// start-up); a percussive model is struck at the first sample and settle is ignored. decim > 1 keeps the sample of largest magnitude of every
// `decim` (a long decay in a short buffer). Not for the audio thread (a separate instance; Braids' random generator is shared with the voices,
// which is harmless).
void mi_render_preview(int model, q15 timbre, q15 morph, q15 harm, int32_t pitch, int settle, int decim, q15 *out, int n);
void register_mi_osc(Registry &reg);

#if defined(ENGINE_PROFILE) && defined(ARDUINO_ARCH_ESP32)
extern uint32_t g_mi_prof[MI_MODELS];        // cycles per model since the last print (like g_osc_prof)
#endif

}  // namespace sc
