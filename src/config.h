#pragma once
#include <Arduino.h>
#include "HWConfig.h"

// ==================== MODES ====================
enum AppMode : uint8_t {
    MODE_SYNTH = 0,
    MODE_OMNI,
    MODE_SAMPLE,
    MODE_LIGHT,
    MODE_LIGHTPLAY, // live light ripple mode
    MODE_BATTERY,
    MODE_SYSINFO,   // cyberpunk HUD — system info / visual test
    MODE_MOD2,      // modular analog synth: algorithm browser + P4-P7 modulation
    MODE_GRANULAR2, // granular2: multi-sample (2 or 4), fwd+rev only, per-sample split control
    MODE_MIDI,      // USB MIDI device: keyboard → NoteOn/Off, host → LED feedback (LaunchPad)
    MODE_TRACKER,   // 32-step quantized recorder: left 4×4 = instruments, right 4×4 = notes
    MODE_DRUM2,     // TR-808 style: 8 pads, per-pad pitch/decay/volume, sequencer, FX
    MODE_SYSEQ,     // 16-step polyphonic synth sequencer (up to 4 notes/step)
    MODE_303S,      // 16-step TB-303 step sequencer (note + accent + slide per step)
    MODE_SS2,       // 16-step sample sequencer: 16 slots, per-step alteration, shared clock
    MODE_ANIM,      // visual animations: wave / bars / techno / acid / 8bit
    MODE_I303,      // polyphonic TB-303: 6-voice chord/melody with full 303S sound engine
    MODE_VID,       // video player: 128×128 1bpp .bvid files from SD card
    MODE_LANIM,     // LED animations: flash, rainbow, chase, noise, organic — pot-controlled
    MODE_EXP,       // experimental theremin: joystick Y=pitch X=texture, keys=modifiers
    MODE_EXP2,      // polybounce: balls in rotating hexagon, bounces trigger notes
    MODE_EXP3,      // orbital: planets orbit star, passing trigger zone plays notes
    MODE_303S2,     // live 303 recorder: last 13 played notes loop at BPM
    MODE_POKEMON,   // theremin-like pokemon mode: 25 pokemon, each with AMY synthesis timbre
    MODE_MODULAR,   // 6-encoder modular: OSC/filter/env/LFO + joystick velocity
    MODE_GEST,      // sequencer manager: 4×8 pattern grid, volume pots, copy/paste, LOOP/LIVE
    MODE_PCMCLEAN,  // SD cache cleaner: recursively delete all .pcm/.pcm16 cache files
    MODE_USB,       // USB Mass Storage: exposes the SD card as a normal drive on the PC
    MODE_STONE,     // sample tone: one SD sample pitched across the keyboard; JY=browse P2=cycle folder
    MODE_DR2,       // hierarchical drum sequencer: 64 steps addressed as beat.step.micro (4.4.4)
    MODE_IMPORT,    // Android-only: SAF folder picker, imports phone files onto the SD root
    MODE_LIFE,      // Conway's-Game-of-Life on the key grid: column=pitch, birth=note-on
    MODE_SWARM,     // boids flocking: joystick-steered attractor zone triggers notes
    MODE_GEN,       // generative: joystick X=procedural texture, Y=sound-making method, independently tunable
    MODE_DJ,        // DJ/remix deck: one big SD track (mp3/wav), scrub/speed/reverse + shared FX
    MODE_GROOVE,    // unified step sequencer: 8 drum pads + synth + 303 on the full 4x8 grid
    MODE_NOOB,      // generative melody: density/rhythm/shape/variation pots, keys pick scale
    MODE_EUCLI,     // euclidean drum sequencer: 4 lanes, per-lane step count + pulse count, concentric-ring display
    MODE_CRUNCH,    // CrunchE-inspired sample instrument: the 32-sample drum bank replayed pitched, across the keyboard
    MODE_COUNT
};

