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
void audioSetShapeOnSynth(SynthShape shape, uint8_t synthCh);  // like audioSetShape but on a specific AMY synth channel
void audioTrackerInit();           // initialize one AMY synth channel per tracker synth track
void audioTrackerNoteOn(uint8_t trackIdx, uint8_t midiNote, float vel);
void audioTrackerNoteOff(uint8_t trackIdx, uint8_t midiNote);
void audioRestoreShapeFilter(SynthShape shape); // restore native filter after FX filter off
void audioGetNativeCutoff(SynthShape shape, float& cc, float& res); // native cutoff/res for a shape (patch-aware)
void audioSetVolume(float vol);
void audioSetFilter(float cutoffHz, float resonance);
void audioSetAllFilters(float cutoffHz, float resonance); // LPF on synth + all sample/seq oscs (0=open)
void audioSetAllFiltersT(float cutoffHz, float resonance, uint8_t filterType); // same but with explicit filter type
void audioSetFilterFreq(float cutoffHz, float resonance); // update cutoff/reso only — no filter_type (no state reset)
void audioSetGranular2FilterFreq(float cutoffHz, float resonance); // smooth update for currently-playing GR2 oscillators
void audioSetPCMFilter(float cutoffHz, float resonance);  // future PCM triggers only (0=off)
void audioSetFmParams(float depth, float cutoffHz, float resonance);
void audioSetFmDepth(float depth);  // feedback only for SHAPE_SAW_FM (ALGO), no filter reset
void audioStopAllSamples();  // velocity=0 on all PCM oscillators (call when leaving sample/seq mode)

// Effects (bus 0)
void audioSetReverb(float level, float liveness, float damping, float xover_hz);
void audioSetChorus(float level, float lfo_freq, float depth);
void audioSetDelay(float level, float delay_ms, float feedback, float filter_coef);
void audioSetOverdrive(float drive);
void audioSetDistortion(float drive, float tone);
void audioSetEq(float low, float mid, float high);  // 3-band EQ: 1.0=flat, >1 boost, <1 cut
void audioSetWavefold(float gain);  // global wavefold on bus 0: 1.0=dry, >1 folds (gain = 1/threshold)
// Global "ladder"-style resonant LPF on bus 0 (FX FILT, Typ=LADDER): a custom 4-pole
// one-pole cascade with tanh-saturated feedback + per-stage saturation, giving a
// nonlinear, self-compressing rolloff — AMY's own filter types are plain linear
// biquads with no such character (see amy.c's bus-0 processing block for the DSP).
// resonance has no hard ceiling (tanh keeps the loop stable at any gain); ~0-6 is
// the useful range, higher self-oscillates harder.
void audioSetLadderFilter(float cutoffHz, float resonance, bool on);

// RINGMOD FX: multiplies bus 0 by a sine carrier. mix 0=dry, 1=fully ring-modulated.
void audioSetRingmod(float freqHz, float mix, bool on);
// COMPRESSOR FX: feedforward peak envelope follower, linked stereo. threshold is
// linear (bus headroom, not dBFS); ratio 1.0=no compression, higher=more limiting.
void audioSetCompressor(float threshold, float ratio, bool on);
// AUTOPAN FX: per-oscillator equal-power pan, one-shot (call every tick to sweep it).
// 0=full left, 0.5=center, 1=full right.
void audioSetPan(float pan);

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
void audioPlayKeyRev(uint8_t keyIdx, float vel); // play reversed sample (SS2 slots only; falls back to fwd)
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

