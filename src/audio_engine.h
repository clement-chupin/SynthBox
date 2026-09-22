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

// ==================== CRUNCH (MODE_CRUNCH) ====================
// MothOS-style 4-track live-record tracker: CRUNCH_TRACKS (4) independent, monophonic
// tracks, each set to one of CRUNCH_INSTR_SLOTS (12) instrument slots — matching
// MothOS's own original structure (see Voice::ReadDrumWaveform/ReadSfxWaveform vs.
// ReadWaveform): slot 0 = "DRUM" bank (12 different drum hits, one per note value, all at
// native pitch), slot 1 = "SFX" bank (same idea), slots 2-6 = 5 melodic instruments
// aliased from the drum-pad bank (BS1/BS2/GTR/SYN/PAD), slots 7-11 = 5 MORE melodic
// instruments using genuine vendored MothOS sample content (not aliased — see
// CRUNCH_INSTR_PRESET_BASE in config.h). Call audioLoadCrunchSamples() after
// audioLoadDrumSamples(), and audioLoadCrunchNativeInstruments() once after that.
// Pattern/transport/track-select state lives in main.cpp
// (crunchTrackInstrument[]/crunchNote[][][]/etc.) — this is just the per-track voice layer.
void audioLoadCrunchSamples();
void audioLoadCrunchNativeInstruments();
void audioCrunchSetTrackInstrument(uint8_t track, uint8_t slot);  // slot: 0-11, see CRUNCH_INSTR_SLOTS
uint8_t audioCrunchGetTrackInstrument(uint8_t track);
void audioCrunchSetTrackDecayMod(uint8_t track, float mod);  // release-time multiplier, 1.0=default 40ms
void audioCrunchSetTrackEnvelope(uint8_t track, uint8_t envIdx);  // 0=FadeOut 1=FadeIn 2=NoFade 3=Loop (MothOS's 'E')
// Resolves (slot, note val 0-11) to a raw kDrumPads[]/CRUNCH_PRESET_BASE sample index —
// only valid for bank slots (0/1) and the 5 aliased melodic slots (2-6); the 5 native
// instrument slots (7-11) have no kDrumPads index, see audioCrunchSlotIsNative() below.
uint8_t audioCrunchResolveSampleIndex(uint8_t slot, uint8_t val0to11);
bool audioCrunchSlotIsBank(uint8_t slot);    // true for slots 0 (DRUM) and 1 (SFX)
bool audioCrunchSlotIsNative(uint8_t slot);  // true for the 5 vendored-instrument slots (7-11)
const char* audioCrunchSlotName(uint8_t slot);  // "DRUM"/"SFX"/sample name — single source of truth for the UI
void audioCrunchNoteOn(uint8_t track, uint8_t val0to11, int8_t octave, float velocity);
void audioCrunchNoteOff(uint8_t track, uint8_t note);
void audioCrunchAllNotesOff();

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