// ==================== MENU ====================
#define MENU_COLS 3
enum MenuItem : uint8_t {
    MENU_SYNTH, MENU_OMNI, MENU_SAMPLE,
    MENU_LIGHT, MENU_LIGHTPLAY, MENU_SD,
    MENU_ABOUT, MENU_MOD2,
    MENU_GRANULAR2, MENU_MIDI, MENU_TRACKER,
    MENU_DRUM2, MENU_SYSEQ, MENU_303S,
    MENU_SS2, MENU_ANIM, MENU_I303,
    MENU_VID,
    MENU_LANIM,
    MENU_EXP,
    MENU_EXP2,
    MENU_EXP3,
    MENU_303S2,
    MENU_POKEMON,
    MENU_MODULAR,
    MENU_GEST,
    MENU_PCMCLEAN,
    MENU_USB,
    MENU_STONE,
    MENU_DR2,
    MENU_IMPORT,
    MENU_LIFE,
    MENU_SWARM,
    MENU_GEN,
    MENU_DJ,
    MENU_GROOVE,
    MENU_NOOB,
    MENU_EUCLI,
    MENU_CRUNCH,
    MENU_ITEM_COUNT
};
static const char* menuLabels[] = {
    "SYNTH","OMNI","SAMPL",
    "LIGHT","LPLY","DIAG",
    "BATT","MOD2",
    "GRANU","MIDI","TRKR",
    "DRUMS","SYNS","303S",
    "SAMPS","ANIM","I303",
    "MEDIA","LANIM","EXP",
    "EXP2","EXP3","303S",
    "PKMN","SERUM","GEST",
    "PURGPCM","USB","STONE",
    "GEST2",
    "IMPORT","LIFE","SWRM",
    "GEN","DJ","GROOVE",
    "NOOB","EUCLI","CRUNCH"
};
#define MENU_ROWS ((MENU_ITEM_COUNT + MENU_COLS - 1) / MENU_COLS)

// ==================== SYNTH SHAPES ====================
enum SynthShape : uint8_t {
    SHAPE_SAW = 0, SHAPE_SAW_FM, SHAPE_SQUARE, SHAPE_SINE, SHAPE_SUPERSAW,
    SHAPE_ACID, SHAPE_BASS, SHAPE_PLUCK,
    SHAPE_NOISE_WHITE, SHAPE_NOISE_PINK, SHAPE_NOISE_BROWN,
    SHAPE_JUNO_BRASS, SHAPE_JUNO_STRINGS, SHAPE_JUNO_PIANO,
    SHAPE_JUNO_ORGAN, SHAPE_JUNO_CHOIR,
    SHAPE_DX7_EP, SHAPE_DX7_BELLS, SHAPE_DX7_BASS,
    SHAPE_DX7_BRASS, SHAPE_DX7_STRINGS, SHAPE_DX7_ORGAN, SHAPE_DX7_VOICE,
    SHAPE_TECHNO_LEAD,   // detuned supersaw + filter envelope sweep
    SHAPE_RAVE_BASS,     // deep sub + filter pump
    SHAPE_HOOVER,        // classic rave hoover (detuned + pitch/filter rise)
    SHAPE_TECHNO_STAB,   // short staccato stab with sharp filter
    SHAPE_ACID_WOBBLE,   // wobble bass: detuned SAW, slow filter wob via EG1
    SHAPE_ELECTRO_PLUCK, // electro pluck: square wave, filter slams shut on trigger
    SHAPE_INDUSTRIAL,    // dark industrial drone: heavy SAWs, resonant burst + long sustain
    // ---- Evolving/saturation family (inspired by J:ORG FM modulator decay mechanism) ----
    SHAPE_JUNO_ORGAN2,   // Juno A22 Organ II — FM ratio 1.932, 10s modulator decay, slightly darker
    SHAPE_JUNO_FRONTIER, // Juno A63 Frontier Organ — FM ratio 5.263, instant EG1 sustain, brighter
    SHAPE_FM_DRIFT,      // Custom ALGO: 10s FM index decay + resonant filter → slow timbral darkening
    SHAPE_FM_BELL,       // Custom ALGO bell: 5s FM decay, inharmonic partials, long ring
    SHAPE_SAT_DRIFT,     // Supersaw + Q=6.5 + EG1 sweeps filter 8s → builds saturation on held notes
    SHAPE_COUNT
};

static const char* shapeNames[] = {
    "SAW","SAWFM","SQR","SIN","SSAW","ACID","BASS","PLCK",
    "WHT","PINK","BRWN",
    "J:BRS","J:STR","J:PNO","J:ORG","J:CHR",
    "D:EP","D:BEL","D:BAS","D:BRS","D:STR","D:ORG","D:VOC",
    "T:LED","T:BAS","HOVR","STAB",
    "WOBB","EPLK","INDS",
    "J:OR2","J:FRG","FMDFT","FMBEL","SDFT"
};

// Patch numbers for preset shapes (-1 = custom wave, -2 = ALGO FM)
static const int16_t shapePatch[] = {
    -1,-2,-1,-1,-1,-1,-1,-1,   // custom waves (SAW, SAW_FM, SQR, SIN, SSAW, ACID, BASS, PLCK)
    -1,-1,-1,                   // noise: WHITE, PINK, BROWN
    0,21,7,8,6,                 // Juno patches
    138,153,142,128,131,144,157,// DX7 patches (128+offset)
    -1,-1,-1,-1,                // Techno: TECHNO_LEAD, RAVE_BASS, HOOVER, TECHNO_STAB
    -1,-1,-1,                   // Acid/industrial: ACID_WOBBLE, ELECTRO_PLUCK, INDUSTRIAL
    9,42,                       // Juno evolution presets: Organ II (ratio 1.932), Frontier (ratio 5.263)
    -2,-2,-2                    // Custom ALGO: FM_DRIFT, FM_BELL, SAT_DRIFT
};