// ==================== STONE (sample tone) ====================
// One SD sample played pitched across the keyboard, like a normal synth voice. Polyphony
// is a small round-robin over fixed, directly-addressed oscillators (not an AMY channel).
// audioStoneInit: reset voice-tracking state (call once on entering MODE_STONE).
// audioLoadStone: background load into the sample slot (aborts any in-flight load, like preview).
// audioIsStoneReady: true once the background load completed successfully.
void audioStoneInit();
void audioLoadStone(const char* path);
bool audioIsStoneReady();
void audioStoneNoteOn(uint8_t note, float velocity);
void audioStoneNoteOff(uint8_t note);
void audioStoneAllNotesOff();
// Advances any releasing STONE voices' software fade-out; call every ~10ms from
// main.cpp's loop() (cheap no-op when nothing is releasing).
void audioStoneFadeTick();
// Waveform (128 peak bins) from the pristine loaded buffer, for OLED display.
bool audioComputeStoneWaveform(uint8_t* waveform128);
// Re-windows the played sample to [startFrac,endFrac] (0..1) of the loaded buffer —
// start/end/position/size editing, granular2-style. Safe to call while notes sound.
// loopMode should match audioStoneSetLoopMode()'s current setting (registers extra
// safety headroom so a live-looping voice doesn't glitch while the window is dragged).
void audioStoneApplyWindow(float startFrac, float endFrac, bool loopMode = false);
// Loop mode: when on, newly-triggered notes loop within the current [start,end]
// window instead of playing through once. Toggle from UI; takes effect on the next
// note-on (does not retroactively change already-sounding voices).
void audioStoneSetLoopMode(bool loop);
bool audioStoneGetLoopMode();

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

// ==================== GRANULAR SLICER ====================
// Flow: audioLoadGranularSource → poll audioIsGranularReady → audioComputeGranularSlices
//       → audioPlayGranularSlice / audioStopGranularOsc → audioUnloadGranular on exit.
//
// Forward slices use pcm_register_extern16 (no copy, points into source buffer).
// Reverse slices are independently allocated (inverted copy).
// All slices are unloaded before source is freed to avoid use-after-free in the audio thread.
void audioLoadGranularSource(const char* path);   // load file into GRANULAR_SOURCE_PRESET
bool audioIsGranularReady();                       // true once source is loaded and slices computed
// Set the sample window [0.0-1.0] used by the next audioComputeGranularSlices call.
// waveform128 always shows the full sample; slices are computed within [startFrac,endFrac].
void audioSetGranularWindow(float startFrac, float endFrac);
// Compute slices from the loaded source. mode=0: 8-slice, mode=1: 1/16 energy-ranked.
// Fills waveform[128] with normalised amplitude (0-255) of FULL source for display.
// Returns actual slice count (8 or 16) or 0 on failure.
uint8_t audioComputeGranularSlices(uint8_t mode, uint8_t* waveform128);
// Re-register all granular presets from (N+1) split points [0.0..1.0] within the window.
// splits[0]=0.0 and splits[N]=1.0 by convention. Call after audioComputeGranularSlices.
// Updates GRANULAR_PRESET_BASE (one-shot), GRANULAR_DBL_PRESET (double), GRANULAR_TAIL_PRESET (tail), GRANULAR_REV_PRESET (reverse).
void audioApplyGranularSplits(float* splits, int N);
void audioPlayGranularSlice(uint8_t keyOscIdx, uint16_t slicePreset, float vel, bool loop);
void audioStopGranularOsc(uint8_t keyOscIdx);
void audioUnloadGranular();  // stop all OSCs, wait for audio thread, free all granular presets