// ==================== DJ (MODE_DJ) ====================
// One mono "deck" for a whole song — scrub/speed/reverse/loop, same 16-bit PSRAM decode
// pipeline as STONE (see config.h's DJ_* block for the capacity tradeoff this implies).
void audioLoadDJTrack(const char* path);
bool audioDJIsLoaded();
// True once EITHER the whole file has finished decoding OR a short prefix is ready — use
// this (not audioDJIsLoaded()) to gate "is the deck usable yet" UI/input, so Play/seek/
// speed become available almost immediately instead of waiting out the full decode.
bool audioDJCanPlayYet();
// Frames decoded so far for the load currently in flight (0 once a new load starts).
uint32_t audioDJGetDecodedFrames();
// Increments once per successful NEW track load (not on every chunk swap — see
// audioDJGetChunkGen() for that) — lets the UI know when the loaded track itself changed.
uint32_t audioDJGetLoadGen();
// Increments on every chunk-boundary swap, seek, or reverse toggle — i.e. whenever the
// currently-displayed ~DJ_CHUNK_SECONDS chunk changes, not just on a brand new track load.
// Use this (not audioDJGetLoadGen()) to know when to recompute the cached waveform.
uint32_t audioDJGetChunkGen();
// Starts/resumes or stops playback from the current position (see audioDJSeek()).
void audioDJPlayPause(bool playing);
bool audioDJIsPlaying();
// Drives chunk-boundary prefetch/handoff — call unconditionally from loop(), regardless
// of whether MODE_DJ is currently on-screen (see its own comment: audio streaming
// shouldn't depend on UI rendering cadence).
void audioDJTick();
// Seeks to a fractional position (0..1) of the estimated whole-track duration and, if
// playing, retriggers from there once the target chunk is decoded in the background (an
// audible retrigger click is expected — this is a real seek, not a smooth scratch; see
// structure/SOFTWARE.md for why AMY has no live-phase API to avoid it).
void audioDJSeek(float posFrac);
// Current playhead position (0..1 of the estimated whole-track duration), continuously
// estimated from elapsed time (AMY has no live phase readback) — good enough for a UI
// playhead/time readout, not sample-accurate.
float audioDJGetPosFrac();
// How far playback is through the CURRENTLY DISPLAYED chunk, in that chunk buffer's own
// natural (already-reversed-if-applicable) reading order — use this for an on-screen
// cursor drawn over audioDJComputeWaveform()'s image, not audioDJGetPosFrac() (which is
// relative to the whole track, not the one chunk actually shown).
float audioDJGetChunkProgressFrac();
bool audioDJChunkTouchesStart();
bool audioDJChunkTouchesEnd();
// Playback rate, pitch coupled (turntable-style, like a real deck's speed/pitch fader) —
// 1.0 = normal, 0.5 = half speed/an octave down, 2.0 = double/an octave up. Updates a
// SOUNDING voice smoothly with no retrigger (AMY's per-voice fractional midi_note).
void audioDJSetSpeed(float speed);
float audioDJGetSpeed();
// Reverse toggle: continues from the current position, streaming chunks backward through
// the file in the background exactly like forward playback streams them ahead (see
// config.h's DJ_* block) — near-unlimited range, not capped to one buffer's worth.
void audioDJSetReverse(bool reverse);
bool audioDJGetReverse();
// True once the deck has ANY chunk loaded and playable — every direction change goes
// through the same background chunk-decode-then-activate path as any other seek now, so
// there's no separate one-time "reverse buffer" build to wait for; kept as its own
// function (rather than folding into audioDJCanPlayYet()) since the UI already
// distinguishes "not ready to play at all" from "reverse specifically isn't set up yet".
bool audioDJIsReverseReady();
// Waveform (128 peak bins) from the CURRENTLY ACTIVE chunk (~DJ_CHUNK_SECONDS), for OLED
// display — mirrors audioComputeStoneWaveform(). Unlike the old one-shot design (which
// held the whole track in RAM and could show all of it), this necessarily shows only the
// active chunk — pair with audioDJGetChunkProgressFrac() for the on-screen cursor, not
// audioDJGetPosFrac().
bool audioDJComputeWaveform(uint8_t* waveform128);
// Estimated whole-track length in seconds (exact for wav, CBR-ratio estimate for mp3).
// 0 if not loaded.
float audioDJGetLengthSeconds();

// ---- Dynamic/experimental performance controls ----
// Manual scratch: jx is the raw joystick X reading, -1..1 (sign = direction, magnitude =
// scrub speed). Call every ~10ms while |jx| exceeds the deadzone; call audioDJScratchEnd()
// once when it returns to center (cheap no-op if no gesture was in progress). Confined to
// whatever's currently resident in RAM (no SD access) — see audio_engine.cpp's comment on
// why a sustained scratch can't range further than that without a real decode.
void audioDJScratchNudge(float jx);
void audioDJScratchEnd();
// Granular spray: amount 0..1, 0 = no grains (default/at rest). Grains are short one-shot
// snippets read from already-resident audio, layered on top of the main deck signal — call
// continuously (every ~10ms) from the joystick Y reading; the actual spawn scheduling runs
// inside audioDJTick().
void audioDJSetGrainAmount(float amount01);
float audioDJGetGrainAmount();
// Stutter/glitch: freezes the playhead and loops a short (BPM-synced) window until released.
// bpmForSync is the app's current global BPM (used only to size the loop window).
void audioDJStutterStart(float bpmForSync);
void audioDJStutterEnd();
bool audioDJIsStuttering();