// Modular synth: limited shape palette for Pot2 selection
enum ModShape : uint8_t {
    MOD_SAW=0, MOD_SQR, MOD_SIN, MOD_SSAW, MOD_KS, MOD_SHAPE_COUNT
};
static const SynthShape modShapeMap[] = {
    SHAPE_SAW, SHAPE_SQUARE, SHAPE_SINE, SHAPE_SUPERSAW, SHAPE_PLUCK
};
static const char* modShapeNames[] = {"SAW","SQR","SIN","SSAW","KS"};

// ==================== ENVELOPES ====================
enum EnvPreset : uint8_t {
    ENV_NORMAL=0, ENV_FAST, ENV_PLUCK, ENV_PAD, ENV_PIANO,
    ENV_PRESET_COUNT
};

static const char* envNames[] = {"Nrm","Fst","Plk","Pad","Pia"};

struct EnvParams { uint16_t atk, dec, rel; float sus; };
static const EnvParams envTable[] = {
    {50, 200, 500, 0.5f},    // Normal
    {1,  50,  50,  0.0f},    // Fast
    {5,  100, 50,  0.0f},    // Pluck
    {500,100, 2000,0.8f},    // Pad
    {10, 500, 800, 0.3f},    // Piano
};

// ==================== SCALES ====================
// Defined in NoteMap.h

// ==================== EFFECTS ====================
enum FxSlot : uint8_t { FX_LPF=0, FX_DRIVE, FX_DELAY, FX_REVERB, FX_SLOT_COUNT };
static const char* fxNames[] = {"LPF","DRIVE","DELAY","REVERB"};

// ==================== DRUMS ====================
#define DRUM_ROWS 4
#define DRUM_MAX_STEPS 16
#define AMY_OSC_DRUM_BASE 200

// ==================== GROOVE (MODE_GROOVE) ====================
// Unified step sequencer: DRUM2's 8 pads + one monophonic synth track + one
// monophonic 303 track, all on the same full 4x8=32-key grid (one track's pattern
// shown/edited at a time, cycled via joystick X). Per-step storage reuses the same
// field shapes DR2/SYSEQ/303S/SS2 already use (grvOn: 0=off else velocity/note+1;
// grvAlt: 0=normal, else track-type-specific articulation) rather than inventing a
// new format — see main.cpp's GROOVE state block.
#define GRV_MAX_STEPS 32
#define GRV_PATS      4
enum {
    GRV_TRK_DRUM0 = 0, GRV_TRK_DRUM1, GRV_TRK_DRUM2, GRV_TRK_DRUM3,
    GRV_TRK_DRUM4, GRV_TRK_DRUM5, GRV_TRK_DRUM6, GRV_TRK_DRUM7,
    GRV_TRK_SYNTH, GRV_TRK_303,
    GRV_TRACKS
};

// ==================== TRACKER (removed — stub only) ====================
// ==================== AUDIO ====================
#define SYNTH_CH 1
#define T303_CH  2
#define SW2_CH_BASE  10   // AMY synth channels 10-13 for SW2 ±1/±2 semitone layers
#define SW2_VOICES    4   // polyphony per SW2 sub-channel
#define MOD3_OSCA_CH 14   // modular synth (MODE_MODULAR) — osc A, dedicated dynamic channel
#define MOD3_OSCB_CH 15   // modular synth — osc B (detuned via per-note pitch_bend)
#define NUM_SYNTH_VOICES 8
#define OSCS_PER_VOICE 2
#define AMY_OSC_STRUM 50
#define PCM_PREVIEW_PRESET  100  // AMY preset slot for sample browser preview
#define PCM_PREVIEW_OSC      60  // AMY oscillator for preview playback
#define DRUM_PRESET_BASE    101  // AMY presets 101-132 for drum pads (32 slots)
#define DRUM_OSC_BASE        70  // AMY oscillators 70-101 for drum pad playback
#define DRUM_PAD_COUNT       32  // 32 unique drum samples (one per key)
#define DRUM_SAMPLERATE    8000  // 16kHz source / 2 for hardware 2x compensation
#define SAMPLE_PRESET_BASE  200  // AMY presets 200-231 for key-assigned samples
#define SAMPLE_OSC_BASE     182  // AMY oscillators 182-213 for key sample playback (above SYNTH_CH range 125-148 and GRANULAR 150-181)
#define SAMPLE_KEY_COUNT     32  // 4×8 keys, each can hold one RAM-loaded sample

