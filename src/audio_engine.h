#pragma once
#include "config.h"
#include <AMY-Arduino.h>
#include <SD.h>

extern "C" { amy_err_t esp32_setup_i2s(); }

// Core
void audioInit();
void audioAllNotesOff();
void audioNoteOn(uint8_t note, float velocity);
void audioNoteOff(uint8_t note);
void audioSetEnvelope(const EnvParams &env);
void audioSetPitchBend(float ratio);
void audioSetShape(SynthShape shape);
void audioRestoreShapeFilter(SynthShape shape); // restore native filter after FX filter off
void audioSetVolume(float vol);
void audioSetFilter(float cutoffHz, float resonance);
void audioSetAllFilters(float cutoffHz, float resonance); // LPF on synth + all sample/seq oscs (0=open)
void audioSetFilterFreq(float cutoffHz, float resonance); // update cutoff/reso only — no filter_type (no state reset)
void audioSetPCMFilter(float cutoffHz, float resonance);  // future PCM triggers only (0=off)
void audioSetFmParams(float depth, float cutoffHz, float resonance);
void audioStopAllSamples();  // velocity=0 on all PCM oscillators (call when leaving sample/seq mode)

// Effects (bus 0)
void audioSetReverb(float level, float liveness, float damping, float xover_hz);
void audioSetChorus(float level, float lfo_freq, float depth);
void audioSetDelay(float level, float delay_ms, float feedback, float filter_coef);
void audioSetOverdrive(float drive);
void audioSetEq(float low, float mid, float high);  // 3-band EQ: 1.0=flat, >1 boost, <1 cut

// Preview: aborts any in-progress load, loads file into preview preset and plays.
void audioLoadAndPlay(const char* path, uint16_t preset, float vel);

// Load error codes — set per key when a background load fails.
#define KEY_ERR_NONE    0
#define KEY_ERR_ALLOC   1   // pcm_load / ps_malloc returned NULL → PSRAM exhausted
#define KEY_ERR_FORMAT  2   // unsupported or corrupt file format
#define KEY_ERR_IO      3   // SD open / read failure

// Per-key RAM sample assignment for polyphonic playback.
// audioLoadKey: background load into key's dedicated preset (no autoplay).
// audioPlayKey: trigger playback on key's own oscillator (call after loaded).
// audioKeyLoaded: true once the background load completed successfully.
// audioKeyError: non-zero (KEY_ERR_*) if the last load for this key failed.
void audioLoadKey(const char* path, uint8_t keyIdx);
void audioPlayKey(uint8_t keyIdx, float vel);
void audioStopKey(uint8_t keyIdx);           // send note-off for key's oscillator
void audioSetSampleVolume(float v);          // 0.0–2.0; default 1.0
bool audioKeyLoaded(uint8_t keyIdx);
uint8_t audioKeyError(uint8_t keyIdx);
uint32_t audioKeyLengthMs(uint8_t keyIdx);   // playback duration in ms (0 if unknown)
void audioClearAllKeys();  // abort pending loads and mark all keys unloaded

// Drum pad samples (loaded once at init from .h files)
void audioLoadDrumSamples();
void audioPlayDrumPad(uint8_t col, float vel);
const char* audioDrumPadLabel(uint8_t col);

// Legacy wrappers (delegate to audioLoadAndPlay with vel=0)
bool audioLoadFromSD(const char* path, uint16_t preset);
bool audioLoadWavFromSD(const char* path, uint16_t preset);
bool audioLoadMp3FromSD(const char* path, uint16_t preset);
void audioPlaySamplePreset(uint16_t preset, float vel);
void audioStopSamplePreset(uint16_t preset);

// Returns true when the background load task has finished filling the buffer
bool audioIsStreamingDone();

// Flash PCM cache (~10MB partition, memory-mapped, zero PSRAM for sample playback)
void flashCacheInit();   // called by audioInit — maps pcmcache partition
void flashCacheClear();  // wipe all cached entries (use when sample files change)

extern bool audioReady;

// ==================== TB-303 ENGINE (T303_CH) ====================
// Monophonic synth: SAW/SQR + resonant LPF + filter envelope (EG1) + amp envelope (EG0)
void audioT303Init(float cutoff, float reso, float envMod, float decay, uint8_t amyWave);
void audioT303NoteOn(uint8_t midiNote, float vel);
void audioT303NoteOff(uint8_t midiNote);
void audioT303Params(float cutoff, float reso, float envMod, float decay);  // update synth params live
void audioT303SetAmpEnv(float atkMs, float sus, float relMs);               // no-op for 303 (uses single EG)
void audioT303SetSustain(float sustain);                                    // 0.0=pluck, 1.0=full sustain
void audioT303PitchBend(float ratio);
void audioT303Wave(uint8_t amyWave);                                        // any AMY wave constant