// ---- USB Mass Storage mode support (MODE_USB, main.cpp) ----
// Nothing may touch SD (background grain decode, the .pcm16 cache builder) while the SD card
// is exposed raw to the USB host — audioDJJobBusy()/audioDJForceQuiesce() let main.cpp poll
// and force-abort that background activity without exposing the file-static state directly.
bool audioDJStreamActive();  // playing or a grain job in flight — used to keep SD polling off the audio path
bool audioDJJobBusy();       // true if a grain-decode or pcm16-cache-build task is in flight
void audioDJCloseFiles();    // closes open cache/tee files; only call with no grain job in flight
void audioDJForceQuiesce();  // bumps the track generation so any in-flight pcm16 build aborts ASAP (does not touch playback state)

// ==================== SS2 large-sample streaming ====================
// SS2 slots (keyIdx 0-15) and MODE_SAMPLE keys share one underlying per-key storage
// (config.h's SS2 comment) and one loader, audioLoadKey() — which now decides on its own
// whether a file fits the normal full-decode-into-PSRAM path or needs this streaming path
// instead (see config.h's "SS2 large-sample streaming" block for the design/rationale).
// Callers don't choose; they just check audioKeyIsLarge() to pick which play function to
// call. Only ONE large key streams at a time — a single shared mono voice, like MODE_DJ's
// one deck — triggering any large key (same or different) cuts whatever it was doing.
bool audioKeyIsLarge(uint8_t keyIdx);
// Triggers/retriggers the shared streaming voice for keyIdx, forward or backward. No-op if
// keyIdx isn't flagged large. Unlike audioPlayKey()/audioPlayKeyRev() (instant, static
// buffer), this cuts any other large key that was streaming and (re)starts fresh — the
// first chunk decodes in the background, so there's a short (bounded) latency before
// sound starts, same tradeoff as MODE_DJ's initial chunk load.
void audioPlayKeyStreamed(uint8_t keyIdx, bool reverse, float vel);
// True while the shared voice is actively streaming THIS key and hasn't reached the true
// end (forward) or true start (backward) of the file yet — use this instead of the
// elapsed-ms-vs-audioKeyLengthMs() comparison for a large key's "still playing" check
// (e.g. SS2's FUL alteration, which doesn't retrigger while true).
bool audioKeyStreamStillPlaying(uint8_t keyIdx);
// Drives the shared streaming voice's chunk-boundary prefetch/handoff — call
// unconditionally from loop(), same reasoning as audioDJTick().
void audioSSTick();

// See audioDJJobBusy()/audioDJForceQuiesce()'s own comment — same purpose, SS2 side.
void audioStreamWorkersInit();  // once at boot, while internal RAM is still unfragmented
bool audioSSStreamActive();
bool audioSSJobBusy();
void audioSSCloseFiles();
void audioSSForceQuiesce();

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

// ==================== MODULAR SYNTH (MODE_MODULAR, wavetable dual-osc) ====================
// Two dedicated dynamic channels (MOD3_OSCA_CH/MOD3_OSCB_CH, config.h) — not part of
// SYNTH_CH — each wave=WAVETABLE. tableIdx (0-4) selects which of the 5 built-in
// wavetables (111/BRAIDS01/PPG_WA00/SINE2SAW/VIRAL); pos01 is the continuous "Serum-style"
// morph position within that table (AMY-native crossfade, see render_wavetable()).
void audioModularOscInit(uint8_t ch, uint8_t tableIdx);
void audioModularSetTable(uint8_t ch, uint8_t tableIdx);  // lighter: preset only, doesn't reset a held note
void audioModularSetWtPos(uint8_t ch, float pos01);
void audioModularNoteOn(uint8_t note, float vel, float detuneSemisB);
void audioModularNoteOff(uint8_t note);
void audioModularAllNotesOff();
void audioModularSetFilter(float cutoffHz, float resonance);
void audioModularSetEnvelope(const EnvParams &env);
void audioModularSetPitchBend(float ratio);