// ==================== CRUNCH (MODE_CRUNCH) ====================
// Ported instrument bank inspired by the sibling CrunchE_GroovePadBox project. Rather than
// converting/duplicating CrunchE's own sample ROM, CRUNCH aliases GrvEP's EXISTING 32-slot
// drum-pad sample bank (kDrumPads[]/audioLoadDrumSamples(), audio_engine.cpp) — confirmed
// (src/sounds/README.md + directory listing) to be the same sample set CrunchE itself ships
// (kick/snare/hihat/clap/crash/ride/bongo/sfx/bass/guitar/synth/pad). CRUNCH registers a
// SECOND set of AMY presets pointing at those already-loaded PSRAM buffers (via
// pcm_get_sample_ram_for_preset(DRUM_PRESET_BASE+i)+pcm_register_extern16 — see
// audioLoadCrunchSamples()/kCrunchSamples[] in audio_engine.cpp), with CRUNCH-specific loop
// points (5 melodic samples loop, the other 27 are one-shot) and real per-note pitch
// (audioStoneNoteOn()'s round-robin-voice pattern, not amyPlayPcm()'s fixed-pitch-69). Zero
// new sample data, zero flash/PSRAM cost beyond the preset table itself. Presets sit in the
// genuinely-free 133-199 gap between DRUM_PRESET_BASE's end (132) and SAMPLE_PRESET_BASE's
// start (200). Oscillators sit right after SAMPLE_OSC_BASE's own range (182-213) —
// AMY_OSC_DRUM_BASE (200, above) looks like it overlaps that gap on paper, but its only
// caller (main.cpp's triggerDrumSound()) is dead code (confirmed: zero call sites) — 214-239
// has no LIVE user before STONE_OSC_BASE (240).
#define CRUNCH_PRESET_BASE  133  // AMY presets 133-164 (32 slots, one per kDrumPads[] entry)
#define CRUNCH_SAMPLE_COUNT  32
// 5 real MothOS instrument samples (instrument1/2/3/7/10 from the user's own
// MothOS_GroovePadBox fork, converted int32->int16_t), vendored directly (NOT aliased —
// this audio content doesn't exist anywhere else in GrvEP) to fill out CRUNCH's melodic
// instrument slots with genuine distinct timbres instead of repurposed drum one-shots.
// ~194KB total flash — picked the 5 smallest of the original's 11 for safety margin.
#define CRUNCH_INSTR_PRESET_BASE  165  // AMY presets 165-169 (133-199 is the free gap, see above)
#define CRUNCH_INSTR_SAMPLE_COUNT  5
// CRUNCH is a 4-track live-record tracker (MothOS-style — see structure/ plan history),
// not a single played instrument: one fixed, monophonic oscillator per track (no
// round-robin voice stealing needed, mirrors MothOS's own Voice[4] model).
#define CRUNCH_OSC_BASE     214  // AMY oscillators 214-217
#define CRUNCH_TRACKS         4
#define CRUNCH_PATTERNS       4
#define CRUNCH_STEPS         32
// Instrument SLOTS (matches MothOS's own 'I' command range, 0-11): slot 0 = "DRUM" bank
// (12 fixed drum hits — the 12 note keys select WHICH drum, at native pitch, not a pitch
// shift of one sample), slot 1 = "SFX" bank (same idea, 12 fixed sfx hits), slots 2-11 =
// 10 melodic instruments (the 12 note keys pitch the SAME sample chromatically). This is
// the actual original structure (see Voice::ReadDrumWaveform/ReadSfxWaveform vs.
// ReadWaveform in MothOS's own source) — the first CRUNCH build collapsed this into "one
// pitched sample per track" for all 12 slots, which was wrong.
#define CRUNCH_INSTR_SLOTS   12

// ==================== STONE (sample tone) ====================
// One SD sample, pitched across the keyboard like a normal synth voice: AMY reads the
// PCM preset's midinote (always 69, native rate) vs the event's midi_note to compute
// the playback speed ratio, exactly like a classic sampler root-key mapping.
// Uses fixed, directly-addressed oscillators (own round-robin polyphony) rather than an
// AMY multi-voice channel (e.synth=N + num_voices): the dynamic voice allocator pulls from
// a shared oscillator pool that isn't aware of the fixed ranges other subsystems address
// directly (GRANULAR_OSC_BASE, SAMPLE_OSC_BASE, DRUM_OSC_BASE, AMY_OSC_DRUM_BASE) — with
// enough channels already competing for that pool, a new channel can land on and corrupt
// one of those fixed oscillators. Fixed osc addressing (this pattern, same as SAMPLE/
// GRANULAR/DRUM) sidesteps that collision entirely.
#define STONE_PRESET     361   // AMY preset slot actually played from; re-windowed in place via
                                // pcm_register_extern16 to point into STONE_SOURCE_PRESET's buffer
                                // (start/end window editing, main.cpp audioStoneApplyWindow() calls).
