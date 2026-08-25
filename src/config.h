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
    MODE_HYBRID,    // synth note + assigned sample triggered simultaneously
    MODE_MOD2,      // PolyAnalog-inspired: waveform morph, power-law filter, LFO dest toggle
    MODE_GRANULAR2, // granular2: multi-sample (2 or 4), fwd+rev only, per-sample split control
    MODE_MIDI,      // USB MIDI device: keyboard → NoteOn/Off, host → LED feedback (LaunchPad)
    MODE_TRACKER,   // 32-step quantized recorder: left 4×4 = instruments, right 4×4 = notes
    MODE_DRUM2,     // TR-808 style: 8 pads, per-pad pitch/decay/volume, sequencer, FX
    MODE_SYSEQ,     // 16-step polyphonic synth sequencer (up to 4 notes/step)
    MODE_303S,      // 16-step TB-303 step sequencer (note + accent + slide per step)
    MODE_SS2,       // 16-step sample sequencer: 16 slots, per-step alteration, shared clock
    MODE_ANIM,      // visual animations: wave / bars / techno / acid / 8bit
    MODE_I303,      // polyphonic TB-303: 6-voice chord/melody with full 303S sound engine
    MODE_COUNT
};

// ==================== MENU ====================
#define MENU_COLS 3
enum MenuItem : uint8_t {
    MENU_SYNTH, MENU_OMNI, MENU_SAMPLE,
    MENU_LIGHT, MENU_LIGHTPLAY, MENU_SD,
    MENU_ABOUT, MENU_HYBRID, MENU_MOD2,
    MENU_GRANULAR2, MENU_MIDI, MENU_TRACKER,
    MENU_DRUM2, MENU_SYSEQ, MENU_303S,
    MENU_SS2, MENU_ANIM, MENU_I303,
    MENU_ITEM_COUNT
};
static const char* menuLabels[] = {
    "SYNTH","OMNI","SAMPL",
    "LIGHT","LPLY","DIAG",
    "BATT","HYBRD","MODUL",
    "GRANU","MIDI","TRKR",
    "DRUMS","SYNS","303S",
    "SAMPS","ANIM","I303"
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

// ==================== TRACKER (removed — stub only) ====================
// ==================== AUDIO ====================
#define SYNTH_CH 1
#define T303_CH  2
#define SW2_CH_BASE  10   // AMY synth channels 10-13 for SW2 ±1/±2 semitone layers
#define SW2_VOICES    4   // polyphony per SW2 sub-channel
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

// MOD2 waveform morph: 3 zones across encoder range
static const SynthShape mod2ShapeSteps[] = {
    SHAPE_SAW, SHAPE_SUPERSAW, SHAPE_SQUARE, SHAPE_SINE,
    SHAPE_ACID, SHAPE_BASS, SHAPE_PLUCK, SHAPE_HOOVER
};
static const char* mod2ShapeStepNames[] = {"SAW","SSAW","SQR","SIN","ACID","BASS","PLCK","HOVR"};
#define MOD2_SHAPE_COUNT 8

// MOD2 LFO combined mode: destination × waveform shape
// Btn1 cycles all 13 modes: Off, then Pitch×6 shapes, then Filter×6 shapes
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
    {"FX","Scl/Arp","Env","Instr"},  // SYNTH
    {"Shape","Mix","Sus","Oct"},     // OMNI
    {"Patt","Play","Clr","Bank"},    // SAMPLE
    {"Spd","Sprd","Brt","Sat"},      // LIGHT
    {"","","",""},                   // LIGHTPLAY
    {"","","",""},                   // BATTERY
    {"","","",""},                   // SYSINFO
    {"Scale","Env","Mix","Oct"},     // HYBRID
    {"OSC","Env","Flt","Oct"},       // MODULAR
    {"Ptch-","Ptch+","","Oct"},      // MOD2
    {"LFO","Mode","Oct",""},         // 303
    {"FX","Wv/Arp","Sus","Tone"},    // GRANULAR
    {"","","",""},                   // GRANULAR2
    {"","","",""},                   // MIDI
    {"","","",""},                   // TRACKER
    {"FX","Ply","Rec","Seq"},        // DRUM2
    {"FX","Ply","Env","Seq"},        // SYSEQ (SYNS)
    {"FX","Ply","Opt","Seq"},        // 303S
    {"Bck","FX","Map","Seq"},        // SS2 (SAMPS)
    {"","","",""},                   // ANIM
    {"FX","Wv","Sld","Oct"},         // I303
};
