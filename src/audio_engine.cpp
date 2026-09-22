#include "audio_engine.h"
#include "mp3dec.h"
#include <esp_partition.h>
// int8_t samples (format: static const int8_t name[], size via sizeof)
#include "sounds/kick1.h"
#include "sounds/kick2.h"
#include "sounds/snare1.h"
#include "sounds/snare2.h"
#include "sounds/snare3.h"
#include "sounds/snareB3.h"
#include "sounds/hihat1.h"
#include "sounds/bongo1.h"
// const int samples (format: const int name[], int nameLength — 16-bit values in 32-bit container)
#include "sounds/kick3.h"
#include "sounds/hihat2.h"
#include "sounds/clap1.h"
#include "sounds/crash1.h"
#include "sounds/ride1.h"
#include "sounds/snareB1.h"
#include "sounds/snareB2.h"
#include "sounds/bass1.h"
#include "sounds/bass2.h"
#include "sounds/sfx1.h"
#include "sounds/sfx2.h"
#include "sounds/sfx3.h"
#include "sounds/sfx4.h"
#include "sounds/sfx5.h"
#include "sounds/sfx6.h"
#include "sounds/sfx7.h"
#include "sounds/sfx8.h"
#include "sounds/sfx9.h"
#include "sounds/sfx10.h"
#include "sounds/sfx11.h"
#include "sounds/sfx12.h"
#include "sounds/guitar1.h"
#include "sounds/synth1.h"
#include "sounds/pad1.h"
// int16_t samples, CRUNCH-only (format: const int16_t name[], const uint32_t nameLength) —
// converted from the user's own MothOS_GroovePadBox fork's real instrument1/2/3/7/10.h
#include "sounds/crunchinstr1.h"
#include "sounds/crunchinstr2.h"
#include "sounds/crunchinstr3.h"
#include "sounds/crunchinstr4.h"
#include "sounds/crunchinstr5.h"

bool audioReady = false;

// pcm_load: AMY internal function — allocates a RAM-based preset and returns pointer to fill
extern "C" int16_t* pcm_load(uint16_t preset_number, uint32_t length,
                              uint32_t samplerate, uint8_t channels,
                              uint8_t midinote, uint32_t loopstart, uint32_t loopend);
extern "C" int8_t* pcm_load8(uint16_t preset_number, uint32_t length,
                              uint32_t samplerate, uint8_t channels,
                              uint8_t midinote, uint32_t loopstart, uint32_t loopend);
extern "C" void pcm_unload_all_presets();
extern "C" void pcm_unload_preset(uint16_t preset_number);
extern "C" void pcm_register_extern8(uint16_t preset_number, const int8_t* data,
                                      uint32_t length, uint32_t samplerate,
                                      uint8_t midinote, uint32_t loopstart, uint32_t loopend);
extern "C" void pcm_register_extern16(uint16_t preset_number, const int16_t* data,
                                       uint32_t length, uint32_t samplerate,
                                       uint8_t midinote, uint32_t loopstart, uint32_t loopend);
extern "C" const int16_t* pcm_get_sample_ram_for_preset(uint16_t preset_number, uint32_t* length);

// ---- Non-blocking sample loader: persistent service task + FreeRTOS queue ----
// osc: which AMY oscillator to stop before loading and play on after (if vel>0).
// vel: 0 = load into RAM only, don't trigger playback.
struct LoadReq { char path[256]; uint16_t preset; uint8_t osc; float vel; };
static void amyStopOsc(uint8_t osc);  // forward declaration
static void ssStopStreamVoice();  // forward declaration — defined in the SS2 streaming section further down; stops the shared streaming voice's internal bookkeeping (not just its audio), called from audioStopAllSamples() below

// ==================== DJ (MODE_DJ) state — true streaming circular buffer ====================
// See config.h's DJ_* block for the full rationale: ONE physical ring buffer is registered
// ONCE as one AMY preset with native looping and triggered ONCE per track load — AMY reads it
// forward, physically, forever after that (a retrigger only ever happens for a real
// reposition: load/seek/resume/true EOF-SOF wrap/scratch-release/stutter-release — never for
// an ordinary chunk boundary, and never for a reverse-direction toggle). A background task's
// only job is to keep writing the NEXT bit of audio (in whichever direction is currently
// selected) into the ring just ahead of wherever AMY is currently reading — a classic
// producer/consumer streaming ring, not two named "halves" needing adjacency bookkeeping.
static int16_t* s_djRing       = nullptr;         // s_djRingFrames contiguous samples, one alloc
static uint32_t s_djRingFrames = DJ_RING_FRAMES;  // may shrink once on first load if PSRAM is tight — see the retry loop in audioLoadDJTrack()
// AMY's own native loop period — NOT the same as s_djRingFrames. The ring is registered with
// loopend = s_djRingFrames-1-DJ_WRAP_SAFETY_FRAMES (see DJ_WRAP_SAFETY_FRAMES's own comment),
// so AMY's phase actually cycles through [0, s_djLoopFrames) and wraps back to 0 there, never
// visiting the last DJ_WRAP_SAFETY_FRAMES frames of the physical buffer at all. Every
// computation that estimates or targets a physical ring position (djPhysicalReadIndexNow(),
// the write cursor) MUST wrap modulo THIS, not s_djRingFrames — using the full ring size was a
// real bug: the two moduli disagree by DJ_WRAP_SAFETY_FRAMES on every single lap (once every
// ~s_djLoopFrames/rate seconds, i.e. every few seconds), so the estimated physical read
// position drifted further from AMY's REAL one, without bound, the longer playback continued
// — reported as "ça marche bien au début, puis ça commence à bugger" (fine at first, then
// degrading the longer a track played).
static uint32_t s_djLoopFrames = DJ_RING_FRAMES;

// ---- Write cursor: owned entirely by our own code, never estimated ----
static uint32_t s_djWriteFrame      = 0;      // next physical ring index a grain gets written at
static float    s_djWriteLogicalSec = 0.0f;   // track-time the NEXT grain's decode should start from
// How many seconds of already-written, not-yet-consumed audio currently sit ahead of the read
// cursor — an explicit fill-level counter, NOT derived from comparing s_djWriteFrame against
// the read cursor's ring position. That comparison is fundamentally ambiguous under
// wraparound: if the writer ever genuinely falls behind for long enough that the reader
// physically catches up to and passes it (a real decode-latency stall), "distance from read
// to write, going forward" reads as nearly a FULL ring's worth — indistinguishable from "tons
// of margin" — so the exact underrun this was supposed to catch instead looks perfectly
// healthy, and top-up stops trying to refill at all (silently stuck until the next unrelated
// reposition/toggle happens to reset the cursors). This counter can't have that ambiguity: it
// only ever goes up when a grain actually lands and down at the real playback rate, so zero
// always means zero, regardless of ring wraparound.
static float    s_djValidAheadSec   = 0.0f;
static uint32_t s_djValidAheadLastMs = 0;  // millis() as of the last decay step — reanchored at every reposition/gesture boundary so a frozen interval is never misread as elapsed playback

// ---- Read cursor: still wall-clock ESTIMATED (AMY has no phase-readback API — unchanged
// from before) ----
static float    s_djElapsedSec            = 0.0f;  // playhead, absolute seconds into the track
static uint32_t s_djPlayStartMs           = 0;
static float    s_djElapsedSecAtPlayStart = 0.0f;
// Physical ring index at the s_djPlayStartMs anchor. Physical read index *now* =
// (s_djReadAnchorFrame + framesElapsedSinceAnchor) mod s_djRingFrames — monotonic FORWARD
// through the ring regardless of logical direction (AMY only ever reads the ring physically
// forward; direction is entirely a property of what content the writer put there). A reverse
// toggle deliberately does NOT touch this — the read side is completely unaffected by a
// direction change, which is what makes the toggle retrigger-free.
static uint32_t s_djReadAnchorFrame = 0;
static bool     s_djReverse = false;
static bool     s_djPlaying = false;
// Which DJ_DISPLAY_WINDOW_SEC-long window s_djElapsedSec was in as of the last waveform/
// cursor redraw — see audioDJAdvancePositionEstimate()'s own comment for why the redraw
// must be tied to this crossing rather than to wall-clock time or grain writes.
static int32_t  s_djDisplayWindowIdx = INT32_MIN;
static float    s_djSpeed   = 1.0f;   // 1.0 = normal, pitch-coupled (turntable-style)

// ---- Background grain-decode job (same one-job-at-a-time pattern as before) ----
static volatile bool s_djNextBuilding = false;  // a grain decode task is in flight
static portMUX_TYPE  s_djJobMux = portMUX_INITIALIZER_UNLOCKED;  // guards s_djNextBuilding/s_djJob/s_djWrite*/s_djDecodeGen across the main thread and the grain decode task
static uint32_t       s_djDecodeGen = 0;         // bumped on load/seek/reverse-toggle/resume to invalidate a stale in-flight grain
static bool           s_djEstFromHeader = false;  // s_djEstTotalSec came from a Xing/Info header — exact, don't overwrite with the first-grain guess
static uint32_t       s_djTrackGen = 0;          // bumped ONLY on an actual track load (audioLoadDJTrack) — unlike s_djDecodeGen, seek/scratch/reverse during ordinary playback of the SAME track must NOT invalidate a background pcm16 cache build in progress
// True from the moment a real reposition (load/seek/resume/true EOF-SOF wrap/scratch-end/
// stutter-end) is kicked off until it actually retriggers AMY — audioDJAdvancePositionEstimate()
// must not advance the wall-clock estimate during this window (see its own comment). NOT set
// for an ordinary grain top-up or a reverse toggle — neither of those retriggers, so nothing
// is ever "not audibly playing yet" for them.
static volatile bool s_djAwaitingActivate = false;
// Prefill: after an explicit Play/resume the first grain lands and the ring is armed, but audio only
// STARTS once DJ_PREFILL_SEC of audio is buffered ahead. Starting on the 50ms quick grain means
// the reader immediately outruns the writer (a 0.5s grain takes far more than 50ms to decode on
// the ESP32), so the ring's zeroed tail plays as micro-dropouts until the buffer catches up.
static volatile bool s_djPrefill = false;
static bool           s_djPrefillNext = false;  // set by Play, consumed by the next activate commit
#define DJ_PREFILL_SEC 1.2f

// ---- Scratch (joystick X) ----
// Deliberately PHYSICAL-offset-based, not absolute-logical-time-based — see
// djScratchStepTo()'s own comment for why: converting an arbitrary logical target into a
// physical ring index needs a single fixed anchor (frozen at gesture start) rather than
// per-grain metadata, because ring-physical-distance and file-time-distance are only in a
// fixed 1:1 relationship relative to ONE anchor point, not globally across a possible
// direction-change seam written before the gesture began.
static bool     s_djScratchGestureActive   = false;
static uint32_t s_djScratchAnchorPhys      = 0;      // physical ring index frozen at gesture start
static float    s_djScratchAnchorLogicalSec = 0.0f;  // s_djElapsedSec frozen at the same instant
static bool     s_djScratchDir             = false;  // s_djReverse frozen at gesture start — never flips mid-gesture

// ---- Granular spray (joystick Y) ----
static float    s_djGrainAmount    = 0.0f;   // 0..1, 0 = no grains
static uint32_t s_djGrainNextDueMs = 0;
static uint8_t  s_djGrainRR        = 0;      // round-robin index into the DJ_GRAIN_COUNT pool

// ---- Stutter/glitch (joystick click, hold) ----
static bool  s_djStuttering              = false;
static float s_djStutterFrozenElapsedSec = 0.0f;

// Small scratch pad, sized for one scratch/stutter window — only used when that window would
// wrap past the physical end of the ring (registering a PCM preset needs one contiguous
// pointer; a wrapping window needs a two-piece memcpy into somewhere contiguous first).
static int16_t* s_djWindowPad      = nullptr;
static uint32_t s_djWindowPadFrames = 0;

static char     s_djPath[256]   = {};
static bool     s_djIsMp3       = false;
static bool     s_djLoaded      = false;   // first grain decoded and active
static uint32_t s_djLoadGen     = 0;       // bumped on each successful (re)load — see audioDJGetLoadGen()
static float    s_djEstTotalSec = 0.0f;    // estimated whole-track duration (exact for wav, CBR-ratio estimate for mp3)
static uint32_t s_djChunkGen    = 0;        // bumped periodically so the UI knows to recompute its cached waveform — see audioDJGetChunkGen()

// wav header info, parsed once per track load and reused by every subsequent grain decode
// (avoids re-parsing RIFF chunks on every grain).
static uint32_t s_djWavDataStart = 0, s_djWavBpf = 0, s_djWavFileSR = 0, s_djWavNumCh = 0;
static uint32_t s_djWavBps       = 16;  // actual bit depth (8/16/24/32) — wavReadRaw needs the real value, not a binary float/not-float guess
static bool     s_djWavIsFloat   = false;
static uint32_t s_djWavTotalInFrames = 0;

// Frames decoded so far for the FIRST grain of the load currently in flight — purely a
// "Chargement... Ns" progress readout (see drawScreen()'s MODE_DJ case), not a play-gate.
static volatile uint32_t s_djDecodedFrames = 0;

static QueueHandle_t     s_loadQueue  = NULL;
static volatile bool     s_svcAbort   = false;
static volatile bool     s_svcDone    = true;
static volatile uint8_t  s_currentOsc = 0xFF;  // OSC currently being loaded (0xFF = idle)
// Tracks which key-assigned sample presets have finished loading.
static volatile bool     s_keyLoaded[SAMPLE_KEY_COUNT]    = {};
static uint32_t          s_keyLengthMs[SAMPLE_KEY_COUNT]  = {}; // playback duration per key
static volatile uint8_t  s_keyError[SAMPLE_KEY_COUNT]     = {}; // KEY_ERR_* per key, 0=OK
static bool              s_keyHasRev[SAMPLE_KEY_COUNT]    = {}; // true if reversed preset registered (SS2 slots 0-15)
// True if this key's file was too long for one full decode and streams instead — see
// config.h's "SS2 large-sample streaming" block and audioPlayKeyStreamed() further down.
static bool              s_keyIsLarge[SAMPLE_KEY_COUNT]   = {};
static char              s_keyLargePath[SAMPLE_KEY_COUNT][256] = {};
static uint8_t           s_loadError                      = KEY_ERR_NONE; // set by loaders before return false
static float s_sampleVolume  = 1.0f;  // 0.0–2.0; applied to vel on sample playback
static float   s_pcmLPFCutoff = 0.0f;  // 0 = no filter applied to PCM oscillators
static float   s_pcmLPFReso   = 1.5f;
static uint8_t s_pcmLPFType   = FILTER_NONE;  // type actif : LPF/HPF/BPF/NONE
static bool    s_synthChIsPatch = false; // true when SYNTH_CH has an AMY preset patch (Juno/DX7)
static float   s_patchVolumeScale = 1.0f; // per-patch amplitude compensation (J:ORG is intrinsically loud)
// Set to true when any filter FX writes filter_freq_coefs to SYNTH_CH for a patch.
static bool    s_patchFilterModified = false;
// AMY patch_number currently on SYNTH_CH (only meaningful while s_synthChIsPatch),
// tracked so a filter FX can restore the patch's own native filter later — this is
// the raw AMY patch index (e.g. 7 for J:PNO), NOT the SynthShape enum value.
static int16_t s_currentPatchNumber = -1;

// Native filter parameters for Juno patches (extracted from AMY patches.h v0F/R fields).
// DX7 patches (128+) use FILTER_NONE and are unaffected by coef changes, so they're not listed.
struct PatchFilterNative { int16_t patch; float coef_const; float resonance; };
static const PatchFilterNative kPatchNativeFilter[] = {
    {0,    179.93f, 0.93f  },  // Juno A11 Brass Set 1
    {6,    776.47f, 5.449f },  // Juno A17 Choir
    {7,    993.85f, 0.91f  },  // Juno A18 Piano I    (J:PNO)
    {8,    358.01f, 3.679f },  // Juno A21 Organ I
    {9,    577.55f, 3.679f },  // Juno A22 Organ II
    {21,   1853.8f, 0.95f  },  // Juno A36 String III
    {42,   1427.9f, 2.276f },  // Juno A63 Frontier Organ
};
static const int kPatchNativeFilterCount = (int)(sizeof(kPatchNativeFilter)/sizeof(kPatchNativeFilter[0]));

static void getPatchNativeFilter(int16_t patch, float& cc, float& res) {
    for (int i = 0; i < kPatchNativeFilterCount; i++) {
        if (kPatchNativeFilter[i].patch == patch) {
            cc  = kPatchNativeFilter[i].coef_const;
            res = kPatchNativeFilter[i].resonance;
            return;
        }
    }
    cc = 18000.0f; res = 1.0f;  // DX7/unknown: filter is FILTER_NONE, these values are inert
}

// FILT's Cut param range/curve (see fxList[0] in main.cpp: paramMin=65, paramMax=18000,
// mapped from the pot as mn*powf(mx/mn, potValue)) — needed here to invert cutoffHz back
// to a 0..1 pot position for the remap below.
static const float kFiltCutMin = 65.0f, kFiltCutMax = 18000.0f;

// Remap the FX's raw cutoff/resonance onto the patch's own native filter, continuously
// across the WHOLE pot travel, instead of hard-clamping every value above native down to
// a single flat "native" plateau. An earlier version just did
// `effCutoff = fminf(cutoffHz, nativeCC)`: correct at the very ends (never brighter than
// native at max, full FX control at min) but for any patch whose native cutoff sits well
// below the pot's own 18000Hz ceiling (every entry in kPatchNativeFilter does — as low as
// ~180Hz), that clamp turns the entire upper portion of the pot's travel into a dead zone:
// e.g. for J:PNO (native ~994Hz) a cutoff of 1000Hz and 18000Hz both clamped to the exact
// same 994Hz, making roughly the top HALF of the knob's rotation produce literally no
// audible difference — reported directly ("le son n'est pas modifié entre un cutoff à 1K
// et à 18K"). Instead, re-derive the pot's 0..1 position from cutoffHz (inverting the same
// exponential curve main.cpp uses to compute it) and use that position to sweep smoothly
// across [kFiltCutMin, nativeCC] for cutoff and to blend resonance between the FX's own
// dialed value (pot at minimum — the filter is genuinely doing its own thing) and the
// patch's native resonance (pot at maximum — fully transparent, matching native exactly,
// which is what stops merely activating the FX from changing the patch's texture). Both
// end exactly at their old fixed-point values, so this changes nothing at the pot's
// physical extremes — only restores a meaningful sweep everywhere in between.
static void remapCutoffResToNative(float cutoffHz, float resonance, float nativeCC, float nativeRes,
                                    float& effCutoff, float& effRes) {
    if (nativeCC >= kFiltCutMax) { effCutoff = cutoffHz; effRes = resonance; return; }
    float t = logf(cutoffHz / kFiltCutMin) / logf(kFiltCutMax / kFiltCutMin);
    t = constrain(t, 0.0f, 1.0f);
    effCutoff = kFiltCutMin * powf(nativeCC / kFiltCutMin, t);
    effRes    = resonance + t * (nativeRes - resonance);
}
static volatile bool s_granularLoaded = false;  // set when GRANULAR_SOURCE_PRESET load completes
static uint8_t s_granLastSliceCount = 0;  // stored by audioComputeGranularSlices

// Granular2 per-sample state
static volatile bool s_gran2Loaded[GRAN2_MAX_SAMPLES]   = {};
// Full reversed copy, sliced into rev presets. Stored at 8-bit (half the PSRAM of the
// 16-bit source) so long samples can still fit a reverse copy on real hardware's ~8MB PSRAM.
static int8_t*        s_gran2FullRevBuf[GRAN2_MAX_SAMPLES] = {};
static uint32_t      s_gran2FullRevLen[GRAN2_MAX_SAMPLES] = {};
static uint8_t       s_gran2NSlices[GRAN2_MAX_SAMPLES]    = {};  // slice count from the last compute, for lazy reverse build
static uint32_t      s_gran2ActiveOscMask = 0;  // bitmask: bit i set → oscIdx i is playing
// GRAN2_TAIL_BASE+si: full reversed buffer preset for FUL reverse mode (registered at compute time).
void bgServiceTask(void* param);  // forward declaration

// ==================== INIT ====================
void audioInit() {
    amy_config_t cfg = amy_default_config();
    cfg.features.startup_bleep = 0;
    cfg.features.default_synths = 0;
    // Emscripten WASM has no pthreads (no -pthread flag); single-threaded mode.
    // SDL simulator is also single-threaded for AMY purposes.
#ifdef __EMSCRIPTEN__
    cfg.platform.multithread = 0;
    cfg.platform.multicore = 0;
#else
    cfg.platform.multithread = 1;
    cfg.platform.multicore = 1;
#endif
    cfg.features.audio_in = 0;
#ifdef SIMULATOR
    cfg.audio = AMY_AUDIO_IS_MINIAUDIO;
#else
    cfg.audio = AMY_AUDIO_IS_I2S;
    cfg.i2s_mclk = 9;
    cfg.i2s_bclk = 9;
    cfg.i2s_lrc = 7;
    cfg.i2s_dout = 8;
#endif
    // No file hooks — we load samples synchronously into RAM instead
    amy_start(cfg);
#ifndef SIMULATOR
    esp32_setup_i2s();
#endif
    delay(500);

    // Setup synth voices — no filter here; filter is owned entirely by the FX system.
    // audioInit-applied filters would be invisible to applyAllFx() and cause a volume jump
    // the first time any FX is activated (applyAllFx resets inactive LPF to FILTER_NONE).
    {
        amy_event e = amy_default_event();
        e.synth = SYNTH_CH;
        e.num_voices = NUM_SYNTH_VOICES;
        e.oscs_per_voice = OSCS_PER_VOICE;
        e.wave = SAW_DOWN;
        amy_add_event(&e);
    }
    // Explicitly set FILTER_NONE so AMY and FX system share the same initial state
    {
        amy_event e = amy_default_event();
        e.synth = SYNTH_CH;
        e.filter_type = FILTER_NONE;
        amy_add_event(&e);
    }

    flashCacheInit();  // map pcmcache partition before the service task starts

    s_loadQueue = xQueueCreate(33, sizeof(LoadReq));  // 32 key slots + 1 preview front-insert slot
    xTaskCreatePinnedToCore(bgServiceTask, "audioSvc", 16384, NULL, 2, NULL, 0);

    audioReady = true;
    audioLoadDrumSamples();
    audioLoadCrunchSamples();  // aliases the drum-pad buffers just loaded above — must run after
    audioLoadCrunchNativeInstruments();
    Serial.printf("Audio OK: AMY_SAMPLE_RATE=%u AMY_BLOCK_SIZE=%u\n",
                  (uint32_t)AMY_SAMPLE_RATE, (uint32_t)AMY_BLOCK_SIZE);
}

// ==================== NOTE CONTROL ====================
void audioAllNotesOff() {
    {
        // velocity=0 with midi_note/preset both unset is AMY's dedicated "all notes
        // off" event (patches.c: patches_voices_for_note_onoff_event() routes it to
        // instrument_all_notes_off(), which only touches voices that are actually
        // active) — unlike sweeping all 128 MIDI notes individually, which sent a
        // note-off for ~120 notes that were never on and spammed the terminal with
        // "note off for X/Y does not match note on" for each one.
        amy_event e = amy_default_event();
        e.synth = SYNTH_CH; e.velocity = 0;
        amy_add_event(&e);
    }
    for (int o = AMY_OSC_STRUM; o < AMY_OSC_STRUM + 8; o++) {
        amy_event e = amy_default_event();
        e.osc = o; e.velocity = 0;
        amy_add_event(&e);
    }
    {
        amy_event e = amy_default_event();
        e.osc = PCM_PREVIEW_OSC; e.velocity = 0;
        amy_add_event(&e);
    }
    amy_event e = amy_default_event();
    e.synth = SYNTH_CH; e.pitch_bend = 1.0f;
    amy_add_event(&e);
}

void audioStopAllSamples() {
    for (int i = 0; i < SAMPLE_KEY_COUNT; i++) {
        amy_event e = amy_default_event();
        e.osc = (uint16_t)(SAMPLE_OSC_BASE + i); e.velocity = 0;
        amy_add_event(&e);
    }
    // SS_OSC (the shared large-sample streaming voice) is a separate fixed oscillator, not
    // in the SAMPLE_OSC_BASE range above — silencing it here too, AND stopping its internal
    // position/prefetch bookkeeping (not just the audio), so it doesn't keep decoding chunks
    // in the background forever after a "stop all" (confirmed during testing: without this,
    // audioSSTick() kept ticking/prefetching indefinitely since s_ssPlaying never noticed
    // the voice had been silenced by anything other than its own trigger/chunk-boundary code).
    ssStopStreamVoice();
}

// config_eq uses SAMPLE (s8.23 fixed-point int32_t) for its gain params — NOT float.
// Passing floats with the wrong prototype would deliver their IEEE-754 bit patterns
// (e.g. 1.0f → 0x3F800000 ≈ 127× gain in s8.23) causing a permanent ×100 volume boost.
extern "C" void config_eq(uint8_t bus, SAMPLE eq_l, SAMPLE eq_m, SAMPLE eq_h);

void audioSetEq(float low, float mid, float high) {
    if (!audioReady) return;
    config_eq(0, F2S(low), F2S(mid), F2S(high));
}

static bool s_noiseShape  = false;  // true when current synth shape is a noise type (pitch-independent)
static bool s_t303Noise   = false;  // true when T303_CH is set to NOISE wave

void audioNoteOn(uint8_t note, float velocity) {
    if (!audioReady) return;
    amy_event e = amy_default_event();
    e.synth = SYNTH_CH;
    e.midi_note = s_noiseShape ? 60 : note;  // noise: fixed pitch so spectrum stays constant
    e.velocity = velocity * s_patchVolumeScale;
    amy_add_event(&e);
}

void audioNoteOff(uint8_t note) {
    if (!audioReady) return;
    amy_event e = amy_default_event();
    e.synth = SYNTH_CH;
    e.midi_note = s_noiseShape ? 60 : note;
    e.velocity = 0;
    amy_add_event(&e);
}

// ==================== STONE (sample tone) ====================
// One SD sample loaded into a fixed preset slot, played pitched across the keyboard.
// AMY computes playback speed from (event.midi_note - preset.midinote); the preset is
// always registered at midinote=69 (native rate at A4), so e.midi_note=note directly
// gives the classic sampler root-key pitch mapping.
// Polyphony is a small self-managed round-robin over STONE_OSC_BASE..+STONE_VOICES-1,
// each addressed directly (e.osc=...), matching audioPlayKey's proven-safe pattern —
// NOT an AMY multi-voice channel (e.synth=N), whose dynamic voice allocator can land on
// and corrupt the fixed oscillator ranges other subsystems address directly.
static volatile bool s_stoneLoaded = false;
static uint8_t       s_stoneVoiceNote[STONE_VOICES] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};  // MIDI note per voice osc, 0xFF = free
static uint8_t       s_stoneVoiceRR = 0;              // round-robin steal cursor

// Software release fade (audioStoneFadeTick(), called every ~10ms from main.cpp's
// loop()). feedback stays 0 so pcm_note_off()'s hard phase-jump-to-end keeps
// happening within ~1 audio block (see audioStoneNoteOn() below for why that
// matters for audioLoadStone()'s reload safety) — but instead of firing that
// note-off the instant the key is released, we ramp e.amp_coefs[COEF_CONST]
// (a documented per-oscillator overall-gain control, independent of the note
// gate) down to ~0 first, so by the time the real note-off's click happens the
// voice is already inaudible.
#define STONE_FADE_MS 40
static bool          s_stoneFading[STONE_VOICES]    = {};
static unsigned long s_stoneFadeStartMs[STONE_VOICES] = {};

// Loop mode: when on, newly-triggered notes loop within STONE_PRESET's current
// [start,end] window (audioStoneApplyWindow()) instead of playing through once.
static bool s_stoneLoop = false;
void audioStoneSetLoopMode(bool loop) { s_stoneLoop = loop; }
bool audioStoneGetLoopMode() { return s_stoneLoop; }

void audioStoneInit() {
    memset(s_stoneVoiceNote, 0xFF, sizeof(s_stoneVoiceNote));
    memset(s_stoneFading, 0, sizeof(s_stoneFading));
    s_stoneVoiceRR = 0;
}

// Advances any releasing STONE voices' fade and finalizes the real note-off once
// each one reaches silence. Cheap no-op when nothing is releasing.
void audioStoneFadeTick() {
    if (!audioReady) return;
    unsigned long now = millis();
    for (uint8_t i = 0; i < STONE_VOICES; i++) {
        if (!s_stoneFading[i]) continue;
        unsigned long elapsed = now - s_stoneFadeStartMs[i];
        amy_event e = amy_default_event();
        e.osc = (uint16_t)(STONE_OSC_BASE + i);
        if (elapsed >= STONE_FADE_MS) {
            s_stoneFading[i] = false;
            e.velocity = 0;  // real note-off now that the voice is already faded down
            amy_add_event(&e);
            // Loop mode: pcm_note_off() only disables looping on the FIRST note-off and
            // needs a second to force an immediate stop (same fix as
            // audioStoneAllNotesOff()) — without this, a released looping voice just
            // keeps playing forward through the rest of the (loop-extended) registered
            // buffer at whatever near-silent level the fade left it at, which can take
            // several audible seconds for a long sample. Harmless no-op for a non-
            // looping voice (already stopped by the first event).
            amy_event e2 = amy_default_event();
            e2.osc = (uint16_t)(STONE_OSC_BASE + i);
            e2.velocity = 0;
            amy_add_event(&e2);
        } else {
            e.amp_coefs[COEF_CONST] = 1.0f - (float)elapsed / (float)STONE_FADE_MS;
            amy_add_event(&e);
        }
    }
}

void audioLoadStone(const char* path) {
    if (!audioReady || !s_loadQueue) return;
    // Reloading STONE_SOURCE_PRESET frees its old buffer (pcm_load/pcm_load8 call
    // pcm_unload_preset on the existing slot before allocating the new one,
    // pcm.c:411/447) on Core 0, while Core 1's audio render may still be reading
    // STONE_PRESET's current window into that same buffer for any still-active
    // voice — force them off first so none are left referencing it when the
    // reload runs.
    audioStoneAllNotesOff();
    LoadReq req;
    strncpy(req.path, path, sizeof(req.path) - 1);
    req.path[sizeof(req.path) - 1] = '\0';
    req.preset = STONE_SOURCE_PRESET;
    req.osc    = STONE_OSC_BASE;
    req.vel    = 0.0f;
    s_stoneLoaded = false;
    // Abort any in-flight load for instant response while scrubbing through a folder with P2.
    s_svcAbort = true;
    xQueueSendToFront(s_loadQueue, &req, 0);
}

bool audioIsStoneReady() { return s_stoneLoaded; }
// audioComputeStoneWaveform()/audioStoneApplyWindow() are defined further down,
// next to audioComputeGranular2Slices()/audioApplyGranular2Splits() (same
// in-place pcm_register_extern16 re-windowing mechanism) — after PCM_TARGET_RATE
// is #defined, which they need.

void audioStoneNoteOn(uint8_t note, float velocity) {
    if (!audioReady) return;
    // Pick a free voice, or round-robin steal the oldest one if all are busy.
    uint8_t voiceIdx = STONE_VOICES;
    for (uint8_t i = 0; i < STONE_VOICES; i++) {
        if (s_stoneVoiceNote[i] == 0xFF) { voiceIdx = i; break; }
    }
    if (voiceIdx == STONE_VOICES) {
        voiceIdx = s_stoneVoiceRR;
        s_stoneVoiceRR = (uint8_t)((s_stoneVoiceRR + 1) % STONE_VOICES);
    }
    s_stoneVoiceNote[voiceIdx] = note;
    s_stoneFading[voiceIdx] = false;  // cancel any release fade this voice was mid-way through

    amy_event e = amy_default_event();
    e.osc       = (uint16_t)(STONE_OSC_BASE + voiceIdx);
    e.wave      = PCM;
    e.preset    = STONE_PRESET;
    e.midi_note = note;
    e.velocity  = velocity * s_sampleVolume;
    e.amp_coefs[COEF_CONST] = 1.0f;  // full volume, in case this osc was still mid-fade
    // 5ms fade-in to suppress click; 40ms release for smooth stop on note-off
    e.eg0_times[0] = 5;   e.eg0_values[0] = 1.0f;
    e.eg0_times[1] = 0;   e.eg0_values[1] = 1.0f;
    e.eg0_times[2] = 40;  e.eg0_values[2] = 0.0f;
    // feedback=0 (one-shot): pcm_note_off() hard-jumps phase to end-of-sample on
    // note-off (pcm.c:187-194), which bypasses the eg0 release ramp above and would
    // click — but it also makes the oscillator stop touching the sample buffer
    // within ~1 audio block of note-off (render_pcm sees base_index >= sample_length
    // next block and sets status=SYNTH_OFF, pcm.c:259-262). That matters because
    // audioLoadStone()'s reload frees/reallocates this same buffer on the other
    // core — feedback!=0 (loop) avoided the click by letting a released voice keep
    // playing to its natural end instead, but that left it reading the buffer for
    // however long the sample had left, and crashed on real hardware from exactly
    // that race (Guru Meditation LoadProhibited) BEFORE audioStoneAllNotesOff() was
    // made to send two note-offs per voice (see there) — pcm_note_off() only
    // disables looping on the first note-off and needs a second to force an
    // immediate stop (pcm.c:187-194 comment), so a single all-notes-off used to
    // leave a looping voice reading the buffer a bit longer. With that fixed,
    // feedback can safely follow the user's loop-mode toggle (audioStoneSetLoopMode()):
    // one-shot still gets the fast bounded stop (feedback=0, click masked by
    // audioStoneFadeTick()'s amp_coefs ramp — see audioStoneNoteOff()); loop mode
    // gets real looping within STONE_PRESET's current [start,end] window, and is
    // just as reload-safe since any reload/window-change first forces every voice
    // off via the double note-off.
    e.feedback = s_stoneLoop ? 1.0f : 0.0f;
    amy_add_event(&e);
}

void audioStoneNoteOff(uint8_t note) {
    if (!audioReady) return;
    for (uint8_t i = 0; i < STONE_VOICES; i++) {
        if (s_stoneVoiceNote[i] != note) continue;
        s_stoneVoiceNote[i] = 0xFF;
        // Start a short software fade (audioStoneFadeTick()) instead of firing the
        // real note-off immediately — masks pcm_note_off()'s click by the time it
        // actually fires, without changing how fast the voice stops touching the
        // sample buffer (see audioStoneNoteOn()'s feedback=0 comment).
        s_stoneFading[i] = true;
        s_stoneFadeStartMs[i] = millis();
    }
}

void audioStoneAllNotesOff() {
    if (!audioReady) return;
    for (uint8_t i = 0; i < STONE_VOICES; i++) {
        s_stoneVoiceNote[i] = 0xFF;
        s_stoneFading[i] = false;  // panic/reload stop: instant, no fade
        // A looping voice (loop mode on) only disables its loop on the FIRST
        // note-off and keeps playing to the natural end of the buffer — pcm.c's
        // pcm_note_off() itself: "sending a second note-off will stop it
        // immediately". This function is the reload-safety guard audioLoadStone()
        // calls before freeing/reallocating the buffer, so it must guarantee an
        // IMMEDIATE stop for every voice regardless of loop state — send it twice.
        for (uint8_t rep = 0; rep < 2; rep++) {
            amy_event e = amy_default_event();
            e.osc = (uint16_t)(STONE_OSC_BASE + i);
            e.velocity = 0;
            amy_add_event(&e);
        }
    }
}

// ==================== CRUNCH (MODE_CRUNCH) ====================
// MothOS-style 4-track live-record tracker (main.cpp owns the pattern data/transport;
// this is just the per-track voice layer). Each track is a FIXED, MONOPHONIC oscillator
// (CRUNCH_OSC_BASE+track, mirrors MothOS's own Voice[4] — one voice per track, a new
// note-on on a track that's already sounding just retriggers that track's own
// oscillator, no cross-track stealing) playing whichever of the 32 aliased drum-bank
// samples that track currently has selected. Sample aliasing itself
// (audioLoadCrunchSamples()) is unchanged from the original single-instrument CRUNCH —
// zero new sample data, registers CRUNCH_PRESET_BASE+i as a second AMY preset pointing
// at the SAME PSRAM buffer audioLoadDrumSamples() already populated at
// DRUM_PRESET_BASE+i (see pcm_get_sample_ram_for_preset()).
// index → sample name, see kDrumPads[] below: 15=BS1, 16=BS2, 29=GTR, 30=SYN, 31=PAD.
static const bool kCrunchLoop[CRUNCH_SAMPLE_COUNT] = {
    false,false,false,false,false,false,false,false,  // KK1 KK2 SN1 SN2 SN3 HH1 BNG SNB
    false,false,false,false,false,false,false,true,   // KK3 HH2 CLP CRS RDE SB1 SB2 BS1
    true, false,false,false,false,false,false,false,  // BS2 SX1 SX2 SX3 SX4 SX5 SX6 SX7
    false,false,false,false,false,true, true, true,   // SX8 SX9 S10 S11 S12 GTR SYN PAD
};
// Instrument slot 0 ("DRUM" bank): 12 note keys = 12 different drum hits, curated from
// kDrumPads[] for variety (kick/snare/hat/clap/bongo/ride), all at native pitch.
static const uint8_t kCrunchDrumBank[12] = { 0, 2, 5, 10, 1, 3, 9, 6, 8, 4, 7, 12 };
// Slot 1 ("SFX" bank): kDrumPads[] SX1-SX12 are already exactly 12 samples — use them all.
static const uint8_t kCrunchSfxBank[12]  = { 17,18,19,20,21,22,23,24,25,26,27,28 };
// Slots 2-6 (5 melodic instruments): the 5 real melodic kDrumPads[] samples (BS1/BS2/
// GTR/SYN/PAD) — aliased, zero extra flash, same as the bank slots above.
static const uint8_t kCrunchMelodic[5]  = { 15, 16, 29, 30, 31 };
// Slots 7-11 (5 more melodic instruments): genuine MothOS instrument samples, vendored
// directly (not aliased — see CRUNCH_INSTR_PRESET_BASE's own comment in config.h).
struct CrunchInstrSample { const int16_t* data; uint32_t len; const char* label; };
static const CrunchInstrSample kCrunchNativeInstr[CRUNCH_INSTR_SAMPLE_COUNT] = {
    { crunchinstr1, crunchinstr1Length, "INS1" },
    { crunchinstr2, crunchinstr2Length, "INS2" },
    { crunchinstr3, crunchinstr3Length, "INS3" },
    { crunchinstr4, crunchinstr4Length, "INS4" },
    { crunchinstr5, crunchinstr5Length, "INS5" },
};

static uint8_t s_crunchTrackInstrument[CRUNCH_TRACKS] = {0,1,2,3};  // instrument SLOT, 0-11 (see CRUNCH_INSTR_SLOTS)

// Resolves (slot, note val 0-11) to a raw kDrumPads[]/CRUNCH_PRESET_BASE index — used for
// bank slots (0/1, where val always maps to a kDrumPads-aliased sample) and for the
// per-track VU row's "which drum was hit" display. NOT valid for the 5 native-instrument
// melodic slots (7-11) — those live in a separate preset range, see
// audioCrunchSlotIsNative()/audioCrunchNoteOn() below.
uint8_t audioCrunchResolveSampleIndex(uint8_t slot, uint8_t val0to11) {
    val0to11 = (uint8_t)(val0to11 % 12);
    if (slot == 0) return kCrunchDrumBank[val0to11];
    if (slot == 1) return kCrunchSfxBank[val0to11];
    uint8_t m = (uint8_t)((slot - 2) % 10);
    return (m < 5) ? kCrunchMelodic[m] : 0;  // native slots have no kDrumPads index
}
bool audioCrunchSlotIsBank(uint8_t slot) { return slot == 0 || slot == 1; }
// True for the 5 melodic slots (7-11) backed by a genuine vendored sample instead of an
// aliased kDrumPads one.
bool audioCrunchSlotIsNative(uint8_t slot) {
    if (slot < 2) return false;
    return ((slot - 2) % 10) >= 5;
}
// Display name for any slot — the single source of truth so the header/legend/browser
// can't drift out of sync with what audioCrunchNoteOn() actually plays.
const char* audioCrunchSlotName(uint8_t slot) {
    if (slot == 0) return "DRUM";
    if (slot == 1) return "SFX";
    uint8_t m = (uint8_t)((slot - 2) % 10);
    if (m < 5) return audioDrumPadLabel(kCrunchMelodic[m]);
    return kCrunchNativeInstr[m - 5].label;
}
static uint8_t s_crunchTrackNote[CRUNCH_TRACKS];        // 0xFF = that track's voice is idle
static float   s_crunchTrackDecayMod[CRUNCH_TRACKS] = {1.0f,1.0f,1.0f,1.0f};
// MothOS's own 4-choice envelope selector ('E' command): 0=Fade Out (percussive decay,
// the original default shape), 1=Fade In, 2=No Fade (sustained plateau), 3=Loop (forces
// looping regardless of kCrunchLoop[]'s per-sample default) — approximated with AMY's
// existing eg0 envelope + feedback fields rather than new DSP.
static uint8_t s_crunchTrackEnv[CRUNCH_TRACKS] = {0,0,0,0};

void audioLoadCrunchSamples() {
    if (!audioReady) return;
    memset(s_crunchTrackNote, 0xFF, sizeof(s_crunchTrackNote));
    for (uint8_t i = 0; i < CRUNCH_SAMPLE_COUNT; i++) {
        uint32_t len = 0;
        const int16_t* buf = pcm_get_sample_ram_for_preset((uint16_t)(DRUM_PRESET_BASE + i), &len);
        if (!buf || !len) { Serial.printf("CRUNCH: no source buffer for pad %u\n", i); continue; }
        // loopend=0 means "loop across the whole buffer" (pcm_register_extern16's own
        // convention) — actual one-shot-vs-loop behavior is chosen per note-on below via
        // e.feedback, exactly like STONE's own loop-mode toggle.
        pcm_register_extern16((uint16_t)(CRUNCH_PRESET_BASE + i), buf, len, DRUM_SAMPLERATE, 69, 0, 0);
    }
}

// The 5 genuine MothOS instrument samples (kCrunchNativeInstr[]) — unlike
// audioLoadCrunchSamples() above, these are NOT aliased (the audio data doesn't exist
// anywhere else in GrvEP), so this copies them into a fresh PSRAM buffer via pcm_load(),
// same established pattern as audioLoadDrumSamples() — just a direct int16_t->int16_t
// copy since the source is already the right format (no int8/int32-container conversion
// needed, see sounds/crunchinstr*.h).
void audioLoadCrunchNativeInstruments() {
    if (!audioReady) return;
    for (uint8_t i = 0; i < CRUNCH_INSTR_SAMPLE_COUNT; i++) {
        const CrunchInstrSample& s = kCrunchNativeInstr[i];
        int16_t* buf = pcm_load((uint16_t)(CRUNCH_INSTR_PRESET_BASE + i), s.len, DRUM_SAMPLERATE, 1, 69, 0, 0);
        if (!buf) { Serial.printf("CRUNCH: alloc fail native instr %u (%s)\n", i, s.label); continue; }
        memcpy(buf, s.data, s.len * sizeof(int16_t));
    }
}

void audioCrunchSetTrackInstrument(uint8_t track, uint8_t slot) {
    if (track >= CRUNCH_TRACKS || slot >= CRUNCH_INSTR_SLOTS) return;
    s_crunchTrackInstrument[track] = slot;
}
uint8_t audioCrunchGetTrackInstrument(uint8_t track) {
    if (track >= CRUNCH_TRACKS) return 0;
    return s_crunchTrackInstrument[track];
}
void audioCrunchSetTrackDecayMod(uint8_t track, float mod) {
    if (track >= CRUNCH_TRACKS) return;
    s_crunchTrackDecayMod[track] = constrain(mod, 0.1f, 8.0f);
}
void audioCrunchSetTrackEnvelope(uint8_t track, uint8_t envIdx) {
    if (track >= CRUNCH_TRACKS) return;
    s_crunchTrackEnv[track] = (uint8_t)(envIdx % 4);
}

void audioCrunchNoteOn(uint8_t track, uint8_t val0to11, int8_t octave, float velocity) {
    if (!audioReady || track >= CRUNCH_TRACKS) return;
    uint8_t slot = s_crunchTrackInstrument[track];
    bool isBank = audioCrunchSlotIsBank(slot);
    bool isNative = audioCrunchSlotIsNative(slot);
    uint16_t preset;
    bool loops;
    if (isNative) {
        uint8_t ni = (uint8_t)(((slot - 2) % 10) - 5);
        preset = (uint16_t)(CRUNCH_INSTR_PRESET_BASE + ni);
        loops = true;  // genuine instrument samples — sustained/looping like the other melodic slots
    } else {
        uint8_t sampleIdx = audioCrunchResolveSampleIndex(slot, val0to11);
        preset = (uint16_t)(CRUNCH_PRESET_BASE + sampleIdx);
        loops = kCrunchLoop[sampleIdx];
    }
    // Bank slots (DRUM/SFX): val0to11 picks WHICH sample, always played at its native
    // pitch — matches Voice::ReadDrumWaveform/ReadSfxWaveform (12 fixed hits, no pitch
    // shift). Melodic slots (aliased or native): val0to11 pitches the SAME sample
    // chromatically — matches Voice::ReadWaveform's GetBaseFreq(note, octave).
    uint8_t midiNote = isBank ? 69 : (uint8_t)constrain(60 + (int)octave * 12 + (int)val0to11, 0, 127);
    s_crunchTrackNote[track] = midiNote;
    float decayMs = 40.0f * s_crunchTrackDecayMod[track];

    amy_event e = amy_default_event();
    e.osc       = (uint16_t)(CRUNCH_OSC_BASE + track);
    e.wave      = PCM;
    e.preset    = preset;
    e.midi_note = midiNote;
    e.velocity  = velocity * s_sampleVolume;
    e.amp_coefs[COEF_CONST] = 1.0f;
    switch (s_crunchTrackEnv[track]) {
        case 1:  // Fade In: ramp up over decayMs, then a quick tail
            e.eg0_times[0] = decayMs; e.eg0_values[0] = 1.0f;
            e.eg0_times[1] = 0;       e.eg0_values[1] = 1.0f;
            e.eg0_times[2] = 5;       e.eg0_values[2] = 0.0f;
            break;
        case 2:  // No Fade: quick attack, long sustained plateau
            e.eg0_times[0] = 5;              e.eg0_values[0] = 1.0f;
            e.eg0_times[1] = decayMs * 5.0f; e.eg0_values[1] = 1.0f;
            e.eg0_times[2] = 40;             e.eg0_values[2] = 0.0f;
            break;
        default:  // 0 (Fade Out) and 3 (Loop) share the same percussive-decay shape;
                   // Loop's only difference is e.feedback below.
            e.eg0_times[0] = 5;  e.eg0_values[0] = 1.0f;
            e.eg0_times[1] = 0;  e.eg0_values[1] = 1.0f;
            e.eg0_times[2] = decayMs; e.eg0_values[2] = 0.0f;
            break;
    }
    // 5ms (or shape-specific) fade-in suppresses click; loop/one-shot decided by
    // e.feedback below, so a released melodic note still rings out its natural
    // (short, ~1-2s) tail rather than hard-cutting — see pcm_note_off()'s own
    // "plays through to end" comment.
    e.feedback = (s_crunchTrackEnv[track] == 3 || loops) ? 1.0f : 0.0f;
    if (s_pcmLPFCutoff > 10.0f) { e.filter_type = s_pcmLPFType; e.filter_freq_coefs[COEF_CONST] = s_pcmLPFCutoff; e.resonance = s_pcmLPFReso; }
    amy_add_event(&e);
}

void audioCrunchNoteOff(uint8_t track, uint8_t note) {
    if (!audioReady || track >= CRUNCH_TRACKS) return;
    if (s_crunchTrackNote[track] != note) return;
    s_crunchTrackNote[track] = 0xFF;
    amy_event e = amy_default_event();
    e.osc = (uint16_t)(CRUNCH_OSC_BASE + track);
    e.velocity = 0;
    amy_add_event(&e);
}

void audioCrunchAllNotesOff() {
    if (!audioReady) return;
    for (uint8_t i = 0; i < CRUNCH_TRACKS; i++) {
        s_crunchTrackNote[i] = 0xFF;
        for (uint8_t rep = 0; rep < 2; rep++) {
            amy_event e = amy_default_event();
            e.osc = (uint16_t)(CRUNCH_OSC_BASE + i);
            e.velocity = 0;
            amy_add_event(&e);
        }
    }
}

// ==================== SYNTH PARAMS ====================
void audioSetFilter(float cutoffHz, float resonance) {
    if (!audioReady) return;
    amy_event e = amy_default_event();
    e.synth = SYNTH_CH;
    e.filter_type = FILTER_LPF;
    e.filter_freq_coefs[COEF_CONST] = cutoffHz;
    e.resonance = resonance;
    amy_add_event(&e);
}

// Apply LPF to synth channel; update state so future audioPlayKey/amyPlayPcm triggers carry it.
// Pass cutoffHz=0 to bypass (FILTER_NONE on the synth channel, clears PCM filter state).
// Do NOT send per-osc events — PCM oscillators mishandle filter_freq_coefs and explode in volume.
// When active (non-bypass), zero COEF_EG0/EG1 so shape-internal filter envelopes (HOOVER etc.)
// cannot fight the FX cutoff. audioRestoreShapeFilter() reverts this when the FX turns off.
// STONE plays on its own fixed round-robin oscillator range (STONE_OSC_BASE..+
// STONE_VOICES-1, config.h) — like GRANULAR2, it is NOT part of SYNTH_CH, so neither
// audioSetAllFilters() nor audioSetAllFiltersT() ever reached it: toggling FILT
// (LPF/HPF/BPF) while playing a STONE sample did nothing audible, even though
// MODE_STONE can open OVERLAY_FX and toggle it. Unlike GRANULAR2's large 32-osc pool
// (which tracks an active-oscillator bitmask to avoid needlessly touching idle
// oscillators), STONE_VOICES is a small fixed 6, so blanket-applying to the whole
// range unconditionally is simpler and cheap enough not to need mask-tracking.
static void audioApplyFilterToStone(bool bypass, float cutoffHz, float resonance, uint8_t filterType) {
    amy_event se = amy_default_event();
    se.filter_type = bypass ? FILTER_NONE : filterType;
    se.filter_freq_coefs[COEF_CONST] = bypass ? 18000.0f : cutoffHz;
    if (!bypass) { se.filter_freq_coefs[COEF_EG0] = 0.0f; se.filter_freq_coefs[COEF_EG1] = 0.0f; }
    se.resonance = bypass ? 1.0f : resonance;
    for (uint8_t i = 0; i < STONE_VOICES; i++) {
        se.osc = (uint16_t)(STONE_OSC_BASE + i);
        amy_add_event(&se);
    }
}

// Same gap as STONE above, for MODE_DJ's single dedicated deck oscillator.
static void audioApplyFilterToDJ(bool bypass, float cutoffHz, float resonance, uint8_t filterType) {
    amy_event de = amy_default_event();
    de.osc = DJ_OSC;
    de.filter_type = bypass ? FILTER_NONE : filterType;
    de.filter_freq_coefs[COEF_CONST] = bypass ? 18000.0f : cutoffHz;
    if (!bypass) { de.filter_freq_coefs[COEF_EG0] = 0.0f; de.filter_freq_coefs[COEF_EG1] = 0.0f; }
    de.resonance = bypass ? 1.0f : resonance;
    amy_add_event(&de);
}

// Same gap as STONE above, for the modular synth's 2 dedicated dynamic channels
// (MOD3_OSCA_CH/MOD3_OSCB_CH — not SYNTH_CH, so the shared FILT FX never reached them
// either, even after OVERLAY_FX was wired into MODE_MODULAR). Channels use e.synth (not
// e.osc) since they're whole AMY synth channels, not individually-addressed oscillators.
static void audioApplyFilterToModular(bool bypass, float cutoffHz, float resonance, uint8_t filterType) {
    amy_event me = amy_default_event();
    me.filter_type = bypass ? FILTER_NONE : filterType;
    me.filter_freq_coefs[COEF_CONST] = bypass ? 18000.0f : cutoffHz;
    if (!bypass) { me.filter_freq_coefs[COEF_EG0] = 0.0f; me.filter_freq_coefs[COEF_EG1] = 0.0f; }
    me.resonance = bypass ? 1.0f : resonance;
    for (uint8_t ch : {(uint8_t)MOD3_OSCA_CH, (uint8_t)MOD3_OSCB_CH}) {
        me.synth = ch;
        amy_add_event(&me);
    }
}

// Same gap as STONE above, for CRUNCH's own fixed per-track oscillator range
// (CRUNCH_OSC_BASE..+CRUNCH_TRACKS-1) — per this repo's own recurring-bug note, a
// dedicated range like this is invisible to the shared FILT FX unless explicitly wired
// in here AND into audioSetAllFilters()/audioSetAllFiltersT() below.
static void audioApplyFilterToCrunch(bool bypass, float cutoffHz, float resonance, uint8_t filterType) {
    amy_event ce = amy_default_event();
    ce.filter_type = bypass ? FILTER_NONE : filterType;
    ce.filter_freq_coefs[COEF_CONST] = bypass ? 18000.0f : cutoffHz;
    if (!bypass) { ce.filter_freq_coefs[COEF_EG0] = 0.0f; ce.filter_freq_coefs[COEF_EG1] = 0.0f; }
    ce.resonance = bypass ? 1.0f : resonance;
    for (uint8_t i = 0; i < CRUNCH_TRACKS; i++) {
        ce.osc = (uint16_t)(CRUNCH_OSC_BASE + i);
        amy_add_event(&ce);
    }
}

void audioSetAllFilters(float cutoffHz, float resonance) {
    if (!audioReady) return;
    // Bypass means ONLY "FX genuinely off" (callers pass 0.0f in that case — see
    // applyFxEffect's case 0) — NOT "cutoff dialed near the top of its range". An
    // earlier version of this also bypassed above ~18000Hz on the theory that a
    // wide-open LPF24 is "basically transparent anyway", but treating a real,
    // engaged filter (even at a very high cutoff) the same as no filter at all is
    // exactly backwards: it made the actual audio path switch between "LPF24
    // engaged" and "FILTER_NONE" — a genuine change in filter topology, not just a
    // frequency change — right at the top of the pot's sweep, which is clearly
    // audible (a real texture jump, not a subtle one) and, worse, depended on
    // float-rounding of the pot's exponential mapping landing on one side or the
    // other of the threshold, so it could trigger unpredictably even at a fixed
    // pot position. The filter must stay engaged and simply track a high cutoff
    // continuously — for patches the native-cutoff cap below already makes "cutoff
    // dialed above native" converge smoothly to the patch's own untouched sound,
    // with no separate bypass branch needed to achieve that.
    bool bypass = (cutoffHz <= 10.0f);
    // Bypass: skip SYNTH_CH event for patches — sending FILTER_NONE would clobber the patch's
    // internal LPF (e.g. activating reverb changes J:PNO's texture via FILT inactive reset path).
    // Active (non-bypass): still apply so FX like LFO or FILT work on patches as expected.
    if (!bypass || !s_synthChIsPatch) {
        amy_event e = amy_default_event();
        e.synth = SYNTH_CH;
        // For patches: don't change filter_type — only modulate freq/resonance so the preset's
        // internal filter character (e.g. Juno BPF stack) is preserved under LFO/FX modulation.
        if (!s_synthChIsPatch) e.filter_type = bypass ? FILTER_NONE : FILTER_LPF24;
        // Cap at the patch's own native cutoff — see the matching comment in
        // audioSetAllFiltersT() for why (an FX cutoff above native brightens the patch
        // instead of ever being able to just darken it, since there's one shared filter).
        // See remapCutoffResToNative() for why this is a continuous remap rather than a
        // flat clamp: resonance blends the same way, from the FX's own dialed value at
        // the pot's low end to the patch's native resonance at the high end, so merely
        // activating the FX at "wide open" settings stays fully transparent instead of
        // slamming a resonant peak onto a patch that never had one.
        float effCutoff = cutoffHz;
        float effRes = resonance;
        if (!bypass && s_synthChIsPatch) {
            float nativeCC, nativeRes;
            getPatchNativeFilter(s_currentPatchNumber, nativeCC, nativeRes);
            remapCutoffResToNative(cutoffHz, resonance, nativeCC, nativeRes, effCutoff, effRes);
        }
        e.filter_freq_coefs[COEF_CONST] = bypass ? 18000.0f : effCutoff;
        if (!bypass && !s_synthChIsPatch) { e.filter_freq_coefs[COEF_EG0] = 0.0f; e.filter_freq_coefs[COEF_EG1] = 0.0f; }
        e.resonance = bypass ? 1.0f : effRes;
        amy_add_event(&e);
        if (!bypass && s_synthChIsPatch) s_patchFilterModified = true;
    } else if (s_patchFilterModified) {
        // Bypass AND a patch whose filter WAS previously modified by an earlier
        // (non-bypass) call — e.g. cutoff was turned down at some point then back up
        // to "wide open". The branch above is skipped entirely in this case (that's
        // the whole point of the "skip SYNTH_CH event for patches" bypass), which
        // used to leave the patch stuck on that earlier, lower cutoff forever: wide
        // open no longer meant "no filtering" once the patch's filter had ever been
        // touched. Explicitly restore its native filter here instead of doing nothing.
        s_patchFilterModified = false;
        float cc, res;
        getPatchNativeFilter(s_currentPatchNumber, cc, res);
        amy_event e = amy_default_event();
        e.synth = SYNTH_CH;
        e.filter_freq_coefs[COEF_CONST] = cc;
        e.resonance = res;
        amy_add_event(&e);
    }
    s_pcmLPFCutoff = bypass ? 0.0f : cutoffHz;
    s_pcmLPFReso   = bypass ? 1.5f : resonance;
    s_pcmLPFType   = bypass ? (uint8_t)FILTER_NONE : (uint8_t)FILTER_LPF24;
    // GR2 oscillators are individually controlled (not part of SYNTH_CH); apply filter to any active ones.
    if (s_gran2ActiveOscMask) {
        amy_event ge = amy_default_event();
        ge.filter_type = bypass ? FILTER_NONE : FILTER_LPF24;
        ge.filter_freq_coefs[COEF_CONST] = bypass ? 18000.0f : cutoffHz;
        if (!bypass) { ge.filter_freq_coefs[COEF_EG0] = 0.0f; ge.filter_freq_coefs[COEF_EG1] = 0.0f; }
        ge.resonance = bypass ? 1.0f : resonance;
        uint32_t mask = s_gran2ActiveOscMask;
        while (mask) {
            int idx = __builtin_ctz(mask);
            ge.osc = (uint16_t)(GRANULAR_OSC_BASE + idx);
            amy_add_event(&ge);
            mask &= mask - 1;
        }
    }
    audioApplyFilterToStone(bypass, cutoffHz, resonance, FILTER_LPF24);
    audioApplyFilterToModular(bypass, cutoffHz, resonance, FILTER_LPF24);
    audioApplyFilterToDJ(bypass, cutoffHz, resonance, FILTER_LPF24);
    audioApplyFilterToCrunch(bypass, cutoffHz, resonance, FILTER_LPF24);
}

// Same as audioSetAllFilters but with a configurable AMY filter type constant.
// Use for FX FILT effect when the user selects a different filter topology.
void audioSetAllFiltersT(float cutoffHz, float resonance, uint8_t filterType) {
    if (!audioReady) return;
    // Bypass when turned off (≤10 Hz) OR when cutoff is fully open (≥18000 Hz = paramMax).
    // A LPF24 at 18 kHz still colours the signal (phase shift + resonance peak) — treat as bypass.
    // Bypass means ONLY "FX genuinely off" (callers pass 0.0f in that case — see
    // applyFxEffect's case 0) — NOT "cutoff dialed near the top of its range". An
    // earlier version of this also bypassed above ~18000Hz on the theory that a
    // wide-open LPF24 is "basically transparent anyway", but treating a real,
    // engaged filter (even at a very high cutoff) the same as no filter at all is
    // exactly backwards: it made the actual audio path switch between "LPF24
    // engaged" and "FILTER_NONE" — a genuine change in filter topology, not just a
    // frequency change — right at the top of the pot's sweep, which is clearly
    // audible (a real texture jump, not a subtle one) and, worse, depended on
    // float-rounding of the pot's exponential mapping landing on one side or the
    // other of the threshold, so it could trigger unpredictably even at a fixed
    // pot position. The filter must stay engaged and simply track a high cutoff
    // continuously — for patches the native-cutoff cap below already makes "cutoff
    // dialed above native" converge smoothly to the patch's own untouched sound,
    // with no separate bypass branch needed to achieve that.
    bool bypass = (cutoffHz <= 10.0f);
    // Bypass: skip SYNTH_CH event for patches — FILTER_NONE on bypass clobbers the patch's
    // internal LPF (e.g. toggling reverb would make J:PNO sound harpsichord-like).
    // Active (non-bypass): still apply so FILT/DISTORT FX work on patches as expected.
    if (!bypass || !s_synthChIsPatch) {
        amy_event e = amy_default_event();
        e.synth = SYNTH_CH;
        // For patches: preserve filter_type so the preset's internal filter is not overridden.
        if (!s_synthChIsPatch) e.filter_type = bypass ? FILTER_NONE : filterType;
        // For patches, never let the FX's cutoff open the filter BEYOND the patch's own
        // native cutoff — the FX overwrites the same single filter_freq_coefs the patch's
        // own native character depends on (there's no separate cascaded stage), so an FX
        // cutoff above the native value doesn't add filtering, it REPLACES the patch's own
        // (often much lower/darker) native cutoff with a brighter one — audibly "the LPF
        // boosts the highs" instead of ever being able to just cut them, and is exactly
        // backwards from what turning on a low-pass filter should be able to do. Capping
        // at the native cutoff means the FX can only ever darken a patch further, matching
        // normal LPF expectations. See remapCutoffResToNative() for why this is a
        // continuous remap across the whole pot travel (both cutoff and resonance)
        // rather than a flat clamp above native.
        float effCutoff = cutoffHz;
        float effRes = resonance;
        if (!bypass && s_synthChIsPatch) {
            float nativeCC, nativeRes;
            getPatchNativeFilter(s_currentPatchNumber, nativeCC, nativeRes);
            remapCutoffResToNative(cutoffHz, resonance, nativeCC, nativeRes, effCutoff, effRes);
        }
        e.filter_freq_coefs[COEF_CONST] = bypass ? 18000.0f : effCutoff;
        if (!bypass && !s_synthChIsPatch) {
            e.filter_freq_coefs[COEF_EG0] = 0.0f;
            e.filter_freq_coefs[COEF_EG1] = 0.0f;
        }
        e.resonance = bypass ? 1.0f : effRes;
        amy_add_event(&e);
        if (!bypass && s_synthChIsPatch) s_patchFilterModified = true;
    } else if (s_patchFilterModified) {
        // Bypass AND a patch whose filter WAS previously modified by an earlier
        // (non-bypass) call — e.g. cutoff was turned down at some point then back up
        // to "wide open". The branch above is skipped entirely in this case, which
        // used to leave the patch stuck on that earlier, lower cutoff forever: wide
        // open no longer meant "no filtering" once the patch's filter had ever been
        // touched. Explicitly restore its native filter here instead of doing nothing.
        s_patchFilterModified = false;
        float cc, res;
        getPatchNativeFilter(s_currentPatchNumber, cc, res);
        amy_event e = amy_default_event();
        e.synth = SYNTH_CH;
        e.filter_freq_coefs[COEF_CONST] = cc;
        e.resonance = res;
        amy_add_event(&e);
    }
    s_pcmLPFCutoff = bypass ? 0.0f : cutoffHz;
    s_pcmLPFReso   = bypass ? 1.5f : resonance;
    s_pcmLPFType   = bypass ? (uint8_t)FILTER_NONE : filterType;
    if (s_gran2ActiveOscMask) {
        amy_event ge = amy_default_event();
        ge.filter_type = bypass ? FILTER_NONE : filterType;
        ge.filter_freq_coefs[COEF_CONST] = bypass ? 18000.0f : cutoffHz;
        if (!bypass) { ge.filter_freq_coefs[COEF_EG0] = 0.0f; ge.filter_freq_coefs[COEF_EG1] = 0.0f; }
        ge.resonance = bypass ? 1.0f : resonance;
        uint32_t mask = s_gran2ActiveOscMask;
        while (mask) {
            int idx = __builtin_ctz(mask);
            ge.osc = (uint16_t)(GRANULAR_OSC_BASE + idx);
            amy_add_event(&ge);
            mask &= mask - 1;
        }
    }
    audioApplyFilterToStone(bypass, cutoffHz, resonance, filterType);
    audioApplyFilterToModular(bypass, cutoffHz, resonance, filterType);
    audioApplyFilterToDJ(bypass, cutoffHz, resonance, filterType);
    audioApplyFilterToCrunch(bypass, cutoffHz, resonance, filterType);
}

// Update only the cutoff/resonance of an already-active LPF — does NOT send filter_type.
// Sending filter_type on every smooth-tick tick causes AMY to re-init the biquad state,
// which is heard as an audible "pop" or "reset" on each encoder step.
//
// This is what the FILT cutoff pot's per-10ms anti-zipper smoothing tick actually
// calls (see main.cpp's "Smooth FILT cutoff application" block) — unlike
// audioSetAllFilters(T)(), it had NO bypass awareness at all: it unconditionally
// wrote filter_freq_coefs/resonance to SYNTH_CH on every tick, so a patch (JUNO/DX7)
// got its native filter permanently overwritten the moment the cutoff pot was
// touched at all, with nothing to undo it once the pot settled back at "wide open"
// (18kHz) — the exact "LPF changes an instrument's texture even with cutoff wide
// open" bug, and the dominant path for it since this runs continuously while turning
// the pot, not just once on toggling FILT on/off.
void audioSetFilterFreq(float cutoffHz, float resonance) {
    if (!audioReady) return;
    // Bypass means ONLY "FX genuinely off" (callers pass 0.0f in that case — see
    // applyFxEffect's case 0) — NOT "cutoff dialed near the top of its range". An
    // earlier version of this also bypassed above ~18000Hz on the theory that a
    // wide-open LPF24 is "basically transparent anyway", but treating a real,
    // engaged filter (even at a very high cutoff) the same as no filter at all is
    // exactly backwards: it made the actual audio path switch between "LPF24
    // engaged" and "FILTER_NONE" — a genuine change in filter topology, not just a
    // frequency change — right at the top of the pot's sweep, which is clearly
    // audible (a real texture jump, not a subtle one) and, worse, depended on
    // float-rounding of the pot's exponential mapping landing on one side or the
    // other of the threshold, so it could trigger unpredictably even at a fixed
    // pot position. The filter must stay engaged and simply track a high cutoff
    // continuously — for patches the native-cutoff cap below already makes "cutoff
    // dialed above native" converge smoothly to the patch's own untouched sound,
    // with no separate bypass branch needed to achieve that.
    bool bypass = (cutoffHz <= 10.0f);
    if (bypass && s_synthChIsPatch) {
        if (s_patchFilterModified) {
            s_patchFilterModified = false;
            float cc, res;
            getPatchNativeFilter(s_currentPatchNumber, cc, res);
            amy_event e = amy_default_event();
            e.synth = SYNTH_CH;
            e.filter_freq_coefs[COEF_CONST] = cc;
            e.resonance = res;
            amy_add_event(&e);
        }
        s_pcmLPFCutoff = 0.0f;
        s_pcmLPFReso   = 1.5f;
        return;
    }
    // Remap onto the patch's own native cutoff/resonance — same as audioSetAllFiltersT():
    // this function had NO capping at all, so the anti-zipper smoothing tick (which
    // calls this every 10ms while the cutoff pot is above the automation path) could
    // push a patch's filter above its native character. See remapCutoffResToNative()
    // for why this is a continuous remap rather than a flat clamp.
    float effCutoff = cutoffHz;
    float effRes = resonance;
    if (s_synthChIsPatch) {
        float nativeCC, nativeRes;
        getPatchNativeFilter(s_currentPatchNumber, nativeCC, nativeRes);
        remapCutoffResToNative(cutoffHz, resonance, nativeCC, nativeRes, effCutoff, effRes);
    }
    amy_event e = amy_default_event();
    e.synth = SYNTH_CH;
    e.filter_freq_coefs[COEF_CONST] = effCutoff;
    e.resonance = effRes;
    amy_add_event(&e);
    if (s_synthChIsPatch) s_patchFilterModified = true;
    s_pcmLPFCutoff = cutoffHz;
    s_pcmLPFReso   = resonance;

    // STONE/MODULAR/DJ's dedicated oscillators: audioSetAllFiltersT()'s toggle/Res/Typ
    // path already reaches them (audioApplyFilterToStone/ToModular/ToDJ), but THIS
    // path — continuous cutoff-only pot movement, deliberately kept separate from that
    // one to avoid a filter_type-reset pop on every tick — never did, so dragging the
    // Cutoff knob silently updated s_pcmLPFCutoff above (read back only on the target's
    // NEXT retrigger) without ever reaching a currently-sounding voice on any of these
    // three. Raw cutoffHz/resonance (not the patch-remapped effCutoff/effRes above,
    // which only makes sense for SYNTH_CH's patches) — matches what audioSetAllFiltersT()
    // itself passes to these same three helpers.
    amy_event se = amy_default_event();
    se.filter_freq_coefs[COEF_CONST] = cutoffHz;
    se.resonance = resonance;
    for (uint8_t i = 0; i < STONE_VOICES; i++) { se.osc = (uint16_t)(STONE_OSC_BASE + i); amy_add_event(&se); }
    amy_event me = amy_default_event();
    me.filter_freq_coefs[COEF_CONST] = cutoffHz;
    me.resonance = resonance;
    for (uint8_t ch : {(uint8_t)MOD3_OSCA_CH, (uint8_t)MOD3_OSCB_CH}) { me.synth = ch; amy_add_event(&me); }
    amy_event de = amy_default_event();
    de.osc = DJ_OSC;
    de.filter_freq_coefs[COEF_CONST] = cutoffHz;
    de.resonance = resonance;
    amy_add_event(&de);
}

// Smooth-update filter coefficients on all currently-playing GR2 oscillators (no filter_type → no biquad reset).
void audioSetGranular2FilterFreq(float cutoffHz, float resonance) {
    if (!audioReady || !s_gran2ActiveOscMask) return;
    amy_event e = amy_default_event();
    e.filter_freq_coefs[COEF_CONST] = cutoffHz;
    e.resonance = resonance;
    uint32_t mask = s_gran2ActiveOscMask;
    while (mask) {
        int idx = __builtin_ctz(mask);
        e.osc = (uint16_t)(GRANULAR_OSC_BASE + idx);
        amy_add_event(&e);
        mask &= mask - 1;
    }
}

void audioSetEnvelope(const EnvParams &env) {
    if (!audioReady) return;
    amy_event e = amy_default_event();
    e.synth = SYNTH_CH;
    e.eg0_times[0] = env.atk;  e.eg0_values[0] = 1.0f;
    e.eg0_times[1] = env.dec;  e.eg0_values[1] = env.sus;
    e.eg0_times[2] = env.rel;  e.eg0_values[2] = 0.0f;
    amy_add_event(&e);
}

// Per-shape filter state — used to restore native filter after FX LPF/Overdrive turns off.
// Indexed by SynthShape. Patches (Juno/DX7) restore to FILTER_NONE (patch self-manages).
// Must stay in sync with SynthShape enum order in config.h.
struct ShapeFilterSave { uint8_t ft; float cc, ce0, ce1, res; };
static const ShapeFilterSave kShapeFilter[] = {
    // ft           CONST     EG0     EG1     Reso
    { FILTER_NONE,  18000,    0,      0,      1    }, // SAW
    { FILTER_LPF,   4000,     0,      0,      2    }, // SAW_FM (ALGO)
    { FILTER_NONE,  18000,    0,      0,      1    }, // SQUARE
    { FILTER_NONE,  18000,    0,      0,      1    }, // SINE
    { FILTER_NONE,  18000,    0,      0,      1    }, // SUPERSAW
    { FILTER_LPF,   700,      0,      0,      14   }, // ACID
    { FILTER_LPF,   1200,     0,      0,      3    }, // BASS
    { FILTER_NONE,  18000,    0,      0,      1    }, // PLUCK (KS)
    { FILTER_NONE,  18000,    0,      0,      1    }, // NOISE_WHITE
    { FILTER_LPF,   2000,     0,      0,      1    }, // NOISE_PINK  (gentle rolloff)
    { FILTER_LPF,   400,      0,      0,      0.7f }, // NOISE_BROWN (deep rolloff)
    { FILTER_NONE,  18000,    0,      0,      1    }, // JUNO_BRASS
    { FILTER_NONE,  18000,    0,      0,      1    }, // JUNO_STRINGS
    { FILTER_NONE,  18000,    0,      0,      1    }, // JUNO_PIANO
    { FILTER_NONE,  18000,    0,      0,      1    }, // JUNO_ORGAN
    { FILTER_NONE,  18000,    0,      0,      1    }, // JUNO_CHOIR
    { FILTER_NONE,  18000,    0,      0,      1    }, // DX7_EP
    { FILTER_NONE,  18000,    0,      0,      1    }, // DX7_BELLS
    { FILTER_NONE,  18000,    0,      0,      1    }, // DX7_BASS
    { FILTER_NONE,  18000,    0,      0,      1    }, // DX7_BRASS
    { FILTER_NONE,  18000,    0,      0,      1    }, // DX7_STRINGS
    { FILTER_NONE,  18000,    0,      0,      1    }, // DX7_ORGAN
    { FILTER_NONE,  18000,    0,      0,      1    }, // DX7_VOICE
    { FILTER_LPF,   200,      0,      3200,   4    }, // TECHNO_LEAD
    { FILTER_LPF,   80,       1400,   0,      2    }, // RAVE_BASS  (EG0!)
    { FILTER_LPF,   400,      0,      2400,   6    }, // HOOVER
    { FILTER_LPF,   4000,     0,      -3500,  5    }, // TECHNO_STAB
    { FILTER_LPF,   150,      0,      2500,   10   }, // ACID_WOBBLE
    { FILTER_LPF,   3000,     0,      -2500,  5    }, // ELECTRO_PLUCK
    { FILTER_LPF,   200,      0,      2800,   12   }, // INDUSTRIAL
    { FILTER_NONE,  18000,    0,      0,      1    }, // JUNO_ORGAN2
    { FILTER_NONE,  18000,    0,      0,      1    }, // JUNO_FRONTIER
    { FILTER_LPF,   450,      0,      1800,   4.5f }, // FM_DRIFT
    { FILTER_NONE,  8000,     0,      0,      1    }, // FM_BELL (no LPF, open)
    { FILTER_LPF,   200,      0,      3500,   6.5f }, // SAT_DRIFT
};
static_assert(sizeof(kShapeFilter)/sizeof(kShapeFilter[0]) == SHAPE_COUNT,
              "kShapeFilter must have one entry per SynthShape");

// Restore the shape's native filter+EG coefficients after FX LPF/Overdrive is turned off.
// For preset shapes (Juno/DX7), re-sends patch_number to restore internal filter EG state that
// was zeroed by audioSetAllFiltersT (which kills patch EG to avoid fighting the FX cutoff).
// For custom shapes, sends only filter params — no num_voices → no voice reset.
void audioRestoreShapeFilter(SynthShape shape) {
    if (!audioReady || (uint8_t)shape >= SHAPE_COUNT) return;
    int16_t patch = shapePatch[(uint8_t)shape];
    if (patch >= 0) {
        // Patch: do NOT reload the preset — that resets note envelopes mid-note and causes an
        // audible texture change. Only undo what we actually modified (COEF_CONST + resonance).
        // If we never touched the patch's filter coefs, skip entirely.
        if (!s_patchFilterModified) return;
        s_patchFilterModified = false;
        float cc, res;
        getPatchNativeFilter(patch, cc, res);
        amy_event e = amy_default_event();
        e.synth = SYNTH_CH;
        e.filter_freq_coefs[COEF_CONST] = cc;
        e.resonance = res;
        amy_add_event(&e);
        return;
    }
    const ShapeFilterSave& sf = kShapeFilter[(uint8_t)shape];
    amy_event e = amy_default_event();
    e.synth = SYNTH_CH;
    e.filter_type = sf.ft;
    e.filter_freq_coefs[COEF_CONST] = sf.cc;
    e.filter_freq_coefs[COEF_EG0]   = sf.ce0;
    e.filter_freq_coefs[COEF_EG1]   = sf.ce1;
    e.resonance = sf.res;
    amy_add_event(&e);
}

void audioSetPitchBend(float ratio) {
    if (!audioReady) return;
    amy_event e = amy_default_event();
    e.synth = SYNTH_CH; e.pitch_bend = ratio;
    amy_add_event(&e);
}

void audioSetShapeOnSynth(SynthShape shape, uint8_t synthCh) {
    if (!audioReady) return;
    if (synthCh == SYNTH_CH) {
        s_noiseShape = (shape==SHAPE_NOISE_WHITE || shape==SHAPE_NOISE_PINK || shape==SHAPE_NOISE_BROWN);
        s_synthChIsPatch = (shapePatch[(uint8_t)shape] >= 0);
        s_currentPatchNumber = shapePatch[(uint8_t)shape];
        if (!s_synthChIsPatch) s_patchVolumeScale = 1.0f;
        s_patchFilterModified = false; // new shape = clean filter state
    }
    int16_t patch = shapePatch[shape];
    amy_event e = amy_default_event();
    e.synth = synthCh;

    if (patch >= 0) {
        // AMY preset patch (Juno / DX7 / Piano)
        e.patch_number = patch;
        // Per-patch volume compensation: some AMY presets are louder than others.
        // patch 8 = JUNO_ORGAN, which is ~2× the amplitude of the other Juno patches.
        if (synthCh == SYNTH_CH)
            s_patchVolumeScale = (patch == 8) ? 0.55f : 1.0f;
    } else if (patch == -2) {
        // SAW_FM: 6-op ALGO with default LPF so pots 3-4 (cutoff/reso) have something to bite
        e.wave = ALGO;
        e.algorithm = 3;
        e.num_voices = 4;
        e.oscs_per_voice = 6;
        e.filter_type = FILTER_LPF;
        e.filter_freq_coefs[COEF_CONST] = 4000.0f;
        e.resonance = 2.0f;
    } else {
        // Custom waveforms
        e.num_voices = NUM_SYNTH_VOICES;
        e.oscs_per_voice = OSCS_PER_VOICE;
        switch (shape) {
            case SHAPE_SAW:          e.wave = SAW_DOWN; break;
            case SHAPE_SQUARE:       e.wave = PULSE;    break;
            case SHAPE_SINE:         e.wave = SINE;     break;
            case SHAPE_PLUCK:        e.wave = KS;       break;
            // NOISE shapes: force oscs_per_voice=1 (overriding the general custom-waveform
            // default of 2 set just above). A second AMY oscillator per voice is a detuned-
            // unison thickening technique meaningful for pitched waves (e.g. SAW/SQUARE
            // sound fuller with two slightly-detuned copies) — it doesn't add anything for
            // NOISE (already full-spectrum) and, worse, produces a spurious narrow resonant
            // peak in the noise spectrum: with no patch_number to supply per-oscillator
            // deltas for the 2nd oscillator of each voice, AMY's multi-osc allocation leaves
            // it in a state that isn't a second independent noise generator, and summing it
            // with the first creates a discrete peak no amount of FILT cutoff can remove
            // (confirmed via the simulator's EQ/spectrogram windows: identical peak with
            // num_voices=1, present only when oscs_per_voice=2, gone at oscs_per_voice=1
            // regardless of num_voices — isolating the 2nd-oscillator-per-voice mechanism,
            // not polyphony, as the cause).
            case SHAPE_NOISE_WHITE:  e.wave = NOISE; e.oscs_per_voice = 1; break;
            case SHAPE_NOISE_PINK:
                e.wave = NOISE;
                e.oscs_per_voice = 1;
                e.filter_type = FILTER_LPF;
                e.filter_freq_coefs[COEF_CONST] = 2000.0f;
                e.resonance = 1.0f;
                break;
            case SHAPE_NOISE_BROWN:
                e.wave = NOISE;
                e.oscs_per_voice = 1;
                e.filter_type = FILTER_LPF;
                e.filter_freq_coefs[COEF_CONST] = 400.0f;
                e.resonance = 0.7f;
                break;
            case SHAPE_ACID:
                // TB-303 style: sawtooth + resonant LPF + fast envelope
                e.wave = SAW_DOWN;
                e.filter_type = FILTER_LPF;
                e.resonance = 14.0f;
                e.filter_freq_coefs[COEF_CONST] = 700.0f;
                e.eg0_times[0] = 2;   e.eg0_values[0] = 1.0f;
                e.eg0_times[1] = 300; e.eg0_values[1] = 0.0f;
                e.eg0_times[2] = 100; e.eg0_values[2] = 0.0f;
                break;
            case SHAPE_BASS:
                // Deep bass: square wave + moderate LPF
                e.wave = PULSE;
                e.filter_type = FILTER_LPF;
                e.resonance = 3.0f;
                e.filter_freq_coefs[COEF_CONST] = 1200.0f;
                e.eg0_times[0] = 5;   e.eg0_values[0] = 1.0f;
                e.eg0_times[1] = 500; e.eg0_values[1] = 0.2f;
                e.eg0_times[2] = 200; e.eg0_values[2] = 0.0f;
                break;
            case SHAPE_SUPERSAW:
                // Thick saw: use SAW with max voices for width
                e.wave = SAW_DOWN;
                e.num_voices = NUM_SYNTH_VOICES;
                e.oscs_per_voice = 4;
                break;
            case SHAPE_TECHNO_LEAD:
                // Detuned supersaw + filter envelope: starts dark, sweeps bright
                e.wave = SAW_DOWN;
                e.num_voices = NUM_SYNTH_VOICES;
                e.oscs_per_voice = 4;
                e.filter_type = FILTER_LPF;
                e.resonance = 4.0f;
                e.filter_freq_coefs[COEF_CONST] = 200.0f;   // closed at rest
                e.filter_freq_coefs[COEF_EG1]   = 3200.0f;  // EG1 opens filter
                e.eg1_times[0] = 150;  e.eg1_values[0] = 1.0f;
                e.eg1_times[1] = 600;  e.eg1_values[1] = 0.4f;
                e.eg1_times[2] = 1500; e.eg1_values[2] = 0.0f;
                e.eg0_times[0] = 5;    e.eg0_values[0] = 1.0f;
                e.eg0_times[1] = 200;  e.eg0_values[1] = 0.6f;
                e.eg0_times[2] = 800;  e.eg0_values[2] = 0.0f;
                break;
            case SHAPE_RAVE_BASS:
                // Sub bass: sine-core + filter tracks amplitude for punchy pump
                e.wave = SINE;
                e.num_voices = 4;
                e.oscs_per_voice = 2;
                e.filter_type = FILTER_LPF;
                e.resonance = 2.0f;
                e.filter_freq_coefs[COEF_CONST] = 80.0f;
                e.filter_freq_coefs[COEF_EG0]   = 1400.0f;  // filter burst on attack
                e.eg0_times[0] = 5;    e.eg0_values[0] = 1.0f;
                e.eg0_times[1] = 80;   e.eg0_values[1] = 0.3f;
                e.eg0_times[2] = 300;  e.eg0_values[2] = 0.0f;
                break;
            case SHAPE_HOOVER:
                // Rave hoover: detuned saws, filter + pitch rise envelope
                e.wave = SAW_DOWN;
                e.num_voices = NUM_SYNTH_VOICES;
                e.oscs_per_voice = 6;
                e.filter_type = FILTER_LPF;
                e.resonance = 6.0f;
                e.filter_freq_coefs[COEF_CONST] = 400.0f;
                e.filter_freq_coefs[COEF_EG1]   = 2400.0f;  // filter sweeps up fast
                e.eg1_times[0] = 80;   e.eg1_values[0] = 1.0f;
                e.eg1_times[1] = 400;  e.eg1_values[1] = 0.5f;
                e.eg1_times[2] = 1200; e.eg1_values[2] = 0.0f;
                e.eg0_times[0] = 10;   e.eg0_values[0] = 1.0f;
                e.eg0_times[1] = 1000; e.eg0_values[1] = 0.5f;
                e.eg0_times[2] = 500;  e.eg0_values[2] = 0.0f;
                break;
            case SHAPE_TECHNO_STAB:
                // Short stabby hit: bright saw, instant filter close
                e.wave = SAW_DOWN;
                e.num_voices = 4;
                e.oscs_per_voice = 2;
                e.filter_type = FILTER_LPF;
                e.resonance = 5.0f;
                e.filter_freq_coefs[COEF_CONST] = 4000.0f;
                e.filter_freq_coefs[COEF_EG1]   = -3500.0f; // filter snaps shut
                e.eg1_times[0] = 2;    e.eg1_values[0] = 1.0f;
                e.eg1_times[1] = 120;  e.eg1_values[1] = 0.0f;
                e.eg1_times[2] = 50;   e.eg1_values[2] = 0.0f;
                e.eg0_times[0] = 2;    e.eg0_values[0] = 1.0f;
                e.eg0_times[1] = 60;   e.eg0_values[1] = 0.0f;
                e.eg0_times[2] = 30;   e.eg0_values[2] = 0.0f;
                break;
            case SHAPE_ACID_WOBBLE:
                // Wobble bass: detuned SAW, filter opened by EG1 then slowly closes
                e.wave = SAW_DOWN;
                e.num_voices = NUM_SYNTH_VOICES;
                e.oscs_per_voice = 4;
                e.filter_type = FILTER_LPF;
                e.resonance = 10.0f;
                e.filter_freq_coefs[COEF_CONST] = 150.0f;   // dark at rest
                e.filter_freq_coefs[COEF_EG1]   = 2500.0f;  // filter sweeps up on trigger
                e.eg1_times[0] = 20;   e.eg1_values[0] = 1.0f;  // fast open
                e.eg1_times[1] = 1200; e.eg1_values[1] = 0.1f;  // slow wob decay
                e.eg1_times[2] = 800;  e.eg1_values[2] = 0.0f;
                e.eg0_times[0] = 5;    e.eg0_values[0] = 1.0f;
                e.eg0_times[1] = 200;  e.eg0_values[1] = 0.7f;  // sustained
                e.eg0_times[2] = 600;  e.eg0_values[2] = 0.0f;
                break;
            case SHAPE_ELECTRO_PLUCK:
                // Electro pluck: square wave, filter snaps shut on attack (staccato)
                e.wave = PULSE;
                e.num_voices = 4;
                e.oscs_per_voice = 2;
                e.filter_type = FILTER_LPF;
                e.resonance = 5.0f;
                e.filter_freq_coefs[COEF_CONST] = 3000.0f;
                e.filter_freq_coefs[COEF_EG1]   = -2500.0f;  // filter slams shut
                e.eg1_times[0] = 1;    e.eg1_values[0] = 1.0f;
                e.eg1_times[1] = 180;  e.eg1_values[1] = 0.0f;
                e.eg1_times[2] = 80;   e.eg1_values[2] = 0.0f;
                e.eg0_times[0] = 1;    e.eg0_values[0] = 1.0f;
                e.eg0_times[1] = 150;  e.eg0_values[1] = 0.0f;  // short pluck, no sustain
                e.eg0_times[2] = 50;   e.eg0_values[2] = 0.0f;
                break;
            case SHAPE_INDUSTRIAL:
                // Dark industrial drone: heavy detuned SAWs, very dark resonant filter bursts
                e.wave = SAW_DOWN;
                e.num_voices = NUM_SYNTH_VOICES;
                e.oscs_per_voice = 6;
                e.filter_type = FILTER_LPF;
                e.resonance = 12.0f;
                e.filter_freq_coefs[COEF_CONST] = 200.0f;   // almost closed
                e.filter_freq_coefs[COEF_EG1]   = 2800.0f;  // brief metallic burst
                e.eg1_times[0] = 10;   e.eg1_values[0] = 1.0f;
                e.eg1_times[1] = 350;  e.eg1_values[1] = 0.0f;  // snap back dark
                e.eg1_times[2] = 200;  e.eg1_values[2] = 0.0f;
                e.eg0_times[0] = 2;    e.eg0_values[0] = 1.0f;
                e.eg0_times[1] = 800;  e.eg0_values[1] = 0.3f;  // long sustain for drones
                e.eg0_times[2] = 1500; e.eg0_values[2] = 0.0f;
                break;
            // ---- Evolving / saturation family ----
            case SHAPE_FM_DRIFT:
                // Inspired by J:ORG: ALGO FM, EG1 decays FM index over 10s → timbre slowly darkens.
                // High resonance (Q=4.5) near harmonic peaks → polyphonic saturation builds with notes.
                e.wave = ALGO;
                e.algorithm = 3;              // 4-op serial chain: mod3→mod2→mod1→carrier
                e.num_voices = NUM_SYNTH_VOICES;
                e.oscs_per_voice = 4;
                e.filter_type = FILTER_LPF;
                e.resonance = 4.5f;
                e.filter_freq_coefs[COEF_CONST] = 450.0f;
                e.filter_freq_coefs[COEF_EG1]   = 1800.0f;  // EG1 also sweeps filter open
                // EG1: FM index 1.0→0.2 over 10s (10 000ms, same as J:ORG modulator envelope)
                e.eg1_times[0] = 30;    e.eg1_values[0] = 1.0f;
                e.eg1_times[1] = 10000; e.eg1_values[1] = 0.2f;
                e.eg1_times[2] = 2000;  e.eg1_values[2] = 0.0f;
                // Carrier amplitude: instant on, slight settle, organ-style sustain
                e.eg0_times[0] = 5;     e.eg0_values[0] = 1.0f;
                e.eg0_times[1] = 300;   e.eg0_values[1] = 0.8f;
                e.eg0_times[2] = 1000;  e.eg0_values[2] = 0.0f;
                break;

            case SHAPE_FM_BELL:
                // FM bell: algorithm 5 (one modulator, three parallel carriers → inharmonic partials).
                // EG1 decays FM depth over 5s: starts metallic/complex, rings out to a pure tone.
                e.wave = ALGO;
                e.algorithm = 5;              // one mod → 3 parallel carriers (bright inharmonic partials)
                e.num_voices = 4;
                e.oscs_per_voice = 4;
                // No LPF — bells are open, bright
                e.filter_freq_coefs[COEF_CONST] = 8000.0f;
                e.resonance = 1.0f;
                // EG1: FM index decays from full to zero over 5s
                e.eg1_times[0] = 5;     e.eg1_values[0] = 1.0f;
                e.eg1_times[1] = 5000;  e.eg1_values[1] = 0.0f;
                e.eg1_times[2] = 500;   e.eg1_values[2] = 0.0f;
                // Amplitude: instant attack, 6s natural exponential decay (bell ring)
                e.eg0_times[0] = 5;     e.eg0_values[0] = 1.0f;
                e.eg0_times[1] = 6000;  e.eg0_values[1] = 0.0f;
                e.eg0_times[2] = 500;   e.eg0_values[2] = 0.0f;
                break;

            case SHAPE_SAT_DRIFT:
                // Supersaw with Q=6.5 (near self-oscillation) + EG1 sweeps filter from 200→3700Hz over 8s.
                // Stacking notes causes beating between detuned voices → dense evolving saturation.
                e.wave = SAW_DOWN;
                e.num_voices = NUM_SYNTH_VOICES;
                e.oscs_per_voice = 4;
                e.filter_type = FILTER_LPF;
                e.resonance = 6.5f;           // near self-oscillation — amplifies harmonics at cutoff
                e.filter_freq_coefs[COEF_CONST] = 200.0f;   // starts very dark
                e.filter_freq_coefs[COEF_EG1]   = 3500.0f;  // EG1 slowly opens — builds brightness
                // EG1: filter opens over 8s → gradual harmonic build-up and saturation
                e.eg1_times[0] = 8000;  e.eg1_values[0] = 1.0f;
                e.eg1_times[1] = 5000;  e.eg1_values[1] = 0.8f;
                e.eg1_times[2] = 3000;  e.eg1_values[2] = 0.0f;
                // Amplitude: slow attack (pad-like), long sustain
                e.eg0_times[0] = 600;   e.eg0_values[0] = 1.0f;
                e.eg0_times[1] = 300;   e.eg0_values[1] = 0.9f;
                e.eg0_times[2] = 2500;  e.eg0_values[2] = 0.0f;
                break;

            default: e.wave = SAW_DOWN; break;
        }
    }
    amy_add_event(&e);
    // Shape events on SYNTH_CH trigger patches_load_patch → reset_osc() which silently clears any active
    // LPF set by the FX system. Re-assert immediately. Tracker channels are independent — don't touch them.
    if (synthCh == SYNTH_CH && s_pcmLPFCutoff > 10.0f)
        audioSetAllFilters(s_pcmLPFCutoff, s_pcmLPFReso);
}

void audioSetShape(SynthShape shape) { audioSetShapeOnSynth(shape, SYNTH_CH); }

void audioTrackerInit() {
    if (!audioReady) return;
    // One AMY synth channel per tracker synth track (channels TRACKER_SYNTH_CH_BASE…+TRACKER_SYNTHS-1).
    // Each channel gets TRK_CHORD_SIZE voices with 1 oscillator each — enough for full chord playback
    // while staying isolated from SYNTH_CH (no shape bleed between tracks).
    for (int t = 0; t < TRACKER_SYNTHS; t++) {
        amy_event e = amy_default_event();
        e.synth       = (uint8_t)(TRACKER_SYNTH_CH_BASE + t);
        e.num_voices  = TRK_CHORD_SIZE;
        e.oscs_per_voice = 1;
        e.wave        = SAW_DOWN;
        e.filter_type = FILTER_NONE;
        amy_add_event(&e);
    }
}

// Silence one specific tracker synth track note (called from main.cpp with the actual playing note).
void audioTrackerNoteOff(uint8_t trackIdx, uint8_t midiNote) {
    if (!audioReady || trackIdx >= TRACKER_SYNTHS) return;
    amy_event e = amy_default_event();
    e.synth     = (uint8_t)(TRACKER_SYNTH_CH_BASE + trackIdx);
    e.midi_note = midiNote;
    e.velocity  = 0;
    amy_add_event(&e);
}

// Play one note on a tracker synth track channel (shape already initialized on the channel).
void audioTrackerNoteOn(uint8_t trackIdx, uint8_t midiNote, float vel) {
    if (!audioReady || trackIdx >= TRACKER_SYNTHS) return;
    amy_event e = amy_default_event();
    e.synth     = (uint8_t)(TRACKER_SYNTH_CH_BASE + trackIdx);
    e.midi_note = midiNote;
    e.velocity  = vel;
    amy_add_event(&e);
}

// FM-specific real-time param control for SHAPE_SAW_FM.
// depth: FM feedback 0..1 (0=clean, 1=max harmonic richness)
// cutoffHz: LPF frequency
// resonance: LPF resonance
void audioSetFmParams(float depth, float cutoffHz, float resonance) {
    if (!audioReady) return;
    amy_event e = amy_default_event();
    e.synth = SYNTH_CH;
    e.feedback = depth;
    e.filter_type = FILTER_LPF;
    e.filter_freq_coefs[COEF_CONST] = cutoffHz;
    e.resonance = resonance;
    amy_add_event(&e);
}

void audioSetFmDepth(float depth) {
    if (!audioReady) return;
    amy_event e = amy_default_event();
    e.synth = SYNTH_CH;
    e.feedback = depth;
    amy_add_event(&e);
}

void audioSetOverdrive(float drive) {
    if (!audioReady) return;
    if (drive < 0.01f) {
        audioSetAllFilters(0.0f, 1.5f);
    } else {
        float cut = 3500.0f - drive * 2500.0f;  // 3500 → 1000 Hz
        float res = 1.5f  + drive * 2.5f;       // 1.5 → 4.0
        audioSetAllFilters(cut, res);
    }
}

// Distortion: BPF resonance peak — tone sweeps the peak frequency, drive raises resonance.
// Using FILTER_BPF (not LPF) gives a "presence boost" character very different from the LPF FX:
// instead of darkening the signal it emphasises a narrow harmonic band, producing crunch/bite.
void audioSetDistortion(float drive, float tone) {
    if (!audioReady) return;
    if (drive < 0.01f) {
        audioSetAllFiltersT(0.0f, 1.5f, FILTER_LPF24);  // bypass — clears filter on all oscillators
        return;
    }
    // tone 0→1 sweeps peak from 600 Hz (warm/mid crunch) to 8000 Hz (bright/harsh bite)
    float cutHz = 600.0f + tone * 7400.0f;
    // drive 0→1 raises resonance peak height: 2 (subtle) → 6 (screaming)
    float res = 2.0f + drive * 4.0f;
    audioSetAllFiltersT(cutHz, res, FILTER_BPF);
}

void audioSetVolume(float vol) {
    if (!audioReady) return;
    amy_event e = amy_default_event();
    e.synth = SYNTH_CH;
    e.volume[0] = vol * 5.0f;
    e.volume[1] = vol * 5.0f;  // T303 is on bus 1 — scale together
    amy_add_event(&e);
}

// ==================== EFFECTS (bus 0) ====================
void audioSetReverb(float level, float liveness, float damping, float xover_hz) {
    if (!audioReady) return;
    config_reverb(0, level, liveness, damping, xover_hz);
}

void audioSetChorus(float level, float lfo_freq, float depth) {
    if (!audioReady) return;
    config_chorus(0, level, 320, lfo_freq, depth);
}

void audioSetDelay(float level, float delay_ms, float feedback, float filter_coef) {
    if (!audioReady) return;
    // max_delay_ms=700 → enclosing_power_of_2(30870)=32768 samples = 256KB PSRAM (vs 512KB at 800ms)
    config_echo(0, level, delay_ms, 700.0f, feedback, filter_coef);
}

// ==================== SAMPLE PLAYBACK ====================
void audioPlaySamplePreset(uint16_t preset, float vel) {
    if (!audioReady) return;
    amy_event e = amy_default_event();
    e.osc = PCM_PREVIEW_OSC;
    e.wave = PCM;
    e.preset = preset;
    e.midi_note = 69;
    e.velocity = vel;
    amy_add_event(&e);
}

void audioStopSamplePreset(uint16_t /*preset*/) {
    if (!audioReady) return;
    s_svcAbort = true;
    amyStopOsc(PCM_PREVIEW_OSC);
}

// ==================== DRUM PAD SAMPLES ====================
// 32 unique pads: padIdx = row*8 + col (row 0=bottom, col 0=rightmost).
// Supports two .h formats: int8_t[] (8-bit) and const int[] (16-bit values in 32-bit containers).
struct DrumPadDesc {
    const void*  data;
    uint32_t     len;
    const char*  label;
    bool         is16bit;
};

static const DrumPadDesc kDrumPads[DRUM_PAD_COUNT] = {
    // Row 0 (bottom), cols 0-7 right→left: standard drums (int8_t)
    {kick1,   (uint32_t)sizeof(kick1),   "KK1", false},
    {kick2,   (uint32_t)sizeof(kick2),   "KK2", false},
    {snare1,  (uint32_t)sizeof(snare1),  "SN1", false},
    {snare2,  (uint32_t)sizeof(snare2),  "SN2", false},
    {snare3,  (uint32_t)sizeof(snare3),  "SN3", false},
    {hihat1,  (uint32_t)sizeof(hihat1),  "HH1", false},
    {bongo1,  (uint32_t)sizeof(bongo1),  "BNG", false},
    {snareB3, (uint32_t)sizeof(snareB3), "SNB", false},
    // Row 1, cols 0-7: extended percussion (const int)
    {kick3,   15872U, "KK3", true},
    {hihat2,  7936U,  "HH2", true},
    {clap1,   8960U,  "CLP", true},
    {crash1,  7804U,  "CRS", true},
    {ride1,   11264U, "RDE", true},
    {snareB1, 5888U,  "SB1", true},
    {snareB2, 7680U,  "SB2", true},
    {bass1,   19584U, "BS1", true},
    // Row 2, cols 0-7: bass + sfx
    {bass2,   5120U,  "BS2", true},
    {sfx1,    4608U,  "SX1", true},
    {sfx2,    3448U,  "SX2", true},
    {sfx3,    5632U,  "SX3", true},
    {sfx4,    2256U,  "SX4", true},
    {sfx5,    7168U,  "SX5", true},
    {sfx6,    18944U, "SX6", true},
    {sfx7,    14332U, "SX7", true},
    // Row 3 (top), cols 0-7: more sfx + melodic
    {sfx8,    9984U,  "SX8", true},
    {sfx9,    3076U,  "SX9", true},
    {sfx10,   4608U,  "S10", true},
    {sfx11,   9216U,  "S11", true},
    {sfx12,   10496U, "S12", true},
    {guitar1, 6384U,  "GTR", true},
    {synth1,  7496U,  "SYN", true},
    {pad1,    7432U,  "PAD", true},
};

const char* audioDrumPadLabel(uint8_t padIdx) {
    if (padIdx >= DRUM_PAD_COUNT) return "---";
    return kDrumPads[padIdx].label;
}

void audioLoadDrumSamples() {
    if (!audioReady) return;
    for (uint8_t i = 0; i < DRUM_PAD_COUNT; i++) {
        const DrumPadDesc& pad = kDrumPads[i];
        int16_t* buf = pcm_load(DRUM_PRESET_BASE + i, pad.len, DRUM_SAMPLERATE, 1, 69, 0, 0);
        if (!buf) { Serial.printf("DRUM: alloc fail pad %u (%s)\n", i, pad.label); continue; }
        // Peak-normalize every pad to a consistent loudness on load. The two source
        // formats weren't exported at matching levels: the int8_t samples' blanket *256
        // scale happens to land near full-scale, but the "16-bit in an int32 container"
        // ones (CLP/RDE/HH2/etc.) play back at whatever arbitrary level their own
        // conversion tool produced — some noticeably quieter, audible as a real
        // "some drums are quiet" bug once several are triggered side by side (first
        // reported via EUCLI's 4-lane polyrhythm, but affects every kDrumPads consumer:
        // DRUM2/GROOVE/SYSEQ/CRUNCH's DRUM+SFX banks too, since they all read this table).
        int32_t peak = 1;
        if (pad.is16bit) {
            const int* src = (const int*)pad.data;
            for (uint32_t j = 0; j < pad.len; j++) { int32_t v = src[j]; if (v < 0) v = -v; if (v > peak) peak = v; }
        } else {
            const int8_t* src = (const int8_t*)pad.data;
            for (uint32_t j = 0; j < pad.len; j++) { int32_t v = (int32_t)src[j] * 256; if (v < 0) v = -v; if (v > peak) peak = v; }
        }
        const int32_t kTargetPeak = 30000;  // headroom below the int16 ceiling
        float gain = (float)kTargetPeak / (float)peak;
        if (gain > 8.0f) gain = 8.0f;  // cap: don't wildly amplify a near-silent/corrupt sample
        if (pad.is16bit) {
            const int* src = (const int*)pad.data;
            for (uint32_t j = 0; j < pad.len; j++) buf[j] = (int16_t)constrain((int)((float)src[j] * gain), -32768, 32767);
        } else {
            const int8_t* src = (const int8_t*)pad.data;
            for (uint32_t j = 0; j < pad.len; j++) buf[j] = (int16_t)constrain((int)((float)src[j] * 256.0f * gain), -32768, 32767);
        }
    }
}

void audioPlayDrumPad(uint8_t padIdx, float vel) {
    if (!audioReady || padIdx >= DRUM_PAD_COUNT) return;
    amy_event e = amy_default_event();
    e.osc       = DRUM_OSC_BASE + padIdx;
    e.wave      = PCM;
    e.preset    = DRUM_PRESET_BASE + padIdx;
    e.midi_note = 69;
    e.velocity  = vel;
    amy_add_event(&e);
}

// ==================== NON-BLOCKING SAMPLE LOADER ====================
// audioLoadAndPlay() returns in <1ms. bgServiceTask on Core 0 handles everything:
// AMY stop → SD open/parse (provides stop safety window) → pcm_load → fill → play → continue.
#define PCM_MIN_FRAMES         20000u  // guaranteed minimum head buffer (1s @ 20kHz)
#define PCM_TARGET_RATE        20000u  // target playback rate; pcm_load gets /2 (2x hardware bug)
// Maximum frames allocated from free PSRAM (int16_t = 2 bytes/frame).
// Uses half the available PSRAM so other presets can coexist.
// After first decode the sample is in flash — subsequent plays use 0 PSRAM.
static uint32_t psramMaxFrames() {
    uint32_t free = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    uint32_t m = free / 4;  // half free PSRAM in int16_t frames
    return (m < PCM_MIN_FRAMES) ? PCM_MIN_FRAMES : m;
}
// Wider budget for isGran16 presets (STONE_SOURCE_PRESET / GRAN2_SOURCE_BASE / legacy
// GRANULAR_SOURCE_PRESET) only: unlike every other preset type, these have NO cheaper
// fallback tier at all (no flash cache, no 8-bit SD cache — svcTryFlash/svcTryCache are
// skipped for them entirely, see the isGran16 branch in svcLoadWav/svcLoadMp3 below), so
// a sample that's merely somewhat larger than psramMaxFrames() allows gets needlessly
// truncated/rejected where MODE_SAMPLE's non-Gran16 path would have had cache tiers to
// fall back on instead. Deliberately NOT changing psramMaxFrames() itself — the non-
// Gran16 path's risk profile (it still has flash/8-bit-cache fallbacks) doesn't need to
// change, only the tier that has nothing else to lean on.
static uint32_t granMaxFrames() {
    uint32_t free = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    uint32_t m = free / 2;  // int16_t frames
    return (m < PCM_MIN_FRAMES) ? PCM_MIN_FRAMES : m;
}
// Try pcm_load, halving frame count on each failure until PCM_MIN_FRAMES.
// Modifies `frames` to reflect the actually allocated size (may be less than requested).
//
// PCM_MIN_FRAMES is a floor for the SHRINKING behavior (don't keep halving forever) —
// it must NOT gate the very first attempt. A `while (frames >= PCM_MIN_FRAMES)` loop
// does exactly that though: a short sample that legitimately needs fewer frames than
// PCM_MIN_FRAMES (e.g. a <1s WAV resampling to under 20000 frames at the 20kHz target
// rate) never even calls pcm_load() once — the loop condition is already false — and
// this returns nullptr unconditionally, regardless of how much free PSRAM exists. This
// was the actual cause of "alloc fail" reports showing huge largest_free_block values
// for tiny requests: pcm_load() was never being called at all, not failing.
static int16_t* pcm_load_best_effort(uint16_t preset, uint32_t& frames,
                                      uint32_t halfRate, uint8_t channels,
                                      uint8_t midiNote, uint8_t loopStart, int32_t loopEnd) {
    while (true) {
        int16_t* buf = pcm_load(preset, frames, halfRate, channels, midiNote, loopStart, loopEnd);
        if (buf) return buf;
        if (frames <= PCM_MIN_FRAMES) break;  // already at/below the floor, nothing smaller to try
        // Total free can look huge while the largest CONTIGUOUS block (what a single
        // malloc actually needs) is much smaller — print both, not just total, so a
        // fragmentation-driven failure (small request failing despite lots of free
        // PSRAM) is immediately diagnosable instead of looking impossible.
        uint32_t next = frames / 2;
        if (next < PCM_MIN_FRAMES) next = PCM_MIN_FRAMES;  // don't overshoot below the floor
        Serial.printf("[PCM] alloc %lu fr failed (PSRAM free=%lu largest_block=%lu), retrying at %lu\n",
                      (unsigned long)frames,
                      (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                      (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
                      (unsigned long)next);
        frames = next;
    }
    return nullptr;
}
#define STREAM_PLAY_FRAMES      512u   // pre-fill before triggering AMY play event
#define STREAM_BATCH_FRAMES    2048u   // frames per batch
#define STREAM_SILENCE_GUARD   4096u   // zero-filled guard region at buffer start
#define AMY_STOP_SAFETY_MS       15u   // ms to wait after AMY stop before freeing old preset

bool audioIsStreamingDone() { return s_svcDone; }

static void amyStopOsc(uint8_t osc) {
    amy_event e = amy_default_event();
    e.osc = osc; e.velocity = 0;
    amy_add_event(&e);
}
// feedback: AMY uses this flag to signal native PCM looping (msynth[osc]->feedback>0 makes
// render_pcm() wrap at preset->loopend instead of stopping at sample end). It MUST be set on
// this same triggering event — AMY copies synth[osc]->feedback into msynth[osc]->feedback only
// once, in pcm_note_on() (lib/AMY Synthesizer/src/pcm.c), and explicitly skips re-copying it on
// every render loop for PCM voices (amy.c: "For PCM, don't re-copy it every loop... you can't
// change feedback mid-playback for PCM") — a follow-up event sent after this one to flip it has
// no effect at all. Default 0 (one-shot, no loop) for every caller except DJ_OSC.
static void amyPlayPcm(uint8_t osc, uint16_t preset, float vel, float feedback = 0.0f) {
    if (vel <= 0.0f) return;
    amy_event e = amy_default_event();
    float v = vel * s_sampleVolume;
    if (v > 2.0f) v = 2.0f;
    e.osc = osc; e.wave = PCM;
    e.preset = preset; e.midi_note = 69; e.velocity = v;
    // 5ms fade-in to suppress click; 40ms release for smooth stop on note-off
    e.eg0_times[0]  = 5;    e.eg0_values[0]  = 1.0f;  // attack
    e.eg0_times[1]  = 0;    e.eg0_values[1]  = 1.0f;  // instant decay → sustain at 1.0
    e.eg0_times[2]  = 40;   e.eg0_values[2]  = 0.0f;  // 40ms release
    e.feedback = feedback;
    if (s_pcmLPFCutoff > 10.0f) {
        e.filter_type = s_pcmLPFType;
        e.filter_freq_coefs[COEF_CONST] = s_pcmLPFCutoff;
        e.resonance = s_pcmLPFReso;
    }
    amy_add_event(&e);
}

void audioSetPCMFilter(float cutoffHz, float resonance) {
    s_pcmLPFCutoff = cutoffHz;
    s_pcmLPFReso   = resonance;
}

// Read raw WAV samples (any bit depth/format) into dst as interleaved int16.
// isFloat: true for IEEE 754 float32 (audioFormat=3), false for PCM integer.
static bool wavReadRaw(File &f, uint16_t bps, uint32_t inFrames, uint16_t numCh, int16_t* dst, bool isFloat) {
    uint32_t total = inFrames * numCh;
    if (bps == 16) {
        // Read all 16-bit samples in one shot — SD library handles multi-sector internally.
        return (uint32_t)f.read((uint8_t*)dst, total * 2) == total * 2;
    } else if (bps == 8) {
        // Read in 512-byte chunks matching the SD sector size; yield between chunks to feed watchdog.
        const uint32_t BATCH = 512;
        uint8_t tmp[BATCH];
        uint32_t pos = 0;
        while (pos < total) {
            uint32_t batch = total - pos; if (batch > BATCH) batch = BATCH;
            if ((uint32_t)f.read(tmp, batch) != batch) return false;
            for (uint32_t i = 0; i < batch; i++)
                dst[pos + i] = (int16_t)((int32_t)(tmp[i] - 128) << 8);
            pos += batch;
            taskYIELD();
        }
        return true;
    } else if (bps == 24) {
        const uint32_t BATCH = 128;
        uint8_t tmp[BATCH * 3];
        uint32_t pos = 0;
        while (pos < total) {
            uint32_t batch = (total - pos < BATCH) ? (total - pos) : BATCH;
            f.read(tmp, batch * 3);
            for (uint32_t j = 0; j < batch; j++) {
                int32_t v = (int32_t)tmp[j*3]
                          | ((int32_t)tmp[j*3+1] << 8)
                          | ((int32_t)(int8_t)tmp[j*3+2] << 16);
                dst[pos + j] = (int16_t)(v >> 8);
            }
            pos += batch;
            taskYIELD();
        }
        return true;
    } else if (bps == 32) {
        // IEEE Float (audioFmt=3) or PCM int32 (audioFmt=1)
        const uint32_t BATCH = 64;
        uint8_t tmp[BATCH * 4];
        uint32_t pos = 0;
        while (pos < total) {
            uint32_t batch = total - pos; if (batch > BATCH) batch = BATCH;
            if ((uint32_t)f.read(tmp, batch * 4) != batch * 4) return false;
            for (uint32_t i = 0; i < batch; i++) {
                uint32_t raw32 = (uint32_t)tmp[i*4]     | ((uint32_t)tmp[i*4+1] << 8)
                               | ((uint32_t)tmp[i*4+2] << 16) | ((uint32_t)tmp[i*4+3] << 24);
                if (isFloat) {
                    float v; memcpy(&v, &raw32, 4);
                    if      (v >  1.0f) v =  1.0f;
                    else if (v < -1.0f) v = -1.0f;
                    dst[pos + i] = (int16_t)(v * 32767.0f);
                } else {
                    dst[pos + i] = (int16_t)((int32_t)raw32 >> 16);
                }
            }
            pos += batch;
            taskYIELD();
        }
        return true;
    }
    Serial.printf("WAV: unsupported %u-bit\n", bps);
    return false;
}

// Resample interleaved int16 (inFrames×numCh) → mono at AMY_SAMPLE_RATE.
static void resampleToAmy(const int16_t* src, uint32_t inFrames, uint16_t numCh,
                           int16_t* dst, uint32_t outFrames, float ratio) {
    for (uint32_t oi = 0; oi < outFrames; oi++) {
        float srcPos = oi * ratio;
        uint32_t si0 = (uint32_t)srcPos;
        float frac = srcPos - si0;
        uint32_t si1 = si0 + 1;
        if (si1 >= inFrames) si1 = inFrames - 1;

        int32_t s0 = 0, s1 = 0;
        for (uint16_t c = 0; c < numCh; c++) {
            s0 += src[si0 * numCh + c];
            s1 += src[si1 * numCh + c];
        }
        if (numCh > 1) { s0 /= numCh; s1 /= numCh; }
        dst[oi] = (int16_t)(s0 + (int32_t)((s1 - s0) * frac));
    }
}

// ==================== FLASH PCM CACHE ====================
// Decoded int8_t samples are stored once in the 'pcmcache' flash partition (~9.875MB).
// The partition is memory-mapped at boot; AMY presets point directly into it → zero PSRAM.
//
// Partition layout:
//   [0x0000..0x1FFF]  FlashCacheDir (8KB = 2 sectors, max 64 entries)
//   [0x2000..]        PCM data, packed sequentially, 4-byte aligned per entry
//
// First load of a sample: decode → PSRAM (stream-play as before) → write to flash.
// Subsequent loads: flash hit → pcm_register_extern8 → play directly from flash (0 PSRAM).

#define FLASH_PART_NAME     "pcmcache"
#define FLASH_DIR_MAGIC     0xF1A5C4C5u  // bumped: invalidates old truncated entries
#define FLASH_MAX_ENTRIES   64
#define FLASH_DIR_SECTORS   2          // 2×4KB = 8KB for directory
#define FLASH_DATA_OFFSET   0x2000u    // PCM data starts after the directory
#define FLASH_SECTOR_SIZE   0x1000u    // 4096-byte erase unit

struct FlashCacheEntry {
    char     path[80];       // source file path (null-terminated, truncated if needed)
    uint32_t srcSize;        // original file size — invalidation key
    uint32_t dataOffset;     // byte offset from partition start to int8_t PCM data
    uint32_t frameCount;     // number of int8_t mono frames
    uint32_t sampleRate;     // stored PCM rate (pcm_register_extern8 receives this ÷2 for AMY 2× bug)
    uint32_t reserved;
};
// 80+4+4+4+4+4 = 100 bytes; 64 entries = 6400 bytes < 8KB dir ✓

struct FlashCacheDir {
    uint32_t        magic;
    uint32_t        count;
    uint32_t        nextDataOffset;
    uint32_t        reserved[13];       // pad header to 64 bytes
    FlashCacheEntry entries[FLASH_MAX_ENTRIES];
};

static const esp_partition_t*      s_pcmPart    = nullptr;
static esp_partition_mmap_handle_t s_mmapHandle = 0;
static const void*                 s_mmapBase   = nullptr;
static EXT_RAM_ATTR FlashCacheDir  s_flashDir;  // on-flash directory mirror in PSRAM (~6KB)

static void flashRemmap() {
    if (s_mmapHandle) { esp_partition_munmap(s_mmapHandle); s_mmapHandle = 0; }
    s_mmapBase = nullptr;
    if (!s_pcmPart) return;
    esp_err_t e = esp_partition_mmap(s_pcmPart, 0, s_pcmPart->size,
                                      ESP_PARTITION_MMAP_DATA, &s_mmapBase, &s_mmapHandle);
    if (e != ESP_OK) { Serial.printf("[FLASH] mmap err %d\n", (int)e); }
}

void flashCacheInit() {
    s_pcmPart = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                          ESP_PARTITION_SUBTYPE_ANY, FLASH_PART_NAME);
    if (!s_pcmPart) { Serial.println("[FLASH] pcmcache partition not found"); return; }
    Serial.printf("[FLASH] partition @ 0x%06X  size=%uKB\n",
                  (unsigned)s_pcmPart->address, (unsigned)(s_pcmPart->size / 1024));

    memset(&s_flashDir, 0, sizeof(s_flashDir));
    esp_err_t e = esp_partition_read(s_pcmPart, 0, &s_flashDir, sizeof(FlashCacheDir));
    bool valid = (e == ESP_OK) && (s_flashDir.magic == FLASH_DIR_MAGIC)
              && (s_flashDir.count <= FLASH_MAX_ENTRIES)
              && (s_flashDir.nextDataOffset >= FLASH_DATA_OFFSET)
              && (s_flashDir.nextDataOffset <= s_pcmPart->size);
    if (!valid) {
        Serial.println("[FLASH] dir invalid, initializing...");
        esp_partition_erase_range(s_pcmPart, 0, FLASH_DIR_SECTORS * FLASH_SECTOR_SIZE);
        memset(&s_flashDir, 0, sizeof(s_flashDir));
        s_flashDir.magic          = FLASH_DIR_MAGIC;
        s_flashDir.count          = 0;
        s_flashDir.nextDataOffset = FLASH_DATA_OFFSET;
        esp_partition_write(s_pcmPart, 0, &s_flashDir, sizeof(FlashCacheDir));
    }

    flashRemmap();
    Serial.printf("[FLASH] ready: %u entries, %uKB used / %uKB avail\n",
                  s_flashDir.count,
                  s_flashDir.nextDataOffset / 1024,
                  (unsigned)(s_pcmPart->size / 1024));
}

// Wipe flash sample cache (use when sample files have changed and old entries are stale).
void flashCacheClear() {
    if (!s_pcmPart) return;
    Serial.println("[FLASH] clearing all entries...");
    esp_partition_erase_range(s_pcmPart, 0, FLASH_DIR_SECTORS * FLASH_SECTOR_SIZE);
    memset(&s_flashDir, 0, sizeof(s_flashDir));
    s_flashDir.magic          = FLASH_DIR_MAGIC;
    s_flashDir.count          = 0;
    s_flashDir.nextDataOffset = FLASH_DATA_OFFSET;
    esp_partition_write(s_pcmPart, 0, &s_flashDir, sizeof(FlashCacheDir));
    flashRemmap();
    Serial.println("[FLASH] cleared");
}

// Returns entry index or -1 (not found) or -2 (stale srcSize mismatch).
static int flashLookup(const char* path, uint32_t srcSize) {
    for (uint32_t i = 0; i < s_flashDir.count; i++) {
        if (strncmp(s_flashDir.entries[i].path, path, 79) == 0)
            return (s_flashDir.entries[i].srcSize == srcSize) ? (int)i : -2;
    }
    return -1;
}

// Register a flash entry as an AMY preset (no PSRAM allocated) and start playback.
static bool flashCacheLoad(int idx, uint16_t preset, uint8_t osc, float vel, uint32_t tStop) {
    if (!s_mmapBase || idx < 0 || idx >= (int)s_flashDir.count) return false;
    const FlashCacheEntry& en = s_flashDir.entries[idx];
    if (en.dataOffset + en.frameCount > s_pcmPart->size) return false;

    uint32_t el = millis() - tStop;
    if (el < AMY_STOP_SAFETY_MS) vTaskDelay(pdMS_TO_TICKS(AMY_STOP_SAFETY_MS - el));

    const int8_t* ptr = (const int8_t*)((const uint8_t*)s_mmapBase + en.dataOffset);
    pcm_register_extern8(preset, ptr, en.frameCount, en.sampleRate / 2, 69, 0, en.frameCount - 1);
    amyPlayPcm(osc, preset, vel);

    if (osc >= SAMPLE_OSC_BASE && osc < SAMPLE_OSC_BASE + SAMPLE_KEY_COUNT)
        s_keyLengthMs[osc - SAMPLE_OSC_BASE] = (uint32_t)
            ((uint64_t)en.frameCount * 1000 / en.sampleRate);

    Serial.printf("[FLASH] hit: %s (%u frames @ %uHz, 0 PSRAM)\n",
                  en.path, en.frameCount, en.sampleRate);
    return true;
}

// Write int16_t PCM (already normalized) to flash as int8_t, update directory.
static bool flashCacheWrite(const char* path, uint32_t srcSize,
                             const int16_t* buf16, uint32_t frames, uint32_t sampleRate) {
    if (!s_pcmPart || !s_mmapBase || frames == 0 || !buf16) return false;
    if (s_flashDir.count >= FLASH_MAX_ENTRIES) {
        Serial.println("[FLASH] dir full"); return false;
    }

    uint32_t offset      = (s_flashDir.nextDataOffset + 3) & ~3u;
    uint32_t writeAligned = (frames + 3) & ~3u;  // flash write must be 4-byte multiple

    if (offset + writeAligned > s_pcmPart->size) {
        Serial.printf("[FLASH] out of space (need %uKB)\n", writeAligned / 1024);
        return false;
    }

    // Erase all sectors that will be written
    uint32_t eraseStart = offset & ~(FLASH_SECTOR_SIZE - 1u);
    uint32_t eraseEnd   = ((offset + writeAligned) + FLASH_SECTOR_SIZE - 1) & ~(FLASH_SECTOR_SIZE - 1u);
    Serial.printf("[FLASH] erasing %uKB for %u frames...\n", (eraseEnd - eraseStart) / 1024, frames);
    esp_partition_erase_range(s_pcmPart, eraseStart, eraseEnd - eraseStart);
    vTaskDelay(2);

    // Convert int16 → int8 and write in 1KB chunks
    const uint32_t CHUNK = 1024;
    uint8_t tmp[CHUNK];
    uint32_t written = 0;
    while (written < writeAligned && !s_svcAbort) {
        uint32_t cnt = writeAligned - written; if (cnt > CHUNK) cnt = CHUNK;
        for (uint32_t i = 0; i < cnt; i++) {
            uint32_t si = written + i;
            tmp[i] = (si < frames) ? (uint8_t)(int8_t)(buf16[si] >> 8) : 0;
        }
        esp_partition_write(s_pcmPart, offset + written, tmp, cnt);
        written += cnt;
        vTaskDelay(1);
    }
    if (s_svcAbort) return false;

    // Update RAM directory
    FlashCacheEntry& en = s_flashDir.entries[s_flashDir.count];
    strncpy(en.path, path, 79); en.path[79] = '\0';
    en.srcSize         = srcSize;
    en.dataOffset      = offset;
    en.frameCount      = frames;
    en.sampleRate      = sampleRate;
    en.reserved        = 0;
    s_flashDir.count++;
    s_flashDir.nextDataOffset = offset + writeAligned;

    // Persist directory to flash
    esp_partition_erase_range(s_pcmPart, 0, FLASH_DIR_SECTORS * FLASH_SECTOR_SIZE);
    esp_partition_write(s_pcmPart, 0, &s_flashDir, sizeof(FlashCacheDir));

    flashRemmap();  // remap so new data is visible through s_mmapBase
    Serial.printf("[FLASH] stored %s  %u frames @ 0x%06X  (%u total entries, %uKB used)\n",
                  path, frames, offset, s_flashDir.count, s_flashDir.nextDataOffset / 1024);
    return true;
}

// Try loading a preset from the flash partition. Returns true = hit.
static bool svcTryFlash(const char* path, uint32_t srcSize, uint16_t preset,
                         uint8_t osc, float vel, uint32_t tStop) {
    if (!s_mmapBase) return false;
    int idx = flashLookup(path, srcSize);
    if (idx < 0) return false;  // -1 = not found, -2 = stale (fall through to re-decode)
    return flashCacheLoad(idx, preset, osc, vel, tStop);
}

// ---- PCM decode cache ----
// After decoding a WAV/MP3, the raw PCM frames are saved as <path>.pcm on SD.
// On subsequent loads the cache is read directly instead of re-decoding.
// Cache format: PcmCacheHdr (20 bytes) + int16_t[frameCount]
struct PcmCacheHdr {
    char     magic[4];       // "GPC3"
    uint32_t frameCount;     // mono int16_t frames stored
    uint32_t srcSize;        // source file size for invalidation
    uint32_t pcmSampleRate;  // sample rate of stored frames (WAV=AMY_SAMPLE_RATE, MP3=native)
    uint32_t reserved;
};

// Opens and validates a "<path>.pcm16" cache (mono int16 @ PCM_TARGET_RATE, magic "G16C") —
// the SAME format/extension the granular-source cache already uses (svcTryCache16 below), now
// also read by DJ/SS2 streaming (see this file's own DJ/SS2 sections) so a track cached from
// either path is directly usable by the other. Leaves the file open and positioned right after
// the header on a hit (ready for readPcmCacheChunk); closes it and returns false on any miss.
static bool openValidPcmCache(const char* path, uint32_t srcSize, File& outFile, PcmCacheHdr& outHdr) {
    char cp[264]; snprintf(cp, sizeof(cp), "%s.pcm16", path);
    File f = SD.open(cp, FILE_READ);
    if (!f) return false;
    bool valid = ((uint32_t)f.read((uint8_t*)&outHdr, sizeof(outHdr)) == sizeof(outHdr))
              && (memcmp(outHdr.magic, "G16C", 4) == 0)
              && (outHdr.srcSize == srcSize)
              && (outHdr.frameCount > 0)
              && (outHdr.pcmSampleRate == PCM_TARGET_RATE)
              && ((uint32_t)f.size() == (uint32_t)(sizeof(outHdr) + outHdr.frameCount * 2u));
    if (!valid) { f.close(); return false; }
    outFile = f;
    return true;
}

// Reads frames directly from an already-validated .pcm16 cache — no decode, byte-exact seek.
// Returns the number of frames actually read (0 at/past true EOF, same "nothing left" contract
// djDecodeMp3Chunk/djDecodeWavChunk already use).
static uint32_t readPcmCacheChunk(File& cacheFile, const PcmCacheHdr& hdr,
                                   uint32_t startFrame, int16_t* outBuf, uint32_t maxFrames) {
    if (startFrame >= hdr.frameCount) return 0;
    uint32_t frames = min(maxFrames, hdr.frameCount - startFrame);
    cacheFile.seek((uint32_t)sizeof(PcmCacheHdr) + startFrame * 2u);
    uint32_t got = (uint32_t)cacheFile.read((uint8_t*)outBuf, frames * 2) / 2;
    return got;
}

// Peak-normalize: scale buf to 90% of int16 full scale. Max gain 16× to avoid noise.
static void normalizeBuffer(int16_t* buf, uint32_t frames) {
    if (frames == 0) return;
    int32_t peak = 0;
    for (uint32_t i = 0; i < frames; i++) {
        int32_t v = buf[i]; if (v < 0) v = -v;
        if (v > peak) peak = v;
    }
    if (peak < 500) return;  // near-silence, don't amplify noise
    float scale = 29491.0f / (float)peak;  // target 90% of 32767
    if (scale <= 1.02f) return;            // already near full scale
    if (scale > 16.0f) scale = 16.0f;     // cap at 24dB
    for (uint32_t i = 0; i < frames; i++) {
        int32_t v = (int32_t)((float)buf[i] * scale);
        if (v > 32767) v = 32767; else if (v < -32767) v = -32767;
        buf[i] = (int16_t)v;
    }
    Serial.printf("PCM norm: peak=%d scale=%.1fx\n", (int)peak, scale);
}

// Apply short fade-in / fade-out to remove clicks at sample boundaries.
static void applyBufferFades(int16_t* buf, uint32_t frames) {
    if (frames == 0) return;
    const uint32_t fadeInF  = (uint32_t)(PCM_TARGET_RATE * 0.005f);  // 5ms
    const uint32_t fadeOutF = (uint32_t)(PCM_TARGET_RATE * 0.040f);  // 40ms
    uint32_t fi = (fadeInF  < frames / 4) ? fadeInF  : frames / 4;
    uint32_t fo = (fadeOutF < frames / 4) ? fadeOutF : frames / 4;
    for (uint32_t i = 0; i < fi; i++)
        buf[i] = (int16_t)((int32_t)buf[i] * (int32_t)i / (int32_t)fi);
    for (uint32_t i = 0; i < fo; i++)
        buf[frames - 1 - i] = (int16_t)((int32_t)buf[frames - 1 - i] * (int32_t)i / (int32_t)fo);
}

// Try loading AMY preset from .pcm cache. Returns true = cache hit; false = miss (caller must decode).
static bool svcTryCache(const char* path, uint16_t preset, uint8_t osc, float vel,
                         uint32_t tStop, uint32_t srcSize) {
    char cp[264]; snprintf(cp, sizeof(cp), "%s.pcm", path);
    File f = SD.open(cp, FILE_READ);
    if (!f) return false;

    PcmCacheHdr h;
    uint32_t maxLoad = (uint32_t)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 2);
    if (maxLoad < PCM_MIN_FRAMES) maxLoad = PCM_MIN_FRAMES;
    bool valid = ((uint32_t)f.read((uint8_t*)&h, sizeof(h)) == sizeof(h))
              && (memcmp(h.magic, "GPC4", 4) == 0)
              && (h.srcSize == srcSize)
              && (h.frameCount > 0)
              && (h.pcmSampleRate == PCM_TARGET_RATE)  // reject stale caches with wrong rate
              && ((uint32_t)f.size() == (uint32_t)(sizeof(h) + h.frameCount * 1u));
    if (valid && h.frameCount > maxLoad) h.frameCount = maxLoad;  // stream only what fits in PSRAM
    if (!valid) { f.close(); return false; }

    // Cache hit — wait for AMY stop safety before freeing old preset.
    uint32_t el = millis() - tStop;
    if (el < AMY_STOP_SAFETY_MS) vTaskDelay(pdMS_TO_TICKS(AMY_STOP_SAFETY_MS - el));

    int8_t* amyBuf = pcm_load8(preset, h.frameCount, h.pcmSampleRate / 2, 1, 69, 0, 0);
    if (!amyBuf) { f.close(); return false; }
    memset(amyBuf, 0, h.frameCount * sizeof(int8_t));

    uint32_t pos = 0; bool played = false;
    while (pos < h.frameCount && !s_svcAbort) {
        uint32_t batch = h.frameCount - pos;
        if (batch > STREAM_BATCH_FRAMES) batch = STREAM_BATCH_FRAMES;
        if ((uint32_t)f.read((uint8_t*)(amyBuf + pos), batch) != batch) break;
        pos += batch;
        if (!played && pos >= STREAM_PLAY_FRAMES) { amyPlayPcm(osc, preset, vel); played = true; }
        vTaskDelay(1);
    }
    if (!played) amyPlayPcm(osc, preset, vel);
    f.close();

    if (osc >= SAMPLE_OSC_BASE && osc < SAMPLE_OSC_BASE + SAMPLE_KEY_COUNT && !s_svcAbort)
        s_keyLengthMs[osc - SAMPLE_OSC_BASE] = (uint32_t)((uint64_t)pos * 1000 / h.pcmSampleRate);

    Serial.printf("CACHE hit: %s (%u frames @ %uHz)\n", cp, pos, h.pcmSampleRate);
    return true;
}

// ---- 16-bit SD cache (granular source only) ----
// Same as the 8-bit SD cache but stores raw int16_t frames in a .pcm16 sidecar.
// Required because granular slicing uses int16_t* pointer arithmetic on the PSRAM buffer;
// the 8-bit cache (.pcm) would cause 2x speed / one-octave-up artefact.

static bool svcTryCache16(const char* path, uint16_t preset, uint8_t osc, float vel,
                            uint32_t tStop, uint32_t srcSize) {
    char cp[264]; snprintf(cp, sizeof(cp), "%s.pcm16", path);
    PcmCacheHdr h;
    File f;
    if (!openValidPcmCache(path, srcSize, f, h)) return false;
    // Only ever called for isGran16 presets (STONE/GRANULAR2/legacy GRANULAR source) —
    // they have no cheaper fallback tier at all, so use the wider budget (see
    // granMaxFrames()'s comment) rather than the general-purpose psramMaxFrames().
    uint32_t maxLoad = granMaxFrames();
    if (h.frameCount > maxLoad) h.frameCount = maxLoad;

    uint32_t el = millis() - tStop;
    if (el < AMY_STOP_SAFETY_MS) vTaskDelay(pdMS_TO_TICKS(AMY_STOP_SAFETY_MS - el));

    int16_t* amyBuf = pcm_load_best_effort(preset, h.frameCount, h.pcmSampleRate / 2, 1, 69, 0, 0);
    if (!amyBuf) {
        Serial.printf("[GR2] pcm_load_best_effort failed for preset %d (%lu B free, %lu B largest block)\n",
                      preset, (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                      (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
        f.close(); return false;
    }
    memset(amyBuf, 0, h.frameCount * sizeof(int16_t));

    uint32_t pos = 0; bool played = false;
    while (pos < h.frameCount && !s_svcAbort) {
        uint32_t batch = h.frameCount - pos;
        if (batch > STREAM_BATCH_FRAMES) batch = STREAM_BATCH_FRAMES;
        if ((uint32_t)f.read((uint8_t*)(amyBuf + pos), batch * sizeof(int16_t)) != batch * sizeof(int16_t)) break;
        pos += batch;
        if (!played && pos >= STREAM_PLAY_FRAMES) { amyPlayPcm(osc, preset, vel); played = true; }
        vTaskDelay(1);
    }
    if (!played) amyPlayPcm(osc, preset, vel);
    f.close();
    Serial.printf("[G16] cache hit: %s (%u frames @ %uHz)\n", cp, pos, h.pcmSampleRate);
    return true;
}

static void svcWriteCache16(const char* path, const int16_t* buf, uint32_t frames,
                              uint32_t srcSize, uint32_t pcmSampleRate) {
    if (!buf || frames == 0) return;
    char cp[264]; snprintf(cp, sizeof(cp), "%s.pcm16", path);
    File f = SD.open(cp, FILE_WRITE);
    if (!f) { Serial.printf("[G16] write fail: %s\n", cp); return; }
    PcmCacheHdr h; memcpy(h.magic, "G16C", 4);
    h.frameCount = frames; h.srcSize = srcSize; h.pcmSampleRate = pcmSampleRate; h.reserved = 0;
    f.write((uint8_t*)&h, sizeof(h));
    const uint32_t CHUNK = 1024;  // 1024 int16_t = 2KB per write
    for (uint32_t p = 0; p < frames && !s_svcAbort; p += CHUNK) {
        uint32_t cnt = frames - p; if (cnt > CHUNK) cnt = CHUNK;
        f.write((uint8_t*)(buf + p), cnt * sizeof(int16_t));
        vTaskDelay(1);
    }
    f.close();
    Serial.printf("[G16] cache saved: %s (%u frames, %lu B)\n", cp, frames,
                  (unsigned long)(sizeof(h) + frames * 2u));
}

// Save decoded PCM frames to .pcm cache alongside the source file.
static void svcWriteCache(const char* path, const int16_t* buf, uint32_t frames,
                           uint32_t srcSize, uint32_t pcmSampleRate) {
    if (!buf || frames == 0) return;
    char cp[264]; snprintf(cp, sizeof(cp), "%s.pcm", path);
    File f = SD.open(cp, FILE_WRITE);
    if (!f) { Serial.printf("CACHE: write fail %s\n", cp); return; }
    PcmCacheHdr h; memcpy(h.magic, "GPC4", 4);
    h.frameCount = frames; h.srcSize = srcSize; h.pcmSampleRate = pcmSampleRate; h.reserved = 0;
    f.write((uint8_t*)&h, sizeof(h));
    const uint32_t CHUNK = 1024;
    int8_t tmp[CHUNK];
    for (uint32_t p = 0; p < frames && !s_svcAbort; p += CHUNK) {
        uint32_t cnt = frames - p; if (cnt > CHUNK) cnt = CHUNK;
        for (uint32_t i = 0; i < cnt; i++)
            tmp[i] = (int8_t)(buf[p + i] >> 8);  // upper 8 bits of int16
        f.write((uint8_t*)tmp, cnt);
        vTaskDelay(1);
    }
    f.close();
    Serial.printf("CACHE: saved %s (%u frames, %lu B)\n", cp, frames, (unsigned long)(sizeof(h) + frames * 1));
}

// ---- WAV loader (runs inside bgServiceTask) ----
// Returns true if the preset was successfully filled (even if partial), false on hard error.
static bool svcLoadWav(const char* path, uint16_t preset, uint8_t osc, float vel, uint32_t tStop) {
    s_loadError = KEY_ERR_IO;  // default; overridden below for specific failures
    // Try flash partition first (zero PSRAM), then SD cache, then full decode.
    uint32_t srcSize = 0;
    { File tmp = SD.open(path, FILE_READ); if (tmp) { srcSize = tmp.size(); tmp.close(); } }
    // Granular source presets need 16-bit PSRAM for int16_t* slice arithmetic.
    bool isGran16 = (preset == GRANULAR_SOURCE_PRESET)
                 || (preset >= GRAN2_SOURCE_BASE && preset < GRAN2_SOURCE_BASE + GRAN2_MAX_SAMPLES)
                 || (preset == STONE_SOURCE_PRESET);
    if (isGran16) {
        if (svcTryCache16(path, preset, osc, vel, tStop, srcSize)) return !s_svcAbort;
    } else {
        if (svcTryFlash(path, srcSize, preset, osc, vel, tStop)) return !s_svcAbort;
        if (svcTryCache(path, preset, osc, vel, tStop, srcSize)) return !s_svcAbort;
    }

    File f = SD.open(path, FILE_READ);
    if (!f) { Serial.printf("WAV: open fail: %s\n", path); return false; }

    // Parse RIFF/WAVE header. SD open+parse (~20ms) naturally covers the AMY stop window.
    uint8_t riff[12];
    if (f.read(riff, 12) < 12 || memcmp(riff, "RIFF", 4) || memcmp(riff+8, "WAVE", 4)) {
        Serial.println("WAV: bad header"); f.close(); return false;
    }
    uint16_t numCh = 1; uint32_t fileSR = AMY_SAMPLE_RATE; uint16_t bps = 16; uint16_t audioFmt = 1;
    uint32_t dataSize = 0; bool foundData = false;
    int chunkIter = 0;
    while (f.available() > 8 && !foundData && ++chunkIter <= 32) {
        uint8_t ch[8]; if (f.read(ch, 8) < 8) break;
        uint32_t csz = (uint32_t)ch[4]|((uint32_t)ch[5]<<8)|((uint32_t)ch[6]<<16)|((uint32_t)ch[7]<<24);
        uint32_t cpos = (uint32_t)f.position();  // start of chunk DATA (after 8-byte header)
        uint32_t fsize = (uint32_t)f.size();
        // Guard: corrupt csz would overflow seek position → infinite loop
        if (csz > fsize || cpos > fsize - csz) {
            if (!memcmp(ch, "data", 4)) { dataSize = fsize - cpos; foundData = true; }
            else { Serial.printf("WAV: corrupt chunk size %u at pos %u\n", csz, cpos); f.close(); s_loadError = KEY_ERR_FORMAT; return false; }
            break;
        }
        if (!memcmp(ch, "fmt ", 4)) {
            // Read up to 40 bytes: needed for WAVE_FORMAT_EXTENSIBLE (sub-format at byte 24)
            uint8_t fmt[40] = {};
            uint32_t rd = csz < sizeof(fmt) ? csz : (uint32_t)sizeof(fmt);
            f.read(fmt, rd);
            f.seek(cpos + csz + (csz & 1));  // seek past entire chunk from its data start
            audioFmt = (uint16_t)(fmt[0]|(fmt[1]<<8));  // 1=PCM, 3=IEEE Float, 65534=EXTENSIBLE
            numCh    = (uint16_t)(fmt[2]|(fmt[3]<<8));
            fileSR   = (uint32_t)fmt[4]|(uint32_t)(fmt[5]<<8)|(uint32_t)(fmt[6]<<16)|(uint32_t)(fmt[7]<<24);
            bps      = (uint16_t)(fmt[14]|(fmt[15]<<8));
            if (audioFmt == 65534 && rd >= 26)  // EXTENSIBLE: actual sub-format GUID at byte 24-25
                audioFmt = (uint16_t)(fmt[24]|(fmt[25]<<8));
        } else if (!memcmp(ch, "data", 4)) {
            // Some writers (streamed/live recordings) leave the data chunk size as a 0
            // placeholder and never patch it after recording ends — fall back to "rest of
            // file" instead of treating it as an empty/corrupt file.
            dataSize = (csz > 0) ? csz : (fsize - cpos);
            foundData = true;
        }
        else { f.seek(cpos + csz + (csz & 1)); }
    }
    if (!foundData || dataSize == 0 || fileSR == 0 || numCh == 0 || numCh > 8
        || (audioFmt != 1 && audioFmt != 3)) {
        Serial.printf("WAV: parse fail (data=%u SR=%u ch=%u bps=%u fmt=%u)\n",
                      dataSize, fileSR, numCh, bps, audioFmt);
        f.close(); s_loadError = KEY_ERR_FORMAT; return false;
    }
    bool wavIsFloat = (audioFmt == 3 && bps == 32);

    // Ensure AMY had enough time to process the stop event before freeing old buffer
    uint32_t el = millis() - tStop;
    if (el < AMY_STOP_SAFETY_MS) vTaskDelay(pdMS_TO_TICKS(AMY_STOP_SAFETY_MS - el));

    uint32_t bpf      = numCh * (bps / 8);
    uint32_t totalIn  = bpf ? dataSize / bpf : 0;
    float    ratio    = (float)fileSR / (float)PCM_TARGET_RATE;
    uint32_t totalOut = (uint32_t)((float)totalIn / ratio);
    {
        uint32_t mf = isGran16 ? granMaxFrames() : psramMaxFrames();
        if (totalOut > mf) { totalOut = mf; totalIn = (uint32_t)(mf * ratio) + 1; }
    }

    Serial.printf("WAV: %uHz %uch %ubps %u→%u frames @%uHz\n", fileSR, numCh, bps, totalIn, totalOut, PCM_TARGET_RATE);

    int16_t* amyBuf = pcm_load_best_effort(preset, totalOut, PCM_TARGET_RATE / 2, 1, 69, 0, 0);
    if (!amyBuf) {
        Serial.printf("WAV: alloc fail (PSRAM exhausted, largest_block=%lu B)\n",
                      (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
        f.close(); s_loadError = KEY_ERR_ALLOC; return false;
    }

    // Zero entire buffer: AMY starts playing before decode finishes, uninitialized
    // frames must be silent (not random PSRAM garbage causing high-pitched artifacts).
    memset(amyBuf, 0, totalOut * sizeof(int16_t));

    int16_t* raw = (int16_t*)ps_malloc(STREAM_BATCH_FRAMES * numCh * sizeof(int16_t));
    if (!raw) { f.close(); s_loadError = KEY_ERR_ALLOC; return false; }

    bool played = false;
    uint32_t inPos = 0, outPos = 0;

    while (inPos < totalIn && outPos < totalOut && !s_svcAbort) {
        uint32_t batchIn = totalIn - inPos;
        if (batchIn > STREAM_BATCH_FRAMES) batchIn = STREAM_BATCH_FRAMES;
        if (!wavReadRaw(f, bps, batchIn, numCh, raw, wavIsFloat)) break;

        uint32_t outEnd = (uint32_t)((float)(inPos + batchIn) / ratio);
        if (outEnd > totalOut) outEnd = totalOut;
        uint32_t outCnt = outEnd > outPos ? outEnd - outPos : 0;

        if (outCnt > 0) {
            if (fileSR == (uint32_t)PCM_TARGET_RATE && numCh == 1) {
                memcpy(amyBuf + outPos, raw, outCnt * sizeof(int16_t));
            } else if (fileSR == (uint32_t)PCM_TARGET_RATE && numCh == 2) {
                // Fast path: average L+R, no resampling
                for (uint32_t i = 0; i < outCnt; i++)
                    amyBuf[outPos + i] = (int16_t)(((int32_t)raw[i*2] + raw[i*2+1]) >> 1);
            } else {
                // General: linear interpolation + channel mix
                for (uint32_t oi = 0; oi < outCnt && !s_svcAbort; oi++) {
                    float sp = (outPos + oi) * ratio - inPos;
                    if (sp < 0.0f) sp = 0.0f;
                    uint32_t si0 = (uint32_t)sp; float frac = sp - si0;
                    uint32_t si1 = si0 + 1; if (si1 >= batchIn) si1 = batchIn - 1;
                    int32_t s0 = 0, s1 = 0;
                    for (uint16_t c = 0; c < numCh; c++) { s0 += raw[si0*numCh+c]; s1 += raw[si1*numCh+c]; }
                    if (numCh > 1) { s0 /= numCh; s1 /= numCh; }
                    amyBuf[outPos + oi] = (int16_t)(s0 + (int32_t)((s1 - s0) * frac));
                }
            }
        }

        inPos  += batchIn;
        outPos  = outEnd;

        if (!played && outPos >= STREAM_PLAY_FRAMES) {
            amyPlayPcm(osc, preset, vel);
            played = true;
        }
        vTaskDelay(1);
    }

    if (!played) amyPlayPcm(osc, preset, vel);
    free(raw);
    f.close();
    Serial.printf("WAV done: %u/%u frames\n", outPos, totalOut);
    if (osc >= SAMPLE_OSC_BASE && osc < SAMPLE_OSC_BASE + SAMPLE_KEY_COUNT && !s_svcAbort)
        s_keyLengthMs[osc - SAMPLE_OSC_BASE] = (uint32_t)((uint64_t)outPos * 1000 / PCM_TARGET_RATE);
    if (!s_svcAbort && outPos > 0) normalizeBuffer(amyBuf, outPos);
    if (!s_svcAbort && outPos > 0) applyBufferFades(amyBuf, outPos);
    if (isGran16) {
        if (!s_svcAbort && outPos > 0) svcWriteCache16(path, amyBuf, outPos, srcSize, PCM_TARGET_RATE);
    } else {
        if (!s_svcAbort && outPos > 0) svcWriteCache(path, amyBuf, outPos, srcSize, PCM_TARGET_RATE);
        if (!s_svcAbort && outPos > 0) flashCacheWrite(path, srcSize, amyBuf, outPos, PCM_TARGET_RATE);
    }
    return !s_svcAbort;
}

// Skips a leading ID3v2 tag, if present, on a freshly-opened file positioned at true byte 0.
// Every mp3 sync-search in this file (small-sample loader, DJ/SS2 streaming, SS2's duration
// probe) reads one fixed-size buffer (as little as 4KB) and gives up if MP3FindSyncWord()
// doesn't find a frame in it — but a common, perfectly valid ID3v2 tag (embedded cover art
// alone routinely does this) is easily tens of KB, well past that window, so real audio was
// never even considered and the load failed outright ("no decodable audio", confirmed on a
// real user file with a ~39KB tag). Parses the syncsafe size field directly instead of
// scanning for it — scanning risks a false-positive sync match inside arbitrary binary tag
// data (e.g. embedded artwork) landing the decoder mid-tag instead of on real audio.
// Returns the file positioned right after the tag (or unchanged, back at byte 0, if there
// isn't one). ID3v1 (a fixed 128-byte tag at the true END of the file) is unrelated and not
// handled here.
static void mp3SkipId3v2(File& f) {
    uint8_t hdr[10];
    if (f.read(hdr, 10) != 10 || hdr[0] != 'I' || hdr[1] != 'D' || hdr[2] != '3') {
        f.seek(0);
        return;
    }
    uint32_t tagSize = ((uint32_t)(hdr[6] & 0x7F) << 21) | ((uint32_t)(hdr[7] & 0x7F) << 14) |
                       ((uint32_t)(hdr[8] & 0x7F) << 7)  |  (uint32_t)(hdr[9] & 0x7F);
    uint32_t skip = 10 + tagSize;
    if (hdr[5] & 0x10) skip += 10;  // extended footer present (ID3v2.4 only) — also 10 bytes
    f.seek(skip);
}

// Total byte length of a leading ID3v2 tag (header+body+optional footer), 0 if none. Duration
// estimates that divide by whole-file size must exclude it: a tag with cover art (tens of KB)
// otherwise makes the bytes-per-second measure absurdly high and the estimated track length
// collapse to a few seconds — the DJ progress display stuck at "10s" on long tagged files.
static uint32_t mp3Id3v2Size(const char* path) {
    File f = SD.open(path, FILE_READ);
    if (!f) return 0;
    uint8_t h[10];
    uint32_t total = 0;
    if (f.read(h, 10) == 10 && h[0]=='I' && h[1]=='D' && h[2]=='3') {
        total = 10 + (((uint32_t)(h[6]&0x7F)<<21)|((uint32_t)(h[7]&0x7F)<<14)|((uint32_t)(h[8]&0x7F)<<7)|(h[9]&0x7F));
        if (h[5] & 0x10) total += 10;
    }
    f.close();
    return total;
}

// Exact duration from a Xing/Info (LAME) header in the first frame after the ID3 tag — 0 if the
// file has none. Far better than extrapolating bytes-per-second from the first grain, which
// is badly off on VBR files (a quiet intro at low bitrate made a 4:18 track read as 6:58).
static float mp3HeaderDuration(const char* path) {
    File f = SD.open(path, FILE_READ);
    if (!f) return 0.0f;
    mp3SkipId3v2(f);
    uint8_t b[220];
    uint32_t n = f.read(b, sizeof(b));
    f.close();
    int i = 0;
    while (i + 4 < (int)n && !(b[i] == 0xFF && (b[i+1] & 0xE0) == 0xE0)) i++;
    if (i + 4 >= (int)n) return 0.0f;
    int ver = (b[i+1] >> 3) & 3, srIdx = (b[i+2] >> 2) & 3;
    if (ver == 1 || srIdx == 3) return 0.0f;
    static const uint32_t sr1[3] = {44100, 48000, 32000};
    uint32_t sr = sr1[srIdx] >> (ver == 3 ? 0 : (ver == 2 ? 1 : 2));
    uint32_t spf = (ver == 3) ? 1152 : 576;
    for (int j = i + 4; j + 12 < (int)n; j++) {
        if ((!memcmp(b + j, "Xing", 4) || !memcmp(b + j, "Info", 4)) && (b[j + 7] & 1)) {
            uint32_t frames = ((uint32_t)b[j+8] << 24) | ((uint32_t)b[j+9] << 16) | ((uint32_t)b[j+10] << 8) | b[j+11];
            return frames ? (float)frames * spf / (float)sr : 0.0f;
        }
    }
    return 0.0f;
}

// ---- .pcm16 cache built as a BY-PRODUCT of live streaming ("tee") ----
// A separate background decoder needs a second Helix decoder plus a task stack in INTERNAL RAM,
// which this app doesn't have to spare on hardware (measured: largest free block ~15KB, ~32KB
// free) — it never got to start. Instead the grains the live stream ALREADY decodes are appended
// to <path>.pcm16.tmp as they land: no extra decoder, task or stack. It only works while the
// track plays forward, contiguously, from the start (any seek/reverse/scratch breaks the run and
// the partial file is dropped; the next forward pass from 0 starts a fresh one), and the file is
// published (rename) once the decoder reports true end-of-file. Play a track through once and
// it is cached for next time.
#define PCM_TEE_RING_FRAMES 131072u   // 256KB of PSRAM: absorbs SD write latency so grain jobs never wait on the card

// Threading: the grain job (worker task) only fills the PSRAM ring and sets flags; ALL SD access
// (open/write/rename/remove) happens in teeFlush(), inside the grain worker task, right after a grain lands — never on the main loop, which also drives audio via amy_update();
// stream is healthy and no grain job is running — a slow SD write must never sit in the audio path.
struct PcmTee {
    bool     armed = false;         // this track wants a cache
    bool     hasRun = false;        // a contiguous run from frame 0 is being recorded
    char     path[256] = {};
    uint32_t srcSize = 0;
    int16_t* ring = nullptr;
    volatile uint32_t w = 0, r = 0; // producer (job) / consumer (service) frame counters
    uint32_t nextFrame = 0;         // frames accepted so far (job side)
    volatile bool abortReq = false, finishReq = false;
    File     f; bool fileOpen = false;
};
static void teeTmpPath(const PcmTee& t, char* out, size_t n) { snprintf(out, n, "%s.pcm16.tmp", t.path); }
// Main thread only, with no grain job in flight.
static void teeReset(PcmTee& t, bool keepArmed) {
    if (t.fileOpen) { t.f.close(); char tp[280]; teeTmpPath(t, tp, sizeof(tp)); SD.remove(tp); }
    t.fileOpen = false; t.hasRun = false; t.abortReq = false; t.finishReq = false;
    t.w = t.r = 0; t.nextFrame = 0;
    if (!keepArmed) { t.armed = false; if (t.ring) { free(t.ring); t.ring = nullptr; } }
}
static void teeDisarm(PcmTee& t) { teeReset(t, false); }
static void teeArm(PcmTee& t, const char* path, uint32_t srcSize) {
    teeDisarm(t);
    strncpy(t.path, path, sizeof(t.path) - 1); t.path[sizeof(t.path) - 1] = '\0';
    t.srcSize = srcSize; t.armed = true;
    Serial.printf("[CACHE] armed: will record %s while it plays forward from the start\n", path);
}
static inline uint32_t teeGap(uint32_t a, uint32_t b) { return a > b ? a - b : b - a; }
// Grain-job side: NO SD access here.
static void teeAppend(PcmTee& t, float readFromSec, bool reverse, const int16_t* buf, uint32_t frames) {
    if (!t.armed || t.abortReq || t.finishReq || !buf || frames == 0 || reverse) return;
    uint32_t start = (uint32_t)lroundf(readFromSec * (float)PCM_TARGET_RATE);
    if (!t.hasRun) {
        if (start != 0) return;  // only a run that begins at the very start is worth keeping
        if (!t.ring) t.ring = (int16_t*)ps_malloc(PCM_TEE_RING_FRAMES * sizeof(int16_t));
        if (!t.ring) { t.armed = false; return; }
        t.hasRun = true; t.nextFrame = 0; t.w = t.r = 0;
    } else if (start > t.nextFrame + 400) { Serial.printf("[CACHE] gap: grain at %u > next %u\n", (unsigned)start, (unsigned)t.nextFrame); t.abortReq = true; return; }  // a forward jump left a hole
    uint32_t skip = start < t.nextFrame ? t.nextFrame - start : 0;       // re-decoded overlap (resume/seek back) — already have it
    if (skip >= frames) return;
    uint32_t n = frames - skip;
    if ((t.w - t.r) + n > PCM_TEE_RING_FRAMES) { Serial.println("[CACHE] ring full"); t.abortReq = true; return; }  // SD can't keep up
    for (uint32_t i = 0; i < n; i++) t.ring[(t.w + i) % PCM_TEE_RING_FRAMES] = buf[skip + i];
    t.w += n; t.nextFrame += n;
}
// Forward decode found nothing at readFromSec: true end of file.
static void teeEnd(PcmTee& t, float readFromSec) {
    if (!t.hasRun || t.abortReq) return;
    uint32_t start = (uint32_t)lroundf(readFromSec * (float)PCM_TARGET_RATE);
    // start==0 means the stream already wrapped past EOF (the wrap resets the logical position to 0
    // before the empty read reaches us), so that also marks a complete pass.
    if (t.nextFrame < PCM_TARGET_RATE || (start != 0 && teeGap(start, t.nextFrame) > 400)) { Serial.printf("[CACHE] EOF at %u but recorded %u — not contiguous\n", (unsigned)start, (unsigned)t.nextFrame); t.abortReq = true; }
    else t.finishReq = true;
}
// Runs INSIDE the grain worker, right after a grain is committed (job slot still held). amy_update()
// — the audio block fill — runs from loop() on core 1, so ANY blocking SD call on the main loop
// (this used to be teeService() from the tick) stalls audio and is heard as micro-dropouts; here
// it only delays the NEXT grain start, which the 2s buffer margin absorbs. Also serialises these
// writes with the stream's own reads instead of racing them for the SPI bus.
static void teeFlush(PcmTee& t) {
    if (!t.armed) return;
    if (t.abortReq) { Serial.println("[CACHE] recording dropped (seek gap / SD too slow) — will retry on the next full forward pass"); teeReset(t, true); return; }
    for (int k = 0; k < 12; k++) {
        uint32_t avail = t.w - t.r;
        if (avail > 0) {
            if (!t.fileOpen) {
                char tp[280]; teeTmpPath(t, tp, sizeof(tp));
                t.f = SD.open(tp, FILE_WRITE);
                if (!t.f) { Serial.println("[CACHE] cannot create .tmp on SD — giving up for this track"); teeReset(t, false); return; }
                PcmCacheHdr h; memcpy(h.magic, "G16C", 4);
                h.frameCount = 0; h.srcSize = t.srcSize; h.pcmSampleRate = PCM_TARGET_RATE; h.reserved = 0;
                t.f.write((uint8_t*)&h, sizeof(h));
                t.fileOpen = true;
                Serial.printf("[CACHE] recording %s.pcm16.tmp\n", t.path);
            }
            // Small on-stack staging buffer: this runs on the grain worker's stack together with
            // FatFs' own deep write path — a 4KB buffer here overflowed it (stack canary panic).
            int16_t tmp[1024];
            uint32_t n = avail < 1024 ? avail : 1024;
            for (uint32_t i = 0; i < n; i++) tmp[i] = t.ring[(t.r + i) % PCM_TEE_RING_FRAMES];
            size_t wr = t.f.write((uint8_t*)tmp, n * sizeof(int16_t));
            if (wr != n * sizeof(int16_t)) {
                Serial.printf("[CACHE] SD write failed (%u of %u B, err=%d, used=%llu MB of %llu MB) — dropping\n", (unsigned)wr, (unsigned)(n * sizeof(int16_t)), (int)t.f.getWriteError(), (unsigned long long)(SD.usedBytes()>>20), (unsigned long long)(SD.totalBytes()>>20));
                teeReset(t, true); return;
            }
            t.r += n;
            continue;
        }
        break;
    }
    if (t.finishReq && t.fileOpen && t.w == t.r) {
        PcmCacheHdr h; memcpy(h.magic, "G16C", 4);
        h.frameCount = t.nextFrame; h.srcSize = t.srcSize; h.pcmSampleRate = PCM_TARGET_RATE; h.reserved = 0;
        t.f.seek(0); t.f.write((uint8_t*)&h, sizeof(h)); t.f.close(); t.fileOpen = false;
        char tp[280], fp[280]; teeTmpPath(t, tp, sizeof(tp)); snprintf(fp, sizeof(fp), "%s.pcm16", t.path);
        SD.remove(fp);
        if (SD.rename(tp, fp)) Serial.printf("[CACHE] pcm16 built: %s (%u frames)\n", fp, (unsigned)t.nextFrame);
        else { Serial.printf("[CACHE] rename failed: %s\n", fp); SD.remove(tp); }
        teeReset(t, false);
    }
}

// ---- MP3 loader (runs inside bgServiceTask) ----
#define MP3_INBUF_SIZE (16 * 1024)

// Returns true if the preset was successfully filled, false on hard error.
static bool svcLoadMp3(const char* path, uint16_t preset, uint8_t osc, float vel, uint32_t tStop) {
    s_loadError = KEY_ERR_IO;  // default; overridden below for specific failures
    // Get source size then try cache — avoids the full MP3 decode on repeated loads.
    uint32_t srcSize = 0;
    { File tmp = SD.open(path, FILE_READ); if (!tmp) { Serial.printf("MP3: open fail: %s\n", path); return false; } srcSize = tmp.size(); tmp.close(); }
    bool isGran16 = (preset == GRANULAR_SOURCE_PRESET)
                 || (preset >= GRAN2_SOURCE_BASE && preset < GRAN2_SOURCE_BASE + GRAN2_MAX_SAMPLES)
                 || (preset == STONE_SOURCE_PRESET);
    if (isGran16) {
        if (svcTryCache16(path, preset, osc, vel, tStop, srcSize)) return !s_svcAbort;
    } else {
        if (svcTryFlash(path, srcSize, preset, osc, vel, tStop)) return !s_svcAbort;
        if (svcTryCache(path, preset, osc, vel, tStop, srcSize)) return !s_svcAbort;
    }

    File f = SD.open(path, FILE_READ);
    if (!f) { return false; }
    uint32_t fileSize = srcSize;
    mp3SkipId3v2(f);

    HMP3Decoder dec = MP3InitDecoder();
    if (!dec) { f.close(); s_loadError = KEY_ERR_ALLOC; return false; }

    uint8_t* inBuf = (uint8_t*)malloc(MP3_INBUF_SIZE);
    if (!inBuf) { MP3FreeDecoder(dec); f.close(); s_loadError = KEY_ERR_ALLOC; return false; }

    int16_t* frameBuf = (int16_t*)malloc(MAX_NGRAN * MAX_NCHAN * MAX_NSAMP * sizeof(int16_t));
    if (!frameBuf) { MP3FreeDecoder(dec); f.close(); s_loadError = KEY_ERR_ALLOC; return false; }
    uint32_t sampleRate = 44100;
    uint8_t  numCh = 2;
    bool     gotInfo = false;
    uint32_t estimatedTotal = isGran16 ? granMaxFrames() : psramMaxFrames();

    uint8_t* ptr = inBuf; int bytesLeft = 0; bool eof = false;
    auto refill = [&]() {
        if (ptr != inBuf && bytesLeft > 0) memmove(inBuf, ptr, bytesLeft);
        ptr = inBuf;
        if (!eof) {
            uint32_t space = (uint32_t)(MP3_INBUF_SIZE - bytesLeft);
            uint32_t rd = f.read(inBuf + bytesLeft, space);
            bytesLeft += (int)rd;
            if (rd < space) eof = true;
        }
    };

    refill();
    int sync = MP3FindSyncWord(ptr, bytesLeft);
    if (sync < 0) { Serial.println("MP3: no sync"); free(frameBuf); free(inBuf); MP3FreeDecoder(dec); f.close(); s_loadError = KEY_ERR_FORMAT; return false; }
    ptr += sync; bytesLeft -= sync;

    // Decode first frame to get sample rate, channels, and estimate total frames
    {
        if (bytesLeft < MAINBUF_SIZE) refill();
        int preDecode = bytesLeft;
        int ret = MP3Decode(dec, &ptr, &bytesLeft, frameBuf, 0);
        if (ret == 0) {
            MP3FrameInfo info; MP3GetLastFrameInfo(dec, &info);
            // Validate before use — corrupt files can yield nChans=0 → div-by-zero
            if (info.nChans >= 1 && info.nChans <= 2 && info.samprate > 0 && info.samprate <= 48000
                && info.outputSamps >= 1 && info.outputSamps <= (int)(MAX_NGRAN * MAX_NCHAN * MAX_NSAMP)) {
                sampleRate = (uint32_t)info.samprate;
                numCh      = (uint8_t)info.nChans;
                gotInfo    = true;
                int frameBytes = preDecode - bytesLeft;
                if (frameBytes > 0) {
                    uint32_t spf  = (uint32_t)info.outputSamps / (uint32_t)info.nChans;
                    uint64_t est  = ((uint64_t)fileSize / (uint32_t)frameBytes) * spf;
                    est = est * PCM_TARGET_RATE / sampleRate;  // convert to target rate frames
                    if (est < estimatedTotal) estimatedTotal = (uint32_t)est;
                }
            } else {
                Serial.printf("MP3: bad first frame (ch=%d SR=%d samps=%d)\n", info.nChans, info.samprate, info.outputSamps);
            }
        }
    }

    // Ensure AMY stop safety window
    uint32_t el = millis() - tStop;
    if (el < AMY_STOP_SAFETY_MS) vTaskDelay(pdMS_TO_TICKS(AMY_STOP_SAFETY_MS - el));

    int16_t* amyBuf = pcm_load_best_effort(preset, estimatedTotal, PCM_TARGET_RATE / 2, 1, 69, 0, 0);
    if (!amyBuf) { free(frameBuf); free(inBuf); MP3FreeDecoder(dec); f.close(); s_loadError = KEY_ERR_ALLOC; return false; }
    // Zero entire buffer so unfinished frames play silence rather than PSRAM garbage.
    memset(amyBuf, 0, estimatedTotal * sizeof(int16_t));

    uint32_t totalFrames = 0;  // output frame count at PCM_TARGET_RATE
    uint32_t inPos = 0;        // decoded frame count at native sampleRate
    bool played = false;

    // Commit first decoded frame (already in frameBuf, numCh already validated above)
    if (gotInfo && numCh > 0) {
        MP3FrameInfo info; MP3GetLastFrameInfo(dec, &info);
        uint32_t spf = (uint32_t)info.outputSamps / (uint32_t)numCh;
        // Downmix stereo→mono in-place (reads [j*2],[j*2+1], writes [j] — safe since j < j*2)
        if (numCh > 1)
            for (uint32_t j = 0; j < spf; j++)
                frameBuf[j] = (int16_t)(((int32_t)frameBuf[j*2] + frameBuf[j*2+1]) >> 1);
        // Resample frameBuf[0..spf-1] from sampleRate → PCM_TARGET_RATE
        float ratio_mp3 = (float)sampleRate / PCM_TARGET_RATE;
        uint32_t outEnd = (uint32_t)(spf / ratio_mp3);
        if (outEnd > estimatedTotal) outEnd = estimatedTotal;
        for (uint32_t oi = 0; oi < outEnd; oi++) {
            float sp = oi * ratio_mp3;
            uint32_t si0 = (uint32_t)sp; float frac = sp - si0;
            if (si0 >= spf) si0 = spf - 1;
            uint32_t si1 = si0 + 1; if (si1 >= spf) si1 = spf - 1;
            amyBuf[oi] = (int16_t)((int32_t)frameBuf[si0] + (int32_t)((frameBuf[si1] - frameBuf[si0]) * frac));
        }
        inPos = spf;
        totalFrames = outEnd;
    }

    // Decode remaining frames
    int consErr = 0;
    // NOT "(!eof || bytesLeft > 0)" — see djDecodeMp3Chunk's identical loop (audio_engine.cpp,
    // DJ section) for why: the desktop simulator's mp3 decoder shim (libmpg123-backed,
    // sim_mp3dec.cpp) feeds its entire input buffer to the library per call (bytesLeft
    // always 0 after, success or fail) rather than consuming one frame at a time, so it can
    // still have real undrained audio buffered internally once the FILE'S bytes run out —
    // stopping right there silently truncated decodes on the simulator (confirmed: a ~175s
    // test track was cut to ~6.7s). Keep retrying (bytesLeft<=0 once eof makes the shim's
    // own feed step a no-op, so this just drains whatever's already buffered) until N
    // CONSECUTIVE calls make no progress — genuine exhaustion either way, reached in 1-2
    // tries on the real ESP32 Helix decoder's plain per-frame contract.
    while (totalFrames < estimatedTotal && !s_svcAbort) {
        if (bytesLeft < MAINBUF_SIZE && !eof) refill();
        int ret = MP3Decode(dec, &ptr, &bytesLeft, frameBuf, 0);
        if (ret == ERR_MP3_INDATA_UNDERFLOW) {
            if (eof) { if (++consErr > 32) break; continue; }
            refill(); consErr = 0; continue;
        }
        if (ret == ERR_MP3_MAINDATA_UNDERFLOW) {
            if (eof) { if (++consErr > 32) break; }
            continue;
        }
        if (ret < 0) {
            if (++consErr > 64) { Serial.println("MP3: too many errors, aborting"); break; }
            int sk = MP3FindSyncWord(ptr + 1, bytesLeft - 1);
            if (sk < 0) break; ptr += sk + 1; bytesLeft -= sk + 1; continue;
        }
        consErr = 0;

        MP3FrameInfo info; MP3GetLastFrameInfo(dec, &info);
        // Validate frame — corrupt data can produce nChans=0 (div-by-zero) or huge outputSamps (OOB)
        if (info.nChans < 1 || info.nChans > 2 || info.outputSamps < 1
            || info.outputSamps > (int)(MAX_NGRAN * MAX_NCHAN * MAX_NSAMP)) {
            Serial.printf("MP3: bad frame info ch=%d samps=%d, skipping\n", info.nChans, info.outputSamps);
            continue;
        }
        if (!gotInfo) { sampleRate=(uint32_t)info.samprate; numCh=(uint8_t)info.nChans; gotInfo=true; }

        uint32_t frOut = (uint32_t)info.outputSamps / (uint32_t)info.nChans;
        if (frOut == 0) break;

        // Downmix stereo→mono in-place
        if (info.nChans > 1)
            for (uint32_t j = 0; j < frOut; j++)
                frameBuf[j] = (int16_t)(((int32_t)frameBuf[j*2] + frameBuf[j*2+1]) >> 1);

        // Resample this frame from sampleRate → PCM_TARGET_RATE with linear interpolation
        float ratio_mp3 = (float)sampleRate / PCM_TARGET_RATE;
        uint32_t outEnd = (uint32_t)((inPos + frOut) / ratio_mp3);
        if (outEnd > estimatedTotal) outEnd = estimatedTotal;
        uint32_t outCnt = outEnd > totalFrames ? outEnd - totalFrames : 0;
        for (uint32_t oi = 0; oi < outCnt; oi++) {
            float sp = (totalFrames + oi) * ratio_mp3 - (float)inPos;
            if (sp < 0.0f) sp = 0.0f;
            uint32_t si0 = (uint32_t)sp; float frac = sp - si0;
            if (si0 >= frOut) si0 = frOut - 1;
            uint32_t si1 = si0 + 1; if (si1 >= frOut) si1 = frOut - 1;
            amyBuf[totalFrames + oi] = (int16_t)((int32_t)frameBuf[si0] + (int32_t)((frameBuf[si1] - frameBuf[si0]) * frac));
        }
        inPos += frOut;
        totalFrames = outEnd;
        if (totalFrames >= estimatedTotal) break;

        if (!played && totalFrames >= STREAM_PLAY_FRAMES) {
            amyPlayPcm(osc, preset, vel);
            played = true;
        }
        vTaskDelay(1);
    }

    if (!played) amyPlayPcm(osc, preset, vel);
    free(frameBuf);
    free(inBuf);
    MP3FreeDecoder(dec);
    f.close();
    // estimatedTotal is only a guess (from bitrate, or the full PSRAM cap when bitrate
    // detection failed e.g. on a large ID3v2 tag) — the preset was registered at that size,
    // so shrink it to the real decoded frame count in-place (same buffer, no realloc) or any
    // consumer that reads preset->length (granular2 slicing) will treat the unfilled silence
    // tail as real audio and compute wildly oversized slices.
    if (totalFrames > 0 && totalFrames < estimatedTotal)
        pcm_register_extern16(preset, amyBuf, totalFrames, PCM_TARGET_RATE / 2, 69, 0, totalFrames - 1);
    Serial.printf("MP3 done: %u/%u frames\n", totalFrames, estimatedTotal);
    if (osc >= SAMPLE_OSC_BASE && osc < SAMPLE_OSC_BASE + SAMPLE_KEY_COUNT && !s_svcAbort)
        s_keyLengthMs[osc - SAMPLE_OSC_BASE] = (uint32_t)((uint64_t)totalFrames * 1000 / PCM_TARGET_RATE);
    if (!s_svcAbort && totalFrames > 0) normalizeBuffer(amyBuf, totalFrames);
    if (!s_svcAbort && totalFrames > 0) applyBufferFades(amyBuf, totalFrames);
    if (isGran16) {
        if (!s_svcAbort && totalFrames > 0) svcWriteCache16(path, amyBuf, totalFrames, srcSize, PCM_TARGET_RATE);
    } else {
        if (!s_svcAbort && totalFrames > 0) svcWriteCache(path, amyBuf, totalFrames, srcSize, PCM_TARGET_RATE);
        if (!s_svcAbort && totalFrames > 0) flashCacheWrite(path, srcSize, amyBuf, totalFrames, PCM_TARGET_RATE);
    }
    return !s_svcAbort;
}

// ---- Persistent service task: processes load requests from queue one at a time ----
void bgServiceTask(void* /*param*/) {
    LoadReq req;
    for (;;) {
        xQueueReceive(s_loadQueue, &req, portMAX_DELAY);
        s_currentOsc = req.osc;
        s_svcDone    = false;
        s_svcAbort   = false;

        // Stop whatever was playing on this OSC before we overwrite its preset.
        amyStopOsc(req.osc);
        uint32_t tStop = millis();

        // If this is a key-slot load, mark unloaded until complete.
        uint8_t keyIdx = 0xFF;
        if (req.osc >= SAMPLE_OSC_BASE && req.osc < SAMPLE_OSC_BASE + SAMPLE_KEY_COUNT) {
            keyIdx = req.osc - SAMPLE_OSC_BASE;
            s_keyLoaded[keyIdx]   = false;
            s_keyLengthMs[keyIdx] = 0;
            s_keyError[keyIdx]    = KEY_ERR_NONE;  // clear previous error when retrying
        }

        if (keyIdx != 0xFF)
            Serial.printf("[KEY %u] loading %s  (psram %ukB / %ukB)\n",
                          keyIdx, req.path,
                          (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM)/1024,
                          (unsigned)heap_caps_get_total_size(MALLOC_CAP_SPIRAM)/1024);

        const char* ext = strrchr(req.path, '.');
        bool ok = false;
        try {
            if (ext && strcasecmp(ext, ".wav") == 0)
                ok = svcLoadWav(req.path, req.preset, req.osc, req.vel, tStop);
            else if (ext && strcasecmp(ext, ".mp3") == 0)
                ok = svcLoadMp3(req.path, req.preset, req.osc, req.vel, tStop);
            else
                Serial.printf("SVC: unknown format %s\n", ext ? ext : "(none)");
        } catch (...) {
            // Safety net: C++ exception from SD library or heap exhaustion — skip this file.
            Serial.printf("SVC: exception while loading %s, skipping\n", req.path);
            ok = false;
        }

        // Mark key as loaded only if the loader returned success (not aborted, no hard error).
        if (keyIdx != 0xFF) {
            if (ok) {
                s_keyLoaded[keyIdx] = true;
                s_keyError[keyIdx]  = KEY_ERR_NONE;
                // For SS2 slots (0-15): build a reversed copy in PSRAM for REV playback.
                s_keyHasRev[keyIdx] = false;
                if (keyIdx < 16) {
                    uint32_t rlen = 0;
                    const int16_t* src = pcm_get_sample_ram_for_preset(SAMPLE_PRESET_BASE + keyIdx, &rlen);
                    if (src && rlen > 0) {
                        int16_t* rev = pcm_load(SAMPLE_REV_PRESET_BASE + keyIdx, rlen, PCM_TARGET_RATE / 2, 1, 69, 0, 0);
                        if (rev) {
                            for (uint32_t ri = 0; ri < rlen; ri++) rev[ri] = src[rlen - 1 - ri];
                            s_keyHasRev[keyIdx] = true;
                        }
                    }
                }
                Serial.printf("[KEY %u] OK rev=%d  (psram %ukB free)\n",
                              keyIdx, (int)s_keyHasRev[keyIdx],
                              (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM)/1024);
            } else if (!s_svcAbort) {
                s_keyError[keyIdx] = s_loadError;
                Serial.printf("[KEY %u] FAIL err=%u  (psram %ukB free)\n",
                              keyIdx, s_loadError, (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM)/1024);
            }
        }
        // Signal granular source loaded (preset match, success, not aborted).
        if (ok && !s_svcAbort && req.preset == GRANULAR_SOURCE_PRESET)
            s_granularLoaded = true;
        for (uint8_t _s = 0; _s < GRAN2_MAX_SAMPLES; _s++) {
            if (ok && !s_svcAbort && req.preset == (uint16_t)(GRAN2_SOURCE_BASE + _s))
                s_gran2Loaded[_s] = true;
        }
        if (ok && !s_svcAbort && req.preset == STONE_SOURCE_PRESET) {
            s_stoneLoaded = true;
            audioStoneApplyWindow(0.0f, 1.0f, s_stoneLoop);  // full-range window; reflect current loop mode immediately, not just on the next pot nudge
        }
        // DJ no longer goes through this queue/task at all — see audioLoadDJTrack() and
        // djStreamDecodeTask() near the other DJ functions further down. An ongoing
        // "keep decoding chunks while the track plays" job can't live in this
        // single-consumer queue without starving every other load for as long as DJ
        // streams (confirmed: bgServiceTask blocks on one LoadReq at a time).
        s_currentOsc = 0xFF;
        s_svcDone    = true;
    }
}

// ---- Public API: non-blocking load + play ----

// Preview: aborts only the currently-running load (for instant response), then inserts the
// preview at the front of the queue. Pending key loads already in the queue are preserved —
// they resume automatically after the preview finishes.
void audioLoadAndPlay(const char* path, uint16_t preset, float vel) {
    if (!audioReady || !s_loadQueue) return;
    LoadReq req;
    strncpy(req.path, path, sizeof(req.path) - 1);
    req.path[sizeof(req.path) - 1] = '\0';
    req.preset = preset;
    req.osc    = PCM_PREVIEW_OSC;
    req.vel    = vel;
    // Abort whatever is currently being decoded (bgServiceTask checks s_svcAbort each batch,
    // so it exits within ~50ms). Queue is NOT cleared so auto-mapped loads continue after preview.
    s_svcAbort = true;
    xQueueSendToFront(s_loadQueue, &req, 0);
}

// Key assignment: queues background load into the key's dedicated RAM preset.
// Does NOT abort in-progress loads. Silently drops request if queue is full.
// ---- SS2 large-sample streaming: cheap load-time metadata probe ----
// Decides, at file-ASSIGNMENT time (not on every trigger), whether a key's file fits the
// existing full-decode-into-one-PSRAM-buffer path or needs to stream instead (see config.h's
// "SS2 large-sample streaming" block). Deliberately a FIXED-threshold check (against
// SS2_CHUNK_SECONDS), not psramMaxFrames()'s free-PSRAM-dependent one — the whole point is
// predictable behavior regardless of what else happens to be loaded. The fields captured
// here are cached in s_ssKeyMeta[] and reused by audioPlayKeyStreamed() on every trigger,
// so a large key's file header/bitrate is only ever read once, at assignment time.
struct SsKeyMeta {
    bool     isMp3 = false;
    float    estTotalSec = 0.0f;
    uint32_t wavDataStart = 0, wavBpf = 0, wavFileSR = 0, wavNumCh = 0, wavBps = 16, wavTotalInFrames = 0;
    bool     wavIsFloat = false;
    bool     hasPcmCache = false;  // a valid .pcm16 was found at audioLoadKey() time — see openValidPcmCache
};
static SsKeyMeta s_ssKeyMeta[SAMPLE_KEY_COUNT];

static bool ssParseWavHeaderInto(const char* path, SsKeyMeta& meta) {
    File f = SD.open(path, FILE_READ);
    if (!f) return false;
    uint8_t riff[12];
    if (f.read(riff, 12) < 12 || memcmp(riff, "RIFF", 4) || memcmp(riff + 8, "WAVE", 4)) { f.close(); return false; }
    uint16_t numCh = 1; uint32_t fileSR = AMY_SAMPLE_RATE; uint16_t bps = 16; uint16_t audioFmt = 1;
    uint32_t dataSize = 0; bool foundData = false; uint32_t dataStart = 0;
    int chunkIter = 0;
    while (f.available() > 8 && !foundData && ++chunkIter <= 32) {
        uint8_t ch[8]; if (f.read(ch, 8) < 8) break;
        uint32_t csz = (uint32_t)ch[4]|((uint32_t)ch[5]<<8)|((uint32_t)ch[6]<<16)|((uint32_t)ch[7]<<24);
        uint32_t cpos = (uint32_t)f.position();
        uint32_t fsize = (uint32_t)f.size();
        if (csz > fsize || cpos > fsize - csz) {
            if (!memcmp(ch, "data", 4)) { dataSize = fsize - cpos; dataStart = cpos; foundData = true; }
            else { f.close(); return false; }
            break;
        }
        if (!memcmp(ch, "fmt ", 4)) {
            uint8_t fmt[40] = {};
            uint32_t rd = csz < sizeof(fmt) ? csz : (uint32_t)sizeof(fmt);
            f.read(fmt, rd);
            f.seek(cpos + csz + (csz & 1));
            audioFmt = (uint16_t)(fmt[0]|(fmt[1]<<8));
            numCh    = (uint16_t)(fmt[2]|(fmt[3]<<8));
            fileSR   = (uint32_t)fmt[4]|(uint32_t)(fmt[5]<<8)|(uint32_t)(fmt[6]<<16)|(uint32_t)(fmt[7]<<24);
            bps      = (uint16_t)(fmt[14]|(fmt[15]<<8));
            if (audioFmt == 65534 && rd >= 26) audioFmt = (uint16_t)(fmt[24]|(fmt[25]<<8));
        } else if (!memcmp(ch, "data", 4)) {
            dataSize = (csz > 0) ? csz : (fsize - cpos);
            dataStart = cpos;
            foundData = true;
        } else {
            f.seek(cpos + csz + (csz & 1));
        }
    }
    f.close();
    if (!foundData || dataSize == 0 || fileSR == 0 || numCh == 0 || numCh > 8 || (audioFmt != 1 && audioFmt != 3))
        return false;
    meta.isMp3            = false;
    meta.wavIsFloat       = (audioFmt == 3 && bps == 32);
    meta.wavBps           = bps;
    meta.wavFileSR        = fileSR;
    meta.wavNumCh         = numCh;
    meta.wavBpf           = numCh * (bps / 8);
    meta.wavDataStart     = dataStart;
    meta.wavTotalInFrames = meta.wavBpf ? dataSize / meta.wavBpf : 0;
    meta.estTotalSec      = (float)meta.wavTotalInFrames / (float)fileSR;
    return true;
}

// Decodes just the first mp3 frame to read its real bitrate (MP3GetLastFrameInfo), then
// estimates totalSec = fileSize*8/bitrate — the same CBR-ratio approximation used
// throughout this file, real-hardware-accurate. On the desktop simulator's decoder shim
// specifically, the bitrate field is a hardcoded placeholder rather than a measured value
// (see the DJ section's own mp3-estimate comments for the same, already-documented,
// real-hardware-irrelevant limitation) — this quick check is simulator-imprecise the same
// way, harmless: worst case a borderline file goes down the "wrong" (but still working)
// path on the simulator only.
static bool ssMp3QuickDuration(const char* path, SsKeyMeta& meta) {
    File f = SD.open(path, FILE_READ);
    if (!f) return false;
    uint32_t fileSize = f.size();
    uint32_t tagBytes = mp3Id3v2Size(path);
    if (tagBytes >= fileSize) tagBytes = 0;
    mp3SkipId3v2(f);
    HMP3Decoder dec = MP3InitDecoder();
    if (!dec) { f.close(); return false; }
    const uint32_t kProbeBufSize = 4096;
    uint8_t* inBuf = (uint8_t*)malloc(kProbeBufSize);
    int16_t* frameBuf = inBuf ? (int16_t*)malloc(MAX_NGRAN * MAX_NCHAN * MAX_NSAMP * sizeof(int16_t)) : nullptr;
    bool ok = false;
    if (inBuf && frameBuf) {
        // A refilling loop, not a single one-shot 4096-byte read — the real ESP32 Helix
        // decoder typically finds a decodable frame within one window (this used to be a
        // single read+decode with no retry), but the desktop simulator's mpg123-backed decode
        // shim has different internal buffering semantics (it can drain the whole input into
        // its own internal state and report "need more" more than once before ever handing
        // back a decoded frame — see sim_mp3dec.cpp's own header comment) — without refilling,
        // a real, valid mp3 file could probe as "not decodable" on the simulator alone,
        // wrongly classifying a large file as small. Mirrors djDecodeMp3Chunk's own
        // refill-on-underflow loop, just bounded tighter since this only needs ONE frame.
        uint8_t* ptr = inBuf; int bytesLeft = 0; bool eof = false;
        auto refill = [&]() {
            if (ptr != inBuf && bytesLeft > 0) memmove(inBuf, ptr, bytesLeft);
            ptr = inBuf;
            if (!eof) {
                uint32_t space = kProbeBufSize - (uint32_t)bytesLeft;
                uint32_t rd = f.read(inBuf + bytesLeft, space);
                bytesLeft += (int)rd;
                if (rd < space) eof = true;
            }
        };
        refill();
        int sync = MP3FindSyncWord(ptr, bytesLeft);
        if (sync >= 0) {
            ptr += sync; bytesLeft -= sync;
            for (int tries = 0; tries < 16 && !ok; tries++) {
                if (bytesLeft < MAINBUF_SIZE && !eof) refill();
                int ret = MP3Decode(dec, &ptr, &bytesLeft, frameBuf, 0);
                if (ret == 0) {
                    MP3FrameInfo info; MP3GetLastFrameInfo(dec, &info);
                    if (info.bitrate > 0) {
                        meta.isMp3 = true;
                        meta.estTotalSec = (float)(fileSize - tagBytes) * 8.0f / (float)info.bitrate;
                        ok = true;
                    }
                    break;
                }
                if (ret == ERR_MP3_INDATA_UNDERFLOW) {
                    if (eof && bytesLeft <= 0) break;
                    refill();
                    continue;
                }
                if (ret != ERR_MP3_MAINDATA_UNDERFLOW) break;
                if (eof && bytesLeft <= 0) break;
            }
        }
    }
    if (inBuf) free(inBuf);
    if (frameBuf) free(frameBuf);
    MP3FreeDecoder(dec);
    f.close();
    return ok;
}

void audioLoadKey(const char* path, uint8_t keyIdx) {
    if (!audioReady || !s_loadQueue || keyIdx >= SAMPLE_KEY_COUNT) return;
    s_keyLoaded[keyIdx]  = false;
    s_keyIsLarge[keyIdx] = false;
    const char* ext = strrchr(path, '.');
    bool isMp3 = ext && strcasecmp(ext, ".mp3") == 0;
    SsKeyMeta meta;
    bool gotMeta = isMp3 ? ssMp3QuickDuration(path, meta) : ssParseWavHeaderInto(path, meta);
    if (isMp3 && gotMeta) {
        float hd = mp3HeaderDuration(path);
        if (hd > 1.0f) meta.estTotalSec = hd;
    }
    if (isMp3) {
        // A cached .pcm16's own frame count is exact — prefer it over the bitrate-estimate
        // probe above whenever both exist (the estimate can be meaningfully off for VBR
        // files; see ssMp3QuickDuration's own comment).
        File tmp = SD.open(path, FILE_READ);
        uint32_t srcSize = tmp ? tmp.size() : 0;
        if (tmp) tmp.close();
        File cacheFile; PcmCacheHdr cacheHdr;
        if (srcSize > 0 && openValidPcmCache(path, srcSize, cacheFile, cacheHdr)) {
            cacheFile.close();
            meta.isMp3 = true;
            meta.hasPcmCache = true;
            meta.estTotalSec = (float)cacheHdr.frameCount / (float)PCM_TARGET_RATE;
            gotMeta = true;
        }
    }
    if (gotMeta && meta.estTotalSec > (float)SS2_CHUNK_SECONDS) {
        // Too long for one chunk — stream instead of the usual full decode. No queueing:
        // there's nothing to decode yet, playback happens live from SD on first trigger
        // (audioPlayKeyStreamed()), so mark ready immediately.
        s_ssKeyMeta[keyIdx] = meta;
        strncpy(s_keyLargePath[keyIdx], path, sizeof(s_keyLargePath[0]) - 1);
        s_keyLargePath[keyIdx][sizeof(s_keyLargePath[0]) - 1] = '\0';
        s_keyIsLarge[keyIdx]  = true;
        s_keyLengthMs[keyIdx] = (uint32_t)(meta.estTotalSec * 1000.0f);
        s_keyHasRev[keyIdx]   = false;  // REV streams on demand now, no separate reversed copy needed
        s_keyError[keyIdx]    = KEY_ERR_NONE;
        s_keyLoaded[keyIdx]   = true;
        Serial.printf("[KEY %u] large file (~%.1fs), will stream: %s\n", keyIdx, meta.estTotalSec, path);
        return;
    }
    // Small (or metadata probe failed — fall back to the existing path, which has its own
    // error handling): queue exactly as before, unaffected.
    LoadReq req;
    strncpy(req.path, path, sizeof(req.path) - 1);
    req.path[sizeof(req.path) - 1] = '\0';
    req.preset = SAMPLE_PRESET_BASE + keyIdx;
    req.osc    = SAMPLE_OSC_BASE + keyIdx;
    req.vel    = 0.0f;  // load only; user triggers play with audioPlayKey()
    if (xQueueSend(s_loadQueue, &req, 0) != pdTRUE)
        Serial.printf("KEY %u: queue full, load dropped\n", keyIdx);
}

bool audioKeyIsLarge(uint8_t keyIdx) { return keyIdx < SAMPLE_KEY_COUNT && s_keyIsLarge[keyIdx]; }

// Play the RAM-loaded sample for a key.
void audioPlayKey(uint8_t keyIdx, float vel) {
    if (!audioReady || keyIdx >= SAMPLE_KEY_COUNT) return;
    float v = vel * s_sampleVolume;
    if (v > 2.0f) v = 2.0f;
    amy_event e = amy_default_event();
    e.osc       = SAMPLE_OSC_BASE + keyIdx;
    e.wave      = PCM;
    e.preset    = SAMPLE_PRESET_BASE + keyIdx;
    e.midi_note = 69;
    e.velocity  = v;
    // 5ms fade-in to suppress click; 40ms release for smooth stop on note-off
    e.eg0_times[0] = 5;   e.eg0_values[0] = 1.0f;
    e.eg0_times[1] = 0;   e.eg0_values[1] = 1.0f;
    e.eg0_times[2] = 40;  e.eg0_values[2] = 0.0f;
    // feedback=0 prevents PCM loop (AMY uses feedback flag to gate looping in pcm_note_on)
    e.feedback = 0.0f;
    if (s_pcmLPFCutoff > 10.0f) {
        e.filter_type = s_pcmLPFType;
        e.filter_freq_coefs[COEF_CONST] = s_pcmLPFCutoff;
        e.resonance = s_pcmLPFReso;
    } else {
        e.filter_type = FILTER_NONE;
    }
    amy_add_event(&e);
}

// Play reversed sample for SS2 slots (keyIdx 0-15). Falls back to forward if no reversed preset.
void audioPlayKeyRev(uint8_t keyIdx, float vel) {
    if (!audioReady || keyIdx >= SAMPLE_KEY_COUNT) return;
    if (!s_keyHasRev[keyIdx]) { audioPlayKey(keyIdx, vel); return; }
    float v = vel * s_sampleVolume;
    if (v > 2.0f) v = 2.0f;
    amy_event e = amy_default_event();
    e.osc       = SAMPLE_OSC_BASE + keyIdx;
    e.wave      = PCM;
    e.preset    = SAMPLE_REV_PRESET_BASE + keyIdx;
    e.midi_note = 69;
    e.velocity  = v;
    e.eg0_times[0] = 5;   e.eg0_values[0] = 1.0f;
    e.eg0_times[1] = 0;   e.eg0_values[1] = 1.0f;
    e.eg0_times[2] = 40;  e.eg0_values[2] = 0.0f;
    e.feedback = 0.0f;
    if (s_pcmLPFCutoff > 10.0f) {
        e.filter_type = s_pcmLPFType;
        e.filter_freq_coefs[COEF_CONST] = s_pcmLPFCutoff;
        e.resonance = s_pcmLPFReso;
    } else {
        e.filter_type = FILTER_NONE;
    }
    amy_add_event(&e);
}

bool audioKeyLoaded(uint8_t keyIdx) {
    if (keyIdx >= SAMPLE_KEY_COUNT) return false;
    return s_keyLoaded[keyIdx];
}

uint8_t audioKeyError(uint8_t keyIdx) {
    if (keyIdx >= SAMPLE_KEY_COUNT) return KEY_ERR_NONE;
    return s_keyError[keyIdx];
}

void audioClearAllKeys() {
    // Stop all sample oscillators so AMY stops reading PCM buffers before we free them.
    audioStopAllSamples();

    // Abort bgServiceTask FIRST — it may be writing into a preset buffer right now.
    // We must stop the writer before calling pcm_unload_preset, otherwise pcm_unload_preset
    // frees PSRAM that bgServiceTask is still writing into → corruption.
    if (s_loadQueue) {
        s_svcAbort = true;
        xQueueReset(s_loadQueue);
        // Wait up to 300ms for bgServiceTask to notice the abort and finish its current chunk.
        for (int i = 0; i < 30 && !s_svcDone; i++)
            vTaskDelay(pdMS_TO_TICKS(10));
    }

    // Also wait a bit for AMY to finish reading any PCM data (process its event queue).
    vTaskDelay(pdMS_TO_TICKS(20));

    // Unload only sample-mode presets (200-231) and the preview slot (100).
    // pcm_unload_all_presets() would also destroy drum presets (101-132),
    // causing all drums to fall back to the same ROM preset after this call.
    pcm_unload_preset(PCM_PREVIEW_PRESET);
    for (int i = 0; i < SAMPLE_KEY_COUNT; i++)
        pcm_unload_preset(SAMPLE_PRESET_BASE + i);
    for (int i = 0; i < 16; i++)  // SS2 reversed presets
        pcm_unload_preset(SAMPLE_REV_PRESET_BASE + i);

    memset((void*)s_keyLoaded,   0, sizeof(s_keyLoaded));
    memset((void*)s_keyLengthMs, 0, sizeof(s_keyLengthMs));
    memset((void*)s_keyError,    0, sizeof(s_keyError));
    memset(s_keyHasRev,          0, sizeof(s_keyHasRev));
}

void audioStopKey(uint8_t keyIdx) {
    if (!audioReady || keyIdx >= SAMPLE_KEY_COUNT) return;
    amyStopOsc(SAMPLE_OSC_BASE + keyIdx);
}

uint32_t audioKeyLengthMs(uint8_t keyIdx) {
    if (keyIdx >= SAMPLE_KEY_COUNT) return 0;
    return s_keyLengthMs[keyIdx];
}

// ==================== AUTO-DETECT FORMAT (legacy synchronous wrappers) ====================
bool audioLoadFromSD(const char* path, uint16_t preset) {
    audioLoadAndPlay(path, preset, 0.0f);   // vel=0: caller must call audioPlaySamplePreset
    return true;
}
bool audioLoadWavFromSD(const char* path, uint16_t preset) { return audioLoadFromSD(path, preset); }
bool audioLoadMp3FromSD(const char* path, uint16_t preset) { return audioLoadFromSD(path, preset); }

void audioSetSampleVolume(float v) {
    if (v < 0.0f) v = 0.0f;
    if (v > 2.0f) v = 2.0f;
    s_sampleVolume = v;
}

// Forward declarations for SW2 (defined later; called from T303NoteOn/Off)
void audioSW2NoteOn(uint8_t note, float vel);
void audioSW2NoteOff(uint8_t note);

// ==================== TB-303 ENGINE ====================
// Monophonic synth on T303_CH: SAW or SQUARE, resonant 4-pole LPF,
// amp envelope (EG0) + filter envelope (EG1 via COEF_EG1), portamento via pitch_bend.

// Single EG (EG0) controls both amp AND filter (like the real 303).
// Decay = P5 (50ms tight pluck → 3000ms long sustained bass).
// Sustain level and decay applied only at NoteOn to avoid EG restart mid-note.
static float s_t303Attack  = 2.0f;
static float s_t303Decay   = 500.0f;
static float s_t303Sustain = 0.0f;  // 0=note decays to silence, 1=held at full amp until note-off
static float s_t303Release = 30.0f;
static bool  s_sw2Active   = false;  // true when T303_SW2_WAVE is selected
extern "C" float amy_wavefold_gain;       // defined in amy.c; controls 303 bus-1 wavefolder (1.0=dry)
extern "C" float amy_wavefold_pos_only;   // defined in amy.c; >0.5 = fold positive half only (TRI2)
extern "C" float amy_wavefold_gain_bus0;  // defined in amy.c; global REP FX on bus 0 (1.0=dry)
extern "C" float amy_ladder_on;         // defined in amy.c; 0=bypass, 1=engaged
extern "C" float amy_ladder_cutoff;     // defined in amy.c; Hz
extern "C" float amy_ladder_resonance;  // defined in amy.c; feedback gain into tanh(), no hard ceiling
extern "C" float amy_ringmod_on;        // defined in amy.c; 0=bypass, 1=engaged
extern "C" float amy_ringmod_freq;      // defined in amy.c; carrier Hz
extern "C" float amy_ringmod_mix;       // defined in amy.c; 0=dry, 1=fully ring-modulated
extern "C" float amy_comp_on;           // defined in amy.c; 0=bypass, 1=engaged
extern "C" float amy_comp_threshold;    // defined in amy.c; linear (bus headroom, not dBFS)
extern "C" float amy_comp_ratio;        // defined in amy.c; 1.0=no compression, higher=more limiting

void audioT303Init(float cutoff, float reso, float envMod, float decay, uint8_t amyWave) {
    if (!audioReady) return;
    amy_event e = amy_default_event();
    e.synth          = T303_CH;
    e.bus            = 1;  // dedicated bus for wavefolder processing
    e.num_voices     = 1;
    e.oscs_per_voice = 1;
    e.wave           = amyWave;
    e.filter_type    = FILTER_LPF24;
    e.resonance      = reso;
    e.filter_freq_coefs[COEF_CONST] = cutoff;
    e.filter_freq_coefs[COEF_EG0]   = envMod;  // filter tied to amp EG — single EG, no conflict
    e.eg0_times[0] = 2.0f;   e.eg0_values[0] = 1.0f;
    e.eg0_times[1] = decay;  e.eg0_values[1] = s_t303Sustain;
    e.eg0_times[2] = 30.0f;  e.eg0_values[2] = 0.0f;
    amy_add_event(&e);
    s_t303Attack  = 2.0f;
    s_t303Decay   = decay;
    s_t303Release = 30.0f;
}

void audioT303NoteOn(uint8_t midiNote, float vel) {
    if (!audioReady) return;
    // Send full EG spec before the note trigger so AMY picks up the latest decay.
    // AMY's bp processing requires eg0_times[0] to be set (index 0 is the gate check);
    // sending only index 1 silently skips the entire breakpoint update.
    { amy_event e = amy_default_event();
      e.synth = T303_CH;
      e.eg0_times[0]  = s_t303Attack;              e.eg0_values[0] = 1.0f;
      e.eg0_times[1]  = (uint32_t)s_t303Decay;   e.eg0_values[1] = s_t303Sustain;
      e.eg0_times[2]  = s_t303Release;            e.eg0_values[2] = 0.0f;
      amy_add_event(&e); }
    { amy_event e = amy_default_event();
      e.synth     = T303_CH;
      e.midi_note = s_t303Noise ? 60 : midiNote;
      e.velocity  = vel;
      amy_add_event(&e); }
    // Wavefolding applied per-bus in amy_fill_buffer() — no extra oscillators needed.
    if (s_sw2Active) audioSW2NoteOn(midiNote, vel);
}

void audioT303NoteOff(uint8_t midiNote) {
    if (!audioReady) return;
    amy_event e = amy_default_event();
    e.synth     = T303_CH;
    e.midi_note = s_t303Noise ? 60 : midiNote;
    e.velocity  = 0.0f;
    amy_add_event(&e);
    if (s_sw2Active) audioSW2NoteOff(midiNote);
}

void audioT303Params(float cutoff, float reso, float envMod, float decay) {
    if (!audioReady) return;
    s_t303Decay = decay;  // stored for next note-on; never sent mid-note (no EG restart)
    amy_event e = amy_default_event();
    e.synth     = T303_CH;
    e.resonance = reso;
    e.filter_freq_coefs[COEF_CONST] = cutoff;
    e.filter_freq_coefs[COEF_EG0]   = envMod;  // filter tied to amp EG
    amy_add_event(&e);
}

void audioT303SetSustain(float sustain) {
    s_t303Sustain = sustain;
}

void audioT303PitchBend(float ratio) {
    if (!audioReady) return;
    amy_event e = amy_default_event();
    e.synth      = T303_CH;
    e.pitch_bend = ratio;
    amy_add_event(&e);
}

void audioT303SetAmpEnv(float /*atkMs*/, float /*sus*/, float /*relMs*/) {
    // No-op: monophonic 303 uses its own single EG (decay+sustain via audioT303SetSustain + pot).
}

void audioI303SetAmpEnv(float atkMs, float sus, float decMs, float relMs) {
    s_t303Attack  = atkMs;
    s_t303Decay   = decMs;
    s_t303Sustain = sus;
    s_t303Release = relMs;
}

// ==================== SxF (sous-octaves : T303_CH base + f-1 + f-2) ====================
// SWF/SQF/SNF sont des types de vague dans 303S/I303.
// T303_CH joue la note de base ; sous-canaux SW2_CH_BASE+0 (f-1) et +1 (f-2) ajoutent
// des copies à -12 et -24 demi-tons (octave en dessous, puis deux octaves).
// audioT303NoteOn/Off déclenche automatiquement audioSW2NoteOn/Off si s_sw2Active==true.
static const int8_t s_sw2_offsets[2] = {-12, -24};  // f-1 et f-2 (sous-octaves)

void audioSW2Init(float cutoff, float reso, float decay, uint8_t numVoices, uint8_t wave) {
    if (!audioReady) return;
    // Init 2 sous-canaux : même vague que la base, filtre LP statique pour l'épaisseur
    for (uint8_t i = 0; i < 2; i++) {
        amy_event e = amy_default_event();
        e.synth = SW2_CH_BASE + i;
        e.num_voices = numVoices;
        e.oscs_per_voice = 1;
        e.wave = wave;
        e.bus = 0;  // bus dry — pas de wavefolder sur les sous-octaves
        e.filter_type = FILTER_LPF24;
        e.resonance = reso * 0.5f;  // réso plus douce pour éviter la dureté
        e.filter_freq_coefs[COEF_CONST] = cutoff;
        e.eg0_times[0] = (uint32_t)s_t303Attack; e.eg0_values[0] = 1.0f;
        e.eg0_times[1] = (uint32_t)decay;        e.eg0_values[1] = s_t303Sustain;
        e.eg0_times[2] = (uint32_t)s_t303Release; e.eg0_values[2] = 0.0f;
        e.amp_coefs[COEF_CONST] = 0.0f;  // silencieux jusqu'à blend
        amy_add_event(&e);
    }
    s_sw2Active = true;
}

void audioSW2Deactivate() {
    if (!audioReady) return;
    s_sw2Active = false;
    for (uint8_t i = 0; i < 2; i++) {
        amy_event e = amy_default_event();
        e.synth = SW2_CH_BASE + i; e.amp_coefs[COEF_CONST] = 0.0f;
        amy_add_event(&e);
    }
    for (uint8_t n = 0; n <= 127; n++) {
        for (uint8_t ch = 0; ch < 2; ch++) {
            amy_event e = amy_default_event();
            e.synth = SW2_CH_BASE + ch; e.midi_note = n; e.velocity = 0.0f;
            amy_add_event(&e);
        }
    }
    { amy_event e = amy_default_event(); e.synth = T303_CH; e.amp_coefs[COEF_CONST] = 1.0f; amy_add_event(&e); }
}

void audioSW2SetBlend(float p2) {
    if (!audioReady) return;
    float a = (p2 < 0.5f) ? p2 * 2.0f : 1.0f;   // f-1 : active 0→50%
    float b = (p2 < 0.5f) ? 0.0f : (p2 - 0.5f) * 2.0f;  // f-2 : active 50→100%
    { amy_event e = amy_default_event(); e.synth = T303_CH;        e.amp_coefs[COEF_CONST] = 1.0f;       amy_add_event(&e); }
    { amy_event e = amy_default_event(); e.synth = SW2_CH_BASE+0;  e.amp_coefs[COEF_CONST] = a * 0.5f;  amy_add_event(&e); }
    { amy_event e = amy_default_event(); e.synth = SW2_CH_BASE+1;  e.amp_coefs[COEF_CONST] = b * 0.5f;  amy_add_event(&e); }
}

void audioSW2NoteOn(uint8_t note, float vel) {
    if (!audioReady || !s_sw2Active) return;
    for (uint8_t i = 0; i < 2; i++) {
        int n = (int)note + s_sw2_offsets[i];  // f-1 = note-12, f-2 = note-24
        if (n < 0 || n > 127) continue;
        { amy_event e = amy_default_event();
          e.synth = SW2_CH_BASE + i;
          e.eg0_times[0] = (uint32_t)s_t303Attack; e.eg0_values[0] = 1.0f;
          e.eg0_times[1] = (uint32_t)s_t303Decay;  e.eg0_values[1] = s_t303Sustain;
          e.eg0_times[2] = (uint32_t)s_t303Release; e.eg0_values[2] = 0.0f;
          amy_add_event(&e); }
        { amy_event e = amy_default_event();
          e.synth = SW2_CH_BASE + i; e.midi_note = (uint8_t)n; e.velocity = vel;
          amy_add_event(&e); }
    }
}

void audioSW2NoteOff(uint8_t note) {
    if (!audioReady || !s_sw2Active) return;
    for (uint8_t i = 0; i < 2; i++) {
        int n = (int)note + s_sw2_offsets[i];
        if (n < 0 || n > 127) continue;
        amy_event e = amy_default_event();
        e.synth = SW2_CH_BASE + i; e.midi_note = (uint8_t)n; e.velocity = 0.0f;
        amy_add_event(&e);
    }
}

void audioSW2AllNotesOff() {
    if (!audioReady) return;
    for (uint8_t n = 0; n <= 127; n++) {
        for (uint8_t ch = 0; ch < 2; ch++) {
            amy_event e = amy_default_event();
            e.synth = SW2_CH_BASE + ch; e.midi_note = n; e.velocity = 0.0f;
            amy_add_event(&e);
        }
    }
}

void audioT303Wave(uint8_t amyWave) {
    if (!audioReady) return;
    s_t303Noise = (amyWave == NOISE);
    amy_event e = amy_default_event();
    e.synth = T303_CH;
    e.wave  = amyWave;
    amy_add_event(&e);
}
void audioT303Feedback(float fb) {
    if (!audioReady) return;
    amy_event e = amy_default_event();
    e.synth    = T303_CH;
    e.feedback = fb;
    amy_add_event(&e);
}

void audioT303Duty(float duty) {
    if (!audioReady) return;
    amy_event e = amy_default_event();
    e.synth          = T303_CH;
    e.duty_coefs[0]  = duty;  // COEF_CONST: continuous pulse width control
    amy_add_event(&e);
}

// Polyphonic 303 (I303 mode): same engine as T303 but 6 voices for chord/melody play.
void audioI303Init(float cutoff, float reso, float envMod, float decay, uint8_t amyWave) {
    if (!audioReady) return;
    amy_event e = amy_default_event();
    e.synth          = T303_CH;
    e.bus            = 1;
    e.num_voices     = 6;
    e.oscs_per_voice = 1;
    e.wave           = amyWave;
    e.filter_type    = FILTER_LPF24;
    e.resonance      = reso;
    e.filter_freq_coefs[COEF_CONST] = cutoff;
    e.filter_freq_coefs[COEF_EG0]   = envMod;
    e.eg0_times[0] = 2.0f;      e.eg0_values[0] = 1.0f;
    e.eg0_times[1] = 30000.0f; e.eg0_values[1] = 1.0f;  // hold at full amp until note-off
    e.eg0_times[2] = 80.0f;    e.eg0_values[2] = 0.0f;  // 80ms release
    amy_add_event(&e);
    s_t303Attack  = 2.0f;
    s_t303Decay   = 30000.0f;
    s_t303Sustain = 1.0f;
    s_t303Release = 80.0f;
}

void audioSetWavefold(float gain) {
    amy_wavefold_gain_bus0 = gain;
}

void audioSetLadderFilter(float cutoffHz, float resonance, bool on) {
    amy_ladder_on        = on ? 1.0f : 0.0f;
    amy_ladder_cutoff     = cutoffHz;
    amy_ladder_resonance  = resonance;
}

void audioSetRingmod(float freqHz, float mix, bool on) {
    amy_ringmod_on   = on ? 1.0f : 0.0f;
    amy_ringmod_freq = freqHz;
    amy_ringmod_mix  = mix;
}

void audioSetCompressor(float threshold, float ratio, bool on) {
    amy_comp_on        = on ? 1.0f : 0.0f;
    amy_comp_threshold = threshold;
    amy_comp_ratio     = ratio;
}

// AUTOPAN FX: per-oscillator equal-power pan (AMY's msynth->pan, real stereo — the
// board's I2S output is genuinely stereo, see i2s.c's I2S_SLOT_MODE_STEREO). 0=full
// left, 0.5=center, 1=full right. Applied to the two channels most modes actually
// play notes on; other channels (T303/tracker/etc.) stay centered — this mirrors
// how audioSetVolume only ever targets SYNTH_CH plus T303's shared bus.
void audioSetPan(float pan) {
    if (!audioReady) return;
    amy_event e = amy_default_event();
    e.synth = SYNTH_CH;
    e.pan_coefs[COEF_CONST] = pan;
    amy_add_event(&e);
}

// Symmetric wavefolder for TRI/SAW2/SQ2: depth 0→1 maps gain 1x→32x (extreme folds).
void audioT303Wavefold(float depth) {
    amy_wavefold_pos_only = 0.0f;
    if (depth < 0.01f) {
        amy_wavefold_gain = 1.0f;
    } else {
        amy_wavefold_gain = powf(32.0f, depth);  // 1→32 as depth 0→1
    }
}
// Asymmetric wavefolder for TRI2/SAW3: folds only positive peaks, bass retained.
void audioT303WavefoldAsym(float depth) {
    amy_wavefold_pos_only = 1.0f;
    if (depth < 0.01f) {
        amy_wavefold_gain = 1.0f;
    } else {
        amy_wavefold_gain = powf(32.0f, depth);  // 1→32 as depth 0→1
    }
}

// ==================== DRUM2 (per-pad pitch/decay control) ====================
// Extends audioPlayDrumPad: custom MIDI note (pitch) and optional EG decay override.
void audioDrum2Hit(uint8_t padIdx, float vel, uint8_t midiNote, float decayMs) {
    if (!audioReady || padIdx >= DRUM_PAD_COUNT) return;
    amy_event e = amy_default_event();
    e.osc       = DRUM_OSC_BASE + padIdx;
    e.wave      = PCM;
    e.preset    = DRUM_PRESET_BASE + padIdx;
    e.midi_note = midiNote;
    e.velocity  = vel;
    if (decayMs > 0.0f) {
        e.eg0_times[0]  = 1.0f;   e.eg0_values[0] = 1.0f;
        e.eg0_times[1]  = decayMs; e.eg0_values[1] = 0.0f;
        e.eg0_times[2]  = 5.0f;   e.eg0_values[2] = 0.0f;
    }
    if (s_pcmLPFCutoff > 10.0f) {
        e.filter_type = s_pcmLPFType;
        e.filter_freq_coefs[COEF_CONST] = s_pcmLPFCutoff;
        e.resonance = s_pcmLPFReso;
    } else {
        e.filter_type = FILTER_NONE; // explicitly clear — oscillator retains previous filter_type otherwise
    }
    amy_add_event(&e);
}

// ==================== GRANULAR SLICER ====================
static volatile bool s_granularReady  = false;
// s_granularLoaded declared near top (used by bgServiceTask)
static float s_granWinStart = 0.0f;
static float s_granWinEnd   = 1.0f;

void audioLoadGranularSource(const char* path) {
    s_granularReady  = false;
    s_granularLoaded = false;
    s_granWinStart   = 0.0f;
    s_granWinEnd     = 1.0f;
    // Stop all granular OSCs before freeing old slice presets.
    for (int i = 0; i < 32; i++) {
        amy_event e = amy_default_event();
        e.osc = (uint16_t)(GRANULAR_OSC_BASE + i); e.velocity = 0;
        amy_add_event(&e);
    }
    // Free slice presets (they point into the old source buffer via pcm_register_extern16).
    for (int i = 0; i < GRANULAR_MAX_SLICES; i++) pcm_unload_preset(GRANULAR_PRESET_BASE + i);
    for (int i = 0; i < 8; i++)                   pcm_unload_preset(GRANULAR_REV_PRESET  + i);
    for (int i = 0; i < 8; i++)                   pcm_unload_preset(GRANULAR_DBL_PRESET  + i);
    for (int i = 0; i < 8; i++)                   pcm_unload_preset(GRANULAR_TAIL_PRESET + i);
    // Queue source load; vel=0 so bgServiceTask doesn't auto-play.
    audioLoadAndPlay(path, GRANULAR_SOURCE_PRESET, 0.0f);
}

bool audioIsGranularReady() { return s_granularReady; }

void audioSetGranularWindow(float startFrac, float endFrac) {
    if (startFrac < 0.0f) startFrac = 0.0f;
    if (endFrac   > 1.0f) endFrac   = 1.0f;
    if (endFrac - startFrac < 0.05f) endFrac = startFrac + 0.05f;
    s_granWinStart = startFrac;
    s_granWinEnd   = endFrac;
}

uint8_t audioComputeGranularSlices(uint8_t mode, uint8_t* waveform128) {
    uint32_t totalLen = 0;
    const int16_t* src = pcm_get_sample_ram_for_preset(GRANULAR_SOURCE_PRESET, &totalLen);
    if (!src || totalLen < 16) return 0;

    // Waveform: always the full source (128px), so the user sees context around the window.
    if (waveform128) {
        uint32_t blk = totalLen / 128;
        if (blk < 1) blk = 1;
        for (int i = 0; i < 128; i++) {
            uint32_t s = (uint32_t)i * blk, e2 = s + blk;
            if (e2 > totalLen) e2 = totalLen;
            int32_t peak = 0;
            for (uint32_t j = s; j < e2; j++) {
                int32_t v = src[j]; if (v < 0) v = -v;
                if (v > peak) peak = v;
            }
            waveform128[i] = (uint8_t)((int32_t)peak * 255 / 32768);
        }
    }

    // Windowed slice range.
    uint32_t winStart = (uint32_t)(s_granWinStart * (float)totalLen);
    uint32_t winEnd   = (uint32_t)(s_granWinEnd   * (float)totalLen);
    if (winEnd > totalLen) winEnd = totalLen;
    if (winEnd <= winStart + 16) winEnd = winStart + 16;
    const int16_t* wsrc  = src + winStart;
    uint32_t       wlen  = winEnd - winStart;

    const uint32_t rate = PCM_TARGET_RATE / 2;  // AMY 2× hardware compensation

    if (mode == 0) {
        // 8-slice: R0=one-shot px, R1=double px+px+1, R2=tail sx→end, R3=px reversed
        uint32_t s8 = wlen / 8;
        for (int i = 0; i < 8; i++) {
            uint32_t start = (uint32_t)i * s8;
            uint32_t len1  = (start + s8     <= wlen) ? s8     : wlen - start;  // one-shot
            uint32_t len2  = (start + s8 * 2 <= wlen) ? s8 * 2 : wlen - start;  // double
            uint32_t lenT  = wlen - start;                                        // tail to end

            // loopend = len-1 ensures AMY loops the full slice (loopend=0 → 1-sample loop → buzz)
            pcm_register_extern16(GRANULAR_PRESET_BASE  + i, wsrc + start, len1, rate, 69, 0, len1 > 0 ? len1 - 1 : 0);
            pcm_register_extern16(GRANULAR_DBL_PRESET   + i, wsrc + start, len2, rate, 69, 0, len2 > 0 ? len2 - 1 : 0);
            pcm_register_extern16(GRANULAR_TAIL_PRESET  + i, wsrc + start, lenT, rate, 69, 0, lenT > 0 ? lenT - 1 : 0);
            int16_t* rbuf = pcm_load(GRANULAR_REV_PRESET + i, len1, rate, 1, 69, 0, 0);
            if (rbuf) for (uint32_t j = 0; j < len1; j++) rbuf[j] = wsrc[start + len1 - 1 - j];
        }
        s_granLastSliceCount = 8;
        s_granularReady = true;
        return 8;
    } else {
        // 1/16 energy-ranked within window
        uint32_t s16 = wlen / 16;
        float energy[16] = {};
        for (int i = 0; i < 16; i++) {
            uint32_t s = (uint32_t)i * s16, e2 = s + s16;
            if (e2 > wlen) e2 = wlen;
            double sum = 0;
            for (uint32_t j = s; j < e2; j++) { float v = wsrc[j] / 32768.f; sum += v * v; }
            energy[i] = (float)(sum / (e2 - s));
        }
        uint8_t ord[16]; for (int i = 0; i < 16; i++) ord[i] = i;
        for (int i = 0; i < 15; i++)
            for (int j = 0; j < 15 - i; j++)
                if (energy[ord[j]] < energy[ord[j+1]]) { uint8_t t = ord[j]; ord[j] = ord[j+1]; ord[j+1] = t; }
        for (int i = 0; i < 16; i++) {
            uint32_t start = (uint32_t)ord[i] * s16;
            uint32_t len   = (start + s16 <= wlen) ? s16 : wlen - start;
            pcm_register_extern16(GRANULAR_PRESET_BASE + i, wsrc + start, len, rate, 69, 0, len > 0 ? len - 1 : 0);
        }
        s_granLastSliceCount = 16;
        s_granularReady = true;
        return 16;
    }
}

void audioApplyGranularSplits(float* splits, int N) {
    if (!splits || N < 1 || s_granLastSliceCount == 0) return;
    uint32_t totalLen = 0;
    const int16_t* src = pcm_get_sample_ram_for_preset(GRANULAR_SOURCE_PRESET, &totalLen);
    if (!src || totalLen < 16) return;

    uint32_t winS = (uint32_t)(s_granWinStart * (float)totalLen);
    uint32_t winE = (uint32_t)(s_granWinEnd   * (float)totalLen);
    if (winE > totalLen) winE = totalLen;
    if (winE <= winS + 16) return;
    uint32_t wlen = winE - winS;
    const int16_t* wsrc = src + winS;
    const uint32_t rate = PCM_TARGET_RATE / 2;

    for (int i = 0; i < N; i++) {
        float fS  = splits[i];
        float fE  = splits[i + 1];
        float fE2 = (i + 2 <= N) ? splits[i + 2] : 1.0f;

        // splits[] is guaranteed non-decreasing by the caller, but clamp defensively.
        // Minimum 2 frames so that loopend = len-1 >= 1 (avoids % 0 in AMY loop path).
        uint32_t s0 = (uint32_t)(constrain(fS,  0.0f, 1.0f) * (float)wlen);
        uint32_t e0 = (uint32_t)(constrain(fE,  0.0f, 1.0f) * (float)wlen);
        uint32_t e1 = (uint32_t)(constrain(fE2, 0.0f, 1.0f) * (float)wlen);
        if (s0 >= wlen) s0 = wlen - 2;
        if (e0 < s0 + 2) e0 = s0 + 2;   // at least 2 frames so loopend >= 1
        if (e0 > wlen)   e0 = wlen;
        if (e1 < s0 + 2) e1 = s0 + 2;
        if (e1 > wlen)   e1 = wlen;

        uint32_t len0 = e0 - s0;    // one-shot px
        uint32_t len1 = e1 - s0;    // double px+px+1
        uint32_t lenT = wlen - s0;  // tail sx→end

        // Only pcm_register_extern16 here — safe to call from main loop concurrently with audio
        // thread because it only updates a pointer (no memory allocation/deallocation).
        // Reverse presets (pcm_load = PSRAM alloc) are NOT updated here to avoid use-after-free
        // crash: audio thread may be reading the old buffer while main loop frees it.
        pcm_register_extern16(GRANULAR_PRESET_BASE + i, wsrc + s0, len0, rate, 69, 0, len0 - 1);
        pcm_register_extern16(GRANULAR_DBL_PRESET  + i, wsrc + s0, len1, rate, 69, 0, len1 - 1);
        pcm_register_extern16(GRANULAR_TAIL_PRESET + i, wsrc + s0, lenT, rate, 69, 0, lenT - 1);
        // Note: GRANULAR_REV_PRESET stays at initial boundaries from audioComputeGranularSlices.
    }
}

void audioPlayGranularSlice(uint8_t keyOscIdx, uint16_t slicePreset, float vel, bool loop) {
    if (!audioReady) return;
    amy_event e = amy_default_event();
    e.osc       = (uint16_t)(GRANULAR_OSC_BASE + keyOscIdx);
    e.wave      = PCM;
    e.preset    = slicePreset;
    e.midi_note = 69;
    e.velocity  = vel;
    e.feedback  = loop ? 1.0f : 0.0f;
    // EG0: 5ms attack (anti-click ramp-in), sustain at 1.0, 10ms release (anti-click ramp-out).
    // amp_coefs left at AMY defaults (CONST=1, VEL=1, EG0=1) — setting COEF_CONST=0 maps to
    // map_60dB(0)=-10 which pushes amp to ~0 regardless of EG0 level (complete silence).
    e.eg0_times[0] = 5;    e.eg0_values[0] = 1.0f;
    e.eg0_times[1] = 1;    e.eg0_values[1] = 1.0f;
    e.eg0_times[2] = 10;   e.eg0_values[2] = 0.0f;
    amy_add_event(&e);
}

void audioStopGranularOsc(uint8_t keyOscIdx) {
    if (!audioReady) return;
    amy_event e = amy_default_event();
    e.osc = (uint16_t)(GRANULAR_OSC_BASE + keyOscIdx); e.velocity = 0;
    amy_add_event(&e);
}

void audioUnloadGranular() {
    // 1. Silence all granular OSCs.
    for (int i = 0; i < 32; i++) {
        amy_event e = amy_default_event();
        e.osc = (uint16_t)(GRANULAR_OSC_BASE + i); e.velocity = 0;
        amy_add_event(&e);
    }
    // 2. Let the audio thread finish the current render block before freeing.
    vTaskDelay(pdMS_TO_TICKS(20));
    // 3. Free slice presets first (they alias into source buffer via pcm_register_extern16).
    for (int i = 0; i < GRANULAR_MAX_SLICES; i++) pcm_unload_preset(GRANULAR_PRESET_BASE + i);
    for (int i = 0; i < 8; i++)                   pcm_unload_preset(GRANULAR_REV_PRESET  + i);
    for (int i = 0; i < 8; i++)                   pcm_unload_preset(GRANULAR_DBL_PRESET  + i);
    for (int i = 0; i < 8; i++)                   pcm_unload_preset(GRANULAR_TAIL_PRESET + i);
    // 4. Now safe to free the source buffer.
    pcm_unload_preset(GRANULAR_SOURCE_PRESET);
    s_granularReady  = false;
    s_granularLoaded = false;
}

// ==================== GRANULAR2 ====================

void audioLoadGranular2Source(const char* path, uint8_t sampleIdx) {
    if (!audioReady || !s_loadQueue || sampleIdx >= GRAN2_MAX_SAMPLES) return;
    s_gran2Loaded[sampleIdx] = false;
    LoadReq req;
    strncpy(req.path, path, sizeof(req.path) - 1);
    req.path[sizeof(req.path) - 1] = '\0';
    req.preset = (uint16_t)(GRAN2_SOURCE_BASE + sampleIdx);
    req.osc    = PCM_PREVIEW_OSC;
    req.vel    = 0.0f;
    xQueueSendToBack(s_loadQueue, &req, 0);  // queue without aborting other loads
}

bool audioIsGranular2Ready(uint8_t sampleIdx) {
    return (sampleIdx < GRAN2_MAX_SAMPLES) && s_gran2Loaded[sampleIdx];
}

uint8_t audioComputeGranular2Slices(uint8_t sampleIdx, uint8_t nSlices, uint8_t* waveform128) {
    if (sampleIdx >= GRAN2_MAX_SAMPLES || !s_gran2Loaded[sampleIdx]) return 0;
    if (nSlices != 4 && nSlices != 8) nSlices = 8;
    Serial.printf("[GR2 COMPUTE] s%d nSlices=%d sram_free=%lu psram_free=%lu\n",
                  sampleIdx, nSlices,
                  (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    uint32_t totalLen = 0;
    const int16_t* src = pcm_get_sample_ram_for_preset(GRAN2_SOURCE_BASE + sampleIdx, &totalLen);
    if (!src || totalLen < 16) return 0;

    // Waveform: full sample for display context.
    if (waveform128) {
        uint32_t blk = totalLen / 128; if (blk < 1) blk = 1;
        for (int i = 0; i < 128; i++) {
            uint32_t s0 = (uint32_t)i * blk, e0 = s0 + blk;
            if (e0 > totalLen) e0 = totalLen;
            int32_t peak = 0;
            for (uint32_t j = s0; j < e0; j++) { int32_t v = src[j]; if (v < 0) v = -v; if (v > peak) peak = v; }
            waveform128[i] = (uint8_t)((int32_t)peak * 255 / 32768);
        }
    }

    uint32_t wlen = totalLen;
    const int16_t* wsrc = src;
    const uint32_t rate = PCM_TARGET_RATE / 2;
    s_gran2NSlices[sampleIdx] = nSlices;

    // The reversed copy is built lazily on first actual reverse playback (see
    // ensureGranular2Reverse) rather than eagerly here — loading additional samples into
    // other slots would otherwise evict a reverse buffer the user hasn't even tried yet,
    // purely because of load order rather than actual use.
    int8_t* fullRev = s_gran2FullRevBuf[sampleIdx];
    if (fullRev && s_gran2FullRevLen[sampleIdx] != wlen) {
        // Source length changed since the reverse copy was built — stale, drop it; it will
        // be rebuilt lazily (at the new length) on the next reverse playback attempt.
        for (uint8_t i = 0; i < GRAN2_MAX_SLICES; i++)
            pcm_unload_preset(GRAN2_REV_BASE + sampleIdx * GRAN2_MAX_SLICES + i);
        pcm_unload_preset(GRAN2_TAIL_BASE + sampleIdx);
        free(fullRev);
        s_gran2FullRevBuf[sampleIdx] = nullptr; s_gran2FullRevLen[sampleIdx] = 0;
        fullRev = nullptr;
    }

    uint32_t sliceLen = wlen / nSlices;
    if (sliceLen < 2) sliceLen = 2;

    for (uint8_t i = 0; i < nSlices; i++) {
        uint32_t s0 = (uint32_t)i * sliceLen;
        uint32_t e0 = (i + 1 < nSlices) ? s0 + sliceLen : wlen;
        if (e0 > wlen) e0 = wlen;
        uint32_t len = e0 - s0; if (len < 2) len = 2;

        // FWD slice: [s0, e0) in source buffer
        uint16_t fwdPreset = (uint16_t)(GRAN2_FWD_BASE + sampleIdx * GRAN2_MAX_SLICES + i);
        pcm_register_extern16(fwdPreset, wsrc + s0, len, rate, 69, 0, len - 1);
        // Verify registration immediately so we know if the preset landed in the LL
        {
            uint32_t chkLen = 0;
            const int16_t* chkPtr = pcm_get_sample_ram_for_preset(fwdPreset, &chkLen);
            Serial.printf("[GR2 COMPUTE] s%d sl%d fwd p%u ptr=%p len=%lu -> chk ptr=%p len=%lu\n",
                          sampleIdx, i, fwdPreset, (void*)(wsrc + s0), (unsigned long)len,
                          (void*)chkPtr, (unsigned long)chkLen);
        }

        // REV slice: corresponding region in full reversed buffer. wsrc[s0..e0) → fullRev[wlen-e0..wlen-s0)
        if (fullRev) {
            uint32_t revStart = wlen - e0;
            uint16_t revPreset = (uint16_t)(GRAN2_REV_BASE + sampleIdx * GRAN2_MAX_SLICES + i);
            pcm_register_extern8(revPreset, fullRev + revStart, len, rate, 69, 0, len - 1);
            uint32_t chkLen = 0;
            const int16_t* chkPtr = pcm_get_sample_ram_for_preset(revPreset, &chkLen);
            Serial.printf("[GR2 COMPUTE] s%d sl%d rev p%u ptr=%p len=%lu -> chk ptr=%p len=%lu\n",
                          sampleIdx, i, revPreset, (void*)(fullRev + revStart), (unsigned long)len,
                          (void*)chkPtr, (unsigned long)chkLen);
        }

    }

    return nSlices;
}

// Lazily build sampleIdx's full reversed copy + REV slice/tail presets, if not already built.
// Deferred to first actual reverse playback attempt (rather than eagerly at compute time) so
// loading additional samples doesn't evict a reverse buffer the user hasn't even tried yet —
// PSRAM for the reversed copy is only spent on samples actually played in reverse.
static bool ensureGranular2Reverse(uint8_t sampleIdx) {
    if (sampleIdx >= GRAN2_MAX_SAMPLES || !s_gran2Loaded[sampleIdx]) return false;
    if (s_gran2FullRevBuf[sampleIdx]) return true;  // already built

    uint32_t wlen = 0;
    const int16_t* wsrc = pcm_get_sample_ram_for_preset(GRAN2_SOURCE_BASE + sampleIdx, &wlen);
    if (!wsrc || wlen < 16) return false;

    int8_t* fullRev = (int8_t*)ps_malloc(wlen * sizeof(int8_t));
    if (!fullRev) {
        // PSRAM exhausted: evict the most-recently-built other sample's revBuf first
        // (highest index), so that older samples (lower index) keep their reverse buffers.
        for (int v = (int)GRAN2_MAX_SAMPLES - 1; v >= 0 && !fullRev; v--) {
            if (v == (int)sampleIdx || !s_gran2FullRevBuf[v]) continue;
            Serial.printf("[GR2] evicting rev buf s%d (%lu B) for s%d\n",
                          v, (unsigned long)s_gran2FullRevLen[v], sampleIdx);
            for (uint8_t i = 0; i < GRAN2_MAX_SLICES; i++)
                pcm_unload_preset(GRAN2_REV_BASE + v * GRAN2_MAX_SLICES + i);
            pcm_unload_preset(GRAN2_TAIL_BASE + v);
            free(s_gran2FullRevBuf[v]);
            s_gran2FullRevBuf[v] = nullptr;
            s_gran2FullRevLen[v] = 0;
            fullRev = (int8_t*)ps_malloc(wlen * sizeof(int8_t));
        }
    }
    if (!fullRev) {
        Serial.printf("[GR2] PSRAM full: full rev buf s%d (%lu B needed, %lu B free)\n",
                      sampleIdx, (unsigned long)wlen,
                      (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        return false;
    }

    for (uint32_t j = 0; j < wlen; j++) fullRev[j] = (int8_t)(wsrc[wlen - 1 - j] >> 8);
    s_gran2FullRevBuf[sampleIdx] = fullRev;
    s_gran2FullRevLen[sampleIdx] = wlen;

    const uint32_t rate = PCM_TARGET_RATE / 2;
    pcm_register_extern8((uint16_t)(GRAN2_TAIL_BASE + sampleIdx), fullRev, wlen, rate, 69, 0, (int32_t)(wlen - 1));

    uint8_t nSlices = s_gran2NSlices[sampleIdx];
    if (nSlices != 4 && nSlices != 8) nSlices = 8;
    uint32_t sliceLen = wlen / nSlices; if (sliceLen < 2) sliceLen = 2;
    for (uint8_t i = 0; i < nSlices; i++) {
        uint32_t s0 = (uint32_t)i * sliceLen;
        uint32_t e0 = (i + 1 < nSlices) ? s0 + sliceLen : wlen;
        if (e0 > wlen) e0 = wlen;
        uint32_t len = e0 - s0; if (len < 2) len = 2;
        uint32_t revStart = wlen - e0;
        pcm_register_extern8((uint16_t)(GRAN2_REV_BASE + sampleIdx * GRAN2_MAX_SLICES + i),
                              fullRev + revStart, len, rate, 69, 0, len - 1);
    }
    Serial.printf("[GR2] built reverse buf s%d (%lu B)\n", sampleIdx, (unsigned long)wlen);
    return true;
}

void audioApplyGranular2Splits(uint8_t sampleIdx, float* splits, int N, bool lopMode) {
    if (!splits || N < 1 || sampleIdx >= GRAN2_MAX_SAMPLES || !s_gran2Loaded[sampleIdx]) return;
    uint32_t totalLen = 0;
    const int16_t* src = pcm_get_sample_ram_for_preset(GRAN2_SOURCE_BASE + sampleIdx, &totalLen);
    if (!src || totalLen < 16) return;

    const uint32_t wlen = totalLen;
    const int16_t* wsrc = src;
    const uint32_t rate = PCM_TARGET_RATE / 2;

    int8_t* fullRev = s_gran2FullRevBuf[sampleIdx];

    for (int i = 0; i < N; i++) {
        uint32_t s0 = (uint32_t)(constrain(splits[i],   0.0f, 1.0f) * (float)wlen);
        uint32_t e0 = (uint32_t)(constrain(splits[i+1], 0.0f, 1.0f) * (float)wlen);
        if (s0 >= wlen) s0 = wlen - 2;
        if (e0 < s0 + 2) e0 = s0 + 2;
        if (e0 > wlen)   e0 = wlen;
        uint32_t len = e0 - s0;

        // LOP only: extend sample_length past loopend so a loop active when splits shrink doesn't
        // see base_index >= sample_length and fire a spurious SYNTH_OFF.
        // NRM uses exact slice length — the one-shot ends naturally at loopend (= len-1).
        uint32_t fwdSampleLen = lopMode ? (wlen - s0) : len;
        pcm_register_extern16(GRAN2_FWD_BASE + sampleIdx * GRAN2_MAX_SLICES + i, wsrc + s0, fwdSampleLen, rate, 69, 0, (int32_t)(len - 1));

        // REV slice: tracks the same portion as fwd using the pre-allocated full reversed buffer
        if (fullRev && s_gran2FullRevLen[sampleIdx] == wlen) {
            uint32_t revStart = wlen - e0;
            uint32_t revSampleLen = lopMode ? (wlen - revStart) : len;
            pcm_register_extern8(GRAN2_REV_BASE + sampleIdx * GRAN2_MAX_SLICES + i, fullRev + revStart, revSampleLen, rate, 69, 0, (int32_t)(len - 1));
        }

    }
}

// Computes 128 waveform peak bins from the pristine STONE_SOURCE_PRESET buffer,
// for OLED display — mirrors audioComputeGranular2Slices()'s waveform pass above.
bool audioComputeStoneWaveform(uint8_t* waveform128) {
    if (!s_stoneLoaded || !waveform128) return false;
    uint32_t totalLen = 0;
    const int16_t* src = pcm_get_sample_ram_for_preset(STONE_SOURCE_PRESET, &totalLen);
    if (!src || totalLen < 16) return false;
    uint32_t blk = totalLen / 128; if (blk < 1) blk = 1;
    for (int i = 0; i < 128; i++) {
        uint32_t s0 = (uint32_t)i * blk, e0 = s0 + blk;
        if (e0 > totalLen) e0 = totalLen;
        int32_t peak = 0;
        for (uint32_t j = s0; j < e0; j++) { int32_t v = src[j]; if (v < 0) v = -v; if (v > peak) peak = v; }
        waveform128[i] = (uint8_t)((int32_t)peak * 255 / 32768);
    }
    return true;
}

// Re-windows STONE_PRESET (the one actually played, via fixed oscs STONE_OSC_BASE+i)
// to [startFrac,endFrac] of STONE_SOURCE_PRESET's pristine buffer — same in-place
// pcm_register_extern16 re-registration mechanism as audioApplyGranular2Splits()
// above, simplified for a single always-full-length window (no multi-slice, no
// reverse). Safe to call while notes are sounding (re-registering a preset's
// pointer/length live is the same pattern GRANULAR2's own pot-drag already relies on).
void audioStoneApplyWindow(float startFrac, float endFrac, bool loopMode) {
    if (!s_stoneLoaded) return;
    uint32_t totalLen = 0;
    const int16_t* src = pcm_get_sample_ram_for_preset(STONE_SOURCE_PRESET, &totalLen);
    if (!src || totalLen < 16) return;
    uint32_t s0 = (uint32_t)(constrain(startFrac, 0.0f, 1.0f) * (float)totalLen);
    uint32_t e0 = (uint32_t)(constrain(endFrac,   0.0f, 1.0f) * (float)totalLen);
    if (s0 >= totalLen) s0 = totalLen - 2;
    if (e0 < s0 + 2) e0 = s0 + 2;
    if (e0 > totalLen) e0 = totalLen;
    uint32_t len = e0 - s0;
    // In loop mode, register more data than the loop actually uses (loopend stays
    // exactly at the window's end, len-1) so a voice currently looping doesn't see
    // base_index >= sample_length and fire a spurious full stop while the window is
    // dragged live — same technique as GRANULAR2's own LOP mode, audioApplyGranular2Splits().
    uint32_t registeredLen = loopMode ? (totalLen - s0) : len;
    pcm_register_extern16(STONE_PRESET, src + s0, registeredLen, PCM_TARGET_RATE / 2, 69, 0, (int32_t)(len - 1));
}

// ==================== DJ (MODE_DJ) ====================
// Bounded-RAM chunked SD streaming — see config.h's DJ_* block for the sizing rationale
// and the state block above for the per-slot bookkeeping. This deliberately does NOT go
// through svcLoadMp3/svcLoadWav/bgServiceTask at all (confirmed via research this session
// that an ongoing streaming job can't live in that single-consumer queue without starving
// every other sample load for as long as DJ streams) — it has its own small decode helpers
// below, structurally mirroring svcLoadMp3/svcLoadWav's per-frame decode/resample math
// (proven correct there) but bounded to one chunk and starting from an arbitrary file
// position instead of "the whole file from byte 0".
//
// DJ_PLAYBACK_RATE_HZ (not PCM_TARGET_RATE/2!) is the rate to use for every "frame count
// -> real elapsed seconds" conversion in this section (chunk duration, position/elapsed
// time, prefetch margins). PCM_TARGET_RATE/2 is a DIFFERENT thing: the rate VALUE passed
// to pcm_register_extern16()/pcm_load() as the declared sample rate, deliberately halved
// to compensate for AMY's own "2x hardware bug" (see PCM_TARGET_RATE's own definition
// comment) — the ACTUAL/effective playback rate once that bug doubles it back is
// PCM_TARGET_RATE itself, matching how every chunk's audio was actually resampled during
// decode (see djDecodeMp3Chunk/djDecodeWavChunk's own `ratio = sampleRate/PCM_TARGET_RATE`
// resampling target). Conflating the two (both look like "10000ish" at a glance) produced
// a real, measured bug during development: chunk/position timing came out silently wrong
// by 2x versus real wall-clock/file duration.
#define DJ_PLAYBACK_RATE_HZ ((float)PCM_TARGET_RATE)
// How close to a chunk boundary (in seconds of playback) prefetch/backward-hop of the NEXT
// chunk starts — also doubles as the floor for the chunk-size shrinking retry in
// audioLoadDJTrack() (moved up here from next to audioDJAdvancePositionEstimate() so both
// can use it; a chunk can't usefully shrink below its own prefetch lookahead margin). Scaled
// down along with DJ_CHUNK_SECONDS (config.h) — same ~50-60% margin/chunk ratio as before.
#define DJ_PREFETCH_MARGIN_SEC 1.0f
// AMY's render_pcm() (lib/AMY Synthesizer/src/pcm.c) checks `base_index >= sample_length`
// (a hard, unconditional turn-OFF) BEFORE it checks `base_index >= loopend` (the wrap) —
// with loopend the usual `length-1`, that's only a ONE-sample gap between "wrap" and "turn
// off forever". At 1x speed the per-render-sample index step is comfortably under 1.0, so
// base_index can never skip over that single value — but at DJ's higher speeds (up to 4x,
// audioDJSetSpeed) the step can exceed 1 index/sample, and a fast-enough step can land
// EXACTLY past loopend and past length in the same tick, tripping the turn-off check
// instead of the wrap — silently killing DJ_OSC with no signal back to this code (measured:
// exactly the "au bout d'un moment le son devient silencieux" report, only reproducible
// after the ring had actually looped all the way around at least once, i.e. not on the
// first pass — and confirmed by checking synth[DJ_OSC]->status via AMY's own exposed
// `synth` array during testing). Fix: register loopend with a safety margin FAR bigger than
// any realistic per-sample step, so the wrap always fires with room to spare — sacrificing a
// handful of samples (<1ms, inaudible) at the very end of the ring on every lap.
#define DJ_WRAP_SAFETY_FRAMES 64
// Display-only window duration for the progress sweep/waveform (audioDJGetChunkProgressFrac/
// audioDJComputeWaveform, further down) — this design has no more "half" concept to derive
// one from, so it's a fixed constant purely for keeping the same "shows a couple seconds at a
// time" visual feel the UI had before, not a buffering unit (DJ_STREAM_GRAIN_SEC owns that).
// Capped at DJ_STREAM_LOW_WATERMARK_SEC (config.h), NOT independent of it: the waveform only
// ever draws the genuinely-confirmed-valid portion of the ring (audioDJComputeWaveform's own
// comment on why — drawing stale/uninitialized content read as "reverse changes the sample"),
// and steady-state playback only ever guarantees DJ_STREAM_LOW_WATERMARK_SEC of that ahead of
// the read cursor. A display window LARGER than the watermark would leave its own outer
// portion permanently blank even during perfectly healthy playback, not just right after a
// toggle — this cap is what keeps the picture fully populated in the common case.
#define DJ_DISPLAY_WINDOW_SEC fminf(2.0f, DJ_STREAM_LOW_WATERMARK_SEC)
// Hard cap on how many ring samples audioDJComputeWaveform() examines per output bin — see
// its own comment for why keeping this scan cheap matters beyond just redraw speed (it can
// preempt and delay the background grain-decode task, both running on core 0).
#define DJ_WAVEFORM_MAX_SAMPLES_PER_BIN 16

// ---- mp3: decode up to maxFrames (at PCM_TARGET_RATE/2) starting near startByte ----
// No seek API exists in the Helix decoder (confirmed this session) — a fresh
// MP3InitDecoder()+MP3FindSyncWord resync from an arbitrary byte offset is the only way
// to start mid-file, and is cheap (loses at most ~1 frame, ~26ms). *outNextByte is set to
// the file position where decode stopped (f.position()-bytesLeft — everything actually
// consumed, whether decoded or discarded during the initial sync-word search), for exact
// forward continuation next time. If startByte==0, also reports *outEstTotalFrames (a
// CBR-ratio estimate from the first frame's bitrate and file size — approximate for VBR
// files, same approximation this project's old one-shot loader already used).
// Smaller than svcLoadMp3's shared MP3_INBUF_SIZE (16KB) — deliberately so, ONLY here.
// On the desktop simulator's mp3 decoder shim (libmpg123-backed, sim_mp3dec.cpp), every
// MP3Decode() call feeds its ENTIRE input buffer into mpg123's own internal queue in one
// gulp (see the loop-termination comment on djDecodeMp3Chunk's main while loop) — with a
// 16KB refill, that queues many seconds' worth of compressed audio internally on every
// single iteration, so by the time this function's target frame count is reached and it
// tears down its (per-call, one-shot) decoder instance, a large amount of ALREADY-FED-BUT-
// NOT-YET-EXTRACTED audio is discarded with it, and outNextByte (which reflects bytes fed,
// not bytes actually turned into this chunk's audio) ends up far past where the NEXT
// chunk should actually resume — breaking continuous forward streaming. A much smaller
// feed size keeps mpg123's internal queue close to "just enough for what's about to be
// read", making outNextByte meaningfully accurate. Not an issue for svcLoadMp3's own
// whole-file, single-pass, never-resumed decodes — deliberately not touching that shared
// constant. The real ESP32 Helix decoder consumes exactly one frame's input at a time
// regardless of buffer size, so this doesn't matter there either way.
#define DJ_MP3_FEED_SIZE 4096

// Persistent mp3 decoder session, kept ALIVE ACROSS MULTIPLE djDecodeMp3Chunk() calls for
// sequential forward continuation, instead of tearing the decoder down after every bounded
// chunk. Necessary because of a real quirk in the desktop simulator's mp3 decoder shim
// (libmpg123-backed, sim_mp3dec.cpp): every MP3Decode() call feeds its ENTIRE input buffer
// into mpg123's own internal queue in one gulp rather than consuming exactly one frame's
// worth, so mpg123 typically has plenty of already-fed-but-not-yet-extracted audio sitting
// in ITS OWN internal buffer at the moment any given chunk's target frame count is reached
// — tearing the decoder down there (as an earlier version of this did) discards all of
// that, and the file-position bookkeeping ends up reflecting "how much was fed" rather
// than "how much became this chunk's audio", corrupting where the NEXT chunk should
// resume (measured: chunk N+1 could find itself already at EOF despite 100+ real seconds
// of untouched audio remaining). Keeping the SAME decoder/file/buffer state alive and just
// continuing to call MP3Decode() on it for the next chunk sidesteps the whole problem —
// nothing is ever discarded. Any seek/reverse-toggle/new-track-load closes this (see
// djMp3SessionClose() call sites) since those jump to an arbitrary new position anyway,
// which already requires a fresh decoder regardless of this quirk.
struct DjMp3Session {
    bool active = false;
    bool pend = false;       // a decoded frame in frameBuf still has un-emitted output (grain filled mid-frame)
    uint32_t pendFr = 0;
    File f;
    HMP3Decoder dec = nullptr;
    uint8_t* inBuf = nullptr;
    int16_t* frameBuf = nullptr;
    uint8_t* ptr = nullptr;
    int bytesLeft = 0;
    bool eof = false;
    uint32_t sampleRate = 44100;
    bool gotInfo = false;
    uint32_t inPos = 0;   // native-rate input frames consumed since session start (absolute)
    uint32_t outPos = 0;  // PCM_TARGET_RATE output frames produced since session start (absolute)
};
static DjMp3Session s_djMp3Session;

static void djMp3SessionClose() {
    if (s_djMp3Session.active) {
        if (s_djMp3Session.dec) MP3FreeDecoder(s_djMp3Session.dec);
        if (s_djMp3Session.inBuf) free(s_djMp3Session.inBuf);
        if (s_djMp3Session.frameBuf) free(s_djMp3Session.frameBuf);
        if (s_djMp3Session.f) s_djMp3Session.f.close();
    }
    s_djMp3Session = DjMp3Session{};
}

// ---- .pcm16 cache-backed streaming (see openValidPcmCache/readPcmCacheChunk above) ----
// When a valid cache exists for the current track, grains are read straight from it instead
// of through djMp3SessionClose/djDecodeMp3Chunk's Helix decode — no decode CPU per grain, and
// frame-exact seeking (no more djMp3EstimateByteForSec bitrate guessing). Set once at load
// time (audioLoadDJTrack) and closed alongside every djMp3SessionClose() call site.
static bool        s_djUsingPcmCache = false;
static File         s_djCacheFile;
static PcmCacheHdr s_djCacheHdr;
static PcmTee        s_djTee;
static void djPcmCacheClose() {
    if (s_djUsingPcmCache && s_djCacheFile) s_djCacheFile.close();
    s_djUsingPcmCache = false;
    teeDisarm(s_djTee);
}


// Decodes up to maxFrames (at PCM_TARGET_RATE) of mono PCM into outBuf, starting a FRESH
// decode at startByte (continueSession=false — used for the very first chunk, any seek,
// and reverse) or continuing the existing persistent session from wherever it left off
// (continueSession=true, startByte ignored — used for ordinary sequential forward
// playback). *outNextByte is set to the file position consumed so far (informational only
// when continuing a session — see djMp3Session's own comment for why it's not reliable as
// a byte-based resume point on this platform's shim). If startByte==0 on a fresh start,
// also reports *outEstTotalFrames-equivalent via the caller measuring this chunk's own
// byte/frame ratio (see djStreamDecodeTask).
static uint32_t djDecodeMp3Chunk(const char* path, uint32_t startByte, int16_t* outBuf,
                                  uint32_t maxFrames, uint32_t* outNextByte, bool continueSession) {
    bool freshStart = !(continueSession && s_djMp3Session.active);
    if (freshStart) {
        djMp3SessionClose();
        File f = SD.open(path, FILE_READ);
        if (!f) return 0;
        uint32_t fileSize = f.size();
        if (startByte >= fileSize) { f.close(); return 0; }
        f.seek(startByte);
        if (startByte == 0) mp3SkipId3v2(f);  // see mp3SkipId3v2's own comment
        HMP3Decoder dec = MP3InitDecoder();
        if (!dec) { f.close(); return 0; }
        uint8_t* inBuf = (uint8_t*)malloc(DJ_MP3_FEED_SIZE);
        int16_t* frameBuf = inBuf ? (int16_t*)malloc(MAX_NGRAN * MAX_NCHAN * MAX_NSAMP * sizeof(int16_t)) : nullptr;
        if (!inBuf || !frameBuf) { if(inBuf) free(inBuf); MP3FreeDecoder(dec); f.close(); return 0; }
        s_djMp3Session.active    = true;
        s_djMp3Session.f         = f;
        s_djMp3Session.dec       = dec;
        s_djMp3Session.inBuf     = inBuf;
        s_djMp3Session.frameBuf  = frameBuf;
        s_djMp3Session.ptr       = inBuf;
        s_djMp3Session.bytesLeft = 0;
        s_djMp3Session.eof       = false;
        s_djMp3Session.sampleRate = 44100;
        s_djMp3Session.gotInfo   = false;
        s_djMp3Session.inPos     = 0;
        s_djMp3Session.outPos    = 0;
        s_djMp3Session.pend      = false;
    }
    DjMp3Session& S = s_djMp3Session;
    auto refill = [&]() {
        if (S.ptr != S.inBuf && S.bytesLeft > 0) memmove(S.inBuf, S.ptr, S.bytesLeft);
        S.ptr = S.inBuf;
        if (!S.eof) {
            uint32_t space = (uint32_t)(DJ_MP3_FEED_SIZE - S.bytesLeft);
            uint32_t rd = S.f.read(S.inBuf + S.bytesLeft, space);
            S.bytesLeft += (int)rd;
            if (rd < space) S.eof = true;
        }
    };
    if (freshStart) {
        refill();
        int sync = MP3FindSyncWord(S.ptr, S.bytesLeft);
        if (sync < 0) { djMp3SessionClose(); if(outNextByte)*outNextByte=startByte; return 0; }
        S.ptr += sync; S.bytesLeft -= sync;
    }

    uint32_t chunkStartOutPos = S.outPos;
    int consErr = 0;
    // Loop bound is deliberately NOT "(!eof || bytesLeft > 0)" (i.e. "stop once the file's
    // bytes are exhausted) — see this function's own header comment on why (the shim can
    // still have real undrained audio buffered internally at that point). Keep calling
    // MP3Decode() (bytesLeft<=0 once eof makes the shim's own feed step a no-op, so this
    // just tries to extract more of what's already buffered inside it) until N CONSECUTIVE
    // calls make no progress at all — genuine exhaustion either way, reached in 1-2 tries
    // on the real ESP32 Helix decoder's plain per-frame contract.
    while (S.outPos - chunkStartOutPos < maxFrames) {
        if (!S.pend) {
            if (S.bytesLeft < MAINBUF_SIZE && !S.eof) refill();
            int ret = MP3Decode(S.dec, &S.ptr, &S.bytesLeft, S.frameBuf, 0);
            if (ret == ERR_MP3_INDATA_UNDERFLOW) {
                if (S.eof) { if (++consErr > 32) break; continue; }
                refill(); consErr = 0; continue;
            }
            if (ret == ERR_MP3_MAINDATA_UNDERFLOW) {
                if (S.eof) { if (++consErr > 32) break; }
                continue;
            }
            if (ret < 0) {
                if (++consErr > 64) break;
                int sk = MP3FindSyncWord(S.ptr + 1, S.bytesLeft - 1);
                if (sk < 0) break; S.ptr += sk + 1; S.bytesLeft -= sk + 1; continue;
            }
            consErr = 0;
            MP3FrameInfo info; MP3GetLastFrameInfo(S.dec, &info);
            if (info.nChans < 1 || info.nChans > 2 || info.outputSamps < 1
                || info.outputSamps > (int)(MAX_NGRAN * MAX_NCHAN * MAX_NSAMP)) continue;
            if (!S.gotInfo) { S.sampleRate = (uint32_t)info.samprate; S.gotInfo = true; }
            uint32_t frOut0 = (uint32_t)info.outputSamps / (uint32_t)info.nChans;
            if (frOut0 == 0) break;
            if (info.nChans > 1)
                for (uint32_t j = 0; j < frOut0; j++)
                    S.frameBuf[j] = (int16_t)(((int32_t)S.frameBuf[j*2] + S.frameBuf[j*2+1]) >> 1);
            S.pend = true; S.pendFr = frOut0;
        }
        // Resample the pending decoded frame from wherever the output cursor is. A grain that fills
        // up MID-frame leaves the frame pending (its remaining output is produced by the NEXT call,
        // straight from S.frameBuf, before any new frame is decoded). The old code advanced past the
        // whole frame at the clip point and discarded the tail — the next frame's outputs for that
        // span then had a negative source position, clamped to sample 0, i.e. a held value for up to
        // ~24ms at EVERY grain boundary: the audible "micro-coupures" every half second.
        const uint32_t frOut = S.pendFr;
        float ratio = (float)S.sampleRate / PCM_TARGET_RATE;
        uint32_t outEndAbs = (uint32_t)((S.inPos + frOut) / ratio);
        uint32_t room = maxFrames - (S.outPos - chunkStartOutPos);
        uint32_t want = outEndAbs > S.outPos ? outEndAbs - S.outPos : 0;
        uint32_t n = want < room ? want : room;
        uint32_t base = S.outPos - chunkStartOutPos;
        for (uint32_t oi = 0; oi < n; oi++) {
            float sp = (float)(S.outPos + oi) * ratio - (float)S.inPos;
            if (sp < 0.0f) sp = 0.0f;
            uint32_t si0 = (uint32_t)sp; float frac = sp - si0;
            if (si0 >= frOut) si0 = frOut - 1;
            uint32_t si1 = si0 + 1; if (si1 >= frOut) si1 = frOut - 1;
            outBuf[base + oi] = (int16_t)((int32_t)S.frameBuf[si0] + (int32_t)((S.frameBuf[si1] - S.frameBuf[si0]) * frac));
        }
        S.outPos += n;
        if (S.outPos >= outEndAbs) { S.inPos += frOut; S.pend = false; }
        if (freshStart && startByte == 0) s_djDecodedFrames = S.outPos - chunkStartOutPos;  // first-chunk progress readout
    }
    if (outNextByte) *outNextByte = (uint32_t)S.f.position() - (uint32_t)S.bytesLeft;
    // Deliberately NOT closing the session here — it stays open for the next
    // continueSession=true call. Callers that don't want that (any seek/reverse/reload)
    // call djMp3SessionClose() themselves.
    return S.outPos - chunkStartOutPos;
}

// ---- mp3: CBR-ratio estimate of the byte offset for a given track-time target ----
// Approximate for VBR files — same class of approximation svcLoadMp3 already used for its
// whole-file "estimatedTotal", just inverted (time→byte instead of byte→time).
static uint32_t djMp3EstimateByteForSec(const char* path, float targetSec) {
    if (s_djEstTotalSec <= 0.01f) return 0;
    File f = SD.open(path, FILE_READ);
    if (!f) return 0;
    uint32_t fileSize = f.size();
    f.close();
    float frac = constrain(targetSec / s_djEstTotalSec, 0.0f, 1.0f);
    if (frac <= 0.0f) return 0;  // start of file: the decoder skips the ID3 tag itself
    uint32_t tag = mp3Id3v2Size(path);
    if (tag >= fileSize) tag = 0;
    return tag + (uint32_t)(frac * (fileSize - tag));
}

// ---- wav: parse the RIFF header once per track load, cache the fields every chunk needs ----
static bool djParseWavHeader(const char* path) {
    File f = SD.open(path, FILE_READ);
    if (!f) return false;
    uint8_t riff[12];
    if (f.read(riff, 12) < 12 || memcmp(riff, "RIFF", 4) || memcmp(riff+8, "WAVE", 4)) { f.close(); return false; }
    uint16_t numCh = 1; uint32_t fileSR = AMY_SAMPLE_RATE; uint16_t bps = 16; uint16_t audioFmt = 1;
    uint32_t dataSize = 0; bool foundData = false; uint32_t dataStart = 0;
    int chunkIter = 0;
    while (f.available() > 8 && !foundData && ++chunkIter <= 32) {
        uint8_t ch[8]; if (f.read(ch, 8) < 8) break;
        uint32_t csz = (uint32_t)ch[4]|((uint32_t)ch[5]<<8)|((uint32_t)ch[6]<<16)|((uint32_t)ch[7]<<24);
        uint32_t cpos = (uint32_t)f.position();
        uint32_t fsize = (uint32_t)f.size();
        if (csz > fsize || cpos > fsize - csz) {
            if (!memcmp(ch, "data", 4)) { dataSize = fsize - cpos; dataStart = cpos; foundData = true; }
            else { f.close(); return false; }
            break;
        }
        if (!memcmp(ch, "fmt ", 4)) {
            uint8_t fmt[40] = {};
            uint32_t rd = csz < sizeof(fmt) ? csz : (uint32_t)sizeof(fmt);
            f.read(fmt, rd);
            f.seek(cpos + csz + (csz & 1));
            audioFmt = (uint16_t)(fmt[0]|(fmt[1]<<8));
            numCh    = (uint16_t)(fmt[2]|(fmt[3]<<8));
            fileSR   = (uint32_t)fmt[4]|(uint32_t)(fmt[5]<<8)|(uint32_t)(fmt[6]<<16)|(uint32_t)(fmt[7]<<24);
            bps      = (uint16_t)(fmt[14]|(fmt[15]<<8));
            if (audioFmt == 65534 && rd >= 26) audioFmt = (uint16_t)(fmt[24]|(fmt[25]<<8));
        } else if (!memcmp(ch, "data", 4)) {
            dataSize = (csz > 0) ? csz : (fsize - cpos);
            dataStart = cpos;
            foundData = true;
        } else {
            f.seek(cpos + csz + (csz & 1));
        }
    }
    f.close();
    if (!foundData || dataSize == 0 || fileSR == 0 || numCh == 0 || numCh > 8 || (audioFmt != 1 && audioFmt != 3))
        return false;
    s_djWavIsFloat     = (audioFmt == 3 && bps == 32);
    s_djWavBps         = bps;
    s_djWavFileSR      = fileSR;
    s_djWavNumCh       = numCh;
    s_djWavBpf         = numCh * (bps / 8);
    s_djWavDataStart   = dataStart;
    s_djWavTotalInFrames = s_djWavBpf ? dataSize / s_djWavBpf : 0;
    s_djEstTotalSec    = (float)s_djWavTotalInFrames / (float)fileSR;  // exact for wav
    return true;
}

static uint32_t djWavByteForSec(float targetSec) {
    if (s_djWavFileSR == 0) return s_djWavDataStart;
    uint32_t frame = (uint32_t)(constrain(targetSec, 0.0f, s_djEstTotalSec) * s_djWavFileSR);
    if (frame > s_djWavTotalInFrames) frame = s_djWavTotalInFrames;
    return s_djWavDataStart + frame * s_djWavBpf;
}

// ---- wav: decode up to maxFrames starting at the exact byte offset startByte ----
static uint32_t djDecodeWavChunk(const char* path, uint32_t startByte, int16_t* outBuf, uint32_t maxFrames, uint32_t* outNextByte) {
    // Callers that don't know the format (audioLoadDJTrack's initial-load and true-EOF-restart
    // call sites both pass a literal byte 0 as their "start of file" target, which is correct
    // for mp3 — near the start of the bitstream — but never valid for wav, where byte 0 is
    // always inside the RIFF/fmt header, before the actual PCM data). Clamp here rather than
    // at every call site: startByte < s_djWavDataStart would otherwise underflow the unsigned
    // "startByte - s_djWavDataStart" below, wrapping to a huge startInFrame and silently
    // decoding 0 frames every time (confirmed: this made every wav track's very first chunk,
    // and every true-EOF loop-to-start restart, decode nothing at all).
    if (startByte < s_djWavDataStart) startByte = s_djWavDataStart;
    File f = SD.open(path, FILE_READ);
    if (!f || s_djWavFileSR == 0) { if(f) f.close(); return 0; }
    f.seek(startByte);
    uint32_t startInFrame = s_djWavBpf ? (startByte - s_djWavDataStart) / s_djWavBpf : 0;
    uint32_t totalIn = s_djWavTotalInFrames > startInFrame ? s_djWavTotalInFrames - startInFrame : 0;
    float ratio = (float)s_djWavFileSR / (float)PCM_TARGET_RATE;
    uint32_t maxIn = (uint32_t)(maxFrames * ratio) + 2;
    if (totalIn > maxIn) totalIn = maxIn;

    int16_t* raw = (int16_t*)ps_malloc(STREAM_BATCH_FRAMES * s_djWavNumCh * sizeof(int16_t));
    if (!raw) { f.close(); return 0; }

    uint32_t inPos = 0, outPos = 0;
    while (inPos < totalIn && outPos < maxFrames) {
        uint32_t batchIn = totalIn - inPos;
        if (batchIn > STREAM_BATCH_FRAMES) batchIn = STREAM_BATCH_FRAMES;
        if (!wavReadRaw(f, (uint16_t)s_djWavBps, batchIn, (uint16_t)s_djWavNumCh, raw, s_djWavIsFloat)) break;
        uint32_t outEnd = (uint32_t)((float)(inPos + batchIn) / ratio);
        if (outEnd > maxFrames) outEnd = maxFrames;
        uint32_t outCnt = outEnd > outPos ? outEnd - outPos : 0;
        if (outCnt > 0) {
            if (s_djWavFileSR == (uint32_t)PCM_TARGET_RATE && s_djWavNumCh == 1) {
                memcpy(outBuf + outPos, raw, outCnt * sizeof(int16_t));
            } else if (s_djWavFileSR == (uint32_t)PCM_TARGET_RATE && s_djWavNumCh == 2) {
                for (uint32_t i = 0; i < outCnt; i++) outBuf[outPos + i] = (int16_t)(((int32_t)raw[i*2] + raw[i*2+1]) >> 1);
            } else {
                for (uint32_t oi = 0; oi < outCnt; oi++) {
                    float sp = (outPos + oi) * ratio - inPos;
                    if (sp < 0.0f) sp = 0.0f;
                    uint32_t si0 = (uint32_t)sp; float frac = sp - si0;
                    uint32_t si1 = si0 + 1; if (si1 >= batchIn) si1 = batchIn - 1;
                    int32_t s0 = 0, s1 = 0;
                    for (uint16_t c = 0; c < s_djWavNumCh; c++) { s0 += raw[si0*s_djWavNumCh+c]; s1 += raw[si1*s_djWavNumCh+c]; }
                    if (s_djWavNumCh > 1) { s0 /= (int32_t)s_djWavNumCh; s1 /= (int32_t)s_djWavNumCh; }
                    outBuf[outPos + oi] = (int16_t)(s0 + (int32_t)((s1 - s0) * frac));
                }
            }
        }
        inPos += batchIn;
        outPos = outEnd;
        if (startByte == s_djWavDataStart) s_djDecodedFrames = outPos;  // first-chunk progress readout
    }
    free(raw);
    if (outNextByte) *outNextByte = s_djWavDataStart + (startInFrame + inPos) * s_djWavBpf;
    f.close();
    return outPos;
}

// ---- background grain-decode task — one-shot, spawned per grain (not a persistent worker;
// matches this codebase's existing djBuildReverseTask/bgServiceTask idiom of short-lived
// tasks rather than inventing a new long-lived-task pattern) ----

struct DjStreamJob {
    uint32_t genAtSpawn;
    float    readFromSec;    // ascending decode-start position (the file has no "read backward" primitive — see djDecodeWavChunk/djDecodeMp3Chunk's own header comments; a reverse-direction grain always decodes ascending, then gets byte-reversed afterward)
    uint32_t maxFrames;      // usually s_djGrainCapFrames; capped smaller for a reverse grain that would otherwise overshoot past true start-of-file
    bool     reverse;        // true: byte-reverse the decoded grain afterward
    bool     continueSession;// mp3 only: continue the persistent decode session (djMp3Session) instead of reseeking — see djDecodeMp3Chunk's header comment; only ever valid for a FORWARD grain immediately following another forward grain
    bool     activateNow;    // true: write at physical ring index 0 and retrigger (a real reposition: load/seek/resume/true-EOF-wrap/scratch-release/stutter-release); false: silently append at the current write cursor (an ordinary top-up — never retriggers)
    bool     isFirstLoad;
};
static DjStreamJob s_djJob;  // single job slot — only one background decode is ever in flight (gated by s_djNextBuilding)

static uint32_t s_djGrainCapFrames = 0;  // DJ_STREAM_GRAIN_SEC * rate, sized in audioLoadDJTrack()
static int16_t* s_djGrainBuf       = nullptr;  // one reusable scratch decode buffer — safe as a single static since only one grain job is ever in flight
static bool     s_djMp3ContinuePossible = false;  // true once the persistent mp3 session is known to be positioned for a forward continuation

// A reposition (seek/resume/scratch-end/stutter-end/load) that lost the job-slot race against
// an already-in-flight grain — unlike an ordinary top-up (which just retries with a fresh
// margin check next tick, costing nothing), a dropped reposition has NO other path back to
// AMY ever being retriggered at the right place, so it must be queued and re-fired the moment
// the in-flight job releases the slot (djStreamDecodeTask, at every exit point).
//
// s_djPendingGenAtQueue records s_djDecodeGen AT THE MOMENT this was queued — every OTHER
// action that should supersede a stale queued request (reverse toggle, a fresh seek, a new
// scratch/stutter gesture, a new load) already bumps s_djDecodeGen itself, so a mismatch here
// means something newer has happened since this was queued. Without this check, a reposition
// that lost its race (e.g. a stutter-end) could sit queued for an arbitrary stretch and then
// fire LATE — jumping playback to that stale target regardless of whatever the user has done
// in the meantime (reported as the displayed position suddenly jumping, e.g. "0:22 -> 0:13",
// on a later, seemingly unrelated reverse toggle that just happened to be what freed the slot).
static volatile bool s_djPendingReposition   = false;
static float         s_djPendingDesiredSec   = 0.0f;
static bool          s_djPendingDir          = false;
static bool          s_djPendingIsFirstLoad  = false;
static uint32_t      s_djPendingGenAtQueue   = 0;

// Set whenever a reposition or reverse-toggle needs the very NEXT grain that actually claims
// the job slot to be small (DJ_STREAM_QUICK_GRAIN_SEC), even if that turns out to be a
// djStreamTopUp() call rather than the request that set this flag — covers the case where the
// original quick-grain kickoff itself loses the job-slot race (see djStreamTopUp()'s own
// comment): without this, that race would silently fall back to a full-size DJ_STREAM_GRAIN_SEC
// grain, reproducing the exact "stale content still audible" lag this mechanism exists to fix.
static volatile bool s_djForceQuickNext = false;

// Computes the ascending [readFromSec, readFromSec+~capSec) span a REVERSE grain should
// decode so that, after reversal, it ends exactly at `endSec` — clamping at true
// start-of-file (reading further back than position 0 is meaningless) instead of overshooting
// past `endSec` itself when there isn't a full grain's worth of track left before it.
// capFrames lets a caller request a smaller-than-usual span (see DJ_STREAM_QUICK_GRAIN_SEC's
// own comment for why the very first grain after a reposition needs one).
static void djReverseGrainSpan(float endSec, uint32_t capFrames, float& outReadFromSec, uint32_t& outMaxFrames) {
    float capSec = (float)capFrames / DJ_PLAYBACK_RATE_HZ;
    float readFrom = endSec - capSec;
    if (readFrom < 0.0f) {
        outReadFromSec = 0.0f;
        outMaxFrames   = (uint32_t)(endSec * DJ_PLAYBACK_RATE_HZ);
    } else {
        outReadFromSec = readFrom;
        outMaxFrames   = capFrames;
    }
}

static bool djStreamKickOff(float readFromSec, uint32_t maxFrames, bool reverse, bool continueSession,
                             bool activateNow, bool isFirstLoad = false);  // forward decl — returns false if the job slot was already busy (claimed nothing)
static void djReposition(float desiredSec, bool dir, bool isFirstLoad = false);  // forward decl — djStreamDecodeTask below re-fires a wrap-around reposition via this
static void djServicePendingReposition();  // forward decl — checked at every djStreamDecodeTask exit point
static uint32_t djPhysicalReadIndexNow();  // forward decl — defined after the read-side reanchor functions further down, used by djStreamTopUp below

static void djStreamDecodeJob() {
    DjStreamJob job = s_djJob;
    uint32_t jobT0 = millis();
    uint32_t frames = 0, nextByte = 0, startByte = 0;
    if (s_djUsingPcmCache) {
        // Frame-exact — no byte-rate estimate needed, and no decode CPU at all.
        uint32_t startFrame = (uint32_t)(job.readFromSec * DJ_PLAYBACK_RATE_HZ);
        frames = readPcmCacheChunk(s_djCacheFile, s_djCacheHdr, startFrame, s_djGrainBuf, job.maxFrames);
    } else {
        startByte = job.continueSession ? 0
                  : (s_djIsMp3 ? djMp3EstimateByteForSec(s_djPath, job.readFromSec)
                               : djWavByteForSec(job.readFromSec));
        nextByte = startByte;
        if (s_djIsMp3) frames = djDecodeMp3Chunk(s_djPath, startByte, s_djGrainBuf, job.maxFrames, &nextByte, job.continueSession);
        else           frames = djDecodeWavChunk(s_djPath, startByte, s_djGrainBuf, job.maxFrames, &nextByte);
    }

    {   // Stutter diagnostics: a grain that takes a large fraction of its own audio duration to
        // produce leaves no margin — logged (rate-limited) so hardware stutter can be pinned on
        // decode/SD speed vs. something else.
        static uint32_t lastSlowLog = 0;
        uint32_t took = millis() - jobT0;
        float audioMs = frames * 1000.0f / DJ_PLAYBACK_RATE_HZ;
        if (frames > 0 && took > audioMs * 0.5f && millis() - lastSlowLog > 2000) {
            lastSlowLog = millis();
            Serial.printf("[DJ] SLOW grain: %u ms to make %.0f ms of audio (%s%s, ahead=%.2fs)\n", (unsigned)took, audioMs,
                          s_djUsingPcmCache ? "cache" : "mp3", job.reverse ? " reverse" : "", s_djValidAheadSec);
        }
    }
    if (job.genAtSpawn != s_djDecodeGen) {
        // Superseded by a newer load/seek/reverse-toggle/resume while this was decoding —
        // discard. Same atomic check-and-clear this design has always needed (see
        // djStreamKickOff's own comment for the cross-core race this guards against).
        taskENTER_CRITICAL(&s_djJobMux);
        s_djNextBuilding = false;
        taskEXIT_CRITICAL(&s_djJobMux);
        djServicePendingReposition();
        return;
    }

    if (job.reverse && frames > 1)
        for (uint32_t i = 0; i < frames / 2; i++) { int16_t t = s_djGrainBuf[i]; s_djGrainBuf[i] = s_djGrainBuf[frames-1-i]; s_djGrainBuf[frames-1-i] = t; }

    if (!s_djUsingPcmCache && !s_djEstFromHeader && job.isFirstLoad && s_djIsMp3 && frames > 0 && nextByte > startByte) {
        // mp3 whole-track duration estimate — see the old design's identical comment (kept
        // verbatim in spirit): measured bytes-consumed-per-second for THIS grain,
        // extrapolated across the whole file. Approximate for VBR/the simulator's decoder
        // shim, same as before — doesn't affect streaming correctness, only the displayed
        // total time and the true-EOF/SOF wrap target.
        File f = SD.open(s_djPath, FILE_READ);
        if (f) {
            uint32_t fileSize = f.size();
            f.close();
            uint32_t tag = (startByte == 0) ? mp3Id3v2Size(s_djPath) : 0;  // decode began AFTER the tag
            if (tag >= fileSize) tag = 0;
            float secondsThisGrain = (float)frames / DJ_PLAYBACK_RATE_HZ;
            uint32_t consumed = (nextByte > startByte + tag) ? nextByte - startByte - tag : 0;
            float bytesPerSecond = (float)consumed / secondsThisGrain;
            if (bytesPerSecond > 0.01f) s_djEstTotalSec = (float)(fileSize - tag) / bytesPerSecond;
        }
    }

    if (frames == 0) {
        if (!job.reverse && !s_djUsingPcmCache) { teeEnd(s_djTee, job.readFromSec); teeFlush(s_djTee); }
        // True EOF/SOF: the persistent mp3 session is exhausted — the wrapped read MUST start a
        // fresh decode. Leaving continuation enabled made every later top-up "continue" the dead
        // session, get 0 frames, wrap again, forever: the ring stopped being refilled and AMY looped
        // the last few seconds endlessly ("relit en boucle la même proportion").
        s_djMp3ContinuePossible = false;
        taskENTER_CRITICAL(&s_djJobMux);
        s_djNextBuilding = false;
        taskEXIT_CRITICAL(&s_djJobMux);
        if (job.isFirstLoad) {
            // Nothing decodable at all from byte 0 of a brand-new load (empty/corrupt/
            // unsupported file, or an mp3 with no audio frames at all — e.g. ID3-only) — a
            // real load failure, not a genuine end-of-track wrap. Wrapping back to the same
            // starting position (as the normal-playback branch below does) would just re-fail
            // the exact same way every time, spawning a fresh djStreamDecodeTask with no yield
            // in between — confirmed on real hardware as a task-watchdog panic (IDLE0 starved
            // on core 0) when loading a track that fails to decode from the start. Fail the
            // load cleanly instead of ever wrapping on isFirstLoad.
            Serial.printf("[DJ] load FAILED: no decodable audio at start of %s\n", s_djPath);
            s_djLoaded = false;
            s_djAwaitingActivate = false;
            djServicePendingReposition();
                return;
        }
        // True EOF/SOF hit with nothing left to give from here — wrap the write cursor's
        // logical position to the other end of the track and let the NEXT top-up tick try
        // again from there. No retrigger needed even for an ordinary top-up grain: AMY
        // doesn't know or care that the logical track position just jumped, it only ever
        // sees physically-contiguous ring memory (see this file's DJ header comment) — this
        // makes even a whole-track wrap gapless. A reposition (activateNow) landing exactly
        // on true EOF/SOF instead falls back to the OTHER end via the pending re-fire below.
        if (job.activateNow) {
            float wrapSec = job.reverse ? s_djEstTotalSec : 0.0f;
            djReposition(wrapSec, job.reverse, job.isFirstLoad);
        } else {
            s_djWriteLogicalSec = job.reverse ? s_djEstTotalSec : 0.0f;
            djServicePendingReposition();
        }
        return;
    }

    float grainDurSec = (float)frames / DJ_PLAYBACK_RATE_HZ;

    if (job.activateNow) {
        // Register ONCE — length/loopend cover the WHOLE ring and never change again after
        // this (harmless/idempotent to re-assert on a later reposition) — then retrigger.
        // A fresh note-on always resets AMY's phase to absolute ring index 0, so every
        // activateNow job writes there by construction.
        memcpy(s_djRing, s_djGrainBuf, (size_t)frames * sizeof(int16_t));
        pcm_register_extern16(DJ_PRESET, s_djRing, s_djRingFrames, PCM_TARGET_RATE / 2, 69,
                               0, s_djRingFrames - 1 - DJ_WRAP_SAFETY_FRAMES);
        s_djReadAnchorFrame = 0;
        s_djElapsedSec = job.reverse ? (job.readFromSec + grainDurSec) : job.readFromSec;
        s_djElapsedSecAtPlayStart = s_djElapsedSec;
        s_djPlayStartMs = millis();
        s_djAwaitingActivate = false;  // position estimate now correctly reanchored — safe to resume advancing
        s_djChunkGen++;
        if (job.isFirstLoad) { s_djLoaded = true; s_djLoadGen++; }
        if (s_djPlaying && s_djPrefillNext) {
            s_djPrefillNext = false;
            s_djPrefill = true;   // audio starts from the top-up commit once enough is buffered
        } else if (s_djPlaying) {
            // feedback=1.0f enables AMY's native per-preset looping — the whole mechanism
            // this ring design relies on for gapless playback.
            amyPlayPcm(DJ_OSC, DJ_PRESET, 1.0f, 1.0f);
            amy_event e = amy_default_event();
            e.osc = DJ_OSC; e.midi_note = 69.0f + 12.0f * log2f(s_djSpeed);
            amy_add_event(&e);
        }
        s_djWriteFrame      = frames % s_djLoopFrames;
        s_djWriteLogicalSec = job.reverse ? job.readFromSec : (job.readFromSec + grainDurSec);
        // Exactly what we just wrote is the whole valid-ahead buffer right now — nothing
        // older survives a reposition's retrigger.
        s_djValidAheadSec    = grainDurSec;
        s_djValidAheadLastMs = millis();
    } else {
        // Ordinary top-up: append at the write cursor, wrapping as needed. Wraps at
        // s_djLoopFrames (AMY's own native loop period), NOT the full physical buffer size —
        // the last DJ_WRAP_SAFETY_FRAMES frames of the buffer are dead space AMY's own loop
        // never visits (see s_djLoopFrames's own comment), so writing there would just be
        // silently discarded content, never actually played. No register, no retrigger — this
        // is the whole point: an ordinary chunk boundary AND a reverse toggle both funnel
        // through here, and neither ever touches AMY at all.
        uint32_t wf = s_djWriteFrame;
        uint32_t firstPart = min(frames, s_djLoopFrames - wf);
        memcpy(s_djRing + wf, s_djGrainBuf, (size_t)firstPart * sizeof(int16_t));
        if (firstPart < frames) memcpy(s_djRing, s_djGrainBuf + firstPart, (size_t)(frames - firstPart) * sizeof(int16_t));
        s_djWriteFrame      = (wf + frames) % s_djLoopFrames;
        s_djWriteLogicalSec = job.reverse ? job.readFromSec : (job.readFromSec + grainDurSec);
        // s_djChunkGen is normally bumped from audioDJAdvancePositionEstimate() instead of
        // here (see its own comment for why an ordinary top-up grain landing must NOT
        // routinely be what triggers a waveform/cursor redraw) — EXCEPT right after a
        // reposition/toggle forced an early redraw while s_djValidAheadSec was still tiny
        // (the safety margin only): that redraw captured a mostly-blank picture, and since
        // the NEXT scheduled redraw could be up to DJ_DISPLAY_WINDOW_SEC away, the incomplete
        // picture would sit on screen for up to a full second — read as "l'affichage de la
        // shape a du mal à suivre". Bump once, right when the buffer actually finishes
        // catching back up to a full picture's worth, so the redraw follows promptly instead
        // of waiting out the normal cadence.
        bool wasStarved = s_djValidAheadSec < DJ_DISPLAY_WINDOW_SEC;
        s_djValidAheadSec += grainDurSec;
        if (wasStarved && s_djValidAheadSec >= DJ_DISPLAY_WINDOW_SEC) s_djChunkGen++;
        if (s_djPrefill && s_djValidAheadSec >= DJ_PREFILL_SEC) {
            s_djPrefill = false;
            if (s_djPlaying) {
                amyPlayPcm(DJ_OSC, DJ_PRESET, 1.0f, 1.0f);
                amy_event e = amy_default_event();
                e.osc = DJ_OSC; e.midi_note = 69.0f + 12.0f * log2f(s_djSpeed);
                amy_add_event(&e);
                s_djPlayStartMs = millis();
                s_djElapsedSecAtPlayStart = s_djElapsedSec;
                s_djValidAheadLastMs = millis();
            }
        }
    }
    s_djMp3ContinuePossible = !job.reverse;
    if (!s_djUsingPcmCache) { teeAppend(s_djTee, job.readFromSec, job.reverse, s_djGrainBuf, frames); teeFlush(s_djTee); }  // after the ring commit, so it never delays the audio

    taskENTER_CRITICAL(&s_djJobMux);
    s_djNextBuilding = false;
    taskEXIT_CRITICAL(&s_djJobMux);
    djServicePendingReposition();
}

// One PERSISTENT worker per stream, created once at boot (audioInit) and woken by a semaphore —
// creating/destroying an 8KB-stack task for every grain fragmented internal RAM until task
// creation itself started failing on hardware ("grain task creation FAILED (low internal RAM)").
static SemaphoreHandle_t s_djWorkSem = nullptr;
static void djWorkerTask(void*) { for (;;) { xSemaphoreTake(s_djWorkSem, portMAX_DELAY); djStreamDecodeJob(); } }

static bool djStreamKickOff(float readFromSec, uint32_t maxFrames, bool reverse, bool continueSession,
                             bool activateNow, bool isFirstLoad) {
    // Defensive — audioLoadDJTrack() already refuses to complete a load without these, so
    // this should never actually trigger, but a decode task writing into a null buffer is a
    // real crash (StoreProhibited) rather than a graceful no-op, so it's worth the cheap check.
    if (!s_djGrainBuf || !s_djWindowPad) return false;
    // s_djNextBuilding's check-and-claim must be atomic across cores — see this design's own
    // long-standing comment (unchanged reasoning from the half-based version): without a
    // lock, a background task's own discard-and-refire and a fresh call from the main thread
    // could both observe "not building" at the same instant and both spawn a task, handing a
    // torn/inconsistent job to one of them. Reproduced on real hardware as a task-watchdog
    // crash under rapid reverse-toggling before this lock existed.
    bool won = false;
    taskENTER_CRITICAL(&s_djJobMux);
    if (!s_djNextBuilding) { s_djNextBuilding = true; won = true; }
    taskEXIT_CRITICAL(&s_djJobMux);
    if (!won) return false;  // one job at a time — an ordinary top-up that loses the race just
                              // retries on the next tick (or, if s_djForceQuickNext is set,
                              // retries QUICK — see djStreamTopUp()'s own comment); a
                              // reposition instead queues itself as s_djPendingReposition (see
                              // djReposition()'s own comment) since it has no other way back.

    if (s_djForceQuickNext) {
        // A quick grain was owed and THIS is the one that actually claimed the slot — clamp
        // it down regardless of who asked for it or what they originally requested. Only
        // safe to shrink a forward span (start position stays valid, just ends earlier); a
        // reverse span was already sized correctly by whichever caller consulted this same
        // flag before computing readFromSec (djStreamTopUp()), so this is a no-op for that
        // case, not a double-shrink.
        uint32_t quickCap = (uint32_t)(DJ_STREAM_QUICK_GRAIN_SEC * DJ_PLAYBACK_RATE_HZ);
        if (!reverse && maxFrames > quickCap) maxFrames = quickCap;
        s_djForceQuickNext = false;
    }
    if (activateNow) s_djAwaitingActivate = true;  // cleared once the task actually retriggers (or, on true EOF/SOF, by the reposition it re-fires)
    s_djJob = { s_djDecodeGen, readFromSec, maxFrames, reverse, continueSession, activateNow, isFirstLoad };
    if (!s_djWorkSem) {
        Serial.println("[DJ] grain worker missing — job slot released");
        s_djAwaitingActivate = false;
        taskENTER_CRITICAL(&s_djJobMux); s_djNextBuilding = false; taskEXIT_CRITICAL(&s_djJobMux);
        return false;
    }
    xSemaphoreGive(s_djWorkSem);
    return true;
}

// The ONE retriggering primitive — every operation that discontinuously repositions playback
// (initial load, seek, resume-from-pause, true EOF/start-of-file wrap, scratch-release,
// stutter-release) funnels through this. A reverse-direction TOGGLE deliberately does NOT use
// this — see audioDJSetReverse()'s own comment for why that specific case needs no retrigger.
static void djReposition(float desiredSec, bool dir, bool isFirstLoad) {
    s_djDecodeGen++;  // invalidate any in-flight grain — it targets the wrong place/direction now
    // Quick, not full-size (DJ_STREAM_QUICK_GRAIN_SEC's own comment) — this is the FIRST
    // grain after a discontinuous jump, so it should decode (and therefore land, and
    // therefore retrigger) as fast as possible; the ordinary top-up loop takes over with
    // full-size grains right after.
    uint32_t quickCap = (uint32_t)(DJ_STREAM_QUICK_GRAIN_SEC * DJ_PLAYBACK_RATE_HZ);
    float readFromSec; uint32_t maxFrames;
    if (dir) djReverseGrainSpan(desiredSec, quickCap, readFromSec, maxFrames);
    else     { readFromSec = desiredSec; maxFrames = quickCap; }
    bool ok = djStreamKickOff(readFromSec, maxFrames, dir, /*continueSession=*/false, /*activateNow=*/true, isFirstLoad);
    if (!ok) {
        // Lost the job-slot race against an already-in-flight grain — unlike an ordinary
        // top-up, nothing else will ever retrigger AMY at this new position on its own, so
        // this MUST be remembered and retried, not silently dropped (a real gap the streaming
        // redesign introduced versus the old half-based design's own pending-request queue).
        // Re-fired the instant the in-flight job releases the slot — see
        // djStreamDecodeTask's own exit points.
        taskENTER_CRITICAL(&s_djJobMux);
        s_djPendingReposition  = true;
        s_djPendingDesiredSec  = desiredSec;
        s_djPendingDir         = dir;
        s_djPendingIsFirstLoad = isFirstLoad;
        s_djPendingGenAtQueue  = s_djDecodeGen;  // this call's own bump above — see the field's own comment
        taskEXIT_CRITICAL(&s_djJobMux);
    }
}

// Checked at every djStreamDecodeTask exit point right after the job slot is released —
// re-fires a reposition that lost the job-slot race earlier (see djReposition()'s own
// comment), UNLESS something newer has superseded it in the meantime (s_djDecodeGen no longer
// matches what it was when queued — see s_djPendingGenAtQueue's own comment). Must be called
// with the slot already released (not holding s_djJobMux), since djReposition()/
// djStreamKickOff() claim it fresh themselves.
static void djServicePendingReposition() {
    bool refire = false;
    float desiredSec = 0.0f; bool dir = false; bool isFirstLoad = false;
    taskENTER_CRITICAL(&s_djJobMux);
    if (s_djPendingReposition) {
        if (s_djPendingGenAtQueue == s_djDecodeGen) {
            refire = true;
            desiredSec  = s_djPendingDesiredSec;
            dir         = s_djPendingDir;
            isFirstLoad = s_djPendingIsFirstLoad;
        }
        // Cleared either way — a gen mismatch means this request is stale and must be
        // dropped, not left queued to possibly fire even later.
        s_djPendingReposition = false;
    }
    taskEXIT_CRITICAL(&s_djJobMux);
    if (refire) djReposition(desiredSec, dir, isFirstLoad);
}

// Keeps the ring topped up: whenever s_djValidAheadSec (an explicit fill-level counter, see
// its own comment for why it must NOT be re-derived from comparing ring positions) drops below
// the low watermark, kicks off one more grain in whichever direction is CURRENTLY selected.
// This one loop is what makes an ordinary chunk boundary AND a reverse-direction toggle both
// inaudible by construction — see this file's DJ header comment.
static void djStreamTopUp() {
    if (!s_djLoaded || !s_djPlaying) return;
    if (s_djAwaitingActivate) return;
    uint32_t nowMs = millis();
    if (s_djScratchGestureActive || s_djStuttering) {
        // AMY is playing a small side-channel loop registered separately during a gesture
        // (see those functions' own comments) — the ring can still be safely topped up in the
        // background (harmless, even useful for a clean resume), but nothing is actually being
        // CONSUMED from it right now, so don't decay the fill level for this stretch — just
        // keep the decay anchor fresh so the elapsed gesture time itself is never misread as
        // playback once it ends.
        s_djValidAheadLastMs = nowMs;
    } else {
        float dSec = (nowMs - s_djValidAheadLastMs) / 1000.0f * s_djSpeed;
        s_djValidAheadLastMs = nowMs;
        float before = s_djValidAheadSec;
        s_djValidAheadSec = fmaxf(0.0f, s_djValidAheadSec - dSec);
        if (before > 0.0f && s_djValidAheadSec <= 0.0f) {   // the reader has caught the writer: audible dropout
            static uint32_t lastUr = 0;
            if (millis() - lastUr > 1000) { lastUr = millis(); Serial.printf("[DJ] UNDERRUN (speed %.2f%s%s) — decode/SD not keeping up\n", s_djSpeed, s_djReverse ? ", reverse" : "", s_djUsingPcmCache ? ", cache" : ", mp3"); }
        }
        if (s_djValidAheadSec >= DJ_STREAM_LOW_WATERMARK_SEC) return;
    }
    if (s_djNextBuilding) return;  // a job is already in flight — this tick's top-up check will retry next tick

    // A quick grain may be owed (a reverse-toggle's own direct kickoff attempt lost the
    // job-slot race — see s_djForceQuickNext's own comment) — consult it BEFORE computing the
    // span so a reverse request is sized correctly from the start (shrinking it after the
    // fact, once readFromSec is already computed, would silently break the "must end exactly
    // at the current position" invariant that keeps a reverse toggle's position from jumping).
    uint32_t cap = s_djForceQuickNext ? (uint32_t)(DJ_STREAM_QUICK_GRAIN_SEC * DJ_PLAYBACK_RATE_HZ) : s_djGrainCapFrames;
    bool wasForcedQuick = s_djForceQuickNext;
    bool ok;
    if (s_djReverse) {
        float readFromSec; uint32_t maxFrames;
        djReverseGrainSpan(s_djWriteLogicalSec, cap, readFromSec, maxFrames);
        ok = djStreamKickOff(readFromSec, maxFrames, true, /*continueSession=*/false, /*activateNow=*/false);
    } else {
        ok = djStreamKickOff(s_djWriteLogicalSec, cap, false,
                              /*continueSession=*/s_djIsMp3 && s_djMp3ContinuePossible, /*activateNow=*/false);
    }
    if (ok && wasForcedQuick) s_djForceQuickNext = false;
}

void audioLoadDJTrack(const char* path) {
    if (!audioReady) return;
    audioDJPlayPause(false);
    // Invalidate whatever the background grain-decode task is currently working on, then
    // wait (bounded) for it to actually release the job slot before touching s_djPath/
    // s_djRing/etc. below. Without this, a reload racing an in-flight grain is a genuine
    // cross-core data race: the memset() a few lines down and that task's own commit
    // (memcpy'ing its just-decoded grain into the SAME s_djRing) can interleave — at best a
    // brief audible glitch (part of the "petit saccadement" reported), at worst a real crash
    // if a track load ever needs to free/reallocate a buffer that background task still
    // expects to be valid (confirmed as a real GDMA/use-after-free-shaped panic on real
    // hardware — EXCVADDR 0xa5a5a5a5 is ESP-IDF's own freed-memory poison pattern).
    s_djDecodeGen++;
    s_djTrackGen++;  // a NEW track — any cache build still running for the PREVIOUS one must abort (see s_djTrackGen's own comment)
    uint32_t waitStart = millis();
    // A single SD read can legitimately stall for seconds on a marginal card/SPI link (the SD
    // driver waits up to 500ms per block token and retries 3x), so 600ms was too tight — and
    // proceeding anyway is a real cross-core race on s_djRing. Wait longer, and if the job
    // STILL hasn't released the slot, refuse the load rather than corrupt shared buffers.
    while (s_djNextBuilding && millis() - waitStart < 4000) delay(1);
    if (s_djNextBuilding) {
        Serial.println("[DJ] load ABORTED: previous grain task still stuck on SD after 4s (card/SPI problem?)");
        return;
    }
    strncpy(s_djPath, path, sizeof(s_djPath) - 1);
    s_djPath[sizeof(s_djPath) - 1] = '\0';
    const char* ext = strrchr(path, '.');
    s_djIsMp3 = ext && strcasecmp(ext, ".mp3") == 0;
    if (!s_djRing) {
        // Same shrink-with-floor retry as before, halving down from the DJ_RING_SECONDS
        // default rather than failing outright the first time free PSRAM doesn't cover the
        // full ask. Floored at DJ_PREFETCH_MARGIN_SEC's worth so the top-up loop still has
        // meaningful room to stay ahead of the read cursor.
        uint32_t tryFrames = DJ_RING_FRAMES;
        for (;;) {
            if (s_djRing) { free(s_djRing); s_djRing = nullptr; }
            s_djRing = (int16_t*)ps_malloc((size_t)tryFrames * sizeof(int16_t));
            if (s_djRing) {
                s_djRingFrames = tryFrames;
                s_djLoopFrames = tryFrames - DJ_WRAP_SAFETY_FRAMES;  // see s_djLoopFrames's own comment
                // ps_malloc does NOT zero memory — AMY registers/loops over the WHOLE ring
                // immediately on the very first retrigger, while only a tiny "quick" grain
                // (DJ_STREAM_QUICK_GRAIN_SEC) has actually been written into it yet. Without
                // this, the read cursor reaches raw, uninitialized PSRAM within a fraction of
                // a second (well before the ongoing top-up loop has caught up filling the
                // rest) and AMY plays it as 16-bit PCM — a loud, harsh noise, not silence.
                // Zeroing once here means any read into not-yet-topped-up territory is
                // silence instead, the same safe fallback the old half-based design's own
                // "short half → memset the tail" already relied on.
                memset(s_djRing, 0, (size_t)tryFrames * sizeof(int16_t));
                break;
            }
            float trySec = tryFrames / DJ_PLAYBACK_RATE_HZ;
            uint32_t floorFrames = (uint32_t)(DJ_PREFETCH_MARGIN_SEC * DJ_PLAYBACK_RATE_HZ);
            if (tryFrames <= floorFrames) {
                Serial.printf("[DJ] ring buffer alloc FAILED even at floor %.0fs (need %lu B, PSRAM free=%lu B largest_block=%lu B)\n",
                    trySec, (unsigned long)((size_t)tryFrames * sizeof(int16_t)),
                    (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                    (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
                return;
            }
            uint32_t nextFrames = tryFrames / 2;
            if (nextFrames < floorFrames) nextFrames = floorFrames;
            Serial.printf("[DJ] ring buffer alloc failed at %.0fs (PSRAM free=%lu B largest_block=%lu B), retrying at %.0fs\n",
                trySec, (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
                nextFrames / DJ_PLAYBACK_RATE_HZ);
            tryFrames = nextFrames;
        }
    } else {
        // Re-loading a different track into the SAME already-allocated ring: nothing garbage
        // here (it's real, valid PCM from whatever played before), but it's the WRONG track —
        // zero it too so an underrun during this load's own ramp-up can only ever expose
        // silence, never a stale snippet of the previous file.
        memset(s_djRing, 0, (size_t)s_djRingFrames * sizeof(int16_t));
    }
    {
        // One reusable grain-decode scratch buffer (small, ~DJ_STREAM_GRAIN_SEC worth) and
        // one small window pad (for a scratch/stutter window that would wrap past the ring's
        // physical end) — not part of the retry loop above, both tiny compared to the ring,
        // but UNLIKE the old design's equivalent buffers (s_djBehind/s_djScratchSrc, which
        // were optional optimizations with their own null-checks at every call site), every
        // single grain decode — ordinary playback included, not just scratch/stutter — now
        // goes through s_djGrainBuf, so a failed alloc here can't be treated as "just disable
        // an optimization" anymore. Fail the whole load cleanly instead of leaving s_djLoaded
        // true with a null buffer that the next decode would unconditionally write into
        // (confirmed as a real StoreProhibited crash on real hardware — PSRAM fragmented by
        // other subsystems by the time DJ mode loads is a real scenario the simulator's
        // effectively-unlimited heap never reproduces).
        s_djGrainCapFrames = (uint32_t)(DJ_STREAM_GRAIN_SEC * DJ_PLAYBACK_RATE_HZ);
        if (!s_djGrainBuf) s_djGrainBuf = (int16_t*)ps_malloc((size_t)s_djGrainCapFrames * sizeof(int16_t));
        s_djWindowPadFrames = (uint32_t)(fmaxf(DJ_SCRATCH_WINDOW_SEC, DJ_STUTTER_MAX_SEC) * DJ_PLAYBACK_RATE_HZ) + 4;
        if (!s_djWindowPad) s_djWindowPad = (int16_t*)ps_malloc((size_t)s_djWindowPadFrames * sizeof(int16_t));
        if (!s_djGrainBuf || !s_djWindowPad) {
            // Deliberately NOT freeing s_djRing here (even though today this branch can only
            // be reached on the very first-ever load, before anything has registered a PCM
            // preset from it, so it would currently be safe) — freeing a buffer this file
            // registers directly with AMY's render engine (pcm_register_extern16 stores the
            // raw pointer) without also confirming nothing could still reference it is exactly
            // the class of bug that produced a real GDMA/use-after-free-shaped crash on real
            // hardware elsewhere in this design. Leaving it allocated (unused, in this rare
            // failure case) is a far safer tradeoff than a freed pointer some future change
            // might reintroduce a path to dereference.
            Serial.println("[DJ] grain/window buffer alloc FAILED — aborting load");
            free(s_djGrainBuf); s_djGrainBuf = nullptr;
            free(s_djWindowPad); s_djWindowPad = nullptr;
            return;
        }
    }
    s_djLoaded = false;
    s_djDecodedFrames = 0;
    s_djReverse = false;
    s_djEstTotalSec = 0.0f;
    s_djEstFromHeader = false;
    if (s_djIsMp3) {
        float hd = mp3HeaderDuration(s_djPath);
        if (hd > 1.0f) { s_djEstTotalSec = hd; s_djEstFromHeader = true; }
    }
    s_djScratchGestureActive = false;
    s_djGrainAmount = 0.0f;
    if (s_djStuttering) {
        // Defensive: a stutter hold spans an arbitrary wall-clock duration and holds the job
        // slot claimed for that whole time — if a new track load lands mid-hold (the user
        // never released the joystick click), force-release it here rather than leaving a
        // load queued behind a stutter tied to the track that's about to be replaced anyway.
        s_djStuttering = false;
        taskENTER_CRITICAL(&s_djJobMux);
        s_djNextBuilding = false;
        taskEXIT_CRITICAL(&s_djJobMux);
    }
    s_djWriteFrame = 0;
    s_djWriteLogicalSec = 0.0f;
    s_djMp3ContinuePossible = false;
    s_djSpeed = 1.0f;
    djPcmCacheClose();       // any cache from the PREVIOUS track is unconditionally stale
    if (s_djIsMp3) {
        uint32_t srcSize = 0;
        { File tmp = SD.open(s_djPath, FILE_READ); if (tmp) { srcSize = tmp.size(); tmp.close(); } }
        if (srcSize > 0 && openValidPcmCache(s_djPath, srcSize, s_djCacheFile, s_djCacheHdr)) {
            s_djUsingPcmCache = true;
            s_djEstTotalSec = (float)s_djCacheHdr.frameCount / DJ_PLAYBACK_RATE_HZ;
            Serial.printf("[DJ] using pcm16 cache: %s (%u frames, %.1fs)\n",
                          s_djPath, s_djCacheHdr.frameCount, s_djEstTotalSec);
        } else if (srcSize > 0) {
            teeArm(s_djTee, s_djPath, srcSize);
        }
    }
    if (!s_djIsMp3 && !djParseWavHeader(s_djPath)) { Serial.println("[DJ] wav header parse failed"); return; }
    djReposition(0.0f, false, /*isFirstLoad=*/true);
}

bool audioDJIsLoaded() { return s_djLoaded; }
bool audioDJCanPlayYet() { return s_djLoaded; }
uint32_t audioDJGetDecodedFrames() { return s_djDecodedFrames; }
uint32_t audioDJGetLoadGen() { return s_djLoadGen; }

// Physical ring index AMY is (estimated to be) reading right now — monotonic forward through
// the ring regardless of logical direction (see this file's DJ header comment). Only
// meaningful outside a scratch/stutter gesture, where AMY is instead playing a small
// side-channel loop registered separately (see those functions' own comments) — during a
// gesture this just returns the frozen anchor, matching the position-estimate freeze.
static uint32_t djPhysicalReadIndexNow() {
    if (!s_djPlaying || s_djAwaitingActivate || s_djPrefill || s_djStuttering || s_djScratchGestureActive)
        return s_djReadAnchorFrame;
    float dSec = (millis() - s_djPlayStartMs) / 1000.0f * s_djSpeed;
    uint32_t framesElapsed = (uint32_t)(dSec * DJ_PLAYBACK_RATE_HZ);
    return (s_djReadAnchorFrame + framesElapsed) % s_djLoopFrames;
}

// Advances s_djElapsedSec by however much time has elapsed since the last anchor at the
// current speed/direction — AMY has no live phase-readback API, so this is an ESTIMATE, not a
// measurement — then tops up the ring. Called whenever the position is queried
// (audioDJGetPosFrac) or from audioDJTick() every loop() iteration.
static void audioDJAdvancePositionEstimate() {
    if (!s_djPlaying) return;
    // While an activateNow job is in flight (resume-after-pause, seek, true EOF/start-of-file
    // wrap), nothing is audibly playing yet and s_djPlayStartMs/s_djElapsedSecAtPlayStart are
    // stale until that job completes and reanchors them — advancing from them here would
    // visibly move the position ahead of where the audio will actually resume.
    if (s_djAwaitingActivate) return;
    if (s_djPrefill) {  // armed but not yet audible: playhead frozen, keep topping up the ring
        s_djPlayStartMs = millis(); s_djValidAheadLastMs = millis();
        djStreamTopUp();
        return;
    }
    // Stuttering/scratching deliberately freeze the playhead — reanchored to the SAME frozen
    // position on release, so simply not advancing here for the duration is enough.
    if (s_djStuttering) return;
    if (s_djScratchGestureActive) return;
    float dSec = (millis() - s_djPlayStartMs) / 1000.0f * s_djSpeed;
    s_djElapsedSec = s_djElapsedSecAtPlayStart + (s_djReverse ? -dSec : dSec);
    // The ring wraps physically at true EOF/SOF (see djStreamDecodeTask's frames==0 branch) —
    // the DISPLAYED estimate wraps the same way, rather than clamping, so it keeps matching
    // what's actually audible.
    if (s_djEstTotalSec > 0.01f) {
        s_djElapsedSec = fmodf(s_djElapsedSec, s_djEstTotalSec);
        if (s_djElapsedSec < 0.0f) s_djElapsedSec += s_djEstTotalSec;
    } else {
        s_djElapsedSec = fmaxf(s_djElapsedSec, 0.0f);
    }
    // Redraw the waveform/cursor exactly when the sweep (audioDJGetChunkProgressFrac's own
    // sawtooth) completes a cycle and resets — NOT on a wall-clock timer or every background
    // grain write. Those both decouple the picture's own refresh from the cursor's sweep
    // phase, so the picture visibly shifts mid-sweep instead of only swapping right when the
    // cursor wraps — read as "le curseur évolue en même temps que le sample affiché évolue
    // lui aussi", i.e. two independent motions instead of one coherent "sweep then swap"
    // rhythm. Tying both to the SAME window-index crossing restores that single rhythm.
    int32_t windowIdx = (int32_t)floorf(s_djElapsedSec / DJ_DISPLAY_WINDOW_SEC);
    if (windowIdx != s_djDisplayWindowIdx) { s_djChunkGen++; s_djDisplayWindowIdx = windowIdx; }
    djStreamTopUp();
}

static void audioDJReanchor() {
    // Must read the OLD anchor before s_djPlayStartMs is overwritten below — this is what
    // keeps the physical read index continuous across a speed change (no retrigger, no jump).
    s_djReadAnchorFrame = djPhysicalReadIndexNow();
    s_djPlayStartMs = millis();
    s_djElapsedSecAtPlayStart = s_djElapsedSec;
}

// Seeks to an absolute track-time target (posFrac * s_djEstTotalSec) and, if playing,
// retriggers from there once decoded. A retrigger click is expected here — this is a real
// seek/cue jump, not a smooth scratch (AMY has no API to move a SOUNDING voice's phase) —
// unlike ordinary playback and reverse-toggle, neither of which ever retriggers.
void audioDJSeek(float posFrac) {
    if (!s_djLoaded || s_djEstTotalSec <= 0.01f) return;
    posFrac = constrain(posFrac, 0.0f, 1.0f);
    djReposition(posFrac * s_djEstTotalSec, s_djReverse);
}

float audioDJGetPosFrac() {
    audioDJAdvancePositionEstimate();
    return (s_djEstTotalSec > 0.01f) ? constrain(s_djElapsedSec / s_djEstTotalSec, 0.0f, 1.0f) : 0.0f;
}

void audioDJPlayPause(bool playing) {
    if (!audioDJCanPlayYet()) { s_djPlaying = false; return; }
    if (s_djAwaitingActivate) {
        // A previous reposition (this function, a seek, or true EOF/SOF wrap) is still
        // mid-decode — its target position is s_djElapsedSec, which stays frozen the whole
        // time (see audioDJAdvancePositionEstimate's own guard), so it's still aiming at the
        // right place no matter how long it takes. Simplest and correct is to just update
        // which outcome the in-flight job should produce once it completes (it already reads
        // live s_djPlaying at that point) and return, matching the reported "il a du mal à
        // recommencer" fix (pressing play/pause again while an earlier press is still being
        // resolved — plausible on real hardware where the SD decode this waits on can take a
        // human-noticeable moment).
        s_djPlaying = playing;
        return;
    }
    if (playing == s_djPlaying) return;
    if (playing) {
        // Resume = reposition to wherever we currently are, in whichever direction was
        // already set — the same primitive every other reposition uses. A retrigger click is
        // expected here (same as an explicit seek always has) — grains are small enough that
        // the decode behind it is fast.
        s_djPlaying = true;
        s_djPrefillNext = true;
        djReposition(s_djElapsedSec, s_djReverse);
    } else {
        audioDJAdvancePositionEstimate();  // bake in elapsed time BEFORE stopping
        // NOT amyStopOsc(DJ_OSC): that sends a plain velocity=0 note-off, but AMY's PCM
        // note-off (pcm_note_off() in pcm.c) treats a looping voice (feedback>0, which
        // DJ_OSC always is) as "disable future looping, let the CURRENT pass ring out to
        // the end of the registered length" — for this ring buffer that end is up to a
        // full ring-length away (several seconds), so pause wouldn't reliably/immediately
        // silence playback (reported as "ne stop pas toujours le play du sample"). A
        // reset_osc event fully reintializes the oscillator (status->SYNTH_OFF) instead —
        // applied through the same delta-queue as every other event (thread-safe, unlike
        // poking synth[DJ_OSC]->status directly from off the render thread) — instant,
        // guaranteed silence. Resume always fully re-triggers via amyPlayPcm() afterwards,
        // so nothing this clears is ever depended on across a pause.
        amy_event e = amy_default_event();
        e.reset_osc = DJ_OSC;
        amy_add_event(&e);
        s_djPlaying = false;
        s_djPrefill = false; s_djPrefillNext = false;
    }
}

bool audioDJIsPlaying() { return s_djPlaying; }

// Drives the streaming top-up even when nothing is currently querying position
// (audioDJGetPosFrac()/audioDJGetChunkProgressFrac() both already call
// audioDJAdvancePositionEstimate() too, so this is somewhat redundant while the OLED is
// actively showing MODE_DJ's transport view — but audio streaming continuing correctly
// should NOT depend on the UI happening to be on-screen/refreshing at any given moment;
// call this unconditionally from loop() instead, same as every other sequencer-style
// clock in this codebase (DRUM2/DR2/etc. all tick regardless of what's being drawn).
//
// Granular spray: short one-shot snippets read directly from the live ring (never triggers
// SD I/O), layered on top of the continuously-playing DJ_OSC deck signal via a small
// round-robin oscillator pool (DJ_GRAIN_OSC_BASE, DJ_GRAIN_COUNT voices — capped at 2 by the
// only 2 free AMY oscillators below the 250 max_oscs ceiling, see config.h). Fully decoupled
// from the reposition/mutex machinery: a pure read of stable, already-resident memory, no
// s_djNextBuilding claim needed. Picked from anywhere in the ring rather than anchored to the
// exact read position — the whole ring holds audio in correct playback order everywhere (see
// audioDJSetReverse()'s own comment), so any window sounds like "recently relevant" texture,
// matching the old design's own similarly loose active-half-or-look-behind choice. Never
// spans the ring's wrap point — pcm_register_extern16 needs one contiguous pointer, so the
// offset is simply kept within [0, s_djRingFrames-grainFrames).
static void djSpawnGrainIfDue() {
    if (s_djGrainAmount <= 0.01f || !s_djPlaying || !s_djRing) return;
    uint32_t now = millis();
    if ((int32_t)(now - s_djGrainNextDueMs) < 0) return;
    float gapMs = DJ_GRAIN_GAP_MS_MAX + (DJ_GRAIN_GAP_MS_MIN - DJ_GRAIN_GAP_MS_MAX) * s_djGrainAmount;
    s_djGrainNextDueMs = now + (uint32_t)gapMs;

    uint32_t grainFrames = (uint32_t)(DJ_PLAYBACK_RATE_HZ *
        (DJ_GRAIN_LEN_MS_MIN + random(DJ_GRAIN_LEN_MS_MAX - DJ_GRAIN_LEN_MS_MIN)) / 1000.0f);
    if (grainFrames >= s_djRingFrames) return;
    grainFrames = constrain(grainFrames, 16u, s_djRingFrames - 1);
    uint32_t maxOffset = s_djRingFrames - grainFrames;
    uint32_t offset = maxOffset > 0 ? (uint32_t)random(maxOffset) : 0;

    uint8_t  rr = s_djGrainRR;
    s_djGrainRR = (s_djGrainRR + 1) % DJ_GRAIN_COUNT;
    uint16_t preset = DJ_GRAIN_PRESET_BASE + rr;
    uint8_t  osc    = DJ_GRAIN_OSC_BASE + rr;

    pcm_register_extern16(preset, s_djRing + offset, grainFrames, PCM_TARGET_RATE / 2, 69, 0, grainFrames - 1);
    amyPlayPcm(osc, preset, 0.7f);  // feedback=0 default -> one-shot, no looping
    float pitchScatter = (random(2001) / 1000.0f - 1.0f) * DJ_GRAIN_PITCH_SCATTER;  // -scatter..+scatter
    amy_event e = amy_default_event();
    e.osc = osc; e.midi_note = 69.0f + 12.0f * log2f(s_djSpeed) + pitchScatter;
    amy_add_event(&e);
}

void audioDJSetGrainAmount(float amount01) { s_djGrainAmount = constrain(amount01, 0.0f, 1.0f); }
float audioDJGetGrainAmount() { return s_djGrainAmount; }

void audioDJTick() { audioDJAdvancePositionEstimate(); djSpawnGrainIfDue(); }

void audioDJSetSpeed(float speed) {
    speed = constrain(speed, 0.25f, 4.0f);
    if (s_djPlaying) audioDJAdvancePositionEstimate();  // bake in time elapsed at the OLD speed first
    s_djSpeed = speed;
    if (s_djPlaying) {
        audioDJReanchor();
        amy_event e = amy_default_event();
        e.osc = DJ_OSC; e.midi_note = 69.0f + 12.0f * log2f(s_djSpeed);
        amy_add_event(&e);
    }
}

float audioDJGetSpeed() { return s_djSpeed; }

// Toggles playback direction, continuing from the CURRENT playback position. Unlike every
// other reposition in this file, this needs NO retrigger at all: it's just "the writer stops
// adding forward content and starts adding reverse content from here" — AMY keeps reading the
// exact same registered ring, at the exact same phase, forever. This is what eliminates the
// whole class of bugs this feature hit earlier (retrigger races, adjacency preconditions
// between a look-behind buffer and an active half, a position estimate drifting outside an
// assumed-valid window and corrupting a buffer-size calculation) — there's no window to build,
// no precondition to satisfy, nothing to race against.
void audioDJSetReverse(bool reverse) {
    if (!s_djLoaded || reverse == s_djReverse) return;
    audioDJAdvancePositionEstimate();  // freshen s_djElapsedSec in the OLD direction first
    taskENTER_CRITICAL(&s_djJobMux);
    s_djReverse = reverse;
    s_djDecodeGen++;  // invalidate any in-flight top-up grain — it's decoding the OLD direction
    // Reanchor the wall-clock position tracking to RIGHT NOW, in the NEW direction — without
    // this, audioDJAdvancePositionEstimate()'s formula (elapsedSecAtPlayStart +/- dSec, dSec
    // measured from the anchor set at the LAST reposition) keeps using that stale anchor with
    // the SIGN now flipped: the very next position query jumps by roughly 2×dSec (dSec being
    // however long playback continued since that last reanchor) — a real, always-present bug,
    // not a race — reported as the displayed position suddenly jumping on every toggle (e.g.
    // "0:22 -> 0:13"), growing the longer a track had been playing before the toggle.
    audioDJReanchor();
    // Leave a small untouched safety margin right ahead of the (now current) read cursor —
    // audio there may be microseconds from being read by the render callback, so only content
    // BEYOND the margin is safe to overwrite with the new direction's content. This is the one
    // honest, minimal, unavoidable latency floor for the toggle: a hardware-timing safety
    // requirement, not a design compromise — nothing else about the toggle takes any real
    // time at all.
    uint32_t marginFrames = (uint32_t)(DJ_STREAM_SAFETY_MARGIN_SEC * DJ_PLAYBACK_RATE_HZ);
    s_djWriteFrame = (s_djReadAnchorFrame + marginFrames) % s_djLoopFrames;
    s_djWriteLogicalSec = s_djElapsedSec;  // continue from the current logical position — no jump
    // Only the small untouched safety margin is still genuinely valid — everything that was
    // buffered further ahead belongs to the OLD direction and no longer describes what's
    // coming up next.
    s_djValidAheadSec    = DJ_STREAM_SAFETY_MARGIN_SEC;
    s_djValidAheadLastMs = millis();
    taskEXIT_CRITICAL(&s_djJobMux);
    // Force an immediate waveform/cursor refresh next tick — a direction change is a
    // discrete, rare event the UI should always redraw for right away, not wait for the
    // sweep's own window to happen to cross next (see audioDJAdvancePositionEstimate's own
    // comment on why that crossing is normally what drives the redraw).
    s_djDisplayWindowIdx = INT32_MIN;
    // Kick off the FIRST post-toggle grain right now, small (DJ_STREAM_QUICK_GRAIN_SEC's own
    // comment) — waiting for the next djStreamTopUp() tick to notice the collapsed margin
    // would still work, but it would request a FULL-size DJ_STREAM_GRAIN_SEC grain, whose
    // decode latency is exactly what left old, wrong-direction content sitting audible past
    // the safety margin (the bug this whole quick-grain mechanism exists to fix). Requesting
    // it here, synchronously, means there's no extra tick's delay before the fast decode
    // even starts.
    uint32_t quickCap = (uint32_t)(DJ_STREAM_QUICK_GRAIN_SEC * DJ_PLAYBACK_RATE_HZ);
    float readFromSec; uint32_t maxFrames;
    if (reverse) djReverseGrainSpan(s_djElapsedSec, quickCap, readFromSec, maxFrames);
    else         { readFromSec = s_djElapsedSec; maxFrames = quickCap; }
    // Set BEFORE attempting the kickoff, cleared only if it actually lands — if this attempt
    // loses the job-slot race (a grain from just before the toggle still in flight), the flag
    // stays set so djStreamTopUp()'s own next successful kickoff honors the debt instead of
    // silently reverting to a full-size grain (see s_djForceQuickNext's own comment).
    s_djForceQuickNext = true;
    if (djStreamKickOff(readFromSec, maxFrames, reverse, /*continueSession=*/false, /*activateNow=*/false))
        s_djForceQuickNext = false;
    // No register, no retrigger — nothing about how AMY is being read ever changes.
}

bool audioDJGetReverse() { return s_djReverse; }
// True whenever nothing is mid-reposition (seek/resume/true-EOF-SOF-wrap) — gives the UI's
// existing "REV(...)" pending indicator (see main.cpp's MODE_DJ transport status line) a real
// signal for the brief window between requesting one and its retrigger actually landing. A
// reverse-direction TOGGLE never sets this at all (it never retriggers), so it's always
// immediately "ready" from this indicator's point of view.
bool audioDJIsReverseReady() { return !s_djAwaitingActivate; }

// Copies a `winFrames`-long window starting at ring-physical index `physStart` into
// s_djWindowPad, handling wraparound (pcm_register_extern16 needs one contiguous pointer). A
// COPY, not a live pointer into the ring — this is what lets the top-up writer keep safely
// extending the ring in the background for however long the registered window keeps looping
// (matters most for stutter's arbitrary hold duration; a live pointer into a moving target
// would glitch far more than the small, accepted cosmetic risk grain-spray already takes for
// a brief one-shot read).
static void djCopyRingWindow(uint32_t physStart, uint32_t winFrames) {
    // Wraps at s_djLoopFrames (AMY's own native loop period), not the physical buffer size —
    // continuing past it into the dead-zone tail (see s_djLoopFrames's own comment) would read
    // meaningless bytes instead of wrapping back to where AMY's phase actually continues.
    if (physStart + winFrames <= s_djLoopFrames) {
        memcpy(s_djWindowPad, s_djRing + physStart, (size_t)winFrames * sizeof(int16_t));
    } else {
        uint32_t firstPart = s_djLoopFrames - physStart;
        memcpy(s_djWindowPad, s_djRing + physStart, (size_t)firstPart * sizeof(int16_t));
        memcpy(s_djWindowPad + firstPart, s_djRing, (size_t)(winFrames - firstPart) * sizeof(int16_t));
    }
}

// ---- Scratch (joystick X) ----
// Deliberately retriggers on every nudge (~100Hz, main.cpp's 10ms joystick poll) — a glitch
// effect, not a seamless control, same as stutter below (see this file's header comment on
// why ordinary playback and reverse-toggle never retrigger but these two do, on purpose).
// Reads its working window straight out of the live ring instead of a dedicated snapshot
// buffer: the ring already holds audio in correct playback order everywhere (see
// audioDJSetReverse()'s own comment), so any window of it, read forward, sounds correct — no
// adjacency precondition, no un-reversal math, no "full chunk on both sides" check needed.
//
// Physical-offset-based, not absolute-logical-time-based: converting an arbitrary logical
// target into a physical ring index needs ONE fixed anchor (frozen at gesture start), because
// ring-physical-distance and file-time-distance are only in a fixed 1:1 relationship relative
// to that anchor — not globally, if a direction-change seam happens to sit inside the window.
static bool djScratchBegin() {
    s_djPrefill = false;
    if (!s_djRing || !s_djWindowPad) return false;
    s_djScratchAnchorPhys       = djPhysicalReadIndexNow();
    s_djScratchAnchorLogicalSec = s_djElapsedSec;
    s_djScratchDir              = s_djReverse;  // never flips mid-gesture — see below
    s_djScratchGestureActive    = true;
    return true;
}

// Retriggers a short window at an offset from the frozen gesture anchor. Direction is
// deliberately NEVER flipped mid-gesture (always s_djScratchDir, frozen at gesture start) —
// only the TARGET position moves (which can still move opposite to that direction's own sign,
// via a negative delta). It's the repeated jump in start point between retriggers — not the
// buffer's own storage direction — that produces the scratch texture.
static void djScratchStepTo(float targetSec) {
    bool claimed = false;
    taskENTER_CRITICAL(&s_djJobMux);
    if (!s_djNextBuilding) { s_djNextBuilding = true; claimed = true; }
    taskEXIT_CRITICAL(&s_djJobMux);
    if (!claimed) return;  // background job in flight — skip this nudge, the next tick retries

    const float rate = DJ_PLAYBACK_RATE_HZ;
    const uint32_t winFrames = min((uint32_t)(DJ_SCRATCH_WINDOW_SEC * rate), s_djWindowPadFrames);
    // Moving forward in logical time moves forward in ring-physical order when the frozen
    // direction is forward, but BACKWARD in ring-physical order when it's reverse (a
    // reverse-stored grain's index 0 is its LATEST time — see this file's header comment).
    float deltaSec = targetSec - s_djScratchAnchorLogicalSec;
    int32_t physOffset = (int32_t)roundf((s_djScratchDir ? -deltaSec : deltaSec) * rate);
    // Clamp to a conservative safe span — content behind the read cursor persists until the
    // writer physically comes back around to overwrite it (a whole ring-length away), content
    // ahead is only valid up to the write cursor — a fraction of the ring's own total duration
    // on either side is comfortably inside both bounds without needing to track exactly how
    // much is really resident.
    int32_t maxOffset = (int32_t)(s_djLoopFrames / 2) - (int32_t)winFrames;
    physOffset = constrain(physOffset, -maxOffset, maxOffset);
    int64_t physStart64 = (int64_t)s_djScratchAnchorPhys + physOffset;
    physStart64 %= (int64_t)s_djLoopFrames;
    if (physStart64 < 0) physStart64 += s_djLoopFrames;

    djCopyRingWindow((uint32_t)physStart64, winFrames);
    pcm_register_extern16(DJ_PRESET, s_djWindowPad, winFrames, PCM_TARGET_RATE / 2, 69, 0, winFrames - 1);
    if (s_djPlaying) {
        amyPlayPcm(DJ_OSC, DJ_PRESET, 1.0f, 1.0f);
        amy_event e = amy_default_event();
        e.osc = DJ_OSC; e.midi_note = 69.0f + 12.0f * log2f(s_djSpeed);
        amy_add_event(&e);
    }

    s_djElapsedSec = (s_djEstTotalSec > 0.01f) ? constrain(targetSec, 0.0f, s_djEstTotalSec) : fmaxf(targetSec, 0.0f);

    taskENTER_CRITICAL(&s_djJobMux);
    s_djNextBuilding = false;
    taskEXIT_CRITICAL(&s_djJobMux);
}

void audioDJScratchNudge(float jx) {
    if (!s_djLoaded || !s_djPlaying) return;
    if (fabsf(jx) <= DJ_SCRATCH_DEADZONE) return;
    if (!s_djScratchGestureActive) {
        if (!djScratchBegin()) return;
    }
    // s_djElapsedSec is NOT refreshed via audioDJAdvancePositionEstimate() here — that
    // function early-returns while a gesture is active anyway (see its own comment), and
    // scratch fully owns s_djElapsedSec via djScratchStepTo()'s direct commits instead.
    float target = s_djElapsedSec + jx * DJ_SCRATCH_MAX_STEP_SEC;
    djScratchStepTo(target);
}

void audioDJScratchEnd() {
    if (!s_djScratchGestureActive) return;
    s_djScratchGestureActive = false;
    // Resume ordinary playback from wherever scratch left the playhead — the same reposition
    // primitive every other discontinuous jump uses. A real, expected retrigger click on
    // release (an actual turntable's scratch release isn't perfectly seamless either) —
    // unlike the reverse-toggle button, which specifically needed to avoid one.
    djReposition(s_djElapsedSec, s_djReverse);
}

// ---- Stutter/glitch (joystick click, hold) ----
// Freezes the playhead and tightly loops a short BPM-synced window at the current position
// until released — deliberately retriggers, same as scratch above (see its own comment for
// why). Reads its working window straight out of the live ring (djCopyRingWindow() handles
// wraparound) instead of a dedicated backup/restore dance — the ring already holds audio in
// correct playback order everywhere, so this needs no adjacency precondition at all, and
// release is simply "resume from the frozen position" — the same primitive every other
// discontinuous reposition uses.
void audioDJStutterStart(float bpmForSync) {
    s_djPrefill = false;
    if (!s_djLoaded || !s_djPlaying || s_djStuttering || !s_djRing || !s_djWindowPad) return;
    bpmForSync = fmaxf(bpmForSync, 1.0f);
    bool claimed = false;
    taskENTER_CRITICAL(&s_djJobMux);
    if (!s_djNextBuilding) { s_djNextBuilding = true; claimed = true; }
    taskEXIT_CRITICAL(&s_djJobMux);
    if (!claimed) return;  // background job in flight — no stutter this press

    uint32_t physNow = djPhysicalReadIndexNow();
    float winSec = constrain(60.0f / bpmForSync / 4.0f, DJ_STUTTER_MIN_SEC, DJ_STUTTER_MAX_SEC);
    uint32_t winFrames = min((uint32_t)(winSec * DJ_PLAYBACK_RATE_HZ), s_djWindowPadFrames);

    djCopyRingWindow(physNow, winFrames);
    pcm_register_extern16(DJ_PRESET, s_djWindowPad, winFrames, PCM_TARGET_RATE / 2, 69, 0, winFrames - 1);
    amyPlayPcm(DJ_OSC, DJ_PRESET, 1.0f, 1.0f);
    amy_event e = amy_default_event();
    e.osc = DJ_OSC; e.midi_note = 69.0f + 12.0f * log2f(s_djSpeed);
    amy_add_event(&e);

    s_djStuttering = true;
    s_djStutterFrozenElapsedSec = s_djElapsedSec;
    taskENTER_CRITICAL(&s_djJobMux);
    s_djNextBuilding = false;
    taskEXIT_CRITICAL(&s_djJobMux);
}

void audioDJStutterEnd() {
    if (!s_djStuttering) return;
    s_djStuttering = false;
    djReposition(s_djStutterFrozenElapsedSec, s_djReverse);
}

bool audioDJIsStuttering() { return s_djStuttering; }

bool audioDJJobBusy() { return s_djNextBuilding; }
bool audioDJStreamActive() { return s_djPlaying || s_djNextBuilding; }
void audioDJCloseFiles() { djPcmCacheClose(); }  // drops the tee's open .tmp and the cache read handle (called with no job in flight)
void audioDJForceQuiesce() { s_djTrackGen++; }

// Bumped every time the DISPLAYED half changes (half-boundary crossing, load, seek) — lets
// the UI know when to recompute its cached waveform (audioDJComputeWaveform() scans a
// whole half, too expensive to call every drawScreen() frame) — unlike audioDJGetLoadGen(),
// which only bumps on a brand new track load, this bumps on every half crossing too, since
// the waveform pane shows "the currently-playing half", not "the track".
uint32_t audioDJGetChunkGen() { return s_djChunkGen; }

// How far playback is through the current DJ_DISPLAY_WINDOW_SEC-long display window — a
// sawtooth that resets every DJ_DISPLAY_WINDOW_SEC of track time, sweeping 0->1 forward and
// 1->0 in reverse (mirroring the CALLER's own drawing convention — see main.cpp's MODE_DJ
// transport view), per the user's own report that a fixed sweep direction didn't read as
// "playing backward".
float audioDJGetChunkProgressFrac() {
    audioDJAdvancePositionEstimate();  // ensure freshness independent of call order vs audioDJGetPosFrac() this frame
    float raw = fmodf(s_djElapsedSec, DJ_DISPLAY_WINDOW_SEC) / DJ_DISPLAY_WINDOW_SEC;
    if (raw < 0.0f) raw += 1.0f;
    return s_djReverse ? (1.0f - raw) : raw;
}

// True if the current display window's own earliest point is the true start of the track —
// lets the UI extend the on-screen cursor down to the true left edge for this one window
// instead of the usual squeezed floor.
bool audioDJChunkTouchesStart() { return s_djElapsedSec < DJ_DISPLAY_WINDOW_SEC; }
// True if the current display window's own latest point is the true end of the track — lets
// the UI extend the on-screen cursor up to the true right edge for this one window instead of
// the usual squeezed ceiling.
bool audioDJChunkTouchesEnd() { return s_djEstTotalSec > 0.01f && s_djElapsedSec > s_djEstTotalSec - DJ_DISPLAY_WINDOW_SEC; }

// Waveform (128 peak bins) from a DJ_DISPLAY_WINDOW_SEC-long window of the ring around the
// current (estimated) read position — the old one-shot design could show the whole track
// since it held the whole track in RAM; this design never does, so the waveform pane
// necessarily shows a window around "now", not "the whole song". A real, disclosed UI change,
// not an oversight (see structure/SOFTWARE.md's DJ section).
bool audioDJComputeWaveform(uint8_t* waveform128) {
    if (!s_djLoaded || !waveform128 || !s_djRing) return false;
    uint32_t winFrames = min((uint32_t)(DJ_DISPLAY_WINDOW_SEC * DJ_PLAYBACK_RATE_HZ), s_djLoopFrames);
    if (winFrames < 16) return false;
    uint32_t physNow = djPhysicalReadIndexNow();
    // Only draw real, already-written audio — anything beyond how far the top-up loop has
    // actually caught up (s_djValidAheadSec) is either stale leftover ring content (left over
    // from the previous direction or the previous track) or, right after a fresh load, plain
    // uninitialized memory. Drawing it as if it were upcoming audio is what read as "reverse
    // changes the sample and its display" — the picture would show mostly-stale content for
    // a moment right after every toggle. Flat/silent instead for the not-yet-valid tail —
    // it fills in within the next couple of top-up cycles as real content actually lands.
    uint32_t validFrames = min(winFrames, (uint32_t)(s_djValidAheadSec * DJ_PLAYBACK_RATE_HZ));
    // Wraps the ring AT MOST ONCE across the whole window (winFrames is small relative to
    // s_djLoopFrames) — splitting it into up to two contiguous runs up front avoids a
    // per-sample modulo below. This isn't just a micro-optimization: this function runs on
    // the display task, which is HIGHER priority than the background grain-decode task (both
    // pinned to core 0) — a slow scan here directly preempts and delays grain decoding,
    // audible as the reported "mini saccades" in playback, not just a slow redraw.
    uint32_t firstRun = min(winFrames, s_djLoopFrames - physNow);
    uint32_t blk = winFrames / 128; if (blk < 1) blk = 1;
    // A 128-bin display is a coarse overview, not a sample-accurate scope — capping how many
    // samples get examined per bin (rather than scanning every one) is most of this
    // function's CPU cost eliminated for an imperceptible loss of peak precision.
    uint32_t stride = (blk + DJ_WAVEFORM_MAX_SAMPLES_PER_BIN - 1) / DJ_WAVEFORM_MAX_SAMPLES_PER_BIN;
    if (stride < 1) stride = 1;
    for (int i = 0; i < 128; i++) {
        uint32_t s0 = (uint32_t)i * blk, e0 = min(s0 + blk, winFrames);
        if (s0 >= validFrames) { waveform128[i] = 0; continue; }
        e0 = min(e0, validFrames);
        int32_t peak = 0;
        for (uint32_t j = s0; j < e0; j += stride) {
            uint32_t idx = (j < firstRun) ? (physNow + j) : (j - firstRun);
            int32_t v = s_djRing[idx];
            if (v < 0) v = -v;
            if (v > peak) peak = v;
        }
        waveform128[i] = (uint8_t)((int32_t)peak * 255 / 32768);
    }
    return true;
}

float audioDJGetLengthSeconds() { return s_djLoaded ? s_djEstTotalSec : 0.0f; }

// ==================== SS2 large-sample streaming ====================
// A single shared mono streaming voice for SS2/SAMPLE keys whose file didn't fit one
// SS2_CHUNK_SECONDS window (see config.h's "SS2 large-sample streaming" block and
// audioLoadKey()'s quick-metadata-probe earlier in this file, which decides large vs small
// and populates s_ssKeyMeta[]/s_keyLargePath[]). Uses the SAME true streaming circular
// buffer design as DJ_OSC (see that section's own header comment for the full rationale) —
// one small ring, one write cursor continuously topped up by a background one-grain-at-a-
// time job, AMY reading it forward forever after a single initial retrigger. This is a
// second INSTANTIATION of that design (own state, own functions, ss-prefixed), not shared
// code with DJ — same reasoning DJ's own predecessor design already documented here: keeping
// them independent means a bug in one can never reach into the other.
//
// Differences from DJ, since this isn't a manual scrub/speed transport deck: no seek, no
// speed, no scratch/stutter/grain-spray, no waveform display, and no direction TOGGLE
// (direction is fixed for the whole playback, set once at trigger) — a key is triggered
// forward (NRM/FUL) or backward (REV), plays once, and STOPS at the true end/start of the
// file instead of looping forever (a sequenced one-shot hit, not a DJ deck). Only one large
// key streams at a time; (re)triggering any large key — the same one or a different one —
// cuts whatever this voice was doing and starts fresh.

static int16_t* s_ssRing       = nullptr;   // s_ssRingFrames contiguous samples, one alloc
static uint32_t s_ssRingFrames = SS2_RING_FRAMES;  // may shrink once on first use if PSRAM is tight
// AMY's own native loop period — see DJ's s_djLoopFrames for why this must differ from
// s_ssRingFrames and why every physical-position wraparound below uses THIS, not the ring's
// raw allocated size (a real, measured bug in DJ's own first version of this design).
static uint32_t s_ssLoopFrames = SS2_RING_FRAMES;

// ---- Write cursor: owned entirely by our own code, never estimated ----
static uint32_t s_ssWriteFrame      = 0;
static float    s_ssWriteLogicalSec = 0.0f;
// Explicit fill-level counter (seconds of already-written, not-yet-consumed audio ahead of
// the read cursor) — NOT derived from comparing ring positions; see DJ's s_djValidAheadSec
// for why that comparison is ambiguous under wraparound and caused a real stuck-forever bug.
static float    s_ssValidAheadSec    = 0.0f;
static uint32_t s_ssValidAheadLastMs = 0;

// ---- Read cursor: wall-clock ESTIMATED, same reasoning as DJ (AMY has no phase-readback) ----
// No physical-ring-position tracking needed here (unlike DJ) — SS2 never repositions after
// trigger (no seek/scratch/stutter/reverse-toggle) and has no waveform display, the only
// things DJ's own equivalent (s_djReadAnchorFrame/djPhysicalReadIndexNow) exists for. The
// fill-level counter below is deliberately NOT derived from a physical read position at all.
static float    s_ssElapsedSec            = 0.0f;
static uint32_t s_ssPlayStartMs           = 0;
static float    s_ssElapsedSecAtPlayStart = 0.0f;
static bool     s_ssReverse = false;  // fixed for the whole playback, set once at trigger — never toggles mid-stream
static bool     s_ssPlaying = false;

// One-shot stop condition, replacing DJ's "wrap to the other end and keep looping forever":
// once a grain decode hits true EOF/SOF, there is nothing further to write — s_ssFinishSec
// records exactly where playback should STOP once the read cursor reaches it (checked in
// ssAdvancePositionEstimate()).
static bool  s_ssFinishedWriting = false;
static float s_ssFinishSec       = 0.0f;

// ---- Background grain-decode job (same one-job-at-a-time pattern as DJ) ----
static volatile bool s_ssNextBuilding = false;
static portMUX_TYPE  s_ssJobMux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t       s_ssDecodeGen = 0;
static uint32_t       s_ssTrackGen = 0;  // bumped ONLY on an actual key trigger — see s_djTrackGen's own comment
static uint32_t s_ssGrainCapFrames = 0;  // DJ_STREAM_GRAIN_SEC * rate, sized on first use
static int16_t* s_ssGrainBuf       = nullptr;  // one reusable scratch decode buffer — safe as a single static since only one grain job is ever in flight
static bool     s_ssMp3ContinuePossible = false;  // true once the persistent mp3 session is known to be positioned for a forward continuation

static uint8_t  s_ssKeyIdx    = 0xFF;   // which key currently owns the shared voice (0xFF = none)
static bool     s_ssIsMp3     = false;
static char     s_ssPath[256] = {};
static bool     s_ssFinished  = false;  // true once true EOF/start-of-file reached — not retriggerable via chunk swap anymore, just a stopped voice ready for a fresh trigger
static float    s_ssEstTotalSec = 0.0f;
static float    s_ssVel = 1.0f;

// wav header fields for the CURRENTLY STREAMING key — copied from s_ssKeyMeta[keyIdx] at
// trigger time (see audioPlayKeyStreamed()), not re-parsed from the file on every trigger.
static uint32_t s_ssWavDataStart = 0, s_ssWavBpf = 0, s_ssWavFileSR = 0, s_ssWavNumCh = 0;
static uint32_t s_ssWavBps       = 16;
static bool     s_ssWavIsFloat   = false;
static uint32_t s_ssWavTotalInFrames = 0;

// Persistent mp3 decoder session for sequential forward continuation — separate instance
// from DjMp3Session, same reasoning (see that struct's own comment in the DJ section).
struct SsMp3Session {
    bool active = false;
    bool pend = false;       // a decoded frame in frameBuf still has un-emitted output (grain filled mid-frame)
    uint32_t pendFr = 0;
    File f;
    HMP3Decoder dec = nullptr;
    uint8_t* inBuf = nullptr;
    int16_t* frameBuf = nullptr;
    uint8_t* ptr = nullptr;
    int bytesLeft = 0;
    bool eof = false;
    uint32_t sampleRate = 44100;
    bool gotInfo = false;
    uint32_t inPos = 0;
    uint32_t outPos = 0;
};
static SsMp3Session s_ssMp3Session;

static void ssMp3SessionClose() {
    if (s_ssMp3Session.active) {
        if (s_ssMp3Session.dec) MP3FreeDecoder(s_ssMp3Session.dec);
        if (s_ssMp3Session.inBuf) free(s_ssMp3Session.inBuf);
        if (s_ssMp3Session.frameBuf) free(s_ssMp3Session.frameBuf);
        if (s_ssMp3Session.f) s_ssMp3Session.f.close();
    }
    s_ssMp3Session = SsMp3Session{};
}

// ---- .pcm16 cache-backed streaming — mirrors the DJ section's own (see its comment) ----
static bool        s_ssUsingPcmCache = false;
static File         s_ssCacheFile;
static PcmCacheHdr s_ssCacheHdr;
static PcmTee       s_ssTee;

static void ssPcmCacheClose() {
    if (s_ssUsingPcmCache && s_ssCacheFile) s_ssCacheFile.close();
    s_ssUsingPcmCache = false;
    teeDisarm(s_ssTee);
}


// Decodes up to maxFrames (at PCM_TARGET_RATE) of mono PCM into outBuf — see
// djDecodeMp3Chunk's extensive header comment (DJ section above) for why the loop
// termination, the persistent session, and the smaller feed size are all shaped the way
// they are; this is the same logic against a separate session instance.
static uint32_t ssDecodeMp3Chunk(const char* path, uint32_t startByte, int16_t* outBuf,
                                  uint32_t maxFrames, uint32_t* outNextByte, bool continueSession) {
    bool freshStart = !(continueSession && s_ssMp3Session.active);
    if (freshStart) {
        ssMp3SessionClose();
        File f = SD.open(path, FILE_READ);
        if (!f) return 0;
        uint32_t fileSize = f.size();
        if (startByte >= fileSize) { f.close(); return 0; }
        f.seek(startByte);
        if (startByte == 0) mp3SkipId3v2(f);  // see mp3SkipId3v2's own comment
        HMP3Decoder dec = MP3InitDecoder();
        if (!dec) { f.close(); return 0; }
        uint8_t* inBuf = (uint8_t*)malloc(DJ_MP3_FEED_SIZE);
        int16_t* frameBuf = inBuf ? (int16_t*)malloc(MAX_NGRAN * MAX_NCHAN * MAX_NSAMP * sizeof(int16_t)) : nullptr;
        if (!inBuf || !frameBuf) { if(inBuf) free(inBuf); MP3FreeDecoder(dec); f.close(); return 0; }
        s_ssMp3Session.active    = true;
        s_ssMp3Session.f         = f;
        s_ssMp3Session.dec       = dec;
        s_ssMp3Session.inBuf     = inBuf;
        s_ssMp3Session.frameBuf  = frameBuf;
        s_ssMp3Session.ptr       = inBuf;
        s_ssMp3Session.bytesLeft = 0;
        s_ssMp3Session.eof       = false;
        s_ssMp3Session.sampleRate = 44100;
        s_ssMp3Session.gotInfo   = false;
        s_ssMp3Session.inPos     = 0;
        s_ssMp3Session.outPos    = 0;
        s_ssMp3Session.pend      = false;
    }
    SsMp3Session& S = s_ssMp3Session;
    auto refill = [&]() {
        if (S.ptr != S.inBuf && S.bytesLeft > 0) memmove(S.inBuf, S.ptr, S.bytesLeft);
        S.ptr = S.inBuf;
        if (!S.eof) {
            uint32_t space = (uint32_t)(DJ_MP3_FEED_SIZE - S.bytesLeft);
            uint32_t rd = S.f.read(S.inBuf + S.bytesLeft, space);
            S.bytesLeft += (int)rd;
            if (rd < space) S.eof = true;
        }
    };
    if (freshStart) {
        refill();
        int sync = MP3FindSyncWord(S.ptr, S.bytesLeft);
        if (sync < 0) { ssMp3SessionClose(); if(outNextByte)*outNextByte=startByte; return 0; }
        S.ptr += sync; S.bytesLeft -= sync;
    }

    uint32_t chunkStartOutPos = S.outPos;
    int consErr = 0;
    while (S.outPos - chunkStartOutPos < maxFrames) {
        if (!S.pend) {
            if (S.bytesLeft < MAINBUF_SIZE && !S.eof) refill();
            int ret = MP3Decode(S.dec, &S.ptr, &S.bytesLeft, S.frameBuf, 0);
            if (ret == ERR_MP3_INDATA_UNDERFLOW) {
                if (S.eof) { if (++consErr > 32) break; continue; }
                refill(); consErr = 0; continue;
            }
            if (ret == ERR_MP3_MAINDATA_UNDERFLOW) {
                if (S.eof) { if (++consErr > 32) break; }
                continue;
            }
            if (ret < 0) {
                if (++consErr > 64) break;
                int sk = MP3FindSyncWord(S.ptr + 1, S.bytesLeft - 1);
                if (sk < 0) break; S.ptr += sk + 1; S.bytesLeft -= sk + 1; continue;
            }
            consErr = 0;
            MP3FrameInfo info; MP3GetLastFrameInfo(S.dec, &info);
            if (info.nChans < 1 || info.nChans > 2 || info.outputSamps < 1
                || info.outputSamps > (int)(MAX_NGRAN * MAX_NCHAN * MAX_NSAMP)) continue;
            if (!S.gotInfo) { S.sampleRate = (uint32_t)info.samprate; S.gotInfo = true; }
            uint32_t frOut0 = (uint32_t)info.outputSamps / (uint32_t)info.nChans;
            if (frOut0 == 0) break;
            if (info.nChans > 1)
                for (uint32_t j = 0; j < frOut0; j++)
                    S.frameBuf[j] = (int16_t)(((int32_t)S.frameBuf[j*2] + S.frameBuf[j*2+1]) >> 1);
            S.pend = true; S.pendFr = frOut0;
        }
        // Resample the pending decoded frame from wherever the output cursor is. A grain that fills
        // up MID-frame leaves the frame pending (its remaining output is produced by the NEXT call,
        // straight from S.frameBuf, before any new frame is decoded). The old code advanced past the
        // whole frame at the clip point and discarded the tail — the next frame's outputs for that
        // span then had a negative source position, clamped to sample 0, i.e. a held value for up to
        // ~24ms at EVERY grain boundary: the audible "micro-coupures" every half second.
        const uint32_t frOut = S.pendFr;
        float ratio = (float)S.sampleRate / PCM_TARGET_RATE;
        uint32_t outEndAbs = (uint32_t)((S.inPos + frOut) / ratio);
        uint32_t room = maxFrames - (S.outPos - chunkStartOutPos);
        uint32_t want = outEndAbs > S.outPos ? outEndAbs - S.outPos : 0;
        uint32_t n = want < room ? want : room;
        uint32_t base = S.outPos - chunkStartOutPos;
        for (uint32_t oi = 0; oi < n; oi++) {
            float sp = (float)(S.outPos + oi) * ratio - (float)S.inPos;
            if (sp < 0.0f) sp = 0.0f;
            uint32_t si0 = (uint32_t)sp; float frac = sp - si0;
            if (si0 >= frOut) si0 = frOut - 1;
            uint32_t si1 = si0 + 1; if (si1 >= frOut) si1 = frOut - 1;
            outBuf[base + oi] = (int16_t)((int32_t)S.frameBuf[si0] + (int32_t)((S.frameBuf[si1] - S.frameBuf[si0]) * frac));
        }
        S.outPos += n;
        if (S.outPos >= outEndAbs) { S.inPos += frOut; S.pend = false; }
    }
    if (outNextByte) *outNextByte = (uint32_t)S.f.position() - (uint32_t)S.bytesLeft;
    return S.outPos - chunkStartOutPos;
}

static uint32_t ssMp3EstimateByteForSec(const char* path, float targetSec) {
    if (s_ssEstTotalSec <= 0.01f) return 0;
    File f = SD.open(path, FILE_READ);
    if (!f) return 0;
    uint32_t fileSize = f.size();
    f.close();
    float frac = constrain(targetSec / s_ssEstTotalSec, 0.0f, 1.0f);
    if (frac <= 0.0f) return 0;  // start of file: the decoder skips the ID3 tag itself
    uint32_t tag = mp3Id3v2Size(path);
    if (tag >= fileSize) tag = 0;
    return tag + (uint32_t)(frac * (fileSize - tag));
}

static uint32_t ssWavByteForSec(float targetSec) {
    if (s_ssWavFileSR == 0) return s_ssWavDataStart;
    uint32_t frame = (uint32_t)(constrain(targetSec, 0.0f, s_ssEstTotalSec) * s_ssWavFileSR);
    if (frame > s_ssWavTotalInFrames) frame = s_ssWavTotalInFrames;
    return s_ssWavDataStart + frame * s_ssWavBpf;
}

// See djDecodeWavChunk's comment (DJ section) for why startByte is clamped up to the real
// data start — byte 0 of the FILE is always inside the RIFF/fmt header for wav.
static uint32_t ssDecodeWavChunk(const char* path, uint32_t startByte, int16_t* outBuf, uint32_t maxFrames, uint32_t* outNextByte) {
    if (startByte < s_ssWavDataStart) startByte = s_ssWavDataStart;
    File f = SD.open(path, FILE_READ);
    if (!f || s_ssWavFileSR == 0) { if(f) f.close(); return 0; }
    f.seek(startByte);
    uint32_t startInFrame = s_ssWavBpf ? (startByte - s_ssWavDataStart) / s_ssWavBpf : 0;
    uint32_t totalIn = s_ssWavTotalInFrames > startInFrame ? s_ssWavTotalInFrames - startInFrame : 0;
    float ratio = (float)s_ssWavFileSR / (float)PCM_TARGET_RATE;
    uint32_t maxIn = (uint32_t)(maxFrames * ratio) + 2;
    if (totalIn > maxIn) totalIn = maxIn;

    int16_t* raw = (int16_t*)ps_malloc(STREAM_BATCH_FRAMES * s_ssWavNumCh * sizeof(int16_t));
    if (!raw) { f.close(); return 0; }

    uint32_t inPos = 0, outPos = 0;
    while (inPos < totalIn && outPos < maxFrames) {
        uint32_t batchIn = totalIn - inPos;
        if (batchIn > STREAM_BATCH_FRAMES) batchIn = STREAM_BATCH_FRAMES;
        if (!wavReadRaw(f, (uint16_t)s_ssWavBps, batchIn, (uint16_t)s_ssWavNumCh, raw, s_ssWavIsFloat)) break;
        uint32_t outEnd = (uint32_t)((float)(inPos + batchIn) / ratio);
        if (outEnd > maxFrames) outEnd = maxFrames;
        uint32_t outCnt = outEnd > outPos ? outEnd - outPos : 0;
        if (outCnt > 0) {
            if (s_ssWavFileSR == (uint32_t)PCM_TARGET_RATE && s_ssWavNumCh == 1) {
                memcpy(outBuf + outPos, raw, outCnt * sizeof(int16_t));
            } else if (s_ssWavFileSR == (uint32_t)PCM_TARGET_RATE && s_ssWavNumCh == 2) {
                for (uint32_t i = 0; i < outCnt; i++) outBuf[outPos + i] = (int16_t)(((int32_t)raw[i*2] + raw[i*2+1]) >> 1);
            } else {
                for (uint32_t oi = 0; oi < outCnt; oi++) {
                    float sp = (outPos + oi) * ratio - inPos;
                    if (sp < 0.0f) sp = 0.0f;
                    uint32_t si0 = (uint32_t)sp; float frac = sp - si0;
                    uint32_t si1 = si0 + 1; if (si1 >= batchIn) si1 = batchIn - 1;
                    int32_t s0 = 0, s1 = 0;
                    for (uint16_t c = 0; c < s_ssWavNumCh; c++) { s0 += raw[si0*s_ssWavNumCh+c]; s1 += raw[si1*s_ssWavNumCh+c]; }
                    if (s_ssWavNumCh > 1) { s0 /= (int32_t)s_ssWavNumCh; s1 /= (int32_t)s_ssWavNumCh; }
                    outBuf[outPos + oi] = (int16_t)(s0 + (int32_t)((s1 - s0) * frac));
                }
            }
        }
        inPos += batchIn;
        outPos = outEnd;
    }
    free(raw);
    if (outNextByte) *outNextByte = s_ssWavDataStart + (startInFrame + inPos) * s_ssWavBpf;
    f.close();
    return outPos;
}

struct SsStreamJob {
    uint32_t genAtSpawn;
    float    readFromSec;     // ascending decode-start position — see DJ's DjStreamJob for why always ascending
    uint32_t maxFrames;
    bool     reverse;
    bool     continueSession; // mp3 only, forward-continuation only — see DJ's own comment
    bool     activateNow;     // true: write at physical index 0 and retrigger (the initial trigger); false: silent top-up
};
static SsStreamJob s_ssJob;

// Computes the ascending [readFromSec, readFromSec+~capSec) span a REVERSE grain should
// decode so that, after reversal, it ends exactly at `endSec` — see DJ's djReverseGrainSpan
// for the full reasoning (clamping at true start-of-file instead of overshooting).
static void ssReverseGrainSpan(float endSec, uint32_t capFrames, float& outReadFromSec, uint32_t& outMaxFrames) {
    float capSec = (float)capFrames / DJ_PLAYBACK_RATE_HZ;
    float readFrom = endSec - capSec;
    if (readFrom < 0.0f) {
        outReadFromSec = 0.0f;
        outMaxFrames   = (uint32_t)(endSec * DJ_PLAYBACK_RATE_HZ);
    } else {
        outReadFromSec = readFrom;
        outMaxFrames   = capFrames;
    }
}

static bool ssStreamKickOff(float readFromSec, uint32_t maxFrames, bool reverse, bool continueSession, bool activateNow);  // forward decl

static void ssStreamDecodeJob() {
    SsStreamJob job = s_ssJob;
    uint32_t frames = 0, nextByte = 0;
    if (s_ssUsingPcmCache) {
        // Frame-exact — no byte-rate estimate needed, and no decode CPU at all.
        uint32_t startFrame = (uint32_t)(job.readFromSec * DJ_PLAYBACK_RATE_HZ);
        frames = readPcmCacheChunk(s_ssCacheFile, s_ssCacheHdr, startFrame, s_ssGrainBuf, job.maxFrames);
    } else {
        uint32_t startByte = job.continueSession ? 0
                            : (s_ssIsMp3 ? ssMp3EstimateByteForSec(s_ssPath, job.readFromSec)
                                         : ssWavByteForSec(job.readFromSec));
        nextByte = startByte;
        if (s_ssIsMp3) frames = ssDecodeMp3Chunk(s_ssPath, startByte, s_ssGrainBuf, job.maxFrames, &nextByte, job.continueSession);
        else           frames = ssDecodeWavChunk(s_ssPath, startByte, s_ssGrainBuf, job.maxFrames, &nextByte);
    }

    if (job.genAtSpawn != s_ssDecodeGen) {
        // Superseded by a newer trigger while this was decoding — discard. No pending-
        // reposition queue needed here (unlike DJ) — SS2 never repositions after the initial
        // trigger, so a lost race can only ever be superseded by a FRESH trigger, which
        // already starts its own job from scratch.
        taskENTER_CRITICAL(&s_ssJobMux);
        s_ssNextBuilding = false;
        taskEXIT_CRITICAL(&s_ssJobMux);
        return;
    }

    if (job.reverse && frames > 1)
        for (uint32_t i = 0; i < frames / 2; i++) { int16_t t = s_ssGrainBuf[i]; s_ssGrainBuf[i] = s_ssGrainBuf[frames-1-i]; s_ssGrainBuf[frames-1-i] = t; }

    if (frames == 0) {
        if (!job.reverse && !s_ssUsingPcmCache) { teeEnd(s_ssTee, job.readFromSec); teeFlush(s_ssTee); }
        // True EOF/SOF — nothing left to give from here. Unlike DJ (which wraps and keeps
        // looping forever), a streamed key is a one-shot: remember exactly where content ran
        // out so ssAdvancePositionEstimate() can stop the voice once the read cursor reaches
        // it, and stop producing more.
        taskENTER_CRITICAL(&s_ssJobMux);
        s_ssNextBuilding = false;
        taskEXIT_CRITICAL(&s_ssJobMux);
        s_ssFinishedWriting = true;
        s_ssFinishSec = job.readFromSec;
        if (job.activateNow) {
            // Triggered right at/past the true edge (e.g. an empty or zero-length file) —
            // nothing to play at all.
            s_ssPlaying = false;
            s_ssFinished = true;
        }
        return;
    }

    float grainDurSec = (float)frames / DJ_PLAYBACK_RATE_HZ;

    if (job.activateNow) {
        // Register ONCE — length/loopend cover the WHOLE ring and never change again after
        // this — then retrigger. A fresh note-on always resets AMY's phase to absolute ring
        // index 0, so the initial trigger writes there by construction (SS2 never
        // repositions again after this, unlike DJ, so this is the ONLY retrigger for the
        // whole lifetime of a streamed key's playback).
        memcpy(s_ssRing, s_ssGrainBuf, (size_t)frames * sizeof(int16_t));
        pcm_register_extern16(SS_PRESET, s_ssRing, s_ssRingFrames, PCM_TARGET_RATE / 2, 69,
                               0, s_ssRingFrames - 1 - DJ_WRAP_SAFETY_FRAMES);
        s_ssElapsedSec = job.reverse ? (job.readFromSec + grainDurSec) : job.readFromSec;
        s_ssElapsedSecAtPlayStart = s_ssElapsedSec;
        s_ssPlayStartMs = millis();
        if (s_ssPlaying) {
            amyPlayPcm(SS_OSC, SS_PRESET, s_ssVel, 1.0f);  // feedback=1.0f: same native-looping trick as DJ_OSC
        }
        s_ssWriteFrame      = frames % s_ssLoopFrames;
        s_ssWriteLogicalSec = job.reverse ? job.readFromSec : (job.readFromSec + grainDurSec);
        s_ssValidAheadSec    = grainDurSec;
        s_ssValidAheadLastMs = millis();
    } else {
        // Ordinary top-up: append at the write cursor, wrapping at s_ssLoopFrames (not the
        // raw physical buffer size — see s_ssLoopFrames's own comment). No register, no
        // retrigger.
        uint32_t wf = s_ssWriteFrame;
        uint32_t firstPart = min(frames, s_ssLoopFrames - wf);
        memcpy(s_ssRing + wf, s_ssGrainBuf, (size_t)firstPart * sizeof(int16_t));
        if (firstPart < frames) memcpy(s_ssRing, s_ssGrainBuf + firstPart, (size_t)(frames - firstPart) * sizeof(int16_t));
        s_ssWriteFrame      = (wf + frames) % s_ssLoopFrames;
        s_ssWriteLogicalSec = job.reverse ? job.readFromSec : (job.readFromSec + grainDurSec);
        s_ssValidAheadSec  += grainDurSec;
    }
    if (!s_ssUsingPcmCache) { teeAppend(s_ssTee, job.readFromSec, job.reverse, s_ssGrainBuf, frames); teeFlush(s_ssTee); }
    s_ssMp3ContinuePossible = !job.reverse;

    taskENTER_CRITICAL(&s_ssJobMux);
    s_ssNextBuilding = false;
    taskEXIT_CRITICAL(&s_ssJobMux);
}

static SemaphoreHandle_t s_ssWorkSem = nullptr;
static void ssWorkerTask(void*) { for (;;) { xSemaphoreTake(s_ssWorkSem, portMAX_DELAY); ssStreamDecodeJob(); } }

static bool ssStreamKickOff(float readFromSec, uint32_t maxFrames, bool reverse, bool continueSession, bool activateNow) {
    // Defensive — audioPlayKeyStreamed() already refuses to trigger without these, so this
    // should never actually fire, but a decode task writing into a null buffer is a real
    // crash, not a graceful no-op (same reasoning as DJ's identical guard).
    if (!s_ssGrainBuf) return false;
    bool won = false;
    taskENTER_CRITICAL(&s_ssJobMux);
    if (!s_ssNextBuilding) { s_ssNextBuilding = true; won = true; }
    taskEXIT_CRITICAL(&s_ssJobMux);
    if (!won) return false;  // one job at a time — an ordinary top-up that loses the race
                              // just retries next tick; the initial trigger losing the race
                              // is handled by audioPlayKeyStreamed() waiting it out first
    s_ssJob = { s_ssDecodeGen, readFromSec, maxFrames, reverse, continueSession, activateNow };
    if (!s_ssWorkSem) {
        Serial.println("[SS] grain worker missing — job slot released");
        taskENTER_CRITICAL(&s_ssJobMux); s_ssNextBuilding = false; taskEXIT_CRITICAL(&s_ssJobMux);
        return false;
    }
    xSemaphoreGive(s_ssWorkSem);
    return true;
}

// Keeps the ring topped up — mirrors djStreamTopUp() exactly, including the explicit
// fill-level counter (s_ssValidAheadSec), NOT a ring-position comparison (see DJ's own
// comment on why that comparison is ambiguous under wraparound). Stops entirely once
// s_ssFinishedWriting — nothing more will ever be produced for this trigger.
static void ssStreamTopUp() {
    if (!s_ssPlaying || s_ssFinishedWriting) return;
    uint32_t nowMs = millis();
    float dSec = (nowMs - s_ssValidAheadLastMs) / 1000.0f;  // no speed control for SS2 — always 1x
    s_ssValidAheadLastMs = nowMs;
    s_ssValidAheadSec = fmaxf(0.0f, s_ssValidAheadSec - dSec);
    if (s_ssValidAheadSec >= DJ_STREAM_LOW_WATERMARK_SEC) return;
    if (s_ssNextBuilding) return;

    if (s_ssReverse) {
        float readFromSec; uint32_t maxFrames;
        ssReverseGrainSpan(s_ssWriteLogicalSec, s_ssGrainCapFrames, readFromSec, maxFrames);
        ssStreamKickOff(readFromSec, maxFrames, true, /*continueSession=*/false, /*activateNow=*/false);
    } else {
        ssStreamKickOff(s_ssWriteLogicalSec, s_ssGrainCapFrames, false,
                         /*continueSession=*/s_ssIsMp3 && s_ssMp3ContinuePossible, /*activateNow=*/false);
    }
}

// Triggers/retriggers the shared streaming voice for keyIdx. Cuts whatever it was doing
// (same key or a different one) and starts fresh — single shared voice, like MODE_DJ's one
// deck. Forward (reverse=false) starts at the real byte 0/data start; backward starts near
// the real end of file, so the FIRST audible frame is always the true edge of the file the
// direction implies, same as DJ's reverse-engage.
void audioPlayKeyStreamed(uint8_t keyIdx, bool reverse, float vel) {
    if (!audioReady || keyIdx >= SAMPLE_KEY_COUNT || !s_keyIsLarge[keyIdx]) return;
    if (!s_ssRing) {
        // Same shrink-with-floor retry as MODE_DJ's ring (see audioLoadDJTrack) — a single
        // small ring now (was two 30-second buffers: ~2.4MB reserved for this feature alone,
        // down to well under 200KB — the ring only needs enough margin to stay ahead of
        // real-time playback, not the whole file, regardless of how long the file is).
        uint32_t tryFrames = SS2_RING_FRAMES;
        for (;;) {
            if (s_ssRing) { free(s_ssRing); s_ssRing = nullptr; }
            s_ssRing = (int16_t*)ps_malloc((size_t)tryFrames * sizeof(int16_t));
            if (s_ssRing) {
                s_ssRingFrames = tryFrames;
                s_ssLoopFrames = tryFrames - DJ_WRAP_SAFETY_FRAMES;
                // ps_malloc does NOT zero memory — see audioLoadDJTrack's identical fix for
                // why registering the whole ring immediately, with only a tiny quick grain
                // actually written, needs this (a real hardware noise bug otherwise).
                memset(s_ssRing, 0, (size_t)tryFrames * sizeof(int16_t));
                break;
            }
            float trySec = tryFrames / DJ_PLAYBACK_RATE_HZ;
            uint32_t floorFrames = (uint32_t)(DJ_PREFETCH_MARGIN_SEC * DJ_PLAYBACK_RATE_HZ);
            if (tryFrames <= floorFrames) {
                Serial.printf("[SS] ring buffer alloc FAILED even at floor %.0fs (need %lu B, PSRAM free=%lu B largest_block=%lu B)\n",
                    trySec, (unsigned long)((size_t)tryFrames * sizeof(int16_t)),
                    (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                    (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
                return;
            }
            uint32_t nextFrames = tryFrames / 2;
            if (nextFrames < floorFrames) nextFrames = floorFrames;
            Serial.printf("[SS] ring buffer alloc failed at %.0fs (PSRAM free=%lu B largest_block=%lu B), retrying at %.0fs\n",
                trySec, (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
                nextFrames / DJ_PLAYBACK_RATE_HZ);
            tryFrames = nextFrames;
        }
        s_ssGrainCapFrames = (uint32_t)(DJ_STREAM_GRAIN_SEC * DJ_PLAYBACK_RATE_HZ);
        if (!s_ssGrainBuf) s_ssGrainBuf = (int16_t*)ps_malloc((size_t)s_ssGrainCapFrames * sizeof(int16_t));
        if (!s_ssGrainBuf) {
            // Same defensive-abort reasoning as DJ's identical failure path — never leave a
            // null buffer that the decode task would unconditionally write into.
            Serial.println("[SS] grain buffer alloc FAILED — aborting trigger");
            return;
        }
    } else {
        // Re-triggering into the SAME already-allocated ring: zero it so an underrun during
        // this trigger's own ramp-up can only ever expose silence, never a stale snippet of
        // whatever the previous key was streaming.
        memset(s_ssRing, 0, (size_t)s_ssRingFrames * sizeof(int16_t));
    }
    // Invalidate whatever the background grain task is currently working on, then wait
    // (bounded) for it to actually release the job slot before touching s_ssPath/s_ssRing/
    // etc. below — the same fix audioLoadDJTrack() needed for the identical race: retriggering
    // while a previous decode is still in flight is otherwise a genuine cross-core data race
    // on s_ssRing (today's un-migrated code cleared s_ssNextBuilding with NO synchronization
    // at all, worse than DJ's own pre-fix bug).
    amyStopOsc(SS_OSC);
    s_ssDecodeGen++;
    s_ssTrackGen++;  // a NEW key trigger — any cache build still running for the PREVIOUS one must abort (see s_ssTrackGen's own comment)
    uint32_t waitStart = millis();
    while (s_ssNextBuilding && millis() - waitStart < 600) delay(1);
    if (s_ssNextBuilding)
        Serial.println("[SS] trigger proceeding despite a still-in-flight grain task (timed out waiting) — should not happen in practice");
    ssMp3SessionClose();  // stale session (if any) belongs to a different file/position now
    ssPcmCacheClose();    // any cache from the PREVIOUS key is unconditionally stale

    s_ssKeyIdx      = keyIdx;
    s_ssIsMp3       = s_ssKeyMeta[keyIdx].isMp3;
    s_ssEstTotalSec = s_ssKeyMeta[keyIdx].estTotalSec;
    strncpy(s_ssPath, s_keyLargePath[keyIdx], sizeof(s_ssPath) - 1);
    s_ssPath[sizeof(s_ssPath) - 1] = '\0';
    if (s_ssIsMp3) {
        if (s_ssKeyMeta[keyIdx].hasPcmCache) {
            File tmp = SD.open(s_ssPath, FILE_READ);
            uint32_t srcSize = tmp ? tmp.size() : 0;
            if (tmp) tmp.close();
            if (srcSize > 0 && openValidPcmCache(s_ssPath, srcSize, s_ssCacheFile, s_ssCacheHdr)) {
                s_ssUsingPcmCache = true;
                Serial.printf("[SS] using pcm16 cache: %s (%u frames)\n", s_ssPath, s_ssCacheHdr.frameCount);
            }
        }
        if (!s_ssUsingPcmCache) {
            File tmp = SD.open(s_ssPath, FILE_READ);
            uint32_t srcSize = tmp ? tmp.size() : 0;
            if (tmp) tmp.close();
            if (srcSize > 0) teeArm(s_ssTee, s_ssPath, srcSize);
        }
    }
    if (!s_ssIsMp3) {
        s_ssWavDataStart     = s_ssKeyMeta[keyIdx].wavDataStart;
        s_ssWavBpf           = s_ssKeyMeta[keyIdx].wavBpf;
        s_ssWavFileSR        = s_ssKeyMeta[keyIdx].wavFileSR;
        s_ssWavNumCh         = s_ssKeyMeta[keyIdx].wavNumCh;
        s_ssWavBps           = s_ssKeyMeta[keyIdx].wavBps;
        s_ssWavIsFloat       = s_ssKeyMeta[keyIdx].wavIsFloat;
        s_ssWavTotalInFrames = s_ssKeyMeta[keyIdx].wavTotalInFrames;
    }
    s_ssReverse          = reverse;
    s_ssFinished         = false;
    s_ssFinishedWriting  = false;
    s_ssPlaying          = true;
    s_ssVel              = vel;
    s_ssMp3ContinuePossible = false;

    // Quick, not full-size (DJ_STREAM_QUICK_GRAIN_SEC's own comment in the DJ section) — this
    // is the very first audio of the hit, so it should decode (and therefore land, and
    // therefore retrigger) as fast as possible; the ordinary top-up loop takes over with
    // full-size grains right after.
    uint32_t quickCap = (uint32_t)(DJ_STREAM_QUICK_GRAIN_SEC * DJ_PLAYBACK_RATE_HZ);
    float readFromSec; uint32_t maxFrames;
    if (reverse) ssReverseGrainSpan(s_ssEstTotalSec, quickCap, readFromSec, maxFrames);
    else         { readFromSec = 0.0f; maxFrames = quickCap; }
    ssStreamKickOff(readFromSec, maxFrames, reverse, /*continueSession=*/false, /*activateNow=*/true);
}

bool audioKeyStreamStillPlaying(uint8_t keyIdx) {
    return s_ssPlaying && s_ssKeyIdx == keyIdx && !s_ssFinished;
}

// Stops the shared streaming voice's bookkeeping (not just its audio) — called from
// audioStopAllSamples(), which otherwise has no idea this voice exists (SS_OSC is outside
// the SAMPLE_OSC_BASE range it silences). Without this, s_ssPlaying stays true and
// ssAdvancePositionEstimate() (ticked unconditionally from loop()) keeps advancing and
// prefetching chunks in the background forever after a generic "stop all" — confirmed
// during this feature's own testing (a stray prefetch fired well after a test had already
// called audioStopAllSamples() and moved on).
static void ssStopStreamVoice() {
    s_ssPlaying = false;
    s_ssFinished = true;
    s_ssDecodeGen++;  // invalidate any in-flight decode job so it discards its result
}

// Advances s_ssElapsedSec and tops up the ring, mirroring audioDJAdvancePositionEstimate()
// — except at the true edge of the file (EOF forward / start-of-file backward), where a
// sequenced one-shot hit just STOPS instead of looping.
static void ssAdvancePositionEstimate() {
    if (!s_ssPlaying || s_ssFinished) return;
    float dSec = (millis() - s_ssPlayStartMs) / 1000.0f;
    s_ssElapsedSec = s_ssElapsedSecAtPlayStart + (s_ssReverse ? -dSec : dSec);

    if (s_ssFinishedWriting) {
        bool reached = s_ssReverse ? (s_ssElapsedSec <= s_ssFinishSec) : (s_ssElapsedSec >= s_ssFinishSec);
        if (reached) {
            s_ssElapsedSec = s_ssFinishSec;
            s_ssPlaying = false;
            s_ssFinished = true;
            amyStopOsc(SS_OSC);
            return;
        }
    }
    ssStreamTopUp();
}

void audioSSTick() { ssAdvancePositionEstimate(); }

void audioStreamWorkersInit() {
    if (!s_djWorkSem) {
        s_djWorkSem = xSemaphoreCreateBinary();
        if (xTaskCreatePinnedToCore(djWorkerTask, "djGrain", 12288, nullptr, 2, nullptr, 0) != pdPASS) { Serial.println("[DJ] worker task creation FAILED"); s_djWorkSem = nullptr; }
    }
    if (!s_ssWorkSem) {
        s_ssWorkSem = xSemaphoreCreateBinary();
        if (xTaskCreatePinnedToCore(ssWorkerTask, "ssGrain", 12288, nullptr, 2, nullptr, 0) != pdPASS) { Serial.println("[SS] worker task creation FAILED"); s_ssWorkSem = nullptr; }
    }
}

bool audioSSJobBusy() { return s_ssNextBuilding; }
bool audioSSStreamActive() { return s_ssPlaying || s_ssNextBuilding; }
void audioSSCloseFiles() { ssPcmCacheClose(); }
void audioSSForceQuiesce() { s_ssTrackGen++; }

void audioPlayGranular2(uint8_t oscIdx, uint8_t sampleIdx, uint8_t sliceIdx, bool reverse, float vel, uint8_t playMode, uint16_t attackMs, uint32_t amyTime) {
    if (!audioReady || sampleIdx >= GRAN2_MAX_SAMPLES) return;
    if (reverse && !ensureGranular2Reverse(sampleIdx)) {
        Serial.printf("[GR2 PLAY] s%d sl%d rev=1 SKIPPED: reverse buffer unavailable (PSRAM exhausted)\n",
                      sampleIdx, sliceIdx);
        return;
    }

    uint16_t preset;
    bool loop;
    if (playMode == 2) {
        // FUL: play the entire sample so the key drains the full source.
        // Reverse uses the pre-registered full-reversed preset (GRAN2_TAIL_BASE+sampleIdx).
        preset = reverse ? (uint16_t)(GRAN2_TAIL_BASE + sampleIdx)
                         : (uint16_t)(GRAN2_SOURCE_BASE + sampleIdx);
        loop = true;
    } else {
        // NRM / LOP: play the slice (fwd or rev).
        preset = reverse ? (uint16_t)(GRAN2_REV_BASE  + sampleIdx * GRAN2_MAX_SLICES + sliceIdx)
                         : (uint16_t)(GRAN2_FWD_BASE  + sampleIdx * GRAN2_MAX_SLICES + sliceIdx);
        loop = (playMode == 1);  // NRM=one-shot, LOP=loop
    }

    // Verify the preset is in the linked list before triggering
    {
        uint32_t chkLen = 0;
        const int16_t* chkPtr = pcm_get_sample_ram_for_preset(preset, &chkLen);
        Serial.printf("[GR2 PLAY] s%d sl%d rev=%d preset=%u -> ll_ptr=%p ll_len=%lu\n",
                      sampleIdx, sliceIdx, (int)reverse, preset,
                      (void*)chkPtr, (unsigned long)chkLen);
    }

    amy_event e = amy_default_event();
    e.osc       = (uint16_t)(GRANULAR_OSC_BASE + oscIdx);
    e.wave      = PCM;
    e.preset    = preset;
    e.midi_note = 69;
    e.velocity  = vel;
    e.feedback  = loop ? 1.0f : 0.0f;
    e.eg0_times[0] = attackMs; e.eg0_values[0] = 1.0f;
    // For looping, use a very long decay so the EG never completes during a held key.
    // This prevents AMY from retriggering the EG at each PCM loop boundary (which causes blipping).
    e.eg0_times[1] = 30000u;  // long sustain for both NRM and LOP: amplitude holds while key is held,
                               // so the LPF is audible regardless of play mode
    e.eg0_values[1] = 1.0f;
    e.eg0_times[2] = 10;    e.eg0_values[2] = 0.0f;  // 10ms release on note-off
    if (s_pcmLPFCutoff > 10.0f) {
        e.filter_type = s_pcmLPFType;
        e.filter_freq_coefs[COEF_CONST] = s_pcmLPFCutoff;
        e.resonance = s_pcmLPFReso;
    }
    s_gran2ActiveOscMask |= (1u << oscIdx);
    if (amyTime != UINT32_MAX) e.time = amyTime;
    amy_add_event(&e);
}

void audioPlayGranular2Ful(uint8_t oscIdx, uint8_t sampleIdx, bool reverse, float vel, float startFrac) {
    if (!audioReady || sampleIdx >= GRAN2_MAX_SAMPLES || !s_gran2Loaded[sampleIdx]) return;

    uint32_t wlen = 0;
    const int16_t* buf = nullptr;    // forward: 16-bit source
    const int8_t*  buf8 = nullptr;   // reverse: 8-bit reversed copy
    if (reverse) {
        if (!ensureGranular2Reverse(sampleIdx)) {
            Serial.printf("[GR2 FUL] s%d rev=1 SKIPPED: reverse buffer unavailable (PSRAM exhausted)\n",
                          sampleIdx);
            return;
        }
        buf8 = s_gran2FullRevBuf[sampleIdx];
        wlen = s_gran2FullRevLen[sampleIdx];
    } else {
        buf = pcm_get_sample_ram_for_preset(GRAN2_SOURCE_BASE + sampleIdx, &wlen);
        if (!buf || !wlen) return;
    }

    uint32_t s0 = (uint32_t)(constrain(startFrac, 0.0f, 1.0f) * (float)wlen);
    if (s0 >= wlen) s0 = 0;
    uint32_t tailLen = wlen - s0;
    if (tailLen < 2) { s0 = 0; tailLen = wlen; }

    // Register a tail preset pointing directly into the existing buffer at s0 (no PSRAM copy).
    // The preset loops [s0→wlen] while held, starting naturally at position 0 of the tail
    // (= s0 of the original) — no trigger_phase needed, no phase ordering ambiguity.
    const uint32_t rate = PCM_TARGET_RATE / 2;
    if (reverse)
        pcm_register_extern8((uint16_t)(GRAN2_TAIL_BASE + sampleIdx),
                              buf8 + s0, tailLen, rate, 69, 0, (int32_t)(tailLen - 1));
    else
        pcm_register_extern16((uint16_t)(GRAN2_TAIL_BASE + sampleIdx),
                              buf + s0, tailLen, rate, 69, 0, (int32_t)(tailLen - 1));

    amy_event e = amy_default_event();
    e.osc          = (uint16_t)(GRANULAR_OSC_BASE + oscIdx);
    e.wave         = PCM;
    e.preset       = (uint16_t)(GRAN2_TAIL_BASE + sampleIdx);
    e.midi_note    = 69;
    e.velocity     = vel;
    e.feedback     = 1.0f;
    e.eg0_times[0] = 5;     e.eg0_values[0] = 1.0f;
    e.eg0_times[1] = 30000; e.eg0_values[1] = 1.0f;
    e.eg0_times[2] = 10;    e.eg0_values[2] = 0.0f;
    if (s_pcmLPFCutoff > 10.0f) {
        e.filter_type = s_pcmLPFType;
        e.filter_freq_coefs[COEF_CONST] = s_pcmLPFCutoff;
        e.resonance = s_pcmLPFReso;
    }
    s_gran2ActiveOscMask |= (1u << oscIdx);
    amy_add_event(&e);
}

void audioStopGranular2(uint8_t oscIdx) {
    if (!audioReady) return;
    s_gran2ActiveOscMask &= ~(1u << oscIdx);
    amy_event e = amy_default_event();
    e.osc      = (uint16_t)(GRANULAR_OSC_BASE + oscIdx);
    // SILENT wave produces zero audio output immediately — no waveform is rendered regardless
    // of PCM loop state. This cuts LOP/FUL without waiting for a loop boundary check.
    e.wave     = SILENT;
    e.velocity = 0;
    amy_add_event(&e);
}

bool audioGranular2HasReverse(uint8_t sampleIdx) {
    if (sampleIdx >= GRAN2_MAX_SAMPLES) return false;
    // Reverse now builds lazily on first actual reverse playback (see ensureGranular2Reverse),
    // so any loaded sample can go reverse — this no longer reflects whether the (possibly not
    // yet built) reverse buffer already exists, only whether the sample itself is loaded.
    return s_gran2Loaded[sampleIdx];
}

uint32_t audioGranular2SampleLenMs(uint8_t sampleIdx) {
    if (sampleIdx >= GRAN2_MAX_SAMPLES || !s_gran2Loaded[sampleIdx]) return 0;
    uint32_t totalLen = 0;
    pcm_get_sample_ram_for_preset(GRAN2_SOURCE_BASE + sampleIdx, &totalLen);
    if (totalLen == 0) return 0;
    return (uint32_t)((uint64_t)totalLen * 1000u / PCM_TARGET_RATE);
}

void audioUnloadGranular2Slot(uint8_t sampleIdx) {
    if (sampleIdx >= GRAN2_MAX_SAMPLES) return;
    for (uint8_t i = 0; i < GRAN2_MAX_SLICES; i++) {
        pcm_unload_preset(GRAN2_FWD_BASE + sampleIdx * GRAN2_MAX_SLICES + i);
        pcm_unload_preset(GRAN2_REV_BASE + sampleIdx * GRAN2_MAX_SLICES + i);
    }
    // GRAN2_TAIL_BASE+sampleIdx: full-rev preset (registered at compute time, memory owned by FullRevBuf)
    pcm_unload_preset(GRAN2_TAIL_BASE + sampleIdx);
    if (s_gran2FullRevBuf[sampleIdx]) {
        free(s_gran2FullRevBuf[sampleIdx]);
        s_gran2FullRevBuf[sampleIdx] = nullptr;
        s_gran2FullRevLen[sampleIdx] = 0;
    }
    pcm_unload_preset(GRAN2_SOURCE_BASE + sampleIdx);
    s_gran2Loaded[sampleIdx] = false;
}

void audioUnloadGranular2() {
    s_gran2ActiveOscMask = 0;
    for (int i = 0; i < 32; i++) {
        amy_event e = amy_default_event();
        e.osc = (uint16_t)(GRANULAR_OSC_BASE + i); e.velocity = 0;
        amy_add_event(&e);
    }
    vTaskDelay(pdMS_TO_TICKS(20));
    for (uint8_t s = 0; s < GRAN2_MAX_SAMPLES; s++) {
        for (uint8_t i = 0; i < GRAN2_MAX_SLICES; i++) {
            pcm_unload_preset(GRAN2_FWD_BASE + s * GRAN2_MAX_SLICES + i);
            pcm_unload_preset(GRAN2_REV_BASE + s * GRAN2_MAX_SLICES + i);
        }
        pcm_unload_preset(GRAN2_TAIL_BASE + s);  // full-rev preset, memory freed with FullRevBuf below
        if (s_gran2FullRevBuf[s]) { free(s_gran2FullRevBuf[s]); s_gran2FullRevBuf[s] = nullptr; s_gran2FullRevLen[s] = 0; }
        pcm_unload_preset(GRAN2_SOURCE_BASE + s);
        s_gran2Loaded[s] = false;
    }
}

// ==================== MODULAR SYNTH (MODE_MODULAR, wavetable dual-osc) ====================
// See audio_engine.h — two dedicated dynamic channels, each wave=WAVETABLE. This is the
// first user-facing use of AMY's WAVETABLE oscillator anywhere in this app: e.preset
// selects which of the 5 built-in tables (pcm_wavetable_base..+pcm_wavetable_samples-1),
// e.duty_coefs[COEF_CONST] is the continuous within-table morph position — both ordinary
// per-event fields, no new AMY-level plumbing needed (render_wavetable() in oscillators.c
// already does the crossfade natively).
void audioModularOscInit(uint8_t ch, uint8_t tableIdx) {
    if (!audioReady) return;
    amy_event e = amy_default_event();
    e.synth = ch;
    e.wave = WAVETABLE;
    e.preset = (int16_t)(pcm_wavetable_base + constrain((int)tableIdx, 0, (int)pcm_wavetable_samples - 1));
    e.num_voices = NUM_SYNTH_VOICES;
    e.oscs_per_voice = 1;
    // Moderate pad-ish envelope so notes don't click — modular synth's whole point is
    // slow evolving wavetable movement, not percussive hits.
    e.eg0_times[0] = 15;  e.eg0_values[0] = 1.0f;
    e.eg0_times[1] = 400; e.eg0_values[1] = 0.7f;
    e.eg0_times[2] = 300; e.eg0_values[2] = 0.0f;
    amy_add_event(&e);
}

// Lighter-weight than audioModularOscInit(): only changes which wavetable is selected,
// without re-touching num_voices/oscs_per_voice/envelope. Use this for the pot-driven
// "change table" gesture (P2) — sending the FULL config event on every table change was
// silently killing whatever note was already sounding (a channel-reconfigure event acts
// like AMY's own patches_load_patch()/reset_osc(), which resets voice state — same class
// of bug already flagged for regular shape-switching elsewhere in this app), whereas a
// bare preset change lets the currently-held note continue on the new table.
void audioModularSetTable(uint8_t ch, uint8_t tableIdx) {
    if (!audioReady) return;
    amy_event e = amy_default_event();
    e.synth = ch;
    e.preset = (int16_t)(pcm_wavetable_base + constrain((int)tableIdx, 0, (int)pcm_wavetable_samples - 1));
    amy_add_event(&e);
}

void audioModularSetWtPos(uint8_t ch, float pos01) {
    if (!audioReady) return;
    amy_event e = amy_default_event();
    e.synth = ch;
    e.duty_coefs[COEF_CONST] = constrain(pos01, 0.0f, 1.0f);
    amy_add_event(&e);
}

void audioModularNoteOn(uint8_t note, float vel, float detuneSemisB) {
    if (!audioReady) return;
    amy_event ea = amy_default_event();
    ea.synth = MOD3_OSCA_CH; ea.midi_note = note; ea.velocity = vel;
    amy_add_event(&ea);
    amy_event eb = amy_default_event();
    eb.synth = MOD3_OSCB_CH; eb.midi_note = note; eb.velocity = vel;
    eb.pitch_bend = powf(2.0f, detuneSemisB / 12.0f);
    amy_add_event(&eb);
}

// velocity=0 with midi_note unset is AMY's dedicated "all notes off for this synth"
// event (see audioAllNotesOff()'s comment) — reused here for the modular synth's 2
// channels instead of tracking individual held notes.
void audioModularAllNotesOff() {
    if (!audioReady) return;
    for (uint8_t ch : {(uint8_t)MOD3_OSCA_CH, (uint8_t)MOD3_OSCB_CH}) {
        amy_event e = amy_default_event();
        e.synth = ch; e.velocity = 0;
        amy_add_event(&e);
    }
}

void audioModularNoteOff(uint8_t note) {
    if (!audioReady) return;
    amy_event ea = amy_default_event();
    ea.synth = MOD3_OSCA_CH; ea.midi_note = note; ea.velocity = 0;
    amy_add_event(&ea);
    amy_event eb = amy_default_event();
    eb.synth = MOD3_OSCB_CH; eb.midi_note = note; eb.velocity = 0;
    amy_add_event(&eb);
}

// Per-channel filter setter — audioSetFilter()/audioSetAllFiltersT() are hardcoded to
// SYNTH_CH, so the modular synth's own 2 channels need their own small setter.
void audioModularSetFilter(float cutoffHz, float resonance) {
    if (!audioReady) return;
    for (uint8_t ch : {(uint8_t)MOD3_OSCA_CH, (uint8_t)MOD3_OSCB_CH}) {
        amy_event e = amy_default_event();
        e.synth = ch;
        e.filter_type = FILTER_LPF24;
        e.filter_freq_coefs[COEF_CONST] = cutoffHz;
        e.resonance = resonance;
        amy_add_event(&e);
    }
}

// audioSetEnvelope() hardcodes e.synth=SYNTH_CH, so it never reaches MOD3_OSCA_CH/
// MOD3_OSCB_CH — same class of gap as the filter functions above. Bare eg0-only event
// (not a full reconfigure) so it's safe to call while a note is held, same reasoning as
// audioModularSetTable()/SetWtPos().
void audioModularSetEnvelope(const EnvParams &env) {
    if (!audioReady) return;
    for (uint8_t ch : {(uint8_t)MOD3_OSCA_CH, (uint8_t)MOD3_OSCB_CH}) {
        amy_event e = amy_default_event();
        e.synth = ch;
        e.eg0_times[0] = env.atk;  e.eg0_values[0] = 1.0f;
        e.eg0_times[1] = env.dec;  e.eg0_values[1] = env.sus;
        e.eg0_times[2] = env.rel;  e.eg0_values[2] = 0.0f;
        amy_add_event(&e);
    }
}

// audioSetPitchBend() also hardcodes e.synth=SYNTH_CH — MODE_MODULAR's joystick-Y "bend"
// feature was silently a no-op ever since it was added, since it called that function
// instead of targeting MOD3_OSCA_CH/MOD3_OSCB_CH. Same recurring gap as the two above.
void audioModularSetPitchBend(float ratio) {
    if (!audioReady) return;
    for (uint8_t ch : {(uint8_t)MOD3_OSCA_CH, (uint8_t)MOD3_OSCB_CH}) {
        amy_event e = amy_default_event();
        e.synth = ch; e.pitch_bend = ratio;
        amy_add_event(&e);
    }
}