#define STONE_VOICES       6   // polyphony (round-robin across fixed oscillators)
#define STONE_OSC_BASE   240   // AMY oscillators 240-245 (250 max_oscs; clear of all other fixed ranges)
#define STONE_SOURCE_PRESET 362  // pristine 16-bit full-length buffer loaded from disk (pointer/length
                                 // database only, never played directly) — mirrors GRAN2_SOURCE_BASE.

// ==================== DJ (MODE_DJ) ====================
// One mono "deck" for a DJ/remix track, streamed in bounded RAM from the SD card as a TRUE
// streaming circular buffer (s_djRing[DJ_RING_FRAMES] in audio_engine.cpp) — registered ONCE
// as ONE preset (DJ_PRESET) with loopstart=0/loopend=DJ_RING_FRAMES-1/feedback=1 and
// triggered ONLY on a real reposition (load/seek/resume/true EOF-SOF wrap/scratch-release/
// stutter-release — AMY's own native PCM looping, lib/AMY Synthesizer/src/pcm.c's
// render_pcm()). AMY then reads the ring forward, physically, FOREVER after that — a
// background task's only job is to keep writing the next DJ_STREAM_GRAIN_SEC of audio (in
// whichever direction is currently selected) into the ring just ahead of wherever AMY is
// currently reading. This is a generalization of the "silent handoff" this design has always
// relied on for ordinary chunk-boundary crossings — applied continuously in small grains
// instead of only at large fixed boundaries — and it's what makes a reverse-direction toggle
// need NO retrigger at all: it's just "the writer stops adding forward content and starts
// adding reverse content from here," nothing about how AMY is being read ever changes.
//
// DJ_STREAM_GRAIN_SEC only needs to stay well inside DJ_STREAM_LOW_WATERMARK_SEC's margin
// (the "how far ahead to stay buffered" target the ONGOING top-up loop maintains) — standard
// multi-buffered streaming. It's a free tuning knob for ordinary playback efficiency — a real,
// measured mistake early on: an initial 0.1s grain spawns a background FreeRTOS task (its own
// real stack-alloc/scheduling cost, unlike the desktop simulator's cheap pthread-backed one)
// roughly every 0.1-0.35s for the ENTIRE duration of playback, versus the old two-half
// design's much rarer ~every-2s spawn — reported as "un peu saccadée" on real hardware. Sized
// up ~5x here to cut that spawn frequency by the same factor. The one place its SIZE does
// matter is immediately after a reposition — see DJ_STREAM_QUICK_GRAIN_SEC below, which
// exists specifically so this larger size doesn't also make reverse-toggle latency worse.
#define DJ_OSC            246  // single fixed oscillator (mono deck; 250 max_oscs ceiling)
#define DJ_RING_SECONDS   6
#define DJ_RING_FRAMES    (DJ_RING_SECONDS * 20000u)  // DJ_PLAYBACK_RATE_HZ in audio_engine.cpp
#define DJ_PRESET         363  // one preset for the whole DJ_RING_FRAMES ring — see above
#define DJ_STREAM_GRAIN_SEC          0.5f   // decode/write granularity for the ONGOING top-up loop
// Untouched zone right ahead of the read cursor on a reverse toggle/reposition — audio there
// may be microseconds from being read by the render callback, so only content BEYOND this
// margin is safe to overwrite with new content. This is the one honest, minimal, unavoidable
// latency floor before a toggle CAN take effect — a hardware-timing safety requirement, not a
// design compromise. What actually determines how long it takes before the effect is
// AUDIBLE, though, is how long the FIRST grain written past this margin takes to decode — see
// DJ_STREAM_QUICK_GRAIN_SEC below for why that must be small, not DJ_STREAM_GRAIN_SEC's size.
#define DJ_STREAM_SAFETY_MARGIN_SEC  0.03f
// The very first grain written after ANY reposition (reverse toggle, seek, resume, scratch/
// stutter release) is this small, not DJ_STREAM_GRAIN_SEC — a real, measured bug: with the
// ongoing loop's larger grain, the content already sitting further ahead in the ring from
// BEFORE the reposition (not yet overwritten) stays audible for however long that first
// larger grain takes to decode, which easily exceeds DJ_STREAM_SAFETY_MARGIN_SEC on real SD/
// mp3 decode — reported as a perceptible delay between pressing reverse and it actually
// taking effect (both ways), "comme si le buffer gardait en mémoire l'inversion du sample".
// A small quick grain decodes fast enough to land within that margin far more reliably; the
// ordinary top-up loop (djStreamTopUp(), next tick) takes over with full-size grains once it
// lands — this only affects the ONE grain immediately following a reposition.
#define DJ_STREAM_QUICK_GRAIN_SEC    0.05f
#define DJ_STREAM_LOW_WATERMARK_SEC  2.0f   // top-up fires once buffered-ahead margin drops below this

