#include <Arduino.h>
#include <math.h>
#include <SD.h>
#include <SPI.h>
#include <AMY-Arduino.h>
#include "HWConfig.h"
#include "mux.h"
#include "keyboard.h"
#include "Leds.h"
#include "oled.h"
#include "joystick.h"
#include "NoteMap.h"
#include "config.h"
#include "audio_engine.h"
#if CONFIG_TINYUSB_MIDI_ENABLED
#include "USB.h"
#include "USBMIDI.h"
static USBMIDI usbMIDI("GrvEP");
#endif
// Patch names from Diapasonix (258 Juno+DX7 presets) — wrapped to avoid ODR conflict
namespace { // anonymous namespace: translation-unit local
#include "../../other_projects/Diapasonix/display/patch_names.h"
}
#define SYNTH2_PATCH_COUNT 258

// ==================== GLOBALS ====================
CRGB leds[NUM_LEDS];
Multiplexer mux(MUX_S0, MUX_S1, MUX_S2, MUX_S3, MUX_COM);
uint16_t muxCache[16];
static int cachedJoyX = 0, cachedJoyY = 0;

// ==================== STATE ====================
AppMode currentMode = MODE_SYNTH;
NoteMap noteMap;
uint8_t activeNotes[KBD_NOTE_ROWS][KBD_COLS] = {};

// Synth / Omni
SynthShape currentShape = SHAPE_JUNO_PIANO;
EnvPreset  currentEnv   = ENV_NORMAL;
uint8_t    arpMode      = 0;
float      volume       = 0.7f;

// Arp state (MODE_SYNTH)
uint8_t arpNotes[32]   = {};  // MIDI notes currently held for arp
uint8_t arpNoteCount   = 0;
uint8_t arpIdx         = 0;   // position in arp sequence
uint8_t arpCurrent     = 0;   // note currently playing in arp
bool    arpUdDir       = true; // UD mode: true=ascending

// Omnichord
// Circle of fifths (Eb→B, 9 roots × 3 types)
static const uint8_t omniNoteOff[9]     = {3, 10, 5, 0, 7, 2, 9, 4, 11};
static const char*   omniNoteNames[9]   = {"Eb","Bb","F","C","G","D","A","E","B"};
static const char*   omniTypeNames[3]   = {"M","m","7"};
static const uint8_t omniChordInt[3][3] = {{0,4,7},{0,3,7},{0,4,10}};
uint8_t omniRoot = 0xFF, omniType = 0xFF;
uint8_t omniChordNotes[3] = {};

// Strum strip: 8 positions from bass to treble (intervals relative to chord root)
// Real Omnichord layout: bass octave, fifth below, root, 3rd, 5th, oct, 10th, 12th
static const int8_t omniStrumMaj[8] = {-12, -5, 0, 4, 7, 12, 16, 19};
static const int8_t omniStrumMin[8] = {-12, -5, 0, 3, 7, 12, 15, 19};
static const int8_t omniStrum7[8]   = {-12, -5, 0, 4, 10, 12, 16, 22};

static int8_t  omniStrumPos    = -1;   // current joystick strum position (0-7), -1=off
static float   omniLastJoyY    = 0.0f; // previous Y value for velocity calculation

// Sample play modes
uint8_t  samplePlayMode = 0;  // 0=NRM 1=STA 2=LOP 3=SLO
static const char* kSplayModes[] = {"NRM","STA","LOP","SLO"};
uint32_t sampleLoopNext[SAMPLE_KEY_COUNT] = {};

// Drum state
bool    drumPattern[DRUM_ROWS][DRUM_MAX_STEPS] = {};
uint8_t drumStep    = 0;
bool    drumPlaying = false;
uint16_t bpm        = 120;
uint8_t drumBank    = 0;

// Sequencer state (MODE_SEQ)
bool    seqPattern[8][8] = {};
String  seqPaths[8];             // file path assigned to each track (2 pages × 4)
bool    seqPlaying    = false;
uint8_t seqStep       = 0;
uint8_t seqTrackSel   = 0;       // selected track within current page (0-3)
uint8_t seqPage       = 0;       // current page: 0=tracks 1-4, 1=tracks 5-8

// FX pot sync: set true when an FX is toggled to prevent stale pot-tracking from re-applying wrong params
static bool fxPotNeedSync = false;

// Overlay menu system
enum OverlayType : uint8_t {
    OVERLAY_NONE=0, OVERLAY_FX, OVERLAY_SCALE_ARP, OVERLAY_ENV,
    OVERLAY_INSTR, OVERLAY_SEQ_OPT, OVERLAY_SAMP_OPT, OVERLAY_303, OVERLAY_303_PRESET,
    OVERLAY_SYSEQ, OVERLAY_SEQB2
};
static OverlayType    s_overlay        = OVERLAY_NONE;
static unsigned long  s_overlayCloseAt = 0;   // millis() when overlay auto-closes (0=no pending close)

// SEQ play mode: Full=loops, CutBar=plays 8 steps once then stops, CutNote=stops prev on same track
enum SeqPlayMode : uint8_t { SEQ_FULL=0, SEQ_CUT_BAR, SEQ_CUT_NOTE };
static SeqPlayMode seqPlayMode = SEQ_FULL;
static const char* kSeqPlayModes[] = {"Full","CBar","CNot"};

// Modular synth state (MODE_MODULAR) — analog signal chain: OSC → FILTER → AMP, LFO → pitch
static uint8_t  modShapeIdx  = 0;
static uint8_t  modEnvIdx    = 0;
static float    modCutoff    = 4000.0f;
static float    modReso      = 1.5f;
static float    modLfoRate   = 2.0f;
static float    modLfoDepth  = 0.0f;

// SYNTH2 state — Diapasonix full patch browser
static uint16_t s2PatchIdx   = 0;       // 0-257 (0-127=Juno, 128-257=DX7)
static float    s2Cutoff     = 8000.0f; // LPF cutoff Hz (pot3, power-law 80→8000)

// MOD2 state — PolyAnalog-inspired analog poly synth
static uint8_t  mod2ShapeIdx  = 0;       // index into mod2ShapeSteps[]
static uint8_t  mod2EnvIdx    = 0;       // ENV_NORMAL..ENV_PIANO
static float    mod2Cutoff    = 4000.0f; // Hz (power-law exp mapping)
static float    mod2Reso      = 1.5f;    // Q 0.5-16 (power-law)
static float    mod2LfoRate   = 2.0f;    // Hz (rate³ × 25)
static float    mod2LfoDepth  = 0.0f;    // semitones for pitch, or Hz for filter
static uint8_t  mod2LfoMode   = 0;               // index into mod2LfoMode* tables (0=Off)
static Mod2PlayMode mod2PlayMode = MOD2_POLY;
static uint8_t  mod2LastNote  = 0;       // for mono/slide mode: note currently playing
static uint8_t  slideFromNote = 0;       // slide: source note (currently playing)
static uint8_t  slideToNote   = 0;       // slide: target note
static unsigned long slideStartMs = 0;   // slide: glide start time
static bool     slideActive   = false;   // slide: glide in progress

// ==================== TB-303 STATE ====================
static float         t303Cutoff      = 500.0f;    // base filter cutoff Hz
static float         t303Reso        = 1.5f;      // resonance 1-3
static float         t303EnvMod      = 0.0f;      // filter EG depth (0=off, classic plat)
static float         t303Decay       = 500.0f;    // EG decay ms (fixed — not pot-controlled)
static uint8_t       t303Wave        = SAW_DOWN;  // AMY wave constant (SAW_DOWN, PULSE, TRIANGLE, SINE…) or T303_NAP_WAVE
#define T303_NAP_WAVE 200   // narrow pulse sentinel: PULSE with narrow duty (~10%), P2 varies [0.01, 0.15]
static int8_t        t303Oct         = 0;         // octave offset -1..+1
static bool          t303AccentOn    = false;
static bool          t303SustainOn   = false;     // OFF=pluck (clear decay), ON=held at full amp
static bool          t303SlideOn     = false;
static uint8_t       t303CurrentNote = 0;
static int8_t        t303PressedRow  = -1;
static int8_t        t303PressedCol  = -1;
static float         t303Duration    = 0.5f;      // gate ratio 0→1 (P5): 0=10% of step, 1=full step
static uint8_t       t303ToneIdx     = 0;         // selected preset index
// Pot tracking for 303 (file-scope so preset loader can force resync)
static float         lp303[4]        = {-1.f,-1.f,-1.f,-1.f};

struct T303Tone { const char* name; uint8_t amyWave; float cutoff; float reso; float envMod; };
static const T303Tone t303Tones[] = {
    {"303S", SAW_DOWN, 600.f, 4.f, 5.f},
    {"303Q", PULSE,    500.f, 4.f, 5.f},
    {"ACID", SAW_DOWN, 300.f, 4.f, 8.f},
    {"SQUD", PULSE,    700.f, 4.f, 3.f},
    {"BAS1", SAW_DOWN, 200.f, 3.f, 2.f},
    {"BAS2", PULSE,    400.f, 4.f, 0.f},
    {"TECH", SAW_DOWN,1200.f, 4.f, 6.f},
    {"RAVE", PULSE,   1500.f, 4.f, 2.f},
    {"TRI ", TRIANGLE, 800.f, 4.f, 4.f},
    {"LEAD", SAW_DOWN,2000.f, 4.f, 3.f},
    {"DARK", SAW_DOWN, 150.f, 4.f, 1.f},
    {"SIN ", SINE,     600.f, 4.f, 3.f},
};
#define T303_TONE_COUNT 12
static bool          t303SlideActive = false;     // portamento glide in progress
static uint8_t       t303SlideFrom   = 0;         // source note
static uint8_t       t303SlideTo     = 0;         // target note
static unsigned long t303SlideMs     = 0;         // glide start timestamp


// ==================== DRUM2 STATE ====================
#define DRUM2_PADS  8
#define DRUM2_OH    5   // open hihat → choked by CH
#define DRUM2_CH    6   // closed hihat → chokes OH
#define DR2_STEPS  16   // 16th-note steps per loop
// Physical drum preset indices (within DRUM_PRESET_BASE)
static const uint8_t kDrum2Remap[DRUM2_PADS] = {0, 2, 5, 10, 6, 9, 5, 12};
// KK1, SN1, HHC, CLP, BNG, OHH, CHH, RDE
static const char* kDrum2Labels[DRUM2_PADS]  = {"KK1","SN1","HHC","CLP","BNG","OHH","CHH","RDE"};
static uint8_t       drum2Pitch[DRUM2_PADS]  = {69,69,69,69,69,69,69,69};
static float         drum2Decay[DRUM2_PADS]  = {0,0,200,0,0,400,100,0};
static float         drum2Volume[DRUM2_PADS] = {1,1,1,1,1,1,1,1};
static int8_t        drum2SelPad             = 0;
// Sequencer
static uint8_t       drum2SeqVel[DRUM2_PADS][DR2_STEPS] = {};  // 0=off, else velocity 1-127
static uint8_t       drum2SeqRow[DRUM2_PADS][DR2_STEPS] = {};  // row variation recorded (0-3)
static uint8_t       drum2SeqMod[DRUM2_PADS][DR2_STEPS] = {};  // step modifier: 0=norm 1=prob 2=rpit 3=dbl
static bool          drum2Playing    = false;
static uint8_t       drum2Step       = 0;
static bool          drum2RecArmed   = false;
static bool          drum2SeqView    = false;  // false=pad view, true=step grid view
static unsigned long drum2LastStepMs = 0;
static unsigned long drum2PadFlashMs[DRUM2_PADS]   = {};
static unsigned long drum2DblPendingAt[DRUM2_PADS] = {}; // scheduled "double hit" timestamp
static uint8_t       drum2DblPitch[DRUM2_PADS]     = {}; // pitch captured for deferred doubled hit

// Row variation: each keyboard row gives a different hit character
struct Dr2RowVar { float vel; int8_t pitchOff; float decayFact; };
static const Dr2RowVar kDr2RowVar[KBD_NOTE_ROWS] = {
    {0.30f, -5, 0.25f},  // row 0: ghost  (very soft, low-pitched, short)
    {0.55f, -2, 0.60f},  // row 1: soft   (quiet, slightly low, shorter)
    {0.80f,  0, 1.00f},  // row 2: normal (baseline)
    {1.00f, +3, 1.40f},  // row 3: accent (full vel, higher pitch, long sustain)
};

// Per-step pitch offset (kept for tick compatibility, always 0 in seq view)
static int8_t  drum2SeqPitchOff[DRUM2_PADS][DR2_STEPS] = {};
// Per-pad current alteration mode (Row 1 cycles this; Row 0 resets to 0)
//   0=normal  1=prob50%  2=randpitch  3=double
static uint8_t drum2PadAltMode[DRUM2_PADS] = {};
static bool    drum2SelFromAlt = false;  // true if pad was last selected from the alt row
// drum2Playing/drum2Step/drum2LastStepMs are SHARED with SYSEQ (synced sequencer)

// ==================== SYSEQ STATE ====================
#define SYSEQ_STEPS 16
#define SYSEQ_CHORD 16   // max notes per step
static bool     syseqSeqView   = false;
static uint8_t  syseqSelStep   = 0;
// syseqStep and syseq playing state use drum2Step/drum2Playing/drum2LastStepMs (shared clock)
static uint8_t  syseqNotes[SYSEQ_STEPS][SYSEQ_CHORD] = {};  // 0=empty, else MIDI note+1
static uint8_t  syseqVels [SYSEQ_STEPS][SYSEQ_CHORD] = {};  // velocity 1-127
static uint8_t  syseqActive[SYSEQ_CHORD] = {};  // notes currently held by sequencer (for note-off)
static uint8_t  syseqActiveCnt = 0;
static uint8_t  syseqAlt    [SYSEQ_STEPS] = {};  // per-step: 0=NRM 1=FUL (hold note through empty steps)
static bool     syseqActiveIsFull = false;        // true if currently-playing notes have FUL alt

// ==================== 303S STATE ====================
#define S303_STEPS 16
static bool    s303SeqView    = false;
static uint8_t s303SelStep    = 0;
static uint8_t s303Note  [S303_STEPS] = {};  // 0=empty, else MIDI note+1
static uint8_t s303Alt   [S303_STEPS] = {};  // per-step: 0=NRM 1=ACC 2=SLD
static uint8_t        s303CurNote    = 0;
static uint8_t s303SelNote    = 0;           // note selected for step placement (MIDI+1), 0=none
static uint8_t s303PendingAlt = 0;           // alt applied at placement time (0=NRM 1=ACC 2=SLD)

// ==================== SS2 STATE ====================
static uint16_t ss2Note  [SS2_SLOTS] = {};           // step→slot bitmask: bit i = slot i plays on this step
static uint8_t  ss2Alt   [SS2_SLOTS] = {};           // per-step: 0=NRM 1=REV 2=FUL 3=SIL
static char     ss2Path  [SS2_SLOTS][128] = {};       // file path for each slot
static bool     ss2Loaded[SS2_SLOTS] = {};            // true when audioLoadKey finished
static uint8_t  ss2SelStep  = 0;
static uint8_t  ss2SelSlot  = 0;
static bool     ss2SeqView  = false;
static uint8_t  ss2PlayMode = 0;  // 0=NRM (play to end) 1=OVR (new sample cuts all running)
static uint8_t  ss2CurSlot = 0xFF;                   // slot playing from sequencer (0xFF=none)
static uint32_t ss2LastPlayMs[SS2_SLOTS] = {};        // millis() of last trigger per slot (for FUL)

// ==================== SDS STATE ====================

// Light play ripple state (MODE_LIGHTPLAY)
struct Ripple { int8_t ledIdx; float radius; uint8_t hue; uint8_t bright; };
static Ripple ripples[8];
static uint8_t rippleBrightMap[NUM_LEDS] = {};

// ==================== GRANULAR STATE ====================
static uint8_t  granSubMode   = 0;        // 0 = 8-slice, 1 = 1/16 energy-ranked
static uint8_t  granSliceCount = 0;       // computed after source load
static uint8_t  granWaveform[128]  = {};  // 0-255 amplitude bars for display
static bool     granComputed   = false;   // slices computed and ready to play
static String   granFilePath   = "";      // path of current granular source

// granSubMode: 0=8-slice  1=1/16-energy  2=8-slice-loop  3=1/16-loop
// 8-slice row layout: R0=one-shot px, R1=double px+px+1, R2=tail sx→end (looped), R3=px reversed
// 16-slice row layout: R0=slices 0-7, R1=slices 8-15, R2=0-7 looped, R3=8-15 looped
static inline uint16_t granPresetForKey(uint8_t row, uint8_t col) {
    uint8_t c = 7u - col;  // physical left key = slice 0 (start of sample)
    if ((granSubMode & 1u) == 0) {
        // 8-slice mode
        if (row == 0) return GRANULAR_PRESET_BASE + c;   // one-shot px
        if (row == 1) return GRANULAR_DBL_PRESET  + c;   // double px+px+1
        if (row == 2) return GRANULAR_TAIL_PRESET + c;   // tail sx→end (looped)
        return              GRANULAR_REV_PRESET   + c;   // px reversed
    } else {
        // 16-slice mode: rows 0-1 = slices 0-15, rows 2-3 = same slices but looped
        if (row == 0) return GRANULAR_PRESET_BASE     + c;       // slices 0-7
        if (row == 1) return GRANULAR_PRESET_BASE + 8 + c;       // slices 8-15
        if (row == 2) return GRANULAR_PRESET_BASE     + c;       // slices 0-7 looped
        return              GRANULAR_PRESET_BASE + 8 + c;        // slices 8-15 looped
    }
}
static inline bool granIsLoop(uint8_t row) {
    if (granSubMode >= 2) return true;                    // modes 2&3: always loop
    if ((granSubMode & 1u) == 0) return (row == 2);      // 8-slice: row2 (tail) loops
    return (row >= 2);                                    // 16-slice: rows 2-3 loop
}
static float granWinStart = 0.0f, granWinEnd = 1.0f;
static int8_t granPlayingSlice = -1;  // 0-N-1: last selected slice index (persists after release); -1 = none
static bool   granKeyHeld = false;   // true while a granular key is physically held
// Split points: N+1 boundaries within window [0.0..1.0]. Pot A = splits[x], Pot B = splits[x+1].
static float granSplits[GRANULAR_MAX_SLICES + 1] = {};  // initialized to even spacing on file load
// Signals the pot handler to re-sync its delta baseline to current pot positions
// (set true on file load and mode entry to prevent stale deltas from other modes)
static bool granPotNeedsSync = true;

// ==================== GRANULAR2 STATE ====================
struct Gran2State {
    String  path;
    bool    loaded;    // audioIsGranular2Ready returned true
    bool    computed;  // slices computed and presets registered
    uint8_t sliceCount;
    float   splits[GRAN2_MAX_SLICES + 1];
    uint8_t waveform[128];
};
static Gran2State   gran2[GRAN2_MAX_SAMPLES];
// gran2PlayMode: 0=once (one-shot), 1=loop (loop slice), 2=full (loop full sample from slice start)
// Fixed x4: 4 samples (TL=S0, TR=S1, BL=S2, BR=S3), 4 slices each, fwd+rev rows per half.
static uint8_t      gran2PlayMode     = 0;
static int8_t       gran2ActiveSample = -1;   // last-played sample index (for display + pot control)
static int8_t       gran2ActiveSlice  = -1;   // last-played slice within that sample
static uint8_t      gran2LoadTarget   = 0;    // which slot the SD browser loads into
static bool         gran2PotNeedsSync = true;

static uint8_t gran2NumSamples() { return 4u; }
static uint8_t gran2NumSlices()  { return 4u; }

// Map (row, col) → (sampleIdx, sliceIdx, isReverse).
// Physical layout (code row 0 = physical bottom; gc = 7-c so code col 7 = physical left):
//   S0=physical TL (rows2-3, cols4-7), S1=physical TR (rows2-3, cols0-3)
//   S2=physical BL (rows0-1, cols4-7), S3=physical BR (rows0-1, cols0-3)
//   Within each half: outer row = fwd, inner row = rev. Slices 0-3 go left to right.
static void gran2KeyInfo(uint8_t row, uint8_t col, uint8_t& sampleIdx, uint8_t& sliceIdx, bool& reverse) {
    sampleIdx = (uint8_t)((row >= 2 ? 0u : 2u) + (col >= 4 ? 0u : 1u));
    reverse   = (row == 1 || row == 3);
    sliceIdx  = (uint8_t)(col >= 4 ? (7u - col) : (3u - col));
}

// ==================== TRACKER STATE ====================
// Left 4×4 (cols 0-3): instrument selector. Right 4×4 (cols 4-7): note grid.
// TRACKER_TRACKS = 16: 0-6 synth, 7 drum, 8-15 sample slots 0-7.
// trkNotes[track][step][chord_slot]: up to TRK_CHORD_SIZE simultaneous notes per step. -1 = unused slot.
static int8_t   trkNotes[TRACKER_TRACKS][TRACKER_STEPS][TRK_CHORD_SIZE];
static uint8_t  trkVel  [TRACKER_TRACKS][TRACKER_STEPS];
static uint8_t  trkStep       = 0;
static bool     trkPlaying    = false;
static bool     trkRec        = false;
static uint8_t  trkInstr      = 0;   // selected track (0-15)
static uint32_t trkLastStepMs = 0;
// Shape per synth track — diverse defaults so each track sounds distinctly different
static uint8_t  trkSynthShape[TRACKER_SYNTHS] = {
    SHAPE_SAW, SHAPE_SQUARE, SHAPE_SINE, SHAPE_BASS, SHAPE_PLUCK, SHAPE_ACID, SHAPE_SUPERSAW
};
static uint8_t  trkDrumPad = 0;  // which drum pad the drum track uses
static int8_t   trkOctave  = 0;  // octave offset: -4..+4, applied to note grid
// C-major diatonic note grid: right 4×4 (rows top→bottom, cols left→right)
// Indexed as kTrkNoteGrid[3-row] so physical row 0 (bottom) = low notes.
static const uint8_t kTrkNoteGrid[4][4] = {
    {60, 62, 64, 65},   // [0] physical bottom row: C4  D4  E4  F4
    {65, 67, 69, 71},   // [1]                      F4  G4  A4  B4
    {72, 74, 76, 77},   // [2]                      C5  D5  E5  F5
    {79, 81, 83, 84},   // [3] physical top row:    G5  A5  B5  C6
};
static const char* kTrkInstrNames[TRACKER_TRACKS] = {
    "SYN1","SYN2","SYN3","SYN4","SYN5","SYN6","SYN7",
    "DRUM","SMP1","SMP2","SMP3","SMP4","SMP5","SMP6","SMP7","SMP8"
};
// Per-track last-played notes (for note-off on step change); [track][chord_slot]
static int8_t trkPlayingNote[TRACKER_TRACKS][TRK_CHORD_SIZE];

// ==================== MIDI STATE ====================
static uint8_t midiLedVel[KBD_ROWS][KBD_COLS] = {};  // velocity 0=off, >0=on color
static uint8_t midiOctave   = 4;    // base octave for MIDI note output (C4=60 at oct=4)
static uint8_t midiChannel  = 1;    // MIDI output channel 1-16 (btn0 short-press cycles)
static bool    midiActive   = true;
static float   midiLpPots[7]= {-1,-1,-1,-1,-1,-1,-1}; // last-sent CC values (by pot index)
static uint8_t midiLastMod  = 255;  // last sent CC#1 (joystick X)
static float   midiLastPB   = -999.f; // last sent pitch bend (joystick Y)
// Per-pot CC assignment: 0-127 = CC number, 0xFF = disabled. Freely editable in config mode.
static uint8_t midiPotCC[7] = {7, 1, 11, 74, 71, 91, 93};
//                              P0  P1  P2  P3   P4  P5  P6
//                             Vol Mod Exp Brt  Res Rev Cho
static int8_t  midiPotSel   = -1; // -1 = normal play; 0-7 = config mode (0-6=pot, 7=layout)

// ==================== KEYBOARD LAYOUT ====================
enum KbdLayout : uint8_t { KBD_LAYOUT_GRID=0, KBD_LAYOUT_PIANO };
static KbdLayout kbdLayout = KBD_LAYOUT_GRID;
// Semitone offset from base note for piano layout, indexed [row][col], -1 = silent key
// row=0 (bottom): white keys C..C'  |  row=1: black keys  |  row=2: white D'..D''  |  row=3: black
static const int8_t pianoPad[KBD_NOTE_ROWS][KBD_COLS] = {
    { 12,  11,   9,   7,   5,   4,   2,   0 }, // r=0 C  D  E  F  G  A  B  C'
    { 13,  -1,  10,   8,   6,  -1,   3,   1 }, // r=1 C#' --  A# G# F# -- D# C#
    { 26,  24,  23,  21,  19,  17,  16,  14 }, // r=2 D'' C'' B' A' G' F' E' D'
    { 27,  25,  -1,  22,  20,  18,  -1,  15 }, // r=3 D#'' C#'' -- A#' G#' F#' -- D#'
};
// Returns MIDI note for piano layout, or 0xFF if the key is silent
static inline uint8_t pianoNote(uint8_t row, uint8_t col, uint8_t baseNote) {
    if (row >= KBD_NOTE_ROWS || col >= KBD_COLS) return 0xFF;
    int8_t off = pianoPad[row][col];
    if (off < 0) return 0xFF;
    return (uint8_t)constrain((int)baseNote + off, 0, 127);
}

// Menu
bool    menuOpen       = false;
uint8_t menuRow        = 0, menuCol = 0;
bool    lastClick      = false;
uint32_t joyClickMs   = 0;    // millis() when joystick click started (0=not pressed)
bool    joyLongFired  = false; // long-click action already triggered this press
uint32_t btn1PressTime = 0;
bool    btn1Handled    = false;

// Pots
struct EncPot { float prevAngle; float accum; float value; bool init; };
EncPot pots[7] = {};

// ==================== FX STATE ====================
struct FxEffect {
    const char* name;
    bool        active;
    float       params[4];
    const char* paramNames[4];
    float       paramMin[4];
    float       paramMax[4];
};

FxEffect fxList[] = {
    // FILT: general filter — type selected by Typ param (0=LPF,1=LaF/24dB,2=HPF,3=BPF)
    {"FILT",     false, {2000.0f, 1.5f, 0.0f, 0.0f},
     {"Cut","Res","Typ",""},
     {65.0f,   0.5f, 0.0f, 0.0f},
     {18000.0f, 3.0f, 3.0f, 0.0f}},
    // Overdrive: filter resonance saturation
    {"OVERDRIVE",false, {0.6f, 0.0f,  0.0f, 0.0f},
     {"Drv","","",""},
     {0.0f, 0.0f, 0.0f, 0.0f},
     {1.0f, 0.0f, 0.0f, 0.0f}},
    // Reverb
    {"REVERB",   false, {1.5f, 0.85f, 0.5f, 3000.0f},
     {"Lvl","Live","Damp","Xover"},
     {0.0f, 0.5f,  0.0f, 500.0f},
     {4.0f, 1.0f,  1.0f, 8000.0f}},
    // Chorus
    {"CHORUS",   false, {1.0f, 0.5f,  0.5f, 0.0f},
     {"Lvl","Freq","Dep",""},
     {0.0f, 0.1f,  0.0f, 0.0f},
     {4.0f, 4.0f,  1.0f, 0.0f}},
    // Flanger: params[3]=Dly controls max_delay (ms) — shorter = comb filter, longer = chorus
    {"FLANGER",  false, {1.0f, 0.3f,  0.9f, 15.0f},
     {"Lvl","Freq","Dep","Dly"},
     {0.0f, 0.05f, 0.1f,  5.0f},
     {4.0f, 50.0f, 1.0f, 100.0f}},
    // Delay — tempo-synced to BPM (1/4 note); only Level is user-controlled
    {"DELAY",    false, {0.6f, 4.0f, 0.55f, 0.0f},
     {"Lvl","","",""},
     {0.0f,  0.0f, 0.0f, -1.0f},
     {1.0f,  6.0f, 0.8f,  1.0f}},
    // LFO: software filter LFO — modulates active LPF cutoff
    {"LFO",      false, {3.0f, 0.7f, 0.0f, 0.0f},
     {"Rate","Dep","",""},
     {0.1f, 0.0f, 0.0f, 0.0f},
     {20.0f, 1.0f, 0.0f, 0.0f}},
    // EQ: 3-band EQ via AMY config_eq (1.0=flat, >1 boost, <1 cut)
    {"EQ",       false, {1.5f, 1.0f, 0.8f, 0.0f},
     {"Low","Mid","Hi",""},
     {0.0f, 0.0f, 0.0f, 0.0f},
     {4.0f, 4.0f, 4.0f, 0.0f}},
    // ResEcho: BPM-synced echo (1/8 note) with resonant tone filter in feedback path
    // Tone: filter_coef — negative=bright metallic echo, positive=warm dark echo
    // Shares the AMY echo bus with DELAY; use one or the other, not both simultaneously.
    {"RESECHO",  false, {0.7f, 0.65f, 0.6f, 2.0f},
     {"Lvl","FB","Tone",""},
     {0.0f, 0.3f, -0.5f, 0.0f},
     {1.5f, 0.85f, 0.9f, 6.0f}},
};
static const uint8_t FX_COUNT = 9;

// Delay subdivisions — param[1] is an index 0..6 into these tables
static const float kDelaySubdiv[]     = { 0.25f, 0.333f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f };
static const char* kDelaySubdivName[] = { "1/16","T1/8","1/8","D1/8","1/4","D1/4","1/2" };
static const uint8_t DELAY_SUBDIV_COUNT = 7;

uint8_t fxSelected = 0;
static float lpfSmoothCut = 8000.0f; // anti-zipper: smoothed cutoff applied each 10ms tick

// Activation order: fxOrderList[0] was activated first, fxOrderList[fxOrderCount-1] last.
// Effects applied in this order, so the last activated filter effect wins.
uint8_t fxOrderList[FX_COUNT] = {};
uint8_t fxOrderCount = 0;

void fxOrderAdd(uint8_t fx) {
    if (fxOrderCount < FX_COUNT) fxOrderList[fxOrderCount++] = fx;
}
void fxOrderRemove(uint8_t fx) {
    for (uint8_t i = 0; i < fxOrderCount; i++) {
        if (fxOrderList[i] == fx) {
            memmove(&fxOrderList[i], &fxOrderList[i+1], fxOrderCount - i - 1);
            fxOrderCount--;
            return;
        }
    }
}

void applyFxEffect(uint8_t fx) {
    FxEffect &e = fxList[fx];
    bool on = e.active;
    // Helper: no FX filter active → safe to restore shape's native filter coefficients
    auto noFilterFx = [&]() {
        return !fxList[0].active && !fxList[1].active && !fxList[6].active;
    };
    switch (fx) {
        case 0: {  // FILT — general filter (0=LPF 1=LaF/LPF24 2=HPF 3=BPF)
            static const uint8_t kFiltAMY[] = {FILTER_LPF, FILTER_LPF24, FILTER_HPF, FILTER_BPF};
            static const char* kFiltName[]  = {"LPF","LaF","HPF","BPF"};
            uint8_t ti = (uint8_t)constrain((int)roundf(e.params[2]), 0, 3);
            Serial.printf("FX0 FILT %s cut=%.0f res=%.2f typ=%s\n",
                on?"ON":"off", on?e.params[0]:0.0f, on?e.params[1]:1.5f, kFiltName[ti]);
            audioSetAllFiltersT(on ? e.params[0] : 0.0f, on ? e.params[1] : 1.5f,
                                on ? kFiltAMY[ti] : FILTER_LPF24);
            if (!on && noFilterFx()) audioRestoreShapeFilter(currentShape);
            if (!on && (currentMode == MODE_303 || currentMode == MODE_303S))
                audioT303Params(t303Cutoff, t303Reso, t303EnvMod, t303Decay);
            break;
        }
        case 1:  // Overdrive
            Serial.printf("FX1 OVD %s drv=%.2f\n", on?"ON":"off", on?e.params[0]:0.0f);
            audioSetOverdrive(on ? e.params[0] : 0.0f);
            if (!on && noFilterFx()) audioRestoreShapeFilter(currentShape);
            break;
        case 2:  // Reverb
            Serial.printf("FX2 REV %s lvl=%.2f\n", on?"ON":"off", on?e.params[0]:0.0f);
            audioSetReverb(on ? e.params[0] : 0.0f, e.params[1], e.params[2], e.params[3]);
            break;
        case 3:  // Chorus
            Serial.printf("FX3 CHO %s lvl=%.2f\n", on?"ON":"off", on?e.params[0]:0.0f);
            audioSetChorus(on ? e.params[0] : 0.0f, e.params[1], e.params[2]);
            break;
        case 4:  // Flanger — params[3] controls max_delay (5-100ms; shorter=comb, longer=chorus)
            Serial.printf("FX4 FLG %s lvl=%.2f dly=%.0fms\n", on?"ON":"off", on?e.params[0]:0.0f, e.params[3]);
            config_chorus(0, on ? e.params[0] : 0.0f, (uint16_t)e.params[3], e.params[1], e.params[2]);
            break;
        case 5: {  // Delay — tempo-synced; param[1] = subdivision index 0..6
            uint8_t si = (uint8_t)constrain((int)roundf(e.params[1]), 0, DELAY_SUBDIV_COUNT-1);
            float dms  = 60000.0f / (float)bpm * kDelaySubdiv[si];
            dms = constrain(dms, 30.0f, 700.0f);
            Serial.printf("FX5 DLY %s lvl=%.2f div=%s %.0fms\n", on?"ON":"off", on?e.params[0]:0.0f, kDelaySubdivName[si], dms);
            audioSetDelay(on ? e.params[0] : 0.0f, dms, e.params[2], e.params[3]);
            break;
        }
        case 6:  // LFO — handled in the 10ms loop
            break;
        case 7:  // EQ
            Serial.printf("FX7 EQ  %s L=%.2f M=%.2f H=%.2f\n", on?"ON":"off",
                          on?e.params[0]:1.0f, on?e.params[1]:1.0f, on?e.params[2]:1.0f);
            audioSetEq(on ? e.params[0] : 1.0f,
                       on ? e.params[1] : 1.0f,
                       on ? e.params[2] : 1.0f);
            break;
        case 8: {  // ResEcho — BPM-synced echo with tonal filter_coef
            uint8_t si = (uint8_t)constrain((int)roundf(e.params[3]), 0, DELAY_SUBDIV_COUNT-1);
            float dms  = 60000.0f / (float)bpm * kDelaySubdiv[si];
            dms = constrain(dms, 30.0f, 700.0f);
            Serial.printf("FX8 RES %s lvl=%.2f fb=%.2f tone=%.2f %s=%.0fms\n",
                          on?"ON":"off", on?e.params[0]:0.0f, e.params[1], e.params[2],
                          kDelaySubdivName[si], dms);
            audioSetDelay(on ? e.params[0] : 0.0f, dms, e.params[1], e.params[2]);
            break;
        }
    }
}

void applyAllFx() {
    // Reset all inactive effects first (so their "off" state is clean)
    for (uint8_t i = 0; i < FX_COUNT; i++) if (!fxList[i].active) applyFxEffect(i);
    // Apply active effects in activation order (last activated filter wins)
    for (uint8_t i = 0; i < fxOrderCount; i++) applyFxEffect(fxOrderList[i]);
}

// ==================== SAMPLE BROWSER ====================
bool   sdReady      = false;
String sdPath       = "/";
String sdFiles[32];
bool   sdFileIsDir[32];
int    sdFileCount  = 0, sdCursor = 0, sdScroll = 0;
String sampleMap[KBD_NOTE_ROWS][KBD_COLS];

bool isAudioFile(const char* name) {
    const char* ext = strrchr(name, '.');
    if (!ext) return false;
    return strcasecmp(ext,".wav")==0 || strcasecmp(ext,".mp3")==0;
}
bool isWavFile(const char* name) {
    const char* ext = strrchr(name, '.');
    return ext && strcasecmp(ext,".wav")==0;
}

void sdListDir(const String &path) {
    sdFileCount=0; sdCursor=0; sdScroll=0; sdPath=path;
    // Always show ".." — at root it opens the main menu
    sdFiles[0]=".."; sdFileIsDir[0]=true; sdFileCount++;
    File dir=SD.open(path); if(!dir||!dir.isDirectory()) return;
    File entry=dir.openNextFile();
    while (entry && sdFileCount<32) {
        const char* full=entry.name(); const char* name=strrchr(full,'/');
        name=name?name+1:full;
        if (name[0]!='.' && (entry.isDirectory()||isAudioFile(name))) {
            sdFiles[sdFileCount]=name; sdFileIsDir[sdFileCount]=entry.isDirectory(); sdFileCount++;
        }
        entry.close(); entry=dir.openNextFile();
    }
    dir.close();
}

String buildSdFilePath() {
    if (sdCursor >= sdFileCount) return "";
    String fp = sdPath;
    if (!fp.endsWith("/")) fp += "/";
    fp += sdFiles[sdCursor];
    return fp;
}

// ==================== MODE SWITCH ====================
void switchMode(AppMode newMode) {
    // Stop sample oscillators when leaving any mode that uses them
    if (currentMode==MODE_SAMPLE||currentMode==MODE_HYBRID)
        audioStopAllSamples();
    if (currentMode==MODE_SEQ) {
        // Keep SEQ playing only when entering utility/FX modes; kill it for any instrument mode
        bool seqContinues = (newMode==MODE_FX||newMode==MODE_LIGHT||
                             newMode==MODE_LIGHTPLAY||newMode==MODE_BATTERY||newMode==MODE_SYSINFO);
        if (!seqContinues) { audioStopAllSamples(); seqPlaying=false; }
    }
    if (currentMode==MODE_MODULAR||currentMode==MODE_MOD2){
        audioSetFilter(0.0f, 1.5f);
        audioSetPitchBend(1.0f);
    }
    if (currentMode==MODE_303){
        if (t303CurrentNote) audioT303NoteOff(t303CurrentNote);
        audioT303PitchBend(1.0f);
        t303CurrentNote=0; t303SlideActive=false; t303PressedRow=-1; t303PressedCol=-1;
        applyFxEffect(2);  // restore global reverb to FX state on 303 exit
    }
    if (currentMode==MODE_DRUM2){
        drum2RecArmed=false;
        // Sequencer keeps playing (shared with SYSEQ)
    }
    if (currentMode==MODE_SYSEQ){
        // Stop synth notes from sequencer; keep shared clock running
        for(uint8_t i=0;i<syseqActiveCnt;i++) audioNoteOff(syseqActive[i]);
        syseqActiveCnt=0; syseqActiveIsFull=false;
        // Stop held keyboard notes
        for(uint8_t r2=0;r2<KBD_NOTE_ROWS;r2++)
            for(uint8_t c2=0;c2<KBD_COLS;c2++)
                if(activeNotes[r2][c2]){ audioNoteOff(activeNotes[r2][c2]); activeNotes[r2][c2]=0; }
    }
    if (currentMode==MODE_303S){
        if (s303CurNote) { audioT303NoteOff(s303CurNote); s303CurNote=0; }
        audioT303PitchBend(1.0f);
        t303SlideActive=false;
        for(uint8_t r2=0;r2<KBD_NOTE_ROWS;r2++)
            for(uint8_t c2=0;c2<KBD_COLS;c2++)
                if(activeNotes[r2][c2]){ audioT303NoteOff(activeNotes[r2][c2]); activeNotes[r2][c2]=0; }
    }
    if (currentMode==MODE_SS2){
        for(uint8_t r2=0;r2<KBD_NOTE_ROWS;r2++)
            for(uint8_t c2=0;c2<KBD_COLS;c2++)
                activeNotes[r2][c2]=0;
        // Keep samples in PSRAM — other sequencer modes may use them
    }
    if (currentMode==MODE_GRANULAR)  audioUnloadGranular();
    if (currentMode==MODE_GRANULAR2) audioUnloadGranular2();
    if (currentMode==MODE_TRACKER && trkPlaying) {
        trkPlaying=false;
        for (int t=0;t<TRACKER_TRACKS;t++) {
            if (t<TRACKER_SYNTHS) {
                for (int ci=0;ci<TRK_CHORD_SIZE;ci++) {
                    if (trkPlayingNote[t][ci]>=0) {
                        audioTrackerNoteOff((uint8_t)t,(uint8_t)trkPlayingNote[t][ci]);
                        trkPlayingNote[t][ci]=-1;
                    }
                }
            } else if (t==TRACKER_DRUM_TRK) { /* percussive — let decay */
            } else {
                audioStopKey((uint8_t)(t-TRACKER_SAMP_BASE));
                trkPlayingNote[t][0]=-1;
            }
        }
    }
    audioAllNotesOff();
    omniRoot=0xFF; omniStrumPos=-1; omniLastJoyY=0.0f;
    memset(activeNotes,0,sizeof(activeNotes));
    arpNoteCount=0; arpIdx=0; arpCurrent=0; arpUdDir=true;
    s_overlay = OVERLAY_NONE; s_overlayCloseAt = 0;
    currentMode=newMode;
    if (newMode==MODE_SEQ&&sdReady) sdListDir("/");
    if (newMode==MODE_LIGHTPLAY) memset(rippleBrightMap,0,sizeof(rippleBrightMap));
    if (newMode==MODE_HYBRID&&sdReady) sdListDir("/");
    if (newMode==MODE_SYNTH2 && audioReady) {
        amy_event e = amy_default_event();
        e.synth = SYNTH_CH; e.patch_number = s2PatchIdx;
        amy_add_event(&e);
        audioSetFilter(s2Cutoff, 1.5f);
    }
    if (newMode==MODE_MOD2 && audioReady) {
        audioSetShape(mod2ShapeSteps[mod2ShapeIdx]);
        audioSetFilter(mod2Cutoff, mod2Reso);
        audioSetEnvelope(envTable[mod2EnvIdx]);
    }
    if (newMode==MODE_303 && audioReady) {
        audioT303SetSustain(t303SustainOn ? 1.0f : 0.0f);
        audioT303Init(t303Cutoff, t303Reso, t303EnvMod, t303Decay, t303Wave);
    }
    if (newMode==MODE_DRUM2) {
        drum2Step=0; drum2LastStepMs=millis();
        drum2SeqView=false;
        memset(drum2PadFlashMs,   0, sizeof(drum2PadFlashMs));
        memset(drum2DblPendingAt, 0, sizeof(drum2DblPendingAt));
    }
    if (newMode==MODE_SYSEQ) {
        syseqSelStep=0;
        syseqSeqView=false;
        if (audioReady) { audioSetShape(currentShape); audioSetEnvelope(envTable[currentEnv]); }
        // Shared clock continues if already playing; will sync on next step
    }
    if (newMode==MODE_303S) {
        s303SelStep=0;
        s303SeqView=false;
        s303CurNote=0;
        t303Duration = 0.5f;
        t303Decay = 30.0f * powf(100.0f, 0.5f);   // ≈300ms at default 50%
        lp303[2] = pots[5].value;                   // pot pickup: keep t303Duration=50% until pot moves
        if (audioReady) audioT303Init(t303Cutoff, t303Reso, t303EnvMod, t303Decay,
                                       t303Wave==T303_NAP_WAVE ? PULSE : t303Wave);
        // Shared clock continues if already playing
    }
    if (newMode==MODE_SS2) {
        ss2SelStep=0; ss2SelSlot=0; ss2SeqView=false; ss2CurSlot=0xFF; ss2PlayMode=0;
        if (sdReady) sdListDir(sdPath.length()>1 ? sdPath.c_str() : "/");
        for(uint8_t i=0;i<SS2_SLOTS;i++) ss2Loaded[i] = ss2Path[i][0] && audioKeyLoaded(SS2_KEY_BASE+i);
    }
    if (newMode==MODE_GRANULAR && sdReady) { sdListDir("/"); granWinStart=0.0f; granWinEnd=1.0f; granPlayingSlice=-1; granKeyHeld=false; granPotNeedsSync=true; }
    if (newMode==MODE_GRANULAR2 && sdReady) {
        sdListDir("/");
        memset(gran2, 0, sizeof(gran2));
        gran2ActiveSample = -1; gran2ActiveSlice = -1; gran2LoadTarget = 0; gran2PotNeedsSync = true;
        pots[3].value = 0.5f; pots[4].value = 0.5f;  // mid-range so splits can move in either direction
    }
    if (newMode==MODE_TRACKER) {
        memset(trkPlayingNote, -1, sizeof(trkPlayingNote));
        trkStep=0; trkLastStepMs=millis();
        audioTrackerInit();  // initialize per-track AMY synth channels with current shapes
        // Apply current shapes to each channel so they're immediately distinct
        for (int t = 0; t < TRACKER_SYNTHS; t++)
            audioSetShapeOnSynth((SynthShape)trkSynthShape[t], (uint8_t)(TRACKER_SYNTH_CH_BASE + t));
    }
#if CONFIG_TINYUSB_MIDI_ENABLED
    if (newMode==MODE_MIDI) {
        memset(midiLedVel, 0, sizeof(midiLedVel));
        midiActive = true;
        midiPotSel = -1;
        midiLastMod = 255; midiLastPB = -999.f;
        for (int p = 0; p < 7; p++) midiLpPots[p] = -1.f;
        // Send All Notes Off + reset controllers on entry
        for (int n = 0; n < 128; n++) usbMIDI.noteOff((uint8_t)n, 0, midiChannel);
        usbMIDI.controlChange(121, 0, midiChannel);  // Reset All Controllers
        usbMIDI.pitchBend((double)0.0, midiChannel);
    }
#endif
}

// ==================== DRUM SYNTH ====================
struct DrumSound { const char* name; uint8_t wave; float freq; uint16_t decay; float vel; };

static const DrumSound drumSounds[32] = {
    {"Kick1",SINE,50,100,0.8f},{"Kick2",SINE,55,80,0.8f},{"Kick3",SINE,45,120,0.9f},{"Kick4",SINE,60,70,0.7f},
    {"Kick5",SINE,40,150,0.9f},{"Kick6",SINE,65,60,0.7f},{"KickD",SINE,35,180,1.0f},{"KickH",SINE,70,50,0.6f},
    {"Snr1",NOISE,250,80,0.7f},{"Snr2",NOISE,300,60,0.7f},{"Snr3",NOISE,200,100,0.8f},{"Snr4",NOISE,350,50,0.6f},
    {"Clap",NOISE,1500,40,0.7f},{"Rim",SINE,800,20,0.5f},{"Snap",NOISE,2000,15,0.6f},{"Clav",SINE,1200,30,0.5f},
    {"HH1",NOISE,6000,25,0.4f},{"HH2",NOISE,8000,20,0.4f},{"HHO",NOISE,6000,80,0.5f},{"Ride",NOISE,5000,150,0.3f},
    {"Crsh",NOISE,4000,200,0.5f},{"Shhh",NOISE,10000,10,0.3f},{"Tik",NOISE,12000,8,0.3f},{"Cym",NOISE,3000,250,0.4f},
    {"Tom1",SINE,100,80,0.6f},{"Tom2",SINE,130,70,0.6f},{"Tom3",SINE,80,90,0.7f},{"Tom4",SINE,160,60,0.5f},
    {"Bong",SINE,300,40,0.5f},{"Cowb",SINE,600,25,0.4f},{"Blk",SINE,1000,15,0.4f},{"Whis",SINE,2000,100,0.3f},
};

void triggerDrumSound(uint8_t idx) {
    if (!audioReady || idx>=32) return;
    const DrumSound &d = drumSounds[idx];
    uint8_t osc = AMY_OSC_DRUM_BASE + idx;  // one oscillator per sound — 32 independent voices
    amy_event e = amy_default_event();
    e.osc=osc; e.wave=d.wave; e.freq_coefs[0]=d.freq;
    e.eg0_times[0]=1; e.eg0_values[0]=1.0f;
    e.eg0_times[1]=d.decay; e.eg0_values[1]=0.0f;
    e.velocity=d.vel*volume;
    amy_add_event(&e);
}

// ==================== MENU ====================
void selectMenuItem() {
    uint8_t sel = menuRow*MENU_COLS + menuCol;
    if (sel>=MENU_ITEM_COUNT) { menuOpen=false; return; }
    menuOpen=false;
    switch (sel) {
        case MENU_SYNTH:     switchMode(MODE_SYNTH);     break;
        case MENU_OMNI:      switchMode(MODE_OMNI);      break;
        case MENU_DRUMS:     switchMode(MODE_DRUMS);     break;
        case MENU_SAMPLE:    switchMode(MODE_SAMPLE);    if(sdReady) sdListDir("/"); break;
        case MENU_FX:        switchMode(MODE_FX);        break;
        case MENU_LIGHT:     switchMode(MODE_LIGHT);     break;
        case MENU_SEQ:       switchMode(MODE_SEQ);       break;
        case MENU_LIGHTPLAY: switchMode(MODE_LIGHTPLAY); break;
        case MENU_SD:        switchMode(MODE_SYSINFO);   break;
        case MENU_ABOUT:     switchMode(MODE_BATTERY);   break;
        case MENU_HYBRID:    switchMode(MODE_HYBRID);    if(sdReady) sdListDir("/"); break;
        case MENU_MODULAR:   switchMode(MODE_MODULAR);   break;
        case MENU_SYNTH2:    switchMode(MODE_SYNTH2);    break;
        case MENU_MOD2:      switchMode(MODE_MOD2);      break;
        case MENU_303:       switchMode(MODE_303);        break;
        case MENU_GRANULAR:  switchMode(MODE_GRANULAR);   break;
        case MENU_GRANULAR2: switchMode(MODE_GRANULAR2);  break;
        case MENU_MIDI:      switchMode(MODE_MIDI);       break;
        case MENU_TRACKER:   switchMode(MODE_TRACKER);    break;
        case MENU_DRUM2:     switchMode(MODE_DRUM2);       break;
        case MENU_SYSEQ:     switchMode(MODE_SYSEQ);       break;
        case MENU_303S:      switchMode(MODE_303S);        break;
        case MENU_SS2:       switchMode(MODE_SS2); if(sdReady) sdListDir("/"); break;
        default: break;
    }
}

// ==================== OVERLAY KEY HANDLER ====================
static const int8_t kOctOpts[] = {-2, -1, 0, 1};

// Overlay key mapping:
//   2-column overlays (FX, ENV, SEQ_OPT, SAMP_OPT): cols 6-7, opts 0-7
//   4-column overlays (SCALE_ARP):                   cols 4-7, opts 0-15
//   Full-grid overlay (INSTR): toutes les touches de notes (rows 0-3, cols 0-7), 32 opts
//     opt = (3-row)*8 + (7-col) → row 3 col7=opt0 … row 0 col0=opt31
// LED positions resolved at runtime via crdToIdx (respects OOPSIE_LED_FLAG routing).

void overlayKeyPress(uint8_t row, uint8_t col) {
    if (s_overlayCloseAt) return;

    // Full-grid overlay INSTR: toute touche de note valide
    if (s_overlay == OVERLAY_INSTR) {
        if (row >= KBD_NOTE_ROWS) { s_overlay = OVERLAY_NONE; s_overlayCloseAt = 0; return; }
        uint8_t opt = (uint8_t)((3u - row) * 8u + (7u - col));
        if (opt < (uint8_t)SHAPE_COUNT) {
            audioAllNotesOff();  // release active voices before resetting oscillator pool
            currentShape = (SynthShape)opt;
            audioSetShape(currentShape);
        }
        s_overlayCloseAt = millis() + 200;
        return;
    }

    bool is4col = (s_overlay == OVERLAY_SCALE_ARP || s_overlay == OVERLAY_303 || s_overlay == OVERLAY_303_PRESET || s_overlay == OVERLAY_SYSEQ || s_overlay == OVERLAY_SEQB2);
    // FX overlay needs 3 columns (col5-7) when FX_COUNT > 8; others use 2 (col6-7) or 4 (col4-7)
    uint8_t colMin = is4col ? 4u : (s_overlay == OVERLAY_FX && FX_COUNT > 8 ? 5u : 6u);
    if (col < colMin) { s_overlay = OVERLAY_NONE; s_overlayCloseAt = 0; return; }

    // colRank: col7→0, col6→1, col5→2, col4→3
    uint8_t opt = (uint8_t)((3 - row) + (uint8_t)(7 - col) * 4);

    switch (s_overlay) {
        case OVERLAY_FX:
            if (opt < FX_COUNT) {
                fxSelected = opt;
                fxList[opt].active = !fxList[opt].active;
                if (fxList[opt].active) { fxOrderAdd(opt); fxPotNeedSync = true; }
                else fxOrderRemove(opt);
                if (opt == 6) {
                    if (!fxList[6].active) {
                        if (fxList[0].active) applyFxEffect(0);
                        else if (fxList[1].active) applyFxEffect(1);
                        else { audioSetAllFilters(0.0f, 1.0f); audioRestoreShapeFilter(currentShape); }
                    }
                } else {
                    applyFxEffect(opt);
                    if (opt == 0 && fxList[0].active) lpfSmoothCut = fxList[0].params[0];
                    // In 303 mode, restore 303's own reverb when FX reverb is disabled
                }
                Serial.printf("OVL FX[%d] %s\n", opt, fxList[opt].active?"ON":"OFF");
            }
            return;  // FX overlay stays open for multi-toggle (no close timer)
        case OVERLAY_SCALE_ARP:
            // col7 (opts 0-3): scales 0-3  |  col6 (opts 4-7): scales 4-5, empty 6-7
            // col5 (opts 8-11): arp Up/Dn/UD/Rnd (modes 1-4, toggle: re-click → off)
            // col4 (opts 12-15): octave -2/-1/0/+1
            if (opt < 4) { if (opt < (uint8_t)SCALE_COUNT) noteMap.setScale((Scale)opt); }
            else if (opt < 8) {
                if (opt == 6) { kbdLayout = (kbdLayout==KBD_LAYOUT_PIANO)?KBD_LAYOUT_GRID:KBD_LAYOUT_PIANO; }
                else { uint8_t si=opt-4; if (si+4 < (uint8_t)SCALE_COUNT) noteMap.setScale((Scale)(si+4)); }
            }
            else if (opt < 12) { uint8_t am=opt-7; arpMode=(arpMode==am)?0:am; }
            else if (opt < 16) noteMap.setOctave(kOctOpts[opt-12]);
            break;
        case OVERLAY_ENV:
            if (opt < ENV_PRESET_COUNT) {
                currentEnv = (EnvPreset)opt;
                audioSetEnvelope(envTable[currentEnv]);
                if (currentMode == MODE_303 && audioReady)
                    audioT303SetAmpEnv((float)envTable[currentEnv].atk,
                                       envTable[currentEnv].sus,
                                       (float)envTable[currentEnv].rel);
            } else if (opt >= 12 && opt < 16) {
                noteMap.setOctave(kOctOpts[opt-12]);
            }
            break;
        case OVERLAY_SEQ_OPT:
            if (opt < 3) {
                seqPlayMode = (SeqPlayMode)opt;
            } else if (opt == 3 && sdReady) {
                uint8_t mapped = 0;
                for (int fi = 0; fi < sdFileCount && mapped < 8; fi++) {
                    if (sdFiles[fi] == ".." || sdFileIsDir[fi]) continue;
                    if (!isAudioFile(sdFiles[fi].c_str())) continue;
                    char fp[256];
                    snprintf(fp, sizeof(fp), "%s%s%s",
                        sdPath.c_str(), sdPath.endsWith("/") ? "" : "/", sdFiles[fi].c_str());
                    seqPaths[mapped] = String(fp);
                    audioLoadKey(fp, SEQ_KEY_BASE + mapped);
                    mapped++;
                }
            } else if (opt == 4) {
                memset(seqPattern, 0, sizeof(seqPattern));
                seqStep = 0;
            }
            break;
        case OVERLAY_SAMP_OPT:
            if (opt < 4) {
                samplePlayMode = opt;
            } else if (opt == 4) {
                for (int r = 0; r < KBD_NOTE_ROWS; r++)
                    for (int c = 0; c < KBD_COLS; c++) sampleMap[r][c] = "";
                audioClearAllKeys();
            }
            break;
        case OVERLAY_303:
            // col7 0-3: SAW/SQR/Sld/Acc | col6 4-7: Oct-2/Oct-1/Oct0/Oct+1 | col5 8-11: Arp modes
            if      (opt == 0)               { t303Wave = SAW_DOWN; if(audioReady) audioT303Wave(SAW_DOWN); }
            else if (opt == 1)               { t303Wave = PULSE;    if(audioReady) audioT303Wave(PULSE); }
            else if (opt == 2)               { t303SlideOn = !t303SlideOn; if(!t303SlideOn){t303SlideActive=false; audioT303PitchBend(1.0f);} }
            else if (opt == 3)               { t303AccentOn = !t303AccentOn; }
            else if (opt >= 4 && opt <= 7)   { t303Oct = (int8_t)((int)opt - 6); }  // opt4→-2, opt5→-1, opt6→0, opt7→+1
            else if (opt >= 8 && opt <= 11)  { arpMode = (opt == 8) ? 0 : (int)(opt - 8); if(arpMode==0){arpNoteCount=0; arpCurrent=0;} }
            break;
        case OVERLAY_SYSEQ:
            if (opt < 8 && opt < (uint8_t)SHAPE_COUNT) {
                audioAllNotesOff();
                currentShape = (SynthShape)opt;
                audioSetShape(currentShape);
            } else if (opt >= 8 && opt < 12) {
                noteMap.setOctave(kOctOpts[opt-8]);
            } else if (opt >= 12 && opt < 16) {
                arpMode = (int)(opt - 12);
                if (arpMode == 0) { arpNoteCount = 0; arpCurrent = 0; }
            }
            break;
        case OVERLAY_303_PRESET:
            if (opt < T303_TONE_COUNT) {
                t303ToneIdx = opt;
                const T303Tone& tn = t303Tones[opt];
                t303Wave   = tn.amyWave;
                t303Cutoff = tn.cutoff;
                t303Reso   = tn.reso;
                t303EnvMod = tn.envMod;
                lp303[0] = lp303[1] = lp303[2] = lp303[3] = -1.0f;  // force pot pickup
                if (audioReady) {
                    audioT303Wave(t303Wave);
                    audioT303Params(t303Cutoff, t303Reso, t303EnvMod, t303Decay);
                }
            }
            break;
        case OVERLAY_SEQB2: {
            // bx[0-8]: FX toggles, bx[9-11]: octave (pitched modes), bx[12-15]: mode preset
            if (opt < FX_COUNT) {
                fxSelected = opt;
                fxList[opt].active = !fxList[opt].active;
                if (fxList[opt].active) { fxOrderAdd(opt); fxPotNeedSync = true; }
                else fxOrderRemove(opt);
                if (opt == 6) {
                    if (!fxList[6].active) {
                        if (fxList[0].active) applyFxEffect(0);
                        else if (fxList[1].active) applyFxEffect(1);
                        else { audioSetAllFilters(0.0f, 1.0f); audioRestoreShapeFilter(currentShape); }
                    }
                } else {
                    applyFxEffect(opt);
                    if (opt == 0 && fxList[0].active) lpfSmoothCut = fxList[0].params[0];
                }
                Serial.printf("SEQB2 FX[%d] %s\n", opt, fxList[opt].active?"ON":"OFF");
                s_overlayCloseAt = 0;  // keep overlay open after FX toggle
                return;
            } else if (opt >= 9 && opt < 12) {
                // Octave -2/-1/0/+1 (index maps: 9→kOctOpts[0], 10→kOctOpts[1], 11→kOctOpts[2])
                if (currentMode == MODE_SYSEQ)
                    noteMap.setOctave(kOctOpts[opt - 9]);
            } else if (opt >= 12 && opt < 16) {
                // Preset / waveform selection — mode-specific
                uint8_t presetIdx = opt - 12;
                if (currentMode == MODE_SYSEQ) {
                    // Shape selection (SHAPE 0-3 mapped on bx 12-15)
                    if (presetIdx < (uint8_t)SHAPE_COUNT) {
                        audioAllNotesOff();
                        currentShape = (SynthShape)presetIdx;
                        audioSetShape(currentShape);
                    }
                } else if (currentMode == MODE_SS2) {
                    if (presetIdx == 0) ss2PlayMode = 0;  // NRM
                    if (presetIdx == 1) ss2PlayMode = 1;  // OVR
                }
            }
            break;
        }
        default: break;
    }
    // Show selection feedback for 200ms before closing
    s_overlayCloseAt = millis() + 200;
}

// ==================== BUTTON HANDLER ====================
void handleButton(uint8_t rawBtn, bool pressed) {
    uint8_t btn = 3 - rawBtn;  // raw3=btn0(left), raw0=btn3(right)
    Serial.printf("BTN raw=%d mapped=%d %s\n", rawBtn, btn, pressed?"DN":"UP");

    if (btn==0) {
        if (pressed) { btn1PressTime=millis(); btn1Handled=false; }
        else {
            if (!btn1Handled && (millis()-btn1PressTime<600)) {
                if (menuOpen) menuOpen=false;
                else switch(currentMode) {
                    case MODE_SYNTH:
                    case MODE_303: {
                        OverlayType old = s_overlay; s_overlay = OVERLAY_NONE; s_overlayCloseAt = 0;
                        if (old != OVERLAY_FX) s_overlay = OVERLAY_FX;
                        break;
                    }
                    case MODE_MIDI:
#if CONFIG_TINYUSB_MIDI_ENABLED
                        if (midiPotSel >= 0) {
                            midiPotSel = -1;  // exit pot config mode
                        } else {
                            for (int n=0;n<128;n++) usbMIDI.noteOff((uint8_t)n, 0, midiChannel);
                            midiChannel = (midiChannel % 16) + 1;  // 1→2→...→16→1
                            midiLastPB = -999.f; usbMIDI.pitchBend((double)0.0, midiChannel);
                        }
#endif
                        break;
                    case MODE_DRUMS:  drumPlaying=!drumPlaying; drumStep=0; break;
                    case MODE_SEQ:   seqPlaying=!seqPlaying; if(!seqPlaying) seqStep=0; break;
                    case MODE_FX:
                        fxList[fxSelected].active = !fxList[fxSelected].active;
                        if (fxList[fxSelected].active) { fxOrderAdd(fxSelected); fxPotNeedSync = true; }
                        else                           fxOrderRemove(fxSelected);
                        if (fxSelected == 6) {
                            // LFO: applyFxEffect is a no-op; avoid blasting all config calls.
                            // If LFO just deactivated and LPF is also off → restore filter to neutral.
                            if (!fxList[6].active) {
                                if (fxList[0].active) applyFxEffect(0);
                                else if (fxList[1].active) applyFxEffect(1);
                                else { audioSetAllFilters(0.0f, 1.0f); audioRestoreShapeFilter(currentShape); }
                            }
                        } else {
                            // Only apply/reset the effect that just toggled — don't blast
                            // all other effects (would reset delay buffers, reverb tails, etc.)
                            applyFxEffect(fxSelected);
                            // Sync smooth tracker so the first 10ms tick starts from the right cutoff
                            if (fxSelected == 0 && fxList[0].active)
                                lpfSmoothCut = fxList[0].params[0];
                        }
                        Serial.printf("FX %s %s order=%d\n", fxList[fxSelected].name, fxList[fxSelected].active?"ON":"OFF", fxOrderCount);
                        break;
                    case MODE_SAMPLE:
                        // Back / go up one directory; at root → open main menu
                        if (sdPath == "/") { audioAllNotesOff(); menuOpen=true; menuRow=0; menuCol=0; }
                        else { int ls=sdPath.lastIndexOf('/',sdPath.length()-2); sdListDir(ls<=0?"/":sdPath.substring(0,ls+1)); }
                        break;
                    case MODE_TRACKER:
                        trkPlaying = !trkPlaying;
                        if (trkPlaying) {
                            trkStep=0; trkLastStepMs=millis();
                            // Play step 0 immediately so the first beat lands on the downbeat.
                            // (The advance loop increments before playing, so without this step 0
                            // would only play after a full 32-step cycle.)
                            for(int t=0;t<TRACKER_TRACKS;t++){
                                float vel=(trkNotes[t][0][0]>=0)?trkVel[t][0]/127.0f:0.7f;
                                if(vel<0.05f) vel=0.7f;
                                if(t<TRACKER_SYNTHS){
                                    for(int ci=0;ci<TRK_CHORD_SIZE;ci++){
                                        if(trkNotes[t][0][ci]<0) break;
                                        audioTrackerNoteOn((uint8_t)t,(uint8_t)trkNotes[t][0][ci],vel);
                                        trkPlayingNote[t][ci]=trkNotes[t][0][ci];
                                    }
                                } else if(t==TRACKER_DRUM_TRK){
                                    if(trkNotes[t][0][0]>=0) audioPlayDrumPad((uint8_t)trkNotes[t][0][0],vel);
                                } else {
                                    if(trkNotes[t][0][0]>=0){ uint8_t k=(uint8_t)(t-TRACKER_SAMP_BASE); audioPlayKey(k,vel); }
                                }
                            }
                        } else {
                            // Stop: send note-off for every tracked note
                            for(int t=0;t<TRACKER_TRACKS;t++){
                                if(t<TRACKER_SYNTHS){
                                    for(int ci=0;ci<TRK_CHORD_SIZE;ci++){
                                        if(trkPlayingNote[t][ci]>=0){
                                            audioTrackerNoteOff((uint8_t)t,(uint8_t)trkPlayingNote[t][ci]);
                                            trkPlayingNote[t][ci]=-1;
                                        }
                                    }
                                } else { trkPlayingNote[t][0]=-1; }
                            }
                        }
                        break;
                    case MODE_GRANULAR2: {
                        OverlayType old = s_overlay; s_overlay = OVERLAY_NONE; s_overlayCloseAt = 0;
                        if (old != OVERLAY_FX) s_overlay = OVERLAY_FX;
                        break;
                    }
                    case MODE_DRUM2:
                        drum2Playing = !drum2Playing;
                        if (drum2Playing) { drum2Step=0; drum2LastStepMs=millis(); }
                        else {
                            drum2RecArmed=false;
                            for(uint8_t i=0;i<syseqActiveCnt;i++) audioNoteOff(syseqActive[i]); syseqActiveCnt=0; syseqActiveIsFull=false;
                            memset(drum2DblPendingAt,0,sizeof(drum2DblPendingAt));
                            if (s303CurNote) { audioT303NoteOff(s303CurNote); s303CurNote=0; }
                            t303SlideActive=false; audioT303PitchBend(1.0f); ss2CurSlot=0xFF;
                        }
                        break;
                    case MODE_303S:
                        drum2Playing = !drum2Playing;
                        if (drum2Playing) { drum2Step=0; drum2LastStepMs=millis(); }
                        else {
                            if (s303CurNote) { audioT303NoteOff(s303CurNote); s303CurNote=0; }
                            audioT303PitchBend(1.0f); t303SlideActive=false;
                            memset(drum2DblPendingAt,0,sizeof(drum2DblPendingAt));
                        }
                        break;
                    case MODE_SS2:
                        drum2Playing = !drum2Playing;
                        if (drum2Playing) { drum2Step=0; drum2LastStepMs=millis(); ss2CurSlot=0xFF; }
                        else {
                            if (s303CurNote) { audioT303NoteOff(s303CurNote); s303CurNote=0; }
                            t303SlideActive=false; audioT303PitchBend(1.0f);
                            ss2CurSlot=0xFF; memset(drum2DblPendingAt,0,sizeof(drum2DblPendingAt));
                        }
                        break;
                    case MODE_SYSEQ:
                        drum2Playing = !drum2Playing;
                        if (drum2Playing) { drum2Step=0; drum2LastStepMs=millis(); }
                        else {
                            for(uint8_t i=0;i<syseqActiveCnt;i++) audioNoteOff(syseqActive[i]); syseqActiveCnt=0; syseqActiveIsFull=false;
                            memset(drum2DblPendingAt,0,sizeof(drum2DblPendingAt));
                            if (s303CurNote) { audioT303NoteOff(s303CurNote); s303CurNote=0; }
                            t303SlideActive=false; audioT303PitchBend(1.0f); ss2CurSlot=0xFF;
                        }
                        break;
                    default: break;
                }
            }
            btn1PressTime=0;
        }
        return;
    }
    if (!pressed && !(currentMode==MODE_SAMPLE&&(samplePlayMode==1||samplePlayMode==2)) && currentMode!=MODE_MIDI) return;

    switch(currentMode) {
        case MODE_SYNTH:
            if (btn==1) { OverlayType old=s_overlay; s_overlay=OVERLAY_NONE; s_overlayCloseAt=0; if(old!=OVERLAY_SCALE_ARP) s_overlay=OVERLAY_SCALE_ARP; }
            if (btn==2) { OverlayType old=s_overlay; s_overlay=OVERLAY_NONE; s_overlayCloseAt=0; if(old!=OVERLAY_ENV) s_overlay=OVERLAY_ENV; }
            if (btn==3) { OverlayType old=s_overlay; s_overlay=OVERLAY_NONE; s_overlayCloseAt=0; if(old!=OVERLAY_INSTR) s_overlay=OVERLAY_INSTR; }
            break;
        case MODE_OMNI:
            if (btn==1) fxSelected=(fxSelected+1)%FX_COUNT;
            if (btn==3) noteMap.nextOctave();
            break;
        case MODE_SAMPLE:
            if (btn==1) { OverlayType old=s_overlay; s_overlay=OVERLAY_NONE; s_overlayCloseAt=0; if(old!=OVERLAY_FX) s_overlay=OVERLAY_FX; }
            if (btn==2 && sdReady) {
                // Auto-map: assign every audio file in the current folder to keys.
                // Order: top-to-bottom, left-to-right on the display
                //   (r=3→0, and within each row c=7→0 since cx=(7-c)*16 puts c=7 on the left).
                for(int r=0;r<KBD_NOTE_ROWS;r++) for(int c=0;c<KBD_COLS;c++) sampleMap[r][c]="";
                audioClearAllKeys();
                uint8_t kidx=0;
                for(int fi=0; fi<sdFileCount && kidx<SAMPLE_KEY_COUNT; fi++){
                    if(sdFiles[fi]==".." || sdFileIsDir[fi]) continue;
                    if(!isAudioFile(sdFiles[fi].c_str())) continue;
                    char fpath[256];
                    snprintf(fpath,sizeof(fpath),"%s%s%s",
                        sdPath.c_str(),sdPath.endsWith("/")?"":"/",sdFiles[fi].c_str());
                    int r = KBD_NOTE_ROWS - 1 - (kidx / KBD_COLS);
                    int c = KBD_COLS     - 1 - (kidx % KBD_COLS);
                    sampleMap[r][c] = fpath;
                    audioLoadKey(fpath, (uint8_t)(r * KBD_COLS + c));
                    kidx++;
                }
            }
            if (btn==3) { OverlayType old=s_overlay; s_overlay=OVERLAY_NONE; s_overlayCloseAt=0; if(old!=OVERLAY_SAMP_OPT) s_overlay=OVERLAY_SAMP_OPT; }
            break;
        case MODE_DRUMS:
            if (btn==1) { drumPlaying=!drumPlaying; if(drumPlaying)drumStep=0; }
            if (btn==2) { memset(drumPattern,0,sizeof(drumPattern)); drumStep=0; }
            if (btn==3) drumBank=(drumBank+1)%3;
            break;
        case MODE_FX:
            // btn 1-3 free (toggle is on btn 0 short press)
            break;
        case MODE_SEQ:
            if (btn==1) { OverlayType old=s_overlay; s_overlay=OVERLAY_NONE; s_overlayCloseAt=0; if(old!=OVERLAY_FX) s_overlay=OVERLAY_FX; }
            if (btn==2) { OverlayType old=s_overlay; s_overlay=OVERLAY_NONE; s_overlayCloseAt=0; if(old!=OVERLAY_SEQ_OPT) s_overlay=OVERLAY_SEQ_OPT; }
            if (btn==3){
                // Toggle page (page 1 = tracks 1-4, page 2 = tracks 5-8)
                seqPage=1-seqPage;
                seqTrackSel=0;
            }
            break;
        case MODE_SYNTH2:
            if (btn==1 && s2PatchIdx>0) {
                s2PatchIdx--;
                amy_event e = amy_default_event();
                e.synth = SYNTH_CH; e.patch_number = s2PatchIdx;
                amy_add_event(&e);
            }
            if (btn==2 && s2PatchIdx<SYNTH2_PATCH_COUNT-1) {
                s2PatchIdx++;
                amy_event e = amy_default_event();
                e.synth = SYNTH_CH; e.patch_number = s2PatchIdx;
                amy_add_event(&e);
            }
            if (btn==3) noteMap.nextOctave();
            break;
        case MODE_303:
            if (btn==1) { OverlayType old=s_overlay; s_overlay=OVERLAY_NONE; s_overlayCloseAt=0; if(old!=OVERLAY_303)            s_overlay=OVERLAY_303;            }
            if (btn==2) { t303SustainOn = !t303SustainOn; if(audioReady) audioT303SetSustain(t303SustainOn ? 1.0f : 0.0f); }
            if (btn==3) { OverlayType old=s_overlay; s_overlay=OVERLAY_NONE; s_overlayCloseAt=0; if(old!=OVERLAY_303_PRESET)     s_overlay=OVERLAY_303_PRESET;     }
            break;
        case MODE_DRUM2:
            // B1(btn0)=Play handled in the btn==0 block above (must reach second switch via btn==1,2,3)
            if (btn==1) {  // B2: FX overlay
                OverlayType old=s_overlay; s_overlay=OVERLAY_NONE; s_overlayCloseAt=0;
                if (old!=OVERLAY_FX) s_overlay=OVERLAY_FX;
            }
            if (btn==2) {  // B3: Rec toggle
                drum2RecArmed = !drum2RecArmed;
            }
            if (btn==3) {  // B4: SEQ/PAD toggle
                drum2SeqView = !drum2SeqView;
            }
            break;
        case MODE_303S:
            // B1(btn0)=Play handled in btn==0 block above
            if (btn==1) {  // B2: FX overlay
                OverlayType old=s_overlay; s_overlay=OVERLAY_NONE; s_overlayCloseAt=0;
                if (old!=OVERLAY_FX) s_overlay=OVERLAY_FX;
            }
            if (btn==2) {  // B3: pending alt (SEQ) / accent toggle (PAD)
                if (s303SeqView) {
                    s303PendingAlt = (s303PendingAlt + 1) % 3;  // NRM→ACC→SLD→NRM
                } else {
                    t303AccentOn = !t303AccentOn;
                }
            }
            if (btn==3) {  // B4: SEQ/PAD toggle
                s303SeqView = !s303SeqView;
                if (!s303SeqView) {
                    for(uint8_t r2=0;r2<KBD_NOTE_ROWS;r2++)
                        for(uint8_t c2=0;c2<KBD_COLS;c2++)
                            if(activeNotes[r2][c2]){ audioT303NoteOff(activeNotes[r2][c2]); activeNotes[r2][c2]=0; }
                }
            }
            break;
        case MODE_SS2:
            // B1(btn0)=Play handled in btn==0 block above
            if (btn==1) {  // B2: FX overlay
                OverlayType old=s_overlay; s_overlay=OVERLAY_NONE; s_overlayCloseAt=0;
                if (old!=OVERLAY_FX) s_overlay=OVERLAY_FX;
            }
            if (btn==2) {  // B3: cycle alt for selected step (SEQ) / automap (PAD)
                if (ss2SeqView) {
                    ss2Alt[ss2SelStep] = (ss2Alt[ss2SelStep] + 1) % 4;  // NRM→REV→FUL→SIL
                } else if (sdReady) {
                    // Unload existing slots before remapping (free PSRAM)
                    audioClearAllKeys();
                    for(uint8_t i=0;i<SS2_SLOTS;i++) { ss2Path[i][0]='\0'; ss2Loaded[i]=false; }
                    uint8_t slotIdx = 0;
                    for (int fi = 0; fi < sdFileCount && slotIdx < SS2_SLOTS; fi++) {
                        if (!sdFileIsDir[fi] && isAudioFile(sdFiles[fi].c_str())) {
                            char fp[256];
                            snprintf(fp,sizeof(fp),"%s%s",sdPath.c_str(),sdFiles[fi].c_str());
                            strncpy(ss2Path[slotIdx],fp,sizeof(ss2Path[0])-1);
                            ss2Path[slotIdx][sizeof(ss2Path[0])-1]='\0';
                            audioLoadKey(fp,(uint8_t)(SS2_KEY_BASE+slotIdx));
                            slotIdx++;
                        }
                    }
                }
            }
            if (btn==3) {  // B4: toggle PAD/SEQ view
                ss2SeqView = !ss2SeqView;
            }
            break;
        case MODE_SYSEQ:
            // B1(btn0)=Play handled in btn==0 block above
            if (btn==1) {  // B2: FX overlay
                OverlayType old=s_overlay; s_overlay=OVERLAY_NONE; s_overlayCloseAt=0;
                if (old!=OVERLAY_FX) s_overlay=OVERLAY_FX;
            }
            if (btn==2) {
                if (syseqSeqView) {  // SEQ view: B3 cycles alt for selected step
                    static uint8_t s_syseqPendAlt = 0;
                    s_syseqPendAlt = (s_syseqPendAlt + 1) % 2;  // 0=NRM 1=FUL
                    syseqAlt[syseqSelStep] = s_syseqPendAlt;
                } else {
                    OverlayType old=s_overlay; s_overlay=OVERLAY_NONE; s_overlayCloseAt=0;
                    if(old!=OVERLAY_SYSEQ) s_overlay=OVERLAY_SYSEQ;
                }
            }
            if (btn==3) {
                for(uint8_t r2=0;r2<KBD_NOTE_ROWS;r2++)
                    for(uint8_t c2=0;c2<KBD_COLS;c2++)
                        if(activeNotes[r2][c2]){ audioNoteOff(activeNotes[r2][c2]); activeNotes[r2][c2]=0; }
                syseqSeqView = !syseqSeqView;
                if (syseqSeqView) {
                    // Compact non-empty steps to consecutive positions from step 0
                    uint8_t tmpN[SYSEQ_STEPS][SYSEQ_CHORD] = {};
                    uint8_t tmpV[SYSEQ_STEPS][SYSEQ_CHORD] = {};
                    uint8_t dst = 0;
                    for (uint8_t s = 0; s < SYSEQ_STEPS; s++) {
                        bool has = false;
                        for (uint8_t i = 0; i < SYSEQ_CHORD; i++) if (syseqNotes[s][i]) { has = true; break; }
                        if (has) {
                            memcpy(tmpN[dst], syseqNotes[s], sizeof(syseqNotes[0]));
                            memcpy(tmpV[dst], syseqVels[s],  sizeof(syseqVels[0]));
                            dst++;
                        }
                    }
                    memcpy(syseqNotes, tmpN, sizeof(syseqNotes));
                    memcpy(syseqVels,  tmpV,  sizeof(syseqVels));
                    syseqSelStep = 0;
                }
            }
            break;
        case MODE_MOD2:
            // Btn1=LFO mode (Off→Pt:SIN…Pt:S&H→Ft:SIN…Ft:S&H), Btn2=Poly/Mono, Btn3=Oct
            if (btn==1) mod2LfoMode=(mod2LfoMode+1)%MOD2_LFO_MODE_COUNT;
            if (btn==2) mod2PlayMode=(Mod2PlayMode)((mod2PlayMode+1)%MOD2_PLAY_COUNT);
            if (btn==3) noteMap.nextOctave();
            break;
        case MODE_MODULAR:
            if (btn==1){ modEnvIdx=(modEnvIdx+1)%ENV_PRESET_COUNT; audioSetEnvelope(envTable[modEnvIdx]); }
            if (btn==3) noteMap.nextOctave();
            break;
        case MODE_HYBRID:
            if (btn==3) noteMap.nextOctave();
            break;
        case MODE_GRANULAR:
            // Btn1: cycle sub-mode 0→1→2→3→0 (8sl / 1/16 / 8sl-loop / 1/16-loop)
            if (btn==1 && granComputed) {
                granSubMode = (granSubMode + 1) & 3u;
                audioSetGranularWindow(granWinStart, granWinEnd);
                granSliceCount = audioComputeGranularSlices(granSubMode & 1u, granWaveform);
                granComputed = (granSliceCount > 0);
                if (granComputed) {
                    for (int i = 0; i <= granSliceCount; i++) granSplits[i] = i / (float)granSliceCount;
                    granPlayingSlice = -1;
                    granPotNeedsSync = true;
                }
            }
            // Btn2: FX overlay
            if (btn==2) { OverlayType old=s_overlay; s_overlay=OVERLAY_NONE; s_overlayCloseAt=0; if(old!=OVERLAY_FX) s_overlay=OVERLAY_FX; }
            break;
        case MODE_GRANULAR2:
            // Btn2: cycle play mode once→loop→full→once
            if (btn==1) {
                gran2PlayMode = (gran2PlayMode + 1) % 3;
                gran2ActiveSample = -1; gran2ActiveSlice = -1; gran2PotNeedsSync = true;
                // Re-register presets with correct sample_length for the new play mode:
                // LOP needs extended length (SYNTH_OFF prevention); NRM needs exact slice length.
                for (uint8_t s = 0; s < GRAN2_MAX_SAMPLES; s++) {
                    if (gran2[s].computed)
                        audioApplyGranular2Splits(s, gran2[s].splits, gran2[s].sliceCount, gran2PlayMode == 1);
                }
            }
            // Btn3: cycle load target slot
            if (btn==2) gran2LoadTarget = (gran2LoadTarget + 1) % gran2NumSamples();
            // Btn4: clear all loaded GR2 samples to allow remapping
            if (btn==3) {
                for (uint8_t r = 0; r < KBD_NOTE_ROWS; r++) for (uint8_t c = 0; c < KBD_COLS; c++)
                    audioStopGranular2((uint8_t)(r * KBD_COLS + c));
                for (uint8_t s = 0; s < GRAN2_MAX_SAMPLES; s++) {
                    if (gran2[s].loaded) audioUnloadGranular2Slot(s);
                    gran2[s] = Gran2State{};
                }
                gran2ActiveSample = -1; gran2ActiveSlice = -1;
                gran2LoadTarget = 0; gran2PotNeedsSync = true;
            }
            break;
        case MODE_TRACKER:
            // B1 (btn==0 short press) = Play/Pause — handled in the btn==0 block above
            // B2: Rec toggle
            if (btn==1) trkRec = !trkRec;
            // B3: Cycle octave -2→-1→0→+1→+2→-2
            if (btn==2) trkOctave = (trkOctave >= 2) ? -2 : trkOctave + 1;
            // B4: Clear current track
            if (btn==3) {
                memset(trkNotes[trkInstr], -1, sizeof(trkNotes[trkInstr]));
                memset(trkVel  [trkInstr],  0, sizeof(trkVel  [trkInstr]));
                memset(trkPlayingNote[trkInstr], -1, sizeof(trkPlayingNote[trkInstr]));
            }
            break;
        case MODE_MIDI:
#if CONFIG_TINYUSB_MIDI_ENABLED
            if (midiPotSel >= 0) {
                // Config mode: 8 rows (0-6=pot CC, 7=layout)
                if (btn==2 && !pressed) midiPotSel = (int8_t)((midiPotSel + 7) % 8);  // prev
                if (btn==3 && !pressed) midiPotSel = (int8_t)((midiPotSel + 1) % 8);  // next
                if (midiPotSel < 7) {
                    if (btn==1 && !pressed) {
                        uint8_t cc = midiPotCC[midiPotSel];
                        midiPotCC[midiPotSel] = (cc == 0) ? 127 : cc - 1;
                        midiLpPots[midiPotSel] = -1.f;
                    }
                    if (btn==4 && !pressed) {
                        midiPotCC[midiPotSel] = (midiPotCC[midiPotSel] >= 127) ? 0 : midiPotCC[midiPotSel] + 1;
                        midiLpPots[midiPotSel] = -1.f;
                    }
                } else {
                    // Row 7 = Layout toggle
                    if ((btn==1 || btn==4) && !pressed)
                        kbdLayout = (kbdLayout==KBD_LAYOUT_PIANO)?KBD_LAYOUT_GRID:KBD_LAYOUT_PIANO;
                }
            } else {
                // Normal MIDI play mode
                if (btn==1) usbMIDI.controlChange(64, pressed ? 127 : 0, midiChannel);
                if (btn==2 && !pressed && midiOctave>0) { midiOctave--; for(int n=0;n<128;n++) usbMIDI.noteOff((uint8_t)n,0,midiChannel); }
                if (btn==3 && !pressed && midiOctave<9) { midiOctave++; for(int n=0;n<128;n++) usbMIDI.noteOff((uint8_t)n,0,midiChannel); }
                if (btn==4 && pressed) { for (int n=0;n<128;n++) usbMIDI.noteOff((uint8_t)n,0,midiChannel); }
            }
#endif
            break;
        default: break;
    }
}

// ==================== ENCODER READING ====================
void readPots() {
    for (int i=0;i<7;i++) {
        float x=(float)muxCache[2+i*2]-2048.0f, y=(float)muxCache[3+i*2]-2048.0f;
        if (sqrtf(x*x+y*y)<80.0f) continue;
        float angle=atan2f(y,x);
        if (!pots[i].init){pots[i].prevAngle=angle;pots[i].init=true;continue;}
        float delta=angle-pots[i].prevAngle;
        if (delta>PI) delta-=2*PI; if (delta<-PI) delta+=2*PI;
        float sign=(i<=1)?1.0f:-1.0f;
        pots[i].accum+=sign*delta*50.0f/PI;
        pots[i].prevAngle=angle;
    }
    for (int i=0;i<7;i++) {
        if (fabsf(pots[i].accum)<0.5f) continue;
        float pMax = (i == 0) ? 2.0f : 1.0f;  // pot0 = volume, goes to 200%
        pots[i].value=constrain(pots[i].value+pots[i].accum*0.01f,0.0f,pMax);
        pots[i].accum=0;
    }
}

// ==================== DISPLAY ====================
// Compact float for 4x6 font: ≤5 chars ("8.0k", "300", "15.5", "0.50")
static void fmtFloat(char* dst, size_t sz, float v) {
    float av = fabsf(v);
    if      (av >= 1000.0f) snprintf(dst, sz, "%.1fk", v/1000.0f);
    else if (av >= 100.0f)  snprintf(dst, sz, "%.0f",  v);
    else if (av >= 10.0f)   snprintf(dst, sz, "%.1f",  v);
    else                    snprintf(dst, sz, "%.2f",  v);
}

void drawScreen() {
    oled.clearBuffer();

    // ---- FULL-SCREEN OVERLAY ----
    // INSTR: grille 8×4 complète (16×32px par case), toutes les 32 touches de notes
    if (s_overlay == OVERLAY_INSTR) {
        for (int i = 0; i < min((int)SHAPE_COUNT, 32); i++) {
            int ci = i % 8, ri = i / 8;   // row-major: top row = shapes 0-7
            int x = ci * 16, y = ri * 32;
            bool sel = (currentShape == (SynthShape)i);
            if (sel) { oled.drawBox(x, y, 16, 32); oled.setDrawColor(0); }
            else oled.drawFrame(x, y, 16, 32);
            oled.setFont(u8g2_font_4x6_tf);
            char nm[5]; strncpy(nm, shapeNames[i], 4); nm[4] = '\0';
            oled.drawStr(x + 1, y + 20, nm);
            if (sel) oled.setDrawColor(1);
        }
        oled.sendBuffer();
        return;
    }

    // 2-col overlays (FX, ENV, SEQ_OPT, SAMP_OPT): 2×4 grid, 64×32px par case
    // 4-col overlays (SCALE_ARP, 303, 303_PRESET):  4×4 grid, 32×32px par case
    if (s_overlay != OVERLAY_NONE) {
        struct OvBox { char l1[12]; char l2[14]; char l3[14]; bool avail, sel; };
        bool is4col = (s_overlay == OVERLAY_SCALE_ARP || s_overlay == OVERLAY_303 || s_overlay == OVERLAY_303_PRESET || s_overlay == OVERLAY_SYSEQ || s_overlay == OVERLAY_SEQB2);
        OvBox bx[16];
        memset(bx, 0, sizeof(bx));

        switch (s_overlay) {
            case OVERLAY_FX:
                for (int i = 0; i < (int)FX_COUNT; i++) {
                    OvBox &b = bx[i]; b.avail = true; b.sel = fxList[i].active;
                    strncpy(b.l1, fxList[i].name, sizeof(b.l1)-1);
                    const FxEffect &fx = fxList[i];
                    // Collect indices of visible params (non-empty name)
                    int vp[4]; int vpc = 0;
                    for (int p = 0; p < 4; p++) if (fx.paramNames[p][0]) vp[vpc++] = p;
                    if (vpc == 0) continue;
                    // Special case: Delay shows level + BPM-synced time hint
                    if (i == 5) {
                        char v[8]; fmtFloat(v, sizeof(v), fx.params[vp[0]]);
                        snprintf(b.l2, sizeof(b.l2), "%s:%s", fx.paramNames[vp[0]], v);
                        int dms = (int)(60000.0f / (float)bpm * kDelaySubdiv[4]);
                        snprintf(b.l3, sizeof(b.l3), "1/4=%dms", dms);
                        continue;
                    }
                    // Special case: ResEcho — compact 2-line layout for narrow 3rd column
                    if (i == 8) {
                        char v[8]; fmtFloat(v, sizeof(v), fx.params[0]);
                        snprintf(b.l2, sizeof(b.l2), "Lv:%s", v);
                        fmtFloat(v, sizeof(v), fx.params[1]);
                        snprintf(b.l3, sizeof(b.l3), "FB:%s", v);
                        continue;
                    }
                    if (vpc <= 2) {
                        // 1-2 params: one per line (standard format)
                        char v[8]; fmtFloat(v, sizeof(v), fx.params[vp[0]]);
                        snprintf(b.l2, sizeof(b.l2), "%s:%s", fx.paramNames[vp[0]], v);
                        if (vpc == 2) {
                            fmtFloat(v, sizeof(v), fx.params[vp[1]]);
                            snprintf(b.l3, sizeof(b.l3), "%s:%s", fx.paramNames[vp[1]], v);
                        }
                    } else {
                        // 3-4 params: pack 2 per line using 2-char abbreviated names
                        char v1[8], v2[8];
                        fmtFloat(v1, sizeof(v1), fx.params[vp[0]]);
                        fmtFloat(v2, sizeof(v2), fx.params[vp[1]]);
                        snprintf(b.l2, sizeof(b.l2), "%.2s:%s %.2s:%s",
                                 fx.paramNames[vp[0]], v1, fx.paramNames[vp[1]], v2);
                        fmtFloat(v1, sizeof(v1), fx.params[vp[2]]);
                        if (vpc == 3) {
                            snprintf(b.l3, sizeof(b.l3), "%.2s:%s", fx.paramNames[vp[2]], v1);
                        } else {
                            fmtFloat(v2, sizeof(v2), fx.params[vp[3]]);
                            snprintf(b.l3, sizeof(b.l3), "%.2s:%s %.2s:%s",
                                     fx.paramNames[vp[2]], v1, fx.paramNames[vp[3]], v2);
                        }
                    }
                }
                break;
            case OVERLAY_SCALE_ARP: {
                // col7 opts 0-3: gammes 0-3  |  col6 opts 4-5: gammes 4-5, 6-7 vides
                // col5 opts 8-11: arp Up/Dn/UD/Rnd  |  col4 opts 12-15: octave
                static const char* sn[] = {"Chro","Maj","Min","Pent","Blue","Hm"};
                static const char* an[] = {"Up","Dn","UD","Rnd"};
                static const char* ol[] = {"-2","-1"," 0","+1"};
                Scale cs = noteMap.getScale();
                for (int i = 0; i < (int)SCALE_COUNT; i++) {
                    bx[i].avail = true; strncpy(bx[i].l1, sn[i], sizeof(bx[i].l1)-1);
                    bx[i].sel = (cs == (Scale)i);
                }
                bx[6].avail = true; strncpy(bx[6].l1, "Pno", sizeof(bx[6].l1)-1);
                bx[6].sel = (kbdLayout == KBD_LAYOUT_PIANO);
                for (int i = 0; i < 4; i++) {
                    bx[8+i].avail = true; strncpy(bx[8+i].l1, an[i], sizeof(bx[8+i].l1)-1);
                    bx[8+i].sel = (arpMode == i + 1);
                }
                for (int i = 0; i < 4; i++) {
                    bx[12+i].avail = true; strncpy(bx[12+i].l1, ol[i], sizeof(bx[12+i].l1)-1);
                    bx[12+i].sel = (noteMap.getOctave() == kOctOpts[i]);
                }
                break;
            }
            case OVERLAY_ENV: {
                for (int i = 0; i < (int)ENV_PRESET_COUNT; i++) {
                    bx[i].avail = true; bx[i].sel = (currentEnv == (EnvPreset)i);
                    strncpy(bx[i].l1, envNames[i], sizeof(bx[i].l1)-1);
                    snprintf(bx[i].l2, sizeof(bx[i].l2), "A:%d D:%d", envTable[i].atk, envTable[i].dec);
                    snprintf(bx[i].l3, sizeof(bx[i].l3), "S:%d%% R:%d",
                             (int)(envTable[i].sus * 100), envTable[i].rel);
                }
                static const char* ol[] = {"-2","-1"," 0","+1"};
                for (int i = 0; i < 4; i++) {
                    bx[12+i].avail = true;
                    strncpy(bx[12+i].l1, ol[i], sizeof(bx[12+i].l1)-1);
                    bx[12+i].sel = (noteMap.getOctave() == kOctOpts[i]);
                }
                break;
            }
            case OVERLAY_INSTR: {
                uint8_t maxS = min((uint8_t)16, (uint8_t)SHAPE_COUNT);
                for (uint8_t i = 0; i < maxS; i++) {
                    bx[i].avail = true;
                    strncpy(bx[i].l1, shapeNames[i], sizeof(bx[i].l1)-1);
                    bx[i].sel = (currentShape == (SynthShape)i);
                }
                break;
            }
            case OVERLAY_SEQ_OPT: {
                static const char* sd[] = {"loop","1-shot","cut nt"};
                for (int i = 0; i < 3; i++) {
                    bx[i].avail = true; bx[i].sel = ((uint8_t)seqPlayMode == i);
                    strncpy(bx[i].l1, kSeqPlayModes[i], sizeof(bx[i].l1)-1);
                    strncpy(bx[i].l2, sd[i], sizeof(bx[i].l2)-1);
                }
                bx[3].avail = true; strncpy(bx[3].l1, "Map", sizeof(bx[3].l1)-1);
                strncpy(bx[3].l2, "automap", sizeof(bx[3].l2)-1);
                bx[4].avail = true; strncpy(bx[4].l1, "Clr", sizeof(bx[4].l1)-1);
                strncpy(bx[4].l2, "pattern", sizeof(bx[4].l2)-1);
                break;
            }
            case OVERLAY_SAMP_OPT: {
                static const char* sd[] = {"normal","static","loop","slow"};
                for (int i = 0; i < 4; i++) {
                    bx[i].avail = true; bx[i].sel = ((uint8_t)samplePlayMode == i);
                    strncpy(bx[i].l1, kSplayModes[i], sizeof(bx[i].l1)-1);
                    strncpy(bx[i].l2, sd[i], sizeof(bx[i].l2)-1);
                }
                bx[4].avail = true; strncpy(bx[4].l1, "Clr", sizeof(bx[4].l1)-1);
                strncpy(bx[4].l2, "samples", sizeof(bx[4].l2)-1);
                break;
            }
            case OVERLAY_303: {
                // col7 0-3: SAW/SQR/Sld/Acc | col6 4-7: Sus/Oct | col5 8-11: Arp
                bx[0].avail=true; strncpy(bx[0].l1,"SAW",sizeof(bx[0].l1)-1); bx[0].sel=(t303Wave==SAW_DOWN);
                bx[1].avail=true; strncpy(bx[1].l1,"SQR",sizeof(bx[1].l1)-1); bx[1].sel=(t303Wave==PULSE);
                bx[2].avail=true; strncpy(bx[2].l1,"Sld",sizeof(bx[2].l1)-1); bx[2].sel=t303SlideOn;
                bx[3].avail=true; strncpy(bx[3].l1,"Acc",sizeof(bx[3].l1)-1); bx[3].sel=t303AccentOn;
                static const char* on303[]={"Oct-2","Oct-1","Oct 0","Oct+1"};
                for (int i=0;i<4;i++){
                    bx[4+i].avail=true;
                    strncpy(bx[4+i].l1,on303[i],sizeof(bx[4+i].l1)-1);
                    bx[4+i].sel=(t303Oct==(int8_t)(i-2));
                }
                static const char* arpN[]={"Off","Up","Dn","Rnd"};
                for (int i=0;i<4;i++){
                    bx[8+i].avail=true;
                    strncpy(bx[8+i].l1,arpN[i],sizeof(bx[8+i].l1)-1);
                    bx[8+i].sel=(arpMode==i);
                }
                break;
            }
            case OVERLAY_303_PRESET: {
                for (int i=0;i<T303_TONE_COUNT;i++){
                    bx[i].avail=true;
                    strncpy(bx[i].l1, t303Tones[i].name, sizeof(bx[i].l1)-1);
                    bx[i].sel=(t303ToneIdx==(uint8_t)i);
                }
                break;
            }
            case OVERLAY_SYSEQ: {
                // col7 (bx 0-3): shapes 0-3 | col6 (bx 4-7): shapes 4-7
                // col5 (bx 8-11): octave -2/-1/0/+1 | col4 (bx 12-15): arp Off/Up/Dn/Rnd
                for (int i = 0; i < 8 && i < (int)SHAPE_COUNT; i++) {
                    bx[i].avail = true;
                    strncpy(bx[i].l1, shapeNames[i], sizeof(bx[i].l1)-1);
                    bx[i].sel = (currentShape == (SynthShape)i);
                }
                static const char* octN[] = {"-2","-1"," 0","+1"};
                for (int i = 0; i < 4; i++) {
                    bx[8+i].avail = true;
                    strncpy(bx[8+i].l1, octN[i], sizeof(bx[8+i].l1)-1);
                    bx[8+i].sel = (noteMap.getOctave() == kOctOpts[i]);
                }
                static const char* arpN[] = {"Off","Up","Dn","Rnd"};
                for (int i = 0; i < 4; i++) {
                    bx[12+i].avail = true;
                    strncpy(bx[12+i].l1, arpN[i], sizeof(bx[12+i].l1)-1);
                    bx[12+i].sel = (arpMode == i);
                }
                break;
            }
            case OVERLAY_SEQB2: {
                // col7 (bx 0-3): FX 0-3   col6 (bx 4-7): FX 4-7
                // col5 (bx 8-11): FX 8 + octave (bx9-11)
                // col4 (bx 12-15): mode preset (shape/wave)
                static const char* kFiltTypAbbr[] = {"LPF","LaF","HPF","BPF"};
                for (int i = 0; i < (int)FX_COUNT; i++) {
                    bx[i].avail = true;
                    bx[i].sel   = fxList[i].active;
                    if (i == 0 && fxList[0].active) {
                        uint8_t ti = (uint8_t)constrain((int)(fxList[0].params[2] * 3.999f), 0, 3);
                        strncpy(bx[i].l1, kFiltTypAbbr[ti], sizeof(bx[i].l1)-1);
                    } else {
                        char a[4]; strncpy(a, fxList[i].name, 3); a[3]='\0';
                        strncpy(bx[i].l1, a, sizeof(bx[i].l1)-1);
                    }
                }
                // bx9-11: octave for pitched modes (not 303 — octave via joystick)
                bool hasPitch = (currentMode == MODE_SYSEQ);
                static const char* octNb[] = {"-2","-1"," 0","+1"};
                for (int i = 0; i < 3; i++) {
                    if (hasPitch) {
                        bx[9+i].avail = true;
                        strncpy(bx[9+i].l1, octNb[i+1], sizeof(bx[9+i].l1)-1);  // -1/0/+1
                        bx[9+i].sel = (noteMap.getOctave() == kOctOpts[i+1]);
                    }
                }
                // bx12-15: mode preset
                if (currentMode == MODE_SYSEQ) {
                    static const char* shapeAbbr[] = {"SIN","PLS","SWD","SWU"};
                    for (int i = 0; i < 4 && i < (int)SHAPE_COUNT; i++) {
                        bx[12+i].avail = true;
                        strncpy(bx[12+i].l1, shapeAbbr[i], sizeof(bx[12+i].l1)-1);
                        bx[12+i].sel = (currentShape == (SynthShape)i);
                    }
                } else if (currentMode == MODE_SS2) {
                    bx[12].avail=true; strncpy(bx[12].l1,"NRM",sizeof(bx[12].l1)-1); bx[12].sel=(ss2PlayMode==0);
                    bx[13].avail=true; strncpy(bx[13].l1,"OVR",sizeof(bx[13].l1)-1); bx[13].sel=(ss2PlayMode==1);
                }
                break;
            }
            default: break;
        }

        // Render: 4-col → 32px, 3-col → 42px, 2-col → 64px cells
        int ncols = is4col ? 4 : (s_overlay == OVERLAY_FX && FX_COUNT > 8 ? 3 : 2);  // SEQB2 is 4-col via is4col
        int cw    = 128 / ncols;
        for (int i = 0; i < ncols * 4; i++) {
            int gc = i / 4, gr = i % 4;
            int x = gc * cw, y = gr * 32;
            // Clip each cell so text never bleeds into adjacent cells
            oled.setClipWindow(x, y, x + cw - 1, y + 31);
            if (bx[i].sel) {
                oled.drawBox(x, y, cw, 32);
                oled.setDrawColor(0);
            } else if (bx[i].avail) {
                oled.drawFrame(x, y, cw, 32);
            }
            if (bx[i].avail) {
                if (is4col) {
                    oled.setFont(u8g2_font_4x6_tf);
                    oled.drawStr(x + 2, y + 8, bx[i].l1);
                    if (bx[i].l2[0]) oled.drawStr(x + 2, y + 16, bx[i].l2);
                    if (bx[i].l3[0]) oled.drawStr(x + 2, y + 24, bx[i].l3);
                } else {
                    oled.setFont(u8g2_font_5x7_tf);
                    oled.drawStr(x + 3, y + 10, bx[i].l1);
                    oled.setFont(u8g2_font_4x6_tf);
                    if (bx[i].l2[0]) oled.drawStr(x + 3, y + 19, bx[i].l2);
                    if (bx[i].l3[0]) oled.drawStr(x + 3, y + 27, bx[i].l3);
                }
                oled.setDrawColor(1);
            }
            oled.setMaxClipWindow();
        }
        oled.sendBuffer();
        return;
    }

    oled.setFont(u8g2_font_5x7_tf);
    char buf[40];

    if (menuOpen) {
        oled.drawStr(40,0,"MENU"); oled.drawHLine(0,9,128);
        const uint8_t CW=42,CH=28,MY=14,visRows=4;
        uint8_t scroll=(menuRow>=visRows)?menuRow-visRows+1:0;
        for (uint8_t vr=0;vr<visRows;vr++) {
            uint8_t r=scroll+vr; if(r>=MENU_ROWS) break;
            for (uint8_t c=0;c<MENU_COLS;c++) {
                uint8_t idx=r*MENU_COLS+c; if(idx>=MENU_ITEM_COUNT) continue;
                uint8_t x=c*CW+1,y=MY+vr*CH;
                bool cur=(r==menuRow&&c==menuCol);
                if(cur){oled.drawRFrame(x,y,CW-2,CH-2,3);oled.drawFrame(x+2,y+2,CW-6,CH-6);}
                else oled.drawFrame(x,y,CW-2,CH-2);
                int tw=oled.getStrWidth(menuLabels[idx]);
                oled.drawStr(x+(CW-2-tw)/2,y+(CH-2)/2,menuLabels[idx]);
            }
        }
    } else {
        float battV=muxCache[1]/4095.0f*10.4f;
        snprintf(buf,sizeof(buf),"%.1fV",battV);
        oled.drawStr(108,0,buf); oled.drawHLine(0,9,128);

        switch(currentMode) {
            // ---- SYNTH ----
            case MODE_SYNTH: {
                static const char* arpNames[]={"Off","Up","Dn","UD","Rnd"};
                oled.drawStr(0,0,"SYNTH");
                snprintf(buf,sizeof(buf),"Shape: %s", shapeNames[currentShape]);
                oled.drawStr(0,16,buf);
                if(currentShape==SHAPE_SAW_FM){
                    snprintf(buf,sizeof(buf),"FM Depth: %.0f%%", pots[3].value*100);
                    oled.drawStr(0,28,buf);
                    float fmCut=500.0f*powf(16.0f,pots[4].value);
                    snprintf(buf,sizeof(buf),"Cut:%.0fHz", fmCut);
                    oled.drawStr(0,39,buf);
                } else {
                    snprintf(buf,sizeof(buf),"Env:   %s", envNames[currentEnv]);
                    oled.drawStr(0,28,buf);
                    snprintf(buf,sizeof(buf)," A:%d D:%d S:%.0f%% R:%d",
                             envTable[currentEnv].atk, envTable[currentEnv].dec,
                             envTable[currentEnv].sus*100, envTable[currentEnv].rel);
                    oled.drawStr(0,39,buf);
                }
                snprintf(buf,sizeof(buf),"Arp:%-4s %dBPM", arpNames[arpMode%5], bpm);
                oled.drawStr(0,50,buf);
                snprintf(buf,sizeof(buf),"Oct:   %+d  Scale: %s",
                         noteMap.getOctave(), scaleName(noteMap.getScale()));
                oled.drawStr(0,62,buf);
                snprintf(buf,sizeof(buf),"Vol:%.0f%% Shp:%.0f%% Env:%.0f%%",
                         pots[0].value*100, pots[1].value*100, pots[2].value*100);
                oled.drawStr(0,80,buf);
                // Show last selected FX + its first 2 params if active
                {
                    const FxEffect &fx=fxList[fxSelected];
                    if(fx.active){
                        snprintf(buf,sizeof(buf),"FX:%s %s:%.1f %s:%.1f",
                                 fx.name,
                                 fx.paramNames[0],fx.params[0],
                                 fx.paramNames[1][0]?fx.paramNames[1]:"",
                                 fx.paramNames[1][0]?fx.params[1]:0.0f);
                    } else {
                        snprintf(buf,sizeof(buf),"FX:%s [OFF]",fx.name);
                    }
                    oled.drawStr(0,92,buf);
                }
                oled.drawStr(0,122,"FX  Scl/Arp  Env   Oct");
                break;
            }
            // ---- OMNI (circle of fifths: Eb Bb F C G D A E B × Maj/min/7th) ----
            case MODE_OMNI: {
                // Header: title + active chord right-aligned (5x7 font)
                oled.drawStr(0, 0, "OMNI");
                if(omniRoot!=0xFF&&omniRoot<9&&omniType<3){
                    snprintf(buf,sizeof(buf),"%s%s",omniNoteNames[omniRoot],omniTypeNames[omniType]);
                    oled.drawStr(128-oled.getStrWidth(buf)-1, 0, buf);
                }
                oled.drawHLine(0, 9, 128);
                oled.setFont(u8g2_font_4x6_tf);

                // Line 1 (y=14): Vol / Shape / Oct
                snprintf(buf, sizeof(buf), "V:%.0f%% %s Oct:%+d",
                         pots[0].value*100.0f, shapeNames[currentShape], noteMap.getOctave());
                oled.drawStr(0, 14, buf);

                // FX strip (y=16..23): 6 slots across 128px
                // Selected → filled box + inverted text. Active (not sel) → frame. Inactive → plain text.
                {
                    const int slotW = 128 / (int)FX_COUNT;  // =21
                    for(int i=0;i<(int)FX_COUNT;i++){
                        int x=i*slotW;
                        int w=(i==(int)FX_COUNT-1)?(128-x):(slotW-1);
                        bool active=fxList[i].active;
                        bool sel=(i==(int)fxSelected);
                        char nm[4]; strncpy(nm, fxList[i].name, 3); nm[3]='\0';
                        int tw=oled.getStrWidth(nm);
                        if(sel){
                            oled.drawRBox(x,16,w,8,1);
                            oled.setDrawColor(0);
                            oled.drawStr(x+(w-tw)/2, 22, nm);
                            oled.setDrawColor(1);
                        } else {
                            if(active) oled.drawFrame(x,16,w,8);
                            oled.drawStr(x+(w-tw)/2, 22, nm);
                        }
                    }
                }

                // Line 3 (y=30): params of selected FX
                {
                    const FxEffect& af=fxList[fxSelected];
                    if(af.active){
                        char line[36]="";
                        int pc=0;
                        for(int p=0;p<4;p++){
                            if(!af.paramNames[p][0]) continue;
                            char fv[6]; fmtFloat(fv, sizeof(fv), af.params[p]);
                            char ent[14];
                            snprintf(ent,sizeof(ent),"%s%.3s:%s",pc?" ":"",af.paramNames[p],fv);
                            strncat(line,ent,sizeof(line)-strlen(line)-1);
                            pc++;
                        }
                        oled.drawStr(0,30,line);
                    } else {
                        oled.drawStr(0,30,"off  (Btn1=sel FX)");
                    }
                }

                // Chord grid: 4 rows × 8 cols, cells 15×23px, y starts at 33
                // Row 3(top)=Maj(ct=0), row 2=min(ct=1), row 1=7th(ct=2). Col 0(right)=E, col 7(left)=Eb.
                for(int r=1;r<=3;r++){
                    uint8_t ct=(uint8_t)(3-r);
                    for(int c=0;c<8;c++){
                        uint8_t ni=(uint8_t)(7-c);
                        int cx=1+(7-c)*16, cy=33+(3-r)*23;
                        bool act=(omniRoot==ni&&omniType==ct);
                        if(act){oled.drawRBox(cx,cy,15,23,2);oled.setDrawColor(0);}
                        else oled.drawFrame(cx,cy,15,23);
                        char lbl[5]; snprintf(lbl,sizeof(lbl),"%s%s",omniNoteNames[ni],omniTypeNames[ct]);
                        int sw=oled.getStrWidth(lbl);
                        oled.drawStr(cx+(15-sw)/2,cy+14,lbl);
                        if(act) oled.setDrawColor(1);
                    }
                }
                // Row 0 (bottom): B chords at cols 0(right)=BM, 1=Bm, 2=B7
                for(int c=0;c<3;c++){
                    uint8_t ct=(uint8_t)c;
                    int cx=1+(7-c)*16, cy=33+3*23;
                    bool act=(omniRoot==8&&omniType==ct);
                    if(act){oled.drawRBox(cx,cy,15,23,2);oled.setDrawColor(0);}
                    else oled.drawFrame(cx,cy,15,23);
                    char lbl[5]; snprintf(lbl,sizeof(lbl),"B%s",omniTypeNames[ct]);
                    int sw=oled.getStrWidth(lbl);
                    oled.drawStr(cx+(15-sw)/2,cy+14,lbl);
                    if(act) oled.setDrawColor(1);
                }
                oled.setFont(u8g2_font_5x7_tf);
                break;
            }
            // ---- DRUMS ----
            case MODE_DRUMS: {
                oled.drawStr(0,0,"DRUMS");
                oled.drawHLine(0,9,128);
                // 4-row × 8-col pad grid. padIdx = row*8+col (row0=bottom, col0=right).
                // Display: col0(right) → screen x=112, col7(left) → x=0.
                //          row0(bottom) → screen y=89, row3(top) → y=11.
                oled.setFont(u8g2_font_4x6_tf);
                for(int r=0;r<4;r++) for(int c=0;c<8;c++){
                    uint8_t padIdx=(uint8_t)(r*8+c);
                    int cx=(7-c)*16;
                    int cy=11+(3-r)*26;
                    bool hit=keyState[r][c];
                    if(hit){oled.drawBox(cx,cy,15,25);oled.setDrawColor(0);}
                    else oled.drawFrame(cx,cy,15,25);
                    const char* lbl=audioDrumPadLabel(padIdx);
                    int sw=oled.getStrWidth(lbl);
                    oled.drawStr(cx+(15-sw)/2,cy+14,lbl);
                    if(hit) oled.setDrawColor(1);
                }
                oled.setFont(u8g2_font_5x7_tf);
                snprintf(buf,sizeof(buf),"Vol:%.0f%%",pots[0].value*100);
                oled.drawStr(0,122,buf);
                break;
            }
            // ---- SAMPLE ----
            case MODE_SAMPLE: {
                // Same 4x6 font + 7px spacing as GR2 browser
                oled.setFont(u8g2_font_4x6_tf);
                {
                    char pb[32];
                    snprintf(pb, sizeof(pb), "SAMPLE %.24s", sdPath.c_str());
                    oled.drawStr(0, 7, pb);
                }
                oled.drawHLine(0, 9, 128);
                if(!sdReady){
                    oled.drawStr(10, 40, "SD not found");
                    oled.drawStr(10, 55, "Click = retry");
                } else {
                    // File list — 10 items at 7px spacing (matches GR2 browse layout)
                    for(int i = 0; i < 10 && (i + sdScroll) < sdFileCount; i++){
                        int idx = i + sdScroll; bool sel = (idx == sdCursor);
                        bool mapped = false;
                        if(!sdFileIsDir[idx]){
                            char fpath[80];
                            snprintf(fpath, sizeof(fpath), "%s%s%s",
                                sdPath.c_str(), sdPath.endsWith("/") ? "" : "/", sdFiles[idx].c_str());
                            for(int r = 0; r < KBD_NOTE_ROWS && !mapped; r++)
                                for(int c = 0; c < KBD_COLS && !mapped; c++)
                                    if(sampleMap[r][c] == fpath) mapped = true;
                        }
                        char line[29];
                        bool isWav = !sdFileIsDir[idx] && isWavFile(sdFiles[idx].c_str());
                        if(sdFileIsDir[idx]) snprintf(line, sizeof(line), "%c[%.24s]", sel ? '>' : ' ', sdFiles[idx].c_str());
                        else snprintf(line, sizeof(line), "%c%s%s%.25s", sel ? '>' : ' ', mapped ? "*" : "", isWav ? "" : "~", sdFiles[idx].c_str());
                        oled.drawStr(0, 17 + i * 7, line);
                        if(sel) oled.drawHLine(0, 18 + i * 7, 128);
                    }
                    // Mini progress grid: 4 rows × 8 cols, cells 15×3px, 1px gap between rows.
                    // □=unassigned  ▭=loading (frame)  ▬=loaded (filled)  ✚=error
                    oled.drawHLine(0, 88, 128);
                    uint8_t nLoaded = 0, nErr = 0, errType = KEY_ERR_NONE;
                    for(int r = 0; r < KBD_NOTE_ROWS; r++) for(int c = 0; c < KBD_COLS; c++){
                        uint8_t kidx = (uint8_t)(r * 8 + c);
                        int cx = (7 - c) * 16, cy = 89 + (3 - r) * 4;
                        bool assigned = sampleMap[r][c].length() > 0;
                        bool loaded = audioKeyLoaded(kidx);
                        uint8_t kerr = audioKeyError(kidx);
                        if(loaded){ oled.drawBox(cx, cy, 15, 3); nLoaded++; }
                        else if(kerr){
                            oled.drawHLine(cx, cy + 1, 15);
                            oled.drawVLine(cx + 7, cy, 3);
                            nErr++; errType = kerr;
                        }
                        else if(assigned) oled.drawFrame(cx, cy, 15, 3);
                    }
                    // Status / hint lines
                    if(nErr > 0){
                        uint32_t freeKb = (uint32_t)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);
                        const char* eStr = errType == KEY_ERR_ALLOC ? "RAM" : errType == KEY_ERR_FORMAT ? "fmt" : "I/O";
                        snprintf(buf, sizeof(buf), "!%u %s %ukB B3=clr", nErr, eStr, (unsigned)freeKb);
                        oled.drawStr(0, 111, buf);
                    } else {
                        const FxEffect& selFx = fxList[fxSelected];
                        if(selFx.active){
                            char fxLine[33]; snprintf(fxLine, sizeof(fxLine), "[%.3s]", selFx.name);
                            for(int p = 0; p < 4; p++){
                                if(!selFx.paramNames[p][0]) continue;
                                char fv[6]; fmtFloat(fv, sizeof(fv), selFx.params[p]);
                                char ent[9]; snprintf(ent, sizeof(ent), " %.2s:%.4s", selFx.paramNames[p], fv);
                                if(strlen(fxLine) + strlen(ent) < sizeof(fxLine) - 1)
                                    strcat(fxLine, ent);
                            }
                            oled.drawStr(0, 111, fxLine);
                        } else {
                            if(sdCursor < sdFileCount && !sdFileIsDir[sdCursor])
                                oled.drawStr(0, 111, "Key=asgn B2=map Clk=dir");
                            else
                                oled.drawStr(0, 111, "Clk=open  B2=mapAll");
                        }
                    }
                    snprintf(buf, sizeof(buf), "[%s] %u/%u rdy", kSplayModes[samplePlayMode], nLoaded, SAMPLE_KEY_COUNT);
                    oled.drawStr(0, 119, buf);
                    // Active FX abbreviations + BPM on last line (FILT shows type)
                    {
                        static const char* kFTLf[]={"LPF","LaF","HPF","BPF"};
                        char fxstr[20] = ""; uint8_t nfx = 0;
                        for(uint8_t fi = 0; fi < FX_COUNT; fi++){
                            if(!fxList[fi].active) continue;
                            if(nfx++) strncat(fxstr, " ", sizeof(fxstr) - strlen(fxstr) - 1);
                            char a[4];
                            if(fi==0){ strncpy(a,kFTLf[(uint8_t)constrain((int)fxList[0].params[2],0,3)],4); }
                            else{ strncpy(a, fxList[fi].name, 3); a[3] = '\0'; }
                            strncat(fxstr, a, sizeof(fxstr) - strlen(fxstr) - 1);
                        }
                        snprintf(buf, sizeof(buf), "BPM:%u FX:%s", bpm, nfx ? fxstr : "--");
                        oled.drawStr(0, 127, buf);
                    }
                    oled.setFont(u8g2_font_5x7_tf);
                }
                break;
            }
            // ---- HYBRID ----
            case MODE_HYBRID: {
                oled.drawStr(0,0,"HYBRID"); oled.drawHLine(0,9,128);
                // Scale + octave info (same as SYNTH header)
                oled.setFont(u8g2_font_4x6_tf);
                snprintf(buf,sizeof(buf),"Scale:%s Oct:%d FX:[%s]",
                    scaleName(noteMap.getScale()), noteMap.getOctave(),
                    (fxList[fxSelected].active?fxList[fxSelected].name:"off"));
                oled.drawStr(0,18,buf);
                oled.setFont(u8g2_font_5x7_tf);
                // Sample grid (4×8 mini cells, same as SAMPLE mode)
                oled.drawHLine(0, 22, 128);
                for(int r=0;r<KBD_NOTE_ROWS;r++) for(int c=0;c<KBD_COLS;c++){
                    uint8_t kidx=(uint8_t)(r*8+c);
                    int cx=(7-c)*16, cy=24+(3-r)*4;
                    if(audioKeyLoaded(kidx))         oled.drawBox(cx,cy,15,3);
                    else if(sampleMap[r][c].length()) oled.drawFrame(cx,cy,15,3);
                }
                oled.drawHLine(0, 42, 128);
                // Active note names
                snprintf(buf,sizeof(buf),"Vol:%.0f%%",pots[0].value*100);
                oled.drawStr(0,122,buf);
                break;
            }
            // ---- MODULAR ----
            case MODE_MODULAR: {
                oled.drawStr(0,0,"MODULAR"); oled.drawHLine(0,9,128);
                oled.setFont(u8g2_font_4x6_tf);
                // P1: OSC shape
                snprintf(buf,sizeof(buf),"P1 OSC  : %s (oct %d)",shapeNames[modShapeIdx],noteMap.getOctave());
                oled.drawStr(0,18,buf);
                // P2/P3: Filter
                snprintf(buf,sizeof(buf),"P3 FILT : %uHz  P4 Q:%.1f",(unsigned)modCutoff,modReso);
                oled.drawStr(0,27,buf);
                // P4: Envelope
                snprintf(buf,sizeof(buf),"P4 ENV  : %s",envNames[modEnvIdx]);
                oled.drawStr(0,36,buf);
                // P5/P6: LFO vibrato
                snprintf(buf,sizeof(buf),"P5 LFO  : %.1fHz  P6 vib:%.1f st",modLfoRate,modLfoDepth);
                oled.drawStr(0,45,buf);
                // Joystick hint
                oled.drawStr(0,54,"JY=bend  JX=velocity");
                oled.setFont(u8g2_font_5x7_tf);
                oled.drawHLine(0,58,128);
                // Joystick X velocity indicator bar
                {
                    float jx=constrain(cachedJoyX/64.0f,-1.0f,1.0f);
                    int barW=(int)((jx+1.0f)*0.5f*128.0f);
                    oled.drawFrame(0,60,128,6);
                    if(barW>2) oled.drawBox(0,60,barW,6);
                    snprintf(buf,sizeof(buf),"vel %.0f%%",(0.8f+jx*0.6f)*100.0f);
                    oled.drawStr(0,74,buf);
                }
                // Volume on last line
                snprintf(buf,sizeof(buf),"Vol:%.0f%%",pots[0].value*100);
                oled.drawStr(0,122,buf);
                break;
            }
            // ---- FX ----
            case MODE_FX: {
                oled.drawStr(0,0,"FX"); oled.drawHLine(0,9,128);

                const int VIS = 5, EH = 19, LY = 12;
                // Center selected in viewport
                int top = (int)fxSelected - VIS/2;
                if (top < 0) top = 0;
                if (top + VIS > (int)FX_COUNT) top = max(0, (int)FX_COUNT - VIS);

                for (int i = top; i < top + VIS && i < (int)FX_COUNT; i++) {
                    int vy = LY + (i - top) * EH;
                    bool sel = (i == (int)fxSelected);
                    if (sel) { oled.drawRBox(0, vy-1, 120, EH-2, 2); oled.setDrawColor(0); }

                    // Find activation order (1-indexed)
                    uint8_t order = 0;
                    for (uint8_t k = 0; k < fxOrderCount; k++)
                        if (fxOrderList[k] == (uint8_t)i) { order = k+1; break; }

                    snprintf(buf, sizeof(buf), "%s%s %s",
                             order ? ">" : " ",
                             fxList[i].name,
                             fxList[i].active ? "ON" : "--");
                    oled.drawStr(2, vy + 10, buf);
                    if (order) {
                        char ob[3]; snprintf(ob, sizeof(ob), "%d", order);
                        oled.drawStr(113, vy + 10, ob);
                    }
                    oled.setDrawColor(1);
                }

                // Scroll arrows
                if (top > 0) oled.drawStr(122, LY + 6, "^");
                if (top + VIS < (int)FX_COUNT) oled.drawStr(122, LY + VIS*EH - 2, "v");

                // Params for selected (if active)
                if (fxList[fxSelected].active) {
                    oled.setFont(u8g2_font_4x6_tf);
                    static const char* kFiltTypLabel[] = {"LPF","LaF","HPF","BPF"};
                    char pb[40]; int ppos = 0;
                    for (int p = 0; p < 4; p++) {
                        if (!fxList[fxSelected].paramNames[p][0]) continue;
                        if (fxSelected == 0 && p == 2) {
                            uint8_t ti = (uint8_t)constrain((int)roundf(fxList[0].params[2]), 0, 3);
                            ppos += snprintf(pb+ppos, sizeof(pb)-ppos, "Typ:%s ", kFiltTypLabel[ti]);
                        } else {
                            ppos += snprintf(pb+ppos, sizeof(pb)-ppos, "%s:%.1f ",
                                             fxList[fxSelected].paramNames[p], fxList[fxSelected].params[p]);
                        }
                    }
                    // ResEcho: append BPM-synced subdivision info (params[3] is internal, not named)
                    if (fxSelected == 8) {
                        uint8_t si = (uint8_t)constrain((int)roundf(fxList[8].params[3]), 0, DELAY_SUBDIV_COUNT-1);
                        float dms  = constrain(60000.0f/(float)bpm*kDelaySubdiv[si], 30.0f, 700.0f);
                        ppos += snprintf(pb+ppos, sizeof(pb)-ppos, "%s(%.0fms)", kDelaySubdivName[si], dms);
                    }
                    pb[39] = '\0';
                    oled.drawStr(0, LY + VIS*EH + 4, pb);
                    oled.setFont(u8g2_font_5x7_tf);
                }

                oled.drawStr(0, 122, "Btn0=ON/OFF Joy=sel");
                break;
            }
            // ---- SEQUENCER ----
            case MODE_SEQ: {
                oled.setFont(u8g2_font_4x6_tf);
                snprintf(buf,sizeof(buf),"SEQ %dBPM %s P%d T%d",
                         bpm, seqPlaying?"[>]":"[.]", seqPage+1, seqPage*4+seqTrackSel+1);
                oled.drawStr(0,0,buf); oled.drawHLine(0,7,128);
                // SD file browser — 7 items
                for(int i=0;i<7&&(i+sdScroll)<sdFileCount;i++){
                    int idx=i+sdScroll, y=9+i*8; bool sel=(idx==sdCursor);
                    int assignedTrack=-1;
                    if(!sdFileIsDir[idx]){
                        char fp[80]; snprintf(fp,sizeof(fp),"%s%s%s",
                            sdPath.c_str(),sdPath.endsWith("/")?"":"/",sdFiles[idx].c_str());
                        for(int t=0;t<8;t++) if(seqPaths[t]==String(fp)){assignedTrack=t;break;}
                    }
                    char line[25];
                    if(sdFileIsDir[idx]) snprintf(line,sizeof(line),"%s[%s]",sel?">":"  ",sdFiles[idx].c_str());
                    else if(assignedTrack>=0) snprintf(line,sizeof(line),"%sT%d:%s",sel?">":"  ",assignedTrack+1,sdFiles[idx].c_str());
                    else snprintf(line,sizeof(line),"%s %s",sel?">":"  ",sdFiles[idx].c_str());
                    line[24]='\0'; oled.drawStr(0,y,line);
                }
                oled.drawHLine(0,65,128);
                // Step grid: 4 tracks of current page; row 3 at top, row 0 at bottom
                for(int t=0;t<4;t++){
                    uint8_t track=seqPage*4+t;
                    int cy=67+(3-t)*14;
                    // Track name (show global track number)
                    char tname[5];
                    if(seqPaths[track].length()>0){
                        const char* base=strrchr(seqPaths[track].c_str(),'/');
                        base=base?base+1:seqPaths[track].c_str();
                        strncpy(tname,base,4); tname[4]='\0';
                        char* dot=strrchr(tname,'.');
                        if(dot)*dot='\0';
                    } else {
                        snprintf(tname,sizeof(tname),"T%d",track+1);
                    }
                    bool trackSel=(t==seqTrackSel);
                    if(trackSel){oled.drawRBox(0,cy-1,16,9,1);oled.setDrawColor(0);}
                    oled.drawStr(0,cy+7,tname);
                    if(trackSel) oled.setDrawColor(1);
                    // Steps
                    for(int s=0;s<8;s++){
                        int cx=17+s*14;
                        bool curStep=(s==(int)seqStep&&seqPlaying);
                        if(curStep){oled.drawBox(cx,cy-1,13,9);oled.setDrawColor(0);}
                        if(seqPattern[track][s]) oled.drawBox(cx+1,cy,11,7);
                        else oled.drawFrame(cx+1,cy,11,7);
                        if(curStep) oled.setDrawColor(1);
                    }
                }
                if(!sdReady) oled.drawStr(20,64,"SD not found");
                oled.setFont(u8g2_font_5x7_tf);
                snprintf(buf,sizeof(buf),"Ply Trk Map8 Pg%d  %dBPM",seqPage+1,bpm); oled.drawStr(0,127,buf);
                break;
            }
            // ---- LIGHT PLAY ----
            case MODE_LIGHTPLAY: {
                oled.drawStr(0,0,"LIGHT PLAY"); oled.drawHLine(0,9,128);
                oled.setFont(u8g2_font_4x6_tf);
                oled.drawStr(0,24,"Appuie sur les touches");
                oled.drawStr(0,33,"pour creer des ripples");
                oled.drawStr(0,50,"Velocity = luminosite");
                snprintf(buf,sizeof(buf),"Scale: %s  Oct: %+d",
                         scaleName(noteMap.getScale()), noteMap.getOctave());
                oled.drawStr(0,70,buf);
                oled.setFont(u8g2_font_5x7_tf);
                break;
            }
            // ---- BATTERY ----
            case MODE_BATTERY: {
                oled.drawStr(0,0,"BATTERIE"); oled.drawHLine(0,9,128);
                float battV = muxCache[1] / 4095.0f * 10.4f;
                float pct = constrain((battV - 6.0f) / 2.4f * 100.0f, 0.0f, 100.0f);
                oled.setFont(u8g2_font_9x18_tf);
                snprintf(buf, sizeof(buf), "%.2f V", battV);
                { int tw=oled.getStrWidth(buf); oled.drawStr((128-tw)/2, 32, buf); }
                oled.setFont(u8g2_font_5x7_tf);
                snprintf(buf, sizeof(buf), "%.0f %%", pct);
                { int tw=oled.getStrWidth(buf); oled.drawStr((128-tw)/2, 48, buf); }
                const int BX=8,BY=58,BW=104,BH=20;
                oled.drawFrame(BX,BY,BW,BH);
                oled.drawBox(BX+BW,BY+BH/2-3,4,6);
                int fillW=(int)(pct/100.0f*(BW-4));
                if(fillW>0) oled.drawBox(BX+2,BY+2,fillW,BH-4);
                {
                    char pb[6]; snprintf(pb,sizeof(pb),"%.0f%%",pct);
                    int px=BX+(BW-oled.getStrWidth(pb))/2;
                    if(fillW>BW/2){oled.setDrawColor(0);oled.drawStr(px,BY+BH-5,pb);oled.setDrawColor(1);}
                    else oled.drawStr(px,BY+BH-5,pb);
                }
                const char* status = battV>8.0f?"PLEINE":pct>60.0f?"OK":pct>25.0f?"Moyen":pct>8.0f?"Faible":"CRITIQUE !";
                { int tw=oled.getStrWidth(status); oled.drawStr((128-tw)/2,98,status); }
                snprintf(buf,sizeof(buf),"raw:%d",(int)muxCache[1]);
                oled.drawStr(0,110,buf);
                oled.drawStr(20,122,"Click = menu");
                break;
            }
            // ---- SYSINFO / HUD ----
            case MODE_SYSINFO: {
                float battV2 = muxCache[1] / 4095.0f * 10.4f;
                float pct2   = constrain((battV2 - 6.0f) / 2.4f * 100.0f, 0.0f, 100.0f);
                oled.setFont(u8g2_font_4x6_tf);
                static uint8_t scanY=0;
                static uint32_t lastScan=0;
                if(millis()-lastScan>=50){scanY=(uint8_t)((scanY+1)%128);lastScan=millis();}
                oled.drawHLine(0,scanY,128);
                oled.drawStr(0,0, "[ GrvEP v2 // SYSTEM ]");
                oled.drawStr(0,8, "======================");
                snprintf(buf,sizeof(buf),"BATT: %.2fV %3.0f%% [%s]",
                         battV2,pct2,battV2>8.0f?"FULL":pct2>60.0f?"OK":pct2>25.0f?"LOW":"!!!!");
                oled.drawStr(0,16,buf);
                {
                    uint32_t psram=(uint32_t)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)/1024);
                    uint32_t heap=(uint32_t)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)/1024);
                    uint32_t psramTotal=(uint32_t)(ESP.getPsramSize()/1024);
                    snprintf(buf,sizeof(buf),"PSRAM:%lu/%lukB HEAP:%lukB",psram,psramTotal,heap);
                    oled.drawStr(0,24,buf);
                }
                snprintf(buf,sizeof(buf),"BPM:%d  SHAPE:%s",bpm,shapeNames[currentShape]);
                oled.drawStr(0,32,buf);
                snprintf(buf,sizeof(buf),"FX on:%d/%d  Vol:%.0f%%",fxOrderCount,(int)FX_COUNT,volume*100);
                oled.drawStr(0,40,buf);
                oled.drawStr(0,48,"======================");
                static bool blink2=false; static uint32_t lastBlink2=0;
                if(millis()-lastBlink2>500){blink2=!blink2;lastBlink2=millis();}
                snprintf(buf,sizeof(buf),"> READY%s",blink2?"_":"");
                oled.drawStr(0,56,buf);
                snprintf(buf,sizeof(buf),"SD:%s  raw:%d",sdReady?"OK":"--",(int)muxCache[1]);
                oled.drawStr(0,64,buf);
                oled.setFont(u8g2_font_5x7_tf);
                oled.drawStr(20,127,"Click = menu");
                break;
            }

            // ---- LIGHT ----
            case MODE_LIGHT: {
                oled.drawStr(0,0,"LIGHT"); oled.drawHLine(0,9,128);
                uint8_t ln = max((uint8_t)1, (uint8_t)(pots[3].value * NUM_LEDS + 0.5f));
                uint8_t lhue = (uint8_t)(pots[5].value * 255.0f);
                uint8_t lbri = (uint8_t)(pots[6].value * 255.0f);
                snprintf(buf,sizeof(buf),"N:   %2d  (1-%d LEDs)",  ln, (int)NUM_LEDS);
                oled.drawStr(0,24,buf);
                snprintf(buf,sizeof(buf),"Spd: %.2f", pots[4].value);
                oled.drawStr(0,40,buf);
                snprintf(buf,sizeof(buf),"Hue: %3d", lhue);
                oled.drawStr(0,56,buf);
                snprintf(buf,sizeof(buf),"Bri: %3d", lbri);
                oled.drawStr(0,72,buf);
                // Color preview strip
                if (lbri > 10) oled.drawBox(90, 20, 30, 60);
                else           oled.drawFrame(90, 20, 30, 60);
                oled.drawStr(0,122,"3=N 4=Spd 5=Hue 6=Bri");
                break;
            }
            // ---- SYNTH2 — Diapasonix patch browser ----
            case MODE_SYNTH2: {
                oled.drawStr(0,0,"SYNTH2"); oled.drawHLine(0,9,128);
                oled.setFont(u8g2_font_4x6_tf);
                // Patch number + bank indicator
                const char* bank = (s2PatchIdx < 128) ? "JUNO" : "DX7 ";
                snprintf(buf,sizeof(buf),"[%s] #%03d", bank, s2PatchIdx);
                oled.drawStr(0,20,buf);
                // Full patch name (may be long — truncate to 25 chars)
                char pname[26]; strncpy(pname, patch_names[s2PatchIdx], 25); pname[25]='\0';
                oled.drawStr(0,29,pname);
                // Cutoff + navigation hints
                snprintf(buf,sizeof(buf),"Cut:%4dHz  Oct:%d",(int)s2Cutoff,noteMap.getOctave());
                oled.drawStr(0,40,buf);
                // Progress bar
                oled.drawFrame(0,45,128,5);
                int barW2 = (int)((float)s2PatchIdx / (SYNTH2_PATCH_COUNT-1) * 128.0f);
                if(barW2>0) oled.drawBox(0,45,barW2,5);
                // Prev/Next patch names
                if(s2PatchIdx>0){ char pn[20]; strncpy(pn,patch_names[s2PatchIdx-1],19); pn[19]='\0'; snprintf(buf,sizeof(buf),"< %s",pn); oled.drawStr(0,53,buf); }
                if(s2PatchIdx<SYNTH2_PATCH_COUNT-1){ char pn[20]; strncpy(pn,patch_names[s2PatchIdx+1],19); pn[19]='\0'; snprintf(buf,sizeof(buf),"> %s",pn); oled.drawStr(0,61,buf); }
                oled.setFont(u8g2_font_5x7_tf);
                snprintf(buf,sizeof(buf),"Vol:%.0f%%  JY=scroll",pots[0].value*100);
                oled.drawStr(0,122,buf);
                break;
            }
            // ---- DRUM2 (TR-808 style + sequencer) ----
            case MODE_DRUM2: {
                // Active FX string (3-char abbrevs; FILT shows type: LPF/LaF/HPF/BPF)
                static const char* kFTL[]={"LPF","LaF","HPF","BPF"};
                char fxstr[32]="";
                for(uint8_t fi=0;fi<FX_COUNT;fi++){
                    if(!fxList[fi].active) continue;
                    if(fxstr[0]) strncat(fxstr," ",sizeof(fxstr)-strlen(fxstr)-1);
                    char a[4];
                    if(fi==0){ strncpy(a,kFTL[(uint8_t)constrain((int)fxList[0].params[2],0,3)],4); }
                    else{ strncpy(a,fxList[fi].name,3); a[3]='\0'; }
                    strncat(fxstr,a,sizeof(fxstr)-strlen(fxstr)-1);
                }
                if(!fxstr[0]) strncpy(fxstr,"--",3);
                static const char* kAltName[4]={"NRM","50%","RND","DBL"};
                // u8g2 drawStr: y = baseline. 5x7→chars at [y-6,y+1]. 4x6→chars at [y-5,y+1].
                // Pad strip: drawBox(x,bY,w,9) + drawStr(x,bY+7,...) puts chars at bY+2..bY+7 ✓
                // Vol bar:   drawFrame(x,vY,100,6) + drawStr(104,vY+5,...) → label chars = bar height ✓

                if(drum2SeqView){
                    // ═══ DR2 SEQ VIEW ════════════════════════════════
                    // Row 0: header (5x7 baseline=7 → chars y=1..8)
                    oled.setFont(u8g2_font_5x7_tf);
                    snprintf(buf,sizeof(buf),"SEQ %s  %d BPM  [%s]",
                             drum2Playing?"[>]":"[ ]",bpm,kDrum2Labels[drum2SelPad]);
                    oled.drawStr(0,7,buf);
                    oled.drawHLine(0,9,128);

                    // Step grid: 2 rows × 8 cells, 16px wide × 14px tall
                    // Row 1: y=10..23, Row 2: y=25..38
                    for(int half=0;half<2;half++){
                        int cy=10+half*15;
                        for(int sc=0;sc<8;sc++){
                            uint8_t step=(uint8_t)(half*8+sc);
                            int cx=sc*16;
                            bool act=(drum2SeqVel[drum2SelPad][step]>0);
                            bool cur=drum2Playing&&(step==(uint8_t)drum2Step);
                            uint8_t m=act?drum2SeqMod[drum2SelPad][step]:0;
                            if(cur){
                                oled.drawBox(cx,cy,15,14);
                                if(!act){oled.setDrawColor(0);oled.drawBox(cx+2,cy+3,11,8);oled.setDrawColor(1);}
                            }else if(act){
                                switch(m){
                                    case 0: oled.drawBox(cx+1,cy+1,13,12); break;
                                    case 1: oled.drawBox(cx+1,cy+1,13,6);oled.drawFrame(cx+1,cy+8,13,5); break;
                                    case 2: oled.drawBox(cx+1,cy+1,13,12);oled.setDrawColor(0);oled.drawBox(cx+4,cy+4,7,5);oled.setDrawColor(1); break;
                                    case 3: oled.drawBox(cx+1,cy+1,5,12);oled.drawBox(cx+9,cy+1,5,12); break;
                                }
                            }else{
                                oled.drawFrame(cx,cy,15,14);
                            }
                        }
                    }
                    oled.drawHLine(0,39,128);

                    // Pad strip: box y=40..48, text baseline=47 → chars y=42..47 ✓
                    oled.setFont(u8g2_font_4x6_tf);
                    for(int pi=0;pi<DRUM2_PADS;pi++){
                        int px=pi*15+2;
                        bool sel=(pi==drum2SelPad);
                        if(sel){oled.drawBox(px-2,40,17,9);oled.setDrawColor(0);}
                        oled.drawStr(px,47,kDrum2Labels[pi]);
                        oled.setDrawColor(1);
                    }
                    oled.drawHLine(0,50,128);

                    // Alt + pitch (5x7 baseline=58 → chars y=52..59)
                    oled.setFont(u8g2_font_5x7_tf);
                    snprintf(buf,sizeof(buf),"Alt: %-3s    Pt: %d",
                             kAltName[drum2PadAltMode[drum2SelPad]],
                             drum2Pitch[drum2SelPad]);
                    oled.drawStr(0,58,buf);
                    oled.drawHLine(0,60,128);

                    // FX (4x6 baseline=67 → chars y=62..67)
                    oled.setFont(u8g2_font_4x6_tf);
                    snprintf(buf,sizeof(buf),"FX: %s",fxstr);
                    oled.drawStr(0,67,buf);

                    // Pad vol bar y=69..74
                    {   int vw=(int)(drum2Volume[drum2SelPad]*100);
                        oled.drawStr(0,74,"V:");
                        oled.drawFrame(9,69,80,6);
                        if(vw>0) oled.drawBox(9,69,vw*80/100,6);
                        snprintf(buf,sizeof(buf),"%d%%",vw);
                        oled.drawStr(91,74,buf);
                        oled.drawStr(108,74,"P5=Vol");
                    }

                    // Hint / REC (baseline=82 → chars y=77..82)
                    if(drum2RecArmed){
                        oled.drawRBox(0,76,50,9,2);
                        oled.setDrawColor(0); oled.drawStr(4,83,"REC ARM"); oled.setDrawColor(1);
                        oled.drawStr(55,83,"B4=Pad  JY=vel JX=swg");
                    }else{
                        oled.drawStr(0,83,"B3=Rec  B4=Pad  JY=vel JX=swg");
                    }
                }else{
                    // ═══ DR2 PAD VIEW ════════════════════════════════
                    // Header (5x7 baseline=7 → chars y=1..8)
                    oled.setFont(u8g2_font_5x7_tf);
                    snprintf(buf,sizeof(buf),"DR2 %s  %d BPM",
                             drum2Playing?"[>]":"[ ]",bpm);
                    oled.drawStr(0,7,buf);
                    oled.drawHLine(0,9,128);

                    // Pad params (5x7 baseline=17 → chars y=11..18)
                    // If REC armed: inverted box y=10..19, text still at baseline=17
                    snprintf(buf,sizeof(buf),"[%s]  Pt:%d  Dcy:%.0f",
                             kDrum2Labels[drum2SelPad],
                             (int)drum2Pitch[drum2SelPad],
                             drum2Decay[drum2SelPad]);
                    if(drum2RecArmed){
                        oled.drawRBox(0,10,128,10,2);
                        oled.setDrawColor(0); oled.drawStr(2,17,buf); oled.setDrawColor(1);
                    }else{
                        oled.drawStr(0,17,buf);
                    }
                    oled.drawHLine(0,20,128);

                    // Step bar: cells 7×9 at y=22..30
                    for(int s=0;s<DR2_STEPS;s++){
                        int sx=s*8;
                        bool cur=drum2Playing&&(s==(int)drum2Step);
                        bool has=(drum2SeqVel[drum2SelPad][s]>0);
                        if(cur&&has){ oled.drawBox(sx,22,7,9);oled.setDrawColor(0);oled.drawBox(sx+1,23,5,7);oled.setDrawColor(1);}
                        else if(cur){ oled.drawBox(sx,22,7,9);}
                        else if(has){ oled.drawBox(sx+1,23,5,7);}
                        else{         oled.drawFrame(sx,22,7,9);}
                    }
                    oled.drawHLine(0,32,128);

                    // Pad strip: box y=33..41, text baseline=40 → chars y=35..40 ✓
                    oled.setFont(u8g2_font_4x6_tf);
                    for(int pi=0;pi<DRUM2_PADS;pi++){
                        int px=pi*15+2;
                        bool sel=(pi==drum2SelPad);
                        if(sel){oled.drawBox(px-2,33,17,9);oled.setDrawColor(0);}
                        oled.drawStr(px,40,kDrum2Labels[pi]);
                        oled.setDrawColor(1);
                    }
                    oled.drawHLine(0,43,128);

                    // FX (4x6 baseline=50 → chars y=45..50)
                    snprintf(buf,sizeof(buf),"FX: %s",fxstr);
                    oled.drawStr(0,50,buf);
                    oled.drawHLine(0,52,128);

                    // Alt mode (5x7 baseline=60 → chars y=54..61)
                    oled.setFont(u8g2_font_5x7_tf);
                    snprintf(buf,sizeof(buf),"Alt: %s",kAltName[drum2PadAltMode[drum2SelPad]]);
                    oled.drawStr(0,60,buf);
                    oled.drawHLine(0,62,128);

                    // Pad vol bar y=64..69
                    oled.setFont(u8g2_font_4x6_tf);
                    {   int vw=(int)(drum2Volume[drum2SelPad]*100);
                        oled.drawStr(0,69,"V:");
                        oled.drawFrame(9,64,88,6);
                        if(vw>0) oled.drawBox(9,64,vw*88/100,6);
                        snprintf(buf,sizeof(buf),"%d%%",vw);
                        oled.drawStr(100,69,buf);
                    }

                    // Hints (4x6 baseline=78,85)
                    oled.drawStr(0,78,"P3=Pit P4=Dcy P5=Vol B3=Rec");
                    oled.drawStr(0,85,"B2=Ply B4=Seq  JY=vel JX=swg");
                }
                break;
            }
            // ---- SYSEQ — 16-step polyphonic synth sequencer ----
            case MODE_SYSEQ: {
                // Active FX string (FILT shows type: LPF/LaF/HPF/BPF)
                static const char* kFTLs[]={"LPF","LaF","HPF","BPF"};
                char fxstr[32]="";
                for(uint8_t fi=0;fi<FX_COUNT;fi++){
                    if(!fxList[fi].active) continue;
                    if(fxstr[0]) strncat(fxstr," ",sizeof(fxstr)-strlen(fxstr)-1);
                    char a[4];
                    if(fi==0){ strncpy(a,kFTLs[(uint8_t)constrain((int)fxList[0].params[2],0,3)],4); }
                    else{ strncpy(a,fxList[fi].name,3); a[3]='\0'; }
                    strncat(fxstr,a,sizeof(fxstr)-strlen(fxstr)-1);
                }
                if(!fxstr[0]) strncpy(fxstr,"--",3);
                static const char* ssnn[]={"C","c","D","d","E","F","f","G","g","A","a","B"};

                if(syseqSeqView){
                    // ═══ SSEQ SEQ VIEW ═══════════════════════════════
                    // Header (5x7 baseline=7 → chars y=1..8)
                    oled.setFont(u8g2_font_5x7_tf);
                    snprintf(buf,sizeof(buf),"SSEQ %s  %d BPM  S:%02d",
                             drum2Playing?"[>]":"[ ]",bpm,syseqSelStep);
                    oled.drawStr(0,7,buf);
                    oled.drawHLine(0,9,128);

                    // Step grid: row1 y=10..23, row2 y=25..38
                    // Note text baseline = cy+11 → chars cy+6..cy+11 (lower-center of 14px cell)
                    oled.setFont(u8g2_font_4x6_tf);
                    for(int half=0;half<2;half++){
                        int cy=10+half*15;
                        for(int sc=0;sc<8;sc++){
                            uint8_t step=(uint8_t)(half*8+sc);
                            int cx=sc*16;
                            uint8_t cnt=0;
                            for(uint8_t i=0;i<SYSEQ_CHORD;i++) if(syseqNotes[step][i]) cnt++;
                            bool cur=drum2Playing&&(step==(uint8_t)drum2Step);
                            bool sel=(step==syseqSelStep);
                            if(cur){
                                oled.drawBox(cx,cy,15,14);
                                if(!cnt){oled.setDrawColor(0);oled.drawBox(cx+2,cy+3,11,8);oled.setDrawColor(1);}
                            }else if(sel&&cnt){
                                oled.drawFrame(cx,cy,15,14);oled.drawBox(cx+2,cy+2,11,10);
                            }else if(sel){
                                oled.drawFrame(cx,cy,15,14);oled.drawFrame(cx+1,cy+1,13,12);
                            }else if(cnt){
                                oled.drawBox(cx+1,cy+1,13,12);
                            }else{
                                oled.drawFrame(cx,cy,15,14);
                            }
                            if(cnt){
                                uint8_t fn=0;
                                for(uint8_t i=0;i<SYSEQ_CHORD;i++) if(syseqNotes[step][i]){fn=syseqNotes[step][i];break;}
                                if(fn){ uint8_t n=fn-1;
                                    char nb[4]; snprintf(nb,sizeof(nb),"%s%d",ssnn[n%12],(int)(n/12)-1);
                                    oled.setDrawColor(0);
                                    oled.drawStr(cx+1,cy+11,nb);
                                    oled.setDrawColor(1);
                                }
                                // FUL alt indicator: small bar at top of cell
                                if(syseqAlt[step]==1) oled.drawHLine(cx+3,cy+1,9);
                            }
                        }
                    }
                    oled.drawHLine(0,39,128);

                    // Held notes (4x6 baseline=46 → chars y=41..46)
                    {
                        char hbuf[22]="";
                        for(uint8_t r2=0;r2<KBD_NOTE_ROWS&&strlen(hbuf)<18;r2++)
                            for(uint8_t c2=0;c2<KBD_COLS&&strlen(hbuf)<18;c2++)
                                if(activeNotes[r2][c2]){
                                    uint8_t n=activeNotes[r2][c2];
                                    char nb[5]; snprintf(nb,sizeof(nb),"%s%d ",ssnn[n%12],(int)(n/12)-1);
                                    strncat(hbuf,nb,sizeof(hbuf)-strlen(hbuf)-1);
                                }
                        if(hbuf[0]) snprintf(buf,sizeof(buf),"HLD: %s",hbuf);
                        else         strncpy(buf,"HLD: (none)",sizeof(buf)-1);
                        oled.drawStr(0,46,buf);
                    }

                    // Step content (4x6 baseline=54 → chars y=49..54)
                    {
                        char sbuf[22]="";
                        for(uint8_t i=0;i<SYSEQ_CHORD&&strlen(sbuf)<18;i++){
                            if(!syseqNotes[syseqSelStep][i]) continue;
                            uint8_t n=syseqNotes[syseqSelStep][i]-1;
                            char nb[5]; snprintf(nb,sizeof(nb),"%s%d ",ssnn[n%12],(int)(n/12)-1);
                            strncat(sbuf,nb,sizeof(sbuf)-strlen(sbuf)-1);
                        }
                        snprintf(buf,sizeof(buf),"S%02d: %s [%s]",syseqSelStep,
                                 sbuf[0]?sbuf:"--", syseqAlt[syseqSelStep]?"FUL":"NRM");
                        oled.drawStr(0,54,buf);
                    }
                    oled.drawHLine(0,56,128);

                    // FX (4x6 baseline=63 → chars y=58..63)
                    snprintf(buf,sizeof(buf),"FX: %s",fxstr);
                    oled.drawStr(0,63,buf);

                    // Vol bar y=65..70, label baseline=70 → chars y=65..70 ✓
                    {   int vw=min(100,(int)(pots[0].value*100));
                        oled.drawFrame(0,65,100,6);
                        if(vw>0) oled.drawBox(0,65,vw,6);
                        snprintf(buf,sizeof(buf),"%d%%",(int)(pots[0].value*100));
                        oled.drawStr(104,70,buf);
                    }

                    // Hint (4x6 baseline=79 → chars y=74..79)
                    oled.drawStr(0,79,"B3=FUL B4=Pad  B2=Opt");
                }else{
                    // ═══ SSEQ PAD VIEW ═══════════════════════════════
                    // Header (5x7 baseline=7 → chars y=1..8)
                    oled.setFont(u8g2_font_5x7_tf);
                    snprintf(buf,sizeof(buf),"SSEQ %s  %d BPM  Oct:%+d",
                             drum2Playing?"[>]":"[ ]",bpm,noteMap.getOctave());
                    oled.drawStr(0,7,buf);
                    oled.drawHLine(0,9,128);

                    // Shape + Env (5x7 baseline=18 → chars y=12..19)
                    snprintf(buf,sizeof(buf),"%-8s  Env: %-3s",
                             shapeNames[currentShape],envNames[currentEnv]);
                    oled.drawStr(0,18,buf);
                    oled.drawHLine(0,20,128);

                    // Scale + Arp (5x7 baseline=29 → chars y=23..30)
                    {   const char* arpStr=(arpMode==0)?"Off":(arpMode==1)?"Up":(arpMode==2)?"Dn":(arpMode==3)?"Rnd":"UD";
                        snprintf(buf,sizeof(buf),"Scale:%-5s  Arp:%s",
                                 scaleName(noteMap.getScale()),arpStr);
                        oled.drawStr(0,29,buf);
                    }
                    oled.drawHLine(0,31,128);

                    // Step mini bar: 16 cells 7×9 at y=33..41
                    for(int s=0;s<SYSEQ_STEPS;s++){
                        int sx=s*8;
                        bool cur=drum2Playing&&(s==(int)drum2Step);
                        uint8_t cnt=0;
                        for(uint8_t i=0;i<SYSEQ_CHORD;i++) if(syseqNotes[s][i]) cnt++;
                        if(cur&&cnt){ oled.drawBox(sx,33,7,9);oled.setDrawColor(0);oled.drawBox(sx+1,34,5,7);oled.setDrawColor(1);}
                        else if(cur){ oled.drawBox(sx,33,7,9);}
                        else if(cnt){ oled.drawBox(sx+1,34,5,7);}
                        else{         oled.drawFrame(sx,33,7,9);}
                    }
                    oled.drawHLine(0,43,128);

                    // FX (4x6 baseline=50 → chars y=45..50)
                    oled.setFont(u8g2_font_4x6_tf);
                    snprintf(buf,sizeof(buf),"FX: %s",fxstr);
                    oled.drawStr(0,50,buf);
                    oled.drawHLine(0,52,128);

                    // Vol bar y=54..59, label baseline=59 → chars y=54..59 ✓
                    {   int vw=min(100,(int)(pots[0].value*100));
                        oled.drawFrame(0,54,100,6);
                        if(vw>0) oled.drawBox(0,54,vw,6);
                        snprintf(buf,sizeof(buf),"%d%%",(int)(pots[0].value*100));
                        oled.drawStr(104,59,buf);
                    }

                    // Hint (4x6 baseline=68 → chars y=63..68)
                    oled.drawStr(0,68,"B2=Menu  B4=Seq");
                }
                break;
            }
            // ---- 303S — TB-303 step sequencer ----
            case MODE_303S: {
                static const char* s3nn[]={"C","c","D","d","E","F","f","G","g","A","a","B"};
                static const char* s3wn[]={"SIN","SQR","SAW","SUP","TRI","NSE","KS"};
                const char* s3wname = (t303Wave==T303_NAP_WAVE) ? "NAP" : (t303Wave < 7) ? s3wn[t303Wave] : "---";
                char s3fxstr[20]="";
                { static const char* kFTL3[]={"LPF","LaF","HPF","BPF"};
                  uint8_t nfx=0;
                  for(uint8_t fi=0;fi<FX_COUNT;fi++){
                      if(!fxList[fi].active) continue;
                      if(nfx++) strncat(s3fxstr," ",sizeof(s3fxstr)-strlen(s3fxstr)-1);
                      char a[4];
                      if(fi==0){ strncpy(a,kFTL3[(uint8_t)constrain((int)fxList[0].params[2],0,3)],4); }
                      else{ strncpy(a,fxList[fi].name,3); a[3]='\0'; }
                      strncat(s3fxstr,a,sizeof(s3fxstr)-strlen(s3fxstr)-1);
                  }
                  if(!s3fxstr[0]) strncpy(s3fxstr,"--",3);
                }
                if (s303SeqView) {
                    // ═══ 303S SEQ VIEW ═══════════════════════════════
                    static const char* s3alt[]={"NRM","ACC","SLD"};
                    oled.setFont(u8g2_font_5x7_tf);
                    snprintf(buf,sizeof(buf),"303S %s %dBPM S:%02d Alt:[%s]",
                             drum2Playing?"[>]":"[ ]",bpm,s303SelStep,s3alt[s303PendingAlt]);
                    oled.drawStr(0,7,buf);
                    oled.drawHLine(0,9,128);

                    // Step grid: 2 rows × 8 cells, 16×14px each
                    oled.setFont(u8g2_font_4x6_tf);
                    for(int half=0;half<2;half++){
                        int cy=10+half*15;
                        for(int sc=0;sc<8;sc++){
                            uint8_t step=(uint8_t)(half*8+sc);
                            int cx=sc*16;
                            bool hasNote=(s303Note[step]>0);
                            bool cur=drum2Playing&&(step==(uint8_t)drum2Step);
                            bool sel=(step==s303SelStep);
                            if(cur){
                                oled.drawBox(cx,cy,15,14);
                                if(!hasNote){oled.setDrawColor(0);oled.drawBox(cx+2,cy+3,11,8);oled.setDrawColor(1);}
                            }else if(sel&&hasNote){
                                oled.drawFrame(cx,cy,15,14);oled.drawBox(cx+2,cy+2,11,10);
                            }else if(sel){
                                oled.drawFrame(cx,cy,15,14);oled.drawFrame(cx+1,cy+1,13,12);
                            }else if(hasNote){
                                oled.drawBox(cx+1,cy+1,13,12);
                            }else{
                                oled.drawFrame(cx,cy,15,14);
                            }
                            if(hasNote){
                                uint8_t n=s303Note[step]-1;
                                char nb[4]; snprintf(nb,sizeof(nb),"%s%d",s3nn[n%12],(int)(n/12)-1);
                                oled.setDrawColor(0);
                                oled.drawStr(cx+1,cy+11,nb);
                                oled.setDrawColor(1);
                            }
                            // Alt indicator (top-right corner): ACC=dot, SLD=dash, SIL=X
                            if(s303Alt[step]==1) oled.drawPixel(cx+13,cy+1);
                            else if(s303Alt[step]==2) oled.drawHLine(cx+11,cy+1,3);
                        }
                    }
                    oled.drawHLine(0,39,128);

                    // Step detail: selected note → step's current note
                    {
                        uint8_t sn = s303Note[s303SelStep];
                        char stepNote[6]; if(sn){ uint8_t m=sn-1; snprintf(stepNote,sizeof(stepNote),"%s%d",s3nn[m%12],(int)(m/12)-1); }
                        else strncpy(stepNote,"--",3);
                        char selNote[6]; if(s303SelNote){ uint8_t m=s303SelNote-1; snprintf(selNote,sizeof(selNote),"%s%d",s3nn[m%12],(int)(m/12)-1); }
                        else strncpy(selNote,"--",4);
                        snprintf(buf,sizeof(buf),"Sel:%-4s  S%02d:%-4s  Alt:%s",
                                 selNote, s303SelStep, stepNote, s3alt[s303PendingAlt]);
                        oled.drawStr(0,47,buf);
                    }

                    // Wave + oct + FX (4x6 baseline=54)
                    snprintf(buf,sizeof(buf),"Wv:%-3s Oct:%+d  FX:%s",s3wname,t303Oct,s3fxstr);
                    oled.drawStr(0,54,buf);
                    oled.drawHLine(0,56,128);

                    // Vol bar y=58..63
                    {   int vw=min(100,(int)(pots[0].value*100));
                        oled.drawFrame(0,58,100,5);
                        if(vw>0) oled.drawBox(0,58,vw,5);
                        snprintf(buf,sizeof(buf),"%d%%",(int)(pots[0].value*100));
                        oled.drawStr(104,63,buf);
                    }

                    // Hints (baseline=72)
                    oled.drawStr(0,72,"B3=Alt B4=Pad P2=Wv JX=vel JY=Oct");

                    // ── Pot params in bigger font ────────────────────
                    oled.setFont(u8g2_font_6x10_tf);
                    oled.drawHLine(0,75,128);
                    snprintf(buf,sizeof(buf),"Cut:%-4d  Res:%.1f",(int)t303Cutoff,t303Reso);
                    oled.drawStr(0,88,buf);
                    snprintf(buf,sizeof(buf),"Dur:%.0f%%  Mod:%-4.1f",t303Duration*100.0f,t303EnvMod);
                    oled.drawStr(0,102,buf);

                    // Waveform preview y=105..124 — shows the effect of P2 on the current wave
                    {
                        const int WX=0,WY=105,WW=128,WH=20,cy=WY+WH/2,ah=WH/2-2;
                        oled.setFont(u8g2_font_4x6_tf);
                        oled.drawFrame(WX,WY,WW,WH);
                        float p2=pots[1].value;
                        if(t303Wave==PULSE||t303Wave==T303_NAP_WAVE){
                            float duty=(t303Wave==PULSE)?(0.5f-p2*0.48f):(0.15f-p2*0.14f);
                            int dw=max(2,min(WW-4,(int)(duty*(WW-2))));
                            int hy=WY+1, ly=WY+WH-2;
                            oled.drawHLine(WX+1,hy,dw);
                            oled.drawVLine(WX+1+dw,hy,ly-hy+1);
                            oled.drawHLine(WX+2+dw,ly,WW-3-dw);
                        } else {
                            float g=(p2<0.01f)?1.0f:powf(4.0f,p2);
                            int prev=cy;
                            for(int i=0;i<WW-2;i++){
                                float t=(float)i/(float)(WW-3);
                                float base=(t303Wave==TRIANGLE)?((t<0.5f)?(4*t-1):(3-4*t)):
                                           (t303Wave==SAW_UP)?(2*t-1):(1-2*t);
                                float s=base*g, ph=fmodf(s+1.0f,4.0f);
                                if(ph<0.0f) ph+=4.0f;
                                float yv=(ph<2.0f)?(ph-1.0f):(3.0f-ph);
                                int py=constrain(cy-(int)(yv*ah+0.5f),WY+1,WY+WH-2);
                                if(i>0) oled.drawLine(WX+i,prev,WX+1+i,py);
                                else    oled.drawPixel(WX+1,py);
                                prev=py;
                            }
                        }
                    }
                } else {
                    // ═══ 303S PAD VIEW ═══════════════════════════════
                    oled.setFont(u8g2_font_5x7_tf);
                    snprintf(buf,sizeof(buf),"303S %s %dBPM  Wv:%-3s Oct:%+d",
                             drum2Playing?"[>]":"[ ]",bpm,s3wname,t303Oct);
                    oled.drawStr(0,7,buf);
                    oled.drawHLine(0,9,128);

                    // Step mini bar: 16 cells 7×9px at y=11..19
                    oled.setFont(u8g2_font_4x6_tf);
                    for(int s=0;s<S303_STEPS;s++){
                        int sx=s*8;
                        bool cur=drum2Playing&&(s==(int)drum2Step);
                        bool hasNote=(s303Note[s]>0);
                        if(cur&&hasNote){ oled.drawBox(sx,11,7,9);oled.setDrawColor(0);oled.drawBox(sx+1,12,5,7);oled.setDrawColor(1);}
                        else if(cur){     oled.drawBox(sx,11,7,9);}
                        else if(hasNote){ oled.drawBox(sx+1,12,5,7);}
                        else{             oled.drawFrame(sx,11,7,9);}
                        // Alteration dot: ACC=dot, SLD=dash, SIL=x
                        if(hasNote){
                            if(s303Alt[s]==1)      oled.drawPixel(sx+6,11);
                            else if(s303Alt[s]==2) oled.drawHLine(sx+4,11,3);
                        }
                    }
                    oled.drawHLine(0,21,128);

                    // Wave + filter (baseline=28 → y=23..28)
                    snprintf(buf,sizeof(buf),"Wv:%-3s Cut:%.0f Res:%.1f",s3wname,t303Cutoff,t303Reso);
                    oled.drawStr(0,28,buf);

                    // Decay + accent state (baseline=35 → y=30..35)
                    snprintf(buf,sizeof(buf),"Dec:%.0fms Acc:%s Sld:%s",
                             t303Decay, t303AccentOn?"ON":"--", t303SlideOn?"ON":"--");
                    oled.drawStr(0,35,buf);

                    // Current step note (baseline=42 → y=37..42)
                    {
                        uint8_t cstep = drum2Playing ? drum2Step : s303SelStep;
                        uint8_t n = s303Note[cstep];
                        char noteStr[6]; if(n){ uint8_t m=n-1; snprintf(noteStr,sizeof(noteStr),"%s%d",s3nn[m%12],(int)(m/12)-1); }
                        else strncpy(noteStr,"--",3);
                        { static const char* altlbl[]={"NRM","ACC","SLD"};
                          snprintf(buf,sizeof(buf),"S%02d:%s [%s]",
                                   cstep, noteStr, altlbl[s303Alt[cstep]]);
                          oled.drawStr(0,42,buf); }
                    }
                    oled.drawHLine(0,44,128);

                    // FX (baseline=51)
                    snprintf(buf,sizeof(buf),"FX: %s",s3fxstr);
                    oled.drawStr(0,51,buf);

                    // Vol bar y=54..59
                    {   int vw=min(100,(int)(pots[0].value*100));
                        oled.drawFrame(0,54,100,6);
                        if(vw>0) oled.drawBox(0,54,vw,6);
                        snprintf(buf,sizeof(buf),"%d%%",(int)(pots[0].value*100));
                        oled.drawStr(104,59,buf);
                    }

                    // Hints (baseline=68)
                    oled.drawStr(0,68,"B3=Acc B4=Seq P2=Wv JX=vel");

                    // ── Pot params in bigger font ────────────────────
                    oled.setFont(u8g2_font_6x10_tf);
                    oled.drawHLine(0,71,128);
                    snprintf(buf,sizeof(buf),"Cut:%-4d  Res:%.1f",(int)t303Cutoff,t303Reso);
                    oled.drawStr(0,84,buf);
                    snprintf(buf,sizeof(buf),"Dur:%.0f%%  Mod:%-4.1f",t303Duration*100.0f,t303EnvMod);
                    oled.drawStr(0,98,buf);

                    // Waveform preview y=101..120
                    {
                        const int WX=0,WY=101,WW=128,WH=20,cy=WY+WH/2,ah=WH/2-2;
                        oled.setFont(u8g2_font_4x6_tf);
                        oled.drawFrame(WX,WY,WW,WH);
                        float p2=pots[1].value;
                        if(t303Wave==PULSE||t303Wave==T303_NAP_WAVE){
                            float duty=(t303Wave==PULSE)?(0.5f-p2*0.48f):(0.15f-p2*0.14f);
                            int dw=max(2,min(WW-4,(int)(duty*(WW-2))));
                            int hy=WY+1, ly=WY+WH-2;
                            oled.drawHLine(WX+1,hy,dw);
                            oled.drawVLine(WX+1+dw,hy,ly-hy+1);
                            oled.drawHLine(WX+2+dw,ly,WW-3-dw);
                        } else {
                            float g=(p2<0.01f)?1.0f:powf(4.0f,p2);
                            int prev=cy;
                            for(int i=0;i<WW-2;i++){
                                float t=(float)i/(float)(WW-3);
                                float base=(t303Wave==TRIANGLE)?((t<0.5f)?(4*t-1):(3-4*t)):
                                           (t303Wave==SAW_UP)?(2*t-1):(1-2*t);
                                float s=base*g, ph=fmodf(s+1.0f,4.0f);
                                if(ph<0.0f) ph+=4.0f;
                                float yv=(ph<2.0f)?(ph-1.0f):(3.0f-ph);
                                int py=constrain(cy-(int)(yv*ah+0.5f),WY+1,WY+WH-2);
                                if(i>0) oled.drawLine(WX+i,prev,WX+1+i,py);
                                else    oled.drawPixel(WX+1,py);
                                prev=py;
                            }
                        }
                    }
                }
                break;
            }
            // ---- SS2 — Sample Step Sequencer ----
            case MODE_SS2: {
                static const char* ss2alt[]={"NRM","REV","FUL","SIL"};
                oled.setFont(u8g2_font_4x6_tf);

                if (ss2SeqView) {
                    // ═══ SS2 SEQ VIEW ════════════════════════════════
                    oled.setFont(u8g2_font_5x7_tf);
                    snprintf(buf,sizeof(buf),"SS2 %s %dBPM S:%02d [%s]",
                             drum2Playing?"[>]":"[ ]",bpm,ss2SelStep,ss2alt[ss2Alt[ss2SelStep]]);
                    oled.drawStr(0,7,buf);
                    oled.drawHLine(0,9,128);

                    // Step grid: 2 rows × 8 cells, 16×14px
                    oled.setFont(u8g2_font_4x6_tf);
                    for(int half=0;half<2;half++){
                        int cy=10+half*15;
                        for(int sc=0;sc<8;sc++){
                            uint8_t step=(uint8_t)(half*8+sc);
                            int cx=sc*16;
                            bool has=(ss2Note[step]!=0);
                            bool cur=drum2Playing&&(step==(uint8_t)drum2Step);
                            bool sel=(step==ss2SelStep);
                            if(cur){ oled.drawBox(cx,cy,15,14);
                                if(!has){oled.setDrawColor(0);oled.drawBox(cx+2,cy+3,11,8);oled.setDrawColor(1);}
                            }else if(sel&&has){ oled.drawFrame(cx,cy,15,14);oled.drawBox(cx+2,cy+2,11,10); }
                            else if(sel){       oled.drawFrame(cx,cy,15,14);oled.drawFrame(cx+1,cy+1,13,12); }
                            else if(has){       oled.drawBox(cx+1,cy+1,13,12); }
                            else{               oled.drawFrame(cx,cy,15,14); }
                            if(has){
                                int cnt=__builtin_popcount(ss2Note[step]);
                                char sb[4];
                                if(cnt==1) snprintf(sb,sizeof(sb),"%02d",(int)__builtin_ctz(ss2Note[step]));
                                else       snprintf(sb,sizeof(sb),"x%d",cnt);
                                oled.setDrawColor(cur?0:1);
                                oled.drawStr(cx+2,cy+11,sb);
                                oled.setDrawColor(1);
                            }
                            // Alt indicator: REV=R pixel, FUL=F pixel, SIL=X mark
                            if(ss2Alt[step]==1)      { oled.drawPixel(cx+13,cy+1); oled.drawPixel(cx+12,cy+2); }  // REV: diagonal
                            else if(ss2Alt[step]==2) oled.drawHLine(cx+11,cy+1,3);   // FUL: bar
                            else if(ss2Alt[step]==3){ oled.drawPixel(cx+12,cy+1);oled.drawPixel(cx+13,cy+2); }  // SIL: cross
                        }
                    }
                    oled.drawHLine(0,39,128);

                    // Step detail
                    {   uint16_t smask=ss2Note[ss2SelStep];
                        int cnt=__builtin_popcount(smask);
                        bool selOn=(smask & (1u<<ss2SelSlot)) != 0;
                        snprintf(buf,sizeof(buf),"S%02d:%d slot%s Sl%02d:%s [%s]",
                                 ss2SelStep,cnt,cnt==1?"":"s",
                                 ss2SelSlot,selOn?"ON":"--",ss2alt[ss2Alt[ss2SelStep]]);
                        oled.drawStr(0,47,buf); }

                    // Slot detail
                    {   const char* fn=(ss2Path[ss2SelSlot][0])?
                            (strrchr(ss2Path[ss2SelSlot],'/')?strrchr(ss2Path[ss2SelSlot],'/')+1:ss2Path[ss2SelSlot]):"(empty)";
                        snprintf(buf,sizeof(buf),"Slot%02d:%s%s",
                                 ss2SelSlot,ss2Loaded[ss2SelSlot]?"":"!",fn);
                        oled.drawStr(0,54,buf); }

                    // Slot loaded bar: 16 dots
                    oled.drawHLine(0,57,128);
                    for(int i=0;i<SS2_SLOTS;i++){
                        int sx=i*8+2;
                        if(ss2Loaded[i])      oled.drawBox(sx,60,4,4);
                        else if(ss2Path[i][0]) oled.drawFrame(sx,60,4,4);
                        // else empty: nothing
                        if((uint8_t)i==ss2SelSlot) oled.drawPixel(sx+2,65);
                    }

                    oled.drawStr(0,79,"B3=Alt  B4=Seq/Pad");
                } else {
                    // ═══ SS2 PAD VIEW — always shows file browser ════
                    oled.setFont(u8g2_font_5x7_tf);
                    snprintf(buf,sizeof(buf),"SS2 Sl:%02d  %.18s",ss2SelSlot,sdPath.c_str());
                    oled.drawStr(0,7,buf);
                    oled.drawHLine(0,9,128);
                    oled.setFont(u8g2_font_4x6_tf);
                    for(int i=0;i<8&&(i+sdScroll)<sdFileCount;i++){
                        int fi=i+sdScroll; bool sel=(fi==sdCursor);
                        snprintf(buf,sizeof(buf),"%c%.26s",
                                 sel?'>':(sdFileIsDir[fi]?'[':' '),sdFiles[fi].c_str());
                        oled.drawStr(0,17+i*7,buf);
                        if(sel) oled.drawHLine(0,18+i*7,128);
                    }
                    oled.drawHLine(0,74,128);
                    // Slot status bar: loaded=filled, assigned=frame, empty=nothing
                    for(int i=0;i<SS2_SLOTS;i++){
                        int sx=i*8+2;
                        if(ss2Loaded[i])       oled.drawBox(sx,76,4,4);
                        else if(ss2Path[i][0]) oled.drawFrame(sx,76,4,4);
                        if((uint8_t)i==ss2SelSlot) oled.drawPixel(sx+2,81);
                    }
                    oled.drawStr(0,90,"Key=Asgn  B3=AutoMap  B4=Seq");
                }
                break;
            }
            // ---- MOD2 — PolyAnalog-inspired ----
            case MODE_MOD2: {
                oled.drawStr(0,0,"MOD2"); oled.drawHLine(0,9,128);
                oled.setFont(u8g2_font_4x6_tf);
                snprintf(buf,sizeof(buf),"OSC:%-4s  Cut:%4dHz",
                         mod2ShapeStepNames[mod2ShapeIdx],(int)mod2Cutoff);
                oled.drawStr(0,18,buf);
                snprintf(buf,sizeof(buf),"Reso:%.1f  Env:%-3s",mod2Reso,envNames[mod2EnvIdx]);
                oled.drawStr(0,27,buf);
                snprintf(buf,sizeof(buf),"LFO:%-6s %.1fHz d:%.1f",
                         mod2LfoModeNames[mod2LfoMode],mod2LfoRate,mod2LfoDepth);
                oled.drawStr(0,36,buf);
                snprintf(buf,sizeof(buf),"Mode:%-4s Oct:%d BPM:%d",
                         mod2PlayModeNames[mod2PlayMode],noteMap.getOctave(),bpm);
                oled.drawStr(0,45,buf);
                oled.drawStr(0,54,"JY=bend  JX=velocity");
                oled.setFont(u8g2_font_5x7_tf);
                oled.drawHLine(0,58,128);
                // Velocity bar (JX)
                {
                    float jx2=constrain(cachedJoyX/64.0f,-1.0f,1.0f);
                    int bw=(int)((jx2+1.0f)*0.5f*128.0f);
                    oled.drawFrame(0,60,128,6);
                    if(bw>2) oled.drawBox(0,60,bw,6);
                    snprintf(buf,sizeof(buf),"vel %.0f%%",(0.8f+jx2*0.6f)*100.0f);
                    oled.drawStr(0,74,buf);
                }
                snprintf(buf,sizeof(buf),"Vol:%.0f%%",pots[0].value*100);
                oled.drawStr(0,122,buf);
                break;
            }
            // ---- 303 — TB-303 emulation ----
            case MODE_303: {
                oled.setFont(u8g2_font_4x6_tf);
                { static const char* wn[]={"SIN","SQR","SAW","SUP","TRI","NSE","KS"};
                  uint8_t wi=(t303Wave<7)?t303Wave:0;
                  snprintf(buf,sizeof(buf),"303 %s/%s %dBPM%s%s%s",
                           t303Tones[t303ToneIdx].name, wn[wi], bpm,
                           t303AccentOn?" [A]":"",t303SlideOn?" [S]":"",
                           t303SustainOn?" [Sus]":""); }
                oled.drawStr(0,0,buf); oled.drawHLine(0,7,128);
                snprintf(buf,sizeof(buf),"Cut:%4dHz  Res:%.1f",(int)t303Cutoff,t303Reso);
                oled.drawStr(0,17,buf);
                snprintf(buf,sizeof(buf),"Mod:%4dHz  Dur:%.0f%%",(int)t303EnvMod,t303Duration*100.0f);
                oled.drawStr(0,26,buf);
                { float pk=constrain(t303Cutoff+t303EnvMod,80.0f,8000.0f);
                  snprintf(buf,sizeof(buf),"Oct:%+d  Pk:%.0fHz",t303Oct,pk); }
                oled.drawStr(0,35,buf);
                { float jx303=constrain(cachedJoyX/64.0f,-1.0f,1.0f);
                  float jy303=constrain(cachedJoyY/64.0f,-1.0f,1.0f);
                  snprintf(buf,sizeof(buf),"JX:vel%+.0f%%  JY:flt%+.0f%%",jx303*40.0f,jy303*100.0f);
                  oled.drawStr(0,44,buf); }
                if(t303CurrentNote>0){
                    static const char* nn[]={"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
                    snprintf(buf,sizeof(buf),">> %s%d%s",
                             nn[t303CurrentNote%12], t303CurrentNote/12-1,
                             t303SlideActive?" ~glide~":"");
                    oled.drawStr(0,55,buf);
                }
                oled.drawHLine(0,62,128);
                oled.drawStr(0,71,"P2=Wv P3=Rs P4=Mod P5=Dur P6=Ct B3=Tn");
                // ── Pot params in bigger font ────────────────────────
                oled.setFont(u8g2_font_6x10_tf);
                oled.drawHLine(0,74,128);
                snprintf(buf,sizeof(buf),"Cut:%-4d  Res:%.1f",(int)t303Cutoff,t303Reso);
                oled.drawStr(0,87,buf);
                snprintf(buf,sizeof(buf),"Dur:%.0f%%  Mod:%-4.1f",t303Duration*100.0f,t303EnvMod);
                oled.drawStr(0,101,buf);
                oled.setFont(u8g2_font_4x6_tf);
                snprintf(buf,sizeof(buf),"Vol:%.0f%%",pots[0].value*100);
                oled.drawStr(0,118,buf);
                break;
            }
            // ---- GRANULAR ----
            case MODE_GRANULAR: {
                static const char* kGranModeStr[4] = {"8-SL","1/16","8-LP","16LP"};
                oled.setFont(u8g2_font_4x6_tf);

                // Trigger slice computation when source finishes loading
                if (!granComputed && audioIsStreamingDone() && !audioIsGranularReady()) {
                    audioSetGranularWindow(granWinStart, granWinEnd);
                    granSliceCount = audioComputeGranularSlices(granSubMode & 1u, granWaveform);
                    granComputed = (granSliceCount > 0);
                    if (granComputed) {
                        for (int i = 0; i <= granSliceCount; i++) granSplits[i] = i / (float)granSliceCount;
                        granPotNeedsSync = true;
                    }
                }

                if (!audioIsStreamingDone()) {
                    // ---- LOADING PHASE ----
                    oled.drawStr(0,7,"GRAN  Loading...");
                    oled.drawHLine(0,9,128);
                    oled.drawStr(0,118,"Clk=load B1=mode B2=FX");
                    break;
                }

                if (!granComputed) {
                    // ---- BROWSE PHASE: no sample loaded yet ----
                    snprintf(buf,sizeof(buf),"GRAN [%s]", kGranModeStr[granSubMode]);
                    oled.drawStr(0,7,buf); oled.drawHLine(0,9,128);
                    // Show up to 13 files (7px line spacing, y=17 to y=101, then 118 for hint)
                    for (int i=0; i<13 && (sdScroll+i)<sdFileCount; i++) {
                        int fi = sdScroll+i;
                        bool sel = (fi==sdCursor);
                        snprintf(buf,sizeof(buf),"%c%.26s", sel?'>':(sdFileIsDir[fi]?'[':' '), sdFiles[fi].c_str());
                        oled.drawStr(0, 17+i*7, buf);
                        if (sel) oled.drawHLine(0, 18+i*7, 128);
                    }
                    oled.drawStr(0,118,"Joy=nav Clk=load B1=mode");
                    break;
                }

                // ---- PLAY PHASE: sample loaded ----
                snprintf(buf,sizeof(buf),"GRAN [%s] W:%.0f-%.0f%%",
                         kGranModeStr[granSubMode], granWinStart*100.f, granWinEnd*100.f);
                oled.drawStr(0,7,buf); oled.drawHLine(0,9,128);

                // Waveform (y 10-32, height 23px)
                for (int x=0;x<128;x++) {
                    uint8_t h = (uint8_t)(granWaveform[x] * 21 / 255);
                    if (h < 1) h=1;
                    oled.drawVLine(x, 32 - h, h);
                }
                // Window boundaries
                int wx0 = (int)(granWinStart * 128);
                int wx1 = (int)(granWinEnd   * 128) - 1;
                if (wx1 >= 128) wx1 = 127;
                int gww = wx1 - wx0;
                oled.drawVLine(wx0, 10, 23);
                oled.drawVLine(wx1, 10, 23);

                // Slice highlight using granSplits positions: XOR when key held, frame when selected
                if (granPlayingSlice >= 0 && granPlayingSlice < granSliceCount) {
                    int sx0 = wx0 + (int)(granSplits[granPlayingSlice]   * gww);
                    int sx1 = wx0 + (int)(granSplits[granPlayingSlice+1] * gww);
                    if (granKeyHeld) {
                        oled.setDrawColor(2);
                        oled.drawBox(sx0, 10, sx1 - sx0, 23);
                        oled.setDrawColor(1);
                    } else {
                        oled.drawFrame(sx0, 10, sx1 - sx0, 23);
                    }
                }

                // Split dividers + mini arrows below waveform (y=33-39)
                if (granSliceCount > 0) {
                    for (int i = 0; i <= granSliceCount; i++) {
                        int x = wx0 + (int)(granSplits[i] * gww);
                        oled.drawVLine(x, 10, 23);  // divider line through waveform
                        oled.drawVLine(x, 33, 4);   // tick mark below waveform
                    }
                    if (granPlayingSlice >= 0 && granPlayingSlice < granSliceCount) {
                        int sx0 = wx0 + (int)(granSplits[granPlayingSlice]   * gww);
                        int sx1 = wx0 + (int)(granSplits[granPlayingSlice+1] * gww);
                        oled.drawHLine(sx0+1, 37, sx1 - sx0 - 1);
                        int mx = (sx0 + sx1) / 2;
                        oled.drawPixel(mx, 38);
                        oled.drawHLine(mx-1, 39, 3);
                    }
                }

                // Info line (y=42): split positions when slice selected, row hint otherwise
                static const char* kGranRowHint[4] = {
                    "R0:px R1:px+px+1 R2:tail R3:rev",
                    "R0-1:1/16  R2-3:1/16Lp",
                    "All: 1/8 loop",
                    "All: 1/16 loop"
                };
                if (granPlayingSlice >= 0 && granPlayingSlice < granSliceCount) {
                    int x = granPlayingSlice;
                    snprintf(buf,sizeof(buf),"Sl%d S%d:%.0f%% S%d:%.0f%% P3/P4",
                             x, x, granSplits[x]*100.f, x+1, granSplits[x+1]*100.f);
                    oled.drawStr(0, 42, buf);
                } else {
                    oled.drawStr(0, 42, kGranRowHint[granSubMode]);
                }

                // File browser in play phase (8 entries at 7px spacing, y=51..105)
                for (int i=0; i<8 && (sdScroll+i)<sdFileCount; i++) {
                    int fi = sdScroll+i;
                    snprintf(buf,sizeof(buf),"%c%.26s", (fi==sdCursor)?'>':(sdFileIsDir[fi]?'[':' '), sdFiles[fi].c_str());
                    oled.drawStr(0, 51+i*7, buf);
                }
                oled.drawStr(0,118,"Joy=nav Clk=load B1=mode B2=FX");
                break;
            }
            // ---- GRANULAR2 ----
            case MODE_GRANULAR2: {
                oled.setFont(u8g2_font_4x6_tf);
                uint8_t numSamp = gran2NumSamples();
                uint8_t nSlices = gran2NumSlices();

                // Poll audio engine: mark loaded once streaming completes, then compute slices
                for (uint8_t s = 0; s < numSamp; s++) {
                    if (!gran2[s].path.isEmpty() && !gran2[s].loaded && audioIsGranular2Ready(s)) {
                        gran2[s].loaded = true;
                    }
                    if (gran2[s].loaded && !gran2[s].computed) {
                        gran2[s].sliceCount = audioComputeGranular2Slices(s, nSlices, gran2[s].waveform);
                        gran2[s].computed = (gran2[s].sliceCount > 0);
                        if (gran2[s].computed) {
                            for (int i = 0; i <= gran2[s].sliceCount; i++)
                                gran2[s].splits[i] = i / (float)gran2[s].sliceCount;
                            // Sync preset lengths to the current play mode right away,
                            // so LOP extended-length presets are ready without requiring a pot move.
                            audioApplyGranular2Splits(s, gran2[s].splits, gran2[s].sliceCount, gran2PlayMode == 1);
                            gran2PotNeedsSync = true;
                        }
                    }
                }

                // Header — include per-sample load status as compact chars after mode tag
                static const char* kGran2ModeStr[3] = {"NRM","LOP","FUL"};
                {
                    char st[5] = "----";
                    for (uint8_t s = 0; s < numSamp && s < 4; s++) {
                        if      (gran2[s].computed)                           st[s] = 'C';
                        else if (gran2[s].loaded)                             st[s] = 'R';
                        else if (!gran2[s].path.isEmpty())                    st[s] = 'L';
                        else                                                  st[s] = '-';
                    }
                    st[numSamp] = '\0';
                    snprintf(buf, sizeof(buf), "GR2[%s] T%d %s", kGran2ModeStr[gran2PlayMode], gran2LoadTarget, st);
                }
                oled.drawStr(0, 7, buf); oled.drawHLine(0, 9, 128);

                // Pick display sample: last played, else first computed, else none
                int8_t dispSamp = gran2ActiveSample;
                if (dispSamp < 0 || dispSamp >= (int8_t)numSamp || !gran2[dispSamp].computed) {
                    dispSamp = -1;
                    for (uint8_t s = 0; s < numSamp; s++) { if (gran2[s].computed) { dispSamp = (int8_t)s; break; } }
                }

                if (dispSamp < 0) {
                    // ---- BROWSE PHASE: no sample computed yet ----
                    // 13 entries from y=17, matching granular browse layout exactly
                    for (int i = 0; i < 13 && (sdScroll + i) < sdFileCount; i++) {
                        int fi = sdScroll + i;
                        bool sel = (fi == sdCursor);
                        snprintf(buf, sizeof(buf), "%c%.26s", sel ? '>' : (sdFileIsDir[fi] ? '[' : ' '), sdFiles[fi].c_str());
                        oled.drawStr(0, 17 + i * 7, buf);
                        if (sel) oled.drawHLine(0, 18 + i * 7, 128);
                    }
                } else {
                    // ---- PLAY PHASE: waveform + sample indicators + file browser ----
                    Gran2State& ds = gran2[dispSamp];
                    // Waveform (y=10..32, height 23px)
                    for (int x = 0; x < 128; x++) {
                        uint8_t h = (uint8_t)(ds.waveform[x] * 21 / 255);
                        if (h < 1) h = 1;
                        oled.drawVLine(x, 32 - h, h);
                    }
                    // Split dividers + tick marks
                    if (ds.sliceCount > 0) {
                        for (int i = 0; i <= ds.sliceCount; i++) {
                            int x = (int)(ds.splits[i] * 127.f);
                            oled.drawVLine(x, 10, 23);
                            oled.drawVLine(x, 33, 4);
                        }
                        // Highlight active slice
                        if (gran2ActiveSample == dispSamp && gran2ActiveSlice >= 0 && gran2ActiveSlice < ds.sliceCount) {
                            int sx0 = (int)(ds.splits[gran2ActiveSlice]   * 127.f);
                            int sx1 = (int)(ds.splits[gran2ActiveSlice+1] * 127.f);
                            oled.drawFrame(sx0, 10, sx1 - sx0, 23);
                        }
                    }
                    // Sample indicators (y=36..44): S0..S3 with status
                    oled.drawStr(0, 43, "S:");
                    for (uint8_t s = 0; s < numSamp; s++) {
                        int x = 12 + (int)s * 14;
                        snprintf(buf, sizeof(buf), "S%d", s);
                        if (s == (uint8_t)dispSamp) {
                            oled.drawBox(x - 1, 36, 11, 8); oled.setDrawColor(0);
                            oled.drawStr(x, 43, buf); oled.setDrawColor(1);
                        } else {
                            oled.drawStr(x, 43, buf);
                        }
                        if (gran2[s].computed)              oled.drawPixel(x + 4, 45);
                        else if (gran2[s].loaded)           oled.drawCircle(x + 4, 44, 2, U8G2_DRAW_ALL);
                        else if (!gran2[s].path.isEmpty())  oled.drawPixel(x + 3, 44); // loading dot
                    }
                    // Slice info (y=51)
                    if (gran2ActiveSample == dispSamp && gran2ActiveSlice >= 0 && gran2ActiveSlice < ds.sliceCount) {
                        snprintf(buf, sizeof(buf), "Sl%d S%d:%.0f%% S%d:%.0f%% P3/P4",
                                 gran2ActiveSlice,
                                 gran2ActiveSlice,   ds.splits[gran2ActiveSlice]   * 100.f,
                                 gran2ActiveSlice+1, ds.splits[gran2ActiveSlice+1] * 100.f);
                        oled.drawStr(0, 51, buf);
                    }
                    // File browser (7 entries from y=58, matching granular play-phase cadence)
                    for (int i = 0; i < 7 && (sdScroll + i) < sdFileCount; i++) {
                        int fi = sdScroll + i;
                        bool sel = (fi == sdCursor);
                        snprintf(buf, sizeof(buf), "%c%.26s", sel ? '>' : (sdFileIsDir[fi] ? '[' : ' '), sdFiles[fi].c_str());
                        oled.drawStr(0, 58 + i * 7, buf);
                        if (sel) oled.drawHLine(0, 59 + i * 7, 128);
                    }
                }
                // Bottom 2 lines: FX params when any FX is active, else button hints
                {
                    bool anyFxDisp = false;
                    for (uint8_t fi = 0; fi < FX_COUNT; fi++) if (fxList[fi].active) { anyFxDisp = true; break; }
                    if (anyFxDisp) {
                        const FxEffect& fx = fxList[fxSelected];
                        // Collect non-empty params and format values
                        struct { const char* name; char val[8]; } pp[4]; int np = 0;
                        for (int p = 0; p < 4; p++) {
                            if (!fx.paramNames[p][0]) continue;
                            float v = fx.params[p];
                            if      (v >= 1000.f) snprintf(pp[np].val, 8, "%.0fk", v / 1000.f);
                            else if (v >= 10.f)   snprintf(pp[np].val, 8, "%.0f",  v);
                            else                  snprintf(pp[np].val, 8, "%.2f",  v);
                            pp[np].name = fx.paramNames[p]; np++;
                        }
                        // Line 1 (y=111): FX name + first 2 params
                        char l1[32] = {}, l2[32] = {};
                        int o1 = snprintf(l1, sizeof(l1), "%s%s:", fx.active ? "*" : "-", fx.name);
                        for (int i = 0; i < np && i < 2; i++)
                            o1 += snprintf(l1 + o1, sizeof(l1) - o1, " %.3s=%s", pp[i].name, pp[i].val);
                        oled.drawStr(0, 111, l1);
                        // Line 2 (y=118): remaining params (3rd and 4th), or button hints if ≤2 params
                        if (np > 2) {
                            int o2 = 0;
                            for (int i = 2; i < np; i++)
                                o2 += snprintf(l2 + o2, sizeof(l2) - o2, " %.3s=%s", pp[i].name, pp[i].val);
                            oled.drawStr(0, 118, l2 + 1); // skip leading space
                        } else {
                            oled.drawStr(0, 118, "B1=FX B2=mode B3=Tgt B4=Clr");
                        }
                    } else {
                        oled.drawStr(0, 118, "B1=FX B2=mode B3=Tgt B4=Clr");
                    }
                }
                break;
            }
            // ---- TRACKER ----
            case MODE_TRACKER: {
                oled.setFont(u8g2_font_4x6_tf);
                const char* shapeLabel = (trkInstr < TRACKER_SYNTHS) ? shapeNames[trkSynthShape[trkInstr]] : "---";
                snprintf(buf,sizeof(buf),"TRK %dBPM %s T:%s[%s]",
                         bpm, trkPlaying?"[PLAY]":"[STOP]", kTrkInstrNames[trkInstr], shapeLabel);
                oled.drawStr(0,7,buf); oled.drawHLine(0,9,128);
                // REC indicator: inverted box when active
                if (trkRec) {
                    oled.setDrawColor(1);
                    oled.drawBox(96,0,32,8);
                    oled.setDrawColor(0);
                    oled.drawStr(98,7,"[REC]");
                    oled.setDrawColor(1);
                } else {
                    oled.drawStr(98,7,"[   ]");
                }
                // Step grid: 32 steps × 4px each = 128px, for selected track
                for (int s=0; s<TRACKER_STEPS; s++) {
                    int x = s * 4;
                    int noteCount = 0;
                    for (int ci=0;ci<TRK_CHORD_SIZE;ci++) if(trkNotes[trkInstr][s][ci]>=0) noteCount++;
                    bool cur = trkPlaying && (s == trkStep);
                    if (cur)              oled.drawBox(x, 10, 3, 10);
                    else if (noteCount>1) { oled.drawBox(x, 11, 3, 8); }  // chord: filled+extra dot
                    else if (noteCount>0) oled.drawFrame(x, 11, 3, 8);
                    else                  oled.drawPixel(x+1, 15);
                }
                // All tracks overview (tiny: 1 row per track, 32px wide = 1px/step)
                for (int t=0; t<TRACKER_TRACKS; t++) {
                    int y = 83 - t * 4;
                    for (int s=0; s<TRACKER_STEPS; s++) {
                        if (trkNotes[t][s][0] >= 0) oled.drawPixel(s * 4 + s/8, y);
                    }
                    if (t == trkInstr) oled.drawStr(100, y+3, kTrkInstrNames[t]);
                }
                snprintf(buf,sizeof(buf),"Ply Rec Oct%+d Clr  [2x=shape]", trkOctave);
                oled.drawStr(0,118,buf);
                snprintf(buf, sizeof(buf), "L4=instr(2x=snd)  R4=note", trkOctave);
                oled.drawStr(0,109,buf);
                break;
            }
            // ---- MIDI ----
            case MODE_MIDI: {
                // CC name helper (common names, fall back to number)
                auto ccLabel = [](uint8_t cc, char* out, int sz) {
                    static const struct { uint8_t n; const char* s; } tab[] = {
                        {0,"BankSel"},{1,"Mod"},{2,"Breath"},{4,"Foot"},{5,"Porta"},
                        {6,"DatEnt"},{7,"Vol"},{8,"Bal"},{10,"Pan"},{11,"Expr"},
                        {12,"FxC1"},{13,"FxC2"},{64,"Sust"},{65,"PortaOn"},
                        {71,"Reso"},{72,"Rel"},{73,"Atk"},{74,"Bright"},
                        {91,"Rev"},{93,"Cho"},{94,"Detune"},{95,"Phaser"}
                    };
                    for (auto& e : tab) if (e.n == cc) { snprintf(out,sz,"%s",e.s); return; }
                    snprintf(out, sz, "%d", cc);
                };
                oled.setFont(u8g2_font_5x7_tf);
                if (midiPotSel >= 0) {
                    // Config mode (pots + layout)
                    snprintf(buf,sizeof(buf),"MIDI CONFIG  ch%d", (int)midiChannel);
                    oled.drawStr(0,0,buf); oled.drawHLine(0,9,128);
                    oled.setFont(u8g2_font_4x6_tf);
                    char lbl[10];
                    for (int p = 0; p < 7; p++) {
                        int y = 18 + p * 10;
                        if (p == midiPotSel) oled.drawBox(0, y-7, 128, 9);
                        if (midiPotCC[p] > 127) {
                            snprintf(buf, sizeof(buf), "P%d  OFF", p);
                        } else {
                            ccLabel(midiPotCC[p], lbl, sizeof(lbl));
                            snprintf(buf, sizeof(buf), "P%d  CC%3d  %s", p, (int)midiPotCC[p], lbl);
                        }
                        if (p == midiPotSel) oled.setDrawColor(0);
                        oled.drawStr(2, y, buf);
                        if (p == midiPotSel) oled.setDrawColor(1);
                    }
                    // Row 7: layout
                    { int y = 18 + 7 * 10;
                      if (7 == midiPotSel) oled.drawBox(0, y-7, 128, 9);
                      snprintf(buf, sizeof(buf), "Layout  [%s]", kbdLayout==KBD_LAYOUT_PIANO?"Piano":"Grid");
                      if (7 == midiPotSel) oled.setDrawColor(0);
                      oled.drawStr(2, y, buf);
                      if (7 == midiPotSel) oled.setDrawColor(1); }
                    oled.drawStr(0,118,"B0=OK  B2=^ B3=v  B1/B4=Change");
                } else {
                    // Normal MIDI play mode
                    // ── Header: channel, octave, note range ──
                    uint8_t lo=(uint8_t)constrain((int)(midiOctave*12),   0,127);
                    uint8_t hi=(uint8_t)constrain((int)(midiOctave*12)+31,0,127);
                    static const char* nms[]={"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
                    snprintf(buf,sizeof(buf),"MIDI ch%d Oct%d%s %s%d-%s%d",
                        (int)midiChannel,(int)midiOctave,
                        kbdLayout==KBD_LAYOUT_PIANO?" [P]":"",
                        nms[lo%12],lo/12-1, nms[hi%12],hi/12-1);
                    oled.setFont(u8g2_font_5x7_tf);
                    oled.drawStr(0,8,buf);
                    oled.drawHLine(0,9,128);
                    oled.setFont(u8g2_font_4x6_tf);

                    // ── 7 vertical sliders ──
                    // Layout: slot=18px, bar=12px wide, frame height=65px, TOP=20 for CC label clearance
                    const int SLOT=18, BW=12, SH=65, TOP=20;
                    for (int p=0; p<7; p++) {
                        int sx = p * SLOT;
                        int bx = sx + (SLOT - BW) / 2;  // center bar in slot

                        // CC number above slider (centered)
                        if (midiPotCC[p] > 127)
                            snprintf(buf,sizeof(buf),"--");
                        else
                            snprintf(buf,sizeof(buf),"%d",(int)midiPotCC[p]);
                        int tw = (int)oled.getStrWidth(buf);
                        oled.drawStr(sx + (SLOT - tw) / 2, TOP - 3, buf);

                        // Slider frame
                        oled.drawFrame(bx, TOP, BW, SH);

                        if (midiPotCC[p] > 127) {
                            // Disabled: X inside frame
                            oled.drawLine(bx+2, TOP+2,    bx+BW-3, TOP+SH-3);
                            oled.drawLine(bx+2, TOP+SH-3, bx+BW-3, TOP+2);
                        } else {
                            // Fill bar from bottom up (clamped so P0/volume ≤200% can't overflow)
                            int fh = min((int)(pots[p].value * (float)(SH - 2)), SH - 2);
                            if (fh > 0)
                                oled.drawBox(bx+1, TOP+1+(SH-2-fh), BW-2, fh);
                        }

                        // Pot label below slider (centered)
                        snprintf(buf,sizeof(buf),"P%d",p);
                        tw = (int)oled.getStrWidth(buf);
                        oled.drawStr(sx + (SLOT - tw) / 2, TOP + SH + 8, buf);
                    }

                    // ── Footer ──
                    oled.drawStr(0, 101, "JY=PitchBend  JX=Mod");
                    oled.drawHLine(0, 105, 128);
                    oled.drawStr(0, 114, "B0=Ch B1=Sus B2/3=Oct B4=Off");
                    oled.drawStr(0, 123, "[HoldB0=PotConfig]");
                }
                break;
            }
            default:
                oled.drawStr(0,0,menuLabels[currentMode]);
                oled.drawStr(20,60,"Coming soon...");
                break;
        }
    }

    oled.sendBuffer();
}

// ==================== LED UPDATE TASK ====================
// Binary semaphore: given by handleNoteKeyAudio() on each key event so LEDs respond
// immediately, even when the main loop is starved by AMY fill-buffer (Core 1).
static SemaphoreHandle_t s_ledSem = nullptr;

static void updateLedsAndShow()
{
    for(int i=0;i<NUM_LEDS;i++) leds[i]=CRGB::Black;
    if(!menuOpen){
        switch(currentMode){
            case MODE_FX:
                // Active effects as colored LEDs, ordered by activation sequence
                for(uint8_t k=0;k<fxOrderCount;k++){
                    uint8_t i=fxOrderList[k];
                    int li=crdToIdx(i*2,0); if(li>=0&&li<NUM_LEDS) leds[li]=CHSV(i*60,255,255);
                }
                break;
            case MODE_SEQ: {
                // Same coordinate transform as all other modes:
                // track t = physical row t → gr = KBD_ROWS-1-t = 4-t
                // step  s = physical col 7-s → gc = KBD_COLS-1-(7-s) = s
                static const uint8_t trackHues[4]={0,85,170,42};
                for(int t=0;t<4;t++) for(int s=0;s<8;s++){
                    uint8_t track=seqPage*4+t;
                    int idx=(4-t)*KBD_COLS + s;
                    int li=crdToIdx(idx,0);
                    if(li<0||li>=NUM_LEDS) continue;
                    if(seqPattern[track][s]){
                        bool cur=(seqPlaying&&s==(int)seqStep);
                        leds[li]=CHSV(trackHues[t],255,cur?255:80);
                    } else if(seqPlaying&&s==(int)seqStep){
                        leds[li]=CHSV(trackHues[t],120,40);
                    }
                }
                break;
            }
            case MODE_LIGHTPLAY: {
                // Decay all LEDs
                for(int i=0;i<NUM_LEDS;i++) if(rippleBrightMap[i]>12) rippleBrightMap[i]-=12;
                else rippleBrightMap[i]=0;
                // Update ripples
                for(uint8_t r=0;r<8;r++){
                    if(ripples[r].bright<10) continue;
                    ripples[r].radius+=0.6f;
                    ripples[r].bright=(uint8_t)(ripples[r].bright>8?ripples[r].bright-8:0);
                    // Light LEDs near ripple ring
                    for(int i=0;i<NUM_LEDS;i++){
                        // Approximate distance using LED index distance
                        float dist=fabsf((float)i-(float)ripples[r].ledIdx);
                        if(dist>NUM_LEDS/2) dist=NUM_LEDS-dist;
                        float diff=fabsf(dist-ripples[r].radius);
                        if(diff<1.8f){
                            uint8_t b=(uint8_t)(ripples[r].bright*(1.0f-diff/1.8f));
                            if(b>rippleBrightMap[i]) rippleBrightMap[i]=b;
                        }
                    }
                }
                for(int i=0;i<NUM_LEDS;i++)
                    if(rippleBrightMap[i]>0)
                        leds[i]=CHSV((uint8_t)(i*17+80),200,rippleBrightMap[i]);
                break;
            }
            case MODE_LIGHT: {
                // Scroll N consecutive LEDs along the strip
                static float lightPos = 0.0f;
                static unsigned long lastLightMs = 0;
                unsigned long now = millis();
                float dt = (now - lastLightMs) / 1000.0f;
                lastLightMs = now;
                // Speed: pot4=0 → 0.1 LED/s; pot4=1 → 36 LED/s (quadratic)
                float speed = 0.1f + pots[4].value * pots[4].value * 35.9f;
                lightPos = fmodf(lightPos + speed * dt, (float)NUM_LEDS);

                uint8_t n   = max((uint8_t)1, (uint8_t)(pots[3].value * NUM_LEDS + 0.5f));
                uint8_t hue = (uint8_t)(pots[5].value * 255.0f);
                uint8_t bri = (uint8_t)(pots[6].value * 255.0f);
                for (uint8_t i = 0; i < n; i++) {
                    uint8_t idx = ((uint8_t)lightPos + i) % NUM_LEDS;
                    leds[idx] = CHSV(hue, 255, bri);
                }
                break;
            }
            case MODE_303: {
                for(int r=0;r<KBD_NOTE_ROWS;r++) for(int c=0;c<KBD_COLS;c++){
                    int gr=KBD_ROWS-1-r, gc=KBD_COLS-1-c, idx=gr*KBD_COLS+gc;
                    int li=(idx>=0&&idx<NUM_LEDS+4)?crdToIdx(idx,0):-1;
                    if(li<0||li>=NUM_LEDS) continue;
                    if (kbdLayout == KBD_LAYOUT_PIANO) {
                        int8_t off = pianoPad[r][c];
                        if (off < 0) { leds[li]=CRGB::Black; continue; }
                        bool white = (0xAB5 >> (off % 12)) & 1;
                        bool pr = keyState[r][c];
                        uint8_t thisNote = (uint8_t)constrain(
                            (int)(48 + noteMap.getOctave()*12) + off + (int)t303Oct*12, 0, 127);
                        bool playing = (thisNote == t303CurrentNote && t303CurrentNote != 0);
                        if (playing)     leds[li] = t303AccentOn ? CHSV(40,255,255) : CHSV(0,255,255);
                        else if (pr)     leds[li] = CHSV(15, 220, 180);
                        else             leds[li] = white ? CHSV(28,200,55) : CHSV(128,255,55);
                    } else {
                        uint8_t base = noteMap.getMidiNote(r, c);
                        uint8_t note = (uint8_t)constrain((int)base + (int)t303Oct*12, 0, 127);
                        bool playing = (note == t303CurrentNote && t303CurrentNote != 0);
                        bool pressed = keyState[r][c];
                        if(playing)      leds[li] = t303AccentOn ? CHSV(40,255,255) : CHSV(0,255,255);
                        else if(pressed) leds[li] = CHSV(15, 220, 180);
                        else             leds[li] = CHSV(10, 180, 15);
                    }
                }
                break;
            }
            case MODE_DRUM2: {
                // Pad hue = rainbow across 8 pads (uniform step of 32)
                // so each pad has a clearly distinct but logically ordered color
                unsigned long now = millis();
                for(int r=0;r<KBD_NOTE_ROWS;r++) for(int c=0;c<KBD_COLS;c++){
                    int gr=KBD_ROWS-1-r, gc=KBD_COLS-1-c, idx=gr*KBD_COLS+gc;
                    int li=(idx>=0&&idx<NUM_LEDS+4)?crdToIdx(idx,0):-1;
                    if(li<0||li>=NUM_LEDS) continue;
                    // col=7(left)=pad0(KK1) … col=0(right)=pad7(RDE)
                    uint8_t padIdx = (uint8_t)(KBD_COLS - 1 - c);
                    if (padIdx >= DRUM2_PADS) { leds[li]=CRGB::Black; continue; }
                    uint8_t hue = (uint8_t)(padIdx * 32);
                    if (drum2SeqView) {
                        if (r >= 2) {
                            // Rows 2+3: unified step grid, pitch-based color when offset ≠ 0
                            uint8_t step = (uint8_t)((r == 3 ? 0 : 8) + (KBD_COLS - 1 - c));
                            if (step >= DR2_STEPS) { leds[li] = CRGB::Black; continue; }
                            uint8_t ph   = (uint8_t)(drum2SelPad * 32);
                            bool active  = (drum2SeqVel[drum2SelPad][step] > 0);
                            bool curStep = drum2Playing && (step == drum2Step);
                            int8_t poff  = active ? drum2SeqPitchOff[drum2SelPad][step] : 0;
                            // Pitch-based hue
                            uint8_t ledHue = ph;
                            if (active && poff != 0) {
                                if      (poff > 7)   ledHue = 0;    // red: high
                                else if (poff > 0)   ledHue = 35;   // amber
                                else if (poff < -7)  ledHue = 180;  // blue: low
                                else                 ledHue = 130;  // cyan
                            }
                            // Modifier color: 0=pad 1=prob50%(amber) 2=randpitch(magenta) 3=double(cyan)
                            static const uint8_t kModHue[4] = {0, 40, 213, 128};
                            if (active && drum2SeqMod[drum2SelPad][step] > 0)
                                ledHue = kModHue[drum2SeqMod[drum2SelPad][step]];
                            if (keyState[r][c])        leds[li] = CHSV(ph,    200, 255);
                            else if (curStep && active) leds[li] = CHSV(0,      0, 255);
                            else if (curStep)           leds[li] = CHSV(ph,    255,  80);
                            else if (active)            leds[li] = CHSV(ledHue, 240, 140);
                            else                        leds[li] = CHSV(ph,    220,  10);
                        } else if (r == 1) {
                            // Row 1: ALTERED note row — color shows alteration mode for each pad
                            // 0=normal(green) 1=prob50%(amber) 2=randpitch(magenta) 3=double(cyan)
                            static const uint8_t kAltHue[4] = {80, 40, 213, 128};
                            uint8_t phue = kAltHue[drum2PadAltMode[padIdx]];
                            bool isSelPad = (padIdx == (uint8_t)drum2SelPad);
                            if (keyState[r][c])   leds[li] = CHSV(phue, 220, 255);
                            else if (isSelPad)    leds[li] = CHSV(phue, 220, 140);
                            else                  leds[li] = CHSV(phue, 220,  25);
                        } else {
                            // Row 0: pad selector (play/audition)
                            bool justHit = (now - drum2PadFlashMs[padIdx] < 80);
                            if (keyState[r][c] || justHit)          leds[li] = CHSV(hue, 180, 255);
                            else if (padIdx == (uint8_t)drum2SelPad) leds[li] = CHSV(hue, 200, 120);
                            else                                      leds[li] = CHSV(hue, 220,  20);
                        }
                    } else {
                        // Pad view: col=pad, row=velocity layers
                        bool justHit = (now - drum2PadFlashMs[padIdx] < 80);
                        if (keyState[r][c] || justHit)
                            leds[li] = CHSV(hue, 180, 255);   // pressed or seq-triggered
                        else if (padIdx == (uint8_t)drum2SelPad)
                            leds[li] = CHSV(hue, 200,  80);   // selected pad dim glow
                        else
                            leds[li] = CHSV(hue, 220,  25);   // idle
                    }
                }
                break;
            }
            case MODE_SYSEQ: {
                for(int r=0;r<KBD_NOTE_ROWS;r++) for(int c=0;c<KBD_COLS;c++){
                    int gr=KBD_ROWS-1-r, gc=KBD_COLS-1-c, idx=gr*KBD_COLS+gc;
                    int li=(idx>=0&&idx<NUM_LEDS+4)?crdToIdx(idx,0):-1;
                    if(li<0||li>=NUM_LEDS) continue;
                    if (syseqSeqView) {
                        if (r >= 2) {
                            // Top 2 rows: step selector (r=3→steps 0-7, r=2→steps 8-15)
                            uint8_t step = (uint8_t)((r==3?0:8)+(KBD_COLS-1-c));
                            uint8_t cnt=0; for(uint8_t i=0;i<SYSEQ_CHORD;i++) if(syseqNotes[step][i]) cnt++;
                            bool curStep = drum2Playing && (step==(uint8_t)drum2Step);
                            bool selStep = (step==syseqSelStep);
                            if (keyState[r][c])        leds[li] = CHSV(0,   0, 255);    // white on press
                            else if (curStep && cnt)   leds[li] = CHSV(0,   0, 255);    // white beat
                            else if (curStep)          leds[li] = CHSV(170, 255,  80);  // dim cursor
                            else if (selStep && cnt)   leds[li] = CHSV(170, 180, 220);  // selected+notes
                            else if (selStep)          leds[li] = CHSV(170, 120,  80);  // selected empty
                            else if (cnt)              leds[li] = CHSV(170, 240, 100);  // has notes
                            else                       leds[li] = CHSV(170, 255,   8);  // empty
                        } else {
                            // Bottom 2 rows: hold-to-play keyboard (sequential 2-row mapping)
                            uint8_t note = noteMap.getMidiNoteByIdx((KBD_COLS-1-c)*2+r);
                            bool isSeqPlaying = false;
                            for(uint8_t i=0;i<syseqActiveCnt;i++) if(syseqActive[i]==note) { isSeqPlaying=true; break; }
                            bool inSelStep = false;
                            for(uint8_t i=0;i<SYSEQ_CHORD;i++) if(syseqNotes[syseqSelStep][i]==note+1) { inSelStep=true; break; }
                            if (keyState[r][c] || isSeqPlaying) leds[li] = CHSV(60, 220, 255);
                            else if (inSelStep)                  leds[li] = CHSV(60, 200, 110);
                            else                                 leds[li] = CHSV(60, 255,   8);
                        }
                    } else {
                        // PAD view: noteMap piano keyboard
                        uint8_t note = noteMap.getMidiNote(r, c);
                        bool playing = false;
                        for(uint8_t i=0;i<syseqActiveCnt;i++) if(syseqActive[i]==note) { playing=true; break; }
                        if (keyState[r][c])    leds[li] = CHSV(170, 200, 255);
                        else if (playing)      leds[li] = CHSV(170, 200, 120);
                        else                   leds[li] = CHSV(170, 255,  15);
                    }
                }
                break;
            }
            case MODE_303S: {
                for(int r=0;r<KBD_NOTE_ROWS;r++) for(int c=0;c<KBD_COLS;c++){
                    int gr=KBD_ROWS-1-r, gc=KBD_COLS-1-c, idx=gr*KBD_COLS+gc;
                    int li=(idx>=0&&idx<NUM_LEDS+4)?crdToIdx(idx,0):-1;
                    if(li<0||li>=NUM_LEDS) continue;
                    if (s303SeqView) {
                        if (r >= 2) {
                            // Top 2 rows: step grid
                            // Color by per-step alteration: NRM=red(0), ACC=orange(40), SLD=teal(130), SIL=dim
                            uint8_t step=(uint8_t)((r==3?0:8)+(KBD_COLS-1-c));
                            bool hasNote=(s303Note[step]>0);
                            bool curStep=drum2Playing&&(step==(uint8_t)drum2Step);
                            bool selStep=(step==s303SelStep);
                            uint8_t alt=s303Alt[step];
                            static const uint8_t aH[]={0,40,130,0};
                            static const uint8_t aS[]={240,240,240,30};
                            if (keyState[r][c])        leds[li] = CHSV(0,   0, 255);
                            else if (curStep&&hasNote)  leds[li] = CHSV(aH[alt],aS[alt],220);
                            else if (curStep)           leds[li] = CHSV(0,  180,  60);
                            else if (selStep&&hasNote)  leds[li] = CHSV(aH[alt],aS[alt],160);
                            else if (selStep)           leds[li] = CHSV(30, 120,  60);
                            else if (hasNote)           leds[li] = CHSV(aH[alt],aS[alt], 80);
                            else                        leds[li] = CHSV(0,  255,   6);
                        } else {
                            // Bottom 2 rows: note keyboard — selected note bright, seq playing bright
                            uint8_t note=noteMap.getMidiNoteByIdx((KBD_COLS-1-c)*2+r);
                            uint8_t noteWOct=(uint8_t)constrain((int)note+(int)t303Oct*12,0,127);
                            bool isSel=(s303SelNote==note+1);
                            bool seqPlay=(noteWOct==s303CurNote&&s303CurNote!=0);
                            if (keyState[r][c]||seqPlay) leds[li] = CHSV(0, 220, 255);
                            else if (isSel)              leds[li] = CHSV(0, 180, 140);
                            else                         leds[li] = CHSV(0, 255,   8);
                        }
                    } else {
                        // PAD view: 303-style keyboard (orange/red theme)
                        uint8_t note = noteMap.getMidiNote(r, c);
                        uint8_t noteWOct=(uint8_t)constrain((int)note+(int)t303Oct*12,0,127);
                        bool playing = (noteWOct==t303CurrentNote && t303CurrentNote!=0);
                        bool seqPlay = (noteWOct==s303CurNote && s303CurNote!=0);
                        if (keyState[r][c])       leds[li] = t303AccentOn ? CHSV(40,255,255) : CHSV(0,255,200);
                        else if (playing||seqPlay) leds[li] = CHSV(0, 255, 120);
                        else                       leds[li] = CHSV(0, 200,  12);
                    }
                }
                break;
            }
            case MODE_SS2: {
                // 16 slots spread across 2×8 rows. Unique hue per slot (16 even steps across 256).
                // Step grid in SEQ view uses slot hue; PAD view shows slot status.
                for(int r=0;r<KBD_NOTE_ROWS;r++) for(int c=0;c<KBD_COLS;c++){
                    int gr=KBD_ROWS-1-r, gc=KBD_COLS-1-c;
                    int idx=gr*KBD_COLS+gc;
                    if(idx<0||idx>=NUM_LEDS+4) continue;
                    int li=crdToIdx(idx,0);
                    if(li<0||li>=NUM_LEDS) continue;

                    if (ss2SeqView) {
                        if (r >= 2) {
                            // Top 2 rows: 16 step grid
                            uint8_t step=(uint8_t)((r==3?0:8)+(KBD_COLS-1-c));
                            bool hasSlot=(ss2Note[step]!=0);
                            bool curStep=drum2Playing&&(step==(uint8_t)drum2Step);
                            bool selOn=(ss2Note[step]&(1u<<ss2SelSlot))!=0;
                            uint8_t alt=ss2Alt[step];
                            uint8_t dispSlot = hasSlot
                                ? ((ss2Note[step] & (1u<<ss2SelSlot)) ? ss2SelSlot : (uint8_t)__builtin_ctz(ss2Note[step]))
                                : 0;
                            uint8_t slotH = (uint8_t)(dispSlot*16);
                            uint8_t selH  = (uint8_t)(ss2SelSlot*16);
                            static const uint8_t aS2[]={240,240,240,30};
                            if (keyState[r][c])             leds[li] = CHSV(0,   0, 255);
                            else if (curStep && hasSlot)    leds[li] = CHSV(slotH,aS2[alt],220);
                            else if (curStep)               leds[li] = CHSV(0,   60, 110);
                            else if (selOn)                 leds[li] = CHSV(selH,aS2[alt],110);
                            else                            leds[li] = CHSV(0,  255,   6);
                        } else {
                            // Bottom 2 rows: slot selector (2×8 = 16 slots)
                            uint8_t slot=(uint8_t)((KBD_COLS-1-c)*2+r);
                            if(slot>=SS2_SLOTS){ leds[li]=CHSV(0,0,0); continue; }
                            uint8_t slotH=(uint8_t)(slot*16);
                            bool slotPlaced=false;
                            for(uint8_t s=0;s<SS2_SLOTS;s++) if(ss2Note[s]&(1u<<slot)){slotPlaced=true;break;}
                            bool slotPlaying=(slot==ss2CurSlot&&ss2CurSlot!=0xFF);
                            if (keyState[r][c]||slotPlaying) leds[li]=CHSV(slotH,240,255);
                            else if(slotPlaced)               leds[li]=CHSV(slotH,220,110);
                            else                              leds[li]=CHSV(0,  255,  4);
                        }
                    } else {
                        // PAD view: 2×8 slot grid (rows 0-1), dim on rows 2-3
                        if (r >= 2) { leds[li]=CHSV(0,0,4); continue; }
                        uint8_t slot=(uint8_t)((KBD_COLS-1-c)*2+r);
                        if(slot>=SS2_SLOTS){ leds[li]=CHSV(0,0,0); continue; }
                        uint8_t slotH=(uint8_t)(slot*16);
                        bool slotPlaced=false;
                        for(uint8_t s=0;s<SS2_SLOTS;s++) if(ss2Note[s]&(1u<<slot)){slotPlaced=true;break;}
                        bool slotPlaying=(slot==ss2CurSlot&&ss2CurSlot!=0xFF);
                        if (keyState[r][c]||slotPlaying) leds[li]=CHSV(slotH,240,255);
                        else if(slotPlaced)               leds[li]=CHSV(slotH,220,110);
                        else                              leds[li]=CHSV(0,  255,  4);
                    }
                }
                break;
            }
            case MODE_MIDI: {
                // LEDs: host NoteOn/Off (LaunchPad) > physical press > piano layout background
                for(int r=0;r<KBD_ROWS;r++) for(int c=0;c<KBD_COLS;c++){
                    int gr=KBD_ROWS-1-r, gc=KBD_COLS-1-c;
                    int idx=gr*KBD_COLS+gc;
                    if(idx<0||idx>=NUM_LEDS+4) continue;
                    int li=crdToIdx(idx,0);
                    if(li<0||li>=NUM_LEDS) continue;
                    if(midiLedVel[r][c] > 0){
                        uint8_t h = (uint8_t)((uint32_t)midiLedVel[r][c] * 170 / 127);
                        leds[li] = CHSV(h, 255, 255);
                    } else if(keyState[r][c]){
                        bool silent = (kbdLayout==KBD_LAYOUT_PIANO && r<KBD_NOTE_ROWS && pianoPad[r][c]<0);
                        if (!silent) leds[li] = CHSV(midiOctave * 20, 200, 200);
                    } else if (kbdLayout == KBD_LAYOUT_PIANO && r < KBD_NOTE_ROWS) {
                        int8_t off = pianoPad[r][c];
                        if (off < 0) continue;
                        bool white = (0xAB5 >> (off % 12)) & 1;
                        leds[li] = white ? CHSV(28,200,55) : CHSV(128,255,55);
                    }
                }
                break;
            }
            case MODE_GRANULAR: {
                if(!granComputed){
                    // Browser phase: show nothing (file list on OLED)
                    break;
                }
                // Highlight active slice regions per row based on sub-mode
                for(int r=0;r<KBD_NOTE_ROWS;r++) for(int c=0;c<KBD_COLS;c++){
                    int gr=KBD_ROWS-1-r, gc=KBD_COLS-1-c;
                    int idx=gr*KBD_COLS+gc;
                    if(idx<0||idx>=NUM_LEDS+4) continue;
                    int li=crdToIdx(idx,0);
                    if(li<0||li>=NUM_LEDS) continue;
                    // dim base colour: row 0=green, 1=cyan, 2=blue, 3=magenta (reverse)
                    static const uint8_t granHues[4]={85,128,170,213};
                    uint8_t bri = keyState[r][c] ? 255 : 30;
                    leds[li] = CHSV(granHues[r], 200, bri);
                }
                break;
            }
            case MODE_GRANULAR2: {
                // Per-sample hue: S0=green, S1=cyan, S2=blue, S3=magenta; fwd=brighter, rev=dimmer
                static const uint8_t g2Hues[4] = {85, 128, 170, 213};
                uint8_t g2NumSamp = gran2NumSamples();
                // NRM: dim idle glow shows loaded samples. LOOP/FULL: dark unless pressed.
                bool g2IdleOn = (gran2PlayMode == 0);
                for (int r = 0; r < KBD_NOTE_ROWS; r++) for (int c = 0; c < KBD_COLS; c++) {
                    uint8_t si, sl; bool rev;
                    gran2KeyInfo((uint8_t)r, (uint8_t)c, si, sl, rev);
                    int gr = KBD_ROWS - 1 - r, gc = KBD_COLS - 1 - c;
                    int idx = gr * KBD_COLS + gc;
                    int li = (idx >= 0 && idx < NUM_LEDS + 4) ? crdToIdx(idx, 0) : -1;
                    if (li < 0 || li >= NUM_LEDS) continue;
                    if (si >= g2NumSamp || !gran2[si].computed) { leds[li] = CHSV(0, 0, 5); continue; }
                    // If reverse buffer failed to allocate (PSRAM exhausted), show faint gray.
                    if (rev && !audioGranular2HasReverse(si)) { leds[li] = CHSV(0, 220, keyState[r][c] ? 80u : 15u); continue; }
                    uint8_t hue = g2Hues[si];
                    uint8_t sat = rev ? 180u : 220u;
                    uint8_t bri = keyState[r][c] ? 255u : (g2IdleOn ? (rev ? 20u : 40u) : 0u);
                    leds[li] = CHSV(hue, sat, bri);
                }
                // Highlight load target sample rows with a dim amber outline
                for (int r = 0; r < KBD_NOTE_ROWS; r++) for (int c = 0; c < KBD_COLS; c++) {
                    uint8_t si, sl; bool rev;
                    gran2KeyInfo((uint8_t)r, (uint8_t)c, si, sl, rev);
                    if (si != gran2LoadTarget) continue;
                    int gr = KBD_ROWS - 1 - r, gc = KBD_COLS - 1 - c;
                    int idx = gr * KBD_COLS + gc;
                    int li = (idx >= 0 && idx < NUM_LEDS + 4) ? crdToIdx(idx, 0) : -1;
                    if (li < 0 || li >= NUM_LEDS) continue;
                    if (!gran2[si].computed && !keyState[r][c]) leds[li] = CHSV(35, 200, 15);
                }
                break;
            }
            case MODE_TRACKER: {
                // Left 4×4 (cols 0-3): instrument selector
                for(int r=0;r<KBD_NOTE_ROWS;r++) for(int c=0;c<4;c++){
                    uint8_t instrIdx = (uint8_t)(r*4 + c);  // matches key handler: trkInstr=row*4+col
                    if(instrIdx >= TRACKER_TRACKS) continue;
                    int gr=KBD_ROWS-1-r, gc=KBD_COLS-1-c;
                    int idx=gr*KBD_COLS+gc, li=crdToIdx(idx,0);
                    if(li<0||li>=NUM_LEDS) continue;
                    bool sel = (instrIdx == trkInstr);
                    uint8_t hue = (instrIdx < TRACKER_SYNTHS) ? (uint8_t)(instrIdx*18)
                                : (instrIdx == TRACKER_DRUM_TRK) ? 0
                                : (uint8_t)(140 + (instrIdx-TRACKER_SAMP_BASE)*14);
                    leds[li] = sel ? CHSV(35,255,255) : CHSV(hue,200,60);
                }
                // Right 4×4 (cols 4-7): note grid, flash current step
                for(int r=0;r<KBD_NOTE_ROWS;r++) for(int c=4;c<KBD_COLS;c++){
                    int gr=KBD_ROWS-1-r, gc=KBD_COLS-1-c;
                    int idx=gr*KBD_COLS+gc, li=crdToIdx(idx,0);
                    if(li<0||li>=NUM_LEDS) continue;
                    leds[li] = keyState[r][c] ? CHSV(85,255,255) : CHSV(85,200,15);
                }
                break;
            }
            default:
                for(int r=0;r<KBD_NOTE_ROWS;r++) for(int c=0;c<KBD_COLS;c++){
                    int gr=KBD_ROWS-1-r, gc=KBD_COLS-1-c, idx=gr*KBD_COLS+gc;
                    if(idx<0||idx>=NUM_LEDS+4) continue;
                    int li=crdToIdx(idx,0);
                    if(li<0||li>=NUM_LEDS) continue;
                    if (kbdLayout == KBD_LAYOUT_PIANO) {
                        int8_t off = pianoPad[r][c];
                        if (off < 0) { leds[li]=CRGB::Black; continue; }
                        bool white = (0xAB5 >> (off % 12)) & 1;
                        bool pr = keyState[r][c];
                        leds[li] = white ? (pr ? CHSV(28,200,255) : CHSV(28,200,55))
                                         : (pr ? CHSV(128,255,255) : CHSV(128,255,55));
                    } else if(keyState[r][c]) {
                        leds[li]=CHSV((r*8+c)*37+80,255,255);
                    }
                }
                break;
        }
    }

    // ---- OVERLAY LEDs ----
    // INSTR: toutes les 32 touches de notes, hue unique par shape, amber pour le sélectionné
    // Autres overlays: cols 4-7 (4-col) ou 6-7 (2-col); même règle de couleur
    if (s_overlay == OVERLAY_INSTR) {
        for (uint8_t r2 = 0; r2 < KBD_NOTE_ROWS; r2++) {
            for (uint8_t c2 = 0; c2 < KBD_COLS; c2++) {
                uint8_t opt = (uint8_t)((3u - r2) * 8u + (7u - c2));
                if (opt >= (uint8_t)SHAPE_COUNT) continue;
                int gr = KBD_ROWS - 1 - (int)r2;
                int gc = KBD_COLS - 1 - (int)c2;
                int idx = gr * KBD_COLS + gc;
                int ledIdx = (idx >= 0 && idx < NUM_LEDS + 4) ? crdToIdx(idx, 0) : -1;
                if (ledIdx < 0 || ledIdx >= NUM_LEDS) continue;
                bool sel = (currentShape == (SynthShape)opt);
                leds[ledIdx] = sel ? CHSV(35, 255, 255) : CHSV((uint8_t)(opt * 8), 255, 255);
            }
        }
    } else if (s_overlay != OVERLAY_NONE) {
        bool ovl4col = (s_overlay == OVERLAY_SCALE_ARP || s_overlay == OVERLAY_303 || s_overlay == OVERLAY_303_PRESET);
        uint8_t nOpts = ovl4col ? 16 : 8;
        for (uint8_t opt = 0; opt < nOpts; opt++) {
            uint8_t r = (uint8_t)(3 - (opt & 3));
            uint8_t colRank = opt >> 2;  // 0→col7, 1→col6, 2→col5, 3→col4
            uint8_t c = (uint8_t)(7 - colRank);
            int gr = KBD_ROWS - 1 - (int)r;
            int gc = KBD_COLS - 1 - (int)c;
            int idx = gr * KBD_COLS + gc;
            int ledIdx = (idx >= 0 && idx < NUM_LEDS + 4) ? crdToIdx(idx, 0) : -1;
            if (ledIdx < 0 || ledIdx >= NUM_LEDS) continue;
            bool show = false, sel = false; uint8_t hue = 0;
            switch (s_overlay) {
                case OVERLAY_FX:
                    if (opt < FX_COUNT) { show=true; sel=fxList[opt].active; hue=(uint8_t)(opt*32); }
                    break;
                case OVERLAY_SCALE_ARP:
                    if (opt < 4 && opt < (uint8_t)SCALE_COUNT)  { show=true; sel=(noteMap.getScale()==(Scale)opt);         hue=100; }
                    else if (opt >= 4 && opt < 8) {
                        if (opt == 6)                             { show=true; sel=(kbdLayout==KBD_LAYOUT_PIANO);            hue=40;  }
                        else if ((opt-4+4) < (uint8_t)SCALE_COUNT){ show=true; sel=(noteMap.getScale()==(Scale)(opt-4+4)); hue=100; }
                    }
                    else if (opt >= 8 && opt < 12)  { show=true; sel=(arpMode==opt-7);                  hue=20;  }
                    else if (opt >= 12 && opt < 16) { show=true; sel=(noteMap.getOctave()==kOctOpts[opt-12]); hue=200; }
                    break;
                case OVERLAY_ENV:
                    if (opt < ENV_PRESET_COUNT) { show=true; sel=(currentEnv==(EnvPreset)opt); hue=170; }
                    else if (opt >= 12 && opt < 16) { show=true; sel=(noteMap.getOctave()==kOctOpts[opt-12]); hue=200; }
                    break;
                case OVERLAY_SEQ_OPT:
                    if (opt < 3)          { show=true; sel=((uint8_t)seqPlayMode==opt); hue=150; }
                    else if (opt==3||opt==4) { show=true; sel=false; hue=60; }
                    break;
                case OVERLAY_SAMP_OPT:
                    if (opt < 4)  { show=true; sel=((uint8_t)samplePlayMode==opt); hue=60; }
                    else if (opt==4) { show=true; sel=false; hue=0; }
                    break;
                case OVERLAY_303:
                    if      (opt==0)             { show=true; sel=(t303Wave==SAW_DOWN);                hue=0;   }
                    else if (opt==1)             { show=true; sel=(t303Wave==PULSE);                   hue=0;   }
                    else if (opt==2)             { show=true; sel=t303SlideOn;                         hue=150; }
                    else if (opt==3)             { show=true; sel=t303AccentOn;                        hue=40;  }
                    else if (opt>=4 && opt<=7)   { show=true; sel=(t303Oct==(int8_t)((int)opt-6));    hue=200; }
                    else if (opt>=8 && opt<=11)  { show=true; sel=(arpMode==(int)(opt-8));             hue=20;  }
                    break;
                case OVERLAY_303_PRESET:
                    if (opt < T303_TONE_COUNT) { show=true; sel=(t303ToneIdx==opt); hue=(uint8_t)(opt*20+60); }
                    break;
                case OVERLAY_SYSEQ:
                    if (opt < 8)                { show=true; sel=(currentShape==(SynthShape)opt); hue=0;   }
                    else if (opt < 12)          { show=true; sel=(noteMap.getOctave()==kOctOpts[opt-8]); hue=200; }
                    else if (opt < 16)          { show=true; sel=(arpMode==(int)(opt-12)); hue=20;  }
                    break;
                default: break;
            }
            // 100% brightness; amber pour sélectionné, hue catégorie pour disponible
            if (show) leds[ledIdx] = sel ? CHSV(35, 255, 255) : CHSV(hue, 255, 255);
        }
    }

    FastLED.show();
}

static void ledUpdateTask(void*)
{
    for (;;) {
        // Wake immediately on key press or after 20ms (50Hz for animations).
        xSemaphoreTake(s_ledSem, pdMS_TO_TICKS(20));
        updateLedsAndShow();
    }
}

// ==================== STATS TASK ====================
static volatile uint32_t s_mainLoopCount = 0;  // incremented each loop() iteration

// Prints system health to Serial every second: temperature, heap, keyboard and main-loop stats.
// Runs at priority 1 on Core 0 — only executes when everything else is idle.
static void statsTask(void*)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        float    temp  = temperatureRead();
        uint32_t freeH = esp_get_free_heap_size();
        uint32_t minH  = esp_get_minimum_free_heap_size();

        uint32_t kbdPolls = 0, kbdMaxIv = 0;
        kbdGetStats(kbdPolls, kbdMaxIv);

        uint32_t loopHz = s_mainLoopCount;
        s_mainLoopCount = 0;

        Serial.printf("[SYS] %.0f°C  heap=%lu(min=%lu)  kbd=%lu/s maxIv=%lums  loop=%lu/s\n",
                      temp, freeH, minH, kbdPolls, kbdMaxIv, loopHz);
    }
}

// ==================== AUDIO EVENT BRIDGE ====================
// kbdEventBridge runs on Core 0 (kbdPollTask, priority 8).
// It only enqueues the event and wakes the LED task — no AMY calls here.
// AMY calls happen in audioHandlerTask (Core 1, priority 22) to avoid racing with
// AMY render (Core 0, priority 23) which preempts Core-0 tasks mid-write.
static QueueHandle_t s_audioEventQueue = nullptr;

static void kbdEventBridge(uint8_t row, uint8_t col, bool pressed)
{
    if (s_audioEventQueue) {
        KeyEvent evt = {row, col, pressed};
        xQueueSend(s_audioEventQueue, &evt, 0);  // non-blocking
    }
    if (s_ledSem) xSemaphoreGive(s_ledSem);
}

// ==================== NOTE KEY AUDIO HANDLER ====================
// Called from audioHandlerTask (Core 1, priority 22).
// Running on Core 1 keeps AMY state writes on the same core as before,
// avoiding races with AMY render (Core 0). Priority 22 beats the main loop (1)
// so events are processed as soon as AMY fill buffer (23) yields.
static void handleNoteKeyAudio(uint8_t row, uint8_t col, bool pressed)
{
    if (menuOpen || !audioReady || s_overlay != OVERLAY_NONE) return;
    switch(currentMode){
        case MODE_SYNTH:{
            uint8_t note;
            if (kbdLayout == KBD_LAYOUT_PIANO) {
                note = pianoNote(row, col, (uint8_t)(48 + noteMap.getOctave()*12));
                if (note == 0xFF) return;
            } else {
                note = noteMap.getMidiNote(row, col);
            }
            activeNotes[row][col]=pressed?note:0;
            if(arpMode==0){
                if(pressed){
                    float jx=constrain(cachedJoyX/64.0f,-1.0f,1.0f);
                    // Scale velocity by 1/sqrt(N) to prevent clipping with polyphony
                    int held=0;
                    for(int rr=0;rr<KBD_NOTE_ROWS;rr++) for(int cc=0;cc<KBD_COLS;cc++) if(activeNotes[rr][cc]) held++;
                    float polyScale = 1.0f / sqrtf(fmaxf(1.0f, (float)held));
                    audioNoteOn(note, constrain((0.8f+jx*0.6f) * polyScale, 0.05f, 1.0f));
                } else audioNoteOff(note);
            } else {
                // Arp: add/remove from held notes list
                if(pressed){
                    bool found=false;
                    for(uint8_t i=0;i<arpNoteCount;i++) if(arpNotes[i]==note){found=true;break;}
                    if(!found&&arpNoteCount<32) arpNotes[arpNoteCount++]=note;
                } else {
                    for(uint8_t i=0;i<arpNoteCount;i++) if(arpNotes[i]==note){
                        memmove(&arpNotes[i],&arpNotes[i+1],(arpNoteCount-i-1));
                        arpNoteCount--;
                        break;
                    }
                    if(arpNoteCount==0&&arpCurrent!=0){audioNoteOff(arpCurrent);arpCurrent=0;}
                }
            }
            break;
        }
        case MODE_OMNI:{
            if(pressed){
                // Release previous chord
                if(omniRoot!=0xFF) for(int i=0;i<3;i++) audioNoteOff(omniChordNotes[i]);
                uint8_t ni, ct; bool valid=true;
                if(row>=1&&row<=3){
                    ni=(uint8_t)(7-col);   // col0(right)=E(7)..col7(left)=Eb(0)
                    ct=(uint8_t)(3-row);   // row3(top)=Maj(0), row2=min(1), row1=7th(2)
                } else {                   // row==0: B chords at cols 0-2
                    ni=8;
                    if(col<3) ct=(uint8_t)col;
                    else valid=false;
                }
                if(valid){
                    omniRoot=ni; omniType=ct;
                    uint8_t base=(uint8_t)(48+omniNoteOff[ni]+noteMap.getOctave()*12);
                    for(int i=0;i<3;i++){
                        omniChordNotes[i]=base+omniChordInt[ct][i];
                        audioNoteOn(omniChordNotes[i],0.5f);
                    }
                }
            }
            break;
        }
        case MODE_DRUMS:{
            if(pressed) audioPlayDrumPad((uint8_t)(row*8+col), volume);
            break;
        }
        case MODE_SAMPLE:{
            uint8_t kidx=(uint8_t)(row*8+col);
            if(pressed){
                if(sampleMap[row][col].length()>0){
                    if(audioKeyLoaded(kidx)){
                        if(samplePlayMode==3){  // Solo: stop all before playing
                            for(uint8_t k=0;k<SAMPLE_KEY_COUNT;k++) audioStopKey(k);
                        }
                        audioPlayKey(kidx, volume);
                        if(samplePlayMode==2){  // Loop: schedule first retrigger
                            uint32_t len=audioKeyLengthMs(kidx);
                            sampleLoopNext[kidx]=millis()+(len>50?len:500);
                        }
                    } else {
                        // Assigned but not loaded: re-queue the load; key will play on next press once ready.
                        audioLoadKey(sampleMap[row][col].c_str(), kidx);
                    }
                } else if(sdReady&&sdCursor<sdFileCount&&!sdFileIsDir[sdCursor]){
                    String fp=buildSdFilePath();
                    sampleMap[row][col]=fp;
                    audioLoadKey(fp.c_str(), kidx);
                    Serial.printf("ASSIGN R%dC%d -> %s\n",row,col,fp.c_str());
                }
            } else {  // released
                if(samplePlayMode==1||samplePlayMode==2) audioStopKey(kidx);
            }
            break;
        }
        case MODE_SEQ:{
            // Each row = one track on current page, each column = one step
            if(pressed){
                uint8_t step=(uint8_t)(KBD_COLS-1-col); // col7=step0..col0=step7
                uint8_t track=seqPage*4+row;
                seqTrackSel = row;  // highlight pressed track on OLED
                bool wasSet=seqPattern[track][step];
                seqPattern[track][step]=!wasSet;
                // Preview sample when activating a step
                if(!wasSet && audioKeyLoaded(SEQ_KEY_BASE+track))
                    audioPlayKey(SEQ_KEY_BASE+track, volume);
            }
            break;
        }
        case MODE_MODULAR:{
            uint8_t note=noteMap.getMidiNote(row,col);
            activeNotes[row][col]=pressed?note:0;
            if(pressed){
                // Joystick X → velocity: center=0.8, full tilt = ±0.6
                float jx=constrain(cachedJoyX/64.0f,-1.0f,1.0f);
                float vel=constrain(0.8f+jx*0.6f, 0.05f, 1.5f);
                audioNoteOn(note, vel);
            } else {
                audioNoteOff(note);
            }
            break;
        }
        case MODE_HYBRID:{
            uint8_t note=noteMap.getMidiNote(row,col);
            uint8_t kidx=(uint8_t)(row*8+col);
            activeNotes[row][col]=pressed?note:0;
            if(pressed){
                float jx=constrain(cachedJoyX/64.0f,-1.0f,1.0f);
                audioNoteOn(note, constrain(0.8f+jx*0.6f, 0.05f, 1.5f));
                if(sampleMap[row][col].length()>0 && audioKeyLoaded(kidx))
                    audioPlayKey(kidx, volume);
            } else {
                audioNoteOff(note);
                audioStopKey(kidx);
            }
            break;
        }
        case MODE_LIGHTPLAY:{
            if(pressed){
                // Find the first free (dim) ripple slot
                uint8_t slot=0;
                for(uint8_t i=0;i<8;i++) if(ripples[i].bright<10){slot=i;break;}
                int gr=KBD_ROWS-1-row, gc=KBD_COLS-1-col;
                int li=crdToIdx(gr*KBD_COLS+gc,0);
                if(li>=0&&li<NUM_LEDS){
                    ripples[slot]={(int8_t)li,0.0f,(uint8_t)((row*8+col)*37+80),220};
                }
                // Also play the note for sound feedback
                audioNoteOn(noteMap.getMidiNote(row,col),0.6f);
            } else {
                audioNoteOff(noteMap.getMidiNote(row,col));
            }
            break;
        }
        case MODE_SYNTH2:{
            uint8_t note=noteMap.getMidiNote(row,col);
            activeNotes[row][col]=pressed?note:0;
            if(pressed){
                float jx=constrain(cachedJoyX/64.0f,-1.0f,1.0f);
                audioNoteOn(note, constrain(0.8f+jx*0.6f, 0.05f, 1.5f));
            } else audioNoteOff(note);
            break;
        }
        case MODE_303: {
            uint8_t base;
            if (kbdLayout == KBD_LAYOUT_PIANO) {
                base = pianoNote(row, col, (uint8_t)(48 + noteMap.getOctave()*12));
                if (base == 0xFF) return;
            } else {
                base = noteMap.getMidiNote(row, col);
            }
            uint8_t note = (uint8_t)constrain((int)base + (int)t303Oct * 12, 0, 127);
            if (arpMode != 0) {
                // Arp mode: manage arpNotes[] like SYNTH does; arp loop calls audioT303NoteOn
                if (pressed) {
                    bool found=false;
                    for(uint8_t i=0;i<arpNoteCount;i++) if(arpNotes[i]==note){found=true;break;}
                    if(!found&&arpNoteCount<32) arpNotes[arpNoteCount++]=note;
                } else {
                    for(uint8_t i=0;i<arpNoteCount;i++) if(arpNotes[i]==note){
                        memmove(&arpNotes[i],&arpNotes[i+1],(arpNoteCount-i-1));
                        arpNoteCount--;
                        break;
                    }
                    if(arpNoteCount==0&&arpCurrent!=0){audioT303NoteOff(arpCurrent);arpCurrent=0;}
                }
                break;
            }
            // Direct play (no arp)
            if (pressed) {
                if (t303SlideOn && t303CurrentNote != 0) {
                    float initSemis = (float)(int8_t)((int)t303CurrentNote - (int)note);
                    audioT303PitchBend(powf(2.0f, initSemis / 12.0f));
                    t303SlideFrom = t303CurrentNote;
                    t303SlideTo   = note;
                    t303SlideMs   = millis();
                    t303SlideActive = true;
                } else {
                    t303SlideActive = false;
                    audioT303PitchBend(1.0f);
                }
                { float jx303=constrain(cachedJoyX/64.0f,-1.0f,1.0f);
                  float vel303=constrain((t303AccentOn?1.2f:0.7f)+jx303*0.4f,0.1f,1.5f);
                  audioT303NoteOn(note, vel303); }
                t303CurrentNote  = note;
                t303PressedRow   = (int8_t)row;
                t303PressedCol   = (int8_t)col;
            } else {
                if ((int8_t)row == t303PressedRow && (int8_t)col == t303PressedCol) {
                    audioT303NoteOff(note);
                    t303CurrentNote = 0;
                    t303PressedRow  = -1;
                    t303PressedCol  = -1;
                }
            }
            break;
        }
        case MODE_DRUM2: {
            if (!pressed) break;
            if (drum2SeqView) {
                if (row >= 2) {
                    // Rows 2+3: 16-step grid
                    // placeMod = 0 (NRM) if pad was selected from normal row, else current alt mode
                    uint8_t step = (uint8_t)((row == 3 ? 0 : 8) + (KBD_COLS - 1 - col));
                    if (step >= DR2_STEPS) break;
                    uint8_t placeMod = drum2SelFromAlt ? drum2PadAltMode[drum2SelPad] : 0;
                    if (drum2SeqVel[drum2SelPad][step] > 0) {
                        if (drum2SeqMod[drum2SelPad][step] == placeMod) {
                            // Same mod → toggle OFF (remove)
                            drum2SeqVel[drum2SelPad][step]      = 0;
                            drum2SeqRow[drum2SelPad][step]      = 0;
                            drum2SeqMod[drum2SelPad][step]      = 0;
                            drum2SeqPitchOff[drum2SelPad][step] = 0;
                        } else {
                            // Different mod → replace, keep velocity
                            drum2SeqMod[drum2SelPad][step] = placeMod;
                        }
                    } else {
                        // Empty → place with placeMod
                        drum2SeqVel[drum2SelPad][step]      = 100;
                        drum2SeqRow[drum2SelPad][step]      = 2;
                        drum2SeqMod[drum2SelPad][step]      = placeMod;
                        drum2SeqPitchOff[drum2SelPad][step] = 0;
                    }
                } else if (row == 1) {
                    // Row 1: alt element row — selects pad as "alt", cycles alt mode 1→2→3→1
                    uint8_t padIdx = (uint8_t)(KBD_COLS - 1 - col);
                    if (padIdx >= DRUM2_PADS) break;
                    drum2SelPad     = (int8_t)padIdx;
                    drum2SelFromAlt = true;
                    uint8_t cur = drum2PadAltMode[padIdx];
                    drum2PadAltMode[padIdx] = (cur == 0 || cur >= 3) ? 1 : cur + 1;
                } else {
                    // Row 0: normal element row — selects pad as "normal" (NRM placement)
                    uint8_t padIdx = (uint8_t)(KBD_COLS - 1 - col);
                    if (padIdx >= DRUM2_PADS) break;
                    drum2SelPad     = (int8_t)padIdx;
                    drum2SelFromAlt = false;
                    if (padIdx == DRUM2_CH || padIdx == DRUM2_OH) {
                        uint8_t other = (padIdx == DRUM2_CH) ? DRUM2_OH : DRUM2_CH;
                        amy_event stop = amy_default_event();
                        stop.osc = DRUM_OSC_BASE + kDrum2Remap[other];
                        stop.velocity = 0.0f;
                        amy_add_event(&stop);
                    }
                    audioDrum2Hit(kDrum2Remap[padIdx], 0.70f * drum2Volume[padIdx],
                                  drum2Pitch[padIdx], drum2Decay[padIdx]);
                    drum2PadFlashMs[padIdx] = millis();
                }
            } else {
                // Pad mode: col=7(left)=pad0(KK1) … col=0(right)=pad7(RDE)
                // Rows vary pitch + decay (ghost/soft/normal/accent) for expressive playing
                uint8_t padIdx = (uint8_t)(KBD_COLS - 1 - col);
                if (padIdx >= DRUM2_PADS) break;
                drum2SelPad = (int8_t)padIdx;
                uint8_t rv = row < KBD_NOTE_ROWS ? row : 2;
                const Dr2RowVar& var = kDr2RowVar[rv];
                float vel = var.vel * drum2Volume[padIdx];
                uint8_t pitch = (uint8_t)constrain((int)drum2Pitch[padIdx] + var.pitchOff, 0, 127);
                float dec = drum2Decay[padIdx] > 0 ? drum2Decay[padIdx] * var.decayFact : 0;
                // Choke
                if (padIdx == DRUM2_CH || padIdx == DRUM2_OH) {
                    uint8_t other = (padIdx == DRUM2_CH) ? DRUM2_OH : DRUM2_CH;
                    amy_event stop = amy_default_event();
                    stop.osc = DRUM_OSC_BASE + kDrum2Remap[other];
                    stop.velocity = 0.0f;
                    amy_add_event(&stop);
                }
                audioDrum2Hit(kDrum2Remap[padIdx], vel, pitch, dec);
                drum2PadFlashMs[padIdx] = millis();
                // Quantized recording — stores row so playback matches the live hit
                if (drum2RecArmed && drum2Playing) {
                    unsigned long stepMs = 60000UL / (unsigned long)bpm / 4;
                    unsigned long elapsed = millis() - drum2LastStepMs;
                    uint8_t nearStep = (elapsed > stepMs / 2)
                                     ? (drum2Step + 1) % DR2_STEPS
                                     : drum2Step;
                    drum2SeqVel[padIdx][nearStep]      = (uint8_t)constrain((int)(vel * 127.f), 1, 127);
                    drum2SeqRow[padIdx][nearStep]      = rv;
                    drum2SeqMod[padIdx][nearStep]      = 0;
                    drum2SeqPitchOff[padIdx][nearStep] = 0;  // auto-record: no pitch offset
                }
            }
            break;
        }
        case MODE_303S: {
            if (s303SeqView) {
                if (row >= 2) {
                    // Top rows (steps): place selected note; same note on occupied step = silence
                    if (!pressed) break;
                    uint8_t step = (uint8_t)((row == 3 ? 0 : 8) + (KBD_COLS - 1 - col));
                    if (step >= S303_STEPS) break;
                    s303SelStep = step;
                    if (s303SelNote > 0) {
                        if (s303Note[step] == s303SelNote) {
                            s303Note[step] = 0;  // same note → silence
                        } else {
                            s303Note[step] = s303SelNote;
                            s303Alt[step]  = s303PendingAlt;
                        }
                    }
                } else {
                    // Bottom rows (notes): select note + preview only — never touches steps
                    uint8_t note = noteMap.getMidiNoteByIdx((KBD_COLS - 1 - col) * 2 + row);
                    uint8_t notePlay = (uint8_t)constrain((int)note + (int)t303Oct*12, 0, 127);
                    if (pressed) {
                        activeNotes[row][col] = note + 1;
                        s303SelNote = note + 1;
                        audioT303NoteOn(notePlay, 0.7f);
                    } else {
                        activeNotes[row][col] = 0;
                        audioT303NoteOff(notePlay);
                    }
                }
            } else {
                // PAD view: play 303 live + auto-record at current step when playing
                uint8_t note = noteMap.getMidiNote(row, col);
                uint8_t notePlay = (uint8_t)constrain((int)note + (int)t303Oct*12, 0, 127);
                if (pressed) {
                    activeNotes[row][col] = note;
                    // Slide if was playing and slide is on
                    if (t303CurrentNote && t303SlideOn) {
                        t303SlideFrom=t303CurrentNote; t303SlideTo=notePlay;
                        t303SlideMs=millis(); t303SlideActive=true;
                    } else {
                        t303SlideActive=false; audioT303PitchBend(1.0f);
                    }
                    float vel = t303AccentOn ? 1.2f : 0.7f;
                    audioT303NoteOn(notePlay, vel);
                    t303CurrentNote = notePlay; t303PressedRow=(int8_t)row; t303PressedCol=(int8_t)col;
                    // Auto-record to current step when playing
                    if (drum2Playing) {
                        uint8_t nearStep = drum2Step;
                        unsigned long stepMs = 60000UL / (unsigned long)bpm / 4;
                        if (millis() - drum2LastStepMs > stepMs / 2)
                            nearStep = (drum2Step + 1) % S303_STEPS;
                        s303Note[nearStep] = note + 1;
                        if (t303AccentOn && s303Alt[nearStep]==0) s303Alt[nearStep]=1;
                    }
                } else {
                    if ((int8_t)row == t303PressedRow && (int8_t)col == t303PressedCol) {
                        audioT303NoteOff(notePlay);
                        t303CurrentNote=0; t303PressedRow=-1; t303PressedCol=-1;
                    }
                    activeNotes[row][col] = 0;
                }
            }
            break;
        }
        case MODE_SS2: {
            // Mode unique (pas de toggle vue) — note-first
            // Top 2 rows (rows 2-3): 16 steps
            // Bottom 2 rows (rows 0-1): 16 slots (select + assign depuis filebrowser)
            if (row >= 2) {
                // Step press: place selected slot on step; same → clear
                if (!pressed) break;
                uint8_t step = (uint8_t)((row==3?0:8)+(KBD_COLS-1-col));
                if (step >= SS2_SLOTS) break;
                ss2SelStep = step;
                if (ss2SelSlot < SS2_SLOTS) {
                    ss2Note[step] ^= (uint16_t)(1u << ss2SelSlot);  // toggle selected slot on/off
                }
            } else {
                // Slot press: sélectionner slot + assigner fichier SD si disponible; sinon preview
                uint8_t slot=(uint8_t)((KBD_COLS-1-col)*2+row);
                if (slot>=SS2_SLOTS) break;
                if (pressed) {
                    activeNotes[row][col]=(uint8_t)(slot+1);
                    ss2SelSlot=slot;
                    if (sdReady && sdCursor<sdFileCount && !sdFileIsDir[sdCursor] && isAudioFile(sdFiles[sdCursor].c_str())) {
                        char fp[256];
                        snprintf(fp,sizeof(fp),"%s%s",sdPath.c_str(),sdFiles[sdCursor].c_str());
                        strncpy(ss2Path[slot],fp,sizeof(ss2Path[0])-1);
                        ss2Path[slot][sizeof(ss2Path[0])-1]='\0';
                        audioLoadKey(fp,(uint8_t)(SS2_KEY_BASE+slot));
                        ss2Loaded[slot]=false;
                        if (slot < SS2_SLOTS-1) ss2SelSlot=slot+1;
                    } else if (ss2Loaded[slot]) {
                        audioPlayKey((uint8_t)(SS2_KEY_BASE+slot),0.8f);
                    }
                } else {
                    activeNotes[row][col]=0;
                }
            }
            break;
        }
        case MODE_SYSEQ: {
            if (syseqSeqView) {
                if (row >= 2) {
                    // Top rows (steps): place all currently-held notes; same note = toggle off
                    if (!pressed) break;
                    uint8_t step = (uint8_t)((row == 3 ? 0 : 8) + (KBD_COLS - 1 - col));
                    if (step >= SYSEQ_STEPS) break;
                    syseqSelStep = step;
                    // Place each note currently held in bottom rows
                    for (uint8_t r2=0; r2<KBD_NOTE_ROWS; r2++)
                        for (uint8_t c2=0; c2<KBD_COLS; c2++) {
                            if (!activeNotes[r2][c2]) continue;
                            uint8_t stored = (uint8_t)(activeNotes[r2][c2] + 1);
                            bool found = false;
                            for (uint8_t i=0; i<SYSEQ_CHORD; i++)
                                if (syseqNotes[step][i] == stored) {
                                    syseqNotes[step][i] = 0; syseqVels[step][i] = 0;
                                    found = true; break;
                                }
                            if (!found)
                                for (uint8_t i=0; i<SYSEQ_CHORD; i++)
                                    if (syseqNotes[step][i] == 0) {
                                        syseqNotes[step][i] = stored;
                                        syseqVels[step][i]  = 100;
                                        break;
                                    }
                        }
                } else {
                    // Bottom rows (notes): select + preview only — no step modification
                    uint8_t note = noteMap.getMidiNoteByIdx((KBD_COLS - 1 - col) * 2 + row);
                    if (pressed) {
                        activeNotes[row][col] = note;
                        audioNoteOn(note, 0.75f);
                    } else {
                        activeNotes[row][col] = 0;
                        audioNoteOff(note);
                    }
                }
            } else {
                // PAD view: standard noteMap keyboard + auto-record when playing
                uint8_t note = noteMap.getMidiNote(row, col);
                activeNotes[row][col] = pressed ? note : 0;
                if (pressed) {
                    float jx = constrain(cachedJoyX/64.0f, -1.0f, 1.0f);
                    int held = 0;
                    for(int rr=0;rr<KBD_NOTE_ROWS;rr++) for(int cc=0;cc<KBD_COLS;cc++) if(activeNotes[rr][cc]) held++;
                    float polyScale = 1.0f / sqrtf(fmaxf(1.0f, (float)held));
                    float vel = constrain((0.8f + jx*0.6f) * polyScale, 0.05f, 1.0f);
                    audioNoteOn(note, vel);
                    // Auto-record at current step if playing
                    if (drum2Playing) {
                        bool found = false;
                        for (uint8_t i = 0; i < SYSEQ_CHORD; i++)
                            if (syseqNotes[drum2Step][i] == note + 1) { found = true; break; }
                        if (!found) {
                            for (uint8_t i = 0; i < SYSEQ_CHORD; i++) {
                                if (syseqNotes[drum2Step][i] == 0) {
                                    syseqNotes[drum2Step][i] = note + 1;
                                    syseqVels[drum2Step][i] = (uint8_t)constrain((int)(vel*127.0f),1,127);
                                    break;
                                }
                            }
                        }
                    }
                } else {
                    audioNoteOff(note);
                }
            }
            break;
        }
        case MODE_MOD2:{
            uint8_t note=noteMap.getMidiNote(row,col);
            activeNotes[row][col]=pressed?note:0;
            if(pressed){
                float jx=constrain(cachedJoyX/64.0f,-1.0f,1.0f);
                float vel=constrain(0.8f+jx*0.6f, 0.05f, 1.5f);
                if(mod2PlayMode==MOD2_MONO){
                    if(mod2LastNote!=0) audioNoteOff(mod2LastNote);
                    audioNoteOn(note, vel);
                    mod2LastNote=note;
                } else if(mod2PlayMode==MOD2_SLIDE){
                    if(mod2LastNote!=0 && mod2LastNote!=note){
                        // Start glide: keep old note playing, bend toward new note
                        slideFromNote=mod2LastNote; slideToNote=note;
                        slideStartMs=millis(); slideActive=true;
                    } else {
                        // No previous note: play directly
                        audioNoteOn(note, vel);
                        mod2LastNote=note;
                    }
                } else {
                    // Poly
                    audioNoteOn(note, vel);
                    mod2LastNote=note;
                }
            } else {
                // Cancel slide if releasing the source note
                if(slideActive && note==slideFromNote){
                    slideActive=false;
                    audioNoteOff(slideFromNote);
                    mod2LastNote=0;
                    audioSetPitchBend(1.0f);
                } else {
                    audioNoteOff(note);
                    if(note==mod2LastNote) mod2LastNote=0;
                }
            }
            break;
        }
        case MODE_GRANULAR: {
            if (!granComputed) break;
            uint8_t oscIdx = (uint8_t)(row * 8 + col);
            if (pressed) {
                uint16_t preset = granPresetForKey(row, col);
                bool   loop    = granIsLoop(row);
                audioPlayGranularSlice(oscIdx, preset, volume, loop);
                granKeyHeld = true;
                // Slice index = col position (0=left/start of sample, 7=right)
                if ((granSubMode & 1u) == 0) {
                    granPlayingSlice = (int8_t)(7u - col);  // 8-slice: all rows share same 0-7 index
                } else {
                    // 16-slice: rows 0/2 → 0-7, rows 1/3 → 8-15
                    granPlayingSlice = (row == 1 || row == 3) ? (int8_t)(15u - col) : (int8_t)(7u - col);
                }
                // Re-sync pot baselines so the pot's current physical position becomes the new
                // delta reference — both directions are accessible regardless of where the pot sits.
                granPotNeedsSync = true;
            } else {
                audioStopGranularOsc(oscIdx);
                granKeyHeld = false;
                // granPlayingSlice intentionally kept: pots still control last played slice
            }
            break;
        }
        case MODE_GRANULAR2: {
            uint8_t sampleIdx, sliceIdx; bool reverse;
            gran2KeyInfo(row, col, sampleIdx, sliceIdx, reverse);
            if (sampleIdx >= gran2NumSamples()) break;
            if (!gran2[sampleIdx].computed) break;
            uint8_t keyOsc = (uint8_t)(row * 8 + col);
            if (pressed) {
                if (gran2PlayMode == 2) {
                    // FUL: build [tail_from_slice, full_sample] buffer → "N 0 1 2 N 0 1 2…"
                    float sf = reverse
                        ? (1.0f - gran2[sampleIdx].splits[sliceIdx + 1])
                        :          gran2[sampleIdx].splits[sliceIdx];
                    audioPlayGranular2Ful(keyOsc, sampleIdx, reverse, volume, sf);
                } else {
                    audioPlayGranular2(keyOsc, sampleIdx, sliceIdx, reverse, volume, gran2PlayMode);
                }
                gran2ActiveSample = (int8_t)sampleIdx;
                gran2ActiveSlice  = (int8_t)sliceIdx;
                gran2PotNeedsSync = true;
            } else {
                audioStopGranular2(keyOsc);
                // keep gran2ActiveSample/Slice for pot control after release
            }
            break;
        }
        case MODE_TRACKER: {
            if (col < 4) {
                // Left 4×4: instrument selector; press again on selected synth track → cycle shape
                if (pressed) {
                    uint8_t instrIdx = (uint8_t)(row * 4 + col);
                    if (instrIdx == trkInstr && instrIdx < TRACKER_SYNTHS) {
                        trkSynthShape[instrIdx] = (trkSynthShape[instrIdx] + 1) % SHAPE_COUNT;
                        // Apply new shape to the track's dedicated channel immediately
                        audioSetShapeOnSynth((SynthShape)trkSynthShape[instrIdx],
                                             (uint8_t)(TRACKER_SYNTH_CH_BASE + instrIdx));
                    } else {
                        trkInstr = instrIdx;
                    }
                }
            } else {
                // Right 4×4: note grid or drum pad
                uint8_t midiNote = (uint8_t)constrain((int)kTrkNoteGrid[row][col - 4] + trkOctave * 12, 0, 127);
                uint8_t drumPad  = (uint8_t)(row * 4 + (col - 4));  // pad 0-15 for drum track
                if (pressed) {
                    int8_t recVal = -1;
                    if (trkInstr < TRACKER_SYNTHS) {
                        // Audition on the track's own channel (not SYNTH_CH) to hear the track's shape
                        audioTrackerNoteOn(trkInstr, midiNote, volume);
                        recVal = (int8_t)midiNote;
                    } else if (trkInstr == TRACKER_DRUM_TRK) {
                        audioPlayDrumPad(drumPad, volume);
                        recVal = (int8_t)drumPad;
                    } else {
                        uint8_t kidx = (uint8_t)(trkInstr - TRACKER_SAMP_BASE);
                        if (audioKeyLoaded(kidx)) audioPlayKey(kidx, volume);
                        recVal = 1;
                    }
                    if (trkRec) {
                        uint8_t step = trkStep;
                        if (trkPlaying) {
                            uint32_t stepMs = 60000u / bpm / 4u;
                            uint32_t dt = millis() - trkLastStepMs;
                            step = (dt * 2 > stepMs) ? (uint8_t)((trkStep + 1) % TRACKER_STEPS) : trkStep;
                        }
                        // Chord recording: count other note-grid keys currently held.
                        // First key of a gesture clears the step; additional simultaneous keys add to chord.
                        int heldOthers = 0;
                        for (int nr = 0; nr < KBD_ROWS; nr++)
                            for (int nc = 4; nc < KBD_COLS; nc++)
                                if ((nr != row || nc != col) && keyState[nr][nc]) heldOthers++;

                        if (heldOthers == 0) {
                            // Start fresh chord for this step
                            for (int ci = 0; ci < TRK_CHORD_SIZE; ci++) trkNotes[trkInstr][step][ci] = -1;
                            trkNotes[trkInstr][step][0] = recVal;
                        } else {
                            // Add to existing chord
                            for (int ci = 0; ci < TRK_CHORD_SIZE; ci++) {
                                if (trkNotes[trkInstr][step][ci] < 0) {
                                    trkNotes[trkInstr][step][ci] = recVal;
                                    break;
                                }
                            }
                        }
                        trkVel[trkInstr][step] = (uint8_t)(volume * 100);
                    }
                } else {
                    if (trkInstr < TRACKER_SYNTHS) audioTrackerNoteOff(trkInstr, midiNote);
                    else if (trkInstr >= TRACKER_SAMP_BASE) audioStopKey((uint8_t)(trkInstr - TRACKER_SAMP_BASE));
                }
            }
            break;
        }
        case MODE_MIDI: {
#if CONFIG_TINYUSB_MIDI_ENABLED
            if (midiActive) {
                uint8_t midiNote;
                if (kbdLayout == KBD_LAYOUT_PIANO) {
                    midiNote = pianoNote(row, col, (uint8_t)(midiOctave * 12));
                    if (midiNote == 0xFF) return;
                } else {
                    // Grid layout matching SYNTH direction: bottom-left=low, top-right=high
                    midiNote = (uint8_t)constrain(
                        (int)(midiOctave*12) + (KBD_COLS-1-(int)col)*KBD_NOTE_ROWS + (int)row, 0, 127);
                }
                uint8_t vel = pressed ? (uint8_t)constrain((int)(volume * 127), 1, 127) : 0;
                if (pressed) usbMIDI.noteOn(midiNote, vel, midiChannel);
                else         usbMIDI.noteOff(midiNote, 0, midiChannel);
            }
#endif
            break;
        }
        default: break;
    }
}

static void audioHandlerTask(void*)
{
    for (;;) {
        KeyEvent evt;
        if (xQueueReceive(s_audioEventQueue, &evt, portMAX_DELAY))
            handleNoteKeyAudio(evt.row, evt.col, evt.pressed);
    }
}

// ==================== SETUP ====================
void setup() {
    pinMode(PWR_ON_EN,OUTPUT); digitalWrite(PWR_ON_EN,HIGH);
    pinMode(I2S_XSMT,OUTPUT);  digitalWrite(I2S_XSMT,HIGH);
    Serial.begin(115200);
    Serial.println("=== GrvEP v2 ===");

    pinMode(PWR_SENSE,INPUT);
    pinMode(JOYSW,INPUT_PULLUP);
    pinMode(MUX_COM,INPUT);
    setup_leds();
    setupKeyboard();
    setupJoystick();
    setupOled();
    joystickCalibration();
    SPI.begin(SD_SCLK,SD_MISO,SD_MOSI);
    if(SD.begin(SD_CS,SPI,20000000)){sdReady=true; Serial.println("SD OK");}

    audioInit();
    s_audioEventQueue = xQueueCreate(16, sizeof(KeyEvent));
    setNoteKeyCallback(kbdEventBridge);
    s_ledSem = xSemaphoreCreateBinary();
    xTaskCreatePinnedToCore(audioHandlerTask, "audioHdlr", 8192, nullptr, 22, nullptr, 1);
    xTaskCreatePinnedToCore(ledUpdateTask,    "led",       3072, nullptr,  4, nullptr, 0);
    xTaskCreatePinnedToCore(statsTask,        "stats",     4096, nullptr,  1, nullptr, 0);

#if CONFIG_TINYUSB_MIDI_ENABLED
    USB.productName("GrvEP");
    USB.manufacturerName("GrvEP");
    USB.begin();  // tinyusb_init() — MIDI interface already registered by global USBMIDI ctor
#endif
    noteMap.setScale(SCALE_CHROMATIC);
    noteMap.setOctave(-1);
    for(int i=0;i<7;i++) pots[i].value=0.5f;
    pots[0].value=0.7f;  // volume
    pots[1].value=0.5f;  // shape (mid range)
    pots[2].value=0.0f;  // envelope (Normal)
    pots[3].value=0.0f; pots[4].value=0.0f; pots[5].value=0.0f; pots[6].value=0.0f;
    audioSetShape(currentShape);  // apply J:PNO default immediately
    audioSetEnvelope(envTable[currentEnv]);
    audioSetVolume(pots[0].value);
    memset(trkNotes, -1, sizeof(trkNotes));  // init tracker to empty (static 0-init would trigger step-0 notes)
    Serial.println("Ready");
}

// ==================== LOOP ====================
void loop() {
    s_mainLoopCount++;
    amy_update();

    // Close overlay after 200ms feedback window
    if (s_overlayCloseAt && millis() >= s_overlayCloseAt) {
        s_overlay = OVERLAY_NONE;
        s_overlayCloseAt = 0;
    }

    // ---- KEY EVENTS ----
    uint8_t row,col; bool pressed;
    while(getNextKeyEvent(row,col,pressed)){
        if(noteMap.isMenuKey(row,col)){handleButton(noteMap.getMenuButton(row,col),pressed);continue;}
        if(menuOpen||row>=KBD_NOTE_ROWS||col>=KBD_COLS||!audioReady) continue;

        // Overlay intercept: bottom-row keys select options; other rows close overlay
        if(s_overlay != OVERLAY_NONE){
            if(pressed) overlayKeyPress(row,col);
            continue;
        }
        // Note audio handled immediately by handleNoteKeyAudio() via kbdPollTask callback.
    }
    if(btn1PressTime>0&&!btn1Handled&&(millis()-btn1PressTime>=600)){
        btn1Handled=true;
#if CONFIG_TINYUSB_MIDI_ENABLED
        if (currentMode == MODE_MIDI) {
            midiPotSel = (midiPotSel < 0) ? 0 : -1;  // toggle pot config mode
        } else {
#endif
            menuOpen=!menuOpen;
            if(menuOpen){audioAllNotesOff();omniRoot=0xFF;menuRow=0;menuCol=0;}
#if CONFIG_TINYUSB_MIDI_ENABLED
        }
#endif
    }

    // ---- JOYSTICK CLICK ----
    bool click=!digitalRead(JOYSW);
    // Long click (>600ms) in file-browser modes: navigate to parent directory (or menu at root)
    // Only arm the timer when the press starts inside a browser mode (avoids false fire after menu navigation)
    {
        bool isBrw = !menuOpen && sdReady &&
            (currentMode==MODE_SS2 ||
             currentMode==MODE_SAMPLE || currentMode==MODE_SEQ ||
             currentMode==MODE_GRANULAR || currentMode==MODE_GRANULAR2);
        if (click && !lastClick) { joyClickMs = isBrw ? millis() : 0; joyLongFired = false; }
    }
    if (click && !joyLongFired && joyClickMs && (millis() - joyClickMs >= 600)) {
        joyLongFired = true;
        bool isBrowserMode = !menuOpen && sdReady &&
            (currentMode==MODE_SS2 ||
             currentMode==MODE_SAMPLE || currentMode==MODE_SEQ ||
             currentMode==MODE_GRANULAR || currentMode==MODE_GRANULAR2);
        if (isBrowserMode) {
            if (sdPath == "/") { audioAllNotesOff(); menuOpen=true; menuRow=0; menuCol=0; }
            else { int ls=sdPath.lastIndexOf('/',sdPath.length()-2); sdListDir(ls<=0?"/":sdPath.substring(0,ls+1)); sdScroll=0; sdCursor=0; }
        }
    }
    if (click && !lastClick && !joyLongFired) {
        if(menuOpen){
            selectMenuItem();
        } else if(currentMode==MODE_GRANULAR&&sdReady){
            // Granular: click navigates folders or loads selected file as granular source
            if(sdCursor<sdFileCount){
                if(sdFiles[sdCursor]==".."){
                    if(sdPath=="/"){audioAllNotesOff();menuOpen=true;menuRow=0;menuCol=0;}
                    else{int ls=sdPath.lastIndexOf('/',sdPath.length()-2);sdListDir(ls<=0?"/":sdPath.substring(0,ls+1));}
                } else if(sdFileIsDir[sdCursor]){
                    String np=sdPath; if(!np.endsWith("/"))np+="/"; np+=sdFiles[sdCursor]; sdListDir(np);
                } else if(isAudioFile(sdFiles[sdCursor].c_str())){
                    granFilePath = buildSdFilePath();
                    audioLoadGranularSource(granFilePath.c_str());
                    granComputed = false;
                    granWinStart = 0.0f; granWinEnd = 1.0f;
                    granPlayingSlice = -1; granKeyHeld = false;
                    for (int i = 0; i <= GRANULAR_MAX_SLICES; i++) granSplits[i] = 0.0f;
                    granPotNeedsSync = true;
                }
            }
        } else if(currentMode==MODE_GRANULAR2&&sdReady){
            // Granular2: click navigates or loads file into gran2LoadTarget slot
            if(sdCursor<sdFileCount){
                if(sdFiles[sdCursor]==".."){
                    if(sdPath=="/"){audioAllNotesOff();menuOpen=true;menuRow=0;menuCol=0;}
                    else{int ls=sdPath.lastIndexOf('/',sdPath.length()-2);sdListDir(ls<=0?"/":sdPath.substring(0,ls+1));}
                } else if(sdFileIsDir[sdCursor]){
                    String np=sdPath; if(!np.endsWith("/"))np+="/"; np+=sdFiles[sdCursor]; sdListDir(np);
                } else if(isAudioFile(sdFiles[sdCursor].c_str())){
                    uint8_t t = gran2LoadTarget;
                    // Unload the old slot first: pcm_load (inside the service task) will call
                    // pcm_unload_preset on the source, freeing the buffer that FWD extern16
                    // presets still reference. Stopping + unloading here prevents dangling pointers
                    // during the load window, which caused FWD to play garbage while REV (using
                    // its own PSRAM copy) still worked.
                    for (uint8_t r = 0; r < KBD_NOTE_ROWS; r++) {
                        for (uint8_t c = 0; c < KBD_COLS; c++) {
                            uint8_t si, sl; bool rv;
                            gran2KeyInfo(r, c, si, sl, rv);
                            if (si == t) audioStopGranular2((uint8_t)(r * KBD_COLS + c));
                        }
                    }
                    audioUnloadGranular2Slot(t);
                    gran2[t].path     = buildSdFilePath();
                    gran2[t].loaded   = false;
                    gran2[t].computed = false;
                    gran2[t].sliceCount = 0;
                    for (int i = 0; i <= GRAN2_MAX_SLICES; i++) gran2[t].splits[i] = 0.0f;
                    audioLoadGranular2Source(gran2[t].path.c_str(), t);
                    // Advance load target to next empty slot if available
                    uint8_t numSamp = gran2NumSamples();
                    for (uint8_t ns = 1; ns < numSamp; ns++) {
                        uint8_t next = (t + ns) % numSamp;
                        if (!gran2[next].loaded && gran2[next].path.isEmpty()) { gran2LoadTarget = next; break; }
                    }
                }
            }
        } else if((currentMode==MODE_SAMPLE||currentMode==MODE_SEQ)&&sdReady){
            if(sdCursor<sdFileCount){
                if(sdFiles[sdCursor]==".."){
                    if(sdPath=="/"){audioAllNotesOff();menuOpen=true;menuRow=0;menuCol=0;}
                    else{int ls=sdPath.lastIndexOf('/',sdPath.length()-2);sdListDir(ls<=0?"/":sdPath.substring(0,ls+1));}
                } else if(sdFileIsDir[sdCursor]){
                    String np=sdPath; if(!np.endsWith("/"))np+="/"; np+=sdFiles[sdCursor]; sdListDir(np);
                } else if(currentMode==MODE_SEQ){
                    String fp=buildSdFilePath();
                    uint8_t track=seqPage*4+seqTrackSel;
                    seqPaths[track]=fp;
                    audioLoadKey(fp.c_str(), SEQ_KEY_BASE+track);
                    Serial.printf("SEQ T%d -> %s\n",track+1,fp.c_str());
                } else {
                    String fp=buildSdFilePath();
                    Serial.printf("LOAD: %s\n",fp.c_str());
                    audioLoadAndPlay(fp.c_str(), PCM_PREVIEW_PRESET, volume);
                }
            }
        } else if (currentMode==MODE_SS2 && sdReady) {
            if (sdCursor < sdFileCount) {
                if (sdFiles[sdCursor]=="..") {
                    if (sdPath=="/") { audioAllNotesOff(); menuOpen=true; menuRow=0; menuCol=0; }
                    else { int ls=sdPath.lastIndexOf('/',sdPath.length()-2); sdListDir(ls<=0?"/":sdPath.substring(0,ls+1)); sdScroll=0; sdCursor=0; }
                } else if (sdFileIsDir[sdCursor]) {
                    if(!sdPath.endsWith("/")) sdPath+="/"; sdPath+=sdFiles[sdCursor]; sdPath+="/";
                    sdListDir(sdPath.c_str()); sdScroll=0; sdCursor=0;
                } else if (isAudioFile(sdFiles[sdCursor].c_str())) {
                    char fp[256];
                    snprintf(fp,sizeof(fp),"%s%s",sdPath.c_str(),sdFiles[sdCursor].c_str());
                    strncpy(ss2Path[ss2SelSlot],fp,sizeof(ss2Path[0])-1);
                    ss2Path[ss2SelSlot][sizeof(ss2Path[0])-1]='\0';
                    audioLoadKey(fp,(uint8_t)(SS2_KEY_BASE+ss2SelSlot));
                    ss2Loaded[ss2SelSlot]=false;
                    if(ss2SelSlot < SS2_SLOTS-1) ss2SelSlot++;
                }
            }
        } else {
            audioAllNotesOff(); omniRoot=0xFF; omniStrumPos=-1;
            menuOpen=true; menuRow=0; menuCol=0;
        }
        // A short-click was handled — prevent the long-click from also firing for this same press.
        // Without this, holding the joystick >600ms after a folder entry would trigger the
        // long-click and immediately navigate back to parent.
        joyLongFired = true;
    }
    if (!click) { joyClickMs = 0; joyLongFired = false; }
    lastClick=click;

    // ---- OMNICHORD STRUM ----
    // Joystick Y axis = linear strum strip (like the Omnichord touch plate).
    // Each position (0=bass..7=treble) maps to a chord tone; moving through positions
    // triggers all intermediate notes. Velocity scales with joystick speed.
    if(currentMode==MODE_OMNI&&omniRoot!=0xFF&&!menuOpen){
        float jy = cachedJoyY / 64.0f;  // -1..+1
        float jx = cachedJoyX / 64.0f;
        float mag = fabsf(jy);

        if(mag > 0.15f){
            // Map Y to strum position 0-7 (bottom=0, top=7)
            float yNorm = (jy + 1.0f) * 0.5f;  // 0..1
            int8_t newPos = (int8_t)(yNorm * 7.5f);
            if(newPos < 0) newPos = 0;
            if(newPos > 7) newPos = 7;

            // Velocity: proportional to joystick speed + slightly boosted by X tilt
            float speed = fabsf(jy - omniLastJoyY) * 6.0f;
            float vel = 0.3f + speed + fabsf(jx) * 0.2f;
            if(vel > 1.0f) vel = 1.0f;

            // Trigger all notes swept through since last position
            if(newPos != omniStrumPos){
                uint8_t base = (uint8_t)(48 + (omniRoot<9 ? omniNoteOff[omniRoot] : 0) + noteMap.getOctave()*12);
                const int8_t* strip = (omniType==0) ? omniStrumMaj :
                                      (omniType==1) ? omniStrumMin : omniStrum7;

                int8_t step = (newPos > omniStrumPos) ? 1 : -1;
                int8_t p = (omniStrumPos < 0) ? newPos : (int8_t)(omniStrumPos + step);
                while(true){
                    // Each strum position has its own OSC so notes ring out simultaneously
                    uint8_t note = (uint8_t)(base + strip[p]);
                    float freq   = 440.0f * powf(2.0f, (note - 69) / 12.0f);
                    amy_event e  = amy_default_event();
                    e.osc        = (uint16_t)(AMY_OSC_STRUM + p);
                    e.wave       = SINE;
                    e.freq_coefs[COEF_CONST] = freq;
                    e.velocity   = vel;
                    // Harp/Omnichord envelope: instant attack, plucked decay
                    e.eg0_times[0] = 2;    e.eg0_values[0] = 1.0f;
                    e.eg0_times[1] = 800;  e.eg0_values[1] = 0.0f;
                    e.eg0_times[2] = 200;  e.eg0_values[2] = 0.0f;
                    amy_add_event(&e);
                    if(p == newPos) break;
                    p += step;
                }
                omniStrumPos = newPos;
            }
        } else {
            omniStrumPos = -1;
        }
        omniLastJoyY = jy;
    }

    // ---- SYNTH JOYSTICK (pitch bend only) ----
    if(currentMode==MODE_SYNTH&&!menuOpen){
        static float ljy=99;
        float jy=cachedJoyY/64.0f;
        if(fabsf(jy-ljy)>0.03f){
            float bend=(fabsf(jy)<0.15f)?0:-jy;
            audioSetPitchBend(powf(2.0f,bend*2.0f/12.0f));
            ljy=jy;
        }
    }

    // ---- DRUM2 SEQUENCER ----
    // Doubled-hit deferred fires (from DR2_MOD_DBL steps)
    for(uint8_t pi=0;pi<DRUM2_PADS;pi++){
        if(drum2DblPendingAt[pi] && millis() >= drum2DblPendingAt[pi]){
            audioDrum2Hit(kDrum2Remap[pi], drum2Volume[pi]*0.65f, drum2DblPitch[pi], drum2Decay[pi]*0.5f);
            drum2PadFlashMs[pi] = millis();
            drum2DblPendingAt[pi] = 0;
        }
    }
    // ---- SHARED DRUM2+SYSEQ SEQUENCER TICK ----
    if(drum2Playing){
        float jx   = constrain(cachedJoyX/64.0f, -1.0f, 1.0f);
        float jy   = constrain(cachedJoyY/64.0f, -1.0f, 1.0f);
        float swing = fmaxf(jx, 0.0f) * 0.34f;
        bool prevEven = ((drum2Step & 1) == 0);
        float factor  = prevEven ? (1.0f + swing) : (1.0f - swing);
        unsigned long waitMs = (unsigned long)fmaxf(10.0f, 60000.0f / bpm / 4.0f * factor);
        if(millis() - drum2LastStepMs >= waitMs){
            drum2LastStepMs += waitMs;
            if(millis() - drum2LastStepMs > waitMs) drum2LastStepMs = millis(); // catch-up guard
            drum2Step = (drum2Step + 1) % DR2_STEPS;
            for(uint8_t pi=0;pi<DRUM2_PADS;pi++){
                uint8_t vel8 = drum2SeqVel[pi][drum2Step];
                if(vel8 == 0) continue;
                // Row variation stored at record time — apply pitch & decay offsets for playback
                uint8_t rv = drum2SeqRow[pi][drum2Step];
                const Dr2RowVar& var = kDr2RowVar[rv < KBD_NOTE_ROWS ? rv : 2];
                float vel = drum2Volume[pi] * vel8 / 127.0f * (1.0f + jy * 0.25f); // JY ±25% vel
                vel = constrain(vel, 0.05f, 1.2f);
                // Per-step pitch: base pot pitch + step offset (from cycling/row-1 edit)
                uint8_t pitch = (uint8_t)constrain(
                    (int)drum2Pitch[pi] + (int)drum2SeqPitchOff[pi][drum2Step] + (int)var.pitchOff,
                    0, 127);
                float dec = drum2Decay[pi] > 0 ? drum2Decay[pi] * var.decayFact : 0;
                uint8_t mod = drum2SeqMod[pi][drum2Step];
                // Choke CH/OH
                if(pi == DRUM2_CH || pi == DRUM2_OH){
                    uint8_t other = (pi == DRUM2_CH) ? DRUM2_OH : DRUM2_CH;
                    if(drum2SeqVel[other][drum2Step] == 0){
                        amy_event stop = amy_default_event();
                        stop.osc = DRUM_OSC_BASE + kDrum2Remap[other];
                        stop.velocity = 0.0f;
                        amy_add_event(&stop);
                    }
                }
                switch(mod){
                    case 1: // Probabilistic 50%
                        if(random(2)){ audioDrum2Hit(kDrum2Remap[pi],vel,pitch,dec); drum2PadFlashMs[pi]=millis(); }
                        break;
                    case 2: // Random pitch — audible range 36-96 (C2-C7)
                        { uint8_t rp=(uint8_t)constrain(36+(int)random(61),0,127);
                          audioDrum2Hit(kDrum2Remap[pi],vel,rp,dec); drum2PadFlashMs[pi]=millis(); }
                        break;
                    case 3: // Double hit: now + exactly half the current step duration
                        audioDrum2Hit(kDrum2Remap[pi],vel,pitch,dec);
                        drum2DblPendingAt[pi] = millis() + waitMs/2;
                        drum2DblPitch[pi] = pitch;
                        drum2PadFlashMs[pi]=millis();
                        break;
                    default: // Normal
                        audioDrum2Hit(kDrum2Remap[pi],vel,pitch,dec);
                        drum2PadFlashMs[pi]=millis();
                        break;
                }
            }
            // ---- SYSEQ notes at same shared step ----
            {
                bool newHasNotes = false;
                for(uint8_t i=0;i<SYSEQ_CHORD;i++) if(syseqNotes[drum2Step][i]) { newHasNotes=true; break; }
                // FUL: only stop previous notes if new step has notes (sustain through empty steps)
                if (!syseqActiveIsFull || newHasNotes) {
                    for(uint8_t i=0;i<syseqActiveCnt;i++) audioNoteOff(syseqActive[i]);
                    syseqActiveCnt = 0;
                }
                if (newHasNotes) {
                    for(uint8_t i=0;i<SYSEQ_CHORD;i++){
                        if(!syseqNotes[drum2Step][i]) continue;
                        uint8_t note = syseqNotes[drum2Step][i] - 1;
                        float sv = syseqVels[drum2Step][i] / 127.0f;
                        sv = constrain(sv * (1.0f + jy * 0.25f), 0.05f, 1.2f);
                        audioNoteOn(note, sv);
                        if(syseqActiveCnt < SYSEQ_CHORD) syseqActive[syseqActiveCnt++] = note;
                    }
                    syseqActiveIsFull = (syseqAlt[drum2Step] == 1);
                } else if (!syseqActiveIsFull) {
                    syseqActiveIsFull = false;
                }
            }
            // ---- 303S notes at same shared step (always, regardless of current view) ----
            {
                uint8_t s3n = s303Note[drum2Step];
                uint8_t alt = s303Alt[drum2Step];
                if (s3n > 0) {
                    uint8_t midiNote = (uint8_t)constrain((int)(s3n - 1) + (int)t303Oct*12, 0, 127);
                    float vel = (alt == 1) ? 1.2f : 0.7f;  // ACC=louder
                    // Slide: previous step must have SLD alteration and a note
                    uint8_t prevStep = (uint8_t)((drum2Step + S303_STEPS - 1) % S303_STEPS);
                    bool slideIn = (s303Alt[prevStep]==2) && (s303Note[prevStep] > 0);
                    if (slideIn && s303CurNote > 0) {
                        // Slide: start pitch-bend glide (reuse t303SlideActive machinery)
                        t303SlideFrom = s303CurNote;
                        t303SlideTo   = midiNote;
                        t303SlideMs   = millis();
                        t303SlideActive = true;
                        audioT303NoteOn(midiNote, vel);  // retrigger at new pitch; AMY blends
                    } else {
                        t303SlideActive = false;
                        audioT303PitchBend(1.0f);
                        if (s303CurNote) audioT303NoteOff(s303CurNote);
                        audioT303NoteOn(midiNote, vel);
                    }
                    s303CurNote = midiNote;
                } else {
                    // Rest: release current note
                    if (s303CurNote) { audioT303NoteOff(s303CurNote); s303CurNote=0; }
                    t303SlideActive = false;
                    audioT303PitchBend(1.0f);
                }
            }
            // ---- SS2 sample at same shared step (always, regardless of current view) ----
            {
                uint16_t s2mask = ss2Note[drum2Step];
                uint8_t alt = ss2Alt[drum2Step];
                ss2CurSlot = 0xFF;
                if (s2mask != 0 && alt != 3) {  // SIL = rest
                    if (ss2PlayMode == 1) audioStopAllSamples();  // OVR: cut all before playing
                    for (uint8_t slot=0; slot<SS2_SLOTS; slot++) {
                        if (!(s2mask & (1u << slot))) continue;
                        ss2Loaded[slot] = audioKeyLoaded((uint8_t)(SS2_KEY_BASE + slot));
                        if (!ss2Loaded[slot]) continue;
                        if (alt == 1) {  // REV
                            audioPlayKeyRev((uint8_t)(SS2_KEY_BASE + slot), 0.7f);
                            ss2LastPlayMs[slot] = millis();
                            ss2CurSlot = slot;
                        } else if (alt == 2) {  // FUL: no retrigger while sample running
                            uint32_t dur = audioKeyLengthMs((uint8_t)(SS2_KEY_BASE + slot));
                            uint32_t ela = (uint32_t)(millis() - ss2LastPlayMs[slot]);
                            if (dur == 0 || ela >= dur || ss2LastPlayMs[slot] == 0) {
                                audioPlayKey((uint8_t)(SS2_KEY_BASE + slot), 0.7f);
                                ss2LastPlayMs[slot] = millis();
                            }
                            ss2CurSlot = slot;
                        } else {  // NRM
                            audioPlayKey((uint8_t)(SS2_KEY_BASE + slot), 0.7f);
                            ss2LastPlayMs[slot] = millis();
                            ss2CurSlot = slot;
                        }
                    }
                }
            }
        }
    }

    // ---- DRUM SEQUENCER ----
    if(currentMode==MODE_DRUMS&&drumPlaying){
        static unsigned long lastStep=0;
        unsigned long stepMs=60000/bpm/4;
        if(millis()-lastStep>=stepMs){
            lastStep=millis(); drumStep=(drumStep+1)%DRUM_MAX_STEPS;
            // rows 0-3 → KK1(0), SN1(2), HH1(5), KK2(1)
            static const uint8_t seqPad[] = {0, 2, 5, 1};
            for(int r=0;r<DRUM_ROWS;r++) if(drumPattern[r][drumStep]) audioPlayDrumPad(seqPad[r], volume);
        }
    }

    // ---- SAMPLE SEQUENCER ----
    if(seqPlaying){
        static unsigned long lastSeqStep=0;
        unsigned long seqStepMs=60000/bpm/2; // 8th notes
        if(millis()-lastSeqStep>=seqStepMs){
            lastSeqStep=millis();
            seqStep=(seqStep+1)%8;
            // CUT_BAR: stop after completing all 8 steps (when seqStep wraps to 0)
            if(seqPlayMode==SEQ_CUT_BAR && seqStep==0){
                seqPlaying=false;
            } else {
                for(int t=0;t<8;t++){
                    if(seqPattern[t][seqStep]&&audioKeyLoaded(SEQ_KEY_BASE+t)){
                        // CUT_NOTE: stop previous instance on this track before retriggering
                        if(seqPlayMode==SEQ_CUT_NOTE) audioStopKey(SEQ_KEY_BASE+t);
                        audioPlayKey(SEQ_KEY_BASE+t, volume);
                    }
                }
            }
        }
    }

    // ---- ARPEGGIATOR (MODE_SYNTH + MODE_303, speed = BPM 16th notes) ----
    bool arpActive = (currentMode==MODE_SYNTH || currentMode==MODE_303) && arpMode!=0 && !menuOpen;
    if(arpActive){
        static unsigned long lastArp=0;
        unsigned long ARP_MS = max(10UL, 60000UL / (unsigned long)bpm / 4); // 16th note
        if(arpNoteCount>0&&millis()-lastArp>=ARP_MS){
            lastArp=millis();
            uint8_t sorted[32]; uint8_t cnt=arpNoteCount;
            memcpy(sorted,arpNotes,cnt);
            for(uint8_t i=0;i<cnt-1;i++) for(uint8_t j=i+1;j<cnt;j++)
                if(sorted[j]<sorted[i]){uint8_t t=sorted[i];sorted[i]=sorted[j];sorted[j]=t;}
            uint8_t noteToPlay=sorted[0];
            switch(arpMode){
                case 1: noteToPlay=sorted[arpIdx%cnt]; arpIdx++; break;
                case 2: noteToPlay=sorted[cnt-1-(arpIdx%cnt)]; arpIdx++; break;
                case 3: {
                    uint8_t span=(cnt<=1)?1:cnt*2-2;
                    uint8_t pos=arpIdx%span;
                    noteToPlay=(pos<cnt)?sorted[pos]:sorted[span-pos];
                    arpIdx++; break;
                }
                case 4: noteToPlay=sorted[random(cnt)]; break;
            }
            if (currentMode==MODE_303) {
                if(arpCurrent!=0) audioT303NoteOff(arpCurrent);
                float vel303=constrain((t303AccentOn?1.2f:0.7f)+(cachedJoyX/64.0f)*0.4f,0.1f,1.5f);
                audioT303NoteOn(noteToPlay, vel303);
            } else {
                if(arpCurrent!=0) audioNoteOff(arpCurrent);
                audioNoteOn(noteToPlay,0.8f);
            }
            arpCurrent=noteToPlay;
        } else if(arpNoteCount==0&&arpCurrent!=0){
            if(currentMode==MODE_303) audioT303NoteOff(arpCurrent);
            else audioNoteOff(arpCurrent);
            arpCurrent=0;
        }
    }

    // ---- FX MODE: navigate effects with joystick ----
    if(currentMode==MODE_FX&&!menuOpen){
        static unsigned long lastFxNav=0;
        float ny=cachedJoyY/64.0f;
        if(millis()-lastFxNav>=250){
            if(ny<-0.3f&&fxSelected>0){fxSelected--;lastFxNav=millis();}
            if(ny>0.3f&&fxSelected<FX_COUNT-1){fxSelected++;lastFxNav=millis();}
        }
    }

    // ---- SS2: poll audioKeyLoaded for each slot ----
    if (currentMode==MODE_SS2) {
        for(uint8_t i=0;i<SS2_SLOTS;i++)
            if(ss2Path[i][0]) ss2Loaded[i]=audioKeyLoaded((uint8_t)(SS2_KEY_BASE+i));
    }


    // ---- SAMPLE LOOP: retrigger held keys ----
    if(currentMode==MODE_SAMPLE&&samplePlayMode==2&&!menuOpen){
        uint32_t now=millis();
        for(int r=0;r<KBD_NOTE_ROWS;r++) for(int c=0;c<KBD_COLS;c++){
            if(keyState[r][c]){
                uint8_t kidx=(uint8_t)(r*8+c);
                if(audioKeyLoaded(kidx)&&sampleMap[r][c].length()>0&&now>=sampleLoopNext[kidx]){
                    audioPlayKey(kidx,volume);
                    uint32_t len=audioKeyLengthMs(kidx);
                    sampleLoopNext[kidx]=now+(len>50?len:500);
                }
            }
        }
    }

    // ---- POTS + JOYSTICK (10ms) ----
    static unsigned long lastSlow=0;
    if(millis()-lastSlow>=10){
        lastSlow=millis();
        for(int i=0;i<16;i++) muxCache[i]=mux.getValue(i);
        cachedJoyX=getJoyX(); cachedJoyY=getJoyY();
        readPots();

        // Pot 0 = Volume (always)
        static float lv=-1;
        volume=pots[0].value;
        if(fabsf(volume-lv)>0.01f){
            audioSetVolume(volume);
            audioSetSampleVolume(volume);

            lv=volume;
        }

        // Pot 2 = BPM (global, like pot 0 for volume): 40..600
        {
            static float lpBpm=-1;
            if(fabsf(pots[2].value-lpBpm)>0.003f){
                bpm=(uint16_t)(40.0f+pots[2].value*pots[2].value*560.0f);  // quadratic: more rotation for low BPM precision
                if(bpm<40) bpm=40; if(bpm>600) bpm=600;
                lpBpm=pots[2].value;
                if(fxList[5].active && audioReady) applyFxEffect(5); // re-sync delay to new BPM
                if(fxList[8].active && audioReady) applyFxEffect(8); // re-sync resecho to new BPM
            }
        }

        if(audioReady&&!menuOpen){
            switch(currentMode){
                case MODE_SYSEQ:  // SYSEQ uses same pot layout as SYNTH (shape/FX/env)
                case MODE_SYNTH: {
                    static float lp1=0.5f, lp2=-1, lp3fm=-1, lp4fm=-1;  // lp1=0.5 matches initial pots[1] so J:PNO default is not overridden on first loop
                    // Pot 1 = Shape (always)
                    if(fabsf(pots[1].value-lp1)>0.01f){
                        uint8_t si=(uint8_t)(pots[1].value*((uint8_t)SHAPE_COUNT-0.01f));
                        if(si!=currentShape){
                            currentShape=(SynthShape)si;
                            audioSetShape(currentShape);
                            // Baseline at current pot position — don't force immediate FM update
                            lp2=pots[2].value; lp3fm=pots[3].value; lp4fm=pots[4].value;
                            Serial.printf("SHAPE->%s\n",shapeNames[currentShape]);
                        }
                        lp1=pots[1].value;
                    }
                    if(currentShape==SHAPE_SAW_FM){
                        // Pots 3-5: FM depth / filter cutoff / resonance (pot2=BPM now)
                        bool fmChanged=false;
                        if(fabsf(pots[3].value-lp3fm)>0.005f) { lp3fm=pots[3].value; fmChanged=true; }
                        if(fabsf(pots[4].value-lp4fm)>0.005f) { lp4fm=pots[4].value; fmChanged=true; }
                        if(fmChanged){
                            float depth  = pots[3].value;
                            float cutoff = 500.0f * powf(16.0f, pots[4].value);  // 500..8000 Hz exp
                            audioSetFmParams(depth, cutoff, 2.0f);
                        }
                    } else {
                        // Pots 3-6 = active FX params
                        if(fxList[fxSelected].active){
                            static float lp_fx[4]={-1,-1,-1,-1};
                            const int pIdx[4]={3,4,5,6};
                            bool changed=false;
                            for(int p=0;p<4;p++){
                                if(fxList[fxSelected].paramNames[p][0]=='\0') continue;
                                if(fabsf(pots[pIdx[p]].value-lp_fx[p])>0.001f){
                                    float mn=fxList[fxSelected].paramMin[p];
                                    float mx=fxList[fxSelected].paramMax[p];
                                    if (fxSelected==0 && p==0)
                                        fxList[0].params[0]=mn*powf(mx/mn, pots[pIdx[0]].value);
                                    else
                                        fxList[fxSelected].params[p]=mn+(mx-mn)*pots[pIdx[p]].value;
                                    lp_fx[p]=pots[pIdx[p]].value;
                                    changed=true;
                                }
                            }
                            if(changed && fxSelected!=0) applyFxEffect(fxSelected);
                        }
                    }
                    break;
                }
                case MODE_OMNI: {
                    static float lp1=-1;
                    // Pot 1 = Shape
                    if(fabsf(pots[1].value-lp1)>0.01f){
                        uint8_t si=(uint8_t)(pots[1].value*((uint8_t)SHAPE_COUNT-0.01f));
                        if(si!=currentShape){currentShape=(SynthShape)si;audioSetShape(currentShape);}
                        lp1=pots[1].value;
                    }
                    // Pots 3-6 = FX params for selected effect (same as SYNTH/FX mode)
                    if(fxList[fxSelected].active){
                        static float lpo[4]={-1,-1,-1,-1};
                        static uint8_t lpoFxSel=0xFF;
                        // Re-sync tracking when FX selection changes or FX was just activated
                        if(lpoFxSel!=fxSelected||fxPotNeedSync){
                            for(int p=0;p<4;p++) lpo[p]=pots[3+p].value;
                            lpoFxSel=fxSelected; fxPotNeedSync=false;
                        }
                        const int pIdx[4]={3,4,5,6};
                        bool changed=false;
                        for(int p=0;p<4;p++){
                            if(fxList[fxSelected].paramNames[p][0]=='\0') continue;
                            if(fabsf(pots[pIdx[p]].value-lpo[p])>0.001f){
                                float mn=fxList[fxSelected].paramMin[p];
                                float mx=fxList[fxSelected].paramMax[p];
                                // LPF cutoff uses exponential mapping for musical sweep (equal octaves per pot range)
                                if (fxSelected==0 && p==0)
                                    fxList[0].params[0]=mn*powf(mx/mn, pots[pIdx[0]].value);
                                else
                                    fxList[fxSelected].params[p]=mn+(mx-mn)*pots[pIdx[p]].value;
                                lpo[p]=pots[pIdx[p]].value;
                                changed=true;
                            }
                        }
                        // LPF changes are applied smoothly by the 10ms tick (anti-zipper)
                        if(changed && fxSelected!=0) applyFxEffect(fxSelected);
                    }
                    break;
                }
                case MODE_FX: {
                    // Pots 3-6 → params 0-3 of the selected active effect
                    if(fxList[fxSelected].active){
                        static float lp[4]={-1,-1,-1,-1};
                        static uint8_t lpFxSel=0xFF;
                        // Re-sync tracking when FX selection changes or FX was just activated
                        if(lpFxSel!=fxSelected||fxPotNeedSync){
                            for(int p=0;p<4;p++) lp[p]=pots[3+p].value;
                            lpFxSel=fxSelected; fxPotNeedSync=false;
                        }
                        int pIdx[4]={3,4,5,6};
                        bool changed=false;
                        for(int p=0;p<4;p++){
                            if(fxList[fxSelected].paramNames[p][0]=='\0') continue;
                            if(fabsf(pots[pIdx[p]].value-lp[p])>0.001f){
                                float mn=fxList[fxSelected].paramMin[p];
                                float mx=fxList[fxSelected].paramMax[p];
                                // LPF cutoff uses exponential mapping for musical sweep (equal octaves per pot range)
                                if (fxSelected==0 && p==0)
                                    fxList[0].params[0]=mn*powf(mx/mn, pots[pIdx[0]].value);
                                else
                                    fxList[fxSelected].params[p]=mn+(mx-mn)*pots[pIdx[p]].value;
                                lp[p]=pots[pIdx[p]].value;
                                changed=true;
                            }
                        }
                        // LPF changes are applied smoothly by the 10ms tick (anti-zipper)
                        if(changed && fxSelected!=0) applyFxEffect(fxSelected);
                    }
                    break;
                }
                case MODE_LIGHT: {
                    // Pots 3-6: N, speed, hue, intensity — read directly in LED tick
                    // Nothing to do here; values used in LED rendering section.
                    break;
                }
                case MODE_MODULAR: {
                    // P1=OSC  P2=BPM(global)  P3=Cutoff  P4=Reso  P5=LFO rate  P6=LFO depth
                    static float lpm[5]={-1,-1,-1,-1,-1};
                    if(fabsf(pots[1].value-lpm[0])>0.005f){
                        uint8_t ns=(uint8_t)(pots[1].value*((float)SHAPE_COUNT-0.01f));
                        if(ns!=modShapeIdx){ modShapeIdx=ns; audioSetShape((SynthShape)modShapeIdx); }
                        lpm[0]=pots[1].value;
                    }
                    // P3 = Filter Cutoff — exponential 80Hz→8000Hz
                    if(fabsf(pots[3].value-lpm[1])>0.002f){
                        modCutoff=80.0f*powf(100.0f, pots[3].value);
                        audioSetFilter(modCutoff, modReso);
                        lpm[1]=pots[3].value;
                    }
                    // P4 = Filter Resonance — 0.5→6
                    if(fabsf(pots[4].value-lpm[2])>0.002f){
                        modReso=0.5f+pots[4].value*2.5f;  // 0.5→3.0
                        audioSetFilter(modCutoff, modReso);
                        lpm[2]=pots[4].value;
                    }
                    // P5 = LFO Rate — 0.1→20Hz
                    if(fabsf(pots[5].value-lpm[3])>0.002f){
                        modLfoRate=0.1f+pots[5].value*19.9f;
                        lpm[3]=pots[5].value;
                    }
                    // P6 = LFO Depth — 0→2 semitones
                    if(fabsf(pots[6].value-lpm[4])>0.002f){
                        modLfoDepth=pots[6].value*2.0f;
                        lpm[4]=pots[6].value;
                    }
                    break;
                }
                case MODE_SAMPLE:
                case MODE_SEQ:
                case MODE_HYBRID: {
                    // Pots 3-6 = active FX params (same as SYNTH mode)
                    if(fxList[fxSelected].active){
                        static float lp_fx_s[4]={-1,-1,-1,-1};
                        const int pIdx[4]={3,4,5,6};
                        bool changed=false;
                        for(int p=0;p<4;p++){
                            if(fxList[fxSelected].paramNames[p][0]=='\0') continue;
                            if(fabsf(pots[pIdx[p]].value-lp_fx_s[p])>0.001f){
                                float mn=fxList[fxSelected].paramMin[p];
                                float mx=fxList[fxSelected].paramMax[p];
                                fxList[fxSelected].params[p]=mn+(mx-mn)*pots[pIdx[p]].value;
                                lp_fx_s[p]=pots[pIdx[p]].value;
                                changed=true;
                            }
                        }
                        if(changed) applyAllFx();
                    }
                    break;
                }
                case MODE_SYNTH2: {
                    static float lp_s2=-1.0f, lp_s2cut=-1.0f;
                    // P1 = patch scroll (quantized 0-257)
                    if(fabsf(pots[1].value-lp_s2)>0.003f){
                        uint16_t np=(uint16_t)(pots[1].value*((float)SYNTH2_PATCH_COUNT-0.01f));
                        if(np!=s2PatchIdx && audioReady){
                            s2PatchIdx=np;
                            amy_event e=amy_default_event();
                            e.synth=SYNTH_CH; e.patch_number=s2PatchIdx;
                            amy_add_event(&e);
                            audioSetFilter(s2Cutoff, 1.5f); // re-apply LPF after patch change
                        }
                        lp_s2=pots[1].value;
                    }
                    // P3 = LPF cutoff — 80→8000 Hz power-law
                    if(fabsf(pots[3].value-lp_s2cut)>0.005f){
                        s2Cutoff=80.0f*powf(100.0f, pots[3].value);
                        if(audioReady) audioSetFilter(s2Cutoff, 1.5f);
                        lp_s2cut=pots[3].value;
                    }
                    break;
                }
                case MODE_303: {
                    // When FX overlay is open, redirect pots to FX param control (same as MODE_FX)
                    if (s_overlay == OVERLAY_FX) {
                        if (fxList[fxSelected].active) {
                            static float lp303fx[4]={-1,-1,-1,-1};
                            static uint8_t lp303FxSel=0xFF;
                            if (lp303FxSel!=fxSelected || fxPotNeedSync) {
                                for (int p=0;p<4;p++) lp303fx[p]=pots[3+p].value;
                                lp303FxSel=fxSelected; fxPotNeedSync=false;
                            }
                            bool fxChanged=false;
                            for (int p=0;p<4;p++) {
                                if (fxList[fxSelected].paramNames[p][0]=='\0') continue;
                                if (fabsf(pots[3+p].value-lp303fx[p])>0.001f) {
                                    float mn=fxList[fxSelected].paramMin[p];
                                    float mx=fxList[fxSelected].paramMax[p];
                                    if (fxSelected==0 && p==0)
                                        fxList[0].params[0]=mn*powf(mx/mn, pots[3].value);
                                    else
                                        fxList[fxSelected].params[p]=mn+(mx-mn)*pots[3+p].value;
                                    lp303fx[p]=pots[3+p].value;
                                    fxChanged=true;
                                }
                            }
                            if (fxChanged && fxSelected!=0) applyFxEffect(fxSelected);
                        }
                        // Keep 303 pot baselines in sync with current pot positions so closing
                        // the FX overlay doesn't cause phantom 303 param jumps (e.g. resonance
                        // spiking because a pot was moved to control reverb level).
                        for (int p = 0; p < 4; p++) lp303[p] = pots[3+p].value;
                        break;
                    }
                    // P2 = 4-zone waveform morph: SUP(0-0.25) → SAW(0.25-0.5) → TRI(0.5-0.75) → SQR+duty(0.75-1)
                    {
                        static float lp303wf=-1.0f;
                        if(fabsf(pots[1].value-lp303wf)>0.005f){
                            float p2=pots[1].value;
                            uint8_t nw = (p2<0.25f)?SAW_UP:(p2<0.50f)?SAW_DOWN:(p2<0.75f)?TRIANGLE:PULSE;
                            if(nw!=t303Wave){ t303Wave=nw; if(audioReady) audioT303Wave(nw); }
                            if(t303Wave==PULSE){
                                float norm=(p2-0.75f)/0.25f;
                                if(audioReady) audioT303Duty(0.5f-norm*0.48f);
                            }
                            lp303wf=p2;
                        }
                    }
                    bool ch303=false;
                    if(fabsf(pots[3].value-lp303[0])>0.002f){ t303Reso=1.0f+pots[3].value*2.0f;           lp303[0]=pots[3].value; ch303=true; }
                    if(fabsf(pots[4].value-lp303[1])>0.002f){ t303EnvMod=pots[4].value*10.0f;              lp303[1]=pots[4].value; ch303=true; }
                    if(fabsf(pots[5].value-lp303[2])>0.002f){ t303Duration=pots[5].value; lp303[2]=pots[5].value; audioT303SetSustain(t303Duration); }
                    if(fabsf(pots[6].value-lp303[3])>0.002f){ t303Cutoff=80.0f*powf(25.0f,pots[6].value); lp303[3]=pots[6].value; ch303=true; }
                    if(ch303 && audioReady) audioT303Params(t303Cutoff,t303Reso,t303EnvMod,t303Decay);
                    break;
                }
                case MODE_303S: {
                    if (s_overlay == OVERLAY_FX) {
                        if (fxList[fxSelected].active) {
                            static float lp303sfx[4]={-1,-1,-1,-1};
                            static uint8_t lp303sFxSel=0xFF;
                            if (lp303sFxSel!=fxSelected || fxPotNeedSync) {
                                for (int p=0;p<4;p++) lp303sfx[p]=pots[3+p].value;
                                lp303sFxSel=fxSelected; fxPotNeedSync=false;
                            }
                            bool fxChanged=false;
                            for (int p=0;p<4;p++) {
                                if (fxList[fxSelected].paramNames[p][0]=='\0') continue;
                                if (fabsf(pots[3+p].value-lp303sfx[p])>0.001f) {
                                    float mn=fxList[fxSelected].paramMin[p];
                                    float mx=fxList[fxSelected].paramMax[p];
                                    if (fxSelected==0 && p==0)
                                        fxList[0].params[0]=mn*powf(mx/mn, pots[3].value);
                                    else
                                        fxList[fxSelected].params[p]=mn+(mx-mn)*pots[3+p].value;
                                    lp303sfx[p]=pots[3+p].value;
                                    fxChanged=true;
                                }
                            }
                            if (fxChanged && fxSelected!=0) applyFxEffect(fxSelected);
                        }
                        for (int p = 0; p < 4; p++) lp303[p] = pots[3+p].value;
                        break;
                    }
                    // P2: per-wave modulation — SQR=duty (50→2%), NAP=narrow duty (15→1%), TRI/SUP/SAW=wavefolder
                    {
                        static float lp303swf=-1.0f;
                        if(fabsf(pots[1].value-lp303swf)>0.005f){
                            float p2=pots[1].value;
                            if(t303Wave==PULSE){
                                if(audioReady) audioT303Duty(0.5f-p2*0.48f);
                            } else if(t303Wave==T303_NAP_WAVE){
                                if(audioReady) audioT303Duty(0.15f-p2*0.14f);  // narrow: [0.01, 0.15]
                            } else {
                                if(audioReady) audioT303Wavefold(p2);
                            }
                            lp303swf=p2;
                        }
                    }
                    bool ch303s=false;
                    if(fabsf(pots[3].value-lp303[0])>0.002f){ t303Reso=1.0f+pots[3].value*2.0f;           lp303[0]=pots[3].value; ch303s=true; }
                    if(fabsf(pots[4].value-lp303[1])>0.002f){ t303EnvMod=pots[4].value*10.0f;              lp303[1]=pots[4].value; ch303s=true; }
                    if(fabsf(pots[5].value-lp303[2])>0.002f){ t303Duration=pots[5].value; lp303[2]=pots[5].value; t303Decay=30.0f*powf(100.0f,t303Duration); audioT303SetSustain(0.0f); ch303s=true; }
                    if(fabsf(pots[6].value-lp303[3])>0.002f){ t303Cutoff=80.0f*powf(25.0f,pots[6].value); lp303[3]=pots[6].value; ch303s=true; }
                    if(ch303s && audioReady) audioT303Params(t303Cutoff,t303Reso,t303EnvMod,t303Decay);
                    break;
                }
                case MODE_SS2: {
                    if (s_overlay == OVERLAY_FX) {
                        if (fxList[fxSelected].active) {
                            static float lpSS2fx[4]={-1,-1,-1,-1};
                            static uint8_t lpSS2FxSel=0xFF;
                            if (lpSS2FxSel!=fxSelected || fxPotNeedSync) {
                                for (int p=0;p<4;p++) lpSS2fx[p]=pots[3+p].value;
                                lpSS2FxSel=fxSelected; fxPotNeedSync=false;
                            }
                            bool fxChanged=false;
                            for (int p=0;p<4;p++) {
                                if (fxList[fxSelected].paramNames[p][0]=='\0') continue;
                                if (fabsf(pots[3+p].value-lpSS2fx[p])>0.001f) {
                                    float mn=fxList[fxSelected].paramMin[p];
                                    float mx=fxList[fxSelected].paramMax[p];
                                    if (fxSelected==0 && p==0)
                                        fxList[0].params[0]=mn*powf(mx/mn, pots[3].value);
                                    else
                                        fxList[fxSelected].params[p]=mn+(mx-mn)*pots[3+p].value;
                                    lpSS2fx[p]=pots[3+p].value; fxChanged=true;
                                }
                            }
                            if (fxChanged && fxSelected!=0) applyFxEffect(fxSelected);
                        }
                        break;
                    }
                    break;
                }
                case MODE_DRUM2: {
                    static float lpD2[3]    = {-1.f,-1.f,-1.f};
                    static int8_t lpD2Pad   = -1;
                    static float lpD2fx[4]  = {-1.f,-1.f,-1.f,-1.f};
                    static uint8_t lpD2FxSel = 0xFF;
                    // If any FX is active, pots P3-P6 control the selected FX's parameters.
                    bool anyFxActive = false;
                    for (int f=0;f<FX_COUNT;f++) if(fxList[f].active){anyFxActive=true;break;}
                    if (anyFxActive) {
                        if (lpD2FxSel != fxSelected || fxPotNeedSync) {
                            for (int p=0;p<4;p++) lpD2fx[p]=pots[3+p].value;
                            lpD2FxSel=fxSelected; fxPotNeedSync=false;
                        }
                        bool fxChanged=false;
                        for (int p=0;p<4;p++) {
                            if (fxList[fxSelected].paramNames[p][0]=='\0') continue;
                            if (fabsf(pots[3+p].value-lpD2fx[p])>0.001f) {
                                float mn=fxList[fxSelected].paramMin[p];
                                float mx=fxList[fxSelected].paramMax[p];
                                if (fxSelected==0 && p==0)
                                    fxList[0].params[0]=mn*powf(mx/mn, pots[3].value);
                                else
                                    fxList[fxSelected].params[p]=mn+(mx-mn)*pots[3+p].value;
                                lpD2fx[p]=pots[3+p].value; fxChanged=true;
                            }
                        }
                        if (fxChanged && fxSelected!=0) applyFxEffect(fxSelected);
                        // Keep pad baselines in sync so no jump when FX are all deactivated
                        for (int p=0;p<3;p++) lpD2[p]=pots[3+p].value;
                        lpD2Pad = drum2SelPad;
                        break;
                    }
                    // No active FX — P3=Pitch  P4=Decay  P5=Volume (per selected pad)
                    lpD2FxSel = 0xFF;  // force FX pot resync next time FX activated
                    if (lpD2Pad != drum2SelPad) {
                        lpD2[0]=pots[3].value; lpD2[1]=pots[4].value; lpD2[2]=pots[5].value;
                        lpD2Pad = drum2SelPad;
                    }
                    if(fabsf(pots[3].value-lpD2[0])>0.004f){
                        drum2Pitch[drum2SelPad]=(uint8_t)constrain((int)(pots[3].value*127.f),0,127);
                        lpD2[0]=pots[3].value;
                    }
                    if(fabsf(pots[4].value-lpD2[1])>0.004f){
                        drum2Decay[drum2SelPad] = (pots[4].value < 0.05f) ? 0.0f
                                                : 50.0f * powf(40.0f, pots[4].value);
                        lpD2[1]=pots[4].value;
                    }
                    if(fabsf(pots[5].value-lpD2[2])>0.004f){
                        drum2Volume[drum2SelPad]=pots[5].value;
                        lpD2[2]=pots[5].value;
                    }
                    break;
                }
                case MODE_MOD2: {
                    // P1=Waveform  P2=BPM(global)  P3=Cutoff  P4=Reso  P5=LFO rate  P6=LFO depth
                    static float lp2[5]={-1,-1,-1,-1,-1};
                    bool m2changed=false;
                    // P1 = waveform morph (8 shapes)
                    if(fabsf(pots[1].value-lp2[0])>0.005f){
                        uint8_t ns=(uint8_t)(pots[1].value*((float)MOD2_SHAPE_COUNT-0.01f));
                        if(ns!=mod2ShapeIdx){ mod2ShapeIdx=ns; audioSetShape(mod2ShapeSteps[mod2ShapeIdx]); }
                        lp2[0]=pots[1].value;
                    }
                    // P3 = Filter Cutoff — power-law: 80..8000 Hz
                    if(fabsf(pots[3].value-lp2[1])>0.005f){
                        mod2Cutoff=80.0f*powf(100.0f,pots[3].value);
                        m2changed=true; lp2[1]=pots[3].value;
                    }
                    // P4 = Resonance — linear: 0.5..4
                    if(fabsf(pots[4].value-lp2[2])>0.01f){
                        mod2Reso=0.5f+pots[4].value*2.5f;  // 0.5→3.0
                        m2changed=true; lp2[2]=pots[4].value;
                    }
                    if(m2changed && audioReady) audioSetFilter(mod2Cutoff,mod2Reso);
                    // P5 = LFO rate — rate³ × 25 Hz
                    if(fabsf(pots[5].value-lp2[3])>0.002f){
                        mod2LfoRate=powf(pots[5].value,3.0f)*25.0f;
                        lp2[3]=pots[5].value;
                    }
                    // P6 = LFO depth
                    if(fabsf(pots[6].value-lp2[4])>0.002f){
                        mod2LfoDepth=pots[6].value*5.0f;
                        lp2[4]=pots[6].value;
                    }
                    break;
                }
                case MODE_MIDI: {
#if CONFIG_TINYUSB_MIDI_ENABLED
                    if (!midiActive) break;
                    for (int p = 0; p < 7; p++) {
                        if (midiPotCC[p] > 127) continue;  // 0xFF = disabled
                        if (fabsf(pots[p].value - midiLpPots[p]) > 0.005f) {
                            usbMIDI.controlChange(midiPotCC[p], (uint8_t)(pots[p].value * 127.f), midiChannel);
                            midiLpPots[p] = pots[p].value;
                        }
                    }
#endif
                    break;
                }
                default: break;
            }
        }

        // LFO filter modulation (10ms tick) — FX chain LFO slot
        if(fxList[6].active&&audioReady){
            static float lfoPhase=0.0f;
            static uint8_t lfoLogTick=0;
            float rate  = fxList[6].params[0];
            float depth = fxList[6].params[1];
            lfoPhase += rate * 2.0f * (float)M_PI * 0.01f;
            if(lfoPhase > 2.0f*(float)M_PI) lfoPhase -= 2.0f*(float)M_PI;
            float lfoMod = (1.0f + sinf(lfoPhase)) * 0.5f;
            float baseCut = fxList[0].active ? fxList[0].params[0] : 4000.0f;
            float baseRes = fxList[0].active ? fxList[0].params[1] : 1.5f;
            float cut = baseCut * (1.0f - depth * lfoMod * 0.8f);
            if(++lfoLogTick >= 50){ // log every 500ms
                Serial.printf("LFO tick cut=%.0f res=%.2f mod=%.2f\n", cut, baseRes, lfoMod);
                lfoLogTick=0;
            }
            audioSetAllFilters(cut, baseRes);
        }

        // Detect FILT type change (params[2]); re-apply immediately (biquad reset on type change is expected).
        if (fxList[0].active && audioReady) {
            static const uint8_t kFiltAMY2[] = {FILTER_LPF, FILTER_LPF24, FILTER_HPF, FILTER_BPF};
            static uint8_t s_fxFiltTypeApplied = 0xFF;
            uint8_t ti = (uint8_t)constrain((int)roundf(fxList[0].params[2]), 0, 3);
            if (ti != s_fxFiltTypeApplied) { s_fxFiltTypeApplied = ti; applyFxEffect(0); }
        }

        // Smooth FILT cutoff application — anti-zipper when turning the cutoff encoder.
        // Use audioSetFilterFreq (no filter_type field) to avoid AMY biquad state resets
        // that cause an audible pop/click on each value change.
        if (fxList[0].active && !fxList[6].active && audioReady) {
            float target = fxList[0].params[0];
            float prevCut = lpfSmoothCut;
            lpfSmoothCut += (target - lpfSmoothCut) * 0.5f; // faster convergence (~30ms to 90%)
            audioSetFilterFreq(lpfSmoothCut, fxList[0].params[1]);
            // audioSetFilterFreq targets SYNTH_CH only — T303_CH needs its own event.
            if (currentMode == MODE_303) {
                amy_event t3e = amy_default_event();
                t3e.synth = T303_CH;
                t3e.filter_freq_coefs[COEF_CONST] = lpfSmoothCut;
                t3e.filter_freq_coefs[COEF_EG0]   = t303EnvMod;
                t3e.resonance = fxList[0].params[1];
                amy_add_event(&t3e);
            }
            // Keep GR2 oscillators in sync while the cutoff is actively converging.
            // Only fires during the ~10 ticks after a cutoff change (when fabsf delta > 1 Hz).
            if (fabsf(lpfSmoothCut - prevCut) > 1.0f)
                audioSetGranular2FilterFreq(lpfSmoothCut, fxList[0].params[1]);
        }

        // MODULAR: joystick Y pitch bend + LFO vibrato (10ms tick)
        if(currentMode==MODE_MODULAR&&!menuOpen&&audioReady){
            static float modLfoPhase=0.0f;
            // Advance LFO
            modLfoPhase += modLfoRate * 2.0f * (float)M_PI * 0.01f;
            if(modLfoPhase > 2.0f*(float)M_PI) modLfoPhase -= 2.0f*(float)M_PI;
            // Joystick Y → pitch bend (same behaviour as SYNTH mode)
            float jy = cachedJoyY / 64.0f;
            float baseBend = (fabsf(jy) < 0.15f) ? 0.0f : -jy * 2.0f;  // ±2 semitones
            // LFO → vibrato: sinusoidal ±modLfoDepth semitones
            float vibrato = (modLfoDepth > 0.01f) ? sinf(modLfoPhase) * modLfoDepth : 0.0f;
            audioSetPitchBend(powf(2.0f, (baseBend + vibrato) / 12.0f));
        }

        // MOD2: joystick Y pitch bend + LFO (10ms tick)
        if(currentMode==MODE_MOD2&&!menuOpen&&audioReady){
            static float mod2LfoPhase=0.0f;
            static float shSample=0.0f;   // S&H held value
            float prevPhase = mod2LfoPhase;
            mod2LfoPhase += mod2LfoRate * 2.0f * (float)M_PI * 0.01f;
            if(mod2LfoPhase >= 2.0f*(float)M_PI){
                mod2LfoPhase -= 2.0f*(float)M_PI;
                shSample = ((float)random(0,2001)-1000.0f)/1000.0f; // new S&H on cycle wrap
            }

            // Compute waveform sample (-1..+1) from current shape
            float sample = 0.0f;
            uint8_t shape = mod2LfoModeShape[mod2LfoMode];
            switch(shape){
                case 0: sample = sinf(mod2LfoPhase); break;                                // Sine
                case 1: sample = (mod2LfoPhase < (float)M_PI)                              // Triangle
                               ? (-1.0f + mod2LfoPhase * 2.0f / (float)M_PI)
                               : (3.0f  - mod2LfoPhase * 2.0f / (float)M_PI); break;
                case 2: sample = mod2LfoPhase / (float)M_PI - 1.0f; break;                // Sawtooth ↑
                case 3: sample = 1.0f - mod2LfoPhase / (float)M_PI; break;                // Reverse Saw ↓
                case 4: sample = (mod2LfoPhase < (float)M_PI) ? 1.0f : -1.0f; break;     // Square
                case 5: sample = shSample; break;                                          // Sample & Hold
            }

            uint8_t dest = mod2LfoModeDest[mod2LfoMode];
            float jy2 = cachedJoyY / 64.0f;
            float baseBend2 = (fabsf(jy2) < 0.15f) ? 0.0f : -jy2 * 2.0f;

            // Slide: linear glide over one beat duration
            float slideSemis = 0.0f;
            if(slideActive){
                unsigned long elapsed = millis() - slideStartMs;
                unsigned long slideMs = max(10UL, 60000UL / (unsigned long)bpm);
                if(elapsed >= slideMs){
                    // Glide complete — switch to target note
                    audioNoteOff(slideFromNote);
                    float jx=constrain(cachedJoyX/64.0f,-1.0f,1.0f);
                    audioNoteOn(slideToNote, constrain(0.8f+jx*0.6f, 0.05f, 1.5f));
                    mod2LastNote=slideToNote;
                    slideActive=false;
                } else {
                    float t = (float)elapsed / (float)slideMs;
                    slideSemis = (float)((int)slideToNote-(int)slideFromNote) * t;
                }
            }

            // Combine JY bend + vibrato/slide → single pitchBend call
            if(dest == 1){
                float vibrato2 = (mod2LfoDepth > 0.01f) ? sample * mod2LfoDepth : 0.0f;
                audioSetPitchBend(powf(2.0f, (baseBend2 + vibrato2 + slideSemis) / 12.0f));
            } else {
                audioSetPitchBend(powf(2.0f, (baseBend2 + slideSemis) / 12.0f));
            }
            if(dest == 2 && mod2LfoDepth > 0.01f){  // Filter LFO
                float lfoVal = (sample + 1.0f) * 0.5f;
                float cut = mod2Cutoff * (1.0f - lfoVal * mod2LfoDepth * 0.2f);
                cut = constrain(cut, 80.0f, 8000.0f);
                audioSetFilter(cut, mod2Reso);
            }
        }

        // 303 portamento slide: interpolate pitch_bend from t303SlideFrom to t303SlideTo
        if((currentMode==MODE_303||currentMode==MODE_303S) && t303SlideActive && audioReady){
            unsigned long elapsed = millis() - t303SlideMs;
            unsigned long slideDur = max(30UL, 60000UL / (unsigned long)bpm / 4); // one 16th note
            if(elapsed >= slideDur){
                audioT303PitchBend(1.0f);
                t303SlideActive = false;
            } else {
                float frac  = (float)elapsed / (float)slideDur;
                float semis = (float)(int8_t)((int)t303SlideFrom - (int)t303SlideTo) * (1.0f - frac);
                audioT303PitchBend(powf(2.0f, semis / 12.0f));
            }
        }

        // 303 joystick Y: real-time filter cutoff offset (push up = filter opens)
        if(currentMode==MODE_303 && !menuOpen && audioReady){
            static float last303CutSent = -1.0f;
            float jy303 = cachedJoyY / 64.0f;
            float jyOff = (fabsf(jy303) > 0.12f) ? jy303 * 2500.0f : 0.0f;
            float effCut = constrain(t303Cutoff + jyOff, 80.0f, 8000.0f);
            if (fabsf(effCut - last303CutSent) > 15.0f) {
                amy_event e = amy_default_event();
                e.synth = T303_CH;
                e.filter_freq_coefs[COEF_CONST] = effCut;
                e.filter_freq_coefs[COEF_EG0]   = t303EnvMod;  // pair with EG0 — must always send both
                amy_add_event(&e);
                last303CutSent = effCut;
            }
        }

        // 303S joystick: X=wave (TRI/SUP/SAW/SQR), Y=octave (debounced, hold-scroll)
        if (currentMode==MODE_303S && !menuOpen) {
            static unsigned long s303jWvMs=0, s303jOctMs=0;
            static bool s303jWvArm=false, s303jOctArm=false;
            float jx=cachedJoyX/64.0f, jy=cachedJoyY/64.0f;
            unsigned long now=millis();
            bool jxH=(fabsf(jx)>0.45f), jyH=(fabsf(jy)>0.45f);
            if(!jxH) s303jWvArm=false;
            if(!jyH) s303jOctArm=false;
            if(jxH && (!s303jWvArm || now-s303jWvMs>=150)){
                int8_t dir=(jx>0)?1:-1;
                static const uint8_t kT303Waves[]={TRIANGLE, SAW_UP, SAW_DOWN, PULSE, T303_NAP_WAVE};
                uint8_t ci=0;
                for(uint8_t i=0;i<5;i++) if(kT303Waves[i]==t303Wave){ci=i;break;}
                ci=(uint8_t)((ci+5+dir)%5);
                t303Wave=kT303Waves[ci];
                uint8_t amyWv=(t303Wave==T303_NAP_WAVE)?PULSE:t303Wave;
                if(audioReady) audioT303Wave(amyWv);
                if(t303Wave==PULSE        && audioReady) audioT303Wavefold(0.0f);
                if(t303Wave==T303_NAP_WAVE && audioReady) { audioT303Wavefold(0.0f); audioT303Duty(0.10f); }
                s303jWvMs=now; s303jWvArm=true;
            }
            if(jyH && (!s303jOctArm || now-s303jOctMs>=300)){
                int8_t dir=(jy>0)?-1:1;
                t303Oct=(int8_t)constrain(t303Oct+dir,-3,3);
                s303jOctMs=now; s303jOctArm=true;
            }
        }

        // SS2 joystick: PAD view always scrolls file browser; SEQ view scrolls step/slot
        if (currentMode==MODE_SS2 && !menuOpen && sdReady) {
            static unsigned long ss2jMs=0;
            if (millis()-ss2jMs >= 150) {
                float jy=cachedJoyY/64.0f;
                if (!ss2SeqView) {
                    // PAD: JY scrolls file list
                    if (jy < -0.4f && sdCursor > 0)           { sdCursor--; if(sdCursor<sdScroll) sdScroll=sdCursor; ss2jMs=millis(); }
                    if (jy >  0.4f && sdCursor<sdFileCount-1) { sdCursor++; if(sdCursor>=sdScroll+8) sdScroll++; ss2jMs=millis(); }
                } else {
                    float jx=cachedJoyX/64.0f;
                    if (jy < -0.4f && ss2SelSlot > 0)             { ss2SelSlot--; ss2jMs=millis(); }
                    if (jy >  0.4f && ss2SelSlot < SS2_SLOTS-1)   { ss2SelSlot++; ss2jMs=millis(); }
                    if (jx < -0.4f && ss2SelStep > 0)             { ss2SelStep--; ss2jMs=millis(); }
                    if (jx >  0.4f && ss2SelStep < SS2_SLOTS-1)   { ss2SelStep++; ss2jMs=millis(); }
                }
            }
        }


        // MIDI joystick: Y=Pitch Bend (vertical), X=Modulation CC#1 (push right)
#if CONFIG_TINYUSB_MIDI_ENABLED
        if (currentMode == MODE_MIDI && midiActive && !menuOpen) {
            float jx = cachedJoyX / 64.0f;  // -1..+1
            float jy = cachedJoyY / 64.0f;
            // Pitch Bend: Y axis (push down = bend up), ±5% deadzone
            double pb = (fabsf(jy) > 0.05) ? (double)-jy : 0.0;
            if (fabsf((float)pb - midiLastPB) > 0.008f) {
                usbMIDI.pitchBend(pb, midiChannel);
                midiLastPB = (float)pb;
            }
            // Modulation: X axis, full range — left(-1)=0, center=64, right(+1)=127
            uint8_t mod = (uint8_t)constrain((int)((jx + 1.0f) * 63.5f), 0, 127);
            if (mod != midiLastMod) {
                usbMIDI.controlChange(1, mod, midiChannel);
                midiLastMod = mod;
            }
        }
#endif

        // SYNTH2: joystick Y scrolls patches (150ms debounce)
        if(currentMode==MODE_SYNTH2&&!menuOpen&&audioReady){
            static unsigned long lastS2Nav=0;
            float ny2=cachedJoyY/64.0f;
            if(millis()-lastS2Nav>=150){
                if(ny2<-0.3f&&s2PatchIdx>0){
                    s2PatchIdx--;
                    amy_event e=amy_default_event(); e.synth=SYNTH_CH; e.patch_number=s2PatchIdx; amy_add_event(&e);
                    lastS2Nav=millis();
                }
                if(ny2>0.3f&&s2PatchIdx<SYNTH2_PATCH_COUNT-1){
                    s2PatchIdx++;
                    amy_event e=amy_default_event(); e.synth=SYNTH_CH; e.patch_number=s2PatchIdx; amy_add_event(&e);
                    lastS2Nav=millis();
                }
            }
        }

        // GRANULAR pots: frame-by-frame delta — no jump on slice change, fully bidirectional.
        // Pot A (pots[3]) → sx or window start.  Pot B (pots[4]) → sx+1 or window end.
        // Split control: pure delta — the pot's movement (not position) is applied to the
        // split. Baseline re-syncs on every key press and on state changes (sub-mode toggle,
        // new sample load) so the pots are always responsive immediately.
        // Bidirectional cascade keeps splits[] non-decreasing: if sx crosses a neighbor,
        // that neighbor is pushed to sx and the propagation continues outward.
        if (currentMode==MODE_GRANULAR && granComputed && audioReady) {
            static float lastPA = 0.0f, lastPB = 0.0f;
            static unsigned long winDebMs = 0;

            if (granPotNeedsSync) {
                granPotNeedsSync = false;
                lastPA = pots[3].value;
                lastPB = pots[4].value;
            }

            if (s_overlay == OVERLAY_FX) {
                // FX overlay active: pots control the selected effect instead of splits.
                if (fxList[fxSelected].active) {
                    static float lpGranFx[4] = {-1,-1,-1,-1};
                    static uint8_t lpGranFxSel = 0xFF;
                    if (lpGranFxSel != fxSelected || fxPotNeedSync) {
                        for (int p = 0; p < 4; p++) lpGranFx[p] = pots[3+p].value;
                        lpGranFxSel = fxSelected; fxPotNeedSync = false;
                    }
                    bool fxChanged = false;
                    for (int p = 0; p < 4; p++) {
                        if (fxList[fxSelected].paramNames[p][0] == '\0') continue;
                        if (fabsf(pots[3+p].value - lpGranFx[p]) > 0.001f) {
                            float mn = fxList[fxSelected].paramMin[p];
                            float mx = fxList[fxSelected].paramMax[p];
                            if (fxSelected == 0 && p == 0)
                                fxList[0].params[0] = mn * powf(mx/mn, pots[3].value);
                            else
                                fxList[fxSelected].params[p] = mn + (mx - mn) * pots[3+p].value;
                            lpGranFx[p] = pots[3+p].value;
                            fxChanged = true;
                        }
                    }
                    if (fxChanged && fxSelected != 0) applyFxEffect(fxSelected);
                }
                lastPA = pots[3].value; lastPB = pots[4].value;
            } else if (granPlayingSlice >= 0 && granPlayingSlice < granSliceCount) {
                // --- Split control (delta, bidirectional cascade) ---
                int x = granPlayingSlice;
                float dpA = pots[3].value - lastPA;
                float dpB = pots[4].value - lastPB;
                lastPA = pots[3].value;
                lastPB = pots[4].value;
                bool changed = false;

                // Pot A → splits[x]. All splits movable, including splits[0] (start of used region).
                if (fabsf(dpA) > 0.002f) {
                    granSplits[x] = constrain(granSplits[x] + dpA, 0.0f, 1.0f);
                    for (int i = x-1; i >= 0; i--) {
                        if (granSplits[i] > granSplits[i+1]) granSplits[i] = granSplits[i+1];
                        else break;
                    }
                    for (int i = x+1; i <= granSliceCount; i++) {
                        if (granSplits[i] < granSplits[i-1]) granSplits[i] = granSplits[i-1];
                        else break;
                    }
                    changed = true;
                }

                // Pot B → splits[x+1]. All splits movable, including splits[N] (end of used region).
                if (fabsf(dpB) > 0.002f) {
                    granSplits[x+1] = constrain(granSplits[x+1] + dpB, 0.0f, 1.0f);
                    for (int i = x; i >= 0; i--) {
                        if (granSplits[i] > granSplits[i+1]) granSplits[i] = granSplits[i+1];
                        else break;
                    }
                    for (int i = x+2; i <= granSliceCount; i++) {
                        if (granSplits[i] < granSplits[i-1]) granSplits[i] = granSplits[i-1];
                        else break;
                    }
                    changed = true;
                }

                if (changed) audioApplyGranularSplits(granSplits, granSliceCount);
            } else {
                // --- Global window control (no slice selected) ---
                float dpA = (pots[3].value - lastPA) * 0.5f;
                float dpB = (pots[4].value - lastPB) * 0.5f;
                lastPA = pots[3].value; lastPB = pots[4].value;

                if (fabsf(dpA) > 0.004f || fabsf(dpB) > 0.004f) {
                    float ws = granWinStart + dpA;
                    float we = granWinEnd   + dpB;
                    if (ws < 0.0f) ws = 0.0f;
                    if (we > 1.0f) we = 1.0f;
                    if (we < ws + 0.05f) we = ws + 0.05f;
                    if (we > 1.0f) { we = 1.0f; ws = we - 0.05f; if (ws < 0.0f) ws = 0.0f; }
                    granWinStart = ws; granWinEnd = we;
                    winDebMs = millis();
                }
                if (winDebMs && millis() - winDebMs >= 300) {
                    winDebMs = 0;
                    audioSetGranularWindow(granWinStart, granWinEnd);
                    granSliceCount = audioComputeGranularSlices(granSubMode & 1u, granWaveform);
                    granComputed = (granSliceCount > 0);
                    if (granComputed)
                        for (int i = 0; i <= granSliceCount; i++) granSplits[i] = i / (float)granSliceCount;
                }
            }
        }

        // GRANULAR2 pots: per-sample split delta control (same cascade logic as GRANULAR).
        // Pot A (pots[3]) → splits[sliceIdx], Pot B (pots[4]) → splits[sliceIdx+1].
        // Only affects the active sample; other samples' splits are untouched.
        if (currentMode == MODE_GRANULAR2 && audioReady) {
            static float g2LastPA = 0.0f, g2LastPB = 0.0f;
            static float lpGr2Fx[4] = {-1,-1,-1,-1};
            static uint8_t lpGr2FxSel = 0xFF;

            bool anyFxActive = false;
            for (uint8_t fi = 0; fi < FX_COUNT; fi++) if (fxList[fi].active) { anyFxActive = true; break; }

            if (anyFxActive) {
                if (fxList[fxSelected].active) {
                    if (lpGr2FxSel != fxSelected || fxPotNeedSync) {
                        for (int p = 0; p < 4; p++) lpGr2Fx[p] = pots[3+p].value;
                        lpGr2FxSel = fxSelected; fxPotNeedSync = false;
                    }
                    bool fxChanged = false;
                    for (int p = 0; p < 4; p++) {
                        if (fxList[fxSelected].paramNames[p][0] == '\0') continue;
                        if (fabsf(pots[3+p].value - lpGr2Fx[p]) > 0.001f) {
                            float mn = fxList[fxSelected].paramMin[p];
                            float mx = fxList[fxSelected].paramMax[p];
                            if (fxSelected == 0 && p == 0)
                                fxList[0].params[0] = mn * powf(mx/mn, pots[3].value);
                            else
                                fxList[fxSelected].params[p] = mn + (mx - mn) * pots[3+p].value;
                            lpGr2Fx[p] = pots[3+p].value;
                            fxChanged = true;
                        }
                    }
                    if (fxChanged) {
                        if (fxSelected != 0) {
                            applyFxEffect(fxSelected);
                        } else {
                            // LPF: sync smooth value immediately so new notes use current cutoff,
                            // then push the update to any currently-playing GR2 oscillators.
                            lpfSmoothCut = fxList[0].params[0];
                            audioSetGranular2FilterFreq(lpfSmoothCut, fxList[0].params[1]);
                        }
                    }
                }
                // Sync split baselines so closing FX overlay doesn't cause phantom split jumps.
                g2LastPA = pots[3].value; g2LastPB = pots[4].value;
            } else if (gran2ActiveSample >= 0) {
                uint8_t as = (uint8_t)gran2ActiveSample;
                if (as < GRAN2_MAX_SAMPLES && gran2[as].computed && gran2ActiveSlice >= 0) {
                    if (gran2PotNeedsSync) {
                        gran2PotNeedsSync = false;
                        // Recenter both encoders so splits can move in either direction
                        // regardless of where they bottomed out on the previous key press.
                        pots[3].value = 0.5f; g2LastPA = 0.5f;
                        pots[4].value = 0.5f; g2LastPB = 0.5f;
                    }

                    int x = gran2ActiveSlice;
                    Gran2State& gs = gran2[as];
                    float dpA = pots[3].value - g2LastPA;
                    float dpB = pots[4].value - g2LastPB;
                    g2LastPA = pots[3].value;
                    g2LastPB = pots[4].value;
                    bool changed = false;

                    if (fabsf(dpA) > 0.002f) {
                        gs.splits[x] = constrain(gs.splits[x] + dpA, 0.0f, 1.0f);
                        // Cascade left: pull lower neighbours down if they overshot
                        for (int i = x-1; i >= 0; i--) {
                            if (gs.splits[i] > gs.splits[i+1]) gs.splits[i] = gs.splits[i+1]; else break;
                        }
                        // Cascade right: push upper neighbours up if they were overtaken
                        for (int i = x+1; i <= gs.sliceCount; i++) {
                            if (gs.splits[i] < gs.splits[i-1]) gs.splits[i] = gs.splits[i-1]; else break;
                        }
                        changed = true;
                    }
                    if (fabsf(dpB) > 0.002f) {
                        gs.splits[x+1] = constrain(gs.splits[x+1] + dpB, 0.0f, 1.0f);
                        // Cascade left: pull x (and lower) down if splits[x+1] went below them
                        for (int i = x; i >= 0; i--) {
                            if (gs.splits[i] > gs.splits[i+1]) gs.splits[i] = gs.splits[i+1]; else break;
                        }
                        // Cascade right: push x+2 (and higher) up if splits[x+1] overtook them
                        for (int i = x+2; i <= gs.sliceCount; i++) {
                            if (gs.splits[i] < gs.splits[i-1]) gs.splits[i] = gs.splits[i-1]; else break;
                        }
                        changed = true;
                    }
                    if (changed) {
                        audioApplyGranular2Splits(as, gs.splits, gs.sliceCount, gran2PlayMode == 1);
                        // Presets are updated in-place; the PCM renderer picks up the new loop
                        // boundary on the next audio block without a playback reset.
                    }
                }
            }
        }

        // Global pitch bend via JY — SYNTH / SYNTH2 / HYBRID (MODULAR/MOD2 handle it themselves)
        if((currentMode==MODE_SYNTH||currentMode==MODE_SYNTH2||currentMode==MODE_HYBRID)&&!menuOpen&&audioReady){
            float jy=cachedJoyY/64.0f;
            float bend=(fabsf(jy)<0.15f)?0.0f:-jy*2.0f;  // ±2 semitones, centre deadzone
            audioSetPitchBend(powf(2.0f,bend/12.0f));
        }

        // SD browser joystick navigation (SAMPLE, SEQ, and GRANULAR — always browsable)
        bool needsSdNav = (currentMode==MODE_SAMPLE||currentMode==MODE_SEQ)
                       || currentMode==MODE_GRANULAR
                       || currentMode==MODE_GRANULAR2;
        if(needsSdNav&&!menuOpen&&sdReady){
            static unsigned long lastSdNav=0;
            float ny=cachedJoyY/64.0f;
            bool gran2AnyComputed = false;
            if (currentMode==MODE_GRANULAR2) for (uint8_t _s=0;_s<GRAN2_MAX_SAMPLES;_s++) if (gran2[_s].computed) { gran2AnyComputed=true; break; }
            uint8_t visLines=(currentMode==MODE_SEQ)?7
                            :(currentMode==MODE_GRANULAR&&!granComputed)?13
                            :(currentMode==MODE_GRANULAR&&granComputed)?8
                            :(currentMode==MODE_GRANULAR2&&!gran2AnyComputed)?13
                            :(currentMode==MODE_GRANULAR2)?7:8;
            if(millis()-lastSdNav>=150){
                if(ny<-0.3f&&sdCursor>0){sdCursor--;if(sdCursor<sdScroll)sdScroll=sdCursor;lastSdNav=millis();}
                if(ny>0.3f&&sdCursor<sdFileCount-1){sdCursor++;if(sdCursor>=(int)(sdScroll+visLines))sdScroll=sdCursor-visLines+1;lastSdNav=millis();}
            }
        }

        // ---- TRACKER: step advance + note playback ----
        if(currentMode==MODE_TRACKER && trkPlaying && audioReady){
            uint32_t stepMs = 60000UL / (uint32_t)bpm / 4;  // 16th note interval
            if(millis() - trkLastStepMs >= stepMs){
                trkLastStepMs += stepMs;
                trkStep = (trkStep + 1) % TRACKER_STEPS;
                for(int t=0; t<TRACKER_TRACKS; t++){
                    float vel = (trkNotes[t][trkStep][0] >= 0) ? trkVel[t][trkStep] / 127.0f : 0.7f;
                    if(vel < 0.05f) vel = 0.7f;
                    if(t < TRACKER_SYNTHS){
                        // Release previous chord notes on this track's dedicated AMY channel
                        for(int ci=0;ci<TRK_CHORD_SIZE;ci++){
                            if(trkPlayingNote[t][ci]>=0){
                                audioTrackerNoteOff((uint8_t)t,(uint8_t)trkPlayingNote[t][ci]);
                                trkPlayingNote[t][ci]=-1;
                            }
                        }
                        if(trkNotes[t][trkStep][0]<0) continue;
                        // Play all chord notes for this step on the track's own channel
                        for(int ci=0;ci<TRK_CHORD_SIZE;ci++){
                            if(trkNotes[t][trkStep][ci]<0) break;
                            audioTrackerNoteOn((uint8_t)t,(uint8_t)trkNotes[t][trkStep][ci],vel);
                            trkPlayingNote[t][ci]=trkNotes[t][trkStep][ci];
                        }
                    } else if(t == TRACKER_DRUM_TRK){
                        if(trkNotes[t][trkStep][0]<0) continue;
                        audioPlayDrumPad((uint8_t)trkNotes[t][trkStep][0], vel);
                    } else {
                        if(trkNotes[t][trkStep][0]<0) continue;
                        audioPlayKey((uint8_t)(t - TRACKER_SAMP_BASE), vel);
                    }
                }
            }
        }

        // ---- MIDI receive: host→LED (LaunchPad protocol) ----
#if CONFIG_TINYUSB_MIDI_ENABLED
        if(currentMode==MODE_MIDI && midiActive){
            midiEventPacket_t pkt;
            while(usbMIDI.readPacket(&pkt)){
                uint8_t status = pkt.byte1 & 0xF0;
                uint8_t note   = pkt.byte2;
                uint8_t velo   = pkt.byte3;
                if(note > 63) continue;  // LaunchPad grid is 8×8 = 64 notes
                uint8_t r = note / 8;
                uint8_t c = note % 8;
                if(r < KBD_ROWS && c < KBD_COLS){
                    if(status == 0x90 && velo > 0) midiLedVel[r][c] = velo;
                    else                            midiLedVel[r][c] = 0;
                }
            }
        }
#endif

        // Menu joystick navigation
        if(menuOpen){
            static unsigned long lastNav=0;
            float nx=cachedJoyX/64.0f,ny=cachedJoyY/64.0f;
            if(millis()-lastNav>=200){bool m=false;
                if(ny<-0.3f&&menuRow>0){menuRow--;m=true;}
                if(ny>0.3f&&menuRow<MENU_ROWS-1){menuRow++;m=true;}
                if(nx<-0.3f&&menuCol>0){menuCol--;m=true;}
                if(nx>0.3f&&menuCol<MENU_COLS-1){menuCol++;m=true;}
                if(m)lastNav=millis();
            }
        }
    }

    // ---- DISPLAY (100ms, or 200ms at high BPM to reduce sequencer jitter) ----
    // sendBuffer() blocks ~47ms (I2C 400kHz × 128×128px). At 600 BPM step=25ms,
    // one step is always late after each display. Doubling the interval halves jitter frequency.
    static unsigned long lastScr=0;
    { unsigned long scrMs = (bpm > 250) ? 200UL : 100UL;
      if(millis()-lastScr>=scrMs){lastScr=millis();drawScreen();} }

    // ---- POWER (500ms) ----
    static unsigned long lastPwr=0;
    if(millis()-lastPwr>=500){lastPwr=millis();
        if(!digitalRead(PWR_SENSE)){delay(50);if(!digitalRead(PWR_SENSE))digitalWrite(PWR_ON_EN,LOW);}
    }
}