// ==================== GRANULAR2 (multi-sample) ====================
// Fixed x4 layout: 4 samples (TL=S0, TR=S1, BL=S2, BR=S3), 4 slices each, fwd+rev rows.
// playMode: 0=NRM one-shot slice, 1=LOP loop-slice while held, 2=FUL loop full sample while held.
// FUL mode: audioPlayGranular2Ful re-registers GRAN2_TAIL_BASE+sampleIdx at key press as a
// pointer into the existing buffer starting at the slice position (no PSRAM copy), then loops
// [slice→end] while held.  Splits update FWD and REV presets live.
void audioLoadGranular2Source(const char* path, uint8_t sampleIdx);
bool audioIsGranular2Ready(uint8_t sampleIdx);
uint8_t audioComputeGranular2Slices(uint8_t sampleIdx, uint8_t nSlices, uint8_t* waveform128);
void audioApplyGranular2Splits(uint8_t sampleIdx, float* splits, int N, bool lopMode = false);
// amyTime: AMY sysclock timestamp (ms) at which playback should start; UINT32_MAX = immediate.
void audioPlayGranular2(uint8_t oscIdx, uint8_t sampleIdx, uint8_t sliceIdx, bool reverse, float vel, uint8_t playMode = 0, uint16_t attackMs = 5, uint32_t amyTime = UINT32_MAX);
// FUL mode: startFrac = splits[sliceIdx] (fwd) or 1-splits[sliceIdx+1] (rev)
void audioPlayGranular2Ful(uint8_t oscIdx, uint8_t sampleIdx, bool reverse, float vel, float startFrac);
void audioStopGranular2(uint8_t oscIdx);
bool audioGranular2HasReverse(uint8_t sampleIdx);  // false if PSRAM exhausted during load
uint32_t audioGranular2SampleLenMs(uint8_t sampleIdx);  // total sample duration in ms (0 if not loaded)
void audioUnloadGranular2Slot(uint8_t sampleIdx);  // free one slot's presets and PSRAM
void audioUnloadGranular2();

// ==================== TB-303 ENGINE (T303_CH) ====================
// Monophonic synth: SAW/SQR + resonant LPF + filter envelope (EG1) + amp envelope (EG0)
void audioT303Init(float cutoff, float reso, float envMod, float decay, uint8_t amyWave);
void audioT303NoteOn(uint8_t midiNote, float vel);
void audioT303NoteOff(uint8_t midiNote);
void audioT303Params(float cutoff, float reso, float envMod, float decay);  // update synth params live
void audioT303SetAmpEnv(float atkMs, float sus, float relMs);               // no-op for monophonic 303 (uses single EG)
void audioI303SetAmpEnv(float atkMs, float sus, float decMs, float relMs);  // apply amp EG0 to polyphonic I303 voices
void audioT303SetSustain(float sustain);                                    // 0.0=pluck, 1.0=full sustain
void audioT303PitchBend(float ratio);
void audioT303Wave(uint8_t amyWave);                                        // any AMY wave constant
void audioI303Init(float cutoff, float reso, float envMod, float decay, uint8_t amyWave); // polyphonic 303 (6 voices)
void audioT303Feedback(float fb);                                           // no-op (AMY feedback is inactive for simple waveforms)
void audioT303Duty(float duty);                                             // PULSE duty 0.5→0.01 for continuous wave morphing
void audioT303Wavefold(float depth);                                        // wavefolder depth 0→1 symmetric — drives signal into triangle fold (1x→8x)
void audioT303WavefoldAsym(float depth);                                    // wavefolder depth 0→1 positive-only (TRI2: fold peaks, preserve bass)

// ==================== SxF (sous-octaves : SWF/SQF/SNF, wave types pour 303S/I303) ====================
// Base sur T303_CH ; sous-canaux SW2_CH_BASE+0 (f-1, -12 demi-tons) et +1 (f-2, -24 demi-tons).
// P2 0→0.5: a=P2*2 (0→1), b=0  — ajoute f-1
// P2 0.5→1: a=1.0,  b=(P2-0.5)*2 — ajoute f-2 en gardant f-1 à plein
void audioSW2Init(float cutoff, float reso, float decay, uint8_t numVoices, uint8_t wave); // init sous-canaux
void audioSW2Deactivate();                               // all-notes-off + restore T303_CH amp
void audioSW2SetBlend(float p2);                         // update a/b from P2
void audioSW2NoteOn(uint8_t note, float vel);            // trigger sub-channels only
void audioSW2NoteOff(uint8_t note);                      // release sub-channels only
void audioSW2AllNotesOff();                              // flush all sub-channel notes

// ==================== DRUM2 (TR-808 style per-pad control) ====================
// Like audioPlayDrumPad but with per-pad pitch (midiNote) and optional EG decay override.
void audioDrum2Hit(uint8_t padIdx, float vel, uint8_t midiNote, float decayMs);