// ---- Dynamic/experimental performance controls (scratch, granular spray, stutter) ----
// Joystick X = scratch, joystick Y = granular spray amount, joystick click (hold) = stutter/
// glitch — all always-live in the DJ play view (inert at rest), see audioDJScratchNudge()/
// audioDJSetGrainAmount()/audioDJStutterStart()/audioDJStutterEnd() in audio_engine.cpp. Both
// scratch and stutter deliberately retrigger on every nudge/hold (unlike ordinary playback
// and reverse-toggle) — they're glitch effects, not seamless controls — reading their working
// window straight out of the live ring (handling wraparound via s_djWindowPad) instead of a
// dedicated snapshot buffer, since the ring already holds audio in correct playback order
// everywhere (see audioDJSetReverse()'s own comment for why that's always true).
#define DJ_SCRATCH_DEADZONE       0.12f  // joystick X magnitude below this = no scratch
#define DJ_SCRATCH_MAX_STEP_SEC   0.12f  // seconds moved per 10ms tick at full deflection
#define DJ_SCRATCH_WINDOW_SEC     0.15f  // registered/retriggered window per nudge
#define DJ_GRAIN_OSC_BASE         248    // 248,249 — the only 2 free oscillators below the 250 max_oscs ceiling (STONE=240-245, DJ_OSC=246, SS_OSC=247)
#define DJ_GRAIN_COUNT            2      // round-robin pool size — capped by the 2 free oscillators above
#define DJ_GRAIN_PRESET_BASE      367    // 367,368 — free (364 unused/skipped, 365 = SS_PRESET, 366 now free too)
#define DJ_GRAIN_LEN_MS_MIN       40
#define DJ_GRAIN_LEN_MS_MAX       120
#define DJ_GRAIN_PITCH_SCATTER    3.0f   // +/- semitones
#define DJ_GRAIN_GAP_MS_MAX       400.0f // inter-grain gap at amount=0 (just above the "no grains" threshold)
#define DJ_GRAIN_GAP_MS_MIN       40.0f  // inter-grain gap at amount=1 (full deflection)
#define DJ_STUTTER_MIN_SEC        0.03f
#define DJ_STUTTER_MAX_SEC        0.5f
#define DJ_STUTTER_MIN_FRAMES     32     // floor so a stutter window is never degenerate/near-zero

// ==================== SS2 large-sample streaming ====================
// SS2 slots (and, sharing the same underlying key storage, MODE_SAMPLE keys) normally
// decode a whole file into one PSRAM buffer capped by psramMaxFrames() (free-PSRAM-
// dependent, audio_engine.cpp) — fine for short one-shots, but silently truncates a long
// file to however many frames happened to fit at that moment. Files too long for one
// SS2_CHUNK_SECONDS window instead stream through this single shared mono voice, using the
// SAME true-streaming-circular-buffer design as DJ_OSC (see the DJ block above for the full
// rationale) — one small ring, one write cursor continuously topped up in the background,
// AMY reading it forward forever after a single initial retrigger. SS2_CHUNK_SECONDS is
// still what decides large-vs-small in audioLoadKey() (unrelated to ring sizing now — the
// ring only needs enough margin to stay ahead of real-time playback, not the whole file, see
// SS2_RING_SECONDS below). Only ONE large sample streams at a time (triggering any large slot
// cuts whatever this voice was doing); small samples are unaffected and keep full polyphony
// via their own oscillators. Unlike DJ, a streamed key is a one-shot: it plays once and stops
// at the true end/start of the file, never loops, and never repositions after being
// triggered (no seek/scratch/stutter) — the DJ constants below are reused as-is since the
// underlying decode/timing characteristics are identical, no SS-specific duplicates needed.
#define SS_OSC            247  // single fixed oscillator (250 max_oscs ceiling; DJ_OSC=246)
#define SS2_CHUNK_SECONDS  30
#define SS2_RING_SECONDS   4   // matches DJ_RING_SECONDS — margin-to-stay-ahead, not file length
#define SS2_RING_FRAMES   (SS2_RING_SECONDS * 20000u)
#define SS_PRESET         365  // one preset for the whole ring (was SS_PRESET_A/B, two 30s buffers)

