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
static QueueHandle_t     s_loadQueue  = NULL;
static volatile bool     s_svcAbort   = false;
static volatile bool     s_svcDone    = true;
static volatile uint8_t  s_currentOsc = 0xFF;  // OSC currently being loaded (0xFF = idle)
// Tracks which key-assigned sample presets have finished loading.
static volatile bool     s_keyLoaded[SAMPLE_KEY_COUNT]    = {};
static uint32_t          s_keyLengthMs[SAMPLE_KEY_COUNT]  = {}; // playback duration per key
static volatile uint8_t  s_keyError[SAMPLE_KEY_COUNT]     = {}; // KEY_ERR_* per key, 0=OK
static bool              s_keyHasRev[SAMPLE_KEY_COUNT]    = {}; // true if reversed preset registered (SS2 slots 0-15)
static uint8_t           s_loadError                      = KEY_ERR_NONE; // set by loaders before return false
static float s_sampleVolume  = 1.0f;  // 0.0–2.0; applied to vel on sample playback
static float   s_pcmLPFCutoff = 0.0f;  // 0 = no filter applied to PCM oscillators
static float   s_pcmLPFReso   = 1.5f;
static uint8_t s_pcmLPFType   = FILTER_NONE;  // type actif : LPF/HPF/BPF/NONE
static bool    s_synthChIsPatch = false; // true when SYNTH_CH has an AMY preset patch (Juno/DX7)
static float   s_patchVolumeScale = 1.0f; // per-patch amplitude compensation (J:ORG is intrinsically loud)
// Set to true when any filter FX writes filter_freq_coefs to SYNTH_CH for a patch.
static bool    s_patchFilterModified = false;

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
void audioSetAllFilters(float cutoffHz, float resonance) {
    if (!audioReady) return;
    bool bypass = (cutoffHz <= 10.0f || cutoffHz >= 18000.0f);
    // Bypass: skip SYNTH_CH event for patches — sending FILTER_NONE would clobber the patch's
    // internal LPF (e.g. activating reverb changes J:PNO's texture via FILT inactive reset path).
    // Active (non-bypass): still apply so FX like LFO or FILT work on patches as expected.
    if (!bypass || !s_synthChIsPatch) {
        amy_event e = amy_default_event();
        e.synth = SYNTH_CH;
        // For patches: don't change filter_type — only modulate freq/resonance so the preset's
        // internal filter character (e.g. Juno BPF stack) is preserved under LFO/FX modulation.
        if (!s_synthChIsPatch) e.filter_type = bypass ? FILTER_NONE : FILTER_LPF24;
        e.filter_freq_coefs[COEF_CONST] = bypass ? 18000.0f : cutoffHz;
        if (!bypass && !s_synthChIsPatch) { e.filter_freq_coefs[COEF_EG0] = 0.0f; e.filter_freq_coefs[COEF_EG1] = 0.0f; }
        e.resonance = bypass ? 1.0f : resonance;
        amy_add_event(&e);
        if (!bypass && s_synthChIsPatch) s_patchFilterModified = true;
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
}

// Same as audioSetAllFilters but with a configurable AMY filter type constant.
// Use for FX FILT effect when the user selects a different filter topology.
void audioSetAllFiltersT(float cutoffHz, float resonance, uint8_t filterType) {
    if (!audioReady) return;
    // Bypass when turned off (≤10 Hz) OR when cutoff is fully open (≥18000 Hz = paramMax).
    // A LPF24 at 18 kHz still colours the signal (phase shift + resonance peak) — treat as bypass.
    bool bypass = (cutoffHz <= 10.0f || cutoffHz >= 18000.0f);
    // Bypass: skip SYNTH_CH event for patches — FILTER_NONE on bypass clobbers the patch's
    // internal LPF (e.g. toggling reverb would make J:PNO sound harpsichord-like).
    // Active (non-bypass): still apply so FILT/DISTORT FX work on patches as expected.
    if (!bypass || !s_synthChIsPatch) {
        amy_event e = amy_default_event();
        e.synth = SYNTH_CH;
        // For patches: preserve filter_type so the preset's internal filter is not overridden.
        if (!s_synthChIsPatch) e.filter_type = bypass ? FILTER_NONE : filterType;
        e.filter_freq_coefs[COEF_CONST] = bypass ? 18000.0f : cutoffHz;
        if (!bypass && !s_synthChIsPatch) {
            e.filter_freq_coefs[COEF_EG0] = 0.0f;
            e.filter_freq_coefs[COEF_EG1] = 0.0f;
        }
        e.resonance = bypass ? 1.0f : resonance;
        amy_add_event(&e);
        if (!bypass && s_synthChIsPatch) s_patchFilterModified = true;
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
}

// Update only the cutoff/resonance of an already-active LPF — does NOT send filter_type.
// Sending filter_type on every smooth-tick tick causes AMY to re-init the biquad state,
// which is heard as an audible "pop" or "reset" on each encoder step.
void audioSetFilterFreq(float cutoffHz, float resonance) {
    if (!audioReady) return;
    amy_event e = amy_default_event();
    e.synth = SYNTH_CH;
    e.filter_freq_coefs[COEF_CONST] = cutoffHz;
    e.resonance = resonance;
    amy_add_event(&e);
    s_pcmLPFCutoff = cutoffHz;
    s_pcmLPFReso   = resonance;
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

void audioGetNativeCutoff(SynthShape shape, float& cc, float& res) {
    if ((uint8_t)shape >= SHAPE_COUNT) { cc = 18000.0f; res = 1.0f; return; }
    int16_t patch = shapePatch[(uint8_t)shape];
    if (patch >= 0) {
        getPatchNativeFilter(patch, cc, res);
    } else {
        cc  = kShapeFilter[(uint8_t)shape].cc;
        res = kShapeFilter[(uint8_t)shape].res;
    }
}

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
            case SHAPE_NOISE_WHITE:  e.wave = NOISE;    break;
            case SHAPE_NOISE_PINK:
                e.wave = NOISE;
                e.filter_type = FILTER_LPF;
                e.filter_freq_coefs[COEF_CONST] = 2000.0f;
                e.resonance = 1.0f;
                break;
            case SHAPE_NOISE_BROWN:
                e.wave = NOISE;
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
        if (pad.is16bit) {
            const int* src = (const int*)pad.data;
            for (uint32_t j = 0; j < pad.len; j++) buf[j] = (int16_t)src[j];
        } else {
            const int8_t* src = (const int8_t*)pad.data;
            for (uint32_t j = 0; j < pad.len; j++) buf[j] = (int16_t)src[j] * 256;
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
static void amyPlayPcm(uint8_t osc, uint16_t preset, float vel) {
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
    // feedback=0 prevents PCM loop (AMY uses feedback flag to signal looping)
    e.feedback = 0.0f;
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
    File f = SD.open(cp, FILE_READ);
    if (!f) return false;

    PcmCacheHdr h;
    // Only ever called for isGran16 presets (STONE/GRANULAR2/legacy GRANULAR source) —
    // they have no cheaper fallback tier at all, so use the wider budget (see
    // granMaxFrames()'s comment) rather than the general-purpose psramMaxFrames().
    uint32_t maxLoad = granMaxFrames();
    bool valid = ((uint32_t)f.read((uint8_t*)&h, sizeof(h)) == sizeof(h))
              && (memcmp(h.magic, "G16C", 4) == 0)
              && (h.srcSize == srcSize)
              && (h.frameCount > 0)
              && (h.pcmSampleRate == PCM_TARGET_RATE)  // reject stale caches with wrong rate
              && ((uint32_t)f.size() == (uint32_t)(sizeof(h) + h.frameCount * 2u));
    if (valid && h.frameCount > maxLoad) h.frameCount = maxLoad;
    if (!valid) { f.close(); return false; }

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
    while ((!eof || bytesLeft > 0) && totalFrames < estimatedTotal && !s_svcAbort) {
        if (bytesLeft < MAINBUF_SIZE && !eof) refill();
        int ret = MP3Decode(dec, &ptr, &bytesLeft, frameBuf, 0);
        if (ret == ERR_MP3_INDATA_UNDERFLOW)   { refill(); continue; }
        if (ret == ERR_MP3_MAINDATA_UNDERFLOW)  { continue; }
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
void audioLoadKey(const char* path, uint8_t keyIdx) {
    if (!audioReady || !s_loadQueue || keyIdx >= SAMPLE_KEY_COUNT) return;
    LoadReq req;
    strncpy(req.path, path, sizeof(req.path) - 1);
    req.path[sizeof(req.path) - 1] = '\0';
    req.preset = SAMPLE_PRESET_BASE + keyIdx;
    req.osc    = SAMPLE_OSC_BASE + keyIdx;
    req.vel    = 0.0f;  // load only; user triggers play with audioPlayKey()
    s_keyLoaded[keyIdx] = false;
    if (xQueueSend(s_loadQueue, &req, 0) != pdTRUE)
        Serial.printf("KEY %u: queue full, load dropped\n", keyIdx);
}

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