// ==================== GRANULAR ====================
// Row layout (8-slice mode): R0=one-shot px, R1=px+px+1, R2=sx→end, R3=px reversed
// GRANULAR_SOURCE_PRESET: raw decoded sample (waveform display)
// GRANULAR_PRESET_BASE  : one-shot forward slices 233-248 (16 slots: mode0→8, mode1→16)
// GRANULAR_REV_PRESET   : reverse slices 249-256 (8 slots, mode0 row3)
// GRANULAR_DBL_PRESET   : double slices 257-264 (px+px+1, mode0 row1)
// GRANULAR_TAIL_PRESET  : tail slices 265-272 (sx→end, mode0 row2)
// OSCs 150-181 (32 total, one per key) play granular slices independently.
// Base moved from 142 to 150: SYNTH_CH with 8 voices × 6 oscs (Juno/DX7 patches) occupies
// even voices 125-148, which was corrupting oscs 142-148 as SYNTH_IS_MOD_SOURCE.
#define GRANULAR_SOURCE_PRESET  232
#define GRANULAR_PRESET_BASE    233  // forward slices 233-248 (16 slots)
#define GRANULAR_REV_PRESET     249  // reverse slices 249-256 (8 slots)
#define GRANULAR_DBL_PRESET     257  // double slices 257-264 (8 slots)
#define GRANULAR_TAIL_PRESET    265  // tail slices 265-272 (8 slots)
#define GRANULAR_OSC_BASE       150  // oscs 150-181 (was 142; moved above SYNTH_CH range 125-148)
#define GRANULAR_MAX_SLICES      16

// ==================== GRANULAR2 ====================
// Multi-sample granular: forward + reverse only, 4 or 8 slices, 4 or 2 samples.
// sliceMode 0 = x4 (4 samples in 4 quadrants); sliceMode 1 = x8 (2 samples, top/bottom half).
// Layout sliceMode 1 (x8, 2 samples):
//   row 0: sample 0 fwd  |  row 1: sample 0 rev
//   row 2: sample 1 fwd  |  row 3: sample 1 rev
// Layout sliceMode 0 (x4, 4 samples):
//   row 0-1 left (cols 0-3): sample 0 fwd/rev
//   row 0-1 right (cols 4-7): sample 1 fwd/rev
//   row 2-3 left (cols 0-3): sample 2 fwd/rev
//   row 2-3 right (cols 4-7): sample 3 fwd/rev
#define GRAN2_MAX_SAMPLES    4
#define GRAN2_MAX_SLICES     8
#define GRAN2_SOURCE_BASE  273   // AMY presets 273-276 (4 source buffers, 16-bit PSRAM)
#define GRAN2_FWD_BASE     277   // AMY presets 277-308 (4 samples × 8 slices, fwd)
#define GRAN2_REV_BASE     309   // AMY presets 309-340 (4 samples × 8 slices, rev)
#define GRAN2_TAIL_BASE    341   // AMY presets 341-344 (1 per sample): full-reversed buffer for FUL-reverse mode
#define SAMPLE_REV_PRESET_BASE 345  // AMY presets 345-360: reversed copies of SS2 slots (keyIdx 0-15)
// OSCs: reuse GRANULAR_OSC_BASE (150-181), modes are mutually exclusive

// ==================== TRACKER ====================
#define TRACKER_STEPS    32   // 32 × 16th note steps (2 bars at 4/4)
#define TRACKER_TRACKS   16   // 7 synth + 1 drum + 8 sample
#define TRACKER_SYNTHS    7
#define TRACKER_DRUM_TRK  7   // track index for drums
#define TRACKER_SAMP_BASE 8   // sample tracks 8-15
#define TRK_CHORD_SIZE    3   // max simultaneous notes per step (tonic+third+fifth)
#define TRACKER_SYNTH_CH_BASE 3  // AMY synth channels 3-9, one per tracker synth track

// ==================== BUTTON LABELS ====================
// SEQ key base: use the last 8 slots of the SAMPLE key space (keyIdx 24-31) for 8 sequencer tracks
#define SEQ_KEY_BASE 24

// ==================== SS2 ====================
// SS2 reuses the first 16 SAMPLE key slots (keyIdx 0-15, presets 200-215, oscs 182-197).
// SS2 and SAMPLE mode cannot both have samples loaded simultaneously,
// but since they're separate modes, samples persist until the user reloads in the other mode.
#define SS2_SLOTS    16
#define SS2_KEY_BASE  0   // keyIdx 0-15

// ==================== MOD2 — Modular Synthesizer Algorithms ====================
// Each algorithm is a distinct synthesis topology with 4 exposed patch parameters (P4-P7).
// P2 = algo selection (discrete steps), B3 = visual algo browser overlay.

struct Mod2ParamDef {
    const char* name;   // ≤4 chars for OLED display
    float       mn;     // minimum real value
    float       mx;     // maximum real value
    float       dflt;   // default (normalized 0-1)
};

struct Mod2AlgoDef {
    const char*    name;   // ≤4 chars
    const char*    desc;   // short description (≤20 chars)
    SynthShape     shape;  // base AMY synthesis shape
    Mod2ParamDef   p[4];   // P4, P5, P6, P7 descriptors
};

static const Mod2AlgoDef kMod2Algos[] = {
    //  name   desc                shape           P4                   P5                   P6                   P7
    { "VCO ", "SAW+filter+drive", SHAPE_SAW,
      {{"Cut",80,8000,0.50f}, {"Res",0.5f,12,0.08f}, {"Drv",1.0f,4.0f,0.0f}, {"Dcy",10,2000,0.25f}} },

    { "DUO ", "Dual SAW+reverb",  SHAPE_SUPERSAW,
      {{"Cut",80,8000,0.50f}, {"Res",0.5f,8,0.08f},  {"Sat",1.0f,4.0f,0.0f}, {"Rvb",0,1,0.0f}} },

    { "FM2 ", "FM algo+reverb+chorus", SHAPE_SAW_FM,
      {{"Dpt",0,7,1.4f},      {"Rvb",0,1,0.15f},     {"Chr",0,1,0.0f},      {"Drv",1.0f,3.0f,0.0f}} },

    { "ACID", "303 acid resonance",SHAPE_SAW,
      {{"Cut",80,2000,0.30f}, {"Res",2.0f,16,0.30f}, {"Dcy",20,800,0.35f},  {"Drv",1.0f,3.0f,0.0f}} },

    { "PAD ", "Supersaw+chorus",   SHAPE_SUPERSAW,
      {{"Cut",200,8000,0.70f},{"Chr",0,1,0.30f},     {"Rvb",0,1,0.40f},     {"Sat",0,2.0f,0.0f}} },

    { "PLCK", "Pluck+brightness",  SHAPE_PLUCK,
      {{"Brg",500,8000,0.65f},{"Res",0.5f,4,0.08f}, {"Drv",1.0f,3.0f,0.0f},{"Dcy",20,500,0.20f}} },

    { "LEAD", "Hoover+hard drive", SHAPE_HOOVER,
      {{"Drv",1.0f,8.0f,0.25f},{"Cut",200,8000,0.50f},{"Res",0.5f,8,0.10f},{"Dpt",0,1,0.20f}} },
};
#define MOD2_ALGO_COUNT  7

// MOD2 LFO combined mode: destination × waveform shape
// B4 cycles 7 modes: Off, Pitch×3 shapes, Filter×3 shapes
#define MOD2_LFO_MODE_COUNT 13
// dest: 0=None 1=Pitch 2=Filter
static const uint8_t mod2LfoModeDest[]  = {0, 1,1,1,1,1,1, 2,2,2,2,2,2};
// shape: 0=Sine 1=Tri 2=Saw 3=RevSaw 4=Square 5=S&H
static const uint8_t mod2LfoModeShape[] = {0, 0,1,2,3,4,5, 0,1,2,3,4,5};
static const char* mod2LfoModeNames[]   = {
    "Off",
    "Pt:SIN","Pt:TRI","Pt:SAW","Pt:RSW","Pt:SQR","Pt:S&H",
    "Ft:SIN","Ft:TRI","Ft:SAW","Ft:RSW","Ft:SQR","Ft:S&H"
};

// MOD2 play mode
enum Mod2PlayMode : uint8_t { MOD2_POLY=0, MOD2_MONO, MOD2_SLIDE, MOD2_PLAY_COUNT };
static const char* mod2PlayModeNames[] = {"Poly","Mono","Slid"};

static const char* btnLabels[][4] = {
    {"FX","Scl/Arp","Env","Instr"},  // SYNTH      (0)
    {"Shape","Mix","Sus","Oct"},     // OMNI       (1)
    {"Patt","Play","Clr","Bank"},    // SAMPLE     (2)
    {"Spd","Sprd","Brt","Sat"},      // LIGHT      (3)
    {"","","",""},                   // LIGHTPLAY  (4)
    {"","","",""},                   // BATTERY    (5)
    {"","","",""},                   // SYSINFO    (6)
    {"FX","Env","Algo","Md"},        // MOD2       (7)
    {"","","",""},                   // GRANULAR2  (8)
    {"","","",""},                   // MIDI       (9)
    {"","","",""},                   // TRACKER    (10)
    {"FX","Ply","Rec","Seq"},        // DRUM2      (11)
    {"FX","Ply","Env","Seq"},        // SYSEQ      (12)
    {"FX","Ply","Opt","Seq"},        // 303S       (13)
    {"Bck","FX","Map","Seq"},        // SS2        (14)
    {"","","",""},                   // ANIM       (15)
    {"FX","Wv","Sld","Oct"},         // I303       (16)
    {"","","",""},                   // VID        (17)
    {"","","",""},                   // LANIM      (18)
    {"","","",""},                   // EXP        (19)
    {"","","",""},                   // EXP2       (20)
    {"","","",""},                   // EXP3       (21)
    {"","","",""},                   // 303S2      (22)
    {"","","",""},                   // POKEMON    (23)
    {"OSC","Env","Flt","Oct"},       // MODULAR    (24)
};
