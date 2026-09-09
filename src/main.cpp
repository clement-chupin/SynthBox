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
#include "midi_usb.h"
#include "jpegdec.h"
#include "sprites/pokemon_sprites.h"

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
// Strum voice control (pot 2 = volume, pot 3 = shape when no FX active)
static float   omniStrumVol    = 0.7f; // strum velocity multiplier (0.1–2.0)
// Strum note tracking for auto-release when using SYNTH_CH
static uint8_t omniStrumNote[8] = {};  // MIDI note at each strum position (0 = free)
static uint32_t omniStrumTime[8] = {}; // millis() when note was triggered
static uint8_t omniStrumWave = SINE;   // wave type for raw OSC strum (pot 3)

// Sample play modes
uint8_t  samplePlayMode = 0;  // 0=NRM 1=STA 2=LOP 3=SLO 4=SQL
static const char* kSplayModes[] = {"NRM","STA","LOP","SLO","SQL"};
uint32_t sampleLoopNext[SAMPLE_KEY_COUNT] = {};

// Sample SQL (sequential loop) state
struct SampleSqlEntry { uint8_t row; uint8_t col; };
#define SAMPLE_SQL_MAX 16
static SampleSqlEntry g_sampleSqlLoop[SAMPLE_SQL_MAX];
static uint8_t  g_sampleSqlCount    = 0;
static uint8_t  g_sampleSqlPlayHead = 0;
static uint8_t  g_sampleSqlWriteIdx = 0;
static uint32_t g_sampleSqlEndMs    = 0;

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
    OVERLAY_SYSEQ, OVERLAY_SEQB2, OVERLAY_PKMN, OVERLAY_I303_WAVE, OVERLAY_MOD2_ALGO,
    OVERLAY_FX_MOD   // double-click an active FX slot in OVERLAY_FX to automate one of its params
};
static OverlayType    s_overlay        = OVERLAY_NONE;
static unsigned long  s_overlayCloseAt = 0;   // millis() when overlay auto-closes (0=no pending close)

// Shared double-click detection, replacing ~6 previously independently-duplicated
// `static uint32_t lastMs; (millis()-lastMs)<350` sites. checkDoubleClickId additionally
// requires the same identity (e.g. same row/col, same instrument index) as the previous
// click, for sites that must not treat two clicks on DIFFERENT items as a double-click.
static bool checkDoubleClick(uint32_t &lastMs, uint32_t windowMs=350) {
    uint32_t now = millis();
    bool isDbl = (now - lastMs) < windowMs;
    lastMs = isDbl ? 0 : now; // consume on hit, so a rapid 3rd click isn't also a "double"
    return isDbl;
}
static bool checkDoubleClickId(uint32_t &lastMs, uint8_t &lastA, uint8_t &lastB, uint8_t a, uint8_t b, uint32_t windowMs=350) {
    uint32_t now = millis();
    bool isDbl = (a==lastA && b==lastB) && (now-lastMs) < windowMs;
    lastA=a; lastB=b;
    lastMs = isDbl ? 0 : now;
    return isDbl;
}

// SEQ play mode: Full=loops, CutBar=plays 8 steps once then stops, CutNote=stops prev on same track
enum SeqPlayMode : uint8_t { SEQ_FULL=0, SEQ_CUT_BAR, SEQ_CUT_NOTE };
static SeqPlayMode seqPlayMode = SEQ_FULL;
static const char* kSeqPlayModes[] = {"Full","CBar","CNot"};


// Smoothed battery voltage (EMA updated every 10ms tick)
static float s_battVSmooth = 8.0f;

// MODULAR state — wavetable dual-oscillator synth (Serum-inspired MVP): OSC A + OSC B
// (AMY's WAVETABLE wave, independent morph position each, shared table + a small fixed
// detune on B for thickness) -> shared filter -> LFO wobbling osc B's morph position for
// that classic "evolving wavetable" motion. See audioModularOscInit() etc, audio_engine.cpp.
static uint8_t  modOscTable  = 0;      // osc A wavetable index (0-4) — P2
static uint8_t  modOscBTable = 0;      // osc B wavetable index (0-4) — B3 cycles it independently
static int8_t   modFocusIdx  = 0;      // 0=OscA 1=OscB 2=Filter 3=Env 4=LFO — which live curve is drawn large
                                        // and which element JY modifies. JX always cycles
                                        // this; the joystick has no performance role in this
                                        // mode (no velocity/bend) — it's purely a live
                                        // parameter-shaping control paired with the curve.
static float    modLfoPhase  = 0.0f;   // file-scope (was a local static in loop()) so drawScreen() can plot a live moving dot
static float    modOscAPos   = 0.0f;   // osc A morph position (0-1)
static float    modOscBPos   = 0.3f;   // osc B morph position, BASE value before LFO wobble
static float    modCutoff    = 4000.0f;
static float    modReso      = 1.5f;
static float    modLfoRate   = 0.3f;   // slow by default — evolving texture, not vibrato
static float    modLfoDepth  = 0.0f;
static const float MOD_OSCB_DETUNE_SEMIS = 0.08f; // subtle fixed detune for chorus-y thickness

// MOD2 state — modular synthesizer with 8 algorithms
static uint8_t      mod2AlgoIdx    = 0;          // index into kMod2Algos[]
static uint8_t      mod2EnvIdx     = 0;          // ENV_NORMAL..ENV_PIANO
static float        mod2P[4]       = {0.5f,0.08f,0.0f,0.25f}; // normalized P4-P7 (0-1)
static float        mod2PCache[4]  = {-1,-1,-1,-1};            // change detection
static float        mod2CurrentCutoff = 4000.0f; // live cutoff Hz (for LFO filter mod)
static float        mod2CurrentReso   = 1.5f;    // live resonance (for LFO filter mod)
static float        mod2LfoRate    = 2.0f;       // Hz
static float        mod2LfoDepth   = 0.5f;       // semitones (pitch) or 500Hz (filter)
static uint8_t      mod2LfoMode    = 0;          // index into mod2LfoMode* tables (0=Off)
static bool         mod2PotNeedsSync = false;    // force pot re-read after algo change
static Mod2PlayMode mod2PlayMode   = MOD2_POLY;
static uint8_t      mod2LastNote   = 0;          // mono/slide: currently playing note
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
#define T303_NAP_WAVE   200  // NAP: PULSE fixed 2% duty (nasal spike) + P2=symmetric wavefold
#define T303_TRI2_WAVE  201  // TRI + asymmetric fold (fold peaks, retain bass)
#define T303_SAW2_WAVE  202  // SAW_DOWN + symmetric wavefold
#define T303_SAW3_WAVE  203  // SAW_DOWN + AMY feedback (progressive harmonics)
#define T303_SQ2_WAVE   204  // SINE + AMY feedback (pure→complex via FM)
#define T303_SWF_WAVE   205  // SAW + sous-octaves : base f, f-1 (P2 0→50%), f-2 (P2 50→100%)
#define T303_SQF_WAVE   206  // SQR + sous-octaves identiques
#define T303_SNF_WAVE   207  // SIN + sous-octaves identiques
#define T303_PINK_WAVE  208  // NOISE rose : passe par le filtre résonant 303
// Vrai si la vague utilise les sous-canaux de sous-octaves (blend via P2)
static inline bool t303IsSubOctWave(uint8_t w) {
    return w==T303_SWF_WAVE || w==T303_SQF_WAVE || w==T303_SNF_WAVE;
}

// Calcule `len` échantillons normalisés [-1,1] représentant 3 cycles de la vague courante.
// P2 contrôle le fold/duty selon le type de vague.
static void t303FillWaveform(uint8_t wave, float p2, float* out, int len) {
    float maxAbs = 1e-4f;
    // SxF blend coefficients (computed once)
    float sxf_a = (p2<0.5f) ? p2*2.0f : 1.0f;
    float sxf_b = (p2<0.5f) ? 0.0f : (p2-0.5f)*2.0f;
    for (int i = 0; i < len; i++) {
        float phase = fmodf((float)i * 3.0f / (float)(len - 1), 1.0f);
        float yv;
        if (wave==PULSE) {
            yv = (phase < 0.5f - p2*0.48f) ? 1.0f : -1.0f;
        } else if (wave==T303_NAP_WAVE) {
            // Métaphore visuelle : duty 7→50% représente l'enrichissement harmonique du fold
            yv = (phase < 0.07f + p2*0.43f) ? 1.0f : -1.0f;
        } else if (wave==T303_PINK_WAVE) {
            // Pseudo-bruit déterministe : somme de sinusoïdes inharmoniques
            yv = sinf(i*0.379f)*0.5f + sinf(i*0.923f)*0.3f + sinf(i*2.17f)*0.15f + sinf(i*5.7f)*0.05f;
        } else if (t303IsSubOctWave(wave)) {
            // Signal composite : base (3 cycles) + f-1 (1.5 cycles) + f-2 (0.75 cycles)
            float ph1 = fmodf((float)i * 1.5f  / (float)(len - 1), 1.0f);
            float ph2 = fmodf((float)i * 0.75f / (float)(len - 1), 1.0f);
            float bf, bf1, bf2;
            if (wave==T303_SNF_WAVE) {
                bf  = sinf(phase * 6.28318f);
                bf1 = sinf(ph1   * 6.28318f);
                bf2 = sinf(ph2   * 6.28318f);
            } else if (wave==T303_SQF_WAVE) {
                bf  = (phase < 0.5f) ? 1.0f : -1.0f;
                bf1 = (ph1   < 0.5f) ? 1.0f : -1.0f;
                bf2 = (ph2   < 0.5f) ? 1.0f : -1.0f;
            } else {  // SWF (SAW)
                bf  = 1.0f - 2.0f*phase;
                bf1 = 1.0f - 2.0f*ph1;
                bf2 = 1.0f - 2.0f*ph2;
            }
            yv = (bf + sxf_a*bf1 + sxf_b*bf2) / (1.0f + sxf_a + sxf_b);
        } else {
            float base;
            if      (wave==TRIANGLE || wave==T303_TRI2_WAVE) base = (phase<0.5f)?(4*phase-1):(3-4*phase);
            else if (wave==T303_SQ2_WAVE)                    base = sinf(phase * 6.28318f);
            else if (wave==T303_SAW3_WAVE)                   base = tanhf(8.0f * sinf(phase * 6.28318f));
            else                                             base = 1.0f - 2.0f*phase;  // SAW
            float g = (p2<0.01f)?1.0f:powf(32.0f,p2);
            if (wave==T303_TRI2_WAVE || wave==T303_SAW2_WAVE) {
                if (base<0.0f) { yv=base; }
                else { float s=base*g, ph=fmodf(s+1.0f,4.0f); if(ph<0)ph+=4.0f; yv=(ph<2.0f)?(ph-1.0f):(3.0f-ph); }
            } else {
                float s=base*g, ph=fmodf(s+1.0f,4.0f); if(ph<0)ph+=4.0f;
                yv = (ph<2.0f)?(ph-1.0f):(3.0f-ph);
            }
        }
        out[i] = yv;
        if (fabsf(yv) > maxAbs) maxAbs = fabsf(yv);
    }
    float inv = 1.0f / maxAbs;
    for (int i = 0; i < len; i++) out[i] *= inv;
}

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
static float         lpGestPots[4]   = {-1.f,-1.f,-1.f,-1.f};  // pickup for GEST P4-P7 volumes
static bool          gestDrillDown   = false;  // true = entered sequencer via GEST double-click
static bool          gestDrillReturn = false;  // true = returning to GEST via joystick click
// Soft-takeover for 303 pots on drill-in from GEST.
// Locked until physical pot reaches stored param's position; then absolute mode (full range).
static bool  lp303DrillDelta   [4] = {};    // true = locked (pot not yet caught up)
static float lp303DrillRef     [4] = {};    // stored param's equivalent pot position
// Wavefold trackers as globals so switchMode can pre-arm them and prevent spurious first-frame fire.
static float g_lp303swf  = -1.0f;   // last p2 applied in MODE_303S
static float g_lp303s2wf = -1.0f;   // last p2 applied in MODE_303S2
static volatile AppMode gestDrillTarget = MODE_COUNT;  // pending switchMode from audioHandlerTask

// Returns the AMY wave constant for any t303Wave value (sentinels map to their base wave).
static inline uint8_t t303AmyWave(uint8_t w) {
    switch (w) {
        case T303_NAP_WAVE:  return PULSE;
        case T303_TRI2_WAVE: return TRIANGLE;
        case T303_SAW2_WAVE: return SAW_DOWN;   // SW3: SAW + asymmetric fold
        case T303_SAW3_WAVE: return PULSE;       // SQ2: PULSE + symmetric fold
        case T303_SQ2_WAVE:  return SINE;        // SIN: SINE + fold
        case T303_SWF_WAVE:  return SAW_DOWN;    // SAW + sous-octaves
        case T303_SQF_WAVE:  return PULSE;       // SQR + sous-octaves
        case T303_SNF_WAVE:  return SINE;        // SIN + sous-octaves
        case T303_PINK_WAVE: return NOISE;       // bruit rose
        default:             return w;
    }
}
// Returns a short display name for any t303Wave value.
static inline const char* t303WaveName(uint8_t w) {
    switch (w) {
        case TRIANGLE:       return "TRI";
        case T303_TRI2_WAVE: return "TRI2";
        case SAW_DOWN:       return "SAW";   // SAW + symmetric fold
        case T303_SAW2_WAVE: return "SW3";   // SAW + asymmetric fold
        case PULSE:          return "SQR";   // duty sweep
        case T303_SAW3_WAVE: return "SQ2";   // PULSE + symmetric fold
        case T303_SQ2_WAVE:  return "SIN";   // SINE + fold
        case T303_NAP_WAVE:  return "NAP";
        case T303_SWF_WAVE:  return "SWF";   // SAW + sous-octaves
        case T303_SQF_WAVE:  return "SQF";   // SQR + sous-octaves
        case T303_SNF_WAVE:  return "SNF";   // SIN + sous-octaves
        case T303_PINK_WAVE: return "PINK";  // bruit rose
        default:             return "---";
    }
}
// Returns what pot 2 ("P2") actually controls for the current wave — mirrors the
// per-wave-type branches in the P2 pot-handling code (MODE_303S/303S2/I303/GEST2).
static inline const char* t303P2Label(uint8_t w) {
    if (t303IsSubOctWave(w))                        return "Blend";
    if (w == T303_PINK_WAVE)                        return "--";
    if (w == PULSE)                                 return "Duty";
    if (w == T303_NAP_WAVE)                         return "Width";
    if (w == T303_TRI2_WAVE || w == T303_SAW2_WAVE) return "AFold";
    return "Fold";
}

// Apply t303Wave audio state after t303Wave has been updated. prevWave = old value before change.
static void t303ApplyWave(uint8_t prevWave);

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
static uint8_t       drum2View       = 0;  // 0=pad, 1=seq, 2=anim
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

// ==================== DR2 STATE (hierarchical 4.4.4 drum sequencer) ====================
// Same 8 pads/sounds as DRUM2 (kDrum2Remap, drum2Pitch/Decay/Volume, playDrum) but a
// different pattern layout: 64 steps addressed as beat(0-3).step(0-3).micro(0-3) instead
// of a flat 0-63 index. Lets the 4×8 physical grid navigate the full 64-slot resolution via
// 3 stacked 4-key rows (beat / step / micro) rather than needing 64 keys directly.
// Musically: beat = quarter note, step = normal 16th-note (this is what BPM is tuned to,
// same rate as DRUM2's 16 steps), micro = 64th-note subdivision of one step. All 64 slots
// are independently audible — it's a real 64-step sequencer, just browsed hierarchically.
#define DR2H_BEATS    4
#define DR2H_STEPS    4
#define DR2H_MICROS   4
static uint8_t       dr2Vel[DRUM2_PADS][DR2H_BEATS][DR2H_STEPS][DR2H_MICROS] = {};  // 0=off, else velocity 1-127
// Per-hit alteration, same 4 modes as DRUM2 (drum2SeqMod): 0=NRM 1=50% 2=random-pitch 3=double.
static uint8_t       dr2Mod[DRUM2_PADS][DR2H_BEATS][DR2H_STEPS][DR2H_MICROS] = {};
static uint8_t        dr2PlaceMod = 0;   // mode stamped onto newly-placed hits — set via the pad zone
static int8_t         dr2SelPad   = 0;
static uint8_t        dr2SelBeat  = 0;   // which beat's steps are shown in the step row
static uint8_t        dr2SelStep  = 0;   // which step's micros are shown in the micro row
static bool           dr2Playing  = false;
static uint8_t        dr2PlayBeat = 0, dr2PlayStep = 0, dr2PlayMicro = 0;
static unsigned long  dr2LastMicroMs = 0;
static unsigned long  dr2PadFlashMs[DRUM2_PADS] = {};
static unsigned long  dr2DblPendingAt[DRUM2_PADS] = {};  // scheduled "double hit" timestamp (mod==3)
static uint8_t        dr2DblPitch[DRUM2_PADS]     = {};
// Multi-select masks (bit i = beat/step i included in a batch edit). Both default to
// a single bit (just the current beat/step) — the clock itself is unaffected either
// way (confirmed correct at any mask value), but a single-select default keeps a
// freshly placed hit from repeating across all 4 beats/steps at once, which reads as
// much denser/faster even though the tempo hasn't changed. Row0/row1 only ever toggle
// between "just this one" and "all four" (see the key handler) — no in-between
// combination is reachable — so pressing the focused beat/step again is the quick way
// to switch to "all".
static uint8_t        dr2BeatSelMask = 0x1;
static uint8_t        dr2StepSelMask = 0x1;

// ---- DR2 patterns (4 slots, GEST-like copy/paste/LIVE-LOOP; self-contained, not part of
// the MODE_GEST pattern manager) — selected via the right zone's row3 (previously unused).
// dr2Vel/dr2Mod above are the "live" buffer being edited/played; a pattern slot's stored
// content is swapped in/out of it when switching slots, same pattern as DRUM2<->GEST.
#define DR2_PATS 4
static uint8_t  dr2Pats   [DR2_PATS][DRUM2_PADS][DR2H_BEATS][DR2H_STEPS][DR2H_MICROS] = {};
static uint8_t  dr2ModPats[DR2_PATS][DRUM2_PADS][DR2H_BEATS][DR2H_STEPS][DR2H_MICROS] = {};
static bool     dr2PatFilled[DR2_PATS] = {};
static uint8_t  dr2ActivePat = 0;      // pattern slot currently loaded into dr2Vel/dr2Mod
static bool     dr2Copied    = false;  // B4 clipboard armed
static uint8_t  dr2CopyPat   = 0;      // clipboard source slot
enum { DR2_LOOP=0, DR2_LIVE=1 };
static uint8_t  dr2PlayMode  = DR2_LOOP;  // B3 toggles: LOOP=repeat active pattern, LIVE=auto-advance to next filled pattern each bar
// B1 double-click arms play+record (quantized live-record, same idea as DRUM2's
// drum2RecArmed): whatever pad/note you hit while dr2Playing gets written into the
// live buffer at the nearest beat.step.micro slot. B1 single-click while armed just
// disarms recording (keeps playing); otherwise B1 single-click is the normal play toggle.
static bool     dr2RecArmed  = false;

// ---- GEST2: selectable instrument slots (right-half row0), layered on top of the
// same shared beat.step.micro clock — all slots sound simultaneously every tick,
// like GEST's own rows; "instrument select" only changes which grid you're
// viewing/editing (dr2Instrument), it does not solo/mute the others.
enum { DR2_INSTR_DRUMS=0, DR2_INSTR_SYNTH, DR2_INSTR_T303, DR2_INSTR_TBD, DR2_INSTR_COUNT };
static uint8_t  dr2Instrument = DR2_INSTR_DRUMS;
static uint8_t  dr2SelNote    = 0;   // 0-11: selected key in the melodic 12-note grid (mirrors dr2SelPad)
#define DR2_NOTES 12
static uint8_t  dr2SynthVel[DR2_NOTES][DR2H_BEATS][DR2H_STEPS][DR2H_MICROS] = {};  // live, no alteration (melodic = plain hit)
static uint8_t  dr2T303Vel [DR2_NOTES][DR2H_BEATS][DR2H_STEPS][DR2H_MICROS] = {};  // live
static uint8_t  dr2SynthPats[DR2_PATS][DR2_NOTES][DR2H_BEATS][DR2H_STEPS][DR2H_MICROS] = {};
static uint8_t  dr2T303Pats [DR2_PATS][DR2_NOTES][DR2H_BEATS][DR2H_STEPS][DR2H_MICROS] = {};
static uint8_t  dr2T303CurNote = 0;      // currently-sounding 303 note (0 = none), for monophonic retrigger
// The T303 track is a single monophonic voice (like the real hardware) but
// dr2T303Vel[] is indexed by note, so nothing previously stopped two different
// notes from both being marked "on" at the same beat/step/micro — the data would
// silently hold both, and only the lowest note index ever actually played back
// (see dr2T303CurNote / the playback loop that does "if (...vel...) { t303Note=n;
// break; }"), so placing a second note on a step you'd already placed one on
// looked like nothing happened rather than replacing it. Call this whenever a
// T303 note is turned ON at a given slot to enforce one-note-per-step for real,
// the same way editing already assumes.
static void dr2T303ClearOtherNotes(uint8_t beat, uint8_t step, uint8_t micro, uint8_t exceptNote) {
    for (uint8_t n = 0; n < DR2_NOTES; n++) {
        if (n != exceptNote) dr2T303Vel[n][beat][step][micro] = 0;
    }
}
static bool     dr2T303Inited  = false;  // audioT303Init() called once on first selecting the 303 slot
static unsigned long dr2NoteFlashMs[DR2_NOTES] = {};  // LED "just hit" flash, mirrors dr2PadFlashMs for the melodic grid
// Unlike drums (one-shot PCM samples that end on their own) the generic synth
// engine's audioNoteOn holds a note until an explicit audioNoteOff — so every
// synth/303 trigger (sequenced or previewed) schedules its own auto-release
// this many ms later, checked unconditionally each tick alongside dr2DblPendingAt.
#define DR2_NOTE_DUR_MS 150
static unsigned long dr2SynthNoteOffAt[DR2_NOTES] = {};  // 0 = none pending, per-note (polyphonic)
static uint8_t  dr2SynthPlayedNote[DR2_NOTES] = {};  // actual MIDI note last triggered per grid index —
                                                      // stored so a later note-off targets the right pitch
                                                      // even if scale/octave changed while it was ringing.
static unsigned long dr2T303NoteOffAt  = 0;               // 0 = none pending (monophonic, one note at a time)
// While touching a param pot for the currently-focused instrument — 303 (joystick
// wave/octave, P2 wavefold, P4-7 Reso/EnvMod/Dur/Cutoff), Drums (P4-6 Pitch/
// Decay/Volume), or Synth (P4 Volume) — the big beat.step OLED readout switches
// to a small popup showing those params instead, for this many ms after the
// last touch, then reverts on its own ("un fonctionnement de pop-up"). Shared by all instruments
// since only one can be focused (and so poppable) at a time.
#define DR2_POPUP_MS 1200
static unsigned long dr2PopupUntil = 0;
static float dr2SynthVolume = 1.0f;  // 0-2x, pot-controlled (see the P4 handler) — same
                                      // headroom convention as Drums' Vol pot.

// Grid index -> MIDI note goes through the same NoteMap scale/octave settings
// the rest of the app uses (opened via B3 double-click, OVERLAY_SCALE_ARP —
// "similaire au menu de synth") rather than a fixed chromatic offset, so
// adjusting octave/scale there actually changes what GEST2's Synth/303 play.
static void dr2TriggerSynth(uint8_t noteIdx, float vel) {
    uint8_t note = noteMap.getMidiNoteByIdx(noteIdx);
    audioNoteOn(note, constrain(vel * dr2SynthVolume, 0.0f, 2.0f));
    dr2SynthPlayedNote[noteIdx] = note;
    dr2SynthNoteOffAt[noteIdx] = millis() + DR2_NOTE_DUR_MS;
}
static void dr2TriggerT303(uint8_t noteIdx, float vel) {
    if (!dr2T303Inited) { audioT303Init(2000.0f, 1.5f, 2.0f, 200.0f, 0); dr2T303Inited = true; }
    if (dr2T303CurNote) audioT303NoteOff(dr2T303CurNote);
    // t303Oct is the same shared octave-shift MODE_303S's joystick Y controls
    // (see the currentMode==MODE_DR2 joystick block) — layered on top of the
    // NoteMap-derived base note, letting the 303 slot reach beyond one octave.
    uint8_t note = (uint8_t)constrain((int)noteMap.getMidiNoteByIdx(noteIdx) + (int)t303Oct*12, 0, 127);
    audioT303NoteOn(note, vel);
    dr2T303CurNote = note;
    dr2T303NoteOffAt = millis() + DR2_NOTE_DUR_MS;
}

// Recomputes dr2PatFilled[dr2ActivePat] from the live buffers (all instruments) —
// call after any edit that might empty or fill the active pattern (placing/
// clearing a hit, clearing a whole instrument, switching slots).
static void dr2RecomputeActivePatFilled() {
    bool hasHits=false;
    for(uint8_t p=0;p<DRUM2_PADS&&!hasHits;p++) for(uint8_t b=0;b<DR2H_BEATS&&!hasHits;b++) for(uint8_t s=0;s<DR2H_STEPS&&!hasHits;s++) for(uint8_t m=0;m<DR2H_MICROS;m++) if(dr2Vel[p][b][s][m]){hasHits=true;break;}
    for(uint8_t n=0;n<DR2_NOTES&&!hasHits;n++) for(uint8_t b=0;b<DR2H_BEATS&&!hasHits;b++) for(uint8_t s=0;s<DR2H_STEPS&&!hasHits;s++) for(uint8_t m=0;m<DR2H_MICROS;m++) if(dr2SynthVel[n][b][s][m]||dr2T303Vel[n][b][s][m]){hasHits=true;break;}
    dr2PatFilled[dr2ActivePat] = hasHits;
}

// Quantizes "now" to the nearest micro-slot (current or the next one, with proper
// beat/step/micro carry) for GEST2's B1-double-click live-record feature — same
// shape as DRUM2's own nearStep quantization (main.cpp, "elapsed > stepMs/2").
static void dr2RecordNearestSlot(uint8_t* outBeat, uint8_t* outStep, uint8_t* outMicro) {
    unsigned long microMs = (unsigned long)fmaxf(5.0f, 60000.0f / (float)bpm / 4.0f);
    unsigned long elapsed = millis() - dr2LastMicroMs;
    uint8_t b=dr2PlayBeat, s=dr2PlayStep, m=dr2PlayMicro;
    if (elapsed > microMs/2) {
        m++;
        if (m >= DR2H_MICROS) { m=0; s++; if (s>=DR2H_STEPS) { s=0; b++; if (b>=DR2H_BEATS) b=0; } }
    }
    *outBeat=b; *outStep=s; *outMicro=m;
}

// ==================== SYSEQ STATE ====================
#define SYSEQ_STEPS 16
#define SYSEQ_CHORD 16   // max notes per step
static bool     syseqSeqView   = false;
static uint8_t  syseqSelStep   = 0;
static uint8_t  syseqSelNote   = 0;   // note-first: remembered selected note (0=none)
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

// ==================== 303S2 STATE ====================
#define S303S2_LEN 16
static uint8_t  s303s2Seq[S303S2_LEN] = {};  // ring buffer: 0=rest, MIDI+1=note
static uint8_t  s303s2Head    = 0;            // next write position
static uint8_t  s303s2Count   = 0;            // filled slots (0..S303S2_LEN)
static uint8_t  s303s2Step    = 0;            // playback position (0..count-1)
static uint32_t s303s2LastMs  = 0;
static uint8_t  s303s2CurNote = 0;            // currently sounding MIDI note (0=none)

// ==================== GEST STATE ====================
#define GEST_PATS 8
#define GEST_NSEQ 4  // 0=DRUMS 1=303S 2=SYNS 3=SAMPS
struct GestDrumPat  { uint8_t vel[DRUM2_PADS][DR2_STEPS]; uint8_t row[DRUM2_PADS][DR2_STEPS];
                      uint8_t mod[DRUM2_PADS][DR2_STEPS]; int8_t  pit[DRUM2_PADS][DR2_STEPS]; };
struct Gest303Pat   { uint8_t seq[S303S2_LEN]; uint8_t count; uint8_t head; };
struct GestSynsPat  { uint8_t notes[SYSEQ_STEPS][SYSEQ_CHORD]; uint8_t vels[SYSEQ_STEPS][SYSEQ_CHORD];
                      uint8_t alt[SYSEQ_STEPS]; };
struct GestSampsPat { uint16_t note[SS2_SLOTS]; uint8_t alt[SS2_SLOTS]; };
static EXT_RAM_ATTR GestDrumPat  g_drumPats [GEST_PATS];
static EXT_RAM_ATTR Gest303Pat   g_303sPats [GEST_PATS];
static EXT_RAM_ATTR GestSynsPat  g_synsPats [GEST_PATS];
static EXT_RAM_ATTR GestSampsPat g_sampsPats[GEST_PATS];
static bool     g_patFilled[GEST_NSEQ][GEST_PATS] = {};
static uint8_t  gestActPat [GEST_NSEQ] = {};   // currently playing pattern per sequencer
static uint8_t  gestSelRow = 0;
static uint8_t  gestSelCol = 0;
static float    gestSeqVol [GEST_NSEQ] = {1.f,1.f,1.f,1.f};
enum GestPlayMode : uint8_t { GEST_LOOP=0, GEST_LIVE };
static GestPlayMode gestPlayMode = GEST_LOOP;
static bool    gestCopied  = false;
static uint8_t gestCopySeq = 0;
static uint8_t gestCopyPat = 0;

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

// ==================== ANIM STATE (MODE_ANIM) ====================
static uint8_t animIdx  = 0;  // active animation: 0=WAVE 1=BARS 2=TECHNO 3=ACID 4=8BIT

// ==================== LANIM STATE (MODE_LANIM) ====================
static uint8_t lanimIdx = 0;  // 0=FLASH 1=RBOW 2=CHSE 3=NOIS 4=ORGA

// ==================== PCMCLEAN STATE (MODE_PCMCLEAN) ====================
// 0=confirm screen  1=running  2=done
static uint8_t  pcmCleanPhase   = 0;
static uint32_t pcmCleanDeleted = 0;   // files deleted this run
static uint32_t pcmCleanScanned = 0;   // files scanned this run
static bool     pcmCleanRunning = false;

// ==================== IMPORT STATE (MODE_IMPORT, Android only) ====================
// SAF folder picker + import copy. B1 triggers the Java picker (GrvActivity.
// pickImportFolder(), via the one-off JNI bridge below); progress comes back not
// via a second JNI direction but by polling a tiny status file the Java import
// thread writes into the SD root — the same dir SD.h resolves to on Android
// (see simulator/hal/SD.h's simSdRoot() Android branch) — so this reuses the
// existing SD.open() file I/O instead of adding a native<-Java callback.
// 0=prompt/idle  1=running  2=done. On ESP32/simulator this mode just shows an
// "Android only" message; B1 is a no-op there (androidPickFolder() doesn't exist).
static uint8_t  importPhase = 0;
static uint32_t importDone  = 0;
static uint32_t importTotal = 0;

#ifdef __ANDROID__
#include <SDL2/SDL.h>
#include <jni.h>
static void androidPickFolder() {
    JNIEnv* env = (JNIEnv*)SDL_AndroidGetJNIEnv();
    jobject activity = (jobject)SDL_AndroidGetActivity();
    if (!env || !activity) return;
    jclass cls = env->GetObjectClass(activity);
    jmethodID mid = env->GetMethodID(cls, "pickImportFolder", "()V");
    if (mid) env->CallVoidMethod(activity, mid);
    env->DeleteLocalRef(cls);
    env->DeleteLocalRef(activity);
}
#endif

// ==================== STONE STATE (MODE_STONE) ====================
// Joystick Y browses the SD card (shared sdFiles[]/sdCursor/sdPath); joystick click loads
// the selected file. P2 auto-cycles through the audio files in the currently browsed folder.
static String   stoneLoadedPath;             // full path of the currently loaded sample ("" = none)
static float    lp_stoneP2      = -1.0f;     // last P2 pot value applied (pickup, avoid jump on entry)
static uint8_t  stoneAudioIdx[32];           // indices into sdFiles[] that are audio files (P2 cycle list)
static uint8_t  stoneAudioCount = 0;

// Sample window (start/end/position/size), granular2-style — visualizes the loaded
// sample and lets P4-P7 trim which portion of it actually plays. See
// audioComputeStoneWaveform()/audioStoneApplyWindow() (audio_engine.cpp).
struct StoneWinState {
    bool    computed = false;   // waveform computed for the currently loaded sample
    uint8_t waveform[128] = {};
    float   start = 0.0f;
    float   end   = 1.0f;
};
static StoneWinState stoneWin;
// Loop mode: double-click B3 toggles. When on, held notes loop within [start,end]
// instead of playing once through — see audioStoneSetLoopMode() (audio_engine.cpp).
static bool stoneLoopMode = false;

// ==================== POKEMON MODE ====================
enum PokemonType : uint8_t {
    PKMN_NORMAL=0, PKMN_FIRE, PKMN_WATER, PKMN_GRASS,
    PKMN_ELECTRIC, PKMN_ICE, PKMN_FIGHTING, PKMN_POISON,
    PKMN_PSYCHIC, PKMN_GHOST, PKMN_DRAGON,
    PKMN_ROCK, PKMN_GROUND, PKMN_BUG, PKMN_FLYING,
    PKMN_TYPE_COUNT
};
static const char* kPkmnTypeNames[] = {
    "NORMAL","FIRE","WATER","GRASS","ELECTRIC","ICE",
    "FIGHTING","POISON","PSYCHIC","GHOST","DRAGON",
    "ROCK","GROUND","BUG","FLYING"
};
struct PokemonDef {
    const char*  name;
    uint16_t     number;
    PokemonType  type;
    SynthShape   wave;
    EnvParams    env;       // {atk(ms), dec(ms), rel(ms), sus(0-1)}
    float        filterHz;  // LPF cutoff (0=open)
    float        filterRes; // resonance
    float        wavefold;  // 1.0=dry, >1=saturation/drive
    uint8_t      pal[4];    // LED palette: 4 HSV hues (sat/val set by LED code)
    float        gain;      // volume normalisation (1.0=reference, <1=quieter)
};
#define PKMN_COUNT 42
static const PokemonDef kPokemon[PKMN_COUNT] = {
    // name         #    type           wave                  atk  dec   rel    sus    fHz    fRes  wfold  pal[4]={h0,h1,h2,h3}  gain
    // ---- NORMAL ----
    {"CLEFAIRY",   35, PKMN_NORMAL,   SHAPE_DX7_BELLS,    { 15, 600,1000, 0.35f}, 5000.f, 1.5f, 1.0f, {218,208,228,235}, 1.00f},
    {"JIGGLYPUFF", 39, PKMN_NORMAL,   SHAPE_SINE,         { 25, 700,1500, 0.70f}, 4000.f, 1.2f, 1.0f, {215,225,205,230}, 1.00f},
    {"MEOWTH",     52, PKMN_NORMAL,   SHAPE_PLUCK,        {  3, 160, 250, 0.15f}, 2800.f, 2.2f, 1.0f, {35, 45, 25, 50 }, 1.00f},
    {"EEVEE",     133, PKMN_NORMAL,   SHAPE_JUNO_PIANO,   {  5, 220, 380, 0.40f}, 3000.f, 1.8f, 1.0f, {20, 30, 15, 40 }, 1.00f},
    {"SNORLAX",   143, PKMN_NORMAL,   SHAPE_BASS,         { 15, 700, 900, 0.45f}, 280.f,  1.5f, 1.8f, {140,150,130,155}, 0.43f},
    // ---- FIRE ----
    {"CHARMANDER",  4, PKMN_FIRE,     SHAPE_SAW,          {  4, 100, 180, 0.12f}, 3200.f, 3.5f, 1.8f, {15,  5, 25,  0 }, 0.33f},
    {"VULPIX",     37, PKMN_FIRE,     SHAPE_SAW_FM,       {  6, 200, 350, 0.30f}, 2400.f, 2.8f, 1.2f, {20, 10, 30,  5 }, 0.64f},
    {"NINETALES",  38, PKMN_FIRE,     SHAPE_JUNO_STRINGS, { 30, 500, 900, 0.55f}, 3000.f, 2.0f, 1.0f, {40, 30, 50, 25 }, 1.00f},
    {"FLAREON",   136, PKMN_FIRE,     SHAPE_SAW,          {  4, 130, 280, 0.22f}, 2800.f, 3.8f, 3.0f, { 5,  0, 10, 15 }, 0.15f},
    {"MAGMAR",    126, PKMN_FIRE,     SHAPE_SAW_FM,       {  5, 200, 300, 0.25f}, 2200.f, 3.5f, 2.0f, { 0,  5, 10,245 }, 0.29f},
    {"MOLTRES",   146, PKMN_FIRE,     SHAPE_SAW_FM,       {  7, 350, 500, 0.30f}, 2800.f, 4.0f, 2.5f, {25, 15, 35, 10 }, 0.17f},
    // ---- WATER ----
    {"SQUIRTLE",    7, PKMN_WATER,    SHAPE_SINE,         { 12, 300, 600, 0.55f}, 2200.f, 2.2f, 1.0f, {160,150,170,140}, 1.00f},
    {"PSYDUCK",    54, PKMN_WATER,    SHAPE_SAW_FM,       {  8, 180, 120, 0.00f}, 900.f,  5.5f, 1.5f, {45, 35, 55, 40 }, 0.26f},
    {"GYARADOS",  130, PKMN_WATER,    SHAPE_SAW,          {  3, 250, 200, 0.00f}, 420.f,  4.5f, 4.0f, {160,  0,170, 10}, 0.07f},
    {"VAPOREON",  134, PKMN_WATER,    SHAPE_JUNO_STRINGS, { 35, 500,1200, 0.60f}, 1400.f, 2.2f, 1.0f, {140,130,150,120}, 1.00f},
    {"LAPRAS",    131, PKMN_WATER,    SHAPE_JUNO_CHOIR,   { 40, 600,1400, 0.65f}, 1800.f, 2.0f, 1.0f, {145,135,155,150}, 1.00f},
    // ---- GRASS ----
    {"BULBASAUR",   1, PKMN_GRASS,    SHAPE_JUNO_STRINGS, { 80, 400,1000, 0.55f}, 1000.f, 2.5f, 1.0f, {90, 80,100, 70 }, 1.00f},
    // ---- ELECTRIC ----
    {"PIKACHU",    25, PKMN_ELECTRIC, SHAPE_SQUARE,       {  1,  50,  80, 0.00f}, 5000.f, 4.5f, 1.0f, {50, 40, 60, 45 }, 0.44f},
    {"MAGNEMITE",  81, PKMN_ELECTRIC, SHAPE_SAW_FM,       {  3, 200, 350, 0.20f}, 3500.f, 3.5f, 1.0f, {155,145,165,135}, 0.64f},
    {"JOLTEON",   135, PKMN_ELECTRIC, SHAPE_SQUARE,       {  1,  40,  60, 0.00f}, 5500.f, 5.0f, 1.0f, {55, 45, 65, 50 }, 0.44f},
    {"ELECTABUZZ",125, PKMN_ELECTRIC, SHAPE_SQUARE,       {  2,  80, 120, 0.05f}, 4800.f, 4.5f, 1.5f, {50, 45, 55, 40 }, 0.30f},
    {"ZAPDOS",    145, PKMN_ELECTRIC, SHAPE_SQUARE,       {  2,  90, 130, 0.08f}, 4500.f, 5.5f, 3.0f, {52, 42, 62, 47 }, 0.08f},
    // ---- ICE ----
    {"ARTICUNO",  144, PKMN_ICE,      SHAPE_DX7_BELLS,    { 22, 800,1200, 0.30f}, 5500.f, 2.5f, 1.0f, {148,138,158,143}, 1.00f},
    // ---- FIGHTING ----
    {"MACHOP",     66, PKMN_FIGHTING, SHAPE_SAW,          {  2,  55,  70, 0.00f}, 550.f,  2.0f, 2.5f, {150,140,160,130}, 0.24f},
    {"PRIMEAPE",   57, PKMN_FIGHTING, SHAPE_SAW,          {  1,  60,  80, 0.00f}, 700.f,  3.0f, 3.0f, {10,  5, 15,  0 }, 0.15f},
    // ---- POISON ----
    {"ARBOK",      24, PKMN_POISON,   SHAPE_SAW_FM,       {  5, 300, 400, 0.15f}, 800.f,  4.0f, 1.5f, {190, 90,200, 80}, 0.34f},
    {"NIDOKING",   34, PKMN_POISON,   SHAPE_SAW,          {  3, 200, 300, 0.15f}, 1200.f, 3.0f, 2.5f, {200,190,210,185}, 0.20f},
    // ---- PSYCHIC ----
    {"ALAKAZAM",   65, PKMN_PSYCHIC,  SHAPE_DX7_EP,       {  5, 400, 700, 0.40f}, 4500.f, 2.5f, 1.0f, {45, 35, 55, 40 }, 1.00f},
    {"MEWTWO",    150, PKMN_PSYCHIC,  SHAPE_SAW_FM,       {  5, 800,1400, 0.50f}, 650.f,  4.0f, 1.0f, {195,185,205,210}, 0.51f},
    {"MEW",       151, PKMN_PSYCHIC,  SHAPE_SINE,         { 22, 700,1500, 0.65f}, 6000.f, 1.8f, 1.0f, {220,210,230,215}, 1.00f},
    // ---- GHOST ----
    {"HAUNTER",    93, PKMN_GHOST,    SHAPE_SAW_FM,       { 12, 500, 700, 0.15f}, 400.f,  3.5f, 1.0f, {198,188,208,215}, 0.64f},
    {"GENGAR",     94, PKMN_GHOST,    SHAPE_SAW_FM,       { 18, 600, 900, 0.20f}, 350.f,  4.0f, 1.0f, {200,190,212,185}, 0.51f},
    // ---- DRAGON ----
    {"DRATINI",   147, PKMN_DRAGON,   SHAPE_SINE,         { 18, 500, 900, 0.55f}, 4000.f, 2.2f, 1.0f, {165,155,175,145}, 1.00f},
    {"DRAGONAIR", 148, PKMN_DRAGON,   SHAPE_SUPERSAW,     { 20, 500, 900, 0.20f}, 3200.f, 2.0f, 1.0f, {168,158,178,148}, 0.40f},
    {"DRAGONITE", 149, PKMN_DRAGON,   SHAPE_SUPERSAW,     {  8, 280, 600, 0.35f}, 2200.f, 2.0f, 1.0f, {28, 18, 38, 90 }, 0.40f},
    // ---- ROCK ----
    {"GEODUDE",    74, PKMN_ROCK,     SHAPE_BASS,         {  2,  80, 100, 0.00f}, 350.f,  2.5f, 3.0f, {22, 18, 28, 25 }, 0.19f},
    {"ONIX",       95, PKMN_ROCK,     SHAPE_SAW,          {  5, 400, 500, 0.10f}, 250.f,  3.0f, 2.5f, {25, 20, 30, 15 }, 0.20f},
    {"AERODACTYL",142, PKMN_ROCK,     SHAPE_SAW_FM,       {  3, 300, 400, 0.20f}, 2500.f, 4.0f, 2.0f, {192,182,202,175}, 0.23f},
    // ---- GROUND ----
    {"DIGLETT",    50, PKMN_GROUND,   SHAPE_BASS,         {  1,  60,  80, 0.00f}, 300.f,  1.5f, 1.5f, {20, 15, 25, 30 }, 0.51f},
    {"RHYDON",    112, PKMN_GROUND,   SHAPE_SAW,          {  4, 300, 350, 0.10f}, 400.f,  3.5f, 3.5f, {18, 12, 25, 22 }, 0.11f},
    // ---- BUG ----
    {"BUTTERFREE", 12, PKMN_BUG,      SHAPE_SINE,         { 20, 400, 600, 0.45f}, 4500.f, 1.8f, 1.0f, {195,185,205,170}, 1.00f},
    {"SCYTHER",   123, PKMN_BUG,      SHAPE_SQUARE,       {  2,  80, 120, 0.05f}, 3500.f, 3.0f, 1.5f, {85, 75, 95,160 }, 0.37f},
};
static uint8_t pkmnSelected = 0;
static uint8_t pkmnBrType = 0;   // selected type in OVERLAY_PKMN browser
static uint8_t pkmnBrItem = 0;   // selected item within that type

static const uint8_t kI303WaveList[] = {
    TRIANGLE, T303_TRI2_WAVE, SAW_DOWN, T303_SAW2_WAVE, PULSE,
    T303_SAW3_WAVE, T303_SQ2_WAVE, T303_NAP_WAVE,
    T303_SWF_WAVE, T303_SQF_WAVE, T303_SNF_WAVE, T303_PINK_WAVE
};
static constexpr uint8_t kI303WaveCount = sizeof(kI303WaveList);
static uint8_t i303WaveBr = 0;   // browsed index in OVERLAY_I303_WAVE

static void pkmnApply() {
    if (!audioReady) return;
    const PokemonDef& p = kPokemon[pkmnSelected];
    audioAllNotesOff();
    audioSetShape(p.wave);
    audioSetEnvelope(p.env);
    audioSetFilter(p.filterHz, p.filterRes);
    audioSetWavefold(p.wavefold);
    audioSetVolume(volume * p.gain);
    Serial.printf("PKMN #%d %s wf=%.1f g=%.2f\n", p.number, p.name, p.wavefold, p.gain);
}

// ==================== EXP STATE (MODE_EXP) ====================
// Key grid: 8 columns × 4 rows. Each column = one modifier group (radio-select per row).
// Col4 = FX is multi-toggle (expFxMask bitmask). Col3 = gate-length preset — indexes the
// pre-existing kExpGateMs[] table below (SHT/MED/LNG/HLD), which was declared with a
// ready-made comment explaining its 4 values but was never actually read anywhere until
// this fix. Col7 is intentionally NOT written by key presses: its LED row is a live
// arp-speed meter driven by expPosX, not a row-selector, so giving it selectable rows
// would fight its own readout.
static uint8_t  expColSel[KBD_COLS] = {0,1,0,0,0,1,2,1}; // col3=0(SHT=80ms), closest default to the old always-25ms rate limit
static uint8_t  expFxMask   = 0;    // bit0=REV bit1=DLY bit2=CHR bit3=REP
static float    expPosX     = 0.5f; // physics position 0..1 (0=left, 1=right)
static float    expPosY     = 0.5f; // physics position 0..1 (0=top=high note, 1=bottom=low)
static float    expVelX     = 0.0f;
static float    expVelY     = 0.0f;
static bool     expNoteOn   = false;
static uint8_t  expCurNote  = 60;
static uint8_t  expArpStep  = 0;
static uint32_t expArpNextMs = 0;
static uint32_t expLastNoteMs = 0;  // rate-limit: min 25ms between note changes (continuous mode)
static float    expLastFilterHz = -1.0f;
static float    expLastFoldGain = 1.0f;
// Arp intervals: root, major-3rd, 5th, octave
static const uint8_t kExpArpIntvl[] = {0, 4, 7, 12};
// Gate durations in ms (col3 selection): SHT=80 MED=200 LNG=500 HLD=0
static const uint16_t kExpGateMs[] = {80, 200, 500, 0};

// Trail: ring buffer of recent dot positions with timestamps
#define EXP_TRAIL_LEN 28
#define EXP_TRAIL_MS  1400u  // trail lifetime in ms
struct ExpTrailPt { int16_t x, y; uint32_t t; };
static ExpTrailPt expTrail[EXP_TRAIL_LEN];
static uint8_t    expTrailHead  = 0;   // next write index
static uint8_t    expTrailCount = 0;   // valid entries (up to EXP_TRAIL_LEN)

// ==================== EXP2 STATE (MODE_EXP2) ====================
// PolyBounce: balls bounce inside a rotating polygon; each wall collision triggers a scale note.
// Joystick XY = direct gravity direction. Polygon auto-rotates (speed from pot P4).
// Keys: col0=Scale col1=Oct col2=Balls col3=Wave col4=Env col5=Bounce col6=Sides col7=FX
// Pots: P2=shape P4=rotation speed P5=speed cap P6=gravity strength P7=bounciness
#define EXP2_MAX_BALLS  4
#define EXP2_MAX_SIDES  8
#define EXP2_BALL_R     3.0f
struct Exp2Ball { float x, y, vx, vy; bool active; };
static Exp2Ball  exp2Balls[EXP2_MAX_BALLS];
static uint8_t   exp2BallCount  = 1;
static float     exp2HexAngle   = 0.0f;   // current polygon rotation angle (radians)
static float     exp2RotVel     = 0.003f; // angular velocity (set by pot, no longer by joystick)
static float     exp2GravAngle  = (float)M_PI/2.0f; // gravity direction — updated from joystick for display
static uint8_t   exp2Scale      = 0;      // 0=MAJ 1=MIN 2=PNT 3=CHR
static int8_t    exp2Octave     = 0;      // -2..+1
static float     exp2Bounce     = 0.85f;
static uint8_t   exp2NumSides   = 6;      // polygon sides 3-8
static float     exp2SpeedCap   = 6.0f;
static float     exp2GravStr    = 0.04f;
static uint8_t   exp2FxMask     = 0;      // bit0=REV bit1=DLY bit2=CHR bit3=WFD
static uint8_t   exp2Shape      = 0;      // 0=SAW 1=SQR 2=SIN 3=NOI
static uint8_t   exp2EnvIdx     = 0;      // 0=PLUCK 1=FAST 2=NRM 3=PAD
static uint8_t   exp2ColSel[8]  = {0,2,0,0,0,2,2,0}; // selected row per col (for LED display)
static uint32_t  exp2WallHitMs[EXP2_MAX_SIDES]   = {};
static bool      exp2WallFlash[EXP2_MAX_SIDES]   = {};
static uint32_t  exp2WallFlashMs[EXP2_MAX_SIDES] = {};
static uint8_t   exp2WallNote[EXP2_MAX_SIDES];     // MIDI note triggered per wall (0xFF=none)
static uint32_t  exp2WallNoteOff[EXP2_MAX_SIDES];  // millis at which to send note-off

// Note offsets (semitones from root) per wall position, by scale type (8 entries to cover octagon)
static const uint8_t kExp2ScaleMAJ[] = {0, 2, 4, 5, 7, 9, 11, 12};
static const uint8_t kExp2ScaleMIN[] = {0, 2, 3, 5, 7, 8, 10, 12};
static const uint8_t kExp2ScalePNT[] = {0, 2, 4, 7, 9, 12, 14, 16};
static const uint8_t kExp2ScaleCHR[] = {0, 2, 4, 6,  8, 10, 12, 14};
static const uint8_t* kExp2Scales[]  = {kExp2ScaleMAJ, kExp2ScaleMIN, kExp2ScalePNT, kExp2ScaleCHR};

// ==================== EXP3 STATE (MODE_EXP3) ====================
// Orbital: 8 balls (one per keyboard column) orbit a star on 4 possible orbits.
// Speed locked to BPM: orbit0=1beat, orbit1=2beats, orbit2=4beats, orbit3=8beats.
// Joystick X = global speed with inertia, Y = gate duration.
// Layout: col=ball(0-7), row=orbit(0=inner..3=outer). Same row twice = deactivate.
#define EXP3_MAX_BALLS 8
#define EXP3_NUM_ORBITS 4
struct Exp3Ball {
    float   angle;     // current orbital angle (radians, CCW from right in screen coords)
    uint8_t orbitRow;  // orbit index 0-3 → kExp3Radii[]
    bool    active;
    bool    triggered; // true while inside trigger zone (debounce)
};
static Exp3Ball   exp3Balls[EXP3_MAX_BALLS];
static uint8_t    exp3BallNote[EXP3_MAX_BALLS];  // last note triggered per ball (0xFF=none)
static uint32_t   exp3NoteOffMs[EXP3_MAX_BALLS]; // note-off timestamp per ball
static float      exp3SpeedMul   = 1.0f;  // global speed multiplier
static float      exp3SpeedVel   = 0.0f;  // inertia velocity on speed (from joystick X)
static uint16_t   exp3GateMs     = 200;   // note gate in ms (from joystick Y or pot)
static uint8_t    exp3Scale      = 0;     // 0=MAJ 1=MIN 2=PNT 3=CHR
static int8_t     exp3Octave     = 0;     // -2..+1
static uint8_t    exp3FxMask     = 0;     // bit0=REV bit1=DLY bit2=CHR

// Orbital radii for orbit 0-3 (pixels from center, 64,64)
static const float kExp3Radii[] = {12.0f, 24.0f, 36.0f, 48.0f};
// Beats per full orbit for each orbit (inner=fast, outer=slow)
static const float kExp3Beats[]  = {1.0f, 2.0f, 4.0f, 8.0f};

// ==================== LIFE STATE (MODE_LIFE) ====================
// Conway's Game of Life on the full 4x8 playable key grid (KBD_NOTE_ROWS x KBD_COLS,
// same addressable space EXP/EXP2/EXP3 already use). Column = pitch (scale-quantized via
// kExp2Scales[], reused verbatim), row = octave. Torus topology (wraps both axes) so a
// small 4x8 grid doesn't have a permanently-dead border. Key press manually seeds/kills a
// cell. Only birth/death EDGES trigger note-on/off (not every alive cell every tick), and
// only one voice sounds per column at a time — this caps polyphony at KBD_COLS=8, safely
// inside NUM_SYNTH_VOICES(8), so no dedicated oscillator range is needed.
static bool     lifeGrid[KBD_NOTE_ROWS][KBD_COLS];
static uint8_t  lifeColNote[KBD_COLS];       // currently-sounding MIDI note per column, 0xFF=none
static uint32_t lifeLastStepMs = 0;
static uint8_t  lifeRule       = 0;          // 0=classic B3/S23, 1=HighLife B36/S23
static uint8_t  lifeTickDivIdx = 3;          // index into kDelaySubdiv[] — simulation tick rate
static uint8_t  lifeScale      = 0;          // index into kExp2Scales[]
static EnvPreset lifeEnv       = ENV_PLUCK;
static bool     lifePaused     = false;      // B1 toggles — freezes the simulation tick
static SynthShape lifeShape    = SHAPE_PLUCK; // P2-selected instrument/algorithm (display only, audioSetShape does the work)

// ==================== SWARM STATE (MODE_SWARM) ====================
// Boids flocking (cohesion/separation/alignment), explicitly reusing EXP2's ball struct
// shape, polygon-arena containment, and speed-cap clamp (see the physics tick for what's
// literally shared vs newly-written). A joystick-steered attractor point pulls the flock
// and defines a small trigger radius: boids crossing into it fire a scale-quantized note.
#define SWARM_MAX_BOIDS 8
struct SwarmBoid { float x, y, vx, vy; bool active, inZone; };
static SwarmBoid  swarmBoids[SWARM_MAX_BOIDS];
static float      swarmHexAngle   = 0.0f;   // rotating arena angle (reused from EXP2)
static float      swarmRotVel     = 0.01f;
static uint8_t     swarmNumSides   = 6;
static float      swarmFlockBal   = 0.5f;   // 0=max separation, 1=max cohesion
static float      swarmSpeedCap   = 4.0f;
static float      swarmAttractStr = 0.10f;
static float      swarmZoneRadius = 10.0f;
static uint8_t     swarmScale      = 0;
static uint8_t     swarmOctave     = 0;
static uint8_t     swarmBoidNote[SWARM_MAX_BOIDS];
static uint32_t    swarmNoteOffMs[SWARM_MAX_BOIDS];
static uint8_t     swarmFxMask     = 0;      // bit0=REV bit1=DLY bit2=CHR

// ==================== GEN STATE (MODE_GEN) ====================
// Generalizes LIFE/SWARM's "a procedural process triggers notes" idea one level further:
// instead of one fixed generator algorithm, GEN exposes a small BROWSABLE set of distinct
// procedural textures (joystick X) crossed with a small BROWSABLE set of distinct
// sound-making methods (joystick Y) — the two axes are fully independent, so any texture
// can be hard with a pluck, soft with a pad, or grimy with the glitch voice. Each generator
// has its own tunable params (P2/P4), each voice its own single tunable param (P5).
static uint8_t  genGenerator   = 0;      // index into kGenNames[]/GEN_GENERATOR_COUNT
static uint8_t  genVoice       = 0;      // index into kGenVoices[]/GEN_VOICE_COUNT
static uint8_t  genTickDivIdx  = 3;      // index into kDelaySubdiv[] — generator tick rate
static uint32_t genLastStepMs  = 0;
static bool     genPaused      = false;  // B1 toggles, same convention as LIFE
static uint8_t  genScale       = 0;      // index into kExp2Scales[]
static uint8_t  genOctave      = 3;      // 0..6-ish, centered around a playable register
static uint8_t  genLastNote    = 0xFF;   // currently-sounding generator note, 0xFF=none
static uint8_t  genLastCol     = 0;      // last column the generator landed on (for LED/OLED)
static float    genVoiceParam  = 0.5f;   // P5: voice-specific brightness/cutoff-ish knob, 0..1

// WALK: a single walker steps across the 8 columns each tick.
static uint8_t  genWalkPos     = 3;
static float    genWalkWander  = 0.5f;   // P2: 0=mostly holds still, 1=jumps more/further

// EUCL: classic Euclidean rhythm (Bjorklund), k pulses over 8 steps, rotating playhead.
static uint8_t  genEuclPulses  = 3;      // P2: 1..8
static uint8_t  genEuclStepIdx = 0;      // playhead 0..7
static bool     genEuclPattern[8];
static uint8_t  genEuclPulsesBuilt = 0xFF; // pattern is only rebuilt when pulses actually changes

// DRIFT: logistic-map chaos (x' = r*x*(1-x)) quantized to a column — genuinely
// unpredictable/non-repeating texture, deliberately embracing chaos here (unlike the LDR
// filter fix) since "interesting and a little out of control" is the point of this one.
static float    genDriftX      = 0.42f;
static float    genDriftR      = 3.7f;   // P2: 3.5 (near-periodic) .. 3.99 (fully chaotic)

struct GenVoiceDef { const char* name; SynthShape shape; EnvPreset env; };
static const GenVoiceDef kGenVoices[] = {
    { "Pluck",  SHAPE_PLUCK,        ENV_PLUCK },
    { "Pad",    SHAPE_JUNO_STRINGS, ENV_PAD   },
    { "Glitch", SHAPE_NOISE_WHITE,  ENV_FAST  },
};
#define GEN_VOICE_COUNT (sizeof(kGenVoices)/sizeof(kGenVoices[0]))
static const char* kGenNames[] = { "Walk", "Eucl", "Drift" };
#define GEN_GENERATOR_COUNT (sizeof(kGenNames)/sizeof(kGenNames[0]))

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
// gran2PlayMode: 0=NRM(one-shot), 1=LOP(loop slice), 2=FUL(loop full), 3=SEQ(sequential queue)
// Fixed x4: 4 samples (TL=S0, TR=S1, BL=S2, BR=S3), 4 slices each, fwd+rev rows per half.
static uint8_t      gran2PlayMode     = 0;
static int8_t       gran2ActiveSample = -1;   // last-played sample index (for display + pot control)
static int8_t       gran2ActiveSlice  = -1;   // last-played slice within that sample
static uint8_t      gran2LoadTarget   = 0;    // which slot the SD browser loads into
static bool         gran2PotNeedsSync = true;

// SEQ mode: circular queue of slices to play in order, one at a time
#define GRAN2_SEQ_MAX 16
#define GRAN2_SEQ_OSC 32   // dedicated oscIdx (AMY osc GRANULAR_OSC_BASE+32=182, outside key range)
struct Gran2SeqEntry { uint8_t sampleIdx, sliceIdx; bool reverse; };
static Gran2SeqEntry g_gran2SeqQueue[GRAN2_SEQ_MAX];
static uint8_t  g_gran2SeqHead = 0, g_gran2SeqTail = 0;  // head=read, tail=write
static uint32_t g_gran2SeqEndMs   = 0;  // millis() when current slice ends (polling fallback)
static uint32_t g_gran2SeqNextAmy = 0;  // AMY sysclock (ms) at which next slice should start
static float    g_gran2SeqVolume = 0.7f;

// SQL mode: circular loop of up to 16 slices, looping continuously
#define GRAN2_SQL_MAX 16
static Gran2SeqEntry g_gran2SqlLoop[GRAN2_SQL_MAX];
static uint8_t  g_gran2SqlCount    = 0;   // valid entries in loop (0..GRAN2_SQL_MAX)
static uint8_t  g_gran2SqlPlayHead = 0;   // currently playing index in loop
static uint8_t  g_gran2SqlWriteIdx = 0;   // next write slot (overwrites oldest when full)
static uint32_t g_gran2SqlEndMs    = 0;   // millis() when current SQL slice ends
static uint32_t g_gran2SqlNextAmy  = 0;   // AMY sysclock for next SQL slice start

static uint8_t gran2NumSamples() { return 4u; }
static uint8_t gran2NumSlices()  { return 4u; }

static void gran2SeqStartHead() {
    if (g_gran2SeqHead == g_gran2SeqTail) return;
    const Gran2SeqEntry& e = g_gran2SeqQueue[g_gran2SeqHead];
    if (!gran2[e.sampleIdx].computed) { g_gran2SeqHead = (g_gran2SeqHead + 1) % GRAN2_SEQ_MAX; return; }
    // Schedule via AMY sysclock so back-to-back slices are sample-accurate regardless of poll jitter.
    // g_gran2SeqNextAmy is initialised to amy_sysclock() by the caller on first enqueue.
    // attackMs=2: each new slice retriggers the same oscillator (GRAN2_SEQ_OSC) — a hard cut
    // (attackMs=0) can land mid-waveform relative to the previous slice's last sample, producing
    // an audible click at every boundary. A tiny fade-in masks the discontinuity without being
    // perceptible as a fade for normal slice durations.
    audioPlayGranular2(GRAN2_SEQ_OSC, e.sampleIdx, e.sliceIdx, e.reverse, g_gran2SeqVolume, 0, 2, g_gran2SeqNextAmy);
    gran2ActiveSample = (int8_t)e.sampleIdx;
    gran2ActiveSlice  = (int8_t)e.sliceIdx;
    uint32_t totalMs = audioGranular2SampleLenMs(e.sampleIdx);
    float frac = gran2[e.sampleIdx].splits[e.sliceIdx + 1] - gran2[e.sampleIdx].splits[e.sliceIdx];
    uint32_t sliceMs = (uint32_t)(frac * (float)totalMs);
    g_gran2SeqNextAmy += sliceMs;          // accumulate: next event starts exactly when this one ends
    g_gran2SeqEndMs = millis() + sliceMs;  // millis-based deadline for head advance (polling fallback)
}

static void gran2SqlStartHead() {
    if (g_gran2SqlCount == 0) return;
    const Gran2SeqEntry& e = g_gran2SqlLoop[g_gran2SqlPlayHead];
    if (!gran2[e.sampleIdx].computed) {
        g_gran2SqlPlayHead = (g_gran2SqlPlayHead + 1) % g_gran2SqlCount;
        return;
    }
    // attackMs=2: see gran2SeqStartHead — masks the retrigger click between back-to-back slices.
    audioPlayGranular2(GRAN2_SEQ_OSC, e.sampleIdx, e.sliceIdx, e.reverse, g_gran2SeqVolume, 0, 2, g_gran2SqlNextAmy);
    gran2ActiveSample = (int8_t)e.sampleIdx;
    gran2ActiveSlice  = (int8_t)e.sliceIdx;
    uint32_t totalMs = audioGranular2SampleLenMs(e.sampleIdx);
    float frac = gran2[e.sampleIdx].splits[e.sliceIdx + 1] - gran2[e.sampleIdx].splits[e.sliceIdx];
    uint32_t sliceMs = (uint32_t)(frac * (float)totalMs);
    g_gran2SqlNextAmy += sliceMs;
    g_gran2SqlEndMs = millis() + sliceMs;
}

static void sampleSqlStartHead() {
    if (g_sampleSqlCount == 0) return;
    const SampleSqlEntry& e = g_sampleSqlLoop[g_sampleSqlPlayHead];
    uint8_t kidx = e.row * 8 + e.col;
    if (!audioKeyLoaded(kidx)) {
        g_sampleSqlPlayHead = (g_sampleSqlPlayHead + 1) % g_sampleSqlCount;
        return;
    }
    audioPlayKey(kidx, volume);
    uint32_t len = audioKeyLengthMs(kidx);
    g_sampleSqlEndMs = millis() + (len > 50 ? len : 500);
}

// EXP: compute quantized MIDI note from joyY and active modifiers
static uint8_t expComputeNote(float joyY) {
    // joyY -1=bottom=low, +1=top=high; center (C4=60) + 3 octaves range
    int8_t octOff = (int8_t)((int)expColSel[6] - 2) * 12;  // -24,-12,0,+12
    int rawNote = 60 + (int)(joyY * 30.0f) + octOff;
    rawNote = constrain(rawNote, 12, 108);
    uint8_t scale = expColSel[5];
    if (scale <= 1) return (uint8_t)rawNote;  // FREE or CHROM: no quantize
    static const uint8_t kPenta[] = {0,2,4,7,9};
    static const uint8_t kMajor[] = {0,2,4,5,7,9,11};
    const uint8_t* sc = (scale == 2) ? kPenta : kMajor;
    uint8_t scLen = (scale == 2) ? 5 : 7;
    uint8_t octave = (uint8_t)rawNote / 12;
    uint8_t semi   = (uint8_t)rawNote % 12;
    uint8_t best = sc[0]; uint8_t bestD = 12;
    for (uint8_t i = 0; i < scLen; i++) {
        uint8_t d = (uint8_t)abs((int)semi - (int)sc[i]);
        if (d > 6) d = 12 - d;
        if (d < bestD) { bestD = d; best = sc[i]; }
    }
    return (uint8_t)(octave * 12 + best);
}

// EXP: apply FX state (call after expFxMask changes)
static void expApplyFx() {
    audioSetReverb ((expFxMask&0x01)?0.55f:0.0f, 0.72f, 0.5f, 1500.0f);
    audioSetDelay  ((expFxMask&0x02)?0.40f:0.0f, 320.0f, 0.38f, 0.7f);
    audioSetChorus ((expFxMask&0x04)?0.40f:0.0f, 0.8f, 0.3f);
    if (!(expFxMask & 0x08)) audioSetWavefold(1.0f);  // REP off → dry
}

// ==================== EXP2 HELPERS ====================

static void exp2ApplyFx() {
    audioSetReverb  ((exp2FxMask&0x01)?0.55f:0.0f, 0.72f, 0.5f, 1500.0f);
    audioSetDelay   ((exp2FxMask&0x02)?0.40f:0.0f, 280.0f, 0.35f, 0.7f);
    audioSetChorus  ((exp2FxMask&0x04)?0.38f:0.0f, 0.9f, 0.28f);
    audioSetWavefold((exp2FxMask&0x08)?3.5f:1.0f);
}

static void exp2ResetBall(uint8_t i) {
    // Spawn near center with diverging directions so balls don't stack
    float angle = (float)i * (2.0f * (float)M_PI / (float)EXP2_MAX_BALLS) + 0.5f;
    float spd   = 2.2f + (float)i * 0.35f;
    exp2Balls[i] = {
        64.0f + cosf(angle) * 4.0f,
        64.0f + sinf(angle) * 4.0f,
        cosf(angle + (float)M_PI * 0.55f) * spd,
        sinf(angle + (float)M_PI * 0.55f) * spd,
        true
    };
}

static void audioPostNote(uint8_t note, float vel);  // forward-decl (defined near audioHandlerTask)

static void exp2TriggerWall(uint8_t w, float impactVel) {
    if (w >= EXP2_MAX_SIDES) return;
    uint32_t now = millis();
    if (now - exp2WallHitMs[w] < 55) return;
    exp2WallHitMs[w]   = now;
    exp2WallFlash[w]   = true;
    exp2WallFlashMs[w] = now;
    if (!audioReady) return;
    // Release previous note on this wall if still ringing
    if (exp2WallNote[w] != 0xFF) { audioPostNote(exp2WallNote[w], 0.f); exp2WallNote[w] = 0xFF; }
    const uint8_t* sc = kExp2Scales[exp2Scale % 4];
    int note = 60 + exp2Octave * 12 + (int)sc[w % exp2NumSides];
    note = constrain(note, 12, 108);
    float vel = constrain(impactVel / 4.0f, 0.3f, 1.0f) * volume;
    exp2WallNote[w]    = (uint8_t)note;
    exp2WallNoteOff[w] = now + 180;  // 180ms gate
    audioPostNote((uint8_t)note, vel);  // non-blocking: posted to audioHandlerTask queue
}

// ==================== EXP3 HELPERS ====================

static void exp3ApplyFx() {
    audioSetReverb ((exp3FxMask&0x01)?0.65f:0.0f, 0.78f, 0.45f, 2000.0f);
    audioSetDelay  ((exp3FxMask&0x02)?0.35f:0.0f, 480.0f, 0.42f, 0.8f);
    audioSetChorus ((exp3FxMask&0x04)?0.45f:0.0f, 0.7f, 0.25f);
}

// ball=0-7 (keyboard column), orbitRow=0(inner)..3(outer)
static uint8_t exp3ComputeNote(uint8_t ball, uint8_t orbitRow) {
    static const uint8_t kMaj[] = {0,2,4,5,7,9,11,12};
    static const uint8_t kMin[] = {0,2,3,5,7,8,10,12};
    static const uint8_t kPnt[] = {0,2,4,7,9,12,14,16};
    static const uint8_t kChr[] = {0,1,2,3,4,5,6,7};
    const uint8_t* sc = (exp3Scale==1)?kMin:(exp3Scale==2)?kPnt:(exp3Scale==3)?kChr:kMaj;
    uint8_t deg = ball % 8;
    // Inner orbit (row 0) = higher pitch; outer (row 3) = lower
    int8_t octOff = (int8_t)(3 - (int)orbitRow);  // 0..3 semitone groups up
    int note = 48 + exp3Octave * 12 + (int)octOff * 4 + (int)sc[deg];
    return (uint8_t)constrain(note, 12, 108);
}

// ==================== LIFE HELPERS ====================
// col=keyboard column (0-7) → scale degree, row=keyboard row (0-3) → octave.
static uint8_t lifeComputeNote(uint8_t row, uint8_t col) {
    const uint8_t* sc = kExp2Scales[lifeScale % 4];
    int note = 36 + (int)row * 12 + (int)sc[col % 8];
    return (uint8_t)constrain(note, 12, 108);
}

static void lifeReseed(float density) {
    for (int r = 0; r < KBD_NOTE_ROWS; r++)
        for (int c = 0; c < KBD_COLS; c++)
            lifeGrid[r][c] = ((float)random(0, 1000) / 1000.0f) < density;
}

// One Conway's-Game-of-Life simulation step, torus topology (wraps both axes so a small
// 4x8 grid has no permanently-dead border). Only birth/death EDGES per column trigger
// note-on/off — not every alive cell every tick — capping polyphony at KBD_COLS(8) voices.
static void lifeStep() {
    if (!audioReady) return;
    bool next[KBD_NOTE_ROWS][KBD_COLS];
    for (int r = 0; r < KBD_NOTE_ROWS; r++) {
        for (int c = 0; c < KBD_COLS; c++) {
            int n = 0;
            for (int dr = -1; dr <= 1; dr++) {
                for (int dc = -1; dc <= 1; dc++) {
                    if (dr == 0 && dc == 0) continue;
                    int rr = (r + dr + KBD_NOTE_ROWS) % KBD_NOTE_ROWS;
                    int cc = (c + dc + KBD_COLS) % KBD_COLS;
                    if (lifeGrid[rr][cc]) n++;
                }
            }
            bool alive = lifeGrid[r][c];
            if (alive) next[r][c] = (n == 2 || n == 3);
            else       next[r][c] = (n == 3) || (lifeRule == 1 && n == 6);
        }
    }
    for (int c = 0; c < KBD_COLS; c++) {
        bool wasAlive = false, willBeAlive = false;
        uint8_t firstAliveRow = 0;
        for (int r = 0; r < KBD_NOTE_ROWS; r++) {
            if (lifeGrid[r][c]) wasAlive = true;
            if (next[r][c]) { if (!willBeAlive) firstAliveRow = (uint8_t)r; willBeAlive = true; }
        }
        if (willBeAlive && !wasAlive) {
            uint8_t note = lifeComputeNote(firstAliveRow, (uint8_t)c);
            lifeColNote[c] = note;
            audioPostNote(note, 0.75f * volume);
        } else if (!willBeAlive && wasAlive && lifeColNote[c] != 0xFF) {
            audioPostNote(lifeColNote[c], 0.f);
            lifeColNote[c] = 0xFF;
        }
    }
    memcpy(lifeGrid, next, sizeof(lifeGrid));
}

// ==================== SWARM HELPERS ====================
static void swarmApplyFx() {
    audioSetReverb((swarmFxMask&0x01)?0.6f:0.0f, 0.78f, 0.45f, 2000.0f);
    audioSetDelay ((swarmFxMask&0x02)?0.35f:0.0f, 420.0f, 0.4f, 0.75f);
    audioSetChorus((swarmFxMask&0x04)?0.45f:0.0f, 0.7f, 0.25f);
}
static void swarmResetBoid(uint8_t i) {
    float a = (float)i * (2.0f*(float)PI / (float)SWARM_MAX_BOIDS);
    swarmBoids[i].x = cosf(a) * 15.0f; swarmBoids[i].y = sinf(a) * 15.0f;
    swarmBoids[i].vx = cosf(a) * 0.5f; swarmBoids[i].vy = sinf(a) * 0.5f;
    swarmBoids[i].active = true; swarmBoids[i].inZone = false;
    swarmBoidNote[i] = 0xFF;
}
static uint8_t swarmComputeNote(uint8_t boid) {
    const uint8_t* sc = kExp2Scales[swarmScale % 4];
    int note = 60 + swarmOctave*12 + (int)sc[boid % 8];
    return (uint8_t)constrain(note, 12, 108);
}

// ==================== GEN HELPERS ====================
static uint8_t genComputeNote(uint8_t col, uint8_t octave) {
    const uint8_t* sc = kExp2Scales[genScale % 4];
    int note = 36 + (int)octave * 12 + (int)sc[col % 8];
    return (uint8_t)constrain(note, 12, 108);
}
static void genApplyVoice() {
    const GenVoiceDef &v = kGenVoices[genVoice % GEN_VOICE_COUNT];
    audioSetShape(v.shape);
    audioSetEnvelope(envTable[v.env]);
    // P5 (genVoiceParam) doubles as brightness for pitched voices and cutoff for the
    // noise-based glitch voice — one knob, meaning tailored per voice so it always does
    // something audible rather than being blank for some voices.
    float cutoff = 300.0f * powf(40.0f, genVoiceParam); // 300Hz..12kHz exp
    audioSetFilter(cutoff, 1.4f);
}
// Bjorklund's algorithm: distributes `pulses` hits as evenly as possible over `steps` slots.
static void genRebuildEuclid() {
    uint8_t pulses = constrain(genEuclPulses, (uint8_t)1, (uint8_t)8);
    for (uint8_t i = 0; i < 8; i++) genEuclPattern[i] = false;
    // Simple accumulator form (equivalent result to full Bjorklund for a single 8-slot
    // ring): step i is a hit when its running fraction crosses an integer boundary.
    float acc = 0.0f;
    for (uint8_t i = 0; i < 8; i++) {
        acc += (float)pulses / 8.0f;
        if (acc >= 1.0f) { acc -= 1.0f; genEuclPattern[i] = true; }
    }
    genEuclPulsesBuilt = pulses;
}
// One generator tick: advances whichever generator is selected and returns the column to
// trigger, or -1 if this tick produces no note (EUCL's rests).
static int8_t genStep() {
    switch (genGenerator % GEN_GENERATOR_COUNT) {
        case 0: { // WALK
            float r = (float)random(0, 1000) / 1000.0f;
            int step = (r < genWalkWander) ? ((random(0,2) ? 1 : -1) * (1 + (int)(genWalkWander*2.0f))) : 0;
            genWalkPos = (uint8_t)(((int)genWalkPos + step + 8*4) % 8);
            return (int8_t)genWalkPos;
        }
        case 1: { // EUCL
            if (genEuclPulsesBuilt != genEuclPulses) genRebuildEuclid();
            uint8_t step = genEuclStepIdx;
            genEuclStepIdx = (uint8_t)((genEuclStepIdx + 1) % 8);
            return genEuclPattern[step] ? (int8_t)step : (int8_t)-1;
        }
        case 2: default: { // DRIFT
            genDriftX = genDriftR * genDriftX * (1.0f - genDriftX);
            if (genDriftX <= 0.0f || genDriftX >= 1.0f) genDriftX = 0.42f; // guard against escaping [0,1]
            return (int8_t)constrain((int)(genDriftX * 8.0f), 0, 7);
        }
    }
}

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

// ==================== INSTRUMENT BROWSER ====================
struct InstrCategory { const char* name; uint8_t start; uint8_t count; };
static const InstrCategory instrCategories[] = {
    {"WAVE",   0,  4},   // SAW SAW_FM SQR SIN
    {"SYNTH",  4,  4},   // SSAW ACID BASS PLCK
    {"NOISE",  8,  3},   // WHT PINK BRWN
    {"JUNO",  11,  7},   // J:BRS J:STR J:PNO J:ORG J:CHR J:OR2 J:FRG
    {"DX7",   18,  7},   // D:EP D:BEL D:BAS D:BRS D:STR D:ORG D:VOC
    {"TECH",  25,  4},   // T:LED T:BAS HOVR STAB
    {"ACID",  29,  3},   // WOBB EPLK INDS
    {"FM",    32,  3},   // FMDFT FMBEL SDFT
};
static const uint8_t INSTR_CAT_COUNT = 8;
static uint8_t instrBrCat  = 0;  // currently highlighted category
static uint8_t instrBrItem = 0;  // currently highlighted item within category

// Menu
bool    menuOpen       = false;
uint8_t menuRow        = 0, menuCol = 0;
uint8_t menuCategory   = 0;    // active tab: 0=INSTR, 1=SEQNC, 2=AUTRE
bool    menuOnTabBar   = true;  // true=cursor on tab bar, false=cursor in item grid

// Tab/sub-menu category definitions
static const char* kMenuCatNames[] = { "INSTR", "SEQNC", "AUTRE" };
static const MenuItem kMenuCatInstr[] = {
    MENU_SYNTH, MENU_STONE, MENU_OMNI, MENU_SAMPLE,
    MENU_MOD2, MENU_I303, MENU_MODULAR,
    MENU_GRANULAR2, MENU_EXP,
    MENU_EXP2, MENU_EXP3, MENU_POKEMON,
    MENU_LIFE, MENU_SWARM, MENU_GEN
};
static const MenuItem kMenuCatSeq[] = {
    MENU_DRUM2, MENU_DR2, MENU_303S2, MENU_SYSEQ, MENU_SS2, MENU_GEST
};
static const MenuItem kMenuCatAut[] = {
    MENU_LIGHT, MENU_LIGHTPLAY, MENU_ABOUT, MENU_SD, MENU_ANIM, MENU_VID, MENU_LANIM, MENU_PCMCLEAN, MENU_IMPORT, MENU_MIDI
};
static const MenuItem* kMenuCatItems[] = { kMenuCatInstr, kMenuCatSeq, kMenuCatAut };
static const uint8_t kMenuCatSizes[] = { 15, 6, 10 };

// ==================== VID STATE ====================
struct __attribute__((packed)) BvidHeader {
    char     magic[4];
    uint16_t width, height;
    uint8_t  fps;
    uint8_t  _pad[3];
    uint32_t frame_count;
};
static File     vidFile;
static bool     vidFileOpen    = false;
static bool     vidPlaying     = false;
static uint8_t  vidFps         = 10;
static uint32_t vidFrameCount  = 0;
static uint32_t vidCurrentFrame = 0;
static uint32_t vidLastFrameMs = 0;
static EXT_RAM_ATTR uint8_t  vidFrameBuf[128 * 128 / 8];  // 2048 bytes in PSRAM
// MEDIA mode (MODE_VID) .wav/.mp3 preview playback — same PCM_PREVIEW_PRESET/
// audioLoadAndPlay() mechanism MODE_SAMPLE's browser already uses.
static bool     mediaAudioPlaying = false;
static String   mediaAudioPath;

// ==================== DRANI STATE ====================
// Folder of .bvid images: first = base layer, next 8 = per-drum overlays.
#define DRANI_MAX_DRUMS 8
#define DRANI_FRAME_BYTES 2048
#define DRANI_HIT_MS 400
static bool     draniRunning        = false;
static bool     draniBrowse         = true;    // true = showing SD folder browser (drum2View==2)
static uint8_t  draniNumOverlays    = 0;
static EXT_RAM_ATTR uint8_t  draniBaseBuf[DRANI_FRAME_BYTES];
static EXT_RAM_ATTR uint8_t  draniOverlayBuf[DRANI_MAX_DRUMS][DRANI_FRAME_BYTES];
static EXT_RAM_ATTR uint8_t  draniDisplayBuf[DRANI_FRAME_BYTES];
static uint32_t draniDrumHitMs[DRANI_MAX_DRUMS] = {};
bool    lastClick      = false;
uint32_t joyClickMs   = 0;    // millis() when joystick click started (0=not pressed)
bool    joyLongFired  = false; // long-click action already triggered this press
bool    joyBrwActed   = false; // browser short-click already acted this press (blocks long-press)
uint32_t btn1PressTime = 0;
bool    btn1Handled    = false;

// Pots
// rawDelta: this frame's relative rotation, in the same units as value's 0..pMax
// range, but NOT clamped by it — value saturates at its bounds (so a shared pot
// re-used for several different per-item targets, e.g. GEST2's per-pad drum
// params, loses headroom once value pins at 0 or pMax: further turning produces
// no further change in value, capping how far any given item's own parameter can
// be pushed). Consumers that apply the pot as a RELATIVE delta directly onto
// their own already-unbounded stored value (instead of converting value's
// absolute position) should read rawDelta, not value, to avoid inheriting that cap.
struct EncPot { float prevAngle; float accum; float value; bool init; float rawDelta; };
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
    // FILT: filtre général — Typ=type (0=LPF,1=HPF,2=BPF,3=LADDER: custom resonant/
    // saturating 4-pole filter, exotic non-linear rolloff — see audioSetLadderFilter())
    {"FILT",     false, {2000.0f, 1.5f, 0.0f, 0.0f},
     {"Cut","Res","","Typ"},
     {65.0f,   0.5f, 0.0f, 0.0f},
     {18000.0f, 3.0f, 0.0f, 3.0f}},
    // Distortion: wavefold-style saturation via filter drive + tone shaping
    {"DISTORT",  false, {0.6f, 0.5f,  0.0f, 0.0f},
     {"Drv","Ton","",""},
     {0.0f, 0.0f, 0.0f, 0.0f},
     {1.0f, 1.0f, 0.0f, 0.0f}},
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
    // REP (replie): global wavefold on all audio — threshold=fraction of peak above which signal folds.
    // gain = 1/threshold applied in AMY bus 0 after T303 merge.
    {"REP",      false, {0.5f, 0.0f, 0.0f, 0.0f},
     {"Seuil","","",""},
     {0.05f, 0.0f, 0.0f, 0.0f},
     {1.0f,  0.0f, 0.0f, 0.0f}},
    // BITCRS: wavefold at extreme gain — simulates bit-depth reduction.
    // param[0]=Bits (2-8): lower = more crushing; gain = 2^(9-bits) → 2x (8bit) to 128x (2bit).
    // param[1]=Cut: optional LPF after fold for classic lo-fi character (0=off, 200-8000Hz).
    {"BITCRS",   false, {6.0f, 0.0f, 0.0f, 0.0f},
     {"Bits","Cut","",""},
     {2.0f,   0.0f, 0.0f, 0.0f},
     {8.0f, 8000.0f, 0.0f, 0.0f}},
    // TREMOLO: volume LFO — software 10ms tick (same pattern as the LFO filter-mod
    // slot above, just modulating the volume pot's output instead of cutoff).
    {"TREMOLO",  false, {5.0f, 0.6f, 0.0f, 0.0f},
     {"Rate","Dep","",""},
     {0.1f, 0.0f, 0.0f, 0.0f},
     {20.0f, 1.0f, 0.0f, 0.0f}},
    // AUTOPAN: stereo pan LFO — same software-tick approach as TREMOLO, driving
    // audioSetPan() (real per-oscillator equal-power pan; the board's I2S output is
    // genuinely stereo, see i2s.c's I2S_SLOT_MODE_STEREO) instead of volume.
    {"AUTOPAN",  false, {2.0f, 0.7f, 0.0f, 0.0f},
     {"Rate","Dep","",""},
     {0.1f, 0.0f, 0.0f, 0.0f},
     {10.0f, 1.0f, 0.0f, 0.0f}},
    // OVERDRIVE: single-knob filter drive (LPF sweep, cutoff 3500→1000Hz + resonance
    // 1.5→4.0 as Drv rises) — simpler/gentler character than FILT's full 4-type/2-
    // param filter or DISTORT's BPF-peak crunch; audioSetOverdrive() already existed
    // in audio_engine.cpp but had never been wired to an FX slot.
    {"OVERDRV",  false, {0.4f, 0.0f, 0.0f, 0.0f},
     {"Drv","","",""},
     {0.0f, 0.0f, 0.0f, 0.0f},
     {1.0f, 0.0f, 0.0f, 0.0f}},
    // RINGMOD: multiplies the signal by a sine carrier (bus-0 DSP in amy.c) — metallic/
    // bell-like inharmonic sidebands. Mix blends carrier-multiplied signal back with
    // dry so low Mix values give a subtle sheen rather than full ring-mod clang.
    {"RINGMOD",  false, {200.0f, 0.5f, 0.0f, 0.0f},
     {"Freq","Mix","",""},
     {20.0f, 0.0f, 0.0f, 0.0f},
     {2000.0f, 1.0f, 0.0f, 0.0f}},
    // COMPRESSOR: feedforward peak-envelope gain reduction (bus-0 DSP in amy.c, fixed
    // 5ms/80ms attack/release) — glue/limiting over everything else on the bus.
    {"COMPRESS", false, {0.5f, 4.0f, 0.0f, 0.0f},
     {"Thresh","Ratio","",""},
     {0.05f, 1.0f, 0.0f, 0.0f},
     {1.0f, 20.0f, 0.0f, 0.0f}},
};
static const uint8_t FX_COUNT = 15;

// Noms des types de filtre FILT — LPF, HPF, BPF
static const char* kFiltTypN[] = {"LPF","HPF","BPF","LDR"};
static const char* fxFiltTypName() {
    return kFiltTypN[(uint8_t)constrain((int)roundf(fxList[0].params[3]), 0, 3)];
}

// Delay subdivisions — param[1] is an index 0..6 into these tables
static const float kDelaySubdiv[]     = { 0.25f, 0.333f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f };
static const char* kDelaySubdivName[] = { "1/16","T1/8","1/8","D1/8","1/4","D1/4","1/2" };
static const uint8_t DELAY_SUBDIV_COUNT = 7;

uint8_t fxSelected = 0;
static float lpfSmoothCut = 8000.0f; // anti-zipper: smoothed cutoff applied each 10ms tick
static bool  s_filtMetaChanged = false; // true when FILT Res/Typ changed → applyFxEffect(0) needed

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

// ==================== MODULATION-SLOT ENGINE ====================
// Generic LFO-style modulation, separate from and additive to the existing fxList[10]
// (TREMOLO), [11] (AUTOPAN) slots — those stay exactly as they are (simple, single-toggle,
// fixed-destination FX everyone already relies on). This engine exists for what those
// can't do: automate an ARBITRARY fxList[] param via the FX-overlay double-click UI, and
// drive the modular synth's small mod matrix. Modeled directly on TREMOLO/AUTOPAN's own
// tick shape (phase accumulator + depth gate + restore-on-deactivate) — see the loop()
// 10ms block below for that precedent. (The old dedicated "LFO" FX slot that used to sit
// at fxList[6] and modulate filter cutoff the same way was removed once this generic
// engine could automate FILT's cutoff directly — see the FX list definition.)
enum ModWaveShape : uint8_t { MODSHAPE_SINE=0, MODSHAPE_TRI, MODSHAPE_SQR, MODSHAPE_SH, MODSHAPE_COUNT };
static const char* kModShapeName[] = { "Sine","Tri","Sqr","S&H" };
enum ModDestKind : uint8_t {
    MODDEST_NONE = 0,
    MODDEST_FX_PARAM,       // fxList[destA].params[destB]
    MODDEST_MOD_PITCH_A,    // modular synth (Section 5) — osc A pitch (semitones)
    MODDEST_MOD_PITCH_B,    // modular synth — osc B pitch (semitones)
    MODDEST_MOD_WTPOS_A,    // modular synth — osc A wavetable position
    MODDEST_MOD_WTPOS_B,    // modular synth — osc B wavetable position
    MODDEST_MOD_FILTER,     // modular synth — filter cutoff
    MODDEST_MOD_AMP         // modular synth — amplitude
};
struct ModSlot {
    bool         active    = false;
    ModWaveShape shape     = MODSHAPE_SINE;
    bool         bpmSync   = false;
    float        rateHz    = 2.0f;   // used when !bpmSync
    uint8_t      bpmDivIdx = 4;      // index into kDelaySubdiv[] when bpmSync (4 = "1/4")
    float        depth     = 0.0f;   // 0..1
    ModDestKind  destKind  = MODDEST_NONE;
    uint8_t      destA     = 0;      // FX_PARAM: fxList slot index
    uint8_t      destB     = 0;      // FX_PARAM: param index 0-3
    float        phase     = 0.0f;
    float        shHold    = 0.0f;
    bool         wasActive = false;  // restore-on-deactivate, same idiom as lfoWasActive etc.
};
#define MOD_SLOT_COUNT 6
// Slots 0-3: general pool, dynamically assigned by FX double-click automation (Section 2).
// Slot 4: reserved for the modular synth's single LFO source (Section 5).
// Slot 5: spare headroom.
ModSlot gModSlots[MOD_SLOT_COUNT];

// Curated LFO starting points for OVERLAY_FX_MOD, browsed with joystick Y (JX still cycles
// which FX param is being automated) — picking one sets shape/rate/sync together in one
// gesture instead of hunting across 3 separate pots to reconstruct a "good" combination by
// hand. P4 (depth) stays a separate, always-continuous pot since "how much" is the one
// dimension that's genuinely intuitive to sweep directly.
struct LfoProfile { const char* name; ModWaveShape shape; bool bpmSync; float rateHz; uint8_t bpmDivIdx; };
static const LfoProfile kLfoProfiles[] = {
    { "Slow Wave",    MODSHAPE_SINE, false, 0.4f, 4 },
    { "Fast Wave",    MODSHAPE_SINE, false, 5.0f, 4 },
    { "Tremolo",      MODSHAPE_SQR,  false, 6.0f, 4 },
    { "Synced 1/4",   MODSHAPE_TRI,  true,  2.0f, 4 },
    { "Synced 1/8 Chop", MODSHAPE_SQR, true, 2.0f, 2 },
    { "Slow Sync Sweep", MODSHAPE_SINE, true, 2.0f, 6 },
    { "Random Glitch", MODSHAPE_SH,  false, 9.0f, 4 },
};
#define LFO_PROFILE_COUNT (sizeof(kLfoProfiles)/sizeof(kLfoProfiles[0]))

static float modWaveformSample(ModWaveShape shape, float phase, float shHold) {
    switch (shape) {
        case MODSHAPE_SINE: return sinf(phase);
        case MODSHAPE_TRI:  return (phase < PI) ? (-1.0f + phase*2.0f/PI) : (3.0f - phase*2.0f/PI);
        case MODSHAPE_SQR:  return (phase < PI) ? 1.0f : -1.0f;
        case MODSHAPE_SH:   return shHold;
        default: return 0.0f;
    }
}

// Find a free general-purpose slot (0..3) for a new FX-param automation assignment, or -1.
int8_t modSlotAllocFxParam() {
    for (int8_t i = 0; i < 4; i++) if (!gModSlots[i].active) return i;
    return -1;
}
// Find the slot (if any) already automating this exact (fx,param) pair.
int8_t modSlotFindFxParam(uint8_t fx, uint8_t param) {
    for (int8_t i = 0; i < 4; i++)
        if (gModSlots[i].active && gModSlots[i].destKind==MODDEST_FX_PARAM
            && gModSlots[i].destA==fx && gModSlots[i].destB==param) return i;
    return -1;
}

// OVERLAY_FX_MOD state (long-press an active FX slot in OVERLAY_FX to enter — see
// s_fxPressOpt below and the poll near btn1PressTime in loop()).
static uint8_t s_fxModEditParam = 0;   // which of fxList[fxSelected]'s params is being automated
static int8_t  s_fxModEditSlot  = -1;  // gModSlots[] index owning this (fx,param); -1 = unassigned
static int8_t  s_fxModProfileIdx = -1; // joystick-Y cursor into kLfoProfiles[]; -1 = none picked yet (custom/pot-tuned)
// Long-press tracking for the FX grid: press arms this without acting yet; released
// before the threshold performs the normal toggle (overlayFxKeyRelease()), held past it
// opens the automation editor instead (polled in loop(), same idiom as btn1PressTime's
// long-press-to-menu) for an already-active FX (automating an inactive one makes no sense).
#define FX_LONGPRESS_MS 500
static uint8_t  s_fxPressOpt       = 255; // opt currently held down in the FX grid, 255=none
static uint32_t s_fxPressStartMs   = 0;
static bool     s_fxLongPressFired = false;

void applyFxEffect(uint8_t fx) {
    FxEffect &e = fxList[fx];
    bool on = e.active;
    // Helper: no FX filter active → safe to restore shape's native filter coefficients
    // BITCRS counts as filter-user when its Cut param is set (params[1] > 200Hz)
    auto noFilterFx = [&]() {
        return !fxList[0].active && !fxList[1].active && !fxList[12].active
               && !(fxList[9].active && fxList[9].params[1] > 200.0f);
    };
    switch (fx) {
        case 0: {  // FILT — filtre général (params[3]=Typ: 0=LPF,1=HPF,2=BPF,3=LADDER)
            static const uint8_t kFiltAMY[] = {FILTER_LPF, FILTER_HPF, FILTER_BPF};
            uint8_t ti  = (uint8_t)constrain((int)roundf(e.params[3]), 0, 3);
            bool ladder = (ti == 3);
            float   cut = e.params[0], res = e.params[1];
            Serial.printf("FX0 FILT %s cut=%.0f res=%.2f typ=%s\n",
                on?"ON":"off", on?cut:0.0f, on?res:1.5f, kFiltTypN[ti]);
            // LADDER bypasses AMY's own per-voice filter (plain linear biquads, no
            // ladder topology exists in AMY) and instead runs a custom bus-level 4-pole
            // filter with tanh-saturated feedback — see audioSetLadderFilter() and
            // amy.c's bus-0 processing block for the actual "exotic" nonlinear rolloff.
            audioSetAllFiltersT(on && !ladder ? cut : 0.0f, on && !ladder ? res : 1.5f,
                                (on && !ladder) ? kFiltAMY[ti] : FILTER_LPF24);
            audioSetLadderFilter(cut, res * 4.5f, on && ladder);  // *4.5: maps the shared 0.5-3.0 param range onto ~2.25-13.5, the smooth/dramatic part of the measured resonance-vs-RMS curve for this filter's feedforward design (RMS climbs ~7x smoothly across that span before the safety soft-clip starts compressing further gains past ~15)
            if (!on && noFilterFx()) audioRestoreShapeFilter(currentShape);
            // audioSetAllFiltersT() above reaches every channel including T303_CH, so
            // turning the FX LPF off leaves the 303 stuck on the FX's last cutoff/type
            // instead of its own native filter — restore it here. Not mode-gated (used
            // to be MODE_303S-only, missing MODE_303S2/I303/GEST/GEST2's 303 slot) since
            // this is a cheap no-op wherever T303_CH isn't actually sounding.
            if (!on) audioT303Params(t303Cutoff, t303Reso, t303EnvMod, t303Decay);
            break;
        }
        case 1:  // Distortion: drive + tone
            Serial.printf("FX1 DIST %s drv=%.2f ton=%.2f\n",
                          on?"ON":"off", on?e.params[0]:0.0f, e.params[1]);
            audioSetDistortion(on ? e.params[0] : 0.0f, e.params[1]);
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
        case 6:  // EQ
            Serial.printf("FX6 EQ  %s L=%.2f M=%.2f H=%.2f\n", on?"ON":"off",
                          on?e.params[0]:1.0f, on?e.params[1]:1.0f, on?e.params[2]:1.0f);
            audioSetEq(on ? e.params[0] : 1.0f,
                       on ? e.params[1] : 1.0f,
                       on ? e.params[2] : 1.0f);
            break;
        case 7: {  // ResEcho — BPM-synced echo with tonal filter_coef
            uint8_t si = (uint8_t)constrain((int)roundf(e.params[3]), 0, DELAY_SUBDIV_COUNT-1);
            float dms  = 60000.0f / (float)bpm * kDelaySubdiv[si];
            dms = constrain(dms, 30.0f, 700.0f);
            Serial.printf("FX7 RES %s lvl=%.2f fb=%.2f tone=%.2f %s=%.0fms\n",
                          on?"ON":"off", on?e.params[0]:0.0f, e.params[1], e.params[2],
                          kDelaySubdivName[si], dms);
            audioSetDelay(on ? e.params[0] : 0.0f, dms, e.params[1], e.params[2]);
            break;
        }
        case 8: {  // REP — global wavefold; threshold=e.params[0], gain=1/threshold
            float thr = constrain(e.params[0], 0.05f, 1.0f);
            float baseWf = (currentMode == MODE_POKEMON) ? kPokemon[pkmnSelected].wavefold : 1.0f;
            float gain = on ? (baseWf / thr) : baseWf;
            Serial.printf("FX8 REP %s seuil=%.2f gain=%.1f\n", on?"ON":"off", thr, gain);
            audioSetWavefold(gain);
            break;
        }
        case 9: {  // BITCRS — wavefold at extreme gain to simulate bit-depth reduction
            float bits = constrain(e.params[0], 2.0f, 8.0f);
            float baseWf = (currentMode == MODE_POKEMON) ? kPokemon[pkmnSelected].wavefold : 1.0f;
            float gain = on ? powf(2.0f, 9.0f - bits) : baseWf;  // 8bit→2x, 4bit→32x, 2bit→128x; off→restore
            float cut  = e.params[1];
            Serial.printf("FX9 BITCRS %s bits=%.0f gain=%.1f cut=%.0f\n",
                          on?"ON":"off", bits, gain, cut);
            audioSetWavefold(gain);
            // LPF: use audioSetAllFiltersT so the filter type is properly set (LPF24)
            if (on && cut > 200.0f) {
                audioSetAllFiltersT(cut, 1.5f, FILTER_LPF24);
            } else if (!on || cut <= 200.0f) {
                if (noFilterFx()) audioRestoreShapeFilter(currentShape);
            }
            break;
        }
        case 10:  // TREMOLO — handled in the 10ms loop (volume LFO)
            break;
        case 11:  // AUTOPAN — handled in the 10ms loop (pan LFO)
            break;
        case 12:  // OVERDRIVE — single-knob filter drive
            Serial.printf("FX12 OVERDRV %s drv=%.2f\n", on?"ON":"off", on?e.params[0]:0.0f);
            audioSetOverdrive(on ? e.params[0] : 0.0f);
            if (!on && noFilterFx()) audioRestoreShapeFilter(currentShape);
            break;
        case 13:  // RINGMOD — bus-0 sine-carrier amplitude modulation
            Serial.printf("FX13 RINGMOD %s freq=%.0f mix=%.2f\n", on?"ON":"off", e.params[0], e.params[1]);
            audioSetRingmod(e.params[0], e.params[1], on);
            break;
        case 14:  // COMPRESSOR — bus-0 feedforward peak compressor
            Serial.printf("FX14 COMPRESS %s thr=%.2f ratio=%.1f\n", on?"ON":"off", e.params[0], e.params[1]);
            audioSetCompressor(e.params[0], e.params[1], on);
            break;
    }
}

// Apply one modulation sample. FX_PARAM uses an "apply then restore fxList[].params[],
// call applyFxEffect()" trick: fxList[].params[] holds the user's pot-set "center" value,
// which we temporarily overwrite, push through the normal FX-apply path (zero changes
// needed to applyFxEffect() itself), then restore — so the center value the user dialed in
// is never lost, only the AMY-facing state is momentarily modulated.
static void modSlotApply(ModSlot &s, float sample) {
    if (s.destKind == MODDEST_FX_PARAM) {
        FxEffect &fx = fxList[s.destA];
        float mn = fx.paramMin[s.destB], mx = fx.paramMax[s.destB];
        float base = fx.params[s.destB];
        float mod = constrain(base + s.depth*(mx-mn)*0.5f*sample, mn, mx);
        fx.params[s.destB] = mod;
        applyFxEffect(s.destA);
        fx.params[s.destB] = base;
    }
    // MODDEST_MOD_WTPOS_B: the modular synth's one LFO destination for this MVP —
    // wobbles osc B's wavetable morph position around its pot-set base (modOscBPos),
    // giving the classic "evolving wavetable" motion. gModSlots[4] is reserved for this
    // (see Section 1's declaration comment); other MODDEST_MOD_* destinations exist in
    // the enum for a future deeper mod-matrix but aren't wired to anything yet.
    else if (s.destKind == MODDEST_MOD_WTPOS_B) {
        float pos = constrain(modOscBPos + s.depth * 0.5f * sample, 0.0f, 1.0f);
        audioModularSetWtPos(MOD3_OSCB_CH, pos);
    }
}

static void modSlotRestore(ModSlot &s) {
    if (s.destKind == MODDEST_FX_PARAM) {
        applyFxEffect(s.destA); // params[] already holds the true base value — just re-push it
    } else if (s.destKind == MODDEST_MOD_WTPOS_B) {
        audioModularSetWtPos(MOD3_OSCB_CH, modOscBPos); // restore to the pot's own base position
    }
}

void modSlotsTick10ms() {
    for (uint8_t i = 0; i < MOD_SLOT_COUNT; i++) {
        ModSlot &s = gModSlots[i];
        if (!s.active || s.depth < 0.005f) {
            if (s.wasActive) { modSlotRestore(s); s.wasActive = false; }
            continue;
        }
        float rateHz = s.bpmSync ? ((float)bpm / (60.0f * kDelaySubdiv[s.bpmDivIdx])) : s.rateHz;
        s.phase += rateHz * 2.0f * (float)PI * 0.01f;
        if (s.phase > 2.0f*(float)PI) {
            s.phase -= 2.0f*(float)PI;
            if (s.shape == MODSHAPE_SH) s.shHold = ((float)random(-1000,1001)) / 1000.0f;
        }
        float sample = modWaveformSample(s.shape, s.phase, s.shHold);
        s.wasActive = true;
        modSlotApply(s, sample);
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
bool isVidFile(const char* name) {
    const char* ext = strrchr(name, '.');
    return ext && strcasecmp(ext,".bvid")==0;
}
// Plain photos browsable alongside .bvid in MODE_VID / DRUM2-anim — auto-converted
// on first use by loadOrConvertBvid(). JPEG and PNG, decoded by imgDecodeGray()
// (ESP32: include/jpegdec.h — JPEG via the framework's bundled esp_jpeg with a
// decode-time downscale, PNG via vendored stb_image with a pixel-count safety cap
// since it has no such downscale; simulator/android: simulator/hal/jpegdec.h,
// both formats via stb_image, uncapped).
bool isImgFile(const char* name) {
    const char* ext = strrchr(name, '.');
    return ext && (strcasecmp(ext,".jpg")==0 || strcasecmp(ext,".jpeg")==0 || strcasecmp(ext,".png")==0);
}
bool isVidOrImgFile(const char* name) { return isVidFile(name) || isImgFile(name); }
// MEDIA mode's own browser filter — everything MODE_VID can open (.bvid/.png/.jpg/.jpeg
// plus .wav/.mp3). Deliberately separate from isVidOrImgFile(): MODE_DRUM2's animation-
// folder browser reuses isVidOrImgFile() too and must NOT gain audio files.
bool isMediaFile(const char* name) { return isVidOrImgFile(name) || isAudioFile(name); }

// Floyd-Steinberg-dithers `gray` (srcW x srcH, 1 byte/pixel) into a centered,
// letterboxed 128x128 1bpp frame (MSB-first per byte, row-major) — a C++ port of
// tools/to_bvid.py's compute_fit()/_dither_fs()/_pack_1bpp(), used for auto-converted
// stills so their look matches offline-converted .bvid files. outFrame2048 must be
// DRANI_FRAME_BYTES (2048) bytes.
static void bvidEncodeFromGray(const uint8_t* gray, int srcW, int srcH, uint8_t* outFrame2048) {
    memset(outFrame2048, 0, DRANI_FRAME_BYTES);
    if (srcW <= 0 || srcH <= 0) return;
    float scale = fminf(128.0f / srcW, 128.0f / srcH);
    int fitW = (int)fmaxf(1.0f, srcW * scale);
    int fitH = (int)fmaxf(1.0f, srcH * scale);
    int offX = (128 - fitW) / 2;
    int offY = (128 - fitH) / 2;

    // Canvas of error-diffused levels, content area only (borders stay black).
    // ps_malloc'd (not a static EXT_RAM_ATTR array): a 128*128 float array placed
    // that way still landed in internal DIRAM in testing rather than PSRAM, eating
    // 64KB of scarce internal RAM; ps_malloc is the mechanism proven to land in
    // PSRAM elsewhere in this codebase (audio_engine.cpp). Called rarely (once per
    // jpg conversion, not per-frame), so an alloc/free per call is cheap enough.
    float* p = (float*)ps_malloc(128 * 128 * sizeof(float));
    if (!p) return;
    memset(p, 0, 128 * 128 * sizeof(float));
    for (int row = 0; row < fitH; row++) {
        int srcRow = (int)((row + 0.5f) * srcH / fitH);
        if (srcRow >= srcH) srcRow = srcH - 1;
        for (int col = 0; col < fitW; col++) {
            int srcCol = (int)((col + 0.5f) * srcW / fitW);
            if (srcCol >= srcW) srcCol = srcW - 1;
            p[(offY + row) * 128 + (offX + col)] = gray[srcRow * srcW + srcCol] / 255.0f;
        }
    }
    int xEnd = offX + fitW, yEnd = offY + fitH;
    for (int y = offY; y < yEnd; y++) {
        for (int x = offX; x < xEnd; x++) {
            int idx = y * 128 + x;
            float old = p[idx];
            float nv = old >= 0.5f ? 1.0f : 0.0f;
            if (nv > 0.0f) outFrame2048[idx >> 3] |= (uint8_t)(0x80 >> (idx & 7));
            float err = old - nv;
            if (x + 1 < xEnd)              p[idx + 1]       += err * 7.0f / 16.0f;
            if (y + 1 < yEnd) {
                if (x > offX)               p[idx + 128 - 1] += err * 3.0f / 16.0f;
                                             p[idx + 128]     += err * 5.0f / 16.0f;
                if (x + 1 < xEnd)            p[idx + 128 + 1] += err * 1.0f / 16.0f;
            }
        }
    }
    free(p);
}

// Cache-or-convert entry point for a still image path: `path` may be a native
// .bvid (read directly) or a .jpg/.jpeg/.png (decoded via imgDecodeGray() +
// encoded via bvidEncodeFromGray(), then cached to SD so future loads skip the
// decode). Cache is a real .bvid sidecar `<path>.bvid` (readable by any existing
// .bvid consumer) plus a tiny `<path>.bvid.meta` (4-byte LE source-file size)
// used only to detect a changed source image — mirrors svcTryCache's
// srcSize-based validity check in audio_engine.cpp. Fills outFrame2048 (2048
// bytes) on success.
static bool loadOrConvertBvid(const String& path, uint8_t* outFrame2048) {
    if (isVidFile(path.c_str())) {
        File f = SD.open(path.c_str());
        if (!f) return false;
        BvidHeader hdr;
        bool ok = f.read((uint8_t*)&hdr, sizeof(hdr)) == sizeof(hdr)
               && memcmp(hdr.magic, "BVID", 4) == 0
               && hdr.width == 128 && hdr.height == 128
               && f.read(outFrame2048, DRANI_FRAME_BYTES) == DRANI_FRAME_BYTES;
        f.close();
        return ok;
    }
    if (!isImgFile(path.c_str())) return false;

    File src = SD.open(path.c_str());
    if (!src) return false;
    uint32_t srcSize = src.size();
    src.close();

    String bvidPath = path + ".bvid";
    String metaPath  = path + ".bvid.meta";

    // Cache hit: .meta's stored srcSize matches the jpg's current size, and the
    // cached .bvid is exactly one frame long.
    File meta = SD.open(metaPath.c_str());
    if (meta) {
        uint32_t cachedSize = 0;
        bool metaOk = meta.read((uint8_t*)&cachedSize, sizeof(cachedSize)) == sizeof(cachedSize);
        meta.close();
        if (metaOk && cachedSize == srcSize) {
            File cached = SD.open(bvidPath.c_str());
            if (cached) {
                bool sizeOk = cached.size() == (uint32_t)(sizeof(BvidHeader) + DRANI_FRAME_BYTES);
                BvidHeader hdr;
                bool ok = sizeOk
                       && cached.read((uint8_t*)&hdr, sizeof(hdr)) == sizeof(hdr)
                       && memcmp(hdr.magic, "BVID", 4) == 0
                       && cached.read(outFrame2048, DRANI_FRAME_BYTES) == DRANI_FRAME_BYTES;
                cached.close();
                if (ok) return true;
            }
        }
    }

    // Cache miss: decode the jpg, encode to a fresh .bvid, write both sidecars.
    Serial.printf("IMG: converting %s (%u bytes)\n", path.c_str(), (unsigned)srcSize);
    File jsrc = SD.open(path.c_str());
    if (!jsrc) { Serial.println("IMG: source open failed"); return false; }
    // ps_malloc (PSRAM), not malloc: a whole PNG/JPEG file can be several MB,
    // easily more than the ~200KB of internal RAM malloc() would otherwise try
    // to carve this out of.
    uint8_t* jbuf = (uint8_t*)ps_malloc(srcSize);
    if (!jbuf) { Serial.println("IMG: ps_malloc(srcSize) failed"); jsrc.close(); return false; }
    uint32_t got = jsrc.read(jbuf, srcSize);
    jsrc.close();
    if (got != srcSize) { Serial.printf("IMG: short read (%u/%u bytes)\n", (unsigned)got, (unsigned)srcSize); free(jbuf); return false; }

    uint8_t* gray = nullptr; int gw = 0, gh = 0;
    bool decoded = imgDecodeGray(jbuf, srcSize, &gray, &gw, &gh);
    free(jbuf);
    if (!decoded) { Serial.println("IMG: imgDecodeGray failed"); return false; }
    Serial.printf("IMG: decoded %dx%d\n", gw, gh);
    bvidEncodeFromGray(gray, gw, gh, outFrame2048);
    free(gray);

    BvidHeader hdr = {};
    memcpy(hdr.magic, "BVID", 4);
    hdr.width = 128; hdr.height = 128; hdr.fps = 1; hdr.frame_count = 1;
    File out = SD.open(bvidPath.c_str(), FILE_WRITE);
    if (out) {
        out.write((const uint8_t*)&hdr, sizeof(hdr));
        out.write(outFrame2048, DRANI_FRAME_BYTES);
        out.close();
    }
    File mout = SD.open(metaPath.c_str(), FILE_WRITE);
    if (mout) {
        mout.write((const uint8_t*)&srcSize, sizeof(srcSize));
        mout.close();
    }
    return true;
}

static void draniLoadFolder(const String& folderPath) {
    draniRunning     = false;
    draniNumOverlays = 0;
    memset(draniBaseBuf, 0, sizeof(draniBaseBuf));
    memset(draniOverlayBuf, 0, sizeof(draniOverlayBuf));
    File dir = SD.open(folderPath.c_str());
    if (!dir || !dir.isDirectory()) return;
    uint8_t loaded = 0;
    String base = folderPath; if (!base.endsWith("/")) base += "/";
    File entry = dir.openNextFile();
    while (entry && loaded < 9) {
        const char* fullName = entry.name();
        const char* name = strrchr(fullName, '/');
        name = name ? name + 1 : fullName;
        if (name[0] != '.' && isVidOrImgFile(name) && !entry.isDirectory()) {
            entry.close();  // loadOrConvertBvid reopens by full path
            uint8_t* dst = loaded == 0 ? draniBaseBuf : draniOverlayBuf[loaded - 1];
            if (loadOrConvertBvid(base + name, dst)) loaded++;
            entry = dir.openNextFile();
            continue;
        }
        entry.close();
        entry = dir.openNextFile();
    }
    dir.close();
    if (loaded > 0) {
        draniNumOverlays = loaded > 1 ? (uint8_t)(loaded - 1) : 0;
        draniRunning = true;
        memset(draniDrumHitMs, 0, sizeof(draniDrumHitMs));
        memcpy(draniDisplayBuf, draniBaseBuf, DRANI_FRAME_BYTES);
    }
}

static bool(*s_sdFileFilter)(const char*) = isAudioFile;

void sdListDir(const String &path, bool(*filter)(const char*) = nullptr) {
    s_sdFileFilter = filter ? filter : isAudioFile;
    sdFileCount=0; sdCursor=0; sdScroll=0; sdPath=path;
    // Always show ".." — at root it opens the main menu
    sdFiles[0]=".."; sdFileIsDir[0]=true; sdFileCount++;
    File dir=SD.open(path); if(!dir||!dir.isDirectory()) return;
    File entry=dir.openNextFile();
    while (entry && sdFileCount<32) {
        const char* full=entry.name(); const char* name=strrchr(full,'/');
        name=name?name+1:full;
        if (name[0]!='.' && (entry.isDirectory()||s_sdFileFilter(name))) {
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

// Rebuild the STONE P2 cycle list: indices into sdFiles[] that are (non-".." ) audio files.
// Call whenever sdFiles[]/sdPath changes while browsing in MODE_STONE.
static void stoneRebuildAudioIdx() {
    stoneAudioCount = 0;
    for (uint8_t i = 0; i < sdFileCount && stoneAudioCount < 32; i++) {
        if (sdFiles[i] == ".." || sdFileIsDir[i]) continue;
        stoneAudioIdx[stoneAudioCount++] = i;
    }
}

// ==================== PCM CACHE CLEANER ====================
// Iterative depth-first recursive deletion of .pcm/.pcm16 cache files.
// Keeps directory handles open on a stack so openNextFile() position is preserved.
// Called from main loop; processes a small batch per call so OLED stays responsive.
// Only File::name() is used below (portable across the real ESP32 VFS FS.h,
// where name() is the basename, and the simulator/web SD.h HAL, which has no
// path()); full paths are rebuilt manually from a parallel path-string stack.
#define PCMCLEAN_STACK_DEPTH 10
static File    pcmCleanDirStack[PCMCLEAN_STACK_DEPTH];
static String  pcmCleanPathStack[PCMCLEAN_STACK_DEPTH];
static uint8_t pcmCleanStackTop = 0;

static void pcmCleanStart() {
    for (uint8_t i = 0; i < pcmCleanStackTop; i++) pcmCleanDirStack[i].close();
    pcmCleanStackTop = 0;
    File root = SD.open("/");
    if (root && root.isDirectory()) {
        pcmCleanDirStack[0] = root;
        pcmCleanPathStack[0] = "/";
        pcmCleanStackTop = 1;
    }
}

// Returns true when fully done.
static bool pcmCleanStep(uint8_t entriesPerCall = 6) {
    if (!sdReady || pcmCleanStackTop == 0) return true;
    for (uint8_t i = 0; i < entriesPerCall; i++) {
        if (pcmCleanStackTop == 0) return true;
        const String& dirPath = pcmCleanPathStack[pcmCleanStackTop - 1];
        File entry = pcmCleanDirStack[pcmCleanStackTop - 1].openNextFile();
        if (!entry) {
            // Directory exhausted — pop
            pcmCleanDirStack[--pcmCleanStackTop].close();
            continue;
        }
        pcmCleanScanned++;
        String name = entry.name();
        String full = dirPath;
        if (!full.endsWith("/")) full += "/";
        full += name;
        if (entry.isDirectory()) {
            if (pcmCleanStackTop < PCMCLEAN_STACK_DEPTH) {
                pcmCleanPathStack[pcmCleanStackTop] = full;
                pcmCleanDirStack[pcmCleanStackTop++] = entry;
            } else {
                entry.close(); // stack full: skip deep dirs
            }
        } else {
            entry.close();
            if (name.endsWith(".pcm") || name.endsWith(".pcm16")) {
                if (SD.remove(full.c_str())) pcmCleanDeleted++;
            }
        }
    }
    return false;
}

// ==================== CONTROL LABELS (simulator/terminal legend) ====================
// P1 (pots[0]) and P3 (pots[2]) are Volume and BPM for every mode without exception
// (see loop(): "Pot 0 = Volume (always)" / "Pot 2 (P3) = BPM (global, always)", both
// applied unconditionally before the per-mode switch even runs) — always printed as
// such below. Everything else is mode-specific and catalogued here per-mode instead
// of duplicating a separate label table in the simulator: the simulator has no font
// renderer to draw on-screen text with anyway, so this legend is meant to be read
// off the terminal/debug console it already prints to (see winOpenDebugConsole() on
// Windows) — one source of truth (this switch mirrors the actual pot/button dispatch
// code), no separate mapping to keep in sync by hand. "-" means genuinely unused OR
// simply not yet catalogued here — both read the same to keep this honest rather
// than guessing; extend case-by-case as modes get audited.
struct CtrlLabels {
    const char* p2; const char* p4; const char* p5; const char* p6; const char* p7;
    const char* b1; const char* b2; const char* b3; const char* b4;
};

// Real parameter name of the currently-selected FX's slot p (0-3), matching exactly
// what the pot-dispatch code itself checks (fxList[fxSelected].paramNames[p]) before
// letting a pot touch it — so whenever a mode routes P4-P7 to FX params, the label
// shown is the ACTUAL parameter a pot nudge will change (e.g. "CUTOFF"/"RESO"), not a
// generic placeholder.
static const char* fxParamName(uint8_t p) {
    const char* n = fxList[fxSelected].paramNames[p];
    return (n && n[0]) ? n : "-";
}
static bool anyFxActive() {
    for (uint8_t f = 0; f < FX_COUNT; f++) if (fxList[f].active) return true;
    return false;
}

static CtrlLabels ctrlLabelsFor(AppMode m) {
    switch (m) {
        case MODE_SYSEQ:
        case MODE_SYNTH: {
            // SYSEQ falls through into SYNTH's own pot-dispatch case (identical P2/P4-P7
            // handling) — only B1-B4 differ between the two modes.
            const char* b1 = (m==MODE_SYNTH) ? "FX"   : "PLAY";
            const char* b2 = (m==MODE_SYNTH) ? "ARP"  : "FX";
            const char* b3 = (m==MODE_SYNTH) ? "ENV"  : "OPTS";
            const char* b4 = (m==MODE_SYNTH) ? "INSTR": "CLEAR";
            if (currentShape == SHAPE_SAW_FM)
                return {"SHAPE", "DEPTH", "CUTOFF", "-", "-", b1, b2, b3, b4};
            if (fxList[fxSelected].active)
                return {"SHAPE", fxParamName(0), fxParamName(1), fxParamName(2), fxParamName(3), b1, b2, b3, b4};
            return {"SHAPE", "-", "-", "-", "-", b1, b2, b3, b4};
        }
        case MODE_POKEMON:
            if (fxList[fxSelected].active)
                return {"PKMN", fxParamName(0), fxParamName(1), fxParamName(2), fxParamName(3), "FX", "ARP", "ENV", "PKMN"};
            return {"PKMN", "-", "-", "-", "-", "FX", "ARP", "ENV", "PKMN"};
        case MODE_OMNI: {
            bool fx = fxList[fxSelected].active;
            return {"SHAPE", fx?fxParamName(0):"WAVE", fx?fxParamName(1):"-", fx?fxParamName(2):"-", fx?fxParamName(3):"-",
                    "FX", "FX SEL", "-", "OCT"};
        }
        case MODE_I303:
            // I303's pots have no FX-routing branch at all (unlike 303S/STONE) — P4-P7
            // always control the 303 engine directly here.
            return {"WAVE", "RESO", "ENVMOD", "SUSTAIN", "CUTOFF", "FX", "ARP", "ENV", "WAVE"};
        case MODE_303S:
            if (s_overlay == OVERLAY_FX && fxList[fxSelected].active)
                return {"WAVE", fxParamName(0), fxParamName(1), fxParamName(2), fxParamName(3), "PLAY", "FX", "ACCENT/ALT", "SEQ/PAD"};
            return {"WAVE", "RESO", "ENVMOD", "DECAY", "CUTOFF", "PLAY", "FX", "ACCENT/ALT", "SEQ/PAD"};
        case MODE_303S2:
            return {"WAVE", "RESO", "ENVMOD", "DECAY", "CUTOFF", "PLAY", "FX", "ACCENT", "CLEAR"};
        case MODE_STONE:
            if (s_overlay == OVERLAY_FX && fxList[fxSelected].active)
                return {"CYCLE", fxParamName(0), fxParamName(1), fxParamName(2), fxParamName(3), "FX", "ARP", "LOOP", "OCT"};
            return {"CYCLE", "START", "END", "POS", "SIZE", "FX", "ARP", "LOOP", "OCT"};
        case MODE_SAMPLE:
            if (fxList[fxSelected].active)
                return {"-", fxParamName(0), fxParamName(1), fxParamName(2), fxParamName(3), "BACK", "FX", "AUTOMAP", "OPTS"};
            return {"-", "-", "-", "-", "-", "BACK", "FX", "AUTOMAP", "OPTS"};
        case MODE_GRANULAR2:
            return {"-", "START", "END", "POS", "SIZE", "FX", "-", "-", "-"};
        case MODE_SS2:
            if (anyFxActive())
                return {"-", fxParamName(0), fxParamName(1), fxParamName(2), fxParamName(3), "PLAY", "FX", "ALT/AUTOMAP", "SEQ/PAD"};
            return {"-", "-", "-", "-", "-", "PLAY", "FX", "ALT/AUTOMAP", "SEQ/PAD"};
        case MODE_DRUM2:
            if (anyFxActive())
                return {"-", fxParamName(0), fxParamName(1), fxParamName(2), fxParamName(3), "PLAY/BACK", "FX", "REC/BROWSE", "VIEW"};
            return {"-", "PITCH", "DECAY", "VOLUME", "-", "PLAY/BACK", "FX", "REC/BROWSE", "VIEW"};
        case MODE_DR2:
            if (anyFxActive())
                return {"-", fxParamName(0), fxParamName(1), fxParamName(2), fxParamName(3), "PLAY", "FX", "LOOP/LIVE", "COPY/PASTE"};
            if (dr2Instrument == DR2_INSTR_T303)
                return {"WAVE", "RESO", "ENVMOD", "DECAY", "CUTOFF", "PLAY", "FX", "LOOP/LIVE", "COPY/PASTE"};
            if (dr2Instrument == DR2_INSTR_SYNTH)
                return {"-", "VOLUME", "-", "-", "-", "PLAY", "FX", "LOOP/LIVE", "COPY/PASTE"};
            return {"-", "PITCH", "DECAY", "VOLUME", "-", "PLAY", "FX", "LOOP/LIVE", "COPY/PASTE"};  // DR2_INSTR_DRUMS
        case MODE_GEST:
            return {"-", "SEQVOL1", "SEQVOL2", "SEQVOL3", "SEQVOL4", "PLAY", "FX", "LOOP/LIVE", "COPY/PASTE"};
        case MODE_MOD2: {
            const Mod2AlgoDef& algo = kMod2Algos[mod2AlgoIdx];
            // B4 is dead code upstream (handleButton's MOD2 case checks btn==4, which
            // btn=3-rawBtn can never produce — see its own comment) so it genuinely does
            // nothing right now; "-" reflects that rather than guessing a function for it.
            return {"ALGO", algo.p[0].name, algo.p[1].name, algo.p[2].name, algo.p[3].name, "FX", "ENV", "ALGO", "-"};
        }
        case MODE_LIGHT:
            return {"-", "N", "SPEED", "HUE", "INTENSITY", "-", "-", "-", "-"};
        case MODE_MODULAR:
            return {"OSCA WAVE", "OSCA POS", "OSCB POS", "FILT CUT", "LFO DEPTH", "FX", "ENV", "OSCB WAVE", "OCTAVE"};
        case MODE_LIFE:
            return {"INSTR", "RULE", "TICKRATE", "RESEED", "ENV", "PAUSE", "-", "-", "-"};
        case MODE_SWARM:
            return {"SHAPE", "FLOCK", "SPEED", "ATTRACT", "ZONE", "-", "-", "-", "-"};
        case MODE_GEN:
            return {"TEXTURE PARAM", "TICKRATE", "SCALE", "OCTAVE", "VOICE PARAM", "PAUSE", "-", "-", "-"};
        default:
            return {"-", "-", "-", "-", "-", "-", "-", "-", "-"};
    }
}

#ifdef SIMULATOR
// Same-process mirror for the simulator's on-screen pot/button labels (see
// sim_window.cpp) — reads this struct directly rather than re-parsing the terminal
// print, since both live in the same binary here (unlike real ESP32 hardware, where
// the terminal is genuinely the only place this reaches). Called far more often than
// printCtrlLabels() itself (every ~150ms, see loop()) so the on-screen labels track
// live state within a mode too — which FX param a pot touches, DRUM2's per-pad
// pitch/decay/volume, etc. — not just what changes on a full mode switch.
static void updateSimCtrlLabels(AppMode m) {
    CtrlLabels L = ctrlLabelsFor(m);
    extern const char* g_ctrlP2; extern const char* g_ctrlP4; extern const char* g_ctrlP5;
    extern const char* g_ctrlP6; extern const char* g_ctrlP7; extern const char* g_ctrlB1;
    extern const char* g_ctrlB2; extern const char* g_ctrlB3; extern const char* g_ctrlB4;
    g_ctrlP2=L.p2; g_ctrlP4=L.p4; g_ctrlP5=L.p5; g_ctrlP6=L.p6; g_ctrlP7=L.p7;
    g_ctrlB1=L.b1; g_ctrlB2=L.b2; g_ctrlB3=L.b3; g_ctrlB4=L.b4;
}
#endif

static void printCtrlLabels(AppMode m) {
    CtrlLabels L = ctrlLabelsFor(m);
    Serial.println("P1 : VOL");
    Serial.printf ("P2 : %s\n", L.p2);
    Serial.println("P3 : BPM");
    Serial.printf ("P4 : %s\n", L.p4);
    Serial.printf ("P5 : %s\n", L.p5);
    Serial.printf ("P6 : %s\n", L.p6);
    Serial.printf ("P7 : %s\n", L.p7);
    Serial.printf ("B1 : %s\n", L.b1);
    Serial.printf ("B2 : %s\n", L.b2);
    Serial.printf ("B3 : %s\n", L.b3);
    Serial.printf ("B4 : %s\n", L.b4);
#ifdef SIMULATOR
    updateSimCtrlLabels(m);
#endif
}

// ==================== MODE SWITCH ====================
void drawScreen(bool blockWait = false);  // forward-decl
static void mod2AlgoApply();              // forward-decl

void switchMode(AppMode newMode) {
    // Stop sample oscillators when leaving any mode that uses them
    if (currentMode==MODE_SAMPLE)
        audioStopAllSamples();

    if (currentMode==MODE_MOD2){
        audioSetFilter(0.0f, 1.5f);
        audioSetPitchBend(1.0f);
    }
    if (currentMode==MODE_DRUM2){
        drum2RecArmed=false;
        // Sequencer keeps playing (shared with SYSEQ)
    }
    if (currentMode==MODE_SYSEQ){
        if (!gestDrillDown && !gestDrillReturn) {
            // Stop synth notes from sequencer; keep shared clock running
            for(uint8_t i=0;i<syseqActiveCnt;i++) audioNoteOff(syseqActive[i]);
            syseqActiveCnt=0; syseqActiveIsFull=false;
        }
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
    if (currentMode==MODE_303S2){
        if (!gestDrillDown && !gestDrillReturn && s303s2CurNote) { audioT303NoteOff(s303s2CurNote); s303s2CurNote=0; }
        // drum2Playing is master clock — no state to propagate
        for(uint8_t r2=0;r2<KBD_NOTE_ROWS;r2++)
            for(uint8_t c2=0;c2<KBD_COLS;c2++)
                if(activeNotes[r2][c2]){ audioT303NoteOff(activeNotes[r2][c2]); activeNotes[r2][c2]=0; }
    }
    if (currentMode==MODE_I303){
        midiAllNotesOff(MIDI_CH_BASS);
    }
    if ((currentMode==MODE_303S || currentMode==MODE_I303 || currentMode==MODE_303S2) && audioReady
        && !(gestDrillDown || gestDrillReturn))
        audioSW2Deactivate();
    if (currentMode==MODE_SS2){
        for(uint8_t r2=0;r2<KBD_NOTE_ROWS;r2++)
            for(uint8_t c2=0;c2<KBD_COLS;c2++)
                activeNotes[r2][c2]=0;
        // Keep samples in PSRAM — other sequencer modes may use them
    }
    if (currentMode==MODE_GRANULAR2) {
        audioStopGranular2(GRAN2_SEQ_OSC);
        g_gran2SeqHead = g_gran2SeqTail = 0;
        g_gran2SqlCount = g_gran2SqlPlayHead = g_gran2SqlWriteIdx = 0;
        audioUnloadGranular2();
    }
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
    if (currentMode==MODE_OMNI) {
        if (omniRoot!=0xFF) for(int i=0;i<3;i++) audioNoteOff(omniChordNotes[i]);
    }
    if (currentMode==MODE_STONE) {
        audioStoneAllNotesOff();
    }
    if (currentMode==MODE_DR2) {
        dr2Playing = false;
        dr2RecArmed = false;
    }
    if (currentMode==MODE_EXP) {
        if (expNoteOn) { audioNoteOff(expCurNote); expNoteOn=false; }
        expFxMask=0;
        if (audioReady) { expApplyFx(); audioSetFilter(0.0f,1.5f); audioSetWavefold(1.0f); }
    }
    if (currentMode==MODE_SAMPLE) {
        g_sampleSqlCount=g_sampleSqlPlayHead=g_sampleSqlWriteIdx=0;
    }
    if (currentMode==MODE_EXP2) {
        if (audioReady) {
            for (int w=0;w<EXP2_MAX_SIDES;w++) {
                if (exp2WallNote[w]!=0xFF) { audioNoteOff(exp2WallNote[w]); exp2WallNote[w]=0xFF; }
                exp2WallNoteOff[w]=0;
            }
            exp2FxMask=0; exp2ApplyFx(); audioSetFilter(0.0f,1.5f); audioSetWavefold(1.0f);
        } else {
            exp2FxMask=0;
        }
    }
    if (currentMode==MODE_EXP3) {
        if (audioReady) {
            for (int b=0;b<EXP3_MAX_BALLS;b++) {
                if (exp3BallNote[b]!=0xFF) audioNoteOff(exp3BallNote[b]);
                exp3NoteOffMs[b]=0; exp3BallNote[b]=0xFF;
            }
            exp3FxMask=0;
            exp3ApplyFx(); audioSetFilter(0.0f,1.5f); audioSetWavefold(1.0f);
        } else { exp3FxMask=0; }
    }
    if (currentMode==MODE_LIFE) {
        if (audioReady) {
            for (int c=0;c<KBD_COLS;c++) {
                if (lifeColNote[c]!=0xFF) { audioNoteOff(lifeColNote[c]); lifeColNote[c]=0xFF; }
            }
            audioSetReverb(0,0.78f,0.45f,2000.0f); audioSetFilter(0.0f,1.5f);
        }
    }
    if (currentMode==MODE_SWARM) {
        if (audioReady) {
            for (int b=0;b<SWARM_MAX_BOIDS;b++) {
                if (swarmBoidNote[b]!=0xFF) audioNoteOff(swarmBoidNote[b]);
                swarmNoteOffMs[b]=0; swarmBoidNote[b]=0xFF;
            }
            swarmFxMask=0;
            swarmApplyFx(); audioSetFilter(0.0f,1.5f); audioSetWavefold(1.0f);
        } else { swarmFxMask=0; }
    }
    if (currentMode==MODE_GEN) {
        if (audioReady) {
            if (genLastNote!=0xFF) { audioNoteOff(genLastNote); genLastNote=0xFF; }
            audioSetFilter(0.0f,1.5f);
        }
    }
    if (currentMode==MODE_MODULAR) {
        gModSlots[4].active = false;
        gModSlots[4].destKind = MODDEST_NONE;
        if (audioReady) { audioModularAllNotesOff(); audioModularSetFilter(0.0f, 1.5f); }
    }
    // Drill transitions (GEST ↔ sequencer) must NOT silence audio — the sequencer clock
    // keeps running and killing notes causes audible glitches/pauses.
    bool isDrillTransition = gestDrillDown || gestDrillReturn;
    if (!isDrillTransition) {
        audioAllNotesOff();
        // Reset filter/wavefold only when leaving a non-sequencer instrument mode
        if (audioReady) {
            // Was silently short by 4 entries (PCMCLEAN/STONE/DR2/IMPORT never added when
            // they were introduced — harmless for a bool array, since C++ aggregate init
            // zero-pads missing trailing entries to false, unlike kVizMode[]'s string array
            // a few lines below whose same gap would crash on printf("%s", nullptr), which
            // is why that one WAS kept correctly sized). That silent false-padding meant
            // DR2 (a real sequencer, see its own "hierarchical drum sequencer" comment)
            // defaulted to false, resetting the filter/wavefold on every exit from it like
            // a plain instrument mode instead of preserving them like DRUM2/SYSEQ/etc. do.
            static const bool kIsSeq[MODE_COUNT] = {
                false,false,false,false,false, // SYNTH,OMNI,SAMPLE,LIGHT,LIGHTPLAY
                false,false,false,false,false, // BATTERY,SYSINFO,MOD2,GRANULAR2,MIDI
                false,true, true, true, true,  // TRACKER,DRUM2,SYSEQ,303S,SS2
                false,false,false,false,false, // ANIM,I103,VID,LANIM,EXP
                false,false,true, false,false, // EXP2,EXP3,303S2,POKEMON,MODULAR
                true,                          // GEST
                false,false,true, false,       // PCMCLEAN,STONE,DR2,IMPORT
                false,false,                   // LIFE,SWARM
                false                          // GEN
            };
            if (!kIsSeq[currentMode]) {
                audioSetFilter(0.0f, 1.5f);
                audioSetWavefold(1.0f);
            }
        }
    }
    omniRoot=0xFF; omniStrumPos=-1; omniLastJoyY=0.0f;
    memset(activeNotes,0,sizeof(activeNotes));
    arpNoteCount=0; arpIdx=0; arpCurrent=0; arpUdDir=true;
    s_overlay = OVERLAY_NONE; s_overlayCloseAt = 0;
    currentMode=newMode;
    { static const char* kVizMode[MODE_COUNT]={
          "SYNTH","OMNI","SAMPL","LIGHT","LPLY",
          "BATT","DIAG","MOD2","GRANU",
          "MIDI","TRKR","DRUMS","SYNS","303S","SAMPS","ANIM","I303","MEDIA","LANIM",
          "EXP","EXP2","EXP3","303S","PKMN","SERUM","GEST",
          "PURGPCM","STONE","GEST2","IMPORT","LIFE","SWARM","GEN"};
      // This array must have exactly MODE_COUNT entries in AppMode order — the
      // compiler silently pads any missing trailing ones with nullptr (no size
      // mismatch warning), and printf("%s", nullptr) crashes (LoadProhibited).
      // That's exactly what caused the STONE/DR2 crashes: this list hadn't been
      // extended when MODE_PCMCLEAN/STONE/DR2 were added, so switchMode() into
      // any of them printed a null string and took down Core 1.
      Serial.printf("M:%s\n", newMode<MODE_COUNT?kVizMode[newMode]:"?"); }
    if (newMode < MODE_COUNT) printCtrlLabels(newMode);
    if (currentMode==MODE_VID && newMode!=MODE_VID) {
        if (vidFileOpen) { vidFile.close(); vidFileOpen=false; }
        vidPlaying=false;
        if (mediaAudioPlaying) { audioStopSamplePreset(PCM_PREVIEW_PRESET); mediaAudioPlaying=false; }
    }
    if (newMode==MODE_VID   && sdReady) sdListDir("/", isMediaFile);
    if (newMode==MODE_PCMCLEAN) { pcmCleanPhase=0; pcmCleanDeleted=0; pcmCleanScanned=0; pcmCleanRunning=false; }
    if (newMode==MODE_IMPORT)   { importPhase=0; importDone=0; importTotal=0; }
    if (newMode==MODE_STONE) {
        if (sdReady) { sdListDir("/"); stoneRebuildAudioIdx(); }
        if (audioReady) { audioStoneInit(); audioStoneSetLoopMode(stoneLoopMode); lp_stoneP2 = pots[1].value; }  // pre-arm P2 pickup — no jump on entry
    }
    if (newMode==MODE_DR2) {
        dr2Playing = false; dr2PlayBeat = dr2PlayStep = dr2PlayMicro = 0;
        dr2SelBeat = dr2SelStep = 0;
        dr2BeatSelMask = 0x1; dr2StepSelMask = 0x1;  // single-select by default — see declaration comment
    }
    if (newMode==MODE_LIGHTPLAY) memset(rippleBrightMap,0,sizeof(rippleBrightMap));
    if (newMode==MODE_SYNTH && audioReady) {
        audioSetShape(currentShape);
        audioSetEnvelope(envTable[currentEnv]);
        audioSetFilter(0.0f, 1.5f);
        audioSetWavefold(1.0f);
    }
    if (newMode==MODE_POKEMON) {
        pkmnApply();
    }
    if (newMode==MODE_MOD2) {
        mod2AlgoApply();
    }
    if (newMode==MODE_MODULAR && audioReady) {
        modOscTable = 0; modOscBTable = 0; modOscAPos = 0.0f; modOscBPos = 0.3f;
        modFocusIdx = 0; modLfoPhase = 0.0f;
        modCutoff = 4000.0f; modReso = 1.5f; modLfoRate = 0.3f; modLfoDepth = 0.0f;
        audioModularOscInit(MOD3_OSCA_CH, modOscTable);
        audioModularOscInit(MOD3_OSCB_CH, modOscBTable);
        audioModularSetWtPos(MOD3_OSCA_CH, modOscAPos);
        audioModularSetWtPos(MOD3_OSCB_CH, modOscBPos);
        audioModularSetFilter(modCutoff, modReso);
        gModSlots[4] = ModSlot{}; // reset to defaults
        gModSlots[4].destKind = MODDEST_MOD_WTPOS_B;
        gModSlots[4].rateHz = modLfoRate;
        gModSlots[4].active = true; // depth starts at 0 (modLfoDepth), so inert until P7 is raised
    }
    if (newMode==MODE_DRUM2) {
        if (!drum2Playing) { drum2Step=0; drum2LastStepMs=millis(); }  // keep step if already playing
        drum2View=1; draniRunning=false; draniBrowse=true;  // always open on sequencer view
        memset(drum2PadFlashMs,   0, sizeof(drum2PadFlashMs));
        memset(drum2DblPendingAt, 0, sizeof(drum2DblPendingAt));
    }
    if (newMode==MODE_SYSEQ) {
        syseqSelStep=0;
        syseqSeqView=false;
        // Skip shape/env re-apply on drill-down — would retrigger AMY mid-note
        if (audioReady && !gestDrillDown) { audioSetShape(currentShape); audioSetEnvelope(envTable[currentEnv]); }
        // Shared clock continues if already playing; will sync on next step
    }
    if (newMode==MODE_303S) {
        s303SelStep=0;
        s303SeqView=false;
        s303CurNote=0;
        // Do NOT reset t303Duration/t303Decay — persist the user's last settings
        for(int _p=0;_p<4;_p++) lp303[_p]=pots[3+_p].value;  // pickup — prevent pot jump from other modes
        if (audioReady) {
            audioT303Init(t303Cutoff, t303Reso, t303EnvMod, t303Decay, t303AmyWave(t303Wave));
            if (t303IsSubOctWave(t303Wave)) {
                audioSW2Init(t303Cutoff, t303Reso, t303Decay, 1, t303AmyWave(t303Wave));
                audioSW2SetBlend(pots[1].value);
            }
        }
        // Shared clock continues if already playing
    }
    if (newMode==MODE_303S2) {
        // Preserve s303s2Seq/Count/Head for continuity — use B4 (clear) to start fresh
        // Do NOT reset t303Duration/t303Decay — persist the user's last settings
        if (gestDrillDown) {
            // Infinite encoders: initialize the encoder position to the stored param value.
            // The user can turn immediately from the correct position — no pickup needed.
            float ref0 = constrain((t303Reso - 1.0f) / 2.0f,                             0.0f, 1.0f);
            float ref1 = constrain(t303EnvMod / 10.0f,                                    0.0f, 1.0f);
            float ref2 = constrain(t303Duration,                                           0.0f, 1.0f);
            float ref3 = constrain(logf(fmaxf(t303Cutoff/80.0f,1.0f))/logf(25.0f),       0.0f, 1.0f);
            float refs[4] = {ref0,ref1,ref2,ref3};
            for(int _p=0;_p<4;_p++){
                pots[3+_p].value = refs[_p];
                pots[3+_p].accum = 0.0f;
                lp303[_p]        = refs[_p];
                lp303DrillDelta[_p] = false;
            }
            // Pre-arm wavefold tracker so pot-1 doesn't fire on the first frame.
            // Pot-1 in GEST is not the 303 wavefold knob — preserve the existing texture.
            g_lp303s2wf = pots[1].value;
        } else {
            for(int _p=0;_p<4;_p++) { lp303DrillDelta[_p]=false; lp303[_p]=pots[3+_p].value; }
        }
        // Skip re-init on drill-down: 303 is already playing with the right params from GEST.
        // Re-calling audioT303Init while a note is playing can cause an audible click.
        if (audioReady && !gestDrillDown) {
            audioT303Init(t303Cutoff,t303Reso,t303EnvMod,t303Decay,t303AmyWave(t303Wave));
            if (t303IsSubOctWave(t303Wave)){ audioSW2Init(t303Cutoff,t303Reso,t303Decay,1,t303AmyWave(t303Wave)); audioSW2SetBlend(pots[1].value); }
        }
    }
    if (newMode==MODE_I303) {
        t303Duration = 0.5f;
        t303Decay = 30.0f * powf(100.0f, 0.5f);
        lp303[2] = pots[5].value;
        if (audioReady) {
            audioI303Init(t303Cutoff, t303Reso, t303EnvMod, t303Decay, t303AmyWave(t303Wave));
            float curP2=pots[1].value;
            if(t303IsSubOctWave(t303Wave)){
                audioSW2Init(t303Cutoff, t303Reso, t303Decay, 6, t303AmyWave(t303Wave));
                audioSW2SetBlend(curP2);
            } else if(t303Wave==T303_PINK_WAVE){
                audioT303Wavefold(0.0f);
            } else {
                if(t303Wave==T303_TRI2_WAVE||t303Wave==T303_SAW2_WAVE) audioT303WavefoldAsym(curP2);
                else if(t303Wave==PULSE)         audioT303Duty(0.5f-curP2*0.48f);
                else if(t303Wave==T303_NAP_WAVE) { audioT303Duty(0.02f); audioT303Wavefold(curP2); }
                else if(t303Wave==T303_SAW3_WAVE){ audioT303Duty(0.5f); audioT303Wavefold(curP2); }
                else                             audioT303Wavefold(curP2);
            }
        }
    }
    if (newMode==MODE_SS2) {
        ss2SelStep=0; ss2SelSlot=0; ss2SeqView=false; ss2CurSlot=0xFF; ss2PlayMode=0;
        if (sdReady) sdListDir(sdPath.length()>1 ? sdPath.c_str() : "/");
        for(uint8_t i=0;i<SS2_SLOTS;i++) ss2Loaded[i] = ss2Path[i][0] && audioKeyLoaded(SS2_KEY_BASE+i);
    }
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
        midiAllNotesOff(midiChannel);
        midiCC(121, 0, midiChannel);  // Reset All Controllers
        midiPitchBend((double)0.0, midiChannel);
    }
#endif
    if (newMode==MODE_EXP && audioReady) {
        memset(expColSel, 0, sizeof(expColSel));
        expColSel[1] = 1;  // Env = NRM
        expColSel[6] = 2;  // Oct = 0
        expFxMask = 0;
        expColSel[7] = 1;  // default: mild texture (X axis active from the start)
        expPosX = 0.5f; expPosY = 0.5f; expVelX = 0.0f; expVelY = 0.0f;
        expNoteOn = false; expCurNote = 60;
        expArpStep = 0; expArpNextMs = 0;
        expLastFilterHz = -1.0f; expLastFoldGain = 1.0f;
        memset(expTrail, 0, sizeof(expTrail)); expTrailHead = 0; expTrailCount = 0;
        audioSetShape(SHAPE_SAW);
        audioSetEnvelope(envTable[ENV_NORMAL]);
        expApplyFx();
        audioSetFilter(0.0f, 1.5f);
    }
    if (newMode==MODE_EXP2 && audioReady) {
        exp2HexAngle  = 0.0f;
        exp2RotVel    = 0.003f;  // slow forward auto-rotation by default
        exp2GravAngle = (float)M_PI / 2.0f;  // display arrow starts pointing down
        exp2Scale     = 0;
        exp2Octave    = 0;
        exp2Bounce    = 0.85f;
        exp2NumSides  = 6;
        exp2SpeedCap  = 6.0f;
        exp2GravStr   = 0.08f;
        exp2FxMask    = 0;
        exp2Shape     = 0;
        exp2EnvIdx    = 0;
        exp2BallCount = 1;
        static const uint8_t kE2ColInit[] = {0,2,0,0,0,2,2,0};
        memcpy(exp2ColSel, kE2ColInit, sizeof(exp2ColSel));
        memset(exp2Balls,        0, sizeof(exp2Balls));
        memset(exp2WallHitMs,    0, sizeof(exp2WallHitMs));
        memset(exp2WallFlash,    0, sizeof(exp2WallFlash));
        memset(exp2WallFlashMs,  0, sizeof(exp2WallFlashMs));
        memset(exp2WallNoteOff,  0, sizeof(exp2WallNoteOff));
        memset(exp2WallNote, 0xFF, sizeof(exp2WallNote));
        exp2ResetBall(0);
        audioSetShape(SHAPE_SAW);
        audioSetEnvelope(envTable[ENV_PLUCK]);
        exp2ApplyFx();
        audioSetFilter(0.0f, 1.5f);
    }
    if (newMode==MODE_EXP3 && audioReady) {
        exp3SpeedMul = 1.0f;
        exp3SpeedVel = 0.0f;
        exp3GateMs   = 200;
        exp3Scale    = 0;
        exp3Octave   = 0;
        exp3FxMask   = 0;
        for (int b=0; b<EXP3_MAX_BALLS; b++) {
            exp3Balls[b].angle     = (float)b * (2.0f*(float)M_PI / (float)EXP3_MAX_BALLS);
            exp3Balls[b].orbitRow  = (uint8_t)(b % EXP3_NUM_ORBITS);
            exp3Balls[b].active    = (b < 4);  // start with 4 balls active (one per orbit)
            exp3Balls[b].triggered = false;
            exp3NoteOffMs[b]  = 0;
            exp3BallNote[b]   = 0xFF;
        }
        // 4 active balls evenly on the 4 orbits
        for (int b=0; b<4; b++) exp3Balls[b].orbitRow = (uint8_t)b;
        audioSetShape(SHAPE_SINE);
        audioSetEnvelope(envTable[ENV_PAD]);
        exp3ApplyFx();
        audioSetFilter(0.0f, 1.5f);
    }
    if (newMode==MODE_LIFE && audioReady) {
        lifeRule = 0; lifeTickDivIdx = 3; lifeScale = 0; lifeEnv = ENV_PLUCK;
        lifePaused = false; lifeShape = SHAPE_PLUCK;
        lifeLastStepMs = millis();
        for (int c=0;c<KBD_COLS;c++) lifeColNote[c] = 0xFF;
        lifeReseed(0.3f);
        audioSetShape(lifeShape);
        audioSetEnvelope(envTable[lifeEnv]);
        audioSetReverb(0.3f, 0.78f, 0.45f, 2000.0f);
        audioSetFilter(0.0f, 1.5f);
    }
    if (newMode==MODE_SWARM && audioReady) {
        swarmHexAngle = 0.0f; swarmRotVel = 0.01f; swarmNumSides = 6;
        swarmFlockBal = 0.5f; swarmSpeedCap = 4.0f; swarmAttractStr = 0.10f; swarmZoneRadius = 10.0f;
        swarmScale = 0; swarmOctave = 0; swarmFxMask = 0;
        for (int b=0; b<SWARM_MAX_BOIDS; b++) {
            swarmResetBoid((uint8_t)b);
            swarmBoids[b].active = (b < 5); // start with 5 of 8 boids active
        }
        audioSetShape(SHAPE_SINE);
        audioSetEnvelope(envTable[ENV_FAST]);
        swarmApplyFx();
        audioSetFilter(0.0f, 1.5f);
    }
    if (newMode==MODE_GEN && audioReady) {
        genGenerator = 0; genVoice = 0; genTickDivIdx = 3; genPaused = false;
        genScale = 0; genOctave = 3; genVoiceParam = 0.5f;
        genWalkPos = 3; genWalkWander = 0.5f;
        genEuclPulses = 3; genEuclStepIdx = 0; genEuclPulsesBuilt = 0xFF;
        genDriftX = 0.42f; genDriftR = 3.7f;
        genLastNote = 0xFF; genLastCol = 0;
        genLastStepMs = millis();
        genApplyVoice();
        audioSetReverb(0.2f, 0.78f, 0.45f, 2000.0f);
    }
    if (newMode==MODE_GEST) {
        // Pickup: don't apply pot values immediately — wait for the pot to actually move
        for(int _p=0;_p<4;_p++) lpGestPots[_p] = pots[3+_p].value;
        // Ensure 303 AMY synth is initialized so 303S2 plays immediately in GEST even if
        // the user never visited MODE_303S or MODE_303S2 first. Skip on drill-return to
        // avoid a re-init glitch while the 303 is already sounding.
        if (audioReady && !gestDrillReturn) {
            audioT303Init(t303Cutoff, t303Reso, t303EnvMod, t303Decay, t303AmyWave(t303Wave));
        }
    }
    // Force immediate display update so the new mode appears without scrMs of old-mode artifact.
    // For drill transitions (GEST ↔ sequencer) skip the blocking wait — the sequencer clock
    // runs in loop() and a 150ms stall would cause the catch-up guard to mis-fire.
    bool drillTransition = gestDrillDown || gestDrillReturn;
    gestDrillReturn = false;
    drawScreen(!drillTransition);
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
static void dispatchMenuItem(MenuItem item) {
    menuOpen = false;
    menuOnTabBar = true;
    switch (item) {
        case MENU_SYNTH:     switchMode(MODE_SYNTH);     break;
        case MENU_OMNI:      switchMode(MODE_OMNI);      break;
        case MENU_SAMPLE:    switchMode(MODE_SAMPLE);    if(sdReady) sdListDir("/"); break;
        case MENU_LIGHT:     switchMode(MODE_LIGHT);     break;
        case MENU_LIGHTPLAY: switchMode(MODE_LIGHTPLAY); break;
        case MENU_SD:        switchMode(MODE_SYSINFO);   break;
        case MENU_ABOUT:     switchMode(MODE_BATTERY);   break;
        case MENU_MOD2:      switchMode(MODE_MOD2);      break;
        case MENU_MODULAR:   switchMode(MODE_MODULAR);   break;
        case MENU_GRANULAR2: switchMode(MODE_GRANULAR2);  break;
        case MENU_MIDI:      switchMode(MODE_MIDI);       break;
        case MENU_TRACKER:   switchMode(MODE_TRACKER);    break;
        case MENU_DRUM2:     switchMode(MODE_DRUM2);       break;
        case MENU_SYSEQ:     switchMode(MODE_SYSEQ);       break;
        case MENU_303S:      switchMode(MODE_303S);         break;
        case MENU_I303:      switchMode(MODE_I303);         break;
        case MENU_SS2:       switchMode(MODE_SS2); if(sdReady) sdListDir("/"); break;
        case MENU_ANIM:      switchMode(MODE_ANIM);  break;
        case MENU_VID:       switchMode(MODE_VID);   break;
        case MENU_LANIM:     switchMode(MODE_LANIM); break;
        case MENU_EXP:       switchMode(MODE_EXP);   break;
        case MENU_EXP2:      switchMode(MODE_EXP2);  break;
        case MENU_EXP3:      switchMode(MODE_EXP3);  break;
        case MENU_303S2:     switchMode(MODE_303S2); break;
        case MENU_POKEMON:   switchMode(MODE_POKEMON); break;
        case MENU_GEST:      switchMode(MODE_GEST);      break;
        case MENU_PCMCLEAN:  switchMode(MODE_PCMCLEAN);  break;
        case MENU_IMPORT:    switchMode(MODE_IMPORT);    break;
        case MENU_STONE:     switchMode(MODE_STONE);     break;
        case MENU_DR2:       switchMode(MODE_DR2);       break;
        case MENU_LIFE:      switchMode(MODE_LIFE);      break;
        case MENU_SWARM:     switchMode(MODE_SWARM);     break;
        case MENU_GEN:       switchMode(MODE_GEN);       break;
        default: break;
    }
}

void selectMenuItem() {
    if (menuOnTabBar) {
        // Enter the items of the current tab
        menuOnTabBar = false; menuRow = 0; menuCol = 0;
        return;
    }
    // In items: dispatch the selected item
    uint8_t idx = menuRow * MENU_COLS + menuCol;
    if (idx >= kMenuCatSizes[menuCategory]) return;
    dispatchMenuItem(kMenuCatItems[menuCategory][idx]);
}

// ==================== MOD2 HELPERS ====================
static float mod2ParamVal(uint8_t pi) {
    const Mod2ParamDef& d = kMod2Algos[mod2AlgoIdx].p[pi];
    return d.mn + (d.mx - d.mn) * mod2P[pi];
}

static void mod2ApplyP4P7() {
    if (!audioReady) return;
    float p[4]; for (int i=0;i<4;i++) p[i]=mod2ParamVal(i);
    uint8_t ai = mod2AlgoIdx;
    switch (ai) {
    case 0: // VCO: Cut, Res, Drv, Dcy
        mod2CurrentCutoff=p[0]; mod2CurrentReso=p[1];
        audioSetFilter(p[0],p[1]); audioSetWavefold(p[2]);
        { EnvParams e=envTable[mod2EnvIdx]; e.dec=(uint16_t)p[3]; audioSetEnvelope(e); }
        break;
    case 1: // DUO: Cut, Res, Sat, Rvb
        mod2CurrentCutoff=p[0]; mod2CurrentReso=p[1];
        audioSetFilter(p[0],p[1]); audioSetWavefold(p[2]);
        audioSetReverb(p[3],0.8f,0.3f,2000.f);
        break;
    case 2: // FM2: Dpt, Rvb, Chr, Drv
        audioSetFmDepth(p[0]);
        audioSetReverb(p[1],0.85f,0.4f,1500.f);
        audioSetChorus(p[2],0.5f,p[2]*0.2f);
        audioSetWavefold(p[3]);
        break;
    case 3: // ACID: Cut, Res, Dcy, Drv
        mod2CurrentCutoff=p[0]; mod2CurrentReso=p[1];
        audioSetFilter(p[0],p[1]);
        { EnvParams e=envTable[mod2EnvIdx]; e.dec=(uint16_t)p[2]; audioSetEnvelope(e); }
        audioSetWavefold(p[3]);
        break;
    case 4: // PAD: Cut, Chr, Rvb, Sat
        mod2CurrentCutoff=p[0]; mod2CurrentReso=0.5f;
        audioSetFilter(p[0],0.5f);
        audioSetChorus(p[1],0.5f,p[1]*0.2f);
        audioSetReverb(p[2],0.85f,0.4f,1500.f);
        audioSetWavefold(1.0f+p[3]);
        break;
    case 5: // PLCK: Brg, Res, Drv, Dcy
        mod2CurrentCutoff=p[0]; mod2CurrentReso=p[1];
        audioSetFilter(p[0],p[1]); audioSetWavefold(p[2]);
        { EnvParams e=envTable[mod2EnvIdx]; e.dec=(uint16_t)p[3]; audioSetEnvelope(e); }
        break;
    case 6: // LEAD: Drv, Cut, Res, Dpt
        mod2CurrentCutoff=p[1]; mod2CurrentReso=p[2];
        audioSetWavefold(p[0]);
        audioSetFilter(p[1],p[2]);
        break;
    default: break;
    }
}

static void mod2AlgoApply() {
    const Mod2AlgoDef& alg = kMod2Algos[mod2AlgoIdx];
    for (int i=0;i<4;i++) { mod2P[i]=alg.p[i].dflt; mod2PCache[i]=-1.0f; }
    mod2PotNeedsSync = true;
    if (!audioReady) return;
    audioAllNotesOff();
    audioSetShape(alg.shape);
    audioSetEnvelope(envTable[mod2EnvIdx]);
    mod2ApplyP4P7();
}

// Apply t303Wave audio parameters after t303Wave has been updated.
static void t303ApplyWave(uint8_t prevWave) {
    if (!audioReady) return;
    if (t303IsSubOctWave(prevWave) && !t303IsSubOctWave(t303Wave)) audioSW2Deactivate();
    audioT303Wave(t303AmyWave(t303Wave));
    audioT303Feedback(0.0f);
    float curP2 = pots[1].value;
    if (t303IsSubOctWave(t303Wave)) {
        audioSW2Init(t303Cutoff, t303Reso, t303Decay, 6, t303AmyWave(t303Wave));
        audioSW2SetBlend(curP2);
    } else if (t303Wave == T303_PINK_WAVE) {
        audioT303Wavefold(0.0f);
    } else {
        if (t303Wave == T303_TRI2_WAVE || t303Wave == T303_SAW2_WAVE) audioT303WavefoldAsym(curP2);
        else if (t303Wave == PULSE) audioT303Wavefold(0.0f);
        else audioT303Wavefold(curP2);
        if (t303Wave == PULSE)          audioT303Duty(0.5f - curP2 * 0.48f);
        if (t303Wave == T303_SAW3_WAVE) audioT303Duty(0.5f);
        if (t303Wave == T303_NAP_WAVE)  audioT303Duty(0.02f);
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

// The FX grid's "short click" action: toggle an FX on/off. Pulled out of
// overlayKeyPress() so both the release handler (short press) and the long-press poll
// can share it without duplicating the logic.
static void overlayFxToggle(uint8_t opt) {
    fxSelected = opt;
    fxList[opt].active = !fxList[opt].active;
    if (fxList[opt].active) {
        fxOrderAdd(opt); fxPotNeedSync = true;
        if (opt == 9) fxList[9].params[0] = 8.0f;  // BITCRS: reset to 8 bits (mild) on activation
        // FILT: don't let the LADDER "special mode" silently persist across a
        // deactivate/reactivate cycle — always come back up as plain LPF, so re-enabling
        // FILT never surprises you with the exotic resonant filter unless you
        // deliberately dial Typ back to LADDER again.
        if (opt == 0) fxList[0].params[3] = 0.0f;
    } else fxOrderRemove(opt);
    // Always use applyAllFx() on toggle so that filter-sharing FX (FILT/DISTORT) don't
    // corrupt each other: applyAllFx resets all inactive then re-applies all active in
    // order, guaranteeing consistent AMY filter state.
    applyAllFx();
    if (opt == 0 && fxList[0].active) lpfSmoothCut = fxList[0].params[0];
    Serial.printf("OVL FX[%d] %s\n", opt, fxList[opt].active?"ON":"OFF");
}

// The FX grid's "long press" action: open the automation editor for an ALREADY-active
// slot — automating an inactive FX makes no sense, so this is a no-op on an inactive one
// (the short-press toggle already fired on press... no: press only arms the timer, so an
// inactive FX held past the threshold just does nothing until release, which then performs
// the normal toggle since s_fxLongPressFired stays false here).
static void overlayFxEnterAutomation(uint8_t opt) {
    if (!fxList[opt].active) return;
    fxSelected = opt;
    s_fxModEditParam = 0;
    s_fxModEditSlot = modSlotFindFxParam(opt, 0);
    s_fxModProfileIdx = -1;
    s_overlay = OVERLAY_FX_MOD;
    Serial.printf("OVL FX_MOD enter fx=%d\n", opt);
}

// Called on key RELEASE (not press) while OVERLAY_FX is open, from the key-event loop —
// performs the deferred short-click toggle if the long-press threshold was never reached.
void overlayFxKeyRelease(uint8_t row, uint8_t col) {
    if (col < 4) return; // not a slot cell in the FX grid
    uint8_t opt = (uint8_t)((3 - row) + (uint8_t)(7 - col) * 4);
    if (opt != s_fxPressOpt) return; // release doesn't match the currently-armed press
    if (!s_fxLongPressFired) overlayFxToggle(opt);
    s_fxPressOpt = 255;
}

void overlayKeyPress(uint8_t row, uint8_t col) {
    if (s_overlayCloseAt) return;

    // FX automation editor: cols 0-3 (note-grid keys) are left alone here so the user can
    // play/preview notes and hear the effect of the modulation while dialing it in — audio
    // for them is handled separately by handleNoteKeyAudio (gated by
    // notePreviewSafeDuringFxOverlay(), same exemption OVERLAY_FX itself uses). Any other
    // key (cols 4-7) closes back to OVERLAY_FX — param selection is via joystick X, not
    // the key grid.
    if (s_overlay == OVERLAY_FX_MOD) {
        if (col < 4) return;
        s_overlay = OVERLAY_FX; s_overlayCloseAt = 0;
        return;
    }

    // INSTR browser: note rows play normally (handled by handleNoteKeyAudio).
    // BTN row closes the browser and applies the selected instrument.
    if (s_overlay == OVERLAY_INSTR) {
        if (row >= KBD_NOTE_ROWS) { s_overlay = OVERLAY_NONE; s_overlayCloseAt = 0; return; }
        return;  // note rows: audio handled separately, no overlay selection logic
    }

    // PKMN browser: any key press selects the highlighted Pokémon and closes
    if (s_overlay == OVERLAY_PKMN) {
        // Find the pkmnBrItem-th Pokémon of pkmnBrType
        uint8_t n=0;
        for (int i=0; i<PKMN_COUNT; i++) {
            if ((uint8_t)kPokemon[i].type == pkmnBrType) {
                if (n == pkmnBrItem) { pkmnSelected=(uint8_t)i; break; }
                n++;
            }
        }
        pkmnApply();
        s_overlay=OVERLAY_NONE; s_overlayCloseAt=0;
        return;
    }

    bool is4col = (s_overlay == OVERLAY_SCALE_ARP || s_overlay == OVERLAY_303 || s_overlay == OVERLAY_303_PRESET || s_overlay == OVERLAY_SYSEQ || s_overlay == OVERLAY_SEQB2 || s_overlay == OVERLAY_FX);
    // FX is a full 4x4 grid (16 cells, FX_COUNT=15 so the last cell is unused) like the
    // other is4col overlays; others use 2 (col6-7)
    uint8_t colMin = is4col ? 4u : 6u;
    if (col < colMin) {
        // OVERLAY_FX: a key outside its selection columns is someone playing a note to
        // hear the effect (now allowed through to handleNoteKeyAudio, see there) — don't
        // close the overlay out from under them just for touching the keybed.
        if (s_overlay != OVERLAY_FX) { s_overlay = OVERLAY_NONE; s_overlayCloseAt = 0; }
        return;
    }

    // colRank: col7→0, col6→1, col5→2, col4→3
    uint8_t opt = (uint8_t)((3 - row) + (uint8_t)(7 - col) * 4);

    switch (s_overlay) {
        case OVERLAY_FX:
            // Deferred: pressing just arms the long-press timer, it doesn't act yet.
            // Releasing before FX_LONGPRESS_MS performs the normal toggle
            // (overlayFxKeyRelease(), called from the key-event loop on release); holding
            // past it opens the automation editor instead (polled in loop(), see
            // s_fxPressOpt) — see overlayFxToggle()/overlayFxEnterAutomation() below.
            if (opt < FX_COUNT) {
                s_fxPressOpt = opt;
                s_fxPressStartMs = millis();
                s_fxLongPressFired = false;
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
                if (currentMode == MODE_I303 && audioReady)
                    audioI303SetAmpEnv((float)envTable[currentEnv].atk,
                                       envTable[currentEnv].sus,
                                       (float)envTable[currentEnv].dec,
                                       (float)envTable[currentEnv].rel);
                if (currentMode == MODE_MODULAR && audioReady)
                    audioModularSetEnvelope(envTable[currentEnv]);
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
            if (opt < 5) {
                samplePlayMode = opt;
                if (opt != 4) { g_sampleSqlCount = g_sampleSqlPlayHead = g_sampleSqlWriteIdx = 0; }
            } else if (opt == 5) {
                for (int r = 0; r < KBD_NOTE_ROWS; r++)
                    for (int c = 0; c < KBD_COLS; c++) sampleMap[r][c] = "";
                audioClearAllKeys();
                g_sampleSqlCount = g_sampleSqlPlayHead = g_sampleSqlWriteIdx = 0;
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
                if (fxList[opt].active) {
                    fxOrderAdd(opt); fxPotNeedSync = true;
                    if (opt == 9) fxList[9].params[0] = 8.0f;  // BITCRS: reset to 8 bits on activation
                    // FILT: don't let the LADDER "special mode" silently persist across a
                    // deactivate/reactivate cycle — see the OVERLAY_FX toggle path above.
                    if (opt == 0) fxList[0].params[3] = 0.0f;
                } else fxOrderRemove(opt);
                applyAllFx();
                if (opt == 0 && fxList[0].active) lpfSmoothCut = fxList[0].params[0];
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
        case OVERLAY_I303_WAVE: {
            if (i303WaveBr < kI303WaveCount) {
                uint8_t prevWave = t303Wave;
                t303Wave = kI303WaveList[i303WaveBr];
                audioAllNotesOff();
                t303ApplyWave(prevWave);
            }
            break;
        }
        case OVERLAY_MOD2_ALGO: {
            if (opt < MOD2_ALGO_COUNT) {
                mod2AlgoIdx = opt;
                mod2AlgoApply();
            }
            break;
        }
        default: break;
    }
    // Show selection feedback for 200ms before closing
    s_overlayCloseAt = millis() + 200;
}

// ==================== SYNTH TEMPLATE ====================
// Shared button layout for every "synth-like" instrument mode (plays notes across the
// keyboard with one active timbre): B1=FX, B2=Scale/Arp, B3=Env, B4=preset/instrument list.
// B1 and B2 are wired globally in handleButton (search "template default"); B4 is wired via
// the two functions below. To add a new synth-template mode:
//   1. Add it to the B1/B2 global mode lists in handleButton.
//   2. Add a case here returning its preset-browser overlay type, and another in
//      synthTemplateInitPresetBrowser seeding that browser's cursor to the mode's current
//      preset (so opening it doesn't jump to an unrelated item).
// No other button-handling boilerplate should be needed — B1/B2/B4 dispatch automatically.
// A mode with no discrete preset enum (e.g. MODE_STONE, which browses SD files continuously
// via joystick instead of picking from a fixed list) returns OVERLAY_NONE here and keeps its
// own B4 binding in the per-mode switch below.
static OverlayType synthTemplatePresetOverlay(AppMode m) {
    switch (m) {
        case MODE_SYNTH:   return OVERLAY_INSTR;
        case MODE_POKEMON: return OVERLAY_PKMN;
        case MODE_I303:    return OVERLAY_I303_WAVE;
        default:           return OVERLAY_NONE;
    }
}

static void synthTemplateInitPresetBrowser(AppMode m) {
    switch (m) {
        case MODE_SYNTH:
            for (uint8_t c = 0; c < INSTR_CAT_COUNT; c++) {
                const InstrCategory &cat = instrCategories[c];
                if ((uint8_t)currentShape >= cat.start && (uint8_t)currentShape < cat.start + cat.count) {
                    instrBrCat = c; instrBrItem = (uint8_t)currentShape - cat.start;
                    break;
                }
            }
            break;
        case MODE_POKEMON: {
            pkmnBrType = (uint8_t)kPokemon[pkmnSelected].type;
            pkmnBrItem = 0;
            uint8_t n = 0;
            for (int i = 0; i < PKMN_COUNT; i++) {
                if (kPokemon[i].type == kPokemon[pkmnSelected].type) {
                    if (i == pkmnSelected) { pkmnBrItem = n; break; }
                    n++;
                }
            }
            break;
        }
        case MODE_I303:
            i303WaveBr = 0;
            for (uint8_t i = 0; i < kI303WaveCount; i++) if (kI303WaveList[i] == t303Wave) { i303WaveBr = i; break; }
            break;
        default: break;
    }
}

// ==================== BUTTON HANDLER ====================
void handleButton(uint8_t rawBtn, bool pressed) {
    uint8_t btn = 3 - rawBtn;  // raw3=btn0(left), raw0=btn3(right)
    Serial.printf("BTN raw=%d mapped=%d %s\n", rawBtn, btn, pressed?"DN":"UP");

    // Any button press while instrument browser is open: close and apply selection
    if (s_overlay == OVERLAY_INSTR && pressed) {
        s_overlay = OVERLAY_NONE; s_overlayCloseAt = 0;
        return;
    }

    if (btn==0) {
        if (pressed) { btn1PressTime=millis(); btn1Handled=false; }
        else {
            if (!btn1Handled && (millis()-btn1PressTime<600)) {
                if (menuOpen) {
                    if (!menuOnTabBar) { menuOnTabBar = true; menuRow = 0; menuCol = 0; }
                    else { menuOpen = false; }
                }
                else switch(currentMode) {
                    case MODE_SYNTH:
                    case MODE_POKEMON:
                    case MODE_I303:
                    case MODE_STONE:
                    case MODE_OMNI:
                    case MODE_MODULAR: {  // newly wired in — was unreachable from this mode before
                        OverlayType old = s_overlay; s_overlay = OVERLAY_NONE; s_overlayCloseAt = 0;
                        if (old != OVERLAY_FX) s_overlay = OVERLAY_FX;
                        break;
                    }
                    case MODE_VID: {
                        if (mediaAudioPlaying) {
                            audioStopSamplePreset(PCM_PREVIEW_PRESET); mediaAudioPlaying=false;
                            sdListDir(sdPath.c_str(), isMediaFile);
                        } else if (vidPlaying) {
                            vidFile.close(); vidFileOpen=false; vidPlaying=false;
                            sdListDir(sdPath.c_str(), isMediaFile);
                        } else if (sdPath == "/") {
                            audioAllNotesOff(); menuOpen=true; menuRow=0; menuCol=0; menuOnTabBar=true;
                        } else {
                            int ls=sdPath.lastIndexOf('/',sdPath.length()-2);
                            sdListDir(ls<=0?"/":sdPath.substring(0,ls+1), isMediaFile);
                        }
                        break;
                    }
                    case MODE_MIDI:
#if CONFIG_TINYUSB_MIDI_ENABLED
                        if (midiPotSel >= 0) {
                            midiPotSel = -1;  // exit pot config mode
                        } else {
                            midiAllNotesOff(midiChannel);
                            midiChannel = (midiChannel % 16) + 1;  // 1→2→...→16→1
                            midiLastPB = -999.f; midiPitchBend((double)0.0, midiChannel);
                        }
#endif
                        break;
                    case MODE_SAMPLE:
                        // Back / go up one directory; at root → open main menu
                        if (sdPath == "/") { audioAllNotesOff(); menuOpen=true; menuRow=0; menuCol=0; menuOnTabBar=true; }
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
                        if (drum2View == 2 && draniBrowse) {
                            // B1 in anim browse = navigate up / return to menu
                            if (sdPath == "/") { audioAllNotesOff(); menuOpen=true; menuRow=0; menuCol=0; menuOnTabBar=true; }
                            else { int ls=sdPath.lastIndexOf('/',sdPath.length()-2); sdListDir(ls<=0?"/":sdPath.substring(0,ls+1), isVidOrImgFile); }
                        } else {
                            drum2Playing = !drum2Playing;
                            if (drum2Playing) { drum2Step=0; drum2LastStepMs=millis(); s303s2Step=0; s303s2LastMs=millis(); }
                            else {
                                drum2RecArmed=false;
                                for(uint8_t i=0;i<syseqActiveCnt;i++) audioNoteOff(syseqActive[i]); syseqActiveCnt=0; syseqActiveIsFull=false;
                                memset(drum2DblPendingAt,0,sizeof(drum2DblPendingAt));
                                if (s303CurNote) { audioT303NoteOff(s303CurNote); s303CurNote=0; }
                                if (s303s2CurNote) { audioT303NoteOff(s303s2CurNote); s303s2CurNote=0; }
                                t303SlideActive=false; audioT303PitchBend(1.0f); ss2CurSlot=0xFF;
                            }
                        }
                        break;
                    case MODE_DR2: {
                        // B1 double-click: ensure playing + arm quantized live-record (any
                        // pad/note hit while playing gets written into the nearest beat.step.
                        // micro slot, see dr2RecordNearestSlot()). B1 single-click while armed
                        // just disarms recording, keeping playback going; otherwise it's the
                        // normal play/stop toggle. Same 350ms double-click window as B3/B4.
                        static uint32_t _dr2B1Last = 0;
                        uint32_t _now = millis();
                        bool isDbl = (_now - _dr2B1Last) < 350;
                        _dr2B1Last = isDbl ? 0 : _now;
                        if (isDbl) {
                            if (!dr2Playing) {
                                dr2Playing = true;
                                dr2PlayBeat = dr2PlayStep = dr2PlayMicro = 0;
                                dr2LastMicroMs = millis();
                            }
                            dr2RecArmed = true;
                        } else if (dr2RecArmed) {
                            dr2RecArmed = false;
                        } else {
                            dr2Playing = !dr2Playing;
                            if (dr2Playing) {
                                dr2PlayBeat = dr2PlayStep = dr2PlayMicro = 0;
                                dr2LastMicroMs = millis();
                            } else {
                                dr2RecArmed = false;
                                memset(dr2DblPendingAt, 0, sizeof(dr2DblPendingAt));
                            }
                        }
                        break;
                    }
                    case MODE_303S:
                        drum2Playing = !drum2Playing;
                        if (drum2Playing) { drum2Step=0; drum2LastStepMs=millis(); }
                        else {
                            if (s303CurNote) { audioT303NoteOff(s303CurNote); s303CurNote=0; }
                            audioT303PitchBend(1.0f); t303SlideActive=false;
                            memset(drum2DblPendingAt,0,sizeof(drum2DblPendingAt));
                        }
                        break;
                    case MODE_303S2:
                        drum2Playing = !drum2Playing;
                        if (drum2Playing) { drum2Step=0; drum2LastStepMs=millis(); s303s2Step=0; s303s2LastMs=millis(); }
                        else { if (s303s2CurNote) { audioT303NoteOff(s303s2CurNote); s303s2CurNote=0; }
                               for(uint8_t i=0;i<syseqActiveCnt;i++) audioNoteOff(syseqActive[i]); syseqActiveCnt=0; syseqActiveIsFull=false;
                               if(s303CurNote){audioT303NoteOff(s303CurNote);s303CurNote=0;} t303SlideActive=false; audioT303PitchBend(1.0f); ss2CurSlot=0xFF; }
                        break;
                    case MODE_GEST:
                        drum2Playing = !drum2Playing;
                        if (drum2Playing) { drum2Step=0; drum2LastStepMs=millis(); s303s2Step=0; s303s2LastMs=millis(); }
                        else { if(s303s2CurNote){audioT303NoteOff(s303s2CurNote);s303s2CurNote=0;}
                               for(uint8_t i=0;i<syseqActiveCnt;i++) audioNoteOff(syseqActive[i]); syseqActiveCnt=0; syseqActiveIsFull=false;
                               if(s303CurNote){audioT303NoteOff(s303CurNote);s303CurNote=0;} t303SlideActive=false; audioT303PitchBend(1.0f); ss2CurSlot=0xFF; }
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
                    case MODE_LIFE:
                        // Pause/resume the automaton tick — held notes and the current grid
                        // state are left exactly as they are (lifeStep() itself decides
                        // note-on/off on birth/death edges, so simply not calling it freezes
                        // both the pattern and whatever's currently sounding).
                        lifePaused = !lifePaused;
                        break;
                    case MODE_GEN:
                        genPaused = !genPaused;
                        break;
                    default: break;
                }
            }
            btn1PressTime=0;
        }
        return;
    }
    if (!pressed && !(currentMode==MODE_SAMPLE&&(samplePlayMode==1||samplePlayMode==2)) && currentMode!=MODE_MIDI) return;

    // B2 (btn==1): scale/arp overlay — template default for all synth-like modes
    if (btn == 1 && pressed) {
        if (currentMode==MODE_SYNTH || currentMode==MODE_POKEMON || currentMode==MODE_I303 || currentMode==MODE_STONE) {
            OverlayType old=s_overlay; s_overlay=OVERLAY_NONE; s_overlayCloseAt=0;
            if (old!=OVERLAY_SCALE_ARP) s_overlay=OVERLAY_SCALE_ARP;
            return;
        }
    }
    // B4 (btn==3): preset/instrument list — template default for all synth-like modes that
    // have a discrete preset enum (see synthTemplatePresetOverlay). Modes without one (e.g.
    // MODE_STONE) fall through to their own B4 binding in the switch below.
    if (btn == 3 && pressed) {
        OverlayType pOvl = synthTemplatePresetOverlay(currentMode);
        if (pOvl != OVERLAY_NONE) {
            OverlayType old = s_overlay; s_overlay = OVERLAY_NONE; s_overlayCloseAt = 0;
            if (old != pOvl) { s_overlay = pOvl; synthTemplateInitPresetBrowser(currentMode); }
            return;
        }
    }

    switch(currentMode) {
        case MODE_SYNTH:
            if (btn==2) { OverlayType old=s_overlay; s_overlay=OVERLAY_NONE; s_overlayCloseAt=0; if(old!=OVERLAY_ENV) s_overlay=OVERLAY_ENV; }
            break;
        case MODE_STONE:
            // B3 single-click toggles NRM/LOOP directly (was double-click, with a
            // single-click panic-stop fallback — dropped the panic-stop binding since
            // double-clicking a small key reliably enough to always land within the
            // detection window was the actual complaint).
            if (btn==2) {
                stoneLoopMode = !stoneLoopMode;
                audioStoneSetLoopMode(stoneLoopMode);
            }
            if (btn==3) noteMap.nextOctave();
            break;
        case MODE_POKEMON:
            if (btn==2) { OverlayType old=s_overlay; s_overlay=OVERLAY_NONE; s_overlayCloseAt=0; if(old!=OVERLAY_ENV) s_overlay=OVERLAY_ENV; }
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
        case MODE_DRUM2:
            // B1(btn0)=Play handled in the btn==0 block above (must reach second switch via btn==1,2,3)
            if (btn==1) {  // B2: FX overlay
                OverlayType old=s_overlay; s_overlay=OVERLAY_NONE; s_overlayCloseAt=0;
                if (old!=OVERLAY_FX) s_overlay=OVERLAY_FX;
            }
            if (btn==2) {  // B3: Rec (pad/seq) or folder browser toggle (anim)
                if (drum2View == 2) {
                    if (draniBrowse) { draniBrowse = false; }
                    else { draniBrowse = true; if (sdReady) sdListDir(sdPath.c_str(), isVidOrImgFile); }
                } else {
                    drum2RecArmed = !drum2RecArmed;
                }
            }
            if (btn==3) {  // B4: cycle views: pad → seq → anim → pad
                drum2View = (drum2View + 1) % 3;
                if (drum2View == 2 && sdReady && !draniRunning) sdListDir(sdPath.c_str(), isVidOrImgFile);
            }
            break;
        case MODE_DR2:
            // B1(btn0)=Play handled in the btn==0 block above
            if (btn==1) {  // B2: FX overlay
                OverlayType old=s_overlay; s_overlay=OVERLAY_NONE; s_overlayCloseAt=0;
                if (old!=OVERLAY_FX) s_overlay=OVERLAY_FX;
            }
            if (btn==2) {  // B3: single-click = LOOP/LIVE toggle (like GEST) — LIVE
                           // auto-advances to the next filled pattern at each bar boundary.
                           // Double-click = open the octave/scale overlay, same
                           // OVERLAY_SCALE_ARP menu the synth template modes use (B2 there);
                           // B3 is double-clicked here since single-click already owns
                           // LOOP/LIVE. Key-grid presses while it's open are handled
                           // generically by overlayKeyPress()/its OLED renderer — no DR2-
                           // specific wiring needed beyond opening it.
                static uint32_t _dr2B3Last = 0;
                uint32_t _now = millis();
                bool isDbl = (_now - _dr2B3Last) < 350;
                _dr2B3Last = isDbl ? 0 : _now;
                if (isDbl) {
                    OverlayType old = s_overlay; s_overlay = OVERLAY_NONE; s_overlayCloseAt = 0;
                    if (old != OVERLAY_SCALE_ARP) s_overlay = OVERLAY_SCALE_ARP;
                } else {
                    dr2PlayMode = (dr2PlayMode==DR2_LOOP) ? DR2_LIVE : DR2_LOOP;
                }
            }
            if (btn==3 && pressed) {  // B4: copy/paste/clear, same semantics as GEST
                static uint32_t _dr2B4Last = 0;
                uint32_t _now = millis();
                bool isDbl = (_now - _dr2B4Last) < 350;
                _dr2B4Last = isDbl ? 0 : _now;
                if (isDbl) {
                    // Double-click anywhere: clear the active pattern (live + stored) and clipboard.
                    dr2Copied = false;
                    memset(dr2Vel, 0, sizeof(dr2Vel));
                    memset(dr2Mod, 0, sizeof(dr2Mod));
                    memset(dr2SynthVel, 0, sizeof(dr2SynthVel));
                    memset(dr2T303Vel, 0, sizeof(dr2T303Vel));
                    dr2PatFilled[dr2ActivePat] = false;
                } else if (dr2PatFilled[dr2ActivePat]) {
                    // Filled: always (re-)copy — sync the stored slot with the live buffer first
                    // in case edits happened since the last slot switch.
                    memcpy(dr2Pats[dr2ActivePat], dr2Vel, sizeof(dr2Vel));
                    memcpy(dr2ModPats[dr2ActivePat], dr2Mod, sizeof(dr2Mod));
                    memcpy(dr2SynthPats[dr2ActivePat], dr2SynthVel, sizeof(dr2SynthVel));
                    memcpy(dr2T303Pats[dr2ActivePat], dr2T303Vel, sizeof(dr2T303Vel));
                    dr2Copied = true; dr2CopyPat = dr2ActivePat;
                } else if (dr2Copied) {
                    // Empty: paste from clipboard — pasting only works onto an empty pattern;
                    // clear a filled one first (double-click) to overwrite it.
                    memcpy(dr2Vel, dr2Pats[dr2CopyPat], sizeof(dr2Vel));
                    memcpy(dr2Mod, dr2ModPats[dr2CopyPat], sizeof(dr2Mod));
                    memcpy(dr2SynthVel, dr2SynthPats[dr2CopyPat], sizeof(dr2SynthVel));
                    memcpy(dr2T303Vel, dr2T303Pats[dr2CopyPat], sizeof(dr2T303Vel));
                    memcpy(dr2Pats[dr2ActivePat], dr2Vel, sizeof(dr2Vel));
                    memcpy(dr2ModPats[dr2ActivePat], dr2Mod, sizeof(dr2Mod));
                    memcpy(dr2SynthPats[dr2ActivePat], dr2SynthVel, sizeof(dr2SynthVel));
                    memcpy(dr2T303Pats[dr2ActivePat], dr2T303Vel, sizeof(dr2T303Vel));
                    dr2PatFilled[dr2ActivePat] = true;
                }
            }
            break;
        case MODE_I303:
            if (btn==2) { OverlayType old=s_overlay; s_overlay=OVERLAY_NONE; s_overlayCloseAt=0; if(old!=OVERLAY_ENV) s_overlay=OVERLAY_ENV; }
            break;
        case MODE_303S2:
            if (btn==1) { OverlayType old2=s_overlay; s_overlay=OVERLAY_NONE; s_overlayCloseAt=0; if(old2!=OVERLAY_FX) s_overlay=OVERLAY_FX; }
            if (btn==2) { t303AccentOn = !t303AccentOn; }
            if (btn==3) { // B4: clear sequence
                memset(s303s2Seq,0,sizeof(s303s2Seq)); s303s2Head=0; s303s2Count=0; s303s2Step=0;
                if (s303s2CurNote){ audioT303NoteOff(s303s2CurNote); s303s2CurNote=0; }
            }
            break;
        case MODE_GEST:
            // B1 (play/stop) handled in the btn==0 block above
            if (btn==1) { OverlayType og=s_overlay; s_overlay=OVERLAY_NONE; s_overlayCloseAt=0; if(og!=OVERLAY_FX) s_overlay=OVERLAY_FX; }
            if (btn==2) { gestPlayMode = (gestPlayMode==GEST_LOOP) ? GEST_LIVE : GEST_LOOP; }
            if (btn==3 && pressed) {
                static uint32_t _b4Last = 0;
                uint32_t _now = millis();
                bool isDbl = (_now - _b4Last) < 350;
                _b4Last = isDbl ? 0 : _now;
                if (isDbl) {
                    // Double-click anywhere: clear the selected pattern (stored + live state)
                    // AND clear the B4 clipboard.
                    gestCopied = false;
                    uint8_t si = gestSelRow, pa = gestSelCol;
                    g_patFilled[si][pa] = false;
                    if (si==0) {
                        memset(g_drumPats[pa].vel,0,sizeof(g_drumPats[pa].vel));
                        memset(g_drumPats[pa].row,0,sizeof(g_drumPats[pa].row));
                        memset(g_drumPats[pa].mod,0,sizeof(g_drumPats[pa].mod));
                        memset(g_drumPats[pa].pit,0,sizeof(g_drumPats[pa].pit));
                        if (pa==gestActPat[0]) { memset(drum2SeqVel,0,sizeof(drum2SeqVel)); memset(drum2SeqRow,0,sizeof(drum2SeqRow)); memset(drum2SeqMod,0,sizeof(drum2SeqMod)); memset(drum2SeqPitchOff,0,sizeof(drum2SeqPitchOff)); }
                    } else if (si==1) {
                        memset(g_303sPats[pa].seq,0,sizeof(g_303sPats[pa].seq)); g_303sPats[pa].count=0; g_303sPats[pa].head=0;
                        if (pa==gestActPat[1]) { memset(s303s2Seq,0,sizeof(s303s2Seq)); s303s2Count=0; s303s2Head=0; s303s2Step=0; if(s303s2CurNote){audioT303NoteOff(s303s2CurNote);s303s2CurNote=0;} }
                    } else if (si==2) {
                        memset(g_synsPats[pa].notes,0,sizeof(g_synsPats[pa].notes)); memset(g_synsPats[pa].vels,0,sizeof(g_synsPats[pa].vels)); memset(g_synsPats[pa].alt,0,sizeof(g_synsPats[pa].alt));
                        if (pa==gestActPat[2]) { for(uint8_t i=0;i<syseqActiveCnt;i++) audioNoteOff(syseqActive[i]); syseqActiveCnt=0; syseqActiveIsFull=false; memset(syseqNotes,0,sizeof(syseqNotes)); memset(syseqVels,0,sizeof(syseqVels)); memset(syseqAlt,0,sizeof(syseqAlt)); }
                    } else {
                        memset(g_sampsPats[pa].note,0,sizeof(g_sampsPats[pa].note)); memset(g_sampsPats[pa].alt,0,sizeof(g_sampsPats[pa].alt));
                        if (pa==gestActPat[3]) { memset(ss2Note,0,sizeof(ss2Note)); memset(ss2Alt,0,sizeof(ss2Alt)); }
                    }
                } else if (g_patFilled[gestSelRow][gestSelCol]) {
                    // Filled pattern: always (re-)copy it — this is the source, not a paste target.
                    gestCopied = true; gestCopySeq = gestSelRow; gestCopyPat = gestSelCol;
                } else if (gestCopied && gestCopySeq == gestSelRow) {
                    // Empty pattern with a matching clipboard: paste. Pasting is only possible
                    // onto an empty slot — clear a filled one first (double-click) to overwrite it.
                    uint8_t si = gestSelRow, dst = gestSelCol, src = gestCopyPat;
                    if (si==0) {
                        g_drumPats[dst] = g_drumPats[src];
                        if (dst==gestActPat[0]) { memcpy(drum2SeqVel,g_drumPats[dst].vel,sizeof(drum2SeqVel)); memcpy(drum2SeqRow,g_drumPats[dst].row,sizeof(drum2SeqRow)); memcpy(drum2SeqMod,g_drumPats[dst].mod,sizeof(drum2SeqMod)); memcpy(drum2SeqPitchOff,g_drumPats[dst].pit,sizeof(drum2SeqPitchOff)); }
                    } else if (si==1) {
                        g_303sPats[dst] = g_303sPats[src];
                        if (dst==gestActPat[1]) {
                            memcpy(s303s2Seq,g_303sPats[dst].seq,sizeof(s303s2Seq)); s303s2Count=g_303sPats[dst].count; s303s2Head=g_303sPats[dst].head;
                            uint8_t qL=(s303s2Count<=1)?1:(s303s2Count<=2)?2:(s303s2Count<=4)?4:(s303s2Count<=8)?8:16;
                            s303s2Step=(uint8_t)(drum2Step%qL);
                            s303s2CurNote=0;
                        }
                    } else if (si==2) {
                        g_synsPats[dst] = g_synsPats[src];
                        if (dst==gestActPat[2]) {
                            for(uint8_t i=0;i<syseqActiveCnt;i++) audioNoteOff(syseqActive[i]);
                            syseqActiveCnt=0; syseqActiveIsFull=false;
                            memcpy(syseqNotes,g_synsPats[dst].notes,sizeof(syseqNotes)); memcpy(syseqVels,g_synsPats[dst].vels,sizeof(syseqVels)); memcpy(syseqAlt,g_synsPats[dst].alt,sizeof(syseqAlt));
                            if (drum2Playing) {
                                for(uint8_t i=0;i<SYSEQ_CHORD;i++) {
                                    if(!syseqNotes[drum2Step][i]) continue;
                                    uint8_t n=syseqNotes[drum2Step][i]-1;
                                    float sv=syseqVels[drum2Step][i]/127.0f*gestSeqVol[2];
                                    audioNoteOn(n, sv);
                                    if(syseqActiveCnt<SYSEQ_CHORD) syseqActive[syseqActiveCnt++]=n;
                                }
                                syseqActiveIsFull=(syseqAlt[drum2Step]==1);
                            }
                        }
                    } else {
                        g_sampsPats[dst] = g_sampsPats[src];
                        if (dst==gestActPat[3]) { memcpy(ss2Note,g_sampsPats[dst].note,sizeof(ss2Note)); memcpy(ss2Alt,g_sampsPats[dst].alt,sizeof(ss2Alt)); }
                    }
                    g_patFilled[si][dst] = g_patFilled[si][src];
                }
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
            if (btn==2) {  // B3: SYSEQ options overlay
                OverlayType old=s_overlay; s_overlay=OVERLAY_NONE; s_overlayCloseAt=0;
                if(old!=OVERLAY_SYSEQ) s_overlay=OVERLAY_SYSEQ;
            }
            if (btn==3) {  // B4: clear all recorded notes
                for(uint8_t i=0;i<syseqActiveCnt;i++) audioNoteOff(syseqActive[i]);
                syseqActiveCnt=0; syseqActiveIsFull=false;
                memset(syseqNotes, 0, sizeof(syseqNotes));
                memset(syseqVels,  0, sizeof(syseqVels));
                memset(syseqAlt,   0, sizeof(syseqAlt));
            }
            break;
        case MODE_MOD2:
            if (btn==1) { OverlayType old=s_overlay; s_overlay=OVERLAY_NONE; s_overlayCloseAt=0; if(old!=OVERLAY_FX) s_overlay=OVERLAY_FX; }
            if (btn==2) { OverlayType old=s_overlay; s_overlay=OVERLAY_NONE; s_overlayCloseAt=0; if(old!=OVERLAY_ENV) s_overlay=OVERLAY_ENV; }
            if (btn==3) { OverlayType old=s_overlay; s_overlay=OVERLAY_NONE; s_overlayCloseAt=0; if(old!=OVERLAY_MOD2_ALGO) s_overlay=OVERLAY_MOD2_ALGO; }
            if (btn==4) { mod2PlayMode=(Mod2PlayMode)((mod2PlayMode+1)%MOD2_PLAY_COUNT); }
            break;
        case MODE_MODULAR:
            // B4 octave was dead code (`btn==4` can never fire — handleButton()'s
            // `btn=3-rawBtn` only ever produces 0-3, the exact same pre-existing bug
            // already fixed once for MOD2/STONE elsewhere in this file); fixed to
            // `btn==3`, matching the working STONE precedent.
            if (btn==3) noteMap.nextOctave();
            // B2: envelope overlay — was entirely unwired (B2/B3 did nothing, matching
            // template SYNTH's usual B2=ENV binding used by SYNTH/POKEMON/MOD2).
            if (btn==1) { OverlayType old=s_overlay; s_overlay=OVERLAY_NONE; s_overlayCloseAt=0; if(old!=OVERLAY_ENV) s_overlay=OVERLAY_ENV; }
            // B3: cycle Osc B's wavetable independently of Osc A's (P2) — a real second
            // control rather than a redundant echo of P2, and true to the Serum-style
            // "two independently-selected wavetable oscillators" idea this mode is based on.
            if (btn==2) {
                modOscBTable = (uint8_t)((modOscBTable+1)%5);
                if (audioReady) audioModularSetTable(MOD3_OSCB_CH, modOscBTable);
            }
            break;
        case MODE_GRANULAR2:
            // Btn2: cycle play mode NRM→LOP→FUL→SEQ→SQL→NRM
            if (btn==1) {
                // Stop SEQ/SQL if leaving them
                if (gran2PlayMode == 3) { audioStopGranular2(GRAN2_SEQ_OSC); g_gran2SeqHead = g_gran2SeqTail = 0; }
                if (gran2PlayMode == 4) { audioStopGranular2(GRAN2_SEQ_OSC); g_gran2SqlCount = g_gran2SqlPlayHead = g_gran2SqlWriteIdx = 0; }
                gran2PlayMode = (gran2PlayMode + 1) % 5;
                gran2ActiveSample = -1; gran2ActiveSlice = -1; gran2PotNeedsSync = true;
                // Re-register presets with correct sample_length for the new play mode:
                // LOP needs extended length (SYNTH_OFF prevention); NRM/SEQ need exact slice length.
                for (uint8_t s = 0; s < GRAN2_MAX_SAMPLES; s++) {
                    if (gran2[s].computed)
                        audioApplyGranular2Splits(s, gran2[s].splits, gran2[s].sliceCount, gran2PlayMode == 1);
                }
            }
            // Btn3: cycle load target slot
            if (btn==2) gran2LoadTarget = (gran2LoadTarget + 1) % gran2NumSamples();
            // Btn4: automap if no samples loaded, else clear all
            if (btn==3) {
                bool anyLoaded = false;
                for (uint8_t s = 0; s < GRAN2_MAX_SAMPLES; s++)
                    if (gran2[s].loaded || !gran2[s].path.isEmpty()) { anyLoaded = true; break; }
                if (!anyLoaded && sdReady) {
                    // Automap: fill slots 0-3 with first audio files in current SD folder
                    uint8_t slot = 0;
                    uint8_t numSamp = gran2NumSamples();
                    for (int fi = 0; fi < sdFileCount && slot < numSamp; fi++) {
                        if (sdFiles[fi] == ".." || sdFileIsDir[fi]) continue;
                        if (!isAudioFile(sdFiles[fi].c_str())) continue;
                        String fp = sdPath;
                        if (!fp.endsWith("/")) fp += "/";
                        fp += sdFiles[fi];
                        gran2[slot].path     = fp;
                        gran2[slot].loaded   = false;
                        gran2[slot].computed = false;
                        gran2[slot].sliceCount = 0;
                        audioLoadGranular2Source(fp.c_str(), slot);
                        slot++;
                    }
                    gran2LoadTarget = 0; gran2ActiveSample = -1; gran2ActiveSlice = -1; gran2PotNeedsSync = true;
                } else {
                    // Clear all loaded samples
                    for (uint8_t r = 0; r < KBD_NOTE_ROWS; r++) for (uint8_t c = 0; c < KBD_COLS; c++)
                        audioStopGranular2((uint8_t)(r * KBD_COLS + c));
                    audioStopGranular2(GRAN2_SEQ_OSC);
                    g_gran2SeqHead = g_gran2SeqTail = 0;
                    g_gran2SqlCount = g_gran2SqlPlayHead = g_gran2SqlWriteIdx = 0;
                    for (uint8_t s = 0; s < GRAN2_MAX_SAMPLES; s++) {
                        if (gran2[s].loaded) audioUnloadGranular2Slot(s);
                        gran2[s] = Gran2State{};
                    }
                    gran2ActiveSample = -1; gran2ActiveSlice = -1;
                    gran2LoadTarget = 0; gran2PotNeedsSync = true;
                }
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
                if (btn==1) midiCC(64, pressed ? 127 : 0, midiChannel);
                if (btn==2 && !pressed && midiOctave>0) { midiOctave--; midiAllNotesOff(midiChannel); }
                if (btn==3 && !pressed && midiOctave<9) { midiOctave++; midiAllNotesOff(midiChannel); }
                if (btn==4 && pressed) { midiAllNotesOff(midiChannel); }
            }
#endif
            break;
        case MODE_ANIM:
            if (btn==1) animIdx = (uint8_t)((animIdx + 1) % 5);
            if (btn==3) animIdx = (uint8_t)((animIdx + 4) % 5);
            break;
        case MODE_LANIM:
            if (btn==1) lanimIdx = (uint8_t)((lanimIdx + 1) % 5);
            if (btn==3) lanimIdx = (uint8_t)((lanimIdx + 4) % 5);
            break;
        case MODE_PCMCLEAN:
            if (btn==0 && pcmCleanPhase==0 && sdReady) {
                pcmCleanPhase=1; pcmCleanDeleted=0; pcmCleanScanned=0; pcmCleanRunning=true;
            }
            break;
        case MODE_IMPORT:
#ifdef __ANDROID__
            if (btn==0 && importPhase==0) { androidPickFolder(); importPhase=1; }
#endif
            break;
        case MODE_EXP:
            if (btn==0) {  // B1: reset modifiers
                memset(expColSel, 0, sizeof(expColSel));
                expColSel[1]=1; expColSel[6]=2;
                expFxMask=0;
                if (audioReady) { audioSetShape(SHAPE_SAW); audioSetEnvelope(envTable[ENV_NORMAL]); expApplyFx(); audioSetFilter(0.0f,1.5f); }
            }
            if (btn==2) {  // B3: octave down
                if (expColSel[6] > 0) expColSel[6]--;
            }
            if (btn==3) {  // B4: octave up
                if (expColSel[6] < 3) expColSel[6]++;
            }
            break;
        case MODE_EXP2:
            if (btn==0) {  // B1: random kick impulse on all balls
                for (int b=0;b<EXP2_MAX_BALLS;b++) {
                    if (!exp2Balls[b].active) continue;
                    exp2Balls[b].vx += ((float)random(-100,100)) / 40.0f;
                    exp2Balls[b].vy += ((float)random(-100,100)) / 40.0f;
                }
            }
            if (btn==1) {  // B2: reverse polygon rotation direction
                exp2RotVel = -exp2RotVel;
            }
            if (btn==2) {  // B3: remove one ball
                if (exp2BallCount > 1) {
                    exp2Balls[--exp2BallCount].active = false;
                    exp2ColSel[2] = exp2BallCount - 1;
                }
            }
            if (btn==3) {  // B4: add one ball
                if (exp2BallCount < EXP2_MAX_BALLS) {
                    exp2ResetBall(exp2BallCount++);
                    exp2ColSel[2] = exp2BallCount - 1;
                }
            }
            break;
        case MODE_EXP3:
            if (btn==0) {  // B1: reset to 4 active balls (one per orbit)
                for (int b=0;b<EXP3_MAX_BALLS;b++) {
                    if (exp3BallNote[b]!=0xFF && audioReady) audioNoteOff(exp3BallNote[b]);
                    exp3Balls[b].angle     = (float)b * (2.0f*(float)M_PI/(float)EXP3_MAX_BALLS);
                    exp3Balls[b].orbitRow  = (uint8_t)(b % EXP3_NUM_ORBITS);
                    exp3Balls[b].active    = (b < 4);
                    exp3Balls[b].triggered = false;
                    exp3NoteOffMs[b] = 0; exp3BallNote[b] = 0xFF;
                }
                exp3SpeedMul = 1.0f; exp3SpeedVel = 0.0f;
            }
            if (btn==1) {  // B2: cycle scale
                exp3Scale = (exp3Scale + 1) % 4;
            }
            if (btn==2) {  // B3: octave down
                if (exp3Octave > -2) exp3Octave--;
            }
            if (btn==3) {  // B4: octave up
                if (exp3Octave < 1) exp3Octave++;
            }
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
        pots[i].rawDelta = 0.0f;
        if (fabsf(pots[i].accum)<0.5f) continue;
        float pMax = (i == 0) ? 2.0f : 1.0f;  // pot0 = volume, goes to 200%
        float d = pots[i].accum*0.01f;
        pots[i].rawDelta = d;  // unclamped — see EncPot's comment
        pots[i].value=constrain(pots[i].value+d,0.0f,pMax);
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

static TaskHandle_t    s_displayTaskHandle = nullptr;
static SemaphoreHandle_t s_oledDone        = nullptr; // given by displayTask after sendBuffer

// Draw XBM sprite at 2× scale (each source pixel → 2×2 box)
static void drawXBMScaled2x(int16_t x0, int16_t y0, const uint8_t* xbm, uint8_t srcW, uint8_t srcH) {
    uint8_t bytesPerRow = (srcW + 7) / 8;
    for (uint8_t sy = 0; sy < srcH; sy++) {
        for (uint8_t bx = 0; bx < bytesPerRow; bx++) {
            uint8_t b = pgm_read_byte(xbm + sy * bytesPerRow + bx);
            for (uint8_t bit = 0; bit < 8 && (bx * 8 + bit) < srcW; bit++) {
                if (b & (1 << bit)) {
                    oled.drawBox(x0 + (int16_t)(bx * 8 + bit) * 2,
                                 y0 + (int16_t)sy * 2, 2, 2);
                }
            }
        }
    }
}

// Draw a small type icon at (cx,cy) center for Pokémon display (16×16 box)
static void drawPkmnTypeIcon(int16_t cx, int16_t cy, PokemonType type) {
    int16_t x0 = cx - 8, y0 = cy - 8;
    switch(type) {
        case PKMN_FIRE:     // flame triangle (3 lines)
            oled.drawLine(cx,y0,x0,y0+15);
            oled.drawLine(x0,y0+15,x0+15,y0+15);
            oled.drawLine(x0+15,y0+15,cx,y0);
            oled.drawLine(cx,y0+5,x0+4,y0+15);
            oled.drawLine(x0+4,y0+15,x0+11,y0+15);
            oled.drawLine(x0+11,y0+15,cx,y0+5);
            break;
        case PKMN_WATER:    // drop
            oled.drawCircle(cx,cy+2,6);
            oled.drawLine(cx,y0,x0+2,cy);
            oled.drawLine(x0+2,cy,x0+13,cy);
            oled.drawLine(x0+13,cy,cx,y0);
            break;
        case PKMN_GRASS:    // cross/leaf
            oled.drawLine(cx,y0,cx,y0+15);
            oled.drawLine(x0,cy,x0+15,cy);
            oled.drawLine(x0+3,y0+3,x0+12,y0+12);
            break;
        case PKMN_ELECTRIC: // lightning bolt
            oled.drawLine(cx+3,y0,cx-3,cy);
            oled.drawLine(cx-3,cy,cx+3,cy);
            oled.drawLine(cx+3,cy,cx-3,y0+15);
            break;
        case PKMN_ICE:      // snowflake (cross + diagonals)
            oled.drawLine(cx,y0,cx,y0+15);
            oled.drawLine(x0,cy,x0+15,cy);
            oled.drawLine(x0+2,y0+2,x0+13,y0+13);
            oled.drawLine(x0+13,y0+2,x0+2,y0+13);
            break;
        case PKMN_FIGHTING: // fist (filled box with notch)
            oled.drawBox(x0+1,cy-2,13,8);
            oled.drawBox(x0+1,y0+1,7,6);
            break;
        case PKMN_GHOST:    // ghost silhouette
            oled.drawCircle(cx,cy-1,6);
            oled.drawBox(x0+1,cy-1,14,7);
            oled.drawLine(x0+1,cy+6,x0+3,cy+4);
            oled.drawLine(x0+3,cy+4,x0+5,cy+6);
            oled.drawLine(x0+5,cy+6,x0+8,cy+4);
            oled.drawLine(x0+8,cy+4,x0+11,cy+6);
            oled.drawLine(x0+11,cy+6,x0+13,cy+4);
            oled.drawLine(x0+13,cy+4,x0+14,cy+6);
            break;
        case PKMN_PSYCHIC:  // eye / spiral
            oled.drawCircle(cx,cy,6);
            oled.drawCircle(cx,cy,3);
            oled.drawPixel(cx,cy);
            break;
        case PKMN_DRAGON:   // diamond
            oled.drawLine(cx,y0,x0+15,cy);
            oled.drawLine(x0+15,cy,cx,y0+15);
            oled.drawLine(cx,y0+15,x0,cy);
            oled.drawLine(x0,cy,cx,y0);
            break;
        case PKMN_POISON:   // skull (circle + crossbones)
            oled.drawCircle(cx,cy-2,5);
            oled.drawLine(x0,cy+3,x0+15,cy+10);
            oled.drawLine(x0,cy+10,x0+15,cy+3);
            break;
        default:            // NORMAL: plain circle
            oled.drawCircle(cx,cy,6);
            oled.drawCircle(cx,cy,4);
            break;
    }
}

void drawScreen(bool blockWait) {
    // blockWait=false (normal): skip frame if displayTask is busy — never block the main loop.
    // blockWait=true (mode-switch): wait up to 150ms; under heavy AMY load sendBuffer can
    // take ~70ms+ due to preemption between I2C chunks — 150ms gives ample margin.
    // NEVER proceed without holding s_oledDone or we'd start a second sendBuffer.
    if (s_oledDone) {
        TickType_t tmo = blockWait ? pdMS_TO_TICKS(150) : 0;
        if (xSemaphoreTake(s_oledDone, tmo) == pdFALSE) return;  // always skip if take fails
    }
    oled.clearBuffer();
    oled.setDrawColor(1);  // reset: previous frames may leave drawColor=0 after inverted-text draws

    // ---- FULL-SCREEN OVERLAY ----
    // INSTR: categorized instrument browser — left column = categories, right = items
    if (s_overlay == OVERLAY_INSTR) {
        oled.setFont(u8g2_font_4x6_tf);
        // Left column: categories (32px wide, each cat 16px tall, 8 cats = 128px)
        for (int c = 0; c < INSTR_CAT_COUNT; c++) {
            int y = c * 16;
            bool selCat = (c == instrBrCat);
            if (selCat) { oled.drawBox(0, y, 32, 15); oled.setDrawColor(0); }
            else        oled.drawFrame(0, y, 32, 15);
            oled.drawStr(2, y + 10, instrCategories[c].name);
            if (selCat) oled.setDrawColor(1);
        }
        // Right column: items in selected category (96px wide, 5 visible, scrolled)
        const InstrCategory &cat = instrCategories[instrBrCat];
        uint8_t maxVis = 8;  // up to 8 items visible (16px each = 128px)
        uint8_t scroll = (instrBrItem >= maxVis) ? (instrBrItem - maxVis + 1) : 0;
        for (uint8_t i = 0; i < maxVis && (i + scroll) < cat.count; i++) {
            uint8_t idx = scroll + i;
            uint8_t shapeIdx = cat.start + idx;
            bool sel = (idx == instrBrItem);
            bool cur = (shapeIdx < SHAPE_COUNT && (SynthShape)shapeIdx == currentShape);
            int y = i * 16;
            if (sel) { oled.drawBox(34, y, 94, 15); oled.setDrawColor(0); }
            else if (cur) oled.drawRBox(34, y, 94, 15, 2);
            else          oled.drawFrame(34, y, 94, 15);
            if (shapeIdx < SHAPE_COUNT) oled.drawStr(37, y + 10, shapeNames[shapeIdx]);
            if (sel || cur) oled.setDrawColor(1);
        }
        oled.drawVLine(33, 0, 128);
        if (s_displayTaskHandle) xTaskNotifyGive(s_displayTaskHandle);
        return;
    }

    // FX automation editor: FX name + which param, current depth/rate/shape/sync, and a
    // live phase dot so the user can see the LFO actually running while they dial it in.
    if (s_overlay == OVERLAY_FX_MOD) {
        FxEffect &fx = fxList[fxSelected];
        oled.setFont(u8g2_font_6x10_tf);
        char hdr[24]; snprintf(hdr, sizeof(hdr), "%s > %s", fx.name, fx.paramNames[s_fxModEditParam]);
        oled.drawStr(2, 11, hdr);
        oled.drawHLine(0, 14, 128);

        oled.setFont(u8g2_font_4x6_tf);
        bool assigned = (s_fxModEditSlot >= 0 && gModSlots[s_fxModEditSlot].active);
        ModSlot dummy; ModSlot &s = assigned ? gModSlots[s_fxModEditSlot] : dummy;
        char l1[24]; snprintf(l1, sizeof(l1), "Shape: %s", kModShapeName[s.shape]);
        oled.drawStr(2, 26, l1);
        char l2[24];
        if (s.bpmSync) snprintf(l2, sizeof(l2), "Rate: %s (BPM)", kDelaySubdivName[s.bpmDivIdx]);
        else           snprintf(l2, sizeof(l2), "Rate: %.1fHz", s.rateHz);
        oled.drawStr(2, 36, l2);
        char l3[24]; snprintf(l3, sizeof(l3), "Depth: %d%%", (int)(s.depth*100.0f));
        oled.drawStr(2, 46, l3);
        char l4[24];
        if (s_fxModProfileIdx >= 0) snprintf(l4, sizeof(l4), "Profile: %s", kLfoProfiles[s_fxModProfileIdx].name);
        else                        snprintf(l4, sizeof(l4), assigned ? "Profile: custom" : "Profile: --");
        oled.drawStr(2, 56, l4);

        // Live phase indicator: horizontal track + moving dot, only while assigned+active.
        oled.drawFrame(4, 70, 120, 8);
        if (assigned) {
            float t = s.phase / (2.0f * (float)PI);
            int x = 5 + (int)(t * 117.0f);
            oled.drawBox(x, 71, 3, 6);
        }
        oled.drawStr(2, 88, "Joy X: param  Joy Y: profile");
        oled.drawStr(2, 98, assigned ? "Keys: play notes" : "P4: raise depth");
        oled.drawStr(2, 108, "B1: back to FX");

        if (s_displayTaskHandle) xTaskNotifyGive(s_displayTaskHandle);
        return;
    }

    if (s_overlay == OVERLAY_I303_WAVE) {
        oled.setFont(u8g2_font_6x10_tf);
        const uint8_t rowH = 10;
        const uint8_t maxVis = 128 / rowH;
        uint8_t sc = (i303WaveBr >= maxVis) ? (i303WaveBr - maxVis + 1) : 0;
        oled.drawStr(1, 9, "Wave:");
        oled.drawHLine(0, 10, 128);
        for (uint8_t i = 0; i < kI303WaveCount; i++) {
            if (i < sc || i >= sc + maxVis) continue;
            int y = (int)(i - sc) * rowH + 11;
            bool sel = (i == i303WaveBr);
            bool cur = (kI303WaveList[i] == t303Wave);
            if (sel) { oled.drawBox(0, y, 128, rowH-1); oled.setDrawColor(0); }
            else if (cur) { oled.drawFrame(0, y, 128, rowH-1); }
            oled.drawStr(4, y + rowH - 2, t303WaveName(kI303WaveList[i]));
            if (sel) oled.setDrawColor(1);
        }
        if (s_displayTaskHandle) xTaskNotifyGive(s_displayTaskHandle);
        return;
    }

    // PKMN browser: left = types, right = Pokémon of selected type
    if (s_overlay == OVERLAY_PKMN) {
        oled.setFont(u8g2_font_4x6_tf);
        // Count types that have at least one Pokémon
        const uint8_t nTypes = (uint8_t)PKMN_TYPE_COUNT;
        const uint8_t typeH  = 8;  // pixels per type row
        const uint8_t maxTypeVis = 128 / typeH;

        // Scroll so selected type stays visible
        uint8_t tScroll = (pkmnBrType >= maxTypeVis) ? (pkmnBrType - maxTypeVis + 1) : 0;
        for (uint8_t t = 0; t < nTypes; t++) {
            // Check if this type has any Pokémon
            bool hasPkmn = false;
            for (int i=0; i<PKMN_COUNT && !hasPkmn; i++) hasPkmn |= ((uint8_t)kPokemon[i].type == t);
            if (!hasPkmn) continue;
            if (t < tScroll || t >= tScroll + maxTypeVis) continue;
            int y = (int)(t - tScroll) * typeH;
            bool sel = (t == pkmnBrType);
            if (sel) { oled.drawBox(0, y, 38, typeH-1); oled.setDrawColor(0); }
            else      oled.drawFrame(0, y, 38, typeH-1);
            oled.drawStr(1, y + typeH - 2, kPkmnTypeNames[t]);
            if (sel) oled.setDrawColor(1);
        }
        oled.drawVLine(39, 0, 128);

        // Right: Pokémon of selected type
        uint8_t itemH = 16;
        uint8_t maxItemVis = 128 / itemH;
        uint8_t iScroll = (pkmnBrItem >= maxItemVis) ? (pkmnBrItem - maxItemVis + 1) : 0;
        uint8_t n = 0;
        for (int i = 0; i < PKMN_COUNT; i++) {
            if ((uint8_t)kPokemon[i].type != pkmnBrType) continue;
            if (n < iScroll || n >= iScroll + maxItemVis) { n++; continue; }
            int y = (int)(n - iScroll) * itemH;
            bool sel = (n == pkmnBrItem);
            bool cur = ((uint8_t)i == pkmnSelected);
            if (sel)      { oled.drawBox(41, y, 87, itemH-1); oled.setDrawColor(0); }
            else if (cur) { oled.drawFrame(41, y, 87, itemH-1); }
            oled.drawStr(43, y + 11, kPokemon[i].name);
            if (sel) oled.setDrawColor(1);
            n++;
        }
        oled.setDrawColor(1);
        if (s_displayTaskHandle) xTaskNotifyGive(s_displayTaskHandle);
        return;
    }

    // 2-col overlays (FX, ENV, SEQ_OPT, SAMP_OPT): 2×4 grid, 64×32px par case
    // 4-col overlays (SCALE_ARP, 303, 303_PRESET):  4×4 grid, 32×32px par case
    if (s_overlay != OVERLAY_NONE) {
        struct OvBox { char l1[12]; char l2[14]; char l3[14]; bool avail, sel; };
        bool is4col = (s_overlay == OVERLAY_SCALE_ARP || s_overlay == OVERLAY_303 || s_overlay == OVERLAY_303_PRESET || s_overlay == OVERLAY_SYSEQ || s_overlay == OVERLAY_SEQB2 || s_overlay == OVERLAY_FX);
        OvBox bx[16];
        memset(bx, 0, sizeof(bx));

        switch (s_overlay) {
            case OVERLAY_FX:
                for (int i = 0; i < (int)FX_COUNT; i++) {
                    OvBox &b = bx[i]; b.avail = true; b.sel = fxList[i].active;
                    strncpy(b.l1, (i==0) ? fxFiltTypName() : fxList[i].name, sizeof(b.l1)-1);
                    const FxEffect &fx = fxList[i];
                    // Collect indices of visible params (non-empty name)
                    int vp[4]; int vpc = 0;
                    for (int p = 0; p < 4; p++) if (fx.paramNames[p][0]) vp[vpc++] = p;
                    if (vpc == 0) continue;
                    // Special case: FILT — Cut:Res sur l2, Typ sur l3
                    if (i == 0) {
                        char cv[8]; fmtFloat(cv, sizeof(cv), fx.params[0]);
                        char rv[8]; fmtFloat(rv, sizeof(rv), fx.params[1]);
                        snprintf(b.l2, sizeof(b.l2), "Cu:%s Re:%s", cv, rv);
                        snprintf(b.l3, sizeof(b.l3), "Ty:%s", fxFiltTypName());
                        continue;
                    }
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
                static const char* sd[] = {"normal","static","loop","slow","seq loop"};
                for (int i = 0; i < 5; i++) {
                    bx[i].avail = true; bx[i].sel = ((uint8_t)samplePlayMode == i);
                    strncpy(bx[i].l1, kSplayModes[i], sizeof(bx[i].l1)-1);
                    strncpy(bx[i].l2, sd[i], sizeof(bx[i].l2)-1);
                }
                bx[5].avail = true; strncpy(bx[5].l1, "Clr", sizeof(bx[5].l1)-1);
                strncpy(bx[5].l2, "samples", sizeof(bx[5].l2)-1);
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
                for (int i = 0; i < (int)FX_COUNT; i++) {
                    bx[i].avail = true;
                    bx[i].sel   = fxList[i].active;
                    if (i == 0) {
                        strncpy(bx[i].l1, fxFiltTypName(), sizeof(bx[i].l1)-1);
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
            case OVERLAY_MOD2_ALGO: {
                for (uint8_t i = 0; i < MOD2_ALGO_COUNT; i++) {
                    bx[i].avail = true;
                    bx[i].sel   = (i == mod2AlgoIdx);
                    strncpy(bx[i].l1, kMod2Algos[i].name, sizeof(bx[i].l1)-1);
                    snprintf(bx[i].l2, sizeof(bx[i].l2), "%s/%s",
                             kMod2Algos[i].p[0].name, kMod2Algos[i].p[1].name);
                    snprintf(bx[i].l3, sizeof(bx[i].l3), "%s/%s",
                             kMod2Algos[i].p[2].name, kMod2Algos[i].p[3].name);
                }
                break;
            }
            default: break;
        }

        // Render: 4-col → 32px, 2-col → 64px cells
        int ncols = is4col ? 4 : 2;
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
        if (s_displayTaskHandle) xTaskNotifyGive(s_displayTaskHandle);
        return;
    }

    oled.setFont(u8g2_font_5x7_tf);
    char buf[40];

    if (menuOpen) {
        // ---- Tab bar (always visible) ----
        const uint8_t TW=42, TH=10;
        for (uint8_t t=0;t<3;t++) {
            uint8_t x=t*TW;
            bool active=(menuCategory==t);
            if (active && menuOnTabBar) {
                // Cursor is on this tab: invert
                oled.drawBox(x,0,TW,TH-1);
                oled.setDrawColor(0);
                int tw=oled.getStrWidth(kMenuCatNames[t]);
                oled.drawStr(x+(TW-tw)/2,TH-2,kMenuCatNames[t]);
                oled.setDrawColor(1);
            } else if (active) {
                // Active tab, cursor in items: underline
                oled.drawHLine(x,TH-1,TW);
                int tw=oled.getStrWidth(kMenuCatNames[t]);
                oled.drawStr(x+(TW-tw)/2,TH-2,kMenuCatNames[t]);
            } else {
                int tw=oled.getStrWidth(kMenuCatNames[t]);
                oled.drawStr(x+(TW-tw)/2,TH-2,kMenuCatNames[t]);
            }
        }
        oled.drawHLine(0,TH,128);
        // ---- Items grid ----
        const uint8_t CW=42,CH=26,MY=TH+2,visRows=4;
        uint8_t catSize=kMenuCatSizes[menuCategory];
        uint8_t catRows=(catSize+MENU_COLS-1)/MENU_COLS;
        uint8_t scroll=(!menuOnTabBar&&menuRow>=visRows)?menuRow-visRows+1:0;
        for (uint8_t vr=0;vr<visRows;vr++) {
            uint8_t r=scroll+vr; if(r>=catRows) break;
            for (uint8_t c=0;c<MENU_COLS;c++) {
                uint8_t idx=r*MENU_COLS+c; if(idx>=catSize) continue;
                MenuItem item=kMenuCatItems[menuCategory][idx];
                uint8_t x=c*CW+1,y=MY+vr*CH;
                bool cur=(!menuOnTabBar&&r==menuRow&&c==menuCol);
                if(cur){oled.drawRFrame(x,y,CW-2,CH-2,3);oled.drawFrame(x+2,y+2,CW-6,CH-6);}
                else oled.drawFrame(x,y,CW-2,CH-2);
                int tw=oled.getStrWidth(menuLabels[item]);
                oled.drawStr(x+(CW-2-tw)/2,y+(CH-2)/2,menuLabels[item]);
            }
        }
    } else {
        if (currentMode != MODE_POKEMON) {
            float battV=s_battVSmooth;
            snprintf(buf,sizeof(buf),"%.1fV",battV);
            oled.drawStr(108,0,buf); oled.drawHLine(0,9,128);
        }

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
                    const char* fxdname = (fxSelected==0) ? fxFiltTypName() : fx.name;
                    if(fx.active){
                        snprintf(buf,sizeof(buf),"FX:%s %s:%.1f %s:%.1f",
                                 fxdname,
                                 fx.paramNames[0],fx.params[0],
                                 fx.paramNames[1][0]?fx.paramNames[1]:"",
                                 fx.paramNames[1][0]?fx.params[1]:0.0f);
                    } else {
                        snprintf(buf,sizeof(buf),"FX:%s [OFF]",fxdname);
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
                        char nm[4]; strncpy(nm, (i==0)?fxFiltTypName():fxList[i].name, 3); nm[3]='\0';
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
                            char fv[6];
                            if(fxSelected==0&&p==3) strncpy(fv,fxFiltTypName(),sizeof(fv)-1);
                            else fmtFloat(fv,sizeof(fv),af.params[p]);
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
                            {
                            const char* sfxdn = (fxSelected==0) ? fxFiltTypName() : selFx.name;
                            char fxLine[33]; snprintf(fxLine, sizeof(fxLine), "[%.3s]", sfxdn);
                            for(int p = 0; p < 4; p++){
                                if(!selFx.paramNames[p][0]) continue;
                                char fv[6];
                                if(fxSelected==0&&p==3) strncpy(fv,fxFiltTypName(),sizeof(fv)-1);
                                else fmtFloat(fv,sizeof(fv),selFx.params[p]);
                                char ent[9]; snprintf(ent, sizeof(ent), " %.2s:%.4s", selFx.paramNames[p], fv);
                                if(strlen(fxLine) + strlen(ent) < sizeof(fxLine) - 1)
                                    strcat(fxLine, ent);
                            }
                            oled.drawStr(0, 111, fxLine);
                            } // end FILT block
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
                        char fxstr[20] = ""; uint8_t nfx = 0;
                        for(uint8_t fi = 0; fi < FX_COUNT; fi++){
                            if(!fxList[fi].active) continue;
                            if(nfx++) strncat(fxstr, " ", sizeof(fxstr) - strlen(fxstr) - 1);
                            char a[4];
                            if(fi==0){ strncpy(a, fxFiltTypName(), sizeof(a)-1); a[3]='\0'; }
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
            // ---- STONE ---- (layout deliberately mirrors MODE_GRANULAR2's: waveform+window
            // up top, a status-chip row, a numeric readout, then the file browser at the same
            // 7-row/y=58 cadence, then an FX-or-hints footer — see that case for the source
            // of each proportion below.)
            case MODE_STONE: {
                oled.setFont(u8g2_font_4x6_tf);
                {
                    char fn[20] = "----";
                    if (stoneLoadedPath.length() > 0) {
                        int ls = stoneLoadedPath.lastIndexOf('/');
                        String f = ls >= 0 ? stoneLoadedPath.substring(ls + 1) : stoneLoadedPath;
                        snprintf(fn, sizeof(fn), "%.18s", f.c_str());
                    }
                    snprintf(buf, sizeof(buf), "STONE[%s] %s", stoneLoopMode ? "LOOP" : "NRM", fn);
                }
                oled.drawStr(0, 7, buf); oled.drawHLine(0, 9, 128);

                // Waveform (y=10..32, height 23px) + start/end window frame — same geometry
                // as GRANULAR2's waveform+active-slice-frame.
                if (stoneWin.computed) {
                    for (int x = 0; x < 128; x++) {
                        uint8_t h = (uint8_t)(stoneWin.waveform[x] * 21 / 255);
                        if (h < 1) h = 1;
                        oled.drawVLine(x, 32 - h, h);
                    }
                    int sx0 = (int)(stoneWin.start * 127.f);
                    int sx1 = (int)(stoneWin.end   * 127.f);
                    oled.drawVLine(sx0, 10, 23);
                    oled.drawVLine(sx1, 10, 23);
                    oled.drawFrame(sx0, 10, (sx1 - sx0) > 0 ? (sx1 - sx0) : 1, 23);
                }

                // Status chip row (y=36..44) — GRANULAR2 shows one chip per sample slot (S0-S3);
                // STONE only ever has one sample, so the chip shows loop on/off instead, plus
                // ready/loading/empty status the same way GRANULAR2's chips do (dot/circle).
                oled.drawStr(0, 43, "M:");
                oled.drawBox(11, 36, 25, 8);
                if (stoneLoopMode) { oled.setDrawColor(0); oled.drawStr(12, 43, "LOOP"); oled.setDrawColor(1); }
                else               { oled.setDrawColor(0); oled.drawStr(12, 43, "NRM");  oled.setDrawColor(1); }
                if (audioIsStoneReady())            oled.drawPixel(40, 45);
                else if (stoneLoadedPath.length())  oled.drawCircle(40, 44, 2, U8G2_DRAW_ALL);
                snprintf(buf, sizeof(buf), "Oct%+d", noteMap.getOctave());
                oled.drawStr(48, 43, buf);

                // Window readout (y=51) — same slot as GRANULAR2's "SlN S0:x% S1:y%" line.
                oled.setFont(u8g2_font_5x7_tf);
                snprintf(buf, sizeof(buf), "St:%d%% En:%d%% Ln:%d%%",
                         (int)(stoneWin.start * 100.f), (int)(stoneWin.end * 100.f),
                         (int)((stoneWin.end - stoneWin.start) * 100.f));
                oled.drawStr(0, 51, buf);
                oled.setFont(u8g2_font_4x6_tf);

                // File browser (7 entries from y=58 — GRANULAR2's exact play-phase cadence).
                if (!sdReady) {
                    oled.drawStr(10, 70, "SD not found");
                } else {
                    for (int i = 0; i < 7 && (i + sdScroll) < sdFileCount; i++) {
                        int idx = i + sdScroll; bool sel = (idx == sdCursor);
                        bool isLoadedFile = !sdFileIsDir[idx] && stoneLoadedPath.endsWith(sdFiles[idx].c_str());
                        char line[29];
                        if (sdFileIsDir[idx]) snprintf(line, sizeof(line), "%c[%.24s]", sel ? '>' : ' ', sdFiles[idx].c_str());
                        else snprintf(line, sizeof(line), "%c%s%.25s", sel ? '>' : ' ', isLoadedFile ? "*" : "", sdFiles[idx].c_str());
                        oled.drawStr(0, 58 + i * 7, line);
                        if (sel) oled.drawHLine(0, 59 + i * 7, 128);
                    }
                }

                // Bottom: FX params when any FX is active, else button hints — identical
                // pattern/positions to GRANULAR2's own footer block.
                {
                    bool anyFxDisp = false;
                    for (uint8_t fi = 0; fi < FX_COUNT; fi++) if (fxList[fi].active) { anyFxDisp = true; break; }
                    if (anyFxDisp) {
                        const FxEffect& fx = fxList[fxSelected];
                        struct { const char* name; char val[8]; } pp[4]; int np = 0;
                        for (int p = 0; p < 4; p++) {
                            if (!fx.paramNames[p][0]) continue;
                            if (fxSelected == 0 && p == 3) {
                                strncpy(pp[np].val, fxFiltTypName(), sizeof(pp[np].val)-1);
                            } else {
                                float v = fx.params[p];
                                if      (v >= 1000.f) snprintf(pp[np].val, 8, "%.0fk", v / 1000.f);
                                else if (v >= 10.f)   snprintf(pp[np].val, 8, "%.0f",  v);
                                else                  snprintf(pp[np].val, 8, "%.2f",  v);
                            }
                            pp[np].name = fx.paramNames[p]; np++;
                        }
                        char l1[32] = {}, l2[32] = {};
                        const char* fxdn = (fxSelected==0) ? fxFiltTypName() : fx.name;
                        int o1 = snprintf(l1, sizeof(l1), "%s%s:", fx.active ? "*" : "-", fxdn);
                        for (int i = 0; i < np && i < 2; i++)
                            o1 += snprintf(l1 + o1, sizeof(l1) - o1, " %.3s=%s", pp[i].name, pp[i].val);
                        oled.drawStr(0, 111, l1);
                        if (np > 2) {
                            int o2 = 0;
                            for (int i = 2; i < np; i++)
                                o2 += snprintf(l2 + o2, sizeof(l2) - o2, " %.3s=%s", pp[i].name, pp[i].val);
                            oled.drawStr(0, 118, l2 + 1);
                        } else {
                            oled.drawStr(0, 118, "JY=browse Clk=load B3=Loop");
                        }
                    } else {
                        oled.drawStr(0, 118, "JY=browse Clk=load B3=Loop");
                    }
                }
                break;
            }
            // ---- SEQUENCER ----
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
                float battV = s_battVSmooth;
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
            // ---- DIAG — system diagnostics ----
            case MODE_SYSINFO: {
                float battV2 = s_battVSmooth;
                float pct2   = constrain((battV2 - 6.0f) / 2.4f * 100.0f, 0.0f, 100.0f);
                oled.setFont(u8g2_font_4x6_tf);
                oled.drawStr(0,0, "[ GrvEP v2 // DIAG   ]");
                oled.drawStr(0,8, "======================");
                snprintf(buf,sizeof(buf),"BATT: %.2fV %3.0f%% [%s]",
                         battV2,pct2,battV2>8.0f?"FULL":pct2>60.0f?"OK":pct2>25.0f?"LOW":"!!!!");
                oled.drawStr(0,16,buf);
                {
                    uint32_t psramFree=(uint32_t)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)/1024);
                    uint32_t psramTotal=(uint32_t)(ESP.getPsramSize()/1024);
                    uint32_t heapFree=(uint32_t)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)/1024);
                    snprintf(buf,sizeof(buf),"RAM:%lu/%lukB  HEAP:%lukB",psramFree,psramTotal,heapFree);
                    oled.drawStr(0,24,buf);
                }
                {
                    uint32_t cpuMHz = (uint32_t)ESP.getCpuFreqMHz();
                    snprintf(buf,sizeof(buf),"CPU:%luMHz  BPM:%d",cpuMHz,bpm);
                    oled.drawStr(0,32,buf);
                }
                if (sdReady) {
                    uint32_t sdTotalMB = (uint32_t)(SD.totalBytes() / (1024*1024));
                    uint32_t sdUsedMB  = (uint32_t)(SD.usedBytes()  / (1024*1024));
                    snprintf(buf,sizeof(buf),"SD:%luMB  used:%luMB",sdTotalMB,sdUsedMB);
                } else {
                    snprintf(buf,sizeof(buf),"SD: not mounted");
                }
                oled.drawStr(0,40,buf);
                snprintf(buf,sizeof(buf),"FX:%d/%d  Vol:%.0f%%  %s",
                         fxOrderCount,(int)FX_COUNT,volume*100,shapeNames[currentShape]);
                oled.drawStr(0,48,buf);
                oled.drawStr(0,56,"======================");
                static bool blink2=false; static uint32_t lastBlink2=0;
                if(millis()-lastBlink2>500){blink2=!blink2;lastBlink2=millis();}
                snprintf(buf,sizeof(buf),"> UP:%lus%s",(unsigned long)(millis()/1000),blink2?" _":"");
                oled.drawStr(0,64,buf);
                oled.setFont(u8g2_font_5x7_tf);
                oled.drawStr(20,127,"Click = menu");
                break;
            }

            // ---- PCMCLEAN ----
            case MODE_PCMCLEAN: {
                oled.setFont(u8g2_font_5x7_tf);
                oled.drawStr(0,7,"PURGE CACHE .PCM");
                oled.drawHLine(0,10,128);
                oled.setFont(u8g2_font_4x6_tf);
                if (pcmCleanPhase == 0) {
                    oled.drawStr(0,26,"Supprime tous les fichiers");
                    oled.drawStr(0,34,".pcm et .pcm16 de la SD.");
                    oled.drawStr(0,46,"Les sources WAV/MP3 sont");
                    oled.drawStr(0,54,"conservees.");
                    oled.drawHLine(0,62,128);
                    oled.setFont(u8g2_font_5x7_tf);
                    oled.drawStr(0,75,"B1 = CONFIRMER");
                    oled.drawStr(0,87,"Click = annuler");
                } else if (pcmCleanPhase == 1) {
                    oled.drawStr(0,26,"Suppression en cours...");
                    snprintf(buf,sizeof(buf),"Scannes : %lu", pcmCleanScanned);
                    oled.drawStr(0,40,buf);
                    snprintf(buf,sizeof(buf),"Supprimes: %lu", pcmCleanDeleted);
                    oled.drawStr(0,50,buf);
                    // Animated spinner
                    static uint8_t spin=0; static uint32_t spinMs=0;
                    if(millis()-spinMs>150){spin=(spin+1)%4;spinMs=millis();}
                    const char* spinCh[]={"   |","   /","   -","   \\"};
                    oled.drawStr(80,26,spinCh[spin]);
                } else {
                    oled.drawStr(0,26,"Terminé !");
                    snprintf(buf,sizeof(buf),"Scannes : %lu", pcmCleanScanned);
                    oled.drawStr(0,40,buf);
                    snprintf(buf,sizeof(buf),"Supprimes: %lu", pcmCleanDeleted);
                    oled.drawStr(0,50,buf);
                    oled.drawStr(0,67,"Click = menu");
                }
                break;
            }

            // ---- IMPORT (Android only) ----
            case MODE_IMPORT: {
                oled.setFont(u8g2_font_5x7_tf);
                oled.drawStr(0,7,"IMPORT TELEPHONE");
                oled.drawHLine(0,10,128);
                oled.setFont(u8g2_font_4x6_tf);
#ifdef __ANDROID__
                if (importPhase == 0) {
                    oled.drawStr(0,26,"Importe les fichiers d'un");
                    oled.drawStr(0,34,"dossier du telephone vers");
                    oled.drawStr(0,42,"la carte SD de l'appli.");
                    oled.drawHLine(0,62,128);
                    oled.setFont(u8g2_font_5x7_tf);
                    oled.drawStr(0,75,"B1 = choisir un dossier");
                    oled.drawStr(0,87,"Click = annuler");
                } else if (importPhase == 1) {
                    oled.drawStr(0,26,"Import en cours...");
                    snprintf(buf,sizeof(buf),"%u / %u fichiers", importDone, importTotal);
                    oled.drawStr(0,40,buf);
                    static uint8_t spin=0; static uint32_t spinMs=0;
                    if(millis()-spinMs>150){spin=(spin+1)%4;spinMs=millis();}
                    const char* spinCh[]={"   |","   /","   -","   \\"};
                    oled.drawStr(80,26,spinCh[spin]);
                } else {
                    oled.drawStr(0,26,"Termine !");
                    snprintf(buf,sizeof(buf),"%u fichiers importes", importTotal);
                    oled.drawStr(0,40,buf);
                    oled.drawStr(0,67,"Click = menu");
                }
#else
                oled.drawStr(0,26,"Fonction reservee a");
                oled.drawStr(0,34,"l'application Android.");
                oled.drawHLine(0,62,128);
                oled.setFont(u8g2_font_5x7_tf);
                oled.drawStr(0,75,"Click = menu");
#endif
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
            // ---- DRUM2 (TR-808 style + sequencer) ----
            case MODE_DRUM2: {
                // Active FX string (3-char abbrevs; FILT shows type: LPF/LaF/HPF/BPF)
                char fxstr[32]="";
                for(uint8_t fi=0;fi<FX_COUNT;fi++){
                    if(!fxList[fi].active) continue;
                    if(fxstr[0]) strncat(fxstr," ",sizeof(fxstr)-strlen(fxstr)-1);
                    char a[4];
                    if(fi==0){ strncpy(a, fxFiltTypName(), sizeof(a)-1); a[3]='\0'; }
                    else{ strncpy(a,fxList[fi].name,3); a[3]='\0'; }
                    strncat(fxstr,a,sizeof(fxstr)-strlen(fxstr)-1);
                }
                if(!fxstr[0]) strncpy(fxstr,"--",3);
                static const char* kAltName[4]={"NRM","50%","RND","DBL"};
                // u8g2 drawStr: y = baseline. 5x7→chars at [y-6,y+1]. 4x6→chars at [y-5,y+1].
                // Pad strip: drawBox(x,bY,w,9) + drawStr(x,bY+7,...) puts chars at bY+2..bY+7 ✓
                // Vol bar:   drawFrame(x,vY,100,6) + drawStr(104,vY+5,...) → label chars = bar height ✓

                if(drum2View == 1){
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
                        if(vw>0) oled.drawBox(9,69,min(vw,100)*80/100,6);  // volume can exceed 100% (up to 2x); clamp the bar, not the number
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
                        oled.drawStr(0,83,"B3=Rec  B4=Anim  JY=vel JX=swg");
                    }
                }else if(drum2View == 2){
                    // ═══ DR2 ANIM VIEW ═══════════════════════════════
                    static const char* kDrAltName[4] = {"NRM","50%","RND","DBL"};
                    if (draniBrowse) {
                        // ── SD folder browser ─────────────────────────────────
                        oled.setFont(u8g2_font_4x6_tf);
                        char pb[32];
                        snprintf(pb, sizeof(pb), "ANIM %.22s", sdPath.c_str());
                        oled.drawStr(0, 7, pb);
                        oled.drawHLine(0, 9, 128);
                        if (!sdReady) {
                            oled.drawStr(10, 40, "SD not found");
                        } else {
                            for (int i = 0; i < 10 && (i + sdScroll) < sdFileCount; i++) {
                                int idx = i + sdScroll; bool sel = (idx == sdCursor);
                                char line[29];
                                if (sdFileIsDir[idx]) snprintf(line, sizeof(line), "%c[%.24s]", sel?'>':' ', sdFiles[idx].c_str());
                                else                  snprintf(line, sizeof(line), "%c%.26s",    sel?'>':' ', sdFiles[idx].c_str());
                                oled.drawStr(0, 17 + i * 7, line);
                                if (sel) oled.drawHLine(0, 18 + i * 7, 128);
                            }
                            oled.drawStr(0, 122, "B1=up  B3=close  click=load");
                        }
                    } else if (draniRunning) {
                        // ── Full-screen animation ─────────────────────────────
                        memcpy(draniDisplayBuf, draniBaseBuf, DRANI_FRAME_BYTES);
                        uint32_t nowMs = millis();
                        for (int d = 0; d < draniNumOverlays; d++) {
                            if (draniDrumHitMs[d] > 0 && nowMs - draniDrumHitMs[d] < DRANI_HIT_MS)
                                for (int i = 0; i < DRANI_FRAME_BYTES; i++)
                                    draniDisplayBuf[i] |= draniOverlayBuf[d][i];
                        }
                        oled.drawBitmap(0, 0, 16, 128, draniDisplayBuf);
                    } else {
                        // ── No folder loaded ──────────────────────────────────
                        oled.setFont(u8g2_font_4x6_tf);
                        oled.drawStr(0, 7, "DRUMS ANIM");
                        oled.drawHLine(0, 9, 128);
                        oled.drawStr(10, 50, "No folder loaded");
                        oled.drawStr(10, 62, "B3 = browse SD");
                        oled.drawStr(0, 122, "B1=play  B3=fold  B4=Seq");
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
                        if(vw>0) oled.drawBox(9,64,min(vw,100)*88/100,6);  // volume can exceed 100% (up to 2x); clamp the bar, not the number
                        snprintf(buf,sizeof(buf),"%d%%",vw);
                        oled.drawStr(100,69,buf);
                    }

                    // Hints (4x6 baseline=78,85)
                    oled.drawStr(0,78,"P3=Pit P4=Dcy P5=Vol B3=Rec");
                    oled.drawStr(0,85,"B2=FX  B4=Seq  JY=vel JX=swg");
                }
                break;
            }
            // ---- DR2 — hierarchical 4.4.4 drum sequencer ----
            case MODE_DR2: {
                // Note names are derived from the actual NoteMap-mapped MIDI note (not a
                // flat chromatic guess) so they stay correct under whatever scale/octave
                // is set via B3-double-click's OVERLAY_SCALE_ARP.
                static const char* kMidiNoteNames[12] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
                static const char* kDr2InstrNames[DR2_INSTR_COUNT] = {"DRUMS","SYNTH","303","---"};
                oled.setFont(u8g2_font_5x7_tf);
                {
                    char detail[8];
                    if (dr2Instrument==DR2_INSTR_DRUMS) strncpy(detail, kDrum2Labels[dr2SelPad], sizeof(detail)-1);
                    else if (dr2Instrument==DR2_INSTR_TBD) strncpy(detail, "--", sizeof(detail)-1);
                    else {
                        uint8_t mn = noteMap.getMidiNoteByIdx(dr2SelNote);
                        snprintf(detail, sizeof(detail), "%s%d", kMidiNoteNames[mn%12], mn/12-1);
                    }
                    detail[sizeof(detail)-1]='\0';
                    snprintf(buf,sizeof(buf),"GEST2 %s%s  %d BPM  [%s:%s]",
                             dr2Playing?"[>]":"[ ]", dr2RecArmed?"REC":"", bpm, kDr2InstrNames[dr2Instrument], detail);
                }
                oled.drawStr(0,7,buf);
                oled.drawHLine(0,9,128);

                // Beat/step/micro state is already fully shown on the key LEDs — showing it
                // again here would be redundant, so this area instead shows the pattern bank
                // (4 slots, selected via the left zone's row3) and the LOOP/LIVE play mode.
                oled.setFont(u8g2_font_4x6_tf);
                oled.drawStr(0,20,"PAT");
                for(int i=0;i<DR2_PATS;i++){
                    int cx=22+i*24, cy=13;
                    bool active=(i==dr2ActivePat);
                    bool filled=dr2PatFilled[i];
                    bool copySrc=(dr2Copied && i==dr2CopyPat);
                    if(active||filled){oled.drawBox(cx,cy,20,10);}
                    else oled.drawFrame(cx,cy,20,10);
                    if(active||filled) oled.setDrawColor(0);
                    snprintf(buf,sizeof(buf),"%d",i+1);
                    oled.drawStr(cx+8,cy+8,buf);
                    oled.setDrawColor(1);
                    if(active) oled.drawFrame(cx-1,cy-1,22,12);
                    if(copySrc) oled.drawBox(cx,cy+9,20,2);  // clipboard-source marker
                }
                oled.setFont(u8g2_font_5x7_tf);
                snprintf(buf,sizeof(buf),"Mode: %s", dr2PlayMode==DR2_LIVE?"LIVE":"LOOP");
                oled.drawStr(0,37,buf);
                oled.drawHLine(0,50,128);

                // Pad / note strip — depends on the focused instrument
                if (dr2Instrument==DR2_INSTR_DRUMS) {
                    oled.setFont(u8g2_font_5x7_tf);
                    for(int pi=0;pi<DRUM2_PADS;pi++){
                        int px=pi*15+2;
                        bool sel=(pi==dr2SelPad);
                        if(sel){oled.drawBox(px-2,52,17,9);oled.setDrawColor(0);}
                        oled.drawStr(px,59,kDrum2Labels[pi]);
                        oled.setDrawColor(1);
                    }
                } else if (dr2Instrument==DR2_INSTR_TBD) {
                    oled.setFont(u8g2_font_5x7_tf);
                    oled.drawStr(0,59,"Instrument non defini");
                } else {
                    oled.setFont(u8g2_font_4x6_tf);
                    for(int ni=0;ni<DR2_NOTES;ni++){
                        int px=ni*10+2;
                        bool sel=(ni==dr2SelNote);
                        if(sel){oled.drawBox(px-1,52,10,9);oled.setDrawColor(0);}
                        oled.drawStr(px,59,kMidiNoteNames[noteMap.getMidiNoteByIdx(ni)%12]);
                        oled.setDrawColor(1);
                    }
                }
                oled.drawHLine(0,62,128);

                // Drums: alteration mode (stamped onto newly-placed hits). Synth: current
                // shape (pot 2 cycles it). 303/TBD: simple label, no extra params yet.
                oled.setFont(u8g2_font_4x6_tf);
                if (dr2Instrument==DR2_INSTR_DRUMS) {
                    static const char* kDr2ModName[4] = {"NRM","50%","RND","DBL"};
                    for(int i=0;i<4;i++){
                        bool sel=(i==dr2PlaceMod);
                        int cx=2+i*24;
                        if(sel){oled.drawBox(cx,64,22,8);oled.setDrawColor(0);}
                        else oled.drawFrame(cx,64,22,8);
                        oled.drawStr(cx+3,71,kDr2ModName[i]);
                        oled.setDrawColor(1);
                    }
                } else if (dr2Instrument==DR2_INSTR_SYNTH) {
                    snprintf(buf,sizeof(buf),"Shape: %s  (pot2)", shapeNames[currentShape]);
                    oled.drawStr(0,71,buf);
                } else if (dr2Instrument==DR2_INSTR_T303) {
                    oled.drawStr(0,71,"303 (mono)");
                }
                oled.drawHLine(0,74,128);

                // Big beat.step readout (0-indexed) — the tempo pulse, as large as the
                // remaining space allows. Each glyph (digit/dot/digit) is drawn at a fixed
                // x derived from font metrics, not from the current string's measured width,
                // so the layout never shifts as the values change.
                // Exception: while actively touching a param control for the focused
                // instrument — 303 (joystick wave/octave, P2 wavefold, P4-P7 params),
                // Drums (P4-6 Pitch/Decay/Volume), or Synth (P4 Volume) — this area pops
                // up those params instead, for DR2_POPUP_MS after the last touch, then
                // reverts on its own — same info MODE_303S's/MODE_DRUM2's own screens
                // show for their own params, just here on demand.
                if (dr2Instrument==DR2_INSTR_T303 && millis()<dr2PopupUntil) {
                    // Wave+octave combined on one line ("SAW+0"/"SAW-1"...) frees a whole
                    // line to show Dur instead, and P2's current value+label (what it
                    // controls varies by wave — see t303P2Label) gets its own line too.
                    oled.setFont(u8g2_font_9x18_tf);
                    snprintf(buf,sizeof(buf),"%s%+d",t303WaveName(t303Wave),t303Oct);
                    oled.drawStr((128-oled.getStrWidth(buf))/2, 92, buf);
                    oled.setFont(u8g2_font_6x10_tf);
                    snprintf(buf,sizeof(buf),"Cut:%-4d Res:%.1f",(int)t303Cutoff,t303Reso);
                    oled.drawStr((128-oled.getStrWidth(buf))/2, 103, buf);
                    snprintf(buf,sizeof(buf),"Dur:%-3.0f%% Mod:%.1f",t303Duration*100.0f,t303EnvMod);
                    oled.drawStr((128-oled.getStrWidth(buf))/2, 114, buf);
                    snprintf(buf,sizeof(buf),"P2 %s:%.2f",t303P2Label(t303Wave),pots[1].value);
                    oled.drawStr((128-oled.getStrWidth(buf))/2, 125, buf);
                    oled.setFont(u8g2_font_5x7_tf);
                } else if (dr2Instrument==DR2_INSTR_DRUMS && millis()<dr2PopupUntil) {
                    oled.setFont(u8g2_font_9x18_tf);
                    snprintf(buf,sizeof(buf),"%s",kDrum2Labels[dr2SelPad]);
                    oled.drawStr((128-oled.getStrWidth(buf))/2, 92, buf);
                    oled.setFont(u8g2_font_6x10_tf);
                    snprintf(buf,sizeof(buf),"Pitch:%d",drum2Pitch[dr2SelPad]);
                    oled.drawStr((128-oled.getStrWidth(buf))/2, 105, buf);
                    snprintf(buf,sizeof(buf),"Decay:%-4.0fms Vol:%.0f%%",drum2Decay[dr2SelPad],drum2Volume[dr2SelPad]*100.0f);
                    oled.drawStr((128-oled.getStrWidth(buf))/2, 118, buf);
                    oled.setFont(u8g2_font_5x7_tf);
                } else if (dr2Instrument==DR2_INSTR_SYNTH && millis()<dr2PopupUntil) {
                    oled.setFont(u8g2_font_9x18_tf);
                    uint8_t mn = noteMap.getMidiNoteByIdx(dr2SelNote);
                    snprintf(buf,sizeof(buf),"%s%d",kMidiNoteNames[mn%12],mn/12-1);
                    oled.drawStr((128-oled.getStrWidth(buf))/2, 92, buf);
                    oled.setFont(u8g2_font_6x10_tf);
                    snprintf(buf,sizeof(buf),"Shape:%s",shapeNames[currentShape]);
                    oled.drawStr((128-oled.getStrWidth(buf))/2, 105, buf);
                    snprintf(buf,sizeof(buf),"Vol:%.0f%%",dr2SynthVolume*100.0f);
                    oled.drawStr((128-oled.getStrWidth(buf))/2, 118, buf);
                    oled.setFont(u8g2_font_5x7_tf);
                } else {
                    uint8_t bDisp = dr2Playing?dr2PlayBeat:dr2SelBeat;
                    uint8_t sDisp = dr2Playing?dr2PlayStep:dr2SelStep;
                    oled.setFont(u8g2_font_fub42_tn);
                    int dw = oled.getStrWidth("0");   // digit advance width (tabular in this font)
                    int ow = oled.getStrWidth(".");   // dot advance width
                    int total = dw + ow + dw;
                    int x0 = (128 - total) / 2;
                    char c1[2] = { (char)('0'+bDisp), 0 };
                    char c2[2] = { '.', 0 };
                    char c3[2] = { (char)('0'+sDisp), 0 };
                    oled.drawStr(x0, 125, c1);
                    oled.drawStr(x0+dw, 125, c2);
                    oled.drawStr(x0+dw+ow, 125, c3);
                    oled.setFont(u8g2_font_5x7_tf);
                }
                break;
            }
            // ---- SYSEQ — 16-step polyphonic synth sequencer ----
            case MODE_SYSEQ: {
                // Active FX string (FILT shows type: LPF/LaF/HPF/BPF)
                char fxstr[32]="";
                for(uint8_t fi=0;fi<FX_COUNT;fi++){
                    if(!fxList[fi].active) continue;
                    if(fxstr[0]) strncat(fxstr," ",sizeof(fxstr)-strlen(fxstr)-1);
                    char a[4];
                    if(fi==0){ strncpy(a, fxFiltTypName(), sizeof(a)-1); a[3]='\0'; }
                    else{ strncpy(a,fxList[fi].name,3); a[3]='\0'; }
                    strncat(fxstr,a,sizeof(fxstr)-strlen(fxstr)-1);
                }
                if(!fxstr[0]) strncpy(fxstr,"--",3);
                static const char* ssnn[]={"C","c","D","d","E","F","f","G","g","A","a","B"};

                {
                    // ═══ SSEQ VIEW ═══════════════════════════════
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
                    oled.drawStr(0,68,"B2=FX  B3=Opt  B4=Clr");
                }
                break;
            }
            // ---- 303S — TB-303 step sequencer ----
            case MODE_303S: {
                static const char* s3nn[]={"C","c","D","d","E","F","f","G","g","A","a","B"};
                const char* s3wname = t303WaveName(t303Wave);
                char s3fxstr[20]="";
                { uint8_t nfx=0;
                  for(uint8_t fi=0;fi<FX_COUNT;fi++){
                      if(!fxList[fi].active) continue;
                      if(nfx++) strncat(s3fxstr," ",sizeof(s3fxstr)-strlen(s3fxstr)-1);
                      char a[4];
                      if(fi==0){ strncpy(a, fxFiltTypName(), sizeof(a)-1); a[3]='\0'; }
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
                        { float wfBuf[126]; t303FillWaveform(t303Wave,p2,wfBuf,WW-2);
                          int prev=cy;
                          for(int i=0;i<WW-2;i++){
                              int py=constrain(cy-(int)(wfBuf[i]*ah+0.5f),WY+1,WY+WH-2);
                              if(i>0) oled.drawLine(WX+i,prev,WX+1+i,py); else oled.drawPixel(WX+1,py);
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
                        { float wfBuf[126]; t303FillWaveform(t303Wave,p2,wfBuf,WW-2);
                          int prev=cy;
                          for(int i=0;i<WW-2;i++){
                              int py=constrain(cy-(int)(wfBuf[i]*ah+0.5f),WY+1,WY+WH-2);
                              if(i>0) oled.drawLine(WX+i,prev,WX+1+i,py); else oled.drawPixel(WX+1,py);
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
            // ---- MOD2 — Modular Synthesizer ----
            case MODE_MOD2: {
                const Mod2AlgoDef& alg = kMod2Algos[mod2AlgoIdx];
                // Line 1: "MOD2  [ALGO]"  (6×10 font, occupies y=0..9)
                snprintf(buf,sizeof(buf),"MOD2  %s", alg.name);
                oled.drawStr(0,0,buf);
                oled.drawHLine(0,9,128);
                // Line 2: description (4×6 font, starts below separator)
                oled.setFont(u8g2_font_4x6_tf);
                oled.drawStr(0,16,alg.desc);
                oled.drawHLine(0,22,128);
                // P4-P7: param name + real value
                static const char* potLabels[]={"P4","P5","P6","P7"};
                for(int i=0;i<4;i++){
                    float rv = alg.p[i].mn + (alg.p[i].mx-alg.p[i].mn)*mod2P[i];
                    const char* pn = alg.p[i].name;
                    bool isHz = (pn[0]=='C' || pn[0]=='B'); // Cut, Brg
                    bool isMs = (pn[0]=='D' && pn[1]=='c');  // Dcy
                    bool isPct = (pn[0]=='C' && pn[1]=='h') || pn[0]=='R'; // Chr, Rvb
                    if(isHz)       snprintf(buf,sizeof(buf),"%s %-3s %4dHz",potLabels[i],pn,(int)rv);
                    else if(isMs)  snprintf(buf,sizeof(buf),"%s %-3s %4dms",potLabels[i],pn,(int)rv);
                    else if(isPct) snprintf(buf,sizeof(buf),"%s %-3s %3d%%",potLabels[i],pn,(int)(rv*100));
                    else           snprintf(buf,sizeof(buf),"%s %-3s %5.2f",potLabels[i],pn,rv);
                    oled.drawStr(0, 30 + i*9, buf);
                }
                snprintf(buf,sizeof(buf),"Env:%-3s  Oct:%+d",
                         envNames[mod2EnvIdx],noteMap.getOctave());
                oled.drawStr(0,70,buf);
                // Velocity bar (JX)
                oled.setFont(u8g2_font_5x7_tf);
                oled.drawHLine(0,86,128);
                {
                    float jx2=constrain(cachedJoyX/64.0f,-1.0f,1.0f);
                    int bw=(int)((jx2+1.0f)*0.5f*128.0f);
                    oled.drawFrame(0,88,128,5);
                    if(bw>2) oled.drawBox(0,88,bw,5);
                }
                break;
            }
            // ---- MODULAR/SERUM — wavetable dual-osc synth, visual editor ----
            // Shows a live curve for whichever element (OscA/OscB/Filter/Env/LFO) is
            // focused, so turning a pot has an immediate, legible on-screen consequence
            // instead of just a number changing. The oscillator curves are a STYLIZED
            // representative shape per wavetable+morph-position (not a literal readout of
            // AMY's internal PCM wavetable bytes, which this UI layer has no cheap access
            // to) — enough to see how the morph pot visibly deforms the wave, which is the
            // actual goal (understanding the *shape* of the interaction, not exact samples).
            case MODE_MODULAR: {
                static const char* kModWtNames[] = {"111","BRAIDS01","PPGWA00","SIN2SAW","VIRAL"};
                static const char* kFocusNames[] = {"OscA","OscB","Filt","Env","LFO"};
                oled.setFont(u8g2_font_4x6_tf);
                oled.drawStr(0,6,"SERUM");
                snprintf(buf,sizeof(buf),"Oct:%+d",noteMap.getOctave());
                oled.drawStr(90,6,buf);
                oled.drawHLine(0,9,128);

                // Tab strip: 5 elements, focused one filled.
                const int tabW = 128/5;
                for (int i=0;i<5;i++) {
                    int tx = i*tabW;
                    if (i==modFocusIdx) { oled.drawBox(tx,11,tabW-1,9); }
                    else                { oled.drawFrame(tx,11,tabW-1,9); }
                    oled.drawStr(tx+2,18,kFocusNames[i]);
                }

                // Curve area: x in [2,125], y in [24,74] (mid=49, half-height=25).
                const int CX0=2, CX1=125, CY_MID=49, CY_HALF=24;
                oled.drawHLine(CX0,CY_MID,CX1-CX0);
                const int N=40;
                int prevX=0, prevY=0; bool havePrev=false;
                switch (modFocusIdx) {
                    case 0: case 1: { // OscA / OscB waveform
                        uint8_t table = (modFocusIdx==0) ? modOscTable : modOscBTable;
                        float pos     = (modFocusIdx==0) ? modOscAPos  : modOscBPos;
                        for (int i=0;i<=N;i++) {
                            float t = (float)i/N, ph = t*2.0f*(float)PI;
                            float base = sinf(ph), character;
                            switch (table%5) {
                                case 0: character = 2.0f*(t-floorf(t+0.5f)); break;                       // 111: saw-ish
                                case 1: character = sinf(ph)+0.4f*sinf(ph*3.0f)-0.2f*sinf(ph*5.0f); break; // BRAIDS01: harmonic-rich
                                case 2: character = (fmodf(t,1.0f)<0.5f)?1.0f:-1.0f; break;                // PPGWA00: pulse-ish
                                case 3: character = base*(1.0f-t)+(2.0f*(t-floorf(t+0.5f)))*t; break;      // SIN2SAW: sine->saw
                                default: character = sinf(ph)*sinf(ph*7.0f)*0.6f+sinf(ph*2.3f)*0.4f; break;// VIRAL: gritty
                            }
                            float y = constrain(base*(1.0f-pos)+character*pos, -1.0f, 1.0f);
                            int px = CX0 + (int)(t*(CX1-CX0));
                            int py = CY_MID - (int)(y*CY_HALF);
                            if (havePrev) oled.drawLine(prevX,prevY,px,py);
                            prevX=px; prevY=py; havePrev=true;
                        }
                        break;
                    }
                    case 2: { // Filter magnitude response, log-frequency x-axis
                        for (int i=0;i<=N;i++) {
                            float t = (float)i/N;
                            float f = 20.0f*powf(1000.0f,t); // 20Hz..20kHz
                            float ratio = f/fmaxf(modCutoff,20.0f);
                            float mag = 1.0f/sqrtf(1.0f+powf(ratio,4.0f));
                            if (modReso>1.0f) {
                                float d = logf(fmaxf(ratio,0.001f));
                                mag += expf(-d*d*8.0f)*(modReso-1.0f)*0.5f;
                            }
                            mag = constrain(mag, 0.0f, 1.3f);
                            int px = CX0 + (int)(t*(CX1-CX0));
                            int py = CY_MID+CY_HALF - (int)(mag/1.3f*(2*CY_HALF));
                            if (havePrev) oled.drawLine(prevX,prevY,px,py);
                            prevX=px; prevY=py; havePrev=true;
                        }
                        break;
                    }
                    case 3: { // ADSR envelope shape
                        const EnvParams &env = envTable[currentEnv];
                        float wA = constrain(env.atk/8.0f, 6.0f, 26.0f);
                        float wD = constrain(env.dec/20.0f, 6.0f, 26.0f);
                        float wS = 20.0f;
                        float wR = constrain(env.rel/20.0f, 6.0f, 30.0f);
                        float total = wA+wD+wS+wR;
                        float scale = (CX1-CX0)/total;
                        int x0=CX0, yBot=CY_MID+CY_HALF, yTop=CY_MID-CY_HALF;
                        int x1=x0+(int)(wA*scale);
                        int ySus=yBot-(int)(env.sus*(yBot-yTop));
                        int x2=x1+(int)(wD*scale);
                        int x3=x2+(int)(wS*scale);
                        int x4=x3+(int)(wR*scale);
                        oled.drawLine(x0,yBot,x1,yTop);
                        oled.drawLine(x1,yTop,x2,ySus);
                        oled.drawLine(x2,ySus,x3,ySus);
                        oled.drawLine(x3,ySus,x4,yBot);
                        break;
                    }
                    default: { // LFO: sine shaped by depth, with a live phase dot
                        for (int i=0;i<=N;i++) {
                            float t = (float)i/N;
                            float y = sinf(t*2.5f*2.0f*(float)PI) * modLfoDepth;
                            int px = CX0 + (int)(t*(CX1-CX0));
                            int py = CY_MID - (int)(y*CY_HALF);
                            if (havePrev) oled.drawLine(prevX,prevY,px,py);
                            prevX=px; prevY=py; havePrev=true;
                        }
                        float tp = modLfoPhase/(2.0f*(float)PI);
                        tp = tp - floorf(tp);
                        int dpx = CX0 + (int)(fmodf(tp*2.5f,1.0f)*(CX1-CX0));
                        int dpy = CY_MID - (int)(sinf(modLfoPhase)*modLfoDepth*CY_HALF);
                        oled.drawDisc(dpx,dpy,3);
                        break;
                    }
                }

                // Live readout for the focused element, naming what JY does to it.
                oled.setFont(u8g2_font_4x6_tf);
                switch (modFocusIdx) {
                    case 0: snprintf(buf,sizeof(buf),"P2 %s  JY pos:%.0f%%",kModWtNames[modOscTable%5],modOscAPos*100.0f); break;
                    case 1: snprintf(buf,sizeof(buf),"B3 %s  JY pos:%.0f%%",kModWtNames[modOscBTable%5],modOscBPos*100.0f); break;
                    case 2: snprintf(buf,sizeof(buf),"P6 Cut:%uHz  JY Res:%.1f",(unsigned)modCutoff,modReso); break;
                    case 3: snprintf(buf,sizeof(buf),"JY:presetA:%d D:%d S:%.0f%% R:%d",envTable[currentEnv].atk,envTable[currentEnv].dec,envTable[currentEnv].sus*100.0f,envTable[currentEnv].rel); break;
                    default: snprintf(buf,sizeof(buf),"P7 Depth:%.0f%%  JY Rate:%.1fHz",modLfoDepth*100.0f,modLfoRate); break;
                }
                oled.drawStr(0,84,buf);
                oled.drawHLine(0,88,128);
                oled.drawStr(0,98,"JX: change element");
                oled.drawStr(0,107,"JY: modify it");
                oled.drawStr(0,116,"B1:FX  B2:Env  B3:OscB  B4:Oct");
                break;
            }
            // ---- I303 — polyphonic TB-303 ----
            case MODE_I303: {
                oled.setFont(u8g2_font_5x7_tf);
                snprintf(buf,sizeof(buf),"I303  Wv:%-4s Oct:%+d", t303WaveName(t303Wave), t303Oct);
                oled.drawStr(0,7,buf); oled.drawHLine(0,9,128);
                oled.setFont(u8g2_font_4x6_tf);
                snprintf(buf,sizeof(buf),"Oct:%+d  Scl:%-6s  %dBPM", t303Oct, scaleName(noteMap.getScale()), bpm);
                oled.drawStr(0,19,buf);
                snprintf(buf,sizeof(buf),"Cut:%4dHz  Res:%.1f",(int)t303Cutoff,t303Reso);
                oled.drawStr(0,29,buf);
                snprintf(buf,sizeof(buf),"Mod:%4.1f  Dur:%.0f%%",t303EnvMod,t303Duration*100.0f);
                oled.drawStr(0,38,buf);
                oled.drawHLine(0,42,128);
                oled.drawStr(0,50,"P2=Fold P3=Rs P4=Mod P5=Dur P6=Ct");
                // Active notes display
                { int nx=0;
                  for(int r=0;r<KBD_NOTE_ROWS;r++) for(int c=0;c<KBD_COLS;c++){
                      if(activeNotes[r][c]&&nx<8){
                          static const char* nn[]={"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
                          uint8_t m=activeNotes[r][c];
                          char nb[5]; snprintf(nb,sizeof(nb),"%s%d",nn[m%12],m/12-1);
                          oled.drawStr(nx*16,62,nb); nx++;
                      }
                  }
                  if(nx==0) oled.drawStr(0,62,"--");
                }
                // Waveform widget (same as 303S)
                {
                    const int WX=0,WY=68,WW=128,WH=20,cy=WY+WH/2,ah=WH/2-2;
                    oled.drawFrame(WX,WY,WW,WH);
                    float p2=pots[1].value;
                    { float wfBuf[126]; t303FillWaveform(t303Wave,p2,wfBuf,WW-2);
                      int prev=cy;
                      for(int i=0;i<WW-2;i++){
                          int py=constrain(cy-(int)(wfBuf[i]*ah+0.5f),WY+1,WY+WH-2);
                          if(i>0) oled.drawLine(WX+i,prev,WX+1+i,py); else oled.drawPixel(WX+1,py);
                          prev=py;
                      }
                    }
                }
                oled.setFont(u8g2_font_6x10_tf);
                oled.drawHLine(0,91,128);
                snprintf(buf,sizeof(buf),"Cut:%-4d  Res:%.1f",(int)t303Cutoff,t303Reso);
                oled.drawStr(0,104,buf);
                snprintf(buf,sizeof(buf),"Dur:%.0f%%  Mod:%-4.1f",t303Duration*100.0f,t303EnvMod);
                oled.drawStr(0,118,buf);
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
                static const char* kGran2ModeStr[5] = {"NRM","LOP","FUL","SEQ","SQL"};
                {
                    char st[5] = "----";
                    for (uint8_t s = 0; s < numSamp && s < 4; s++) {
                        if      (gran2[s].computed)                           st[s] = 'C';
                        else if (gran2[s].loaded)                             st[s] = 'R';
                        else if (!gran2[s].path.isEmpty())                    st[s] = 'L';
                        else                                                  st[s] = '-';
                    }
                    st[numSamp] = '\0';
                    snprintf(buf, sizeof(buf), "GRANU[%s] T%d %s", kGran2ModeStr[gran2PlayMode], gran2LoadTarget, st);
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
                            if (fxSelected == 0 && p == 3) {
                                strncpy(pp[np].val, fxFiltTypName(), sizeof(pp[np].val)-1);
                            } else {
                                float v = fx.params[p];
                                if      (v >= 1000.f) snprintf(pp[np].val, 8, "%.0fk", v / 1000.f);
                                else if (v >= 10.f)   snprintf(pp[np].val, 8, "%.0f",  v);
                                else                  snprintf(pp[np].val, 8, "%.2f",  v);
                            }
                            pp[np].name = fx.paramNames[p]; np++;
                        }
                        // Line 1 (y=111): FX name + first 2 params
                        char l1[32] = {}, l2[32] = {};
                        const char* fxdn = (fxSelected==0) ? fxFiltTypName() : fx.name;
                        int o1 = snprintf(l1, sizeof(l1), "%s%s:", fx.active ? "*" : "-", fxdn);
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
                            bool g2AnyLoaded2 = false;
                            for (uint8_t _s=0;_s<GRAN2_MAX_SAMPLES;_s++) if(gran2[_s].loaded||!gran2[_s].path.isEmpty()){g2AnyLoaded2=true;break;}
                            oled.drawStr(0, 118, g2AnyLoaded2 ? "B1=FX B2=mode B3=Tgt B4=Clr" : "B1=FX B2=mode B3=Tgt B4=auto");
                        }
                    } else {
                        bool g2AnyLoaded2 = false;
                        for (uint8_t _s=0;_s<GRAN2_MAX_SAMPLES;_s++) if(gran2[_s].loaded||!gran2[_s].path.isEmpty()){g2AnyLoaded2=true;break;}
                        oled.drawStr(0, 118, g2AnyLoaded2 ? "B1=FX B2=mode B3=Tgt B4=Clr" : "B1=FX B2=mode B3=Tgt B4=auto");
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
            // ---- ANIM ----
            case MODE_ANIM: {
                static const char* kAnimNames[] = {"WAVE","BARS","TECHNO","ACID","8BIT"};
                uint8_t  ai    = animIdx % 5;
                float    speed = pots[0].value * 0.8f + 0.2f;   // [0.2, 1.8]
                float    p2    = pots[1].value;
                uint32_t now   = millis();

                oled.setFont(u8g2_font_5x7_tf);
                char hdr[24];
                snprintf(hdr, sizeof(hdr), "ANIM  %s", kAnimNames[ai]);
                oled.drawStr(0, 7, hdr);
                oled.drawHLine(0, 9, 128);

                oled.setFont(u8g2_font_4x6_tf);

                if (ai == 0) {
                    // WAVE — double sine
                    float t = now * 0.001f * speed;
                    int prev = -1;
                    for (int x = 0; x < 128; x++) {
                        float ph = x * (6.2832f / 128.0f);
                        int iy = constrain((int)(68 + 26*sinf(ph*2 + t*2.5f) + p2*22*sinf(ph*3 - t*1.7f)), 11, 126);
                        if (prev >= 0) oled.drawLine(x-1, prev, x, iy);
                        prev = iy;
                    }
                    for (int x = 0; x < 128; x += 2) {
                        int iy = constrain((int)(68 + 14*sinf(x*(6.2832f/128.0f)*4 + t*1.5f + 1.2f) * (0.4f + p2*0.6f)), 11, 126);
                        oled.drawPixel(x, iy);
                    }
                } else if (ai == 1) {
                    // BARS — 16 colonnes VU-meter
                    static float bH[16] = {}, bT[16] = {};
                    static uint32_t bUpd = 0;
                    float maxH = 40.0f + p2 * 76.0f;
                    if (now - bUpd > (uint32_t)(600.0f / speed)) {
                        bUpd = now;
                        for (int i = 0; i < 16; i++) bT[i] = 5.0f + (float)random(0, (int)maxH);
                    }
                    float lp = 0.22f * speed;
                    for (int i = 0; i < 16; i++) {
                        bH[i] += (bT[i] - bH[i]) * lp;
                        int h = max(2, (int)bH[i]);
                        oled.drawBox(i*8, 127 - h, 6, h);
                    }
                } else if (ai == 2) {
                    // TECHNO — radar sweep
                    int cx = 64, cy = 69;
                    oled.drawCircle(cx, cy, 18);
                    oled.drawCircle(cx, cy, 36);
                    oled.drawCircle(cx, cy, 54);
                    oled.drawLine(cx-2, cy, cx+2, cy);
                    oled.drawLine(cx, cy-2, cx, cy+2);
                    float angle = fmodf(now * 0.001f * speed * 2.094f, 6.2832f);  // 1 tour / 3s à speed=1
                    for (int tr = 0; tr <= 6; tr++) {
                        float a = angle - tr * 0.13f;
                        float r = 56.0f * (1.0f - tr * 0.03f);
                        int x2 = constrain(cx + (int)(r * cosf(a)), 0, 127);
                        int y2 = constrain(cy + (int)(r * sinf(a)), 11, 127);
                        if (tr == 0) oled.drawLine(cx, cy, x2, y2);
                        else if (tr % 2 == 0) oled.drawLine(cx, cy, (cx+x2)/2, (cy+y2)/2);
                    }
                    if (p2 > 0.05f) {
                        int br = 20 + (int)(p2 * 34);
                        float ba = angle * 1.7f;
                        oled.drawBox(constrain(cx+(int)(br*cosf(ba))-2,0,124),
                                     constrain(cy+(int)(br*sinf(ba))-2,11,124), 4, 4);
                    }
                } else if (ai == 3) {
                    // ACID — Lissajous
                    float t = now * 0.001f * speed;
                    float a = 2.0f + p2;   // ratio 2→3
                    float delta = t * 1.3f;
                    float r = 52.0f;
                    int px = -1, py = -1;
                    for (int i = 0; i <= 256; i++) {
                        float s = i * (6.2832f / 256.0f);
                        int x = constrain(64 + (int)(r * sinf(a * s + delta)), 0, 127);
                        int y = constrain(69 + (int)(r * sinf(3.0f * s)), 11, 127);
                        if (px >= 0) oled.drawLine(px, py, x, y);
                        else oled.drawPixel(x, y);
                        px = x; py = y;
                    }
                } else {
                    // 8BIT — matrix rain
                    static uint8_t cY[16] = {};
                    static uint8_t cS[16] = {};
                    static bool c8Init = false;
                    static uint32_t c8Upd = 0;
                    if (!c8Init) {
                        for (int i = 0; i < 16; i++) { cY[i]=(uint8_t)(11+random(0,110)); cS[i]=(uint8_t)(1+random(0,4)); }
                        c8Init = true;
                    }
                    if (now - c8Upd >= (uint32_t)max(20, (int)(120.0f / speed))) {
                        c8Upd = now;
                        for (int i = 0; i < 16; i++) {
                            cY[i] = (uint8_t)(11 + (cY[i] - 11 + cS[i]) % 116);
                        }
                    }
                    int trail = 3 + (int)(p2 * 14);
                    for (int i = 0; i < 16; i++) {
                        int x = i*8 + 3;
                        oled.drawBox(x-1, cY[i], 3, 4);   // head pixel
                        for (int t = 1; t <= trail; t++) {
                            int ty = cY[i] - t*7;
                            if (ty < 11) ty += 116;
                            if (ty >= 11 && ty < 124) oled.drawPixel(x, ty);
                        }
                    }
                }

                oled.drawStr(0, 127, "B1=nxt B3=prv P1=spd P2");
                break;
            }
            // ---- MEDIA (formerly VIDEO) — plays .bvid/.png/.jpg/.jpeg as before, plus
            // .wav/.mp3 preview playback via the same PCM_PREVIEW_PRESET mechanism
            // MODE_SAMPLE's browser already uses. ----
            case MODE_VID: {
                if (vidPlaying && vidFileOpen) {
                    // Full-screen 1bpp video — drawBitmap: MSB of each byte = leftmost pixel
                    oled.drawBitmap(0, 0, 16, 128, vidFrameBuf);
                } else if (mediaAudioPlaying) {
                    oled.setFont(u8g2_font_4x6_tf);
                    oled.drawStr(0, 7, "MEDIA - playing audio");
                    oled.drawHLine(0, 9, 128);
                    oled.setFont(u8g2_font_5x7_tf);
                    { int ls = mediaAudioPath.lastIndexOf('/');
                      String fn = ls >= 0 ? mediaAudioPath.substring(ls + 1) : mediaAudioPath;
                      oled.drawStr(0, 55, fn.c_str()); }
                    oled.setFont(u8g2_font_4x6_tf);
                    oled.drawStr(0, 122, "B1/click = stop");
                } else {
                    oled.setFont(u8g2_font_4x6_tf);
                    char pb[32];
                    snprintf(pb, sizeof(pb), "MEDIA %.22s", sdPath.c_str());
                    oled.drawStr(0, 7, pb);
                    oled.drawHLine(0, 9, 128);
                    if (!sdReady) {
                        oled.drawStr(10, 40, "SD not found");
                        oled.drawStr(10, 55, "Click = retry");
                    } else {
                        for (int i = 0; i < 10 && (i + sdScroll) < sdFileCount; i++) {
                            int idx = i + sdScroll; bool sel = (idx == sdCursor);
                            char line[29];
                            if (sdFileIsDir[idx]) snprintf(line, sizeof(line), "%c[%.24s]", sel?'>':' ', sdFiles[idx].c_str());
                            else                  snprintf(line, sizeof(line), "%c%.26s",    sel?'>':' ', sdFiles[idx].c_str());
                            oled.drawStr(0, 17 + i * 7, line);
                            if (sel) oled.drawHLine(0, 18 + i * 7, 128);
                        }
                        oled.drawStr(0, 122, "B1=back  click=play");
                    }
                }
                break;
            }
            case MODE_LANIM: {
                static const char* kLAnimNames[] = {"FLASH","RBOW","CHSE","NOIS","ORGA"};
                oled.setFont(u8g2_font_5x7_tf);
                char lhdr[24];
                snprintf(lhdr, sizeof(lhdr), "LANIM  %s", kLAnimNames[lanimIdx % 5]);
                oled.drawStr(0, 7, lhdr);
                oled.drawHLine(0, 9, 128);
                oled.setFont(u8g2_font_4x6_tf);
                // Pot indicators
                static const char* lPotNames[] = {"Spd","Dens","Hue","Bri"};
                for (int i = 0; i < 4; i++) {
                    oled.drawStr(0, 20 + i*10, lPotNames[i]);
                    int bw = (int)(pots[i].value * 80);
                    oled.drawBox(22, 14 + i*10, bw, 6);
                    oled.drawFrame(22, 14 + i*10, 80, 6);
                }
                oled.drawStr(0, 126, "B2=nxt B4=prv  key=nxt");
                break;
            }
            case MODE_EXP: {
                // Theremin display: play field + sidebar modifiers
                static const char* kExpWave[] = {"SAW","SQR","SIN","NOI"};
                static const char* kExpArp[]  = {"OFF","UP","DWN","RND"};
                static const char* kExpScale[]= {"FRE","CHR","PNT","MAJ"};
                static const char* kExpOct[]  = {"-2","-1"," 0","+1"};
                static const char* kExpFxN[]  = {"REV","DLY","CHR","REP"};

                // Play field — dot follows physics position
                int dotX = 18 + (int)(expPosX * 100.0f);
                int dotY = 10 + (int)(expPosY * 100.0f);
                dotX = constrain(dotX, 18, 118); dotY = constrain(dotY, 10, 110);

                oled.setFont(u8g2_font_4x6_tf);
                oled.drawFrame(17, 9, 103, 103);

                // Note name on Y axis
                float jyForDisp = 1.0f - expPosY * 2.0f;
                uint8_t dispNote = expComputeNote(jyForDisp);
                static const char* kNoteNames[] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
                char noteBuf[8]; snprintf(noteBuf, sizeof(noteBuf), "%s%d", kNoteNames[dispNote%12], dispNote/12-1);
                oled.drawStr(0, dotY < 10 ? 15 : (dotY > 110 ? 110 : dotY+3), noteBuf);

                // Trail: render fading path before cursor (newest = disc, middle = pixel, old = gone)
                {
                    uint32_t nowT = millis();
                    for (uint8_t i = 0; i < expTrailCount; i++) {
                        // Walk backward from head-1 (newest) to oldest
                        uint8_t idx = (expTrailHead + EXP_TRAIL_LEN - 1 - i) % EXP_TRAIL_LEN;
                        uint32_t age = nowT - expTrail[idx].t;
                        if (age >= EXP_TRAIL_MS) continue;  // expired
                        if (age < 450) {
                            oled.drawDisc(expTrail[idx].x, expTrail[idx].y, 1);
                        } else if (age < 900) {
                            oled.drawPixel(expTrail[idx].x, expTrail[idx].y);
                        }
                        // age 900..EXP_TRAIL_MS: invisible
                    }
                }

                // Cursor crosshair
                oled.drawLine(18, dotY, 119, dotY);
                oled.drawLine(dotX, 10, dotX, 111);
                oled.drawBox(dotX-2, dotY-2, 5, 5);

                // X axis = arp BPM multiplier [0.5×..2.0×] of global BPM
                float arpMulDisp = 0.5f + expPosX * 1.5f;
                char arpBuf[8];
                if (expColSel[2] == 0) {
                    snprintf(arpBuf, sizeof(arpBuf), "--");
                } else {
                    uint16_t arpBpm = (uint16_t)((float)bpm * arpMulDisp + 0.5f);
                    snprintf(arpBuf, sizeof(arpBuf), "%3d", arpBpm);
                }

                // Sidebar: wave / arp mode / scale / octave / arp BPM
                oled.drawStr(121, 15, kExpWave[expColSel[0]%4]);
                oled.drawStr(121, 28, kExpArp[expColSel[2]%4]);
                oled.drawStr(121, 41, kExpScale[expColSel[5]%4]);
                char octBuf[4]; snprintf(octBuf, sizeof(octBuf), "%s", kExpOct[expColSel[6]%4]);
                oled.drawStr(121, 54, octBuf);
                oled.drawStr(121, 67, arpBuf);  // arp BPM (replaces TEX)

                // FX indicators at bottom
                char fxBuf[20]; fxBuf[0]=0;
                for (int i=0;i<4;i++) if(expFxMask&(1<<i)){ strcat(fxBuf,kExpFxN[i]); strcat(fxBuf," "); }
                if (fxBuf[0]) oled.drawStr(0, 126, fxBuf);
                else oled.drawStr(0, 126, "B1=rst B3=oct- B4=oct+");
                break;
            }
            case MODE_EXP2: {
                static const char* kScaleNm[] = {"MAJ","MIN","PNT","CHR"};
                static const char* kWaveNm[]  = {"SAW","SQR","SIN","NOI"};
                static const char* kEnvNm[]   = {"PLK","FST","NRM","PAD"};
                oled.setFont(u8g2_font_4x6_tf);
                // Compact header: scale octave | waveform envelope | balls sides
                char hdr[32];
                snprintf(hdr, sizeof(hdr), "%s O%+d %s %s B%d P%d",
                         kScaleNm[exp2Scale%4], exp2Octave,
                         kWaveNm[exp2Shape%4], kEnvNm[exp2EnvIdx%4],
                         exp2BallCount, exp2NumSides);
                oled.drawStr(0, 6, hdr);
                // Polygon walls (N sides)
                uint8_t N = exp2NumSides;
                struct { int16_t x, y; } pv[EXP2_MAX_SIDES];
                float aStep = 2.0f*(float)M_PI/(float)N;
                for (int i=0;i<N;i++) {
                    float a=exp2HexAngle+i*aStep;
                    pv[i].x=(int16_t)(64+40.0f*cosf(a));
                    pv[i].y=(int16_t)(64+40.0f*sinf(a));
                }
                uint32_t nowD = millis();
                for (int i=0;i<N;i++) {
                    int ni=(i+1)%N;
                    oled.drawLine(pv[i].x, pv[i].y, pv[ni].x, pv[ni].y);
                    if (exp2WallFlash[i] && (nowD - exp2WallFlashMs[i] < 130)) {
                        int mx=(pv[i].x+pv[ni].x)/2, my=(pv[i].y+pv[ni].y)/2;
                        oled.drawDisc(mx, my, 2);
                    }
                }
                // Gravity direction indicator: arrow from center toward current joystick direction
                {
                    int gx=(int)(64+cosf(exp2GravAngle)*16);
                    int gy=(int)(64+sinf(exp2GravAngle)*16);
                    oled.drawLine(64, 64, gx, gy);
                    oled.drawDisc(gx, gy, 3);
                }
                // Balls
                for (int b=0;b<EXP2_MAX_BALLS;b++) {
                    if (!exp2Balls[b].active) continue;
                    int bx=constrain((int)exp2Balls[b].x,2,125);
                    int by=constrain((int)exp2Balls[b].y,8,125);
                    oled.drawDisc(bx, by, 2);
                }
                oled.drawStr(0, 127, "B1=kick B2=rev B3=- B4=+ J=grav");
                break;
            }
            case MODE_EXP3: {
                static const char* kScaleNm[] = {"MAJ","MIN","PNT","CHR"};
                static const char* kOrbitLabel[] = {"1b","2b","4b","8b"};
                oled.setFont(u8g2_font_4x6_tf);

                // Center shifted down to leave header room
                const int CX = 64, CY = 70;

                // Compact header: BPM, speed, scale, octave
                char hdr[32];
                snprintf(hdr, sizeof(hdr), "BPM:%d x%.1f %s O%+d",
                         bpm, exp3SpeedMul, kScaleNm[exp3Scale%4], exp3Octave);
                oled.drawStr(0, 6, hdr);

                // Central star — filled + outer ring
                oled.drawDisc(CX, CY, 2);
                oled.drawCircle(CX, CY, 5);

                // 4 orbit circles with period labels and trigger markers
                for (int or_=0; or_<EXP3_NUM_ORBITS; or_++) {
                    bool anyActive = false;
                    for (int b=0;b<EXP3_MAX_BALLS;b++)
                        if (exp3Balls[b].active && exp3Balls[b].orbitRow==(uint8_t)or_) { anyActive=true; break; }
                    int r = (int)kExp3Radii[or_];

                    if (anyActive) {
                        oled.drawCircle(CX, CY, r);
                        // Trigger zone: short bold bar at 12-o-clock (top of orbit)
                        oled.drawLine(CX-2, CY-r-1, CX+2, CY-r-1);
                        oled.drawLine(CX-1, CY-r-2, CX+1, CY-r-2);
                    } else {
                        // Inactive orbit: sparse dotted circle
                        for (int deg=0; deg<360; deg+=12) {
                            float a = (float)deg * (float)M_PI / 180.0f;
                            oled.drawPixel((int16_t)(CX + r*cosf(a)), (int16_t)(CY + r*sinf(a)));
                        }
                    }
                    // Period label at 3-o-clock position
                    oled.drawStr(CX + r + 2, CY + 3, kOrbitLabel[or_]);
                }

                // Balls: filled disc + column index label
                for (int b=0;b<EXP3_MAX_BALLS;b++) {
                    if (!exp3Balls[b].active) continue;
                    float r = kExp3Radii[exp3Balls[b].orbitRow];
                    int px = constrain((int)(CX + r*cosf(exp3Balls[b].angle)), 10, 118);
                    int py = constrain((int)(CY + r*sinf(exp3Balls[b].angle)), 10, 122);
                    if (exp3Balls[b].triggered) {
                        // Triggered: filled disc + outline ring
                        oled.drawDisc(px, py, 3);
                        oled.drawCircle(px, py, 5);
                    } else {
                        oled.drawDisc(px, py, 2);
                    }
                    // Column number offset slightly above ball
                    char nc[2] = {(char)('0'+b), 0};
                    oled.drawStr(px - 1, py - 4, nc);
                }

                // Bottom: gate duration indicator
                char bot[20];
                snprintf(bot, sizeof(bot), "G:%dms J:spd/gate", (int)exp3GateMs);
                oled.drawStr(0, 127, bot);
                break;
            }
            case MODE_LIFE: {
                oled.setFont(u8g2_font_4x6_tf);
                static const char* kRuleNm[] = {"B3/S23","B36/S23"};
                char hdr[32];
                snprintf(hdr, sizeof(hdr), "%sBPM:%d %s", lifePaused?"[||] ":"", bpm, kRuleNm[lifeRule%2]);
                oled.drawStr(0, 6, hdr);
                char hdr2[32];
                snprintf(hdr2, sizeof(hdr2), "Instr:%s", shapeNames[lifeShape]);
                oled.drawStr(0, 13, hdr2);
                // Grid: 8 cols x 4 rows, each cell a filled/outline box, big enough to be
                // clearly legible (12px cells, centered-ish in the 128x128 screen).
                const int cellW = 15, cellH = 15, ox = 2, oy = 18;
                for (int r=0;r<KBD_NOTE_ROWS;r++) {
                    for (int c=0;c<KBD_COLS;c++) {
                        // Column flipped (KBD_COLS-1-c) to match the physical key layout —
                        // same convention every other grid renderer in this file uses (LED
                        // render, DRUM2's OLED grid, etc.); this one was the odd one out,
                        // drawing column c straight through and mirroring the grid left-right
                        // relative to which physical key actually toggled a cell.
                        int x = ox + (KBD_COLS-1-c)*cellW, y = oy + (KBD_NOTE_ROWS-1-r)*cellH; // row0=bottom
                        if (lifeGrid[r][c]) oled.drawBox(x, y, cellW-2, cellH-2);
                        else                oled.drawFrame(x, y, cellW-2, cellH-2);
                    }
                }
                char bot[32];
                snprintf(bot, sizeof(bot), "Keys=seed P2=instr B1=%s", lifePaused?"play":"pause");
                oled.drawStr(0, 127, bot);
                break;
            }
            case MODE_SWARM: {
                oled.setFont(u8g2_font_4x6_tf);
                static const char* kScaleNmS[] = {"MAJ","MIN","PNT","CHR"};
                const int CX = 64, CY = 70;
                char hdr[32];
                snprintf(hdr, sizeof(hdr), "Flock:%.0f%% Spd:%.1f %s",
                         swarmFlockBal*100.0f, swarmSpeedCap, kScaleNmS[swarmScale%4]);
                oled.drawStr(0, 6, hdr);
                // Rotating arena outline (reused visual idiom from EXP2)
                for (int s=0; s<swarmNumSides; s++) {
                    float a0 = swarmHexAngle + s*2.0f*(float)PI/swarmNumSides;
                    float a1 = swarmHexAngle + (s+1)*2.0f*(float)PI/swarmNumSides;
                    oled.drawLine((int)(CX+cosf(a0)*50), (int)(CY+sinf(a0)*50),
                                  (int)(CX+cosf(a1)*50), (int)(CY+sinf(a1)*50));
                }
                // Attractor zone (joystick-steered)
                float jx = constrain(cachedJoyX/64.0f,-1.0f,1.0f), jy = constrain(cachedJoyY/64.0f,-1.0f,1.0f);
                int azx = (int)(CX + jx*18.0f), azy = (int)(CY + jy*18.0f);
                oled.drawCircle(azx, azy, (int)swarmZoneRadius);
                // Boids
                for (int i=0;i<SWARM_MAX_BOIDS;i++) {
                    if (!swarmBoids[i].active) continue;
                    int px = constrain((int)(CX+swarmBoids[i].x), 4, 124);
                    int py = constrain((int)(CY+swarmBoids[i].y), 12, 124);
                    if (swarmBoids[i].inZone) { oled.drawDisc(px, py, 3); }
                    else oled.drawDisc(px, py, 2);
                }
                oled.drawStr(0, 127, "Joy=steer  Keys=on/off");
                break;
            }
            case MODE_GEN: {
                oled.setFont(u8g2_font_4x6_tf);
                static const char* kScaleNmG[] = {"MAJ","MIN","PNT","CHR"};
                char hdr[32];
                snprintf(hdr, sizeof(hdr), "%s%s > %s", genPaused?"[||] ":"",
                         kGenNames[genGenerator%GEN_GENERATOR_COUNT], kGenVoices[genVoice%GEN_VOICE_COUNT].name);
                oled.drawStr(0, 6, hdr);
                char hdr2[32];
                snprintf(hdr2, sizeof(hdr2), "BPM:%d Oct:%d %s", bpm, genOctave, kScaleNmG[genScale%4]);
                oled.drawStr(0, 13, hdr2);

                // Generator-specific detail line
                char gd[32];
                switch (genGenerator % GEN_GENERATOR_COUNT) {
                    case 0: snprintf(gd, sizeof(gd), "Wander:%d%%", (int)(genWalkWander*100.0f)); break;
                    case 1: snprintf(gd, sizeof(gd), "Pulses:%d/8", genEuclPulses); break;
                    default: snprintf(gd, sizeof(gd), "Chaos R:%.2f", genDriftR); break;
                }
                oled.drawStr(0, 20, gd);

                // 8-column strip: highlight the column the generator most recently landed
                // on. Column flipped (KBD_COLS-1-c) to match the physical key layout, same
                // convention the LED renderer for this mode already uses below.
                const int cellW = 14, cellH = 24, ox = 4, oy = 30;
                for (int c=0;c<KBD_COLS;c++) {
                    int x = ox + (KBD_COLS-1-c)*cellW;
                    if (c == genLastCol) oled.drawBox(x, oy, cellW-3, cellH);
                    else                 oled.drawFrame(x, oy, cellW-3, cellH);
                }
                oled.drawStr(2, 92, "Joy X:texture  Y:voice");
                oled.drawStr(2, 102, "Keys: play  P2:tune P7:tone");
                char bot[24]; snprintf(bot, sizeof(bot), "B1:%s", genPaused?"play":"pause");
                oled.drawStr(2, 112, bot);
                break;
            }
            case MODE_303S2: {
                static const char* s2nn[]={"C","c","D","d","E","F","f","G","g","A","a","B"};
                const char* s2wname = t303WaveName(t303Wave);
                oled.setFont(u8g2_font_5x7_tf);
                snprintf(buf,sizeof(buf),"303S2 %s %dBPM %s O%+d",
                         drum2Playing?"[>]":"[ ]",bpm,s2wname,t303Oct);
                oled.drawStr(0,7,buf);
                oled.drawHLine(0,9,128);

                // Sequence display: 2 rows of 8, each cell 16×13px (128/8=16)
                oled.setFont(u8g2_font_4x6_tf);
                {
                    const int cellW=16, cellH=13;
                    for(int half=0;half<2;half++){
                        int sStart=half*8, cy=11+half*14;
                        for(int sc=0;sc<8;sc++){
                            int si=sStart+sc, cx=sc*cellW;
                            bool filled=(si<(int)s303s2Count);
                            if(!filled){ oled.drawFrame(cx,cy,cellW-1,cellH); continue; }
                            uint8_t idx=(uint8_t)((s303s2Head-s303s2Count+si+S303S2_LEN)%S303S2_LEN);
                            uint8_t noteVal=s303s2Seq[idx];
                            bool cur=drum2Playing&&(si==(int)s303s2Step);
                            bool isRest=(noteVal==0);
                            if(cur){ oled.drawBox(cx,cy,cellW-1,cellH); }
                            else if(!isRest){ oled.drawBox(cx+1,cy+1,cellW-3,cellH-2); }
                            else { oled.drawFrame(cx,cy,cellW-1,cellH); }
                            if(!isRest){
                                uint8_t n=noteVal-1;
                                char nb[4]; snprintf(nb,sizeof(nb),"%s%d",s2nn[n%12],(int)(n/12)-1);
                                oled.setDrawColor(cur?0:1);
                                oled.drawStr(cx+1,cy+9,nb);
                                oled.setDrawColor(1);
                            } else {
                                oled.setDrawColor(cur?0:1);
                                oled.drawStr(cx+5,cy+9,"-");
                                oled.setDrawColor(1);
                            }
                        }
                    }
                }
                oled.drawHLine(0,39,128);

                // Count + accent state
                snprintf(buf,sizeof(buf),"%d/16 notes  Acc:%s  B4=clear",s303s2Count,t303AccentOn?"ON":"--");
                oled.drawStr(0,47,buf);

                // Wave + filter params
                oled.setFont(u8g2_font_6x10_tf);
                oled.drawHLine(0,50,128);
                snprintf(buf,sizeof(buf),"Cut:%-4d  Res:%.1f",(int)t303Cutoff,t303Reso);
                oled.drawStr(0,63,buf);
                snprintf(buf,sizeof(buf),"Dur:%.0f%%  Mod:%-4.1f",t303Duration*100.0f,t303EnvMod);
                oled.drawStr(0,77,buf);

                // Waveform preview
                oled.setFont(u8g2_font_4x6_tf);
                {
                    const int WX=0,WY=82,WW=128,WH=20,cy=WY+WH/2,ah=WH/2-2;
                    oled.drawFrame(WX,WY,WW,WH);
                    float p2=pots[1].value;
                    float wfBuf[126]; t303FillWaveform(t303Wave,p2,wfBuf,WW-2);
                    int prev=cy;
                    for(int i=0;i<WW-2;i++){
                        int py=constrain(cy-(int)(wfBuf[i]*ah+0.5f),WY+1,WY+WH-2);
                        if(i>0) oled.drawLine(WX+i,prev,WX+1+i,py); else oled.drawPixel(WX+1,py);
                        prev=py;
                    }
                }

                // Hint
                oled.drawStr(0,127,"REST=top-left   B1=play  JY=oct  JX=wave");
                break;
            }
            case MODE_GEST: {
                static const char* kGestSeqNames[GEST_NSEQ]={"DRUM","303S","SYNS","SAMP"};
                oled.setFont(u8g2_font_5x7_tf);
                snprintf(buf,sizeof(buf),"GEST  %s  BPM:%d  %s",
                    gestPlayMode==GEST_LOOP?"LOOP":"LIVE", bpm, drum2Playing?"[>]":"[ ]");
                oled.drawStr(0,7,buf);
                oled.drawHLine(0,9,128);
                oled.setFont(u8g2_font_4x6_tf);
                // 4 rows: label(20px) + 8 squares (each 11px) + vol bar (rest)
                for (uint8_t si=0; si<GEST_NSEQ; si++) {
                    int rowY = 11 + (int)si * 15;
                    oled.drawStr(0, rowY+7, kGestSeqNames[si]);
                    for (uint8_t p=0; p<GEST_PATS; p++) {
                        int bx = 22 + (int)p * 12, by = rowY;
                        bool isFilled = g_patFilled[si][p];
                        bool isActive = (gestActPat[si]==p);
                        bool isSel    = (gestSelRow==si && gestSelCol==p);
                        if (isActive && drum2Playing) oled.drawBox(bx,by,11,10);
                        else if (isFilled)            oled.drawBox(bx+1,by+1,9,8);
                        else                          oled.drawFrame(bx,by,11,10);
                        if (isSel) { oled.setDrawColor(2); oled.drawFrame(bx,by,11,10); oled.setDrawColor(1); }
                    }
                    // Volume bar (right side)
                    int vx=120, vw=(int)(gestSeqVol[si]*7.0f); if(vw>7)vw=7;
                    oled.drawFrame(vx,rowY,8,10); if(vw>0) oled.drawBox(vx,rowY+10-vw,8,vw);
                }
                oled.drawHLine(0,73,128);
                // Pot labels
                snprintf(buf,sizeof(buf),"P4=D:%.0f%% P5=3:%.0f%% P6=S:%.0f%% P7=A:%.0f%%",
                    gestSeqVol[0]*100,gestSeqVol[1]*100,gestSeqVol[2]*100,gestSeqVol[3]*100);
                oled.drawStr(0,81,buf);
                // Copy buffer indicator
                if (gestCopied) {
                    snprintf(buf,sizeof(buf),"CPY: %s #%d  B4=PST",kGestSeqNames[gestCopySeq],gestCopyPat+1);
                    oled.drawStr(0,89,buf);
                }
                oled.drawHLine(0,117,128);
                oled.drawStr(0,127,"B1=Play B2=FX B3=LOOP/LIVE B4=CPY/PST");
                break;
            }
            case MODE_POKEMON: {
                const PokemonDef& pk = kPokemon[pkmnSelected];

                // Sprite 2× (96×96) centered, y=0..95
                drawXBMScaled2x(16, 0, kPkmnSprites[pkmnSelected],
                                PKMN_SPRITE_SIZE, PKMN_SPRITE_SIZE);

                // Bandeau nom inversé (blanc sur noir), y=96..106
                oled.drawBox(0, 96, 128, 11);
                oled.setDrawColor(0);
                oled.setFont(u8g2_font_6x10_tf);
                {
                    int nw = oled.getStrWidth(pk.name);
                    oled.drawStr((128 - nw) / 2, 106, pk.name);
                }
                oled.setDrawColor(1);

                // Info compacte: type · #NNN · N/25 · Oct, y=115
                oled.setFont(u8g2_font_4x6_tf);
                snprintf(buf, sizeof(buf), "%s  #%d  %d/%d  Oct:%+d",
                         kPkmnTypeNames[pk.type], pk.number,
                         pkmnSelected + 1, (int)PKMN_COUNT, noteMap.getOctave());
                oled.drawStr(0, 115, buf);

                // Hint, y=127
                oled.drawHLine(0, 118, 128);
                oled.drawStr(0, 127, "FX  Arp  Env  Oct | P2=pkmn");
                break;
            }
            default:
                oled.drawStr(0,0,menuLabels[currentMode]);
                oled.drawStr(20,60,"Coming soon...");
                break;
        }
    }

    // sendBuffer() delegated to displayTask (Core 0) — Core 1 is not blocked.
    if (s_displayTaskHandle) xTaskNotifyGive(s_displayTaskHandle);
}

// ==================== DISPLAY TASK ====================
// sendBuffer() blocks ~47ms on I2C. Running it on Core 0 keeps Core 1 (physics/audio) free.
// Main loop fills the u8g2 buffer (< 1ms) then wakes this task; display latency ≤ 100ms,
// but the physics tick runs uninterrupted at 10ms throughout.
static void displayTask(void*) {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(200));
        oled.sendBuffer();
        if (s_oledDone) xSemaphoreGive(s_oledDone);
    }
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
            case MODE_ANIM: {
                // Rainbow wave synced to animation speed
                float aspeed = pots[0].value * 0.8f + 0.2f;
                uint8_t hoff = (uint8_t)(millis() * aspeed * 0.05f);
                for (int i = 0; i < NUM_LEDS; i++)
                    leds[i] = CHSV((uint8_t)(hoff + i * 255 / NUM_LEDS), 200, 60 + animIdx * 18);
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
                    if (drum2View == 1) {
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
                    uint8_t note = noteMap.getMidiNote(r, c);
                    bool playing = false;
                    for(uint8_t i=0;i<syseqActiveCnt;i++) if(syseqActive[i]==note) { playing=true; break; }
                    if (keyState[r][c])    leds[li] = CHSV(170, 200, 255);
                    else if (playing)      leds[li] = CHSV(170, 200, 120);
                    else                   leds[li] = CHSV(170, 255,  15);
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
            case MODE_303S2: {
                for(int r=0;r<KBD_NOTE_ROWS;r++) for(int c=0;c<KBD_COLS;c++){
                    int gr=KBD_ROWS-1-r, gc=KBD_COLS-1-c, idx=gr*KBD_COLS+gc;
                    int li=(idx>=0&&idx<NUM_LEDS+4)?crdToIdx(idx,0):-1;
                    if(li<0||li>=NUM_LEDS) continue;
                    bool isRest=(r==3&&c==0);
                    if(keyState[r][c])       leds[li]=CHSV(0,0,255);
                    else if(isRest)          leds[li]=CHSV(0,240,80);   // REST key: dim red
                    else {
                        uint8_t note2=noteMap.getMidiNote(r,c);
                        // Current playing note: 100%, notes in sequence: 50%, others: dim
                        bool isCurNote = drum2Playing && s303s2CurNote>0 && s303s2CurNote==
                            (uint8_t)constrain((int)note2+(int)t303Oct*12,0,127);
                        bool inSeq=false;
                        if (!isCurNote && s303s2Count>0) {
                            for(uint8_t si=0;si<s303s2Count;si++){
                                uint8_t sidx=(uint8_t)((s303s2Head-s303s2Count+si+S303S2_LEN)%S303S2_LEN);
                                if(s303s2Seq[sidx]>0 && (s303s2Seq[sidx]-1)==note2){inSeq=true;break;}
                            }
                        }
                        if(isCurNote)   leds[li]=CHSV(160,230,255); // 100% — playing now
                        else if(inSeq)  leds[li]=CHSV(160,200,110); // 50%  — in sequence
                        else            leds[li]=CHSV(0,0,4);        // dim
                    }
                }
                break;
            }
            case MODE_GEST: {
                // 4 rows = DRUMS/303S/SYNS/SAMPS, 8 cols = 8 patterns
                // Hue per sequencer: DRUMS=160(blue), 303S=85(green), SYNS=21(orange), SAMPS=213(purple)
                static const uint8_t kGestHue[GEST_NSEQ] = {160, 85, 21, 213};
                uint32_t nowMs = millis();
                for (int r=0; r<KBD_NOTE_ROWS; r++) for (int c=0; c<KBD_COLS; c++) {
                    int gr=KBD_ROWS-1-r, gc=KBD_COLS-1-c;
                    int idx=gr*KBD_COLS+gc;
                    int li=(idx>=0&&idx<NUM_LEDS+4)?crdToIdx(idx,0):-1;
                    if(li<0||li>=NUM_LEDS) continue;
                    uint8_t seqIdx = (uint8_t)(KBD_NOTE_ROWS-1-r);
                    uint8_t pat    = (uint8_t)(KBD_COLS-1-c);
                    uint8_t hue = kGestHue[seqIdx];
                    bool isActive = (gestActPat[seqIdx]==pat);
                    bool isSel    = (gestSelRow==seqIdx && gestSelCol==pat);
                    bool isFilled = g_patFilled[seqIdx][pat];
                    bool isPlaying= drum2Playing && isActive;
                    uint8_t bri;
                    if (isPlaying && isSel) bri = (uint8_t)(180 + ((nowMs/300)&1)*75);
                    else if (isPlaying)     bri = 200;
                    else if (isSel)        bri = 160;
                    else if (isFilled)     bri = 80;
                    else                   bri = 12;
                    leds[li] = CHSV(hue, isFilled?220:60, bri);
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
            case MODE_GRANULAR2: {
                // Per-sample hue: S0=green, S1=cyan, S2=blue, S3=magenta; fwd=brighter, rev=dimmer
                static const uint8_t g2Hues[4] = {85, 128, 170, 213};
                uint8_t g2NumSamp = gran2NumSamples();
                // NRM/SEQ/SQL: dim idle glow shows loaded samples. LOOP/FULL: dark unless pressed.
                bool g2IdleOn = (gran2PlayMode == 0 || gran2PlayMode == 3 || gran2PlayMode == 4);
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
                    uint8_t bri;
                    if (gran2PlayMode == 3) {
                        // SEQ: 100% = currently playing head; 50% = in queue; idle glow otherwise
                        bool isPlaying = false, isQueued = false;
                        if (g_gran2SeqHead != g_gran2SeqTail) {
                            const Gran2SeqEntry& cur = g_gran2SeqQueue[g_gran2SeqHead];
                            if (cur.sampleIdx == si && cur.sliceIdx == sl && cur.reverse == rev) isPlaying = true;
                        }
                        for (uint8_t qi = (g_gran2SeqHead + 1) % GRAN2_SEQ_MAX; qi != g_gran2SeqTail; qi = (qi + 1) % GRAN2_SEQ_MAX) {
                            const Gran2SeqEntry& qe = g_gran2SeqQueue[qi];
                            if (qe.sampleIdx == si && qe.sliceIdx == sl && qe.reverse == rev) { isQueued = true; break; }
                        }
                        bri = isPlaying ? 255u : isQueued ? 128u : (rev ? 20u : 40u);
                    } else if (gran2PlayMode == 4) {
                        // SQL: 100% = currently playing; 50% = in loop; idle glow otherwise
                        bool isPlaying = false, isQueued = false;
                        if (g_gran2SqlCount > 0) {
                            const Gran2SeqEntry& cur = g_gran2SqlLoop[g_gran2SqlPlayHead];
                            if (cur.sampleIdx == si && cur.sliceIdx == sl && cur.reverse == rev) isPlaying = true;
                        }
                        for (uint8_t qi = 0; qi < g_gran2SqlCount; qi++) {
                            if (qi == g_gran2SqlPlayHead) continue;
                            const Gran2SeqEntry& qe = g_gran2SqlLoop[qi];
                            if (qe.sampleIdx == si && qe.sliceIdx == sl && qe.reverse == rev) { isQueued = true; break; }
                        }
                        bri = isPlaying ? 255u : isQueued ? 128u : (rev ? 20u : 40u);
                    } else {
                        bri = keyState[r][c] ? 255u : (g2IdleOn ? (rev ? 20u : 40u) : 0u);
                    }
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
            case MODE_SAMPLE: {
                for(int r=0;r<KBD_NOTE_ROWS;r++) for(int c=0;c<KBD_COLS;c++){
                    int gr=KBD_ROWS-1-r, gc=KBD_COLS-1-c, idx=gr*KBD_COLS+gc;
                    int li=(idx>=0&&idx<NUM_LEDS+4)?crdToIdx(idx,0):-1;
                    if(li<0||li>=NUM_LEDS) continue;
                    uint8_t kidx=(uint8_t)(r*8+c);
                    bool loaded = audioKeyLoaded(kidx) && sampleMap[r][c].length()>0;
                    if (samplePlayMode==4 && g_sampleSqlCount>0) {
                        // SQL: 100%=currently playing, 50%=in loop, dim=loaded, off=empty
                        bool isPlaying = false, isQueued = false;
                        const SampleSqlEntry& cur = g_sampleSqlLoop[g_sampleSqlPlayHead];
                        if (cur.row==(uint8_t)r && cur.col==(uint8_t)c) isPlaying=true;
                        for(uint8_t qi=0;qi<g_sampleSqlCount;qi++){
                            if(qi==g_sampleSqlPlayHead) continue;
                            if(g_sampleSqlLoop[qi].row==(uint8_t)r&&g_sampleSqlLoop[qi].col==(uint8_t)c){isQueued=true;break;}
                        }
                        uint8_t bri = isPlaying?255u:isQueued?128u:(loaded?25u:0u);
                        leds[li] = CHSV(80, 220, bri);
                    } else if(loaded) {
                        leds[li] = keyState[r][c] ? CHSV(80,255,255) : CHSV(80,200,40);
                    } else {
                        leds[li] = keyState[r][c] ? CRGB(CHSV(80,255,120)) : CRGB::Black;
                    }
                }
                break;
            }
            case MODE_POKEMON: {
                const PokemonDef& pkL = kPokemon[pkmnSelected];
                for (int r = 0; r < KBD_NOTE_ROWS; r++) for (int c = 0; c < KBD_COLS; c++) {
                    int gr = KBD_ROWS-1-r, gc = KBD_COLS-1-c, idx = gr*KBD_COLS+gc;
                    int li = (idx>=0 && idx<NUM_LEDS+4) ? crdToIdx(idx,0) : -1;
                    if (li<0 || li>=NUM_LEDS) continue;
                    uint8_t note = noteMap.getMidiNote(r, c);
                    uint8_t hue  = pkL.pal[note % 4];
                    if (keyState[r][c])
                        leds[li] = CHSV(hue, 230, 255);  // pressed: full bright
                    else
                        leds[li] = CHSV(hue, 200, 16);   // idle: dim glow in Pokémon palette
                }
                break;
            }
            case MODE_EXP: {
                // Cols 0-3,5-6: modifier groups (active row = full brightness)
                // Col 4: FX multi-toggle; Col 7: arp speed meter (posX as 4-step bar)
                // Hues: Wave=0 Env=60 Arp=170 Gate=30 FX=200 Scale=100 Oct=40 ArpSpd=170
                static const uint8_t kExpColHue[] = {0, 60, 170, 30, 200, 100, 40, 170};
                // Col7: how many rows to light = expPosX mapped to 0-4
                uint8_t arpSpdRows = (uint8_t)(expPosX * 4.0f + 0.5f);  // 0-4, 0=slowest
                for (int r=0;r<KBD_NOTE_ROWS;r++) for (int c=0;c<KBD_COLS;c++) {
                    int gr=KBD_ROWS-1-r, gc=KBD_COLS-1-c, idx=gr*KBD_COLS+gc;
                    int li=(idx>=0&&idx<NUM_LEDS+4)?crdToIdx(idx,0):-1;
                    if (li<0||li>=NUM_LEDS) continue;
                    uint8_t h = kExpColHue[c];
                    bool active;
                    if (c == 4)      active = (bool)((expFxMask >> r) & 1);  // FX multi-toggle
                    else if (c == 7) active = ((uint8_t)r < arpSpdRows);     // arp speed meter
                    else             active = (expColSel[c] == (uint8_t)r);
                    // Flash the active note row in oct column when playing
                    if (c == 6 && expNoteOn) {
                        uint8_t noteRow = (uint8_t)((expCurNote - 12) / 12);
                        if (noteRow % 4 == (uint8_t)r) { leds[li]=CHSV(h,200,255); continue; }
                    }
                    leds[li] = active ? CHSV(h,230,220) : CHSV(h,200,25);
                }
                break;
            }
            case MODE_LANIM: {
                static const char* kLanimNames[] = {"FLASH","RBOW","CHSE","NOIS","ORGA"};
                uint8_t ai = lanimIdx % 5;
                uint32_t nowL = millis();
                float spd = pots[0].value * 2.0f + 0.1f;
                float p2  = pots[1].value;
                uint8_t hue = (uint8_t)(pots[2].value * 255.0f);
                uint8_t bri = (uint8_t)(50 + pots[3].value * 205.0f);
                if (ai == 0) {  // FLASH — random flashes
                    static uint32_t lFlashMs[NUM_LEDS]  = {};
                    static uint8_t  lFlashHue[NUM_LEDS] = {};
                    static uint32_t lFlashLast = 0;
                    uint32_t rate = max((uint32_t)5, (uint32_t)(150.0f / (spd * (p2 * 0.9f + 0.1f) + 0.01f)));
                    if (nowL - lFlashLast >= rate) {
                        lFlashLast = nowL;
                        for (int li = 0; li < NUM_LEDS; li++) {
                            if ((uint32_t)random(100) < (uint32_t)(p2 * 40 + 5)) {
                                lFlashMs[li]  = nowL;
                                lFlashHue[li] = hue + (uint8_t)(random(60) - 30);
                            }
                        }
                    }
                    for (int li = 0; li < NUM_LEDS; li++) {
                        uint32_t age = nowL - lFlashMs[li];
                        if (age < 120) {
                            uint8_t fb = (uint8_t)((1.0f - age / 120.0f) * bri);
                            leds[li] = CHSV(lFlashHue[li], 200, fb);
                        } else {
                            leds[li] = CRGB::Black;
                        }
                    }
                } else if (ai == 1) {  // RBOW — rainbow sweep
                    uint8_t hoff = (uint8_t)(nowL * spd * 0.04f);
                    uint8_t spread = (uint8_t)(p2 * 200 + 30);
                    for (int li = 0; li < NUM_LEDS; li++)
                        leds[li] = CHSV((uint8_t)(hoff + hue + li * spread / NUM_LEDS), 220, bri);
                } else if (ai == 2) {  // CHSE — chase
                    static float chasePos = 0.0f;
                    chasePos = fmodf(chasePos + spd * 0.25f, (float)NUM_LEDS);
                    uint8_t tail = (uint8_t)(p2 * 14 + 2);
                    for (int li = 0; li < NUM_LEDS; li++) leds[li] = CRGB::Black;
                    for (uint8_t t = 0; t < tail; t++) {
                        int idx = ((int)chasePos - t + NUM_LEDS) % NUM_LEDS;
                        leds[idx] = CHSV(hue + t * 6, 255, (uint8_t)(bri * (tail - t) / tail));
                    }
                } else if (ai == 3) {  // NOIS — evolving noise
                    static float lNoise[NUM_LEDS] = {};
                    static uint32_t lNoiseLast = 0;
                    if (nowL - lNoiseLast >= max((uint32_t)5, (uint32_t)(30.0f / (spd + 0.01f)))) {
                        lNoiseLast = nowL;
                        float rate = spd * 0.2f;
                        for (int li = 0; li < NUM_LEDS; li++) {
                            float tgt = (float)random(256) / 255.0f;
                            lNoise[li] += (tgt - lNoise[li]) * rate;
                        }
                    }
                    uint8_t sat = (uint8_t)(p2 * 220 + 30);
                    for (int li = 0; li < NUM_LEDS; li++) {
                        uint8_t b = (uint8_t)(lNoise[li] * bri);
                        leds[li] = CHSV(hue + (uint8_t)(lNoise[li] * 50), sat, b);
                    }
                } else {  // ORGA — organic breathing (golden-ratio phase offsets)
                    float t = nowL * 0.001f * spd;
                    float spread = p2 * 4.0f + 1.0f;
                    for (int li = 0; li < NUM_LEDS; li++) {
                        float phase = t + li * (6.2832f / NUM_LEDS) * spread;
                        float val = (sinf(phase) * 0.5f + sinf(phase * 1.618f) * 0.3f + sinf(phase * 2.618f) * 0.2f + 1.0f) * 0.5f;
                        val = constrain(val, 0.0f, 1.0f);
                        leds[li] = CHSV(hue + (uint8_t)(val * 40), 210, (uint8_t)(val * bri));
                    }
                }
                break;
            }
            case MODE_EXP2: {
                // Semantic hues per column: Scale/Oct/Balls/Wave/Env/Bounce/Sides/FX
                static const uint8_t kE2Hue[] = {96, 136, 25, 48, 200, 160, 220, 0};
                uint32_t nowL2 = millis();
                // Expire wall flashes
                for (int w=0;w<EXP2_MAX_SIDES;w++)
                    if (exp2WallFlash[w] && (nowL2 - exp2WallFlashMs[w] >= 120)) exp2WallFlash[w] = false;

                for (int c=0; c<KBD_COLS; c++) {
                    // Check if this column has an active wall flash
                    bool flash = false; uint8_t flashHue = 0;
                    for (int w=0; w<(int)exp2NumSides; w++) {
                        if (exp2WallFlash[w] && (w % KBD_COLS) == c) {
                            flash = true; flashHue = (uint8_t)(w * 255 / EXP2_MAX_SIDES); break;
                        }
                    }
                    for (int r=0; r<KBD_NOTE_ROWS; r++) {
                        int gr=KBD_ROWS-1-r, gc=KBD_COLS-1-c;
                        int idx=gr*KBD_COLS+gc;
                        int li=(idx>=0&&idx<NUM_LEDS+4)?crdToIdx(idx,0):-1;
                        if (li<0||li>=NUM_LEDS) continue;
                        if (flash) {
                            leds[li] = CHSV(flashHue, 200, 255);
                        } else if (c == 2) {  // Balls: bar indicator (rows 0..count-1 lit)
                            leds[li] = (r < (int)exp2BallCount) ? CHSV(kE2Hue[c],220,210) : CHSV(kE2Hue[c],150,12);
                        } else if (c == 7) {  // FX: bitmask — each row = one FX bit
                            bool act = (exp2FxMask >> r) & 1;
                            leds[li] = CHSV(kE2Hue[c], act?220:80, act?220:12);
                        } else {  // Cols 0-1, 3-6: single selection indicator
                            bool sel = ((uint8_t)exp2ColSel[c] == (uint8_t)r);
                            leds[li] = CHSV(kE2Hue[c], sel?230:120, sel?215:12);
                        }
                    }
                }
                break;
            }
            case MODE_EXP3: {
                // 8 cols = 8 balls; 4 rows = 4 orbits. Active ball's orbit row = full brightness.
                // Brightness pulses as ball approaches trigger zone (top of screen).
                static const uint8_t kBallHue[] = {0,16,64,96,128,160,200,240};
                for (int b=0;b<EXP3_MAX_BALLS;b++) {
                    bool act = exp3Balls[b].active;
                    float da = act ? (exp3Balls[b].angle - 3.0f*(float)M_PI/2.0f) : (float)M_PI;
                    while (da > (float)M_PI)  da -= 2.0f*(float)M_PI;
                    while (da < -(float)M_PI) da += 2.0f*(float)M_PI;
                    float pulse = act ? (1.0f - fabsf(da)/(float)M_PI) : 0.0f;
                    for (int r=0;r<KBD_NOTE_ROWS;r++) {
                        int gr=KBD_ROWS-1-r, gc=KBD_COLS-1-b, idx=gr*KBD_COLS+gc;
                        int li=(idx>=0&&idx<NUM_LEDS+4)?crdToIdx(idx,0):-1;
                        if (li<0||li>=NUM_LEDS) continue;
                        bool thisOrbit = act && (exp3Balls[b].orbitRow == (uint8_t)r);
                        uint8_t bri = thisOrbit ? (uint8_t)(25+pulse*230.0f) : (act ? 10 : 4);
                        leds[li] = CHSV(kBallHue[b], act?220:60, bri);
                    }
                }
                break;
            }
            case MODE_LIFE: {
                // 1:1 cell->key LED mapping: alive=bright hue-by-column, dead=dim.
                static const uint8_t kLifeColHue[] = {0,32,64,96,128,160,192,224};
                for (int r=0;r<KBD_NOTE_ROWS;r++) for (int c=0;c<KBD_COLS;c++) {
                    int gr=KBD_ROWS-1-r, gc=KBD_COLS-1-c, idx=gr*KBD_COLS+gc;
                    int li=(idx>=0&&idx<NUM_LEDS+4)?crdToIdx(idx,0):-1;
                    if (li<0||li>=NUM_LEDS) continue;
                    leds[li] = lifeGrid[r][c] ? CHSV(kLifeColHue[c], 220, 220) : CHSV(kLifeColHue[c], 150, 8);
                }
                break;
            }
            case MODE_SWARM: {
                // Column = boid index. Active+in-zone = bright flash, active = mid, inactive = dim.
                static const uint8_t kBoidHue[] = {0,32,64,96,128,160,192,224};
                for (int b=0;b<SWARM_MAX_BOIDS;b++) {
                    uint8_t bri = swarmBoids[b].inZone ? 240 : (swarmBoids[b].active ? 90 : 6);
                    for (int r=0;r<KBD_NOTE_ROWS;r++) {
                        int gr=KBD_ROWS-1-r, gc=KBD_COLS-1-b, idx=gr*KBD_COLS+gc;
                        int li=(idx>=0&&idx<NUM_LEDS+4)?crdToIdx(idx,0):-1;
                        if (li<0||li>=NUM_LEDS) continue;
                        leds[li] = CHSV(kBoidHue[b], 220, bri);
                    }
                }
                break;
            }
            case MODE_GEN: {
                // Hue = which voice is selected (so the sound-making choice is visible at a
                // glance); the generator's current column lights up bright, the rest dim.
                // Manually-held keys light independently so hand-played notes are visible too.
                static const uint8_t kVoiceHue[] = {32, 160, 0}; // Pluck=amber Pad=blue Glitch=red
                uint8_t hue = kVoiceHue[genVoice % GEN_VOICE_COUNT];
                for (int r=0;r<KBD_NOTE_ROWS;r++) for (int c=0;c<KBD_COLS;c++) {
                    int gr=KBD_ROWS-1-r, gc=KBD_COLS-1-c, idx=gr*KBD_COLS+gc;
                    int li=(idx>=0&&idx<NUM_LEDS+4)?crdToIdx(idx,0):-1;
                    if (li<0||li>=NUM_LEDS) continue;
                    bool held = activeNotes[r][c] != 0;
                    bool genCol = ((uint8_t)c == genLastCol);
                    uint8_t bri = held ? 255 : (genCol ? 200 : 10);
                    leds[li] = CHSV(hue, 220, bri);
                }
                break;
            }
            case MODE_DR2: {
                // Left half (cols 4-7) = sequencer: row0=beat, row1=step (bright=touched
                // last, mid=in multi-select batch, dim=has hits for the focused instrument,
                // black=empty; red flash=playhead), row2=micro on/off for the focused
                // instrument's selected pad/note (drums: color=alteration mode; synth/303:
                // a fixed instrument color; red flash on playhead), row3=4-slot pattern bank
                // (moved here from the instrument side).
                // Right half (cols 0-3) = row3: instrument select (Drums/Synth/303/—), in
                // place of the old pattern buttons.
                // Drums focused: row0=pads 0-3, row1=pads 4-7, row2=alteration mode select.
                // Synth/303 focused: rows0-2 = one chromatic octave (12 keys).
                static const uint8_t kModHue[4] = {96, 224, 0, 208}; // NRM=green 50%=pink RND=red DBL=purple
                static const uint8_t kInstrHue[DR2_INSTR_COUNT] = {96, 160, 208, 0}; // Drums=green Synth=blue 303=purple TBD=none
                uint8_t curHue = dr2Instrument==DR2_INSTR_DRUMS ? 32
                                : dr2Instrument==DR2_INSTR_SYNTH ? kInstrHue[DR2_INSTR_SYNTH]
                                : dr2Instrument==DR2_INSTR_T303  ? kInstrHue[DR2_INSTR_T303]
                                                                  : 32;
                for (int r=0;r<KBD_NOTE_ROWS;r++) for (int c=0;c<KBD_COLS;c++) {
                    int gr=KBD_ROWS-1-r, gc=KBD_COLS-1-c, idx=gr*KBD_COLS+gc;
                    if (idx<0||idx>=NUM_LEDS+4) continue;
                    int li=crdToIdx(idx,0);
                    if (li<0||li>=NUM_LEDS) continue;
                    CRGB col = CRGB::Black;
                    bool leftHalf = (c>=4);
                    uint8_t lidx = leftHalf ? (uint8_t)(7-c) : (uint8_t)(3-c);
                    if (leftHalf) {
                        if (r==0) {
                            // In "all" mode every beat is equally part of the batch edit, so
                            // all 4 light identically — no single one stays visually "more
                            // selected" than the rest (see dr2BeatSelMask's row0 toggle above).
                            bool sel=(dr2BeatSelMask==0xF)||(lidx==dr2SelBeat), multiSel=(dr2BeatSelMask&(1<<lidx));
                            bool playing=dr2Playing&&(lidx==dr2PlayBeat);
                            bool hasHits=false;
                            if (dr2Instrument==DR2_INSTR_DRUMS)
                                for(uint8_t s=0;s<DR2H_STEPS&&!hasHits;s++) for(uint8_t m=0;m<DR2H_MICROS;m++) if(dr2Vel[dr2SelPad][lidx][s][m]){hasHits=true;break;}
                            else if (dr2Instrument==DR2_INSTR_SYNTH)
                                for(uint8_t s=0;s<DR2H_STEPS&&!hasHits;s++) for(uint8_t m=0;m<DR2H_MICROS;m++) if(dr2SynthVel[dr2SelNote][lidx][s][m]){hasHits=true;break;}
                            else if (dr2Instrument==DR2_INSTR_T303)
                                for(uint8_t s=0;s<DR2H_STEPS&&!hasHits;s++) for(uint8_t m=0;m<DR2H_MICROS;m++) if(dr2T303Vel[dr2SelNote][lidx][s][m]){hasHits=true;break;}
                            col = playing ? CHSV(0,255,255)
                                : sel      ? CHSV(160,255,220)
                                : multiSel ? CHSV(160,255,120)
                                : hasHits  ? CHSV(160,180,40)
                                           : CHSV(160,100,12);
                        } else if (r==1) {
                            // Same reasoning as row0: all 4 look identical when in "all" mode.
                            bool sel=(dr2StepSelMask==0xF)||(lidx==dr2SelStep), multiSel=(dr2StepSelMask&(1<<lidx));
                            bool playing=dr2Playing&&(lidx==dr2PlayStep)&&(dr2SelBeat==dr2PlayBeat);
                            bool hasHits=false;
                            if (dr2Instrument==DR2_INSTR_DRUMS)
                                for(uint8_t m=0;m<DR2H_MICROS;m++) if(dr2Vel[dr2SelPad][dr2SelBeat][lidx][m]){hasHits=true;break;}
                            else if (dr2Instrument==DR2_INSTR_SYNTH)
                                for(uint8_t m=0;m<DR2H_MICROS;m++) if(dr2SynthVel[dr2SelNote][dr2SelBeat][lidx][m]){hasHits=true;break;}
                            else if (dr2Instrument==DR2_INSTR_T303)
                                for(uint8_t m=0;m<DR2H_MICROS;m++) if(dr2T303Vel[dr2SelNote][dr2SelBeat][lidx][m]){hasHits=true;break;}
                            col = playing ? CHSV(0,255,255)
                                : sel      ? CHSV(96,255,220)
                                : multiSel ? CHSV(96,255,120)
                                : hasHits  ? CHSV(96,180,40)
                                           : CHSV(96,100,12);
                        } else if (r==2) {
                            bool on=false; uint8_t hue=curHue;
                            if (dr2Instrument==DR2_INSTR_DRUMS) {
                                on = (dr2Vel[dr2SelPad][dr2SelBeat][dr2SelStep][lidx]>0);
                                uint8_t modv = dr2Mod[dr2SelPad][dr2SelBeat][dr2SelStep][lidx];
                                // Same hue mapping as the alteration-mode selector (right half
                                // row2) so a placed hit's color tells you which mode it was
                                // stamped with (NRM/50%/RND/DBL) at a glance.
                                hue = on ? kModHue[modv] : 32;
                            } else if (dr2Instrument==DR2_INSTR_SYNTH) {
                                on = (dr2SynthVel[dr2SelNote][dr2SelBeat][dr2SelStep][lidx]>0);
                            } else if (dr2Instrument==DR2_INSTR_T303) {
                                on = (dr2T303Vel[dr2SelNote][dr2SelBeat][dr2SelStep][lidx]>0);
                            }
                            bool playing=dr2Playing&&(lidx==dr2PlayMicro)&&(dr2SelBeat==dr2PlayBeat)&&(dr2SelStep==dr2PlayStep);
                            col = playing ? (on?CHSV(0,255,255):CHSV(0,140,90))
                                          : (on?CHSV(hue,255,220):CHSV(32,120,12));
                        } else { // r==3: pattern slot select (moved here from the instrument side)
                            bool active=(lidx==dr2ActivePat);
                            bool filled=dr2PatFilled[lidx];
                            bool copySrc=(dr2Copied && lidx==dr2CopyPat);
                            col = active   ? CHSV(180,255,220)
                                : copySrc  ? CHSV(50,255,160)
                                : filled   ? CHSV(180,220,60)
                                           : CHSV(180,100,12);
                        }
                    } else {
                        if (r==3) {
                            bool sel=(lidx==dr2Instrument);
                            uint8_t hue=kInstrHue[lidx];
                            col = (lidx==DR2_INSTR_TBD) ? (sel?CHSV(0,0,90):CHSV(0,0,20))
                                : sel ? CHSV(hue,255,220) : CHSV(hue,180,30);
                        } else if (dr2Instrument==DR2_INSTR_DRUMS) {
                            if (r==0 || r==1) {
                                uint8_t padIdx=(uint8_t)(r==0?lidx:lidx+4);
                                if (padIdx<DRUM2_PADS) {
                                    bool sel = (padIdx==dr2SelPad);
                                    bool flash = (millis()-dr2PadFlashMs[padIdx] < 80);
                                    col = flash ? CHSV(0,0,255) : (sel ? CHSV(96,255,200) : CHSV(96,180,40));
                                }
                            } else { // r==2: alteration mode select
                                bool sel=(lidx==dr2PlaceMod);
                                col = sel ? CHSV(kModHue[lidx],255,220) : CHSV(kModHue[lidx],180,30);
                            }
                        } else if (dr2Instrument==DR2_INSTR_SYNTH || dr2Instrument==DR2_INSTR_T303) {
                            uint8_t noteIdx=(uint8_t)(r*4+lidx);
                            if (noteIdx<DR2_NOTES) {
                                bool sel = (noteIdx==dr2SelNote);
                                bool flash = (millis()-dr2NoteFlashMs[noteIdx] < 80);
                                bool hasHits=false;
                                const uint8_t (*arr)[DR2H_BEATS][DR2H_STEPS][DR2H_MICROS] =
                                    dr2Instrument==DR2_INSTR_SYNTH ? dr2SynthVel : dr2T303Vel;
                                for(uint8_t b=0;b<DR2H_BEATS&&!hasHits;b++) for(uint8_t s=0;s<DR2H_STEPS&&!hasHits;s++) for(uint8_t m=0;m<DR2H_MICROS;m++) if(arr[noteIdx][b][s][m]){hasHits=true;break;}
                                col = flash ? CHSV(0,0,255)
                                    : sel   ? CHSV(curHue,255,200)
                                    : hasHits ? CHSV(curHue,220,60)
                                              : CHSV(curHue,180,12);
                            }
                        }
                        // DR2_INSTR_TBD: rows0-2 stay black.
                    }
                    leds[li]=col;
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
        bool ovl4col = (s_overlay == OVERLAY_SCALE_ARP || s_overlay == OVERLAY_303 || s_overlay == OVERLAY_303_PRESET || s_overlay == OVERLAY_FX);
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
                    if (opt < 5)  { show=true; sel=((uint8_t)samplePlayMode==opt); hue=60; }
                    else if (opt==5) { show=true; sel=false; hue=0; }
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
// Physics-triggered notes (EXP2 bounces etc.) use s_noteDirectQueue so the main
// loop never blocks on amy_queue_lock while AMY render holds it.
static QueueHandle_t s_audioEventQueue  = nullptr;
static QueueHandle_t s_noteDirectQueue  = nullptr;  // physics-triggered note on/off
static TaskHandle_t  s_audioHandlerHandle = nullptr;

struct NoteEvent { uint8_t note; float vel; };  // vel==0 → note off

// Non-blocking: post a note-on/off from physics (main loop, Core 1).
// No xTaskNotifyGive here — cross-core IPC spinlock blocks Core 1 if Core 0 is in a
// critical section (AMY mutex). audioHandlerTask polls s_noteDirectQueue on a 15ms timer.
static void audioPostNote(uint8_t note, float vel) {
    if (!s_noteDirectQueue || !audioReady) return;
    NoteEvent ne = {note, vel};
    xQueueSend(s_noteDirectQueue, &ne, 0);  // drop if full (rare: queue=16 slots)
}

static void kbdEventBridge(uint8_t row, uint8_t col, bool pressed)
{
    if (s_audioEventQueue) {
        KeyEvent evt = {row, col, pressed};
        xQueueSend(s_audioEventQueue, &evt, 0);  // non-blocking
        if (s_audioHandlerHandle) xTaskNotifyGive(s_audioHandlerHandle);
    }
    if (s_ledSem) xSemaphoreGive(s_ledSem);
}

// ==================== NOTE KEY AUDIO HANDLER ====================
// ── MIDI + audio wrappers ─────────────────────────────────────────────────────
// Replace direct audioNoteOn/Off / audioT303NoteOn/Off / audioDrum2Hit calls
// with these so all playing modes emit USB MIDI alongside audio output.
static inline void playNoteOn(uint8_t n, float v) {
    audioNoteOn(n, v);
    midiNoteOn(n, (uint8_t)constrain((int)(v * 127), 1, 127), MIDI_CH_SYNTH);
}
static inline void playNoteOff(uint8_t n) {
    audioNoteOff(n);
    midiNoteOff(n, MIDI_CH_SYNTH);
}
static inline void playBassOn(uint8_t n, float v) {
    audioT303NoteOn(n, v);
    midiNoteOn(n, (uint8_t)constrain((int)(v * 100), 1, 127), MIDI_CH_BASS);
}
static inline void playBassOff(uint8_t n) {
    audioT303NoteOff(n);
    midiNoteOff(n, MIDI_CH_BASS);
}
static inline void playI303On(uint8_t n, float v) {
    audioT303NoteOn(n, v);
    midiNoteOn(n, (uint8_t)constrain((int)(v*127),1,127), MIDI_CH_BASS);
}
static inline void playI303Off(uint8_t n) {
    audioT303NoteOff(n);
    midiNoteOff(n, MIDI_CH_BASS);
}
static inline void playDrum(uint8_t pi, float v, uint8_t pitch, float dec) {
    audioDrum2Hit(kDrum2Remap[pi], v, pitch, dec);
    midiDrum(pi, (uint8_t)constrain((int)(v * 127), 1, 127));
    if (currentMode == MODE_DRUM2 && drum2View == 2 && pi < draniNumOverlays) draniDrumHitMs[pi] = millis();
}

// Called from audioHandlerTask (Core 1, priority 22).
// Running on Core 1 keeps AMY state writes on the same core as before,
// avoiding races with AMY render (Core 0). Priority 22 beats the main loop (1)
// so events are processed as soon as AMY fill buffer (23) yields.
// Whether it's safe to let note-row key presses reach handleNoteKeyAudio's per-mode
// switch below while the FX overlay is open. For a plain instrument mode this just
// previews a sound (harmless, even wanted — see the OVERLAY_FX exemption below). But
// several modes reuse the SAME note-grid keys to edit a step-sequencer/pattern grid
// (GEST picks a pattern slot and auto-saves the live one into it; DRUM2/303S/SS2's
// SEQ view toggles a step) — for those, letting a key press through while adjusting
// an FX silently mutates the sequence "underneath" the FX menu, which is exactly the
// bug report this guards against. Modes not listed here (GEST, 303S2, SYSEQ, TRACKER,
// DR2 chief among them) have no view where pressing a note key is side-effect-free,
// so they're excluded entirely rather than guessing a safe sub-state.
static bool notePreviewSafeDuringFxOverlay() {
    switch (currentMode) {
        case MODE_SYNTH: case MODE_STONE: case MODE_OMNI: case MODE_SAMPLE:
        case MODE_I303:  case MODE_MOD2:  case MODE_MODULAR: case MODE_GRANULAR2:
        case MODE_POKEMON:
            return true;
        case MODE_DRUM2: return drum2View != 1;   // PAD/ANIM views preview; SEQ view edits steps
        case MODE_303S:  return !s303SeqView;      // PAD view previews; SEQ view edits steps
        case MODE_SS2:   return !ss2SeqView;        // PAD view previews; SEQ view edits steps
        default: return false;
    }
}

static void handleNoteKeyAudio(uint8_t row, uint8_t col, bool pressed)
{
    // OVERLAY_FX and OVERLAY_FX_MOD are deliberately exempted (for modes where it's safe
    // — see notePreviewSafeDuringFxOverlay() above): both are designed to "stay open for
    // multi-toggle / live tweak" (see overlayKeyPress's OVERLAY_FX and OVERLAY_FX_MOD
    // cases) so pots/joystick can keep adjusting an FX's (or its automation's) params
    // while it's open — but with every overlay silently blocking note playback here, a
    // user could never actually HEAR the effect while dialing it in,
    // since pressing a note key produced no sound at all (and, via overlayKeyPress's
    // colMin check below, silently closed the overlay too — so by the time a pot got
    // touched, the overlay was already gone and pots fell back to whatever the mode's
    // own params are).
    // col>=4 is excluded even when otherwise "safe": those are the FX overlay's own
    // 4x4 selection grid (16 slots, colMin=4 in overlayKeyPress) — a key press there
    // is picking/toggling which FX is selected, not a performance gesture, so it
    // shouldn't also fire whatever note that same physical key would play outside
    // the overlay.
    if (menuOpen || !audioReady ||
        (s_overlay != OVERLAY_NONE &&
         !((s_overlay == OVERLAY_FX || s_overlay == OVERLAY_FX_MOD) && col < 4 && notePreviewSafeDuringFxOverlay())))
        return;
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
                    float vnote = constrain((0.8f+jx*0.6f) * polyScale, 0.05f, 1.0f);
                    playNoteOn(note, vnote);
                    Serial.printf("N+:%d:%d\n", note, (int)(vnote*127));
                } else { playNoteOff(note); Serial.printf("N-:%d\n", note); }
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
        case MODE_STONE:{
            uint8_t note;
            if (kbdLayout == KBD_LAYOUT_PIANO) {
                note = pianoNote(row, col, (uint8_t)(48 + noteMap.getOctave()*12));
                if (note == 0xFF) return;
            } else {
                note = noteMap.getMidiNote(row, col);
            }
            activeNotes[row][col]=pressed?note:0;
            if(pressed){
                float jx=constrain(cachedJoyX/64.0f,-1.0f,1.0f);
                int held=0;
                for(int rr=0;rr<KBD_NOTE_ROWS;rr++) for(int cc=0;cc<KBD_COLS;cc++) if(activeNotes[rr][cc]) held++;
                float polyScale = 1.0f / sqrtf(fmaxf(1.0f, (float)held));
                float vnote = constrain((0.8f+jx*0.6f) * polyScale, 0.05f, 1.0f);
                audioStoneNoteOn(note, vnote);
                // audioStoneNoteOn() round-robin-steals one of STONE's 6 fixed oscillators
                // and sends it a note-on event — same bug class as MOD3's table-change fix
                // earlier this session: re-triggering a PCM voice can leave that oscillator's
                // filter state not matching what FILT last set (a stale/reused voice can end
                // up unfiltered even while FILT shows active), since audioApplyFilterToStone()
                // is only ever called when the FX itself changes, not on every new note. Cheap
                // to re-assert unconditionally here since it's just re-sending the same state.
                if (fxList[0].active) applyFxEffect(0);
            } else {
                audioStoneNoteOff(note);
            }
            break;
        }
        case MODE_OMNI:{
            if(pressed){
                // Release previous chord
                if(omniRoot!=0xFF) for(int i=0;i<3;i++) playNoteOff(omniChordNotes[i]);
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
                        playNoteOn(omniChordNotes[i],0.5f);
                    }
                }
            }
            break;
        }
        case MODE_SAMPLE:{
            uint8_t kidx=(uint8_t)(row*8+col);
            if(pressed){
                if(samplePlayMode==4){  // SQL: add to circular loop, overwriting oldest when full
                    if(sampleMap[row][col].length()>0 && audioKeyLoaded(kidx)){
                        bool wasEmpty = (g_sampleSqlCount == 0);
                        uint8_t writePos = g_sampleSqlWriteIdx % SAMPLE_SQL_MAX;
                        g_sampleSqlLoop[writePos] = {row, col};
                        g_sampleSqlWriteIdx++;
                        if (g_sampleSqlCount < SAMPLE_SQL_MAX) g_sampleSqlCount++;
                        if (wasEmpty) {
                            g_sampleSqlPlayHead = 0;
                            sampleSqlStartHead();
                        }
                    }
                } else if(sampleMap[row][col].length()>0){
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
        case MODE_I303: {
            uint8_t note;
            if (kbdLayout == KBD_LAYOUT_PIANO) {
                note = pianoNote(row, col, (uint8_t)(48 + noteMap.getOctave()*12));
                if (note == 0xFF) return;
            } else {
                note = noteMap.getMidiNote(row, col);
            }
            note = (uint8_t)constrain((int)note + t303Oct*12, 0, 127);
            activeNotes[row][col] = pressed ? note : 0;
            if (arpMode != 0) {
                if (pressed) {
                    bool found=false;
                    for(uint8_t i=0;i<arpNoteCount;i++) if(arpNotes[i]==note){found=true;break;}
                    if(!found&&arpNoteCount<32) arpNotes[arpNoteCount++]=note;
                } else {
                    for(uint8_t i=0;i<arpNoteCount;i++) if(arpNotes[i]==note){
                        memmove(&arpNotes[i],&arpNotes[i+1],(arpNoteCount-i-1));
                        arpNoteCount--; break;
                    }
                    if(arpNoteCount==0&&arpCurrent!=0){playI303Off(arpCurrent);arpCurrent=0;}
                }
            } else {
                if (pressed) {
                    float jx = constrain(cachedJoyX/64.0f,-1.0f,1.0f);
                    int held=0;
                    for(int rr=0;rr<KBD_NOTE_ROWS;rr++) for(int cc=0;cc<KBD_COLS;cc++) if(activeNotes[rr][cc]) held++;
                    float polyScale=1.0f/sqrtf(fmaxf(1.0f,(float)held));
                    float vel=constrain((0.8f+jx*0.6f)*polyScale,0.05f,1.0f);
                    playI303On(note, vel);
                } else {
                    playI303Off(note);
                }
            }
            break;
        }
        case MODE_DRUM2: {
            if (!pressed) break;
            if (drum2View == 2) {
                // Anim view: col = drum pad trigger (one key per column)
                uint8_t padIdx = (uint8_t)(KBD_COLS - 1 - col);
                if (padIdx < DRUM2_PADS) {
                    playDrum(padIdx, volume * drum2Volume[padIdx], drum2Pitch[padIdx], drum2Decay[padIdx]);
                    drum2PadFlashMs[padIdx] = millis();
                    drum2SelPad = (int8_t)padIdx;
                }
                break;
            }
            if (drum2View == 1) {
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
                    playDrum(padIdx, 0.70f * drum2Volume[padIdx],
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
                playDrum(padIdx, vel, pitch, dec);
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
        case MODE_DR2: {
            // Left half (cols 4-7, idx=KBD_COLS-1-col, left-to-right 0-3) = sequencer:
            //   row0=beat, row1=step: each is a simple 2-state toggle, not an arbitrary
            //   multi-select — pressing a DIFFERENT beat/step focuses it and narrows the
            //   batch-edit mask to just that one; pressing the ALREADY-focused one instead
            //   flips the mask between "just this one" and "all four" (there is no reachable
            //   in-between combination, e.g. 2-of-4).
            //   row2=micro toggle for the currently selected item (drum pad, or melodic
            //   note when Synth/303 is the focused instrument), applied across every
            //   currently-selected beat×step pair. row3=4-slot pattern bank (moved here
            //   from the instrument side, which needed the room for instrument select).
            // Right half (cols 0-3, idx=3-col, left-to-right 0-3) = instrument + its
            // per-instrument controls:
            //   row3=instrument select (Drums/Synth/303/—), in place of the old pattern
            //   buttons (moved to the left half's row3) — single click is UI focus only,
            //   every slot with content keeps sounding during playback regardless of
            //   which is focused; double-click clears that instrument's hits on the
            //   CURRENT pattern only (other instruments/patterns untouched).
            //   Drums focused: row0=pads 0-3, row1=pads 4-7, row2=alteration mode
            //   (NRM/50%/RND/DBL) stamped onto newly-placed hits.
            //   Synth/303 focused: rows0-2 together = one chromatic octave (12 keys) —
            //   press selects the note (dr2SelNote, via NoteMap's scale/octave) and previews it.
            //   Placeholder focused: rows0-2 do nothing yet.
            if (!pressed) break;
            bool leftHalf = (col >= 4);
            uint8_t idx = leftHalf ? (uint8_t)(7 - col) : (uint8_t)(3 - col);
            if (leftHalf) {
                if (row == 0) {
                    if (idx == dr2SelBeat) {
                        dr2BeatSelMask = (dr2BeatSelMask == 0xF) ? (uint8_t)(1 << idx) : 0xF;
                    } else {
                        dr2SelBeat = idx;
                        dr2BeatSelMask = (uint8_t)(1 << idx);
                    }
                } else if (row == 1) {
                    if (idx == dr2SelStep) {
                        dr2StepSelMask = (dr2StepSelMask == 0xF) ? (uint8_t)(1 << idx) : 0xF;
                    } else {
                        dr2SelStep = idx;
                        dr2StepSelMask = (uint8_t)(1 << idx);
                    }
                } else if (row == 2) {
                    if (dr2Instrument == DR2_INSTR_DRUMS) {
                        bool turnOn = (dr2Vel[dr2SelPad][dr2SelBeat][dr2SelStep][idx] == 0);
                        uint8_t newVal = turnOn ? 100 : 0;
                        uint8_t newMod = turnOn ? dr2PlaceMod : 0;
                        for (uint8_t b = 0; b < DR2H_BEATS; b++) {
                            if (!(dr2BeatSelMask & (1 << b))) continue;
                            for (uint8_t s = 0; s < DR2H_STEPS; s++) {
                                if (!(dr2StepSelMask & (1 << s))) continue;
                                dr2Vel[dr2SelPad][b][s][idx] = newVal;
                                dr2Mod[dr2SelPad][b][s][idx] = newMod;
                            }
                        }
                        if (turnOn) playDrum((uint8_t)dr2SelPad, 0.70f * drum2Volume[dr2SelPad], drum2Pitch[dr2SelPad], drum2Decay[dr2SelPad]);
                    } else if (dr2Instrument == DR2_INSTR_SYNTH) {
                        bool turnOn = (dr2SynthVel[dr2SelNote][dr2SelBeat][dr2SelStep][idx] == 0);
                        uint8_t newVal = turnOn ? 100 : 0;
                        for (uint8_t b = 0; b < DR2H_BEATS; b++) {
                            if (!(dr2BeatSelMask & (1 << b))) continue;
                            for (uint8_t s = 0; s < DR2H_STEPS; s++) {
                                if (!(dr2StepSelMask & (1 << s))) continue;
                                dr2SynthVel[dr2SelNote][b][s][idx] = newVal;
                            }
                        }
                        if (turnOn) { dr2TriggerSynth(dr2SelNote, 0.75f); dr2NoteFlashMs[dr2SelNote] = millis(); }
                    } else if (dr2Instrument == DR2_INSTR_T303) {
                        bool turnOn = (dr2T303Vel[dr2SelNote][dr2SelBeat][dr2SelStep][idx] == 0);
                        uint8_t newVal = turnOn ? 100 : 0;
                        for (uint8_t b = 0; b < DR2H_BEATS; b++) {
                            if (!(dr2BeatSelMask & (1 << b))) continue;
                            for (uint8_t s = 0; s < DR2H_STEPS; s++) {
                                if (!(dr2StepSelMask & (1 << s))) continue;
                                // T303 is one monophonic voice — placing a note on a step
                                // must replace whatever note was already there, not just
                                // add alongside it (see dr2T303ClearOtherNotes()).
                                if (turnOn) dr2T303ClearOtherNotes(b, s, idx, dr2SelNote);
                                dr2T303Vel[dr2SelNote][b][s][idx] = newVal;
                            }
                        }
                        if (turnOn) { dr2TriggerT303(dr2SelNote, 0.7f); dr2NoteFlashMs[dr2SelNote] = millis(); }
                    }
                    // DR2_INSTR_TBD: no-op, nothing to place yet.
                } else { // row == 3 (left half): pattern bank select (4 slots) — moved here
                         // from the instrument side to make room for instrument select.
                    if (idx != dr2ActivePat) {
                        // Save the live buffers (all instruments) into the slot being left...
                        memcpy(dr2Pats[dr2ActivePat], dr2Vel, sizeof(dr2Vel));
                        memcpy(dr2ModPats[dr2ActivePat], dr2Mod, sizeof(dr2Mod));
                        memcpy(dr2SynthPats[dr2ActivePat], dr2SynthVel, sizeof(dr2SynthVel));
                        memcpy(dr2T303Pats[dr2ActivePat], dr2T303Vel, sizeof(dr2T303Vel));
                        dr2RecomputeActivePatFilled();
                        // ...and load the newly-selected slot into it.
                        dr2ActivePat = idx;
                        memcpy(dr2Vel, dr2Pats[idx], sizeof(dr2Vel));
                        memcpy(dr2Mod, dr2ModPats[idx], sizeof(dr2Mod));
                        memcpy(dr2SynthVel, dr2SynthPats[idx], sizeof(dr2SynthVel));
                        memcpy(dr2T303Vel, dr2T303Pats[idx], sizeof(dr2T303Vel));
                    }
                }
            } else {
                if (row == 3) {
                    // Single click: focus only, does not solo/mute other slots. Double-
                    // click (same 350ms pattern as B3/B4): clear that instrument's hits
                    // on the CURRENT pattern only — other instruments and other pattern
                    // slots are untouched.
                    static uint32_t _dr2InstrLast[DR2_INSTR_COUNT] = {};
                    uint32_t _now = millis();
                    bool isDbl = (_now - _dr2InstrLast[idx]) < 350;
                    _dr2InstrLast[idx] = isDbl ? 0 : _now;
                    dr2Instrument = idx;
                    if (isDbl) {
                        if (idx == DR2_INSTR_DRUMS) { memset(dr2Vel, 0, sizeof(dr2Vel)); memset(dr2Mod, 0, sizeof(dr2Mod)); }
                        else if (idx == DR2_INSTR_SYNTH) memset(dr2SynthVel, 0, sizeof(dr2SynthVel));
                        else if (idx == DR2_INSTR_T303) memset(dr2T303Vel, 0, sizeof(dr2T303Vel));
                        dr2RecomputeActivePatFilled();
                    }
                } else if (dr2Instrument == DR2_INSTR_DRUMS) {
                    if (row == 0 || row == 1) {
                        uint8_t padIdx = (uint8_t)(row==0 ? idx : idx+4);
                        if (padIdx >= DRUM2_PADS) break;
                        dr2SelPad = (int8_t)padIdx;
                        if (padIdx == DRUM2_CH || padIdx == DRUM2_OH) {
                            uint8_t other = (padIdx == DRUM2_CH) ? DRUM2_OH : DRUM2_CH;
                            amy_event stop = amy_default_event();
                            stop.osc = DRUM_OSC_BASE + kDrum2Remap[other];
                            stop.velocity = 0.0f;
                            amy_add_event(&stop);
                        }
                        playDrum(padIdx, 0.70f * drum2Volume[padIdx], drum2Pitch[padIdx], drum2Decay[padIdx]);
                        dr2PadFlashMs[padIdx] = millis();
                        if (dr2RecArmed && dr2Playing) {
                            uint8_t rb,rs,rm; dr2RecordNearestSlot(&rb,&rs,&rm);
                            dr2Vel[padIdx][rb][rs][rm] = 100;
                            dr2Mod[padIdx][rb][rs][rm] = 0;  // auto-record: no alteration
                            dr2RecomputeActivePatFilled();
                        }
                    } else { // row == 2: alteration mode select
                        dr2PlaceMod = idx;  // 0=NRM 1=50% 2=random-pitch 3=double
                    }
                } else if (dr2Instrument == DR2_INSTR_SYNTH || dr2Instrument == DR2_INSTR_T303) {
                    // rows 0-2 together = one chromatic octave (12 keys); select + preview.
                    uint8_t noteIdx = (uint8_t)(row * 4 + idx);
                    if (noteIdx >= DR2_NOTES) break;
                    dr2SelNote = noteIdx;
                    if (dr2Instrument == DR2_INSTR_SYNTH) dr2TriggerSynth(noteIdx, 0.75f);
                    else                                  dr2TriggerT303(noteIdx, 0.7f);
                    dr2NoteFlashMs[noteIdx] = millis();
                    if (dr2RecArmed && dr2Playing) {
                        uint8_t rb,rs,rm; dr2RecordNearestSlot(&rb,&rs,&rm);
                        if (dr2Instrument == DR2_INSTR_SYNTH) dr2SynthVel[noteIdx][rb][rs][rm] = 100;
                        else { dr2T303ClearOtherNotes(rb, rs, rm, noteIdx); dr2T303Vel[noteIdx][rb][rs][rm] = 100; }
                        dr2RecomputeActivePatFilled();
                    }
                }
                // DR2_INSTR_TBD: rows 0-2 no-op.
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
                        audioT303NoteOn(notePlay, 0.5f);
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
                    float vel = t303AccentOn ? 0.85f : 0.5f;
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
        case MODE_303S2: {
            // Top-left key (row3, col0) = REST: insert silence (highest-note position)
            if (row==3 && col==0) {
                if (pressed) {
                    s303s2Seq[s303s2Head]=0;
                    s303s2Head=(s303s2Head+1)%S303S2_LEN;
                    if(s303s2Count<S303S2_LEN) s303s2Count++;
                }
                break;
            }
            uint8_t note2 = noteMap.getMidiNote(row,col);
            uint8_t notePlay2 = (uint8_t)constrain((int)note2+(int)t303Oct*12,0,127);
            if (pressed) {
                activeNotes[row][col]=note2;
                // Push note to ring buffer
                s303s2Seq[s303s2Head]=note2+1;
                s303s2Head=(s303s2Head+1)%S303S2_LEN;
                if(s303s2Count<S303S2_LEN) s303s2Count++;
                // Play live only when paused
                if (!drum2Playing) {
                    if (s303s2CurNote) audioT303NoteOff(s303s2CurNote);
                    float vel2=t303AccentOn?0.85f:0.5f;
                    audioT303NoteOn(notePlay2,vel2);
                    s303s2CurNote=notePlay2;
                }
            } else {
                activeNotes[row][col]=0;
                if (!drum2Playing && s303s2CurNote==notePlay2) {
                    audioT303NoteOff(notePlay2); s303s2CurNote=0;
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
            // PAD view: standard noteMap keyboard + auto-record when playing
            uint8_t note = noteMap.getMidiNote(row, col);
            activeNotes[row][col] = pressed ? note : 0;
            if (arpMode != 0) {
                // Arp on: held keys feed the arp note list; the ARPEGGIATOR tick plays
                // the stepped note and auto-records it (not the raw held keys).
                if (pressed) {
                    bool found=false;
                    for(uint8_t i=0;i<arpNoteCount;i++) if(arpNotes[i]==note){found=true;break;}
                    if(!found&&arpNoteCount<32) arpNotes[arpNoteCount++]=note;
                } else {
                    for(uint8_t i=0;i<arpNoteCount;i++) if(arpNotes[i]==note){
                        memmove(&arpNotes[i],&arpNotes[i+1],(arpNoteCount-i-1));
                        arpNoteCount--; break;
                    }
                    if(arpNoteCount==0&&arpCurrent!=0){audioNoteOff(arpCurrent);arpCurrent=0;}
                }
            } else if (pressed) {
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
        case MODE_MODULAR: {
            // Wavetable dual-osc synth (Section 5 rebuild) — plays on its own dedicated
            // MOD3_OSCA_CH/MOD3_OSCB_CH channels, not the generic SYNTH_CH audioNoteOn().
            uint8_t note = noteMap.getMidiNote(row, col);
            activeNotes[row][col] = pressed ? note : 0;
            if (pressed) {
                // Fixed velocity — the joystick has no performance role in this mode
                // (JX/JY modify the focused element's parameters instead, see the
                // MODE_MODULAR joystick-nav block in loop()).
                audioModularNoteOn(note, 0.85f, MOD_OSCB_DETUNE_SEMIS);
            } else {
                audioModularNoteOff(note);
            }
            break;
        }
        case MODE_GRANULAR2: {
            uint8_t sampleIdx, sliceIdx; bool reverse;
            gran2KeyInfo(row, col, sampleIdx, sliceIdx, reverse);
            if (sampleIdx >= gran2NumSamples()) break;
            if (!gran2[sampleIdx].computed) break;
            uint8_t keyOsc = (uint8_t)(row * 8 + col);
            if (gran2PlayMode == 3) {
                // SEQ: on press enqueue; no action on release (playback is timer-driven)
                if (pressed) {
                    g_gran2SeqVolume = volume;
                    uint8_t next = (g_gran2SeqTail + 1) % GRAN2_SEQ_MAX;
                    if (next != g_gran2SeqHead) {
                        bool wasEmpty = (g_gran2SeqHead == g_gran2SeqTail);
                        g_gran2SeqQueue[g_gran2SeqTail] = {sampleIdx, sliceIdx, reverse};
                        g_gran2SeqTail = next;
                        if (wasEmpty) {
                            g_gran2SeqNextAmy = amy_sysclock();  // first slice: play immediately
                            gran2SeqStartHead();
                        }
                    }
                }
            } else if (gran2PlayMode == 4) {
                // SQL: add to circular loop, overwriting oldest when full; loops continuously
                if (pressed) {
                    g_gran2SeqVolume = volume;
                    bool wasEmpty = (g_gran2SqlCount == 0);
                    uint8_t writePos = g_gran2SqlWriteIdx % GRAN2_SQL_MAX;
                    g_gran2SqlLoop[writePos] = {sampleIdx, sliceIdx, reverse};
                    g_gran2SqlWriteIdx++;
                    if (g_gran2SqlCount < GRAN2_SQL_MAX) g_gran2SqlCount++;
                    if (wasEmpty) {
                        g_gran2SqlPlayHead = 0;
                        g_gran2SqlNextAmy = amy_sysclock();
                        gran2SqlStartHead();
                    }
                }
            } else if (pressed) {
                if (gran2PlayMode == 2) {
                    // FUL: build [tail_from_slice, full_sample] buffer → "N 0 1 2 N 0 1 2…"
                    float sf = reverse
                        ? (1.0f - gran2[sampleIdx].splits[sliceIdx + 1])
                        :          gran2[sampleIdx].splits[sliceIdx];
                    audioPlayGranular2Ful(keyOsc, sampleIdx, reverse, volume, sf);
                } else {
                    audioPlayGranular2(keyOsc, sampleIdx, sliceIdx, reverse, volume, gran2PlayMode);
                }
                // Reverse buffer builds lazily on first use (see ensureGranular2Reverse in
                // audio_engine.cpp), using the default uniform slicing — re-apply the current
                // split layout right after so a freshly-built reverse copy's slice presets match
                // any custom splits the user made before ever pressing reverse.
                if (reverse) audioApplyGranular2Splits(sampleIdx, gran2[sampleIdx].splits, gran2[sampleIdx].sliceCount, gran2PlayMode == 1);
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
        case MODE_ANIM:
            if (pressed) animIdx = (uint8_t)((animIdx + 1) % 5);
            break;
        case MODE_LANIM:
            if (pressed) lanimIdx = (uint8_t)((lanimIdx + 1) % 5);
            break;
        case MODE_EXP:
            if (pressed) {
                if (col == 4) {
                    // FX column: multi-toggle (each row independent)
                    expFxMask ^= (uint8_t)(1 << row);
                    if (audioReady) expApplyFx();
                } else if (col == 7) {
                    // col7's LED row is a live arp-speed meter (driven by expPosX), not a
                    // row-selector — deliberately not writing expColSel[7] here so it stays
                    // a pure readout instead of a dead/confusing selection state.
                } else {
                    expColSel[col] = row;
                    if (!audioReady) break;
                    if (col == 0) {  // Wave
                        static const SynthShape kExpShapes[] = {SHAPE_SAW, SHAPE_SQUARE, SHAPE_SINE, SHAPE_NOISE_WHITE};
                        audioSetShape(kExpShapes[row]);
                    } else if (col == 1) {  // Envelope
                        static const EnvPreset kExpEnvs[] = {ENV_FAST, ENV_NORMAL, ENV_PAD, ENV_PLUCK};
                        audioSetEnvelope(envTable[(uint8_t)kExpEnvs[row]]);
                    } else if (col == 2) {  // Arp: toggling off → stop note so next tick re-triggers cleanly
                        if (row == 0 && expNoteOn) { audioPostNote(expCurNote, 0.f); expNoteOn = false; }
                    }
                    // col3=gate length (kExpGateMs[], used in the continuous-mode rate limit),
                    // col5=scale, col6=oct — all take effect in the 10ms physics tick.
                }
            }
            break;
        case MODE_EXP2:
            if (pressed) {
                if (col == 0) {  // Scale: MAJ/MIN/PNT/CHR
                    exp2Scale = row; exp2ColSel[0] = row;
                } else if (col == 1) {  // Octave: -2/-1/0/+1
                    exp2Octave = (int8_t)(row - 2); exp2ColSel[1] = row;
                } else if (col == 2) {  // Ball count 1-4
                    uint8_t nc = row + 1;
                    if (nc > exp2BallCount) {
                        for (uint8_t b = exp2BallCount; b < nc; b++) exp2ResetBall(b);
                    } else {
                        for (uint8_t b = nc; b < exp2BallCount; b++) exp2Balls[b].active = false;
                    }
                    exp2BallCount = nc; exp2ColSel[2] = row;
                } else if (col == 3) {  // Waveform: SAW/SQR/SIN/NOI
                    static const SynthShape kExp2Shapes[] = {SHAPE_SAW, SHAPE_SQUARE, SHAPE_SINE, SHAPE_NOISE_WHITE};
                    exp2Shape = row; exp2ColSel[3] = row;
                    if (audioReady) audioSetShape(kExp2Shapes[row]);
                } else if (col == 4) {  // Envelope: PLUCK/FAST/NRM/PAD
                    static const EnvPreset kExp2Envs[] = {ENV_PLUCK, ENV_FAST, ENV_NORMAL, ENV_PAD};
                    exp2EnvIdx = row; exp2ColSel[4] = row;
                    if (audioReady) audioSetEnvelope(envTable[(uint8_t)kExp2Envs[row]]);
                } else if (col == 5) {  // Bounciness: LOW/MED/HI/MAX
                    static const float kBnc[] = {0.50f, 0.72f, 0.88f, 1.00f};
                    exp2Bounce = kBnc[row]; exp2ColSel[5] = row;
                } else if (col == 6) {  // Polygon sides: 3/4/6/8
                    static const uint8_t kSides[] = {3, 4, 6, 8};
                    exp2NumSides = kSides[row]; exp2ColSel[6] = row;
                } else if (col == 7) {  // FX multi-toggle: REV/DLY/CHR/WFD
                    exp2FxMask ^= (uint8_t)(1 << row);
                    if (audioReady) exp2ApplyFx();
                }
            }
            break;
        case MODE_EXP3:
            if (pressed) {
                // col = ball index (0-7), row = orbit (0=inner..3=outer)
                uint8_t b = col;  // ball index = keyboard column
                if (exp3Balls[b].active && exp3Balls[b].orbitRow == (uint8_t)row) {
                    // Same orbit pressed → deactivate ball
                    if (exp3BallNote[b]!=0xFF && audioReady) audioNoteOff(exp3BallNote[b]);
                    exp3Balls[b].active = false;
                    exp3BallNote[b] = 0xFF; exp3NoteOffMs[b] = 0;
                } else {
                    // Activate/move to new orbit (keep current angle)
                    exp3Balls[b].orbitRow  = (uint8_t)row;
                    exp3Balls[b].active    = true;
                    exp3Balls[b].triggered = false;
                }
            }
            break;
        case MODE_LIFE:
            // Manual seeding: toggle a cell alive/dead. Doesn't directly trigger a note —
            // the next simulation tick's birth/death edge detection handles that, keeping
            // note-triggering logic in one place (lifeStep()) regardless of whether a cell
            // became alive by hand or by the automaton's own rules.
            if (pressed) lifeGrid[row][col] = !lifeGrid[row][col];
            break;
        case MODE_SWARM:
            // Toggle a boid active at a grid-column spawn point (row picks which of the
            // 4 orbit-like starting angles within that column's spawn arc — kept simple:
            // row is ignored for spawn geometry, swarmResetBoid() already spaces boids
            // evenly; only the on/off toggle matters here).
            if (pressed) {
                uint8_t b = col;
                if (swarmBoids[b].active) {
                    if (swarmBoidNote[b]!=0xFF && audioReady) audioNoteOff(swarmBoidNote[b]);
                    swarmBoids[b].active = false;
                    swarmBoidNote[b] = 0xFF; swarmNoteOffMs[b] = 0;
                } else {
                    swarmResetBoid(b);
                }
            }
            break;
        case MODE_GEN:
            // Manual play, independent of whatever the generator is doing on its own tick —
            // lets any texture/voice combo be auditioned by hand, not just watched run.
            {
                uint8_t note = genComputeNote(col, (uint8_t)(genOctave + row));
                activeNotes[row][col] = pressed ? note : 0;
                if (audioReady) { if (pressed) audioNoteOn(note, 0.8f*volume); else audioNoteOff(note); }
            }
            break;
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
                if (pressed) midiNoteOn(midiNote, vel, midiChannel);
                else         midiNoteOff(midiNote, midiChannel);
            }
#endif
            break;
        }
        case MODE_POKEMON: {
            uint8_t note;
            if (kbdLayout == KBD_LAYOUT_PIANO) {
                note = pianoNote(row, col, (uint8_t)(48 + noteMap.getOctave()*12));
                if (note == 0xFF) return;
            } else {
                note = noteMap.getMidiNote(row, col);
            }
            activeNotes[row][col] = pressed ? note : 0;
            if (pressed) {
                float jx = constrain(cachedJoyX / 64.0f, -1.0f, 1.0f);
                int held = 0;
                for (int rr=0;rr<KBD_NOTE_ROWS;rr++) for (int cc=0;cc<KBD_COLS;cc++) if(activeNotes[rr][cc]) held++;
                float polyScale = 1.0f / sqrtf(fmaxf(1.0f,(float)held));
                float vnote = constrain((0.8f + jx*0.6f) * polyScale, 0.05f, 1.0f);
                playNoteOn(note, vnote);
            } else {
                playNoteOff(note);
            }
            break;
        }
        case MODE_GEST: {
            if (!pressed || row >= KBD_NOTE_ROWS) break;
            uint8_t seqIdx = (uint8_t)(KBD_NOTE_ROWS - 1 - row);  // row 3=DRUMS, row 2=303S, row 1=SYNS, row 0=SAMPS
            uint8_t pat    = (uint8_t)(KBD_COLS - 1 - col);        // col 7=pat0 … col 0=pat7
            if (seqIdx >= GEST_NSEQ || pat >= GEST_PATS) break;
            gestSelRow = seqIdx;
            gestSelCol = pat;
            // Auto-save current live state into active pattern — only mark filled if content is non-trivial
            uint8_t prevPat = gestActPat[seqIdx];
            if (seqIdx==0){
                memcpy(g_drumPats[prevPat].vel,drum2SeqVel,sizeof(drum2SeqVel)); memcpy(g_drumPats[prevPat].row,drum2SeqRow,sizeof(drum2SeqRow)); memcpy(g_drumPats[prevPat].mod,drum2SeqMod,sizeof(drum2SeqMod)); memcpy(g_drumPats[prevPat].pit,drum2SeqPitchOff,sizeof(drum2SeqPitchOff));
                bool _h=false; for(int _p=0;_p<DRUM2_PADS&&!_h;_p++) for(int _s=0;_s<DR2_STEPS&&!_h;_s++) _h=(drum2SeqVel[_p][_s]>0);
                g_patFilled[0][prevPat]=_h;
            } else if(seqIdx==1){
                memcpy(g_303sPats[prevPat].seq,s303s2Seq,sizeof(s303s2Seq)); g_303sPats[prevPat].count=s303s2Count; g_303sPats[prevPat].head=s303s2Head;
                g_patFilled[1][prevPat]=(s303s2Count>0);
            } else if(seqIdx==2){
                memcpy(g_synsPats[prevPat].notes,syseqNotes,sizeof(syseqNotes)); memcpy(g_synsPats[prevPat].vels,syseqVels,sizeof(syseqVels)); memcpy(g_synsPats[prevPat].alt,syseqAlt,sizeof(syseqAlt));
                bool _h=false; for(int _s=0;_s<SYSEQ_STEPS&&!_h;_s++) for(int _i=0;_i<SYSEQ_CHORD&&!_h;_i++) _h=(syseqNotes[_s][_i]>0);
                g_patFilled[2][prevPat]=_h;
            } else {
                memcpy(g_sampsPats[prevPat].note,ss2Note,sizeof(ss2Note)); memcpy(g_sampsPats[prevPat].alt,ss2Alt,sizeof(ss2Alt));
                bool _h=false; for(int _s=0;_s<SS2_SLOTS&&!_h;_s++) _h=(ss2Note[_s]!=0);
                g_patFilled[3][prevPat]=_h;
            }
            // Switch to selected slot — filled: load; empty: mute (clear live state)
            if (pat != prevPat) {
                gestActPat[seqIdx] = pat;
                if (g_patFilled[seqIdx][pat]) {
                    if(seqIdx==0){ memcpy(drum2SeqVel,g_drumPats[pat].vel,sizeof(drum2SeqVel)); memcpy(drum2SeqRow,g_drumPats[pat].row,sizeof(drum2SeqRow)); memcpy(drum2SeqMod,g_drumPats[pat].mod,sizeof(drum2SeqMod)); memcpy(drum2SeqPitchOff,g_drumPats[pat].pit,sizeof(drum2SeqPitchOff)); }
                    else if(seqIdx==1){ memcpy(s303s2Seq,g_303sPats[pat].seq,sizeof(s303s2Seq)); s303s2Count=g_303sPats[pat].count; s303s2Head=g_303sPats[pat].head;
                        // Sync to current bar position so the new pattern starts in-place (not from 0)
                        { uint8_t qL=(s303s2Count<=1)?1:(s303s2Count<=2)?2:(s303s2Count<=4)?4:(s303s2Count<=8)?8:16; s303s2Step=(uint8_t)(drum2Step%qL); }
                        // Don't kill current note — let the sequencer handle it on the next tick
                        s303s2CurNote=0; }
                    else if(seqIdx==2){
                        // Stop old sequencer notes, immediately fire new pattern's notes at drum2Step
                        for(uint8_t i=0;i<syseqActiveCnt;i++) audioNoteOff(syseqActive[i]);
                        syseqActiveCnt=0; syseqActiveIsFull=false;
                        memcpy(syseqNotes,g_synsPats[pat].notes,sizeof(syseqNotes)); memcpy(syseqVels,g_synsPats[pat].vels,sizeof(syseqVels)); memcpy(syseqAlt,g_synsPats[pat].alt,sizeof(syseqAlt));
                        // Immediately play the new pattern's note(s) at the current sequencer position
                        if (drum2Playing) {
                            for(uint8_t i=0;i<SYSEQ_CHORD;i++) {
                                if(!syseqNotes[drum2Step][i]) continue;
                                uint8_t n=syseqNotes[drum2Step][i]-1;
                                float sv=syseqVels[drum2Step][i]/127.0f*gestSeqVol[2];
                                audioNoteOn(n, sv);
                                if(syseqActiveCnt<SYSEQ_CHORD) syseqActive[syseqActiveCnt++]=n;
                            }
                            syseqActiveIsFull=(syseqAlt[drum2Step]==1);
                        } }
                    else{ memcpy(ss2Note,g_sampsPats[pat].note,sizeof(ss2Note)); memcpy(ss2Alt,g_sampsPats[pat].alt,sizeof(ss2Alt)); }
                } else {
                    // Empty slot: silence this sequencer; content recorded after this becomes pat's data
                    if(seqIdx==0){ memset(drum2SeqVel,0,sizeof(drum2SeqVel)); memset(drum2SeqRow,0,sizeof(drum2SeqRow)); memset(drum2SeqMod,0,sizeof(drum2SeqMod)); memset(drum2SeqPitchOff,0,sizeof(drum2SeqPitchOff)); }
                    else if(seqIdx==1){ memset(s303s2Seq,0,sizeof(s303s2Seq)); s303s2Count=0; s303s2Head=0; s303s2Step=0; if(s303s2CurNote){audioT303NoteOff(s303s2CurNote);s303s2CurNote=0;} }
                    else if(seqIdx==2){ for(uint8_t i=0;i<syseqActiveCnt;i++) audioNoteOff(syseqActive[i]); syseqActiveCnt=0; syseqActiveIsFull=false; memset(syseqNotes,0,sizeof(syseqNotes)); memset(syseqVels,0,sizeof(syseqVels)); memset(syseqAlt,0,sizeof(syseqAlt)); }
                    else{ memset(ss2Note,0,sizeof(ss2Note)); memset(ss2Alt,0,sizeof(ss2Alt)); }
                }
            }
            // Double-tap: drill into the sequencer for this row/pattern.
            // switchMode must run on the main thread — post via gestDrillTarget instead.
            { static uint32_t _lastMs=0; static uint8_t _lastRow=0xFF,_lastCol=0xFF;
              uint32_t _now=millis();
              if (seqIdx==_lastRow && pat==_lastCol && _now-_lastMs < 350) {
                  static const AppMode kGestSeqMode[]={MODE_DRUM2,MODE_303S2,MODE_SYSEQ,MODE_SS2};
                  gestDrillDown=true;
                  gestDrillTarget=kGestSeqMode[seqIdx];  // main loop picks this up
                  _lastMs=0;
              } else { _lastMs=_now; _lastRow=seqIdx; _lastCol=pat; } }
            break;
        }
        default: break;
    }
}

static void audioHandlerTask(void*)
{
    for (;;) {
        // Drain keyboard events (non-blocking)
        KeyEvent evt;
        while (s_audioEventQueue && xQueueReceive(s_audioEventQueue, &evt, 0))
            handleNoteKeyAudio(evt.row, evt.col, evt.pressed);
        // Drain physics-triggered direct note events (non-blocking)
        NoteEvent nevt;
        while (s_noteDirectQueue && xQueueReceive(s_noteDirectQueue, &nevt, 0)) {
            if (nevt.vel > 0.f) audioNoteOn(nevt.note, nevt.vel);
            else                audioNoteOff(nevt.note);
        }
        // Block until notified by kbdEventBridge, or 15ms timeout to poll physics notes.
        // Physics notes (audioPostNote) don't send a notification to avoid cross-core
        // spinlock contention — they're picked up here within 15ms max.
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(15));
    }
}

// ==================== PRESET PATTERNS ====================
// Pre-fills GEST patterns 4-7 with factory sequences.
// vel[pad][step]: 0=rest, 1-127=velocity. Row/mod/pit default to 0.
static void initGestPresets() {

    // ── DRUMS ─────────────────────────────────────────────────────────────────
    // Pad mapping: 0=KK1, 1=SN1, 2=HHC, 3=CLP, 4=BNG, 5=OHH, 6=CHH, 7=RDE

    // Pattern 4 — Basic rock/pop (4-on-floor kick, snare 2&4, 8th-note HH)
    { GestDrumPat& p = g_drumPats[4]; memset(&p,0,sizeof(p));
      // Kick on 1 and 3 (steps 0, 8)
      p.vel[0][0]=110; p.vel[0][8]=110;
      // Snare on 2 and 4 (steps 4, 12)
      p.vel[1][4]=100; p.vel[1][12]=100;
      // Closed HH 8th notes (steps 0,2,4,6,8,10,12,14)
      for(int s=0;s<16;s+=2) p.vel[2][s]=70;
      g_patFilled[0][4]=true; }

    // Pattern 5 — House (4-on-floor kick, open HH on offbeats, clap 2&4)
    { GestDrumPat& p = g_drumPats[5]; memset(&p,0,sizeof(p));
      // Kick every quarter (steps 0,4,8,12)
      p.vel[0][0]=115; p.vel[0][4]=110; p.vel[0][8]=115; p.vel[0][12]=110;
      // Clap 2&4
      p.vel[3][4]=105; p.vel[3][12]=105;
      // Open HH on upbeats (steps 2,6,10,14)
      p.vel[5][2]=75; p.vel[5][6]=75; p.vel[5][10]=75; p.vel[5][14]=75;
      // Ride 16ths for texture
      for(int s=0;s<16;s++) p.vel[7][s]=40;
      g_patFilled[0][5]=true; }

    // Pattern 6 — Breakbeat
    { GestDrumPat& p = g_drumPats[6]; memset(&p,0,sizeof(p));
      // Syncopated kick
      p.vel[0][0]=115; p.vel[0][3]=90; p.vel[0][10]=95;
      // Snare + ghost notes
      p.vel[1][4]=105; p.vel[1][9]=70; p.vel[1][14]=100;
      // Closed HH 16ths
      for(int s=0;s<16;s++) p.vel[2][s]=(s%4==0)?80:55;
      // Clap accent on 12
      p.vel[3][12]=100;
      // Bongo fill
      p.vel[4][2]=65; p.vel[4][6]=65;
      g_patFilled[0][6]=true; }

    // Pattern 7 — Funk/Afro
    { GestDrumPat& p = g_drumPats[7]; memset(&p,0,sizeof(p));
      // Kick: 0, 6, 12
      p.vel[0][0]=115; p.vel[0][6]=95; p.vel[0][12]=110;
      // Snare: 4, 10 (off-beat feel)
      p.vel[1][4]=100; p.vel[1][10]=90;
      // Closed HH 8ths
      for(int s=0;s<16;s+=2) p.vel[2][s]=65;
      // Bongo syncopation
      p.vel[4][2]=80; p.vel[4][5]=75; p.vel[4][9]=80; p.vel[4][13]=70;
      // Open HH on 8
      p.vel[5][8]=85;
      g_patFilled[0][7]=true; }

    // ── 303S ──────────────────────────────────────────────────────────────────
    // seq[i] = MIDI+1 (0=rest), count=number of notes, head=count (linear fill).
    // Loop length is quantized to nearest power-of-2 ≥ count (1/2/4/8/16).

    // Pattern 4 — 4-note A minor acid (loops every 4 steps)
    { Gest303Pat& p = g_303sPats[4]; memset(&p,0,sizeof(p));
      // A2=45 E2=40 G2=43 D2=38
      uint8_t ns[]={45,40,45,43}; p.count=4; p.head=4;
      for(int i=0;i<4;i++) p.seq[i]=(uint8_t)(ns[i]+1);
      g_patFilled[1][4]=true; }

    // Pattern 5 — 8-note walking A minor (loops every 8 steps)
    { Gest303Pat& p = g_303sPats[5]; memset(&p,0,sizeof(p));
      // A1 C#2 E2 G2 A2 G2 E2 C#2
      uint8_t ns[]={33,37,40,43,45,43,40,37}; p.count=8; p.head=8;
      for(int i=0;i<8;i++) p.seq[i]=(uint8_t)(ns[i]+1);
      g_patFilled[1][5]=true; }

    // Pattern 6 — 4-note acid riff with rests (loops every 4 steps)
    { Gest303Pat& p = g_303sPats[6]; memset(&p,0,sizeof(p));
      // A2 _ A2(+oct) E2
      uint8_t ns[]={45,57,45,40}; p.count=4; p.head=4;
      for(int i=0;i<4;i++) p.seq[i]=(uint8_t)(ns[i]+1);
      g_patFilled[1][6]=true; }

    // Pattern 7 — 8-note Am pentatonic descent
    { Gest303Pat& p = g_303sPats[7]; memset(&p,0,sizeof(p));
      // A3 G3 E3 D3 C3 D3 E3 G3
      uint8_t ns[]={57,55,52,50,48,50,52,55}; p.count=8; p.head=8;
      for(int i=0;i<8;i++) p.seq[i]=(uint8_t)(ns[i]+1);
      g_patFilled[1][7]=true; }

    // ── SYNS ──────────────────────────────────────────────────────────────────
    // notes[step][chord_voice] = MIDI+1 (0=empty), vels = velocity (0-127).
    // alt[step]: 0=NRM, 1=FUL (sustain through empty steps).

    // Pattern 4 — Am chord progression (chord on beats 1/2/3/4)
    { GestSynsPat& p = g_synsPats[4]; memset(&p,0,sizeof(p));
      // Am: A3(57) C4(60) E4(64) — step 0
      p.notes[0][0]=58; p.notes[0][1]=61; p.notes[0][2]=65; p.vels[0][0]=p.vels[0][1]=p.vels[0][2]=90;
      // F: F3(53) A3(57) C4(60) — step 4
      p.notes[4][0]=54; p.notes[4][1]=58; p.notes[4][2]=61; p.vels[4][0]=p.vels[4][1]=p.vels[4][2]=85;
      // G: G3(55) B3(59) D4(62) — step 8
      p.notes[8][0]=56; p.notes[8][1]=60; p.notes[8][2]=63; p.vels[8][0]=p.vels[8][1]=p.vels[8][2]=88;
      // E: E3(52) G#3(56) B3(59) — step 12
      p.notes[12][0]=53; p.notes[12][1]=57; p.notes[12][2]=60; p.vels[12][0]=p.vels[12][1]=p.vels[12][2]=90;
      g_patFilled[2][4]=true; }

    // Pattern 5 — Pad sustain (FUL): 2 long chords per bar
    { GestSynsPat& p = g_synsPats[5]; memset(&p,0,sizeof(p));
      // Am at step 0, held (FUL)
      p.notes[0][0]=58; p.notes[0][1]=61; p.notes[0][2]=65; p.vels[0][0]=p.vels[0][1]=p.vels[0][2]=80; p.alt[0]=1;
      // F at step 8, held (FUL)
      p.notes[8][0]=54; p.notes[8][1]=58; p.notes[8][2]=61; p.vels[8][0]=p.vels[8][1]=p.vels[8][2]=80; p.alt[8]=1;
      g_patFilled[2][5]=true; }

    // Pattern 6 — Stab chords every 4 steps (Am / G / F / E)
    { GestSynsPat& p = g_synsPats[6]; memset(&p,0,sizeof(p));
      uint8_t chords[4][3]={{58,61,65},{56,60,63},{54,58,61},{53,57,60}};
      int steps[4]={0,4,8,12};
      for(int c=0;c<4;c++) for(int v=0;v<3;v++){
          p.notes[steps[c]][v]=chords[c][v]; p.vels[steps[c]][v]=95;}
      g_patFilled[2][6]=true; }

    // Pattern 7 — Lead melody (A minor, single notes on 8th-note grid)
    { GestSynsPat& p = g_synsPats[7]; memset(&p,0,sizeof(p));
      // A4 G4 E4 D4 C4 D4 E4 A4 on steps 0,2,4,6,8,10,12,14
      uint8_t mel[]={70,68,65,63,61,63,65,70};
      int mst[]={0,2,4,6,8,10,12,14};
      for(int i=0;i<8;i++){ p.notes[mst[i]][0]=(uint8_t)(mel[i]); p.vels[mst[i]][0]=85; }
      g_patFilled[2][7]=true; }
}

#ifndef SIMULATOR
// Arduino-ESP32's cores/esp32/main.cpp sizes loopTask's stack from this weak
// symbol (default 8192 if unset). stb_image's PNG/zlib decoder (used by
// imgDecodeGray() for the jpg/png->bvid auto-convert, include/jpegdec.h) nests
// several KB of local structs (stbi__zbuf holds two ~1KB stbi__zhuffman tables,
// plus another ~1KB one in stbi__compute_huffman_codes, plus a 1KB palette
// buffer elsewhere in PNG chunk parsing) on top of main.cpp's own call depth —
// enough to blow the default 8KB stack (confirmed on real hardware: "Stack
// canary watchpoint triggered (loopTask)" when opening a .png in MODE_VID).
size_t getArduinoLoopTaskStackSize(void) { return 20480; }
#endif

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
    s_noteDirectQueue = xQueueCreate(16, sizeof(NoteEvent));
    setNoteKeyCallback(kbdEventBridge);
    s_ledSem  = xSemaphoreCreateBinary();
    s_oledDone = xSemaphoreCreateBinary();
    xSemaphoreGive(s_oledDone); // pre-give: first drawScreen can start immediately
    xTaskCreatePinnedToCore(audioHandlerTask, "audioHdlr", 8192, nullptr, 22, &s_audioHandlerHandle, 0);
    xTaskCreatePinnedToCore(ledUpdateTask,    "led",       3072, nullptr,  4, nullptr, 0);
    xTaskCreatePinnedToCore(statsTask,        "stats",     4096, nullptr,  1, nullptr, 0);
    xTaskCreatePinnedToCore(displayTask,      "disp",      2048, nullptr,  3, &s_displayTaskHandle, 0);

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
    initGestPresets();

    // Land on the main menu at boot instead of dropping straight into SYNTH —
    // same state handleButton()'s long-press-B1 path sets when opening the menu.
    menuOpen = true; menuRow = 0; menuCol = 0; menuOnTabBar = true;

    // TEMP DBG: J:PNO brightness with FILT off / near-max-but-not-bypassed / genuinely low.
    // Retrigger a FRESH note per phase (J:PNO has its own decay envelope even while held,
    // so measuring across one continuous hold would conflate envelope decay with the
    // filter's effect) — each phase measures the same early post-attack window instead.
    menuOpen = false;
    switchMode(MODE_SYNTH);
    audioSetVolume(0.8f);
    audioSetShape(SHAPE_JUNO_PIANO); // J:PNO
    fxList[0].active = false; applyAllFx();
    audioNoteOff(48); delay(200);
    audioNoteOn(48, 0.8f);
    Serial.println("TEMP DBG --- baseline, FILT off (native ~994Hz) ---"); delay(400);
    audioNoteOff(48); delay(200);

    fxList[0].active = true; fxList[0].params[0]=17000.0f; fxList[0].params[1]=1.5f; fxList[0].params[3]=0.0f;
    applyAllFx();
    audioNoteOn(48, 0.8f);
    Serial.println("TEMP DBG --- FILT on, cutoff=17000 (near max, should be capped to native) ---"); delay(400);
    audioNoteOff(48); delay(200);

    fxList[0].params[0]=500.0f; applyAllFx();
    audioNoteOn(48, 0.8f);
    Serial.println("TEMP DBG --- FILT on, cutoff=500 (genuinely darker than native) ---"); delay(400);
    audioNoteOff(48); delay(200);
    Serial.println("TEMP DBG: J:PNO brightness check done");

    Serial.println("Ready");
}

// ==================== LOOP ====================
void loop() {
    s_mainLoopCount++;
    amy_update();

#ifndef SIMULATOR
    // ---- SD CARD HOT-SWAP DETECTION ----
    // SDFS::begin() (arduino-esp32's SD.cpp) is a no-op once mounted — it returns
    // true immediately without touching the hardware again as long as its internal
    // _pdrv stays set, and nothing else ever notices a card being pulled. So without
    // this, removing/reinserting the card left sdReady stuck true against a card that
    // no longer responds, requiring a full power cycle to re-run setup()'s one-shot
    // SD.begin(). Gated on audioIsStreamingDone() so this never shares the SPI bus
    // with an in-flight background sample load from bgServiceTask.
    {
        static uint32_t lastSdCheck = 0;
        uint32_t nowSd = millis();
        if (nowSd - lastSdCheck > 1000 && audioIsStreamingDone()) {
            lastSdCheck = nowSd;
            if (sdReady) {
                // Cheap real I/O probe — fails immediately once the card is physically gone.
                File f = SD.open("/");
                if (f) {
                    f.close();
                } else {
                    sdReady = false;
                    SD.end();  // reset _pdrv so a later begin() actually re-probes
                    Serial.println("SD: card lost");
                }
            } else if (SD.begin(SD_CS, SPI, 20000000)) {
                sdReady = true;
                Serial.println("SD: card detected");
                sdListDir(sdPath.length() > 1 ? sdPath.c_str() : "/");
            }
        }
    }
#endif

    // Deferred mode switch posted by audioHandlerTask (thread-safe)
    if (gestDrillTarget != MODE_COUNT) {
        AppMode t = gestDrillTarget;
        gestDrillTarget = MODE_COUNT;
        switchMode(t);
    }

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
            // OVERLAY_FX defers its action to release/long-press (see s_fxPressOpt) —
            // every other overlay still acts entirely on press, so release is a no-op there.
            else if(s_overlay == OVERLAY_FX) overlayFxKeyRelease(row,col);
            continue;
        }
        // Note audio handled immediately by handleNoteKeyAudio() via kbdPollTask callback.
    }
#ifdef SIMULATOR
    // No audioHandlerTask in simulator builds (xTaskCreate is a no-op) — drain the audio
    // event queue here so note keys produce sound. On ESP32 this loop is never compiled.
    {
        KeyEvent _evt;
        while (s_audioEventQueue && xQueueReceive(s_audioEventQueue, &_evt, 0))
            handleNoteKeyAudio(_evt.row, _evt.col, _evt.pressed);
    }
#endif
    if(btn1PressTime>0&&!btn1Handled&&(millis()-btn1PressTime>=600)){
        btn1Handled=true;
#if CONFIG_TINYUSB_MIDI_ENABLED
        if (currentMode == MODE_MIDI) {
            midiPotSel = (midiPotSel < 0) ? 0 : -1;  // toggle pot config mode
        } else {
#endif
            menuOpen = !menuOpen;
            if(menuOpen){audioAllNotesOff();omniRoot=0xFF;menuRow=0;menuCol=0;menuOnTabBar=true;}
#if CONFIG_TINYUSB_MIDI_ENABLED
        }
#endif
    }

    // FX grid long-press poll (same idiom as btn1PressTime above): fires once the armed
    // key has been held past FX_LONGPRESS_MS, opening the automation editor instead of
    // waiting for release. If the key is released first, overlayFxKeyRelease() (called
    // from the key-event loop above) performs the normal toggle instead.
    if (s_fxPressOpt != 255 && !s_fxLongPressFired && s_overlay == OVERLAY_FX
        && (millis() - s_fxPressStartMs >= FX_LONGPRESS_MS)) {
        s_fxLongPressFired = true;
        uint8_t firedOpt = s_fxPressOpt;
        s_fxPressOpt = 255; // done with this press regardless of outcome — s_overlay may
                             // change inside overlayFxEnterAutomation(), after which the key
                             // loop's release handler won't see OVERLAY_FX anymore to clear it
        overlayFxEnterAutomation(firedOpt);
    }

    // ---- JOYSTICK CLICK ----
    // Browser modes (GRANU2, SAMPLE, SS2):
    //   Short click → act immediately on press-down (same UX as before).
    //   Long press  → if held >=600ms, open main menu.
    //   Both can coexist: short-click navigates, and if the user KEEPS holding
    //   past 600ms, the menu opens. joyBrwActed is NOT set, so long-press is free.
    bool click=!digitalRead(JOYSW);

    // Arm timer on press-down
    if (click && !lastClick) {
        joyClickMs   = millis();
        joyLongFired = false;
        joyBrwActed  = false;
    }

    // (long-press to menu removed from browser modes: navigation consumes the press
    //  immediately via joyLongFired=true; use B1 long-press to open the main menu)

    // Press-down edge: act immediately for ALL contexts
    if (click && !lastClick && !joyLongFired) {
        if (gestDrillDown && !menuOpen &&
            (currentMode==MODE_DRUM2||currentMode==MODE_303S2||currentMode==MODE_SYSEQ||currentMode==MODE_SS2)) {
            gestDrillDown=false;
            gestDrillReturn=true;
            switchMode(MODE_GEST);
            joyLongFired=true;
        } else if (s_overlay == OVERLAY_INSTR) {
            s_overlay = OVERLAY_NONE; s_overlayCloseAt = 0;
            joyLongFired = true;
        } else if (menuOpen) {
            selectMenuItem();
            joyLongFired = true;
        } else if (currentMode==MODE_GRANULAR2 && sdReady) {
            if(sdCursor<sdFileCount){
                if(sdFiles[sdCursor]==".."){
                    if(sdPath=="/"){audioAllNotesOff();menuOpen=true;menuRow=0;menuCol=0;}
                    else{int ls=sdPath.lastIndexOf('/',sdPath.length()-2);sdListDir(ls<=0?"/":sdPath.substring(0,ls+1));}
                } else if(sdFileIsDir[sdCursor]){
                    String np=sdPath; if(!np.endsWith("/"))np+="/"; np+=sdFiles[sdCursor]; sdListDir(np);
                } else if(isAudioFile(sdFiles[sdCursor].c_str())){
                    uint8_t t = gran2LoadTarget;
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
                    uint8_t numSamp = gran2NumSamples();
                    for (uint8_t ns = 1; ns < numSamp; ns++) {
                        uint8_t next = (t + ns) % numSamp;
                        if (!gran2[next].loaded && gran2[next].path.isEmpty()) { gran2LoadTarget = next; break; }
                    }
                }
                joyLongFired = true; // consume press; long-press to menu via B1 long-press
            }
        } else if (currentMode==MODE_VID && sdReady) {
            if (mediaAudioPlaying) {
                // Click while playing audio → stop, return to browser
                audioStopSamplePreset(PCM_PREVIEW_PRESET); mediaAudioPlaying=false;
                sdListDir(sdPath.c_str(), isMediaFile);
            } else if (vidPlaying) {
                // Click while playing → pause / stop, return to browser
                vidFile.close(); vidFileOpen=false; vidPlaying=false;
                sdListDir(sdPath.c_str(), isMediaFile);
            } else if (sdCursor < sdFileCount) {
                if (sdFiles[sdCursor]=="..") {
                    if (sdPath=="/") { audioAllNotesOff(); menuOpen=true; menuRow=0; menuCol=0; menuOnTabBar=true; }
                    else { int ls=sdPath.lastIndexOf('/',sdPath.length()-2); sdListDir(ls<=0?"/":sdPath.substring(0,ls+1), isMediaFile); }
                } else if (sdFileIsDir[sdCursor]) {
                    String np=sdPath; if(!np.endsWith("/"))np+="/"; np+=sdFiles[sdCursor];
                    sdListDir(np, isMediaFile);
                } else if (isAudioFile(sdFiles[sdCursor].c_str())) {
                    String fp = buildSdFilePath();
                    audioLoadAndPlay(fp.c_str(), PCM_PREVIEW_PRESET, volume);
                    mediaAudioPath = fp;
                    mediaAudioPlaying = true;
                    Serial.printf("MEDIA: playing %s\n", fp.c_str());
                } else if (isVidOrImgFile(sdFiles[sdCursor].c_str())) {
                    String fp = buildSdFilePath();
                    String playPath = fp;
                    if (isImgFile(fp.c_str())) {
                        // Convert (or reuse the cached .bvid) then stream that like any native video.
                        uint8_t tmpFrame[DRANI_FRAME_BYTES];
                        if (loadOrConvertBvid(fp, tmpFrame)) playPath = fp + ".bvid";
                        else { Serial.println("VID: image conversion failed, see IMG: log above"); playPath = ""; }
                    }
                    if (playPath.length()) {
                        if (vidFileOpen) { vidFile.close(); vidFileOpen=false; }
                        vidFile = SD.open(playPath.c_str());
                        if (vidFile) {
                            BvidHeader hdr;
                            if (vidFile.read((uint8_t*)&hdr, sizeof(hdr)) == sizeof(hdr)
                                && memcmp(hdr.magic,"BVID",4)==0
                                && hdr.width==128 && hdr.height==128) {
                                vidFps         = hdr.fps > 0 ? hdr.fps : 10;
                                vidFrameCount  = hdr.frame_count;
                                vidCurrentFrame = 0;
                                vidLastFrameMs = millis();
                                vidFile.read(vidFrameBuf, sizeof(vidFrameBuf));
                                vidFileOpen = true;
                                vidPlaying  = true;
                                Serial.printf("VID: %s %ufps %u frames\n", playPath.c_str(), vidFps, vidFrameCount);
                            } else {
                                vidFile.close();
                                Serial.println("VID: bad header or wrong size");
                            }
                        }
                    }
                }
            }
            joyLongFired = true;
        } else if (currentMode==MODE_DRUM2 && drum2View==2 && sdReady && draniBrowse) {
            if (sdCursor < sdFileCount) {
                if (sdFiles[sdCursor]=="..") {
                    if (sdPath=="/") { audioAllNotesOff(); menuOpen=true; menuRow=0; menuCol=0; menuOnTabBar=true; }
                    else { int ls=sdPath.lastIndexOf('/',sdPath.length()-2); sdListDir(ls<=0?"/":sdPath.substring(0,ls+1), isVidOrImgFile); }
                } else if (sdFileIsDir[sdCursor]) {
                    String np=sdPath; if(!np.endsWith("/"))np+="/"; np+=sdFiles[sdCursor];
                    draniLoadFolder(np);
                    if (draniRunning) {
                        sdPath = np;
                        draniBrowse = false;  // auto-close browser once folder is loaded
                    } else {
                        sdListDir(np, isVidOrImgFile);
                    }
                }
            }
            joyLongFired = true;
        } else if (currentMode==MODE_SAMPLE && sdReady) {
            if(sdCursor<sdFileCount){
                if(sdFiles[sdCursor]==".."){
                    if(sdPath=="/"){audioAllNotesOff();menuOpen=true;menuRow=0;menuCol=0;}
                    else{int ls=sdPath.lastIndexOf('/',sdPath.length()-2);sdListDir(ls<=0?"/":sdPath.substring(0,ls+1));}
                } else if(sdFileIsDir[sdCursor]){
                    String np=sdPath; if(!np.endsWith("/"))np+="/"; np+=sdFiles[sdCursor]; sdListDir(np);
                } else {
                    String fp=buildSdFilePath();
                    Serial.printf("LOAD: %s\n",fp.c_str());
                    audioLoadAndPlay(fp.c_str(), PCM_PREVIEW_PRESET, volume);
                }
                joyLongFired = true;
            }
        } else if (currentMode==MODE_STONE && sdReady) {
            if(sdCursor<sdFileCount){
                if(sdFiles[sdCursor]==".."){
                    if(sdPath=="/"){audioStoneAllNotesOff();audioAllNotesOff();menuOpen=true;menuRow=0;menuCol=0;menuOnTabBar=true;}
                    else{int ls=sdPath.lastIndexOf('/',sdPath.length()-2);sdListDir(ls<=0?"/":sdPath.substring(0,ls+1));stoneRebuildAudioIdx();lp_stoneP2=pots[1].value;}
                } else if(sdFileIsDir[sdCursor]){
                    String np=sdPath; if(!np.endsWith("/"))np+="/"; np+=sdFiles[sdCursor]; sdListDir(np); stoneRebuildAudioIdx(); lp_stoneP2=pots[1].value;
                } else if(isAudioFile(sdFiles[sdCursor].c_str())){
                    String fp=buildSdFilePath();
                    stoneLoadedPath = fp;
                    audioLoadStone(fp.c_str());
                    stoneWin = StoneWinState{};  // new sample: full-range window, waveform recomputed on ready
                    // Pickup: freeze P2 at its actual current position so it doesn't immediately
                    // re-trigger a different file on the next poll (P2 only takes over once the
                    // user actually turns it away from here).
                    lp_stoneP2 = pots[1].value;
                }
                joyLongFired = true;
            }
        } else if (currentMode==MODE_SS2 && sdReady) {
            if (sdCursor < sdFileCount) {
                if (sdFiles[sdCursor]=="..") {
                    if (sdPath=="/") { audioAllNotesOff(); menuOpen=true; menuRow=0; menuCol=0; menuOnTabBar=true; }
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
                joyLongFired = true;
            }
        } else {
            audioAllNotesOff(); omniRoot=0xFF; omniStrumPos=-1;
            menuOpen=true; menuRow=0; menuCol=0; menuOnTabBar=true;
            joyLongFired = true;
        }
    }

    if (!click) { joyClickMs = 0; joyLongFired = false; joyBrwActed = false; }
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
                float strumVel = constrain(vel * omniStrumVol, 0.05f, 1.5f);

                int8_t step = (newPos > omniStrumPos) ? 1 : -1;
                int8_t p = (omniStrumPos < 0) ? newPos : (int8_t)(omniStrumPos + step);
                while(true){
                    uint8_t note = (uint8_t)(base + strip[p]);
                    float freq   = 440.0f * powf(2.0f, (note - 69) / 12.0f);
                    amy_event e  = amy_default_event();
                    e.osc        = (uint16_t)(AMY_OSC_STRUM + p);
                    e.wave       = omniStrumWave;
                    e.freq_coefs[COEF_CONST] = freq;
                    e.velocity   = strumVel;
                    // Plucked envelope: instant attack, 600ms decay
                    e.eg0_times[0] = 2;    e.eg0_values[0] = 1.0f;
                    e.eg0_times[1] = 600;  e.eg0_values[1] = 0.0f;
                    e.eg0_times[2] = 100;  e.eg0_values[2] = 0.0f;
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
            playDrum(pi, drum2Volume[pi]*0.65f, drum2DblPitch[pi], drum2Decay[pi]*0.5f);
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
            Serial.printf("S:%d\n", drum2Step);
            // LIVE mode: at bar start, advance each sequencer to next filled pattern
            if (gestPlayMode == GEST_LIVE && drum2Step == 0 && !gestDrillDown) {
                auto gestSaveDrum = [&](uint8_t p){ memcpy(g_drumPats[p].vel,drum2SeqVel,sizeof(drum2SeqVel)); memcpy(g_drumPats[p].row,drum2SeqRow,sizeof(drum2SeqRow)); memcpy(g_drumPats[p].mod,drum2SeqMod,sizeof(drum2SeqMod)); memcpy(g_drumPats[p].pit,drum2SeqPitchOff,sizeof(drum2SeqPitchOff)); g_patFilled[0][p]=true; };
                auto gestSave303 = [&](uint8_t p){ memcpy(g_303sPats[p].seq,s303s2Seq,sizeof(s303s2Seq)); g_303sPats[p].count=s303s2Count; g_303sPats[p].head=s303s2Head; g_patFilled[1][p]=true; };
                auto gestSaveSyns = [&](uint8_t p){ memcpy(g_synsPats[p].notes,syseqNotes,sizeof(syseqNotes)); memcpy(g_synsPats[p].vels,syseqVels,sizeof(syseqVels)); memcpy(g_synsPats[p].alt,syseqAlt,sizeof(syseqAlt)); g_patFilled[2][p]=true; };
                auto gestSaveSamps = [&](uint8_t p){ memcpy(g_sampsPats[p].note,ss2Note,sizeof(ss2Note)); memcpy(g_sampsPats[p].alt,ss2Alt,sizeof(ss2Alt)); g_patFilled[3][p]=true; };
                for (uint8_t si=0; si<GEST_NSEQ; si++) {
                    uint8_t cur = gestActPat[si];
                    uint8_t nxt = 0xFF;
                    for (uint8_t p=1; p<=GEST_PATS; p++) { uint8_t c=(cur+p)%GEST_PATS; if(g_patFilled[si][c]){nxt=c;break;} }
                    if (nxt==0xFF || nxt==cur) continue;
                    if(si==0) gestSaveDrum(cur);
                    else if(si==1) gestSave303(cur);
                    else if(si==2) gestSaveSyns(cur);
                    else gestSaveSamps(cur);
                    gestActPat[si]=nxt;
                    if(si==0){ memcpy(drum2SeqVel,g_drumPats[nxt].vel,sizeof(drum2SeqVel)); memcpy(drum2SeqRow,g_drumPats[nxt].row,sizeof(drum2SeqRow)); memcpy(drum2SeqMod,g_drumPats[nxt].mod,sizeof(drum2SeqMod)); memcpy(drum2SeqPitchOff,g_drumPats[nxt].pit,sizeof(drum2SeqPitchOff)); }
                    else if(si==1){ memcpy(s303s2Seq,g_303sPats[nxt].seq,sizeof(s303s2Seq)); s303s2Count=g_303sPats[nxt].count; s303s2Head=g_303sPats[nxt].head; s303s2Step=0; }
                    else if(si==2){ for(uint8_t i=0;i<syseqActiveCnt;i++) audioNoteOff(syseqActive[i]); syseqActiveCnt=0; syseqActiveIsFull=false; memcpy(syseqNotes,g_synsPats[nxt].notes,sizeof(syseqNotes)); memcpy(syseqVels,g_synsPats[nxt].vels,sizeof(syseqVels)); memcpy(syseqAlt,g_synsPats[nxt].alt,sizeof(syseqAlt)); }
                    else{ memcpy(ss2Note,g_sampsPats[nxt].note,sizeof(ss2Note)); memcpy(ss2Alt,g_sampsPats[nxt].alt,sizeof(ss2Alt)); }
                }
            }
            for(uint8_t pi=0;pi<DRUM2_PADS;pi++){
                uint8_t vel8 = drum2SeqVel[pi][drum2Step];
                if(vel8 == 0) continue;
                // Row variation stored at record time — apply pitch & decay offsets for playback
                uint8_t rv = drum2SeqRow[pi][drum2Step];
                const Dr2RowVar& var = kDr2RowVar[rv < KBD_NOTE_ROWS ? rv : 2];
                float vel = drum2Volume[pi] * vel8 / 127.0f * 3.0f * (1.0f + jy * 0.25f) * gestSeqVol[0];
                vel = constrain(vel, 0.05f, 3.0f);
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
                        if(random(2)){ playDrum(pi,vel,pitch,dec); drum2PadFlashMs[pi]=millis(); }
                        break;
                    case 2: // Random pitch — audible range 36-96 (C2-C7)
                        { uint8_t rp=(uint8_t)constrain(36+(int)random(61),0,127);
                          playDrum(pi,vel,rp,dec); drum2PadFlashMs[pi]=millis(); }
                        break;
                    case 3: // Double hit: now + exactly half the current step duration
                        playDrum(pi,vel,pitch,dec);
                        drum2DblPendingAt[pi] = millis() + waitMs/2;
                        drum2DblPitch[pi] = pitch;
                        drum2PadFlashMs[pi]=millis();
                        break;
                    default: // Normal
                        playDrum(pi,vel,pitch,dec);
                        drum2PadFlashMs[pi]=millis();
                        Serial.printf("D:%d:%d\n", pi, (int)(vel*100));
                        break;
                }
            }
            // ---- SYSEQ notes at same shared step ----
            {
                // Auto-record: any key still held gets written into the new step
                if (currentMode == MODE_SYSEQ) {
                    for(uint8_t rr=0;rr<KBD_NOTE_ROWS;rr++) for(uint8_t cc=0;cc<KBD_COLS;cc++) {
                        uint8_t hn = activeNotes[rr][cc];
                        if (!hn) continue;
                        bool found=false;
                        for(uint8_t i=0;i<SYSEQ_CHORD;i++) if(syseqNotes[drum2Step][i]==hn+1){found=true;break;}
                        if (!found) for(uint8_t i=0;i<SYSEQ_CHORD;i++) if(!syseqNotes[drum2Step][i]){
                            syseqNotes[drum2Step][i]=hn+1; syseqVels[drum2Step][i]=100; break;
                        }
                    }
                }
                bool newHasNotes = false;
                for(uint8_t i=0;i<SYSEQ_CHORD;i++) if(syseqNotes[drum2Step][i]) { newHasNotes=true; break; }
                // FUL: only stop previous notes if new step has notes (sustain through empty steps)
                if (!syseqActiveIsFull || newHasNotes) {
                    for(uint8_t i=0;i<syseqActiveCnt;i++) { audioNoteOff(syseqActive[i]); midiNoteOff(syseqActive[i], MIDI_CH_SYNTH); }
                    syseqActiveCnt = 0;
                }
                if (newHasNotes) {
                    for(uint8_t i=0;i<SYSEQ_CHORD;i++){
                        if(!syseqNotes[drum2Step][i]) continue;
                        uint8_t note = syseqNotes[drum2Step][i] - 1;
                        float sv = syseqVels[drum2Step][i] / 127.0f * gestSeqVol[2];
                        sv = constrain(sv * (1.0f + jy * 0.25f), 0.05f, 1.2f);
                        playNoteOn(note, sv);
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
                    float vel = (alt == 1) ? 0.85f : 0.5f;  // ACC=louder; reduced vs other modes
                    // Slide: previous step must have SLD alteration and a note
                    uint8_t prevStep = (uint8_t)((drum2Step + S303_STEPS - 1) % S303_STEPS);
                    bool slideIn = (s303Alt[prevStep]==2) && (s303Note[prevStep] > 0);
                    if (slideIn && s303CurNote > 0) {
                        // Slide: start pitch-bend glide (reuse t303SlideActive machinery)
                        t303SlideFrom = s303CurNote;
                        t303SlideTo   = midiNote;
                        t303SlideMs   = millis();
                        t303SlideActive = true;
                        playBassOn(midiNote, vel);  // retrigger at new pitch; AMY blends
                    } else {
                        t303SlideActive = false;
                        audioT303PitchBend(1.0f);
                        if (s303CurNote) playBassOff(s303CurNote);
                        playBassOn(midiNote, vel);
                    }
                    s303CurNote = midiNote;
                } else {
                    // Rest: release current note
                    if (s303CurNote) { playBassOff(s303CurNote); s303CurNote=0; }
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
                        float ss2v = 0.7f * gestSeqVol[3];
                        if (alt == 1) {  // REV
                            audioPlayKeyRev((uint8_t)(SS2_KEY_BASE + slot), ss2v);
                            ss2LastPlayMs[slot] = millis();
                            ss2CurSlot = slot;
                        } else if (alt == 2) {  // FUL: no retrigger while sample running
                            uint32_t dur = audioKeyLengthMs((uint8_t)(SS2_KEY_BASE + slot));
                            uint32_t ela = (uint32_t)(millis() - ss2LastPlayMs[slot]);
                            if (dur == 0 || ela >= dur || ss2LastPlayMs[slot] == 0) {
                                audioPlayKey((uint8_t)(SS2_KEY_BASE + slot), ss2v);
                                ss2LastPlayMs[slot] = millis();
                            }
                            ss2CurSlot = slot;
                        } else {  // NRM
                            audioPlayKey((uint8_t)(SS2_KEY_BASE + slot), ss2v);
                            ss2LastPlayMs[slot] = millis();
                            ss2CurSlot = slot;
                        }
                    }
                }
            }
            // ---- 303S2 LIVE SEQUENCER — same tick as drum2 (no drift possible) ----
            // Loop length quantized to nearest power-of-2 ≥ note count so the
            // 303S loop always divides the 16-step bar evenly (1/2/4/8/16 notes).
            if (s303s2Count > 0) {
                uint8_t qLen = (s303s2Count <= 1) ? 1 :
                               (s303s2Count <= 2) ? 2 :
                               (s303s2Count <= 4) ? 4 :
                               (s303s2Count <= 8) ? 8 : 16;
                if (s303s2CurNote) { audioT303NoteOff(s303s2CurNote); s303s2CurNote=0; }
                if (s303s2Step < s303s2Count) {
                    uint8_t idx2 = (uint8_t)((s303s2Head - s303s2Count + s303s2Step + S303S2_LEN) % S303S2_LEN);
                    uint8_t nv = s303s2Seq[idx2];
                    if (nv > 0) {
                        uint8_t midi2 = (uint8_t)constrain((int)(nv-1) + (int)t303Oct*12, 0, 127);
                        audioT303NoteOn(midi2, (t303AccentOn?0.85f:0.5f)*gestSeqVol[1]);
                        s303s2CurNote = midi2;
                    }
                }
                s303s2Step = (uint8_t)((s303s2Step + 1) % qLen);
            }
        }
    }

    // ---- DR2 SEQUENCER (independent 4.4.4 hierarchical clock — 64th-note micro rate) ----
    // Doubled-hit deferred fires (from dr2Mod==3 hits)
    for (uint8_t pi = 0; pi < DRUM2_PADS; pi++) {
        if (dr2DblPendingAt[pi] && millis() >= dr2DblPendingAt[pi]) {
            playDrum(pi, drum2Volume[pi]*0.65f, dr2DblPitch[pi], drum2Decay[pi]*0.5f);
            dr2PadFlashMs[pi] = millis();
            dr2DblPendingAt[pi] = 0;
        }
    }
    // Auto-release for synth/303 triggers (dr2TriggerSynth/dr2TriggerT303) — the
    // generic synth engine holds a note until an explicit note-off, unlike drums'
    // one-shot samples, so every trigger schedules its own release here. Runs
    // unconditionally (not just while dr2Playing) so preview presses release too.
    unsigned long dr2Now = millis();
    for (uint8_t n = 0; n < DR2_NOTES; n++) {
        if (dr2SynthNoteOffAt[n] && dr2Now >= dr2SynthNoteOffAt[n]) {
            audioNoteOff(dr2SynthPlayedNote[n]);
            dr2SynthNoteOffAt[n] = 0;
        }
    }
    if (dr2T303NoteOffAt && dr2Now >= dr2T303NoteOffAt && dr2T303CurNote) {
        audioT303NoteOff(dr2T303CurNote);
        dr2T303CurNote = 0;
        dr2T303NoteOffAt = 0;
    }
    if (dr2Playing) {
        // BPM paces the STEP digit directly (60000/bpm ms/step) — matching the
        // original design ("le BPM correspondra au 4.X.4") — not a quarter-note
        // beat with 4 steps/beat like the rest of the codebase's 16-step
        // sequencers (DRUM2/303S/GEST all use 60000/bpm/4 for that reason, which
        // doesn't apply here). Getting this wrong once already made every DR2
        // step/micro pulse 4x faster than the BPM you set (e.g. 40 sounded like
        // 160) — confirmed by the user across multiple reports before finding
        // the actual cause here, rather than in selection-mask density.
        unsigned long microMs = (unsigned long)fmaxf(5.0f, 60000.0f / (float)bpm / 4.0f);
        if (millis() - dr2LastMicroMs >= microMs) {
            dr2LastMicroMs += microMs;
            if (millis() - dr2LastMicroMs > microMs) dr2LastMicroMs = millis(); // catch-up guard
            for (uint8_t pi = 0; pi < DRUM2_PADS; pi++) {
                uint8_t v = dr2Vel[pi][dr2PlayBeat][dr2PlayStep][dr2PlayMicro];
                if (v == 0) continue;
                uint8_t mod = dr2Mod[pi][dr2PlayBeat][dr2PlayStep][dr2PlayMicro];
                float vel = drum2Volume[pi] * v / 127.0f;
                if (pi == DRUM2_CH || pi == DRUM2_OH) {
                    uint8_t other = (pi == DRUM2_CH) ? DRUM2_OH : DRUM2_CH;
                    if (dr2Vel[other][dr2PlayBeat][dr2PlayStep][dr2PlayMicro] == 0) {
                        amy_event stop = amy_default_event();
                        stop.osc = DRUM_OSC_BASE + kDrum2Remap[other];
                        stop.velocity = 0.0f;
                        amy_add_event(&stop);
                    }
                }
                switch (mod) {
                    case 1: // Probabilistic 50%
                        if (random(2)) { playDrum(pi, vel, drum2Pitch[pi], drum2Decay[pi]); dr2PadFlashMs[pi]=millis(); }
                        break;
                    case 2: { // Random pitch — audible range 36-96 (C2-C7)
                        uint8_t rp=(uint8_t)constrain(36+(int)random(61),0,127);
                        playDrum(pi, vel, rp, drum2Decay[pi]); dr2PadFlashMs[pi]=millis();
                        break;
                    }
                    case 3: // Double hit: now + half the current micro-slot duration
                        playDrum(pi, vel, drum2Pitch[pi], drum2Decay[pi]);
                        dr2DblPendingAt[pi] = millis() + microMs/2;
                        dr2DblPitch[pi] = drum2Pitch[pi];
                        dr2PadFlashMs[pi] = millis();
                        break;
                    default:
                        playDrum(pi, vel, drum2Pitch[pi], drum2Decay[pi]);
                        dr2PadFlashMs[pi] = millis();
                        break;
                }
            }
            // Synth and 303 slots sound every tick regardless of which instrument is
            // currently UI-focused (dr2Instrument) — same simultaneous-playback model
            // as GEST's rows. No alteration modes for melodic hits (plain on/off).
            for (uint8_t n = 0; n < DR2_NOTES; n++) {
                if (dr2SynthVel[n][dr2PlayBeat][dr2PlayStep][dr2PlayMicro] == 0) continue;
                dr2TriggerSynth(n, 0.8f);
                dr2NoteFlashMs[n] = millis();
            }
            {
                // Monophonic like a real 303: at most one note sounds at a time, note-off
                // the previous one (even on repeat) before the new note-on.
                uint8_t t303Note = 0xFF;
                for (uint8_t n = 0; n < DR2_NOTES; n++) {
                    if (dr2T303Vel[n][dr2PlayBeat][dr2PlayStep][dr2PlayMicro] != 0) { t303Note = n; break; }
                }
                if (t303Note != 0xFF) {
                    dr2TriggerT303(t303Note, 0.75f);
                    dr2NoteFlashMs[t303Note] = millis();
                }
            }
            dr2PlayMicro++;
            if (dr2PlayMicro >= DR2H_MICROS) {
                dr2PlayMicro = 0;
                dr2PlayStep++;
                if (dr2PlayStep >= DR2H_STEPS) {
                    dr2PlayStep = 0;
                    dr2PlayBeat++;
                    if (dr2PlayBeat >= DR2H_BEATS) {
                        dr2PlayBeat = 0;
                        // Just completed a full bar (64 micro-slots) — LIVE mode auto-advances
                        // to the next filled pattern, like GEST's LIVE mode for other sequencers.
                        if (dr2PlayMode == DR2_LIVE) {
                            uint8_t nxt = 0xFF;
                            for (uint8_t p = 1; p <= DR2_PATS; p++) {
                                uint8_t c = (uint8_t)((dr2ActivePat + p) % DR2_PATS);
                                if (dr2PatFilled[c]) { nxt = c; break; }
                            }
                            if (nxt != 0xFF && nxt != dr2ActivePat) {
                                memcpy(dr2Pats[dr2ActivePat], dr2Vel, sizeof(dr2Vel));
                                memcpy(dr2ModPats[dr2ActivePat], dr2Mod, sizeof(dr2Mod));
                                memcpy(dr2SynthPats[dr2ActivePat], dr2SynthVel, sizeof(dr2SynthVel));
                                memcpy(dr2T303Pats[dr2ActivePat], dr2T303Vel, sizeof(dr2T303Vel));
                                dr2ActivePat = nxt;
                                memcpy(dr2Vel, dr2Pats[nxt], sizeof(dr2Vel));
                                memcpy(dr2Mod, dr2ModPats[nxt], sizeof(dr2Mod));
                                memcpy(dr2SynthVel, dr2SynthPats[nxt], sizeof(dr2SynthVel));
                                memcpy(dr2T303Vel, dr2T303Pats[nxt], sizeof(dr2T303Vel));
                            }
                        }
                    }
                }
            }
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

    // ---- ARPEGGIATOR (MODE_SYNTH + MODE_I303 + MODE_SYSEQ, speed = BPM 16th notes) ----
    bool arpActive = (currentMode==MODE_SYNTH || currentMode==MODE_I303 || currentMode==MODE_SYSEQ) && arpMode!=0 && !menuOpen;
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
            if (currentMode==MODE_I303) {
                if(arpCurrent!=0) playI303Off(arpCurrent);
                float vel303=constrain(0.75f+(cachedJoyX/64.0f)*0.4f,0.1f,1.5f);
                playI303On(noteToPlay, vel303);
            } else {
                if(arpCurrent!=0) audioNoteOff(arpCurrent);
                audioNoteOn(noteToPlay,0.8f);
                // SYSEQ: auto-record the arpeggiated note at the current step (mirrors the
                // direct-press recording used when arp is off).
                if (currentMode==MODE_SYSEQ && drum2Playing) {
                    bool found=false;
                    for(uint8_t i=0;i<SYSEQ_CHORD;i++) if(syseqNotes[drum2Step][i]==noteToPlay+1){found=true;break;}
                    if(!found){
                        for(uint8_t i=0;i<SYSEQ_CHORD;i++){
                            if(syseqNotes[drum2Step][i]==0){
                                syseqNotes[drum2Step][i]=noteToPlay+1;
                                syseqVels[drum2Step][i]=(uint8_t)constrain((int)(0.8f*127.0f),1,127);
                                break;
                            }
                        }
                    }
                }
            }
            arpCurrent=noteToPlay;
        } else if(arpNoteCount==0&&arpCurrent!=0){
            if(currentMode==MODE_I303) playI303Off(arpCurrent);
            else audioNoteOff(arpCurrent);
            arpCurrent=0;
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

    // ---- SAMPLE SQL: advance play head when current sample ends ----
    if(currentMode==MODE_SAMPLE&&samplePlayMode==4&&audioReady){
        if(g_sampleSqlCount>0 && (int32_t)(millis()-g_sampleSqlEndMs)>=0){
            g_sampleSqlPlayHead = (g_sampleSqlPlayHead + 1) % g_sampleSqlCount;
            sampleSqlStartHead();
        }
    }

    // ---- POTS + JOYSTICK (10ms) ----
    static unsigned long lastSlow=0;
    if(millis()-lastSlow>=10){
        lastSlow=millis();
        for(int i=0;i<16;i++) muxCache[i]=mux.getValue(i);
        cachedJoyX=getJoyX(); cachedJoyY=getJoyY();

        // ---- EXP: physics inside 10ms tick → fixed time step, no jitter ----
        if(currentMode==MODE_EXP && audioReady && !menuOpen){
            float jx = constrain(cachedJoyX/64.0f,-1.0f,1.0f);
            float jy = constrain(cachedJoyY/64.0f,-1.0f,1.0f);
            // Responsive direct follow: low friction so dot reacts in ~2 ticks (~20ms)
            expVelX = expVelX * 0.84f + jx * 0.003f;
            expVelY = expVelY * 0.84f + jy * 0.003f;
            expPosX = constrain(expPosX + expVelX, 0.0f, 1.0f);
            expPosY = constrain(expPosY + expVelY, 0.0f, 1.0f);
            if (expPosX <= 0.0f || expPosX >= 1.0f) expVelX = 0.0f;
            if (expPosY <= 0.0f || expPosY >= 1.0f) expVelY = 0.0f;

            // Push current pixel position into trail ring buffer (every 10ms tick)
            {
                int16_t tx = (int16_t)(18 + (int)(expPosX * 100.0f));
                int16_t ty = (int16_t)(10 + (int)(expPosY * 100.0f));
                tx = (int16_t)constrain((int)tx, 18, 118);
                ty = (int16_t)constrain((int)ty, 10, 110);
                expTrail[expTrailHead] = {tx, ty, millis()};
                expTrailHead = (expTrailHead + 1) % EXP_TRAIL_LEN;
                if (expTrailCount < EXP_TRAIL_LEN) expTrailCount++;
            }

            // Snap cursor to 32×32 grid for audio (display stays on raw float position)
            float gridPosX = (float)((int)(expPosX * 31.0f + 0.5f)) / 31.0f;
            float gridPosY = (float)((int)(expPosY * 31.0f + 0.5f)) / 31.0f;
            float jyForNote = 1.0f - gridPosY * 2.0f;
            uint8_t baseNote = expComputeNote(jyForNote);
            uint8_t arpMode  = expColSel[2];
            // X axis = arp BPM multiplier of global BPM: left=0.5×, center=1.0×, right=2.0×
            float arpMult = 0.5f + gridPosX * 1.5f;
            uint32_t arpInterval = (uint32_t)(60000.0f / (float)bpm / arpMult);
            arpInterval = constrain(arpInterval, 15u, 3000u);
            if (arpMode==0) {
                // Continuous mode: rate-limit note changes to reduce AMY load — the
                // minimum gap is now user-controlled via col3 (kExpGateMs[]) instead of a
                // fixed 25ms, doubling as a "gate length" feel control (short=skittery/
                // arpeggio-like retriggering, long=smoother/more legato).
                uint16_t gateMs = kExpGateMs[expColSel[3]];
                if (!expNoteOn || baseNote!=expCurNote) {
                    uint32_t nowN = millis();
                    if (!expNoteOn || (int32_t)(nowN - expLastNoteMs) >= gateMs) {
                        if (expNoteOn) audioPostNote(expCurNote, 0.f);
                        audioPostNote(baseNote, volume);
                        expNoteOn=true; expCurNote=baseNote;
                        expLastNoteMs = nowN;
                    }
                }
            } else {
                if (!expNoteOn || (int32_t)(millis()-expArpNextMs)>=0) {
                    if (expNoteOn) audioPostNote(expCurNote, 0.f);
                    if      (arpMode==1) expArpStep=(expArpStep+1)%4;
                    else if (arpMode==2) expArpStep=(expArpStep+3)%4;
                    else                 expArpStep=(uint8_t)random(4);
                    uint8_t playNote=(uint8_t)constrain(baseNote+kExpArpIntvl[expArpStep],0,127);
                    audioPostNote(playNote, volume);
                    expNoteOn=true; expCurNote=playNote;
                    expArpNextMs = millis() + arpInterval;
                }
            }
        }

        // ==================== EXP2 PHYSICS (10ms tick) ====================
        if (currentMode==MODE_EXP2 && !menuOpen) {
            float jx = constrain(cachedJoyX/64.0f,-1.0f,1.0f);
            float jy = constrain(cachedJoyY/64.0f,-1.0f,1.0f);

            // Polygon auto-rotates at speed set by pot (no joystick coupling)
            exp2HexAngle += exp2RotVel;

            // Joystick XY → direct gravity vector (instant response, no inertia). This is
            // deliberate, not a bug: gravity itself should feel immediate so tilting the
            // stick reads as an instant push on the balls. exp2GravAngle below is a
            // display-only convenience (drawn as an arrow, main.cpp ~6056) that only
            // updates while the stick is meaningfully deflected, so the arrow doesn't
            // jitter toward (0,0) when the stick recenters — it is intentionally NOT fed
            // back into gravX/gravY.
            float gravX = jx * exp2GravStr;
            float gravY = jy * exp2GravStr;
            // Update display angle only when stick is non-trivially deflected
            if (fabsf(jx) > 0.08f || fabsf(jy) > 0.08f)
                exp2GravAngle = atan2f(jy, jx);

            // Polygon vertices (N sides)
            uint8_t N = exp2NumSides;
            struct { float x, y; } pv[EXP2_MAX_SIDES];
            float angleStep = 2.0f*(float)M_PI / (float)N;
            for (int i=0;i<N;i++) {
                float a = exp2HexAngle + i*angleStep;
                pv[i].x = 64.0f + 40.0f*cosf(a);
                pv[i].y = 64.0f + 40.0f*sinf(a);
            }

            // Note-off gates
            uint32_t nowP = millis();
            for (int w=0;w<(int)N;w++) {
                if (exp2WallNoteOff[w] && nowP >= exp2WallNoteOff[w]) {
                    if (exp2WallNote[w]!=0xFF && audioReady) audioPostNote(exp2WallNote[w], 0.f);
                    exp2WallNote[w]=0xFF; exp2WallNoteOff[w]=0;
                }
            }

            for (int b=0;b<EXP2_MAX_BALLS;b++) {
                if (!exp2Balls[b].active) continue;
                // Apply gravity
                exp2Balls[b].vx += gravX;
                exp2Balls[b].vy += gravY;
                // Cap speed
                float spd = sqrtf(exp2Balls[b].vx*exp2Balls[b].vx + exp2Balls[b].vy*exp2Balls[b].vy);
                if (spd > exp2SpeedCap && spd > 0.001f) {
                    float s = exp2SpeedCap/spd;
                    exp2Balls[b].vx *= s; exp2Balls[b].vy *= s;
                }
                exp2Balls[b].x += exp2Balls[b].vx;
                exp2Balls[b].y += exp2Balls[b].vy;

                // Wall collisions: proper restitution — reflect only normal component,
                // slight wall friction on tangential (no double-damping)
                for (int w=0;w<(int)N;w++) {
                    float ax=pv[w].x, ay=pv[w].y;
                    float bx_=pv[(w+1)%N].x, by_=pv[(w+1)%N].y;
                    float edx=bx_-ax, edy=by_-ay;
                    float len=sqrtf(edx*edx+edy*edy);
                    if (len<0.001f) continue;
                    float nx=-edy/len, ny=edx/len;  // inward normal
                    float d=(exp2Balls[b].x-ax)*nx+(exp2Balls[b].y-ay)*ny;
                    if (d < EXP2_BALL_R) {
                        float vn=exp2Balls[b].vx*nx+exp2Balls[b].vy*ny;
                        if (vn < 0.0f) {
                            // Decompose: tangential fully preserved, normal reflects with restitution
                            float vtx = exp2Balls[b].vx - vn*nx;
                            float vty = exp2Balls[b].vy - vn*ny;
                            exp2Balls[b].vx = vtx + (-exp2Bounce * vn) * nx;
                            exp2Balls[b].vy = vty + (-exp2Bounce * vn) * ny;
                            exp2Balls[b].x  += (EXP2_BALL_R - d) * nx;
                            exp2Balls[b].y  += (EXP2_BALL_R - d) * ny;
                            exp2TriggerWall((uint8_t)w, fabsf(vn));
                        }
                    }
                }
                // Air resistance (tiny, prevents perpetual acceleration from gravity)
                // no air resistance — infinite inertia
            }

            // Ball-ball elastic collisions (equal mass)
            for (int ba=0; ba<EXP2_MAX_BALLS-1; ba++) {
                if (!exp2Balls[ba].active) continue;
                for (int bb=ba+1; bb<EXP2_MAX_BALLS; bb++) {
                    if (!exp2Balls[bb].active) continue;
                    float dx = exp2Balls[bb].x - exp2Balls[ba].x;
                    float dy = exp2Balls[bb].y - exp2Balls[ba].y;
                    float d2 = dx*dx + dy*dy;
                    float minD = EXP2_BALL_R * 2.0f;
                    if (d2 < minD*minD && d2 > 0.0001f) {
                        float dist = sqrtf(d2);
                        float bnx = dx/dist, bny = dy/dist;
                        // Relative velocity along collision normal
                        float dvn = (exp2Balls[bb].vx - exp2Balls[ba].vx)*bnx
                                  + (exp2Balls[bb].vy - exp2Balls[ba].vy)*bny;
                        if (dvn < 0.0f) {  // only when approaching
                            float imp = dvn * exp2Bounce;
                            exp2Balls[ba].vx += imp * bnx;
                            exp2Balls[ba].vy += imp * bny;
                            exp2Balls[bb].vx -= imp * bnx;
                            exp2Balls[bb].vy -= imp * bny;
                        }
                        // Separate overlapping balls
                        float ov = (minD - dist) * 0.5f;
                        exp2Balls[ba].x -= bnx * ov;  exp2Balls[ba].y -= bny * ov;
                        exp2Balls[bb].x += bnx * ov;  exp2Balls[bb].y += bny * ov;
                    }
                }
            }
        }

        // ==================== EXP3 PHYSICS (10ms tick) ====================
        if (currentMode==MODE_EXP3 && !menuOpen) {
            float jx = constrain(cachedJoyX/64.0f,-1.0f,1.0f);
            float jy = constrain(cachedJoyY/64.0f,-1.0f,1.0f);

            // Joystick X = speed inertia: push right → accelerate, left → decelerate.
            // Was a plain one-pole low-pass (no real inertia despite exp3SpeedVel's name
            // and comment implying one) — exp3SpeedVel was declared but never actually
            // read anywhere. Real spring/velocity integrator: exp3SpeedVel accumulates
            // toward the target, damped each tick, so the orbit speed can overshoot and
            // settle rather than just smoothly creeping — genuine momentum feel.
            float targetSpeed = 1.0f + jx * 2.5f;  // 0 = paused, 1 = normal, 3.5 = fast
            exp3SpeedVel += (targetSpeed - exp3SpeedMul) * 0.05f;
            exp3SpeedVel *= 0.90f; // damping
            exp3SpeedMul += exp3SpeedVel;
            exp3SpeedMul = constrain(exp3SpeedMul, 0.0f, 5.0f);

            // Joystick Y: gate duration (short to long)
            float jyN = (jy + 1.0f) * 0.5f;  // 0..1
            exp3GateMs = (uint16_t)(50 + jyN * 550.0f);  // 50..600ms

            // Base omega for orbit 0 (1 beat) in rad/tick at current BPM
            // 1 beat = 6000/BPM ticks → omega = 2π × BPM / 6000
            float bpmOmega = 2.0f * (float)M_PI * (float)bpm / 6000.0f * exp3SpeedMul;

            uint32_t now3 = millis();
            for (int b=0;b<EXP3_MAX_BALLS;b++) {
                if (!exp3Balls[b].active) continue;
                uint8_t or_ = exp3Balls[b].orbitRow;
                float omega = bpmOmega / kExp3Beats[or_];  // inner = fast
                exp3Balls[b].angle += omega;
                if (exp3Balls[b].angle > 2.0f*(float)M_PI) exp3Balls[b].angle -= 2.0f*(float)M_PI;

                // Trigger zone at top of screen: angle ≈ 3π/2 (sin ≈ -1)
                float da = exp3Balls[b].angle - 3.0f*(float)M_PI/2.0f;
                while (da > (float)M_PI)  da -= 2.0f*(float)M_PI;
                while (da < -(float)M_PI) da += 2.0f*(float)M_PI;
                bool inZone = fabsf(da) < (omega * 2.0f + 0.12f);  // zone widens at high speed

                if (inZone && !exp3Balls[b].triggered && audioReady) {
                    exp3Balls[b].triggered = true;
                    uint8_t note = exp3ComputeNote((uint8_t)b, or_);
                    if (exp3BallNote[b] != 0xFF) audioNoteOff(exp3BallNote[b]);
                    exp3BallNote[b] = note;
                    float vel = constrain(0.5f + (float)(EXP3_NUM_ORBITS-1-or_) * 0.12f, 0.4f, 0.9f) * volume;
                    audioNoteOn(note, vel);
                    exp3NoteOffMs[b] = now3 + (uint32_t)exp3GateMs;
                } else if (!inZone) {
                    exp3Balls[b].triggered = false;
                }
                // Note-off gate
                if (exp3NoteOffMs[b] && now3 >= exp3NoteOffMs[b] && audioReady) {
                    if (exp3BallNote[b] != 0xFF) audioNoteOff(exp3BallNote[b]);
                    exp3NoteOffMs[b] = 0;
                }
            }
        }

        // ==================== LIFE (10ms tick) ====================
        if (currentMode==MODE_LIFE && !menuOpen && !lifePaused) {
            uint32_t stepMs = (uint32_t)(60000.0f / (float)bpm * kDelaySubdiv[lifeTickDivIdx]);
            stepMs = constrain(stepMs, 40UL, 2000UL);
            uint32_t nowL = millis();
            if (nowL - lifeLastStepMs >= stepMs) {
                lifeLastStepMs = nowL;
                lifeStep();
            }
        }

        // ==================== SWARM PHYSICS (10ms tick) ====================
        // Explicitly reuses EXP2's ball-physics scaffolding: struct shape, polygon-arena
        // rotation/containment math, and speed-cap clamp are the same idioms as EXP2's
        // physics tick above (see main.cpp's EXP2 block) — only the per-boid force kernel
        // (cohesion/separation/alignment replacing EXP2's elastic wall/ball collisions)
        // and the joystick-steered attractor-zone note trigger are new.
        if (currentMode==MODE_SWARM && !menuOpen) {
            float jx = constrain(cachedJoyX/64.0f,-1.0f,1.0f);
            float jy = constrain(cachedJoyY/64.0f,-1.0f,1.0f);
            swarmHexAngle += swarmRotVel;

            float attrX = jx * 18.0f, attrY = jy * 18.0f;
            float cohW = 0.006f * swarmFlockBal, sepW = 0.05f * (1.0f - swarmFlockBal), aliW = 0.03f * swarmFlockBal;

            // Arena polygon vertices — same origin/radius (50) as the rotating hexagon
            // drawn in the OLED renderer (main.cpp, MODE_SWARM case), so boid coords
            // (relative to that same origin) can be edge-tested against it directly.
            const float SWARM_ARENA_R = 50.0f, SWARM_BALL_R = 2.5f, SWARM_BOUNCE = 0.85f;
            struct { float x, y; } spv[EXP2_MAX_SIDES];
            uint8_t sN = swarmNumSides;
            float sAngleStep = 2.0f*(float)M_PI / (float)sN;
            for (int i=0;i<sN;i++) {
                float a = swarmHexAngle + i*sAngleStep;
                spv[i].x = SWARM_ARENA_R*cosf(a);
                spv[i].y = SWARM_ARENA_R*sinf(a);
            }

            for (int i=0;i<SWARM_MAX_BOIDS;i++) {
                if (!swarmBoids[i].active) continue;
                SwarmBoid &s = swarmBoids[i];
                float cx=0,cy=0,sx=0,sy=0,ax=0,ay=0; int neigh=0;
                for (int j=0;j<SWARM_MAX_BOIDS;j++) {
                    if (j==i || !swarmBoids[j].active) continue;
                    float dx = swarmBoids[j].x - s.x, dy = swarmBoids[j].y - s.y;
                    float d2 = dx*dx+dy*dy;
                    if (d2 < 400.0f) { // neighbor radius 20
                        cx += swarmBoids[j].x; cy += swarmBoids[j].y;
                        ax += swarmBoids[j].vx; ay += swarmBoids[j].vy;
                        neigh++;
                        if (d2 < 25.0f && d2 > 0.001f) { sx -= dx/d2; sy -= dy/d2; } // separation, inverse-dist weighted
                    }
                }
                if (neigh > 0) {
                    cx = cx/neigh - s.x; cy = cy/neigh - s.y; // vector toward local flock center
                    ax = ax/neigh; ay = ay/neigh;
                    s.vx += cx*cohW + sx*sepW + ax*aliW;
                    s.vy += cy*cohW + sy*sepW + ay*aliW;
                }
                // Joystick attractor: mild pull toward the steered point
                s.vx += (attrX - s.x) * swarmAttractStr * 0.01f;
                s.vy += (attrY - s.y) * swarmAttractStr * 0.01f;

                float sp = sqrtf(s.vx*s.vx + s.vy*s.vy);
                if (sp > swarmSpeedCap) { s.vx = s.vx/sp*swarmSpeedCap; s.vy = s.vy/sp*swarmSpeedCap; }
                s.x += s.vx * 0.15f; s.y += s.vy * 0.15f;

                // Hexagon-wall collision: real per-edge reflection against the rotating
                // arena (same math as EXP2's polygon-wall bounce) — replaces a previous
                // purely-circular soft pullback that never actually interacted with the
                // hexagon drawn on screen (the bug report this fixes).
                for (int w=0;w<(int)sN;w++) {
                    float ax=spv[w].x, ay=spv[w].y;
                    float bx_=spv[(w+1)%sN].x, by_=spv[(w+1)%sN].y;
                    float edx=bx_-ax, edy=by_-ay;
                    float len=sqrtf(edx*edx+edy*edy);
                    if (len<0.001f) continue;
                    float nx=-edy/len, ny=edx/len;  // inward normal
                    float d=(s.x-ax)*nx+(s.y-ay)*ny;
                    if (d < SWARM_BALL_R) {
                        float vn=s.vx*nx+s.vy*ny;
                        if (vn < 0.0f) {
                            float vtx = s.vx - vn*nx;
                            float vty = s.vy - vn*ny;
                            s.vx = vtx + (-SWARM_BOUNCE * vn) * nx;
                            s.vy = vty + (-SWARM_BOUNCE * vn) * ny;
                            s.x  += (SWARM_BALL_R - d) * nx;
                            s.y  += (SWARM_BALL_R - d) * ny;
                        }
                    }
                }

                // Attractor-zone note trigger: edge-detect entering the small trigger radius.
                float dax = s.x - attrX, day = s.y - attrY;
                bool inZ = (dax*dax + day*day) < (swarmZoneRadius*swarmZoneRadius);
                if (inZ && !s.inZone && audioReady) {
                    s.inZone = true;
                    uint8_t note = swarmComputeNote((uint8_t)i);
                    if (swarmBoidNote[i] != 0xFF) audioNoteOff(swarmBoidNote[i]);
                    swarmBoidNote[i] = note;
                    audioNoteOn(note, 0.6f * volume);
                    swarmNoteOffMs[i] = millis() + 220;
                } else if (!inZ) {
                    s.inZone = false;
                }
            }
            uint32_t nowS = millis();
            for (int i=0;i<SWARM_MAX_BOIDS;i++) {
                if (swarmNoteOffMs[i] && nowS >= swarmNoteOffMs[i] && audioReady) {
                    if (swarmBoidNote[i] != 0xFF) audioNoteOff(swarmBoidNote[i]);
                    swarmNoteOffMs[i] = 0;
                }
            }
        }

        // ==================== GEN (10ms tick) ====================
        // Advances whichever generator is selected; a returned column of -1 (EUCL rests)
        // produces no note this tick. Monophonic on purpose — the point is auditioning one
        // texture/voice combination clearly, not building a dense pattern.
        if (currentMode==MODE_GEN && !menuOpen && !genPaused) {
            uint32_t stepMs = (uint32_t)(60000.0f / (float)bpm * kDelaySubdiv[genTickDivIdx]);
            stepMs = constrain(stepMs, 40UL, 2000UL);
            uint32_t nowG = millis();
            if (nowG - genLastStepMs >= stepMs) {
                genLastStepMs = nowG;
                int8_t col = genStep();
                if (col >= 0 && audioReady) {
                    genLastCol = (uint8_t)col;
                    if (genLastNote != 0xFF) audioNoteOff(genLastNote);
                    uint8_t note = genComputeNote((uint8_t)col, genOctave);
                    genLastNote = note;
                    audioNoteOn(note, 0.75f * volume);
                }
            }
        }

        readPots();
        s_battVSmooth += (muxCache[1] / 4095.0f * 10.4f - s_battVSmooth) * 0.05f;

        // Pot 0 = Volume (always)
        static float lv=-1;
        volume=pots[0].value;
        if(fabsf(volume-lv)>0.01f){
            float effVol = (currentMode == MODE_POKEMON)
                         ? volume * kPokemon[pkmnSelected].gain
                         : volume;
            audioSetVolume(effVol);
            audioSetSampleVolume(volume);
            lv=volume;
        }

        // Pot 1 (P2) = Pokémon selection in MODE_POKEMON
        if (currentMode == MODE_POKEMON) {
            static float lpPkmn = -1.0f;
            if (fabsf(pots[1].value - lpPkmn) > 0.005f) {
                uint8_t idx = (uint8_t)(pots[1].value * (PKMN_COUNT - 0.01f));
                if (idx != pkmnSelected) {
                    pkmnSelected = idx;
                    pkmnApply();
                }
                lpPkmn = pots[1].value;
            }
        }
        // GEST2 (MODE_DR2): pot 2 cycles the Synth instrument's sound, same
        // pot-driven cycling pattern as MODE_POKEMON above / MODE_SYNTH below
        // (idx = pot * (COUNT-eps), apply on a small hysteresis threshold) —
        // only meaningful while the Synth slot is focused.
        if (currentMode == MODE_DR2 && dr2Instrument == DR2_INSTR_SYNTH) {
            static float lpDr2Shape = -1.0f;
            if (fabsf(pots[1].value - lpDr2Shape) > 0.01f) {
                uint8_t si = (uint8_t)(pots[1].value * ((uint8_t)SHAPE_COUNT - 0.01f));
                if (si != currentShape) {
                    currentShape = (SynthShape)si;
                    audioSetShape(currentShape);
                }
                lpDr2Shape = pots[1].value;
            }
        }
        // Pot 2 (P3) = BPM (global, always)
        {
            static float lpBpm=-1;
            if(fabsf(pots[2].value-lpBpm)>0.003f){
                bpm=(uint16_t)(40.0f+pots[2].value*pots[2].value*560.0f);  // quadratic: more rotation for low BPM precision
                if(bpm<40) bpm=40; if(bpm>600) bpm=600;
                lpBpm=pots[2].value;
                Serial.printf("B:%d\n", bpm);
                if(fxList[5].active && audioReady) applyFxEffect(5); // re-sync delay to new BPM
                if(fxList[7].active && audioReady) applyFxEffect(7); // re-sync resecho to new BPM
            }
        }

        // GEST mode: pots 3-6 = sequencer volumes with pickup (don't jump when switching from 303/FX)
        if (currentMode == MODE_GEST && !menuOpen) {
            for (uint8_t gi=0; gi<GEST_NSEQ; gi++) {
                if (fabsf(pots[3+gi].value - lpGestPots[gi]) > 0.002f) {
                    gestSeqVol[gi] = pots[3+gi].value;
                    lpGestPots[gi] = pots[3+gi].value;
                }
            }
        }

        // OVERLAY_FX_MOD pot dispatch: centralized here (not duplicated per-mode like the
        // FX-active-param dispatch below) since its meaning never depends on currentMode —
        // it's always "configure the automation for fxList[fxSelected]'s selected param."
        if (audioReady && s_overlay == OVERLAY_FX_MOD) {
            static float lpFm[4] = {-1,-1,-1,-1};
            bool changed = false;
            if (fabsf(pots[3].value - lpFm[0]) > 0.003f) { lpFm[0]=pots[3].value; changed=true; } // P4 depth
            if (fabsf(pots[4].value - lpFm[1]) > 0.003f) { lpFm[1]=pots[4].value; changed=true; } // P5 rate
            if (fabsf(pots[5].value - lpFm[2]) > 0.003f) { lpFm[2]=pots[5].value; changed=true; } // P6 shape
            if (fabsf(pots[6].value - lpFm[3]) > 0.003f) { lpFm[3]=pots[6].value; changed=true; } // P7 bpm-sync/div
            if (changed) {
                float depth = pots[3].value;
                if (s_fxModEditSlot < 0 && depth > 0.01f) {
                    s_fxModEditSlot = modSlotAllocFxParam();
                }
                if (s_fxModEditSlot >= 0) {
                    ModSlot &s = gModSlots[s_fxModEditSlot];
                    s.destKind = MODDEST_FX_PARAM;
                    s.destA = fxSelected; s.destB = s_fxModEditParam;
                    s.depth = depth;
                    s.rateHz = 0.1f + pots[4].value * 19.9f;
                    s.shape = (ModWaveShape)constrain((int)(pots[5].value*3.99f), 0, MODSHAPE_COUNT-1);
                    s.bpmSync = pots[6].value > 0.5f;
                    if (s.bpmSync) s.bpmDivIdx = (uint8_t)constrain((int)((pots[6].value-0.5f)*2.0f*6.99f), 0, DELAY_SUBDIV_COUNT-1);
                    s.active = depth > 0.005f;
                }
            }
        } else if(audioReady&&!menuOpen){
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
                                    else if (fxSelected==0 && p==3) {
                                        fxList[0].params[3]=floorf(pots[pIdx[p]].value*3.9999f);
                                        s_filtMetaChanged=true;
                                    } else {
                                        fxList[fxSelected].params[p]=mn+(mx-mn)*pots[pIdx[p]].value;
                                        if (fxSelected==0) s_filtMetaChanged=true;
                                    }
                                    lp_fx[p]=pots[pIdx[p]].value;
                                    changed=true;
                                }
                            }
                            if(changed && fxSelected!=0) applyFxEffect(fxSelected);
                            else if(changed && fxSelected==0 && fxList[0].active && s_filtMetaChanged) { applyFxEffect(0); s_filtMetaChanged=false; }
                        }
                    }
                    break;
                }
                case MODE_POKEMON: {
                    // Pot 1 unused (shape set by pokemon); pots 3-6 = active FX params
                    if (fxList[fxSelected].active) {
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
                                else if (fxSelected==0 && p==3) {
                                    fxList[0].params[3]=floorf(pots[pIdx[p]].value*3.9999f);
                                    s_filtMetaChanged=true;
                                } else {
                                    fxList[fxSelected].params[p]=mn+(mx-mn)*pots[pIdx[p]].value;
                                    if (fxSelected==0) s_filtMetaChanged=true;
                                }
                                lp_fx[p]=pots[pIdx[p]].value;
                                changed=true;
                            }
                        }
                        if(changed && fxSelected!=0) applyFxEffect(fxSelected);
                        else if(changed && fxSelected==0 && fxList[0].active && s_filtMetaChanged) { applyFxEffect(0); s_filtMetaChanged=false; }
                    }
                    break;
                }
                case MODE_OMNI: {
                    static float lp1=-1, lp2=-1, lp3=-1;
                    // Pot 1 = chord/strum instrument shape
                    if(fabsf(pots[1].value-lp1)>0.01f){
                        uint8_t si=(uint8_t)(pots[1].value*((uint8_t)SHAPE_COUNT-0.01f));
                        if(si!=currentShape){currentShape=(SynthShape)si;audioSetShape(currentShape);}
                        lp1=pots[1].value;
                    }
                    // Pot 2 = strum volume (always available)
                    if(fabsf(pots[2].value-lp2)>0.005f){
                        omniStrumVol = 0.1f + pots[2].value * 1.9f; // 0.1→2.0
                        lp2=pots[2].value;
                    }
                    // Pot 3 = strum wave shape (SINE/TRI/SAW/SQUARE) when no FX active
                    if(!fxList[fxSelected].active && fabsf(pots[3].value-lp3)>0.01f){
                        static const uint8_t kStrumWaves[] = {SINE, TRIANGLE, SAW_DOWN, PULSE};
                        uint8_t wi = (uint8_t)(pots[3].value * 3.99f);
                        omniStrumWave = kStrumWaves[wi];
                        lp3=pots[3].value;
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
                                else if (fxSelected==0 && p==3) {
                                    fxList[0].params[3]=floorf(pots[pIdx[p]].value*3.9999f);
                                    s_filtMetaChanged=true;
                                } else
                                    fxList[fxSelected].params[p]=mn+(mx-mn)*pots[pIdx[p]].value;
                                lpo[p]=pots[pIdx[p]].value;
                                changed=true;
                            }
                        }
                        // LPF changes are applied smoothly by the 10ms tick (anti-zipper)
                        if(changed && fxSelected!=0) applyFxEffect(fxSelected);
                        else if(changed && fxSelected==0 && fxList[0].active && s_filtMetaChanged) { applyFxEffect(0); s_filtMetaChanged=false; }
                    }
                    break;
                }
                case MODE_LIGHT: {
                    // Pots 3-6: N, speed, hue, intensity — read directly in LED tick
                    // Nothing to do here; values used in LED rendering section.
                    break;
                }
                case MODE_SAMPLE: {
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
                                if (fxSelected==0 && p==0)
                                    fxList[0].params[0]=mn*powf(mx/mn, pots[pIdx[0]].value);
                                else if (fxSelected==0 && p==3)
                                    fxList[0].params[3]=floorf(pots[pIdx[p]].value*3.9999f);
                                else
                                    fxList[fxSelected].params[p]=mn+(mx-mn)*pots[pIdx[p]].value;
                                lp_fx_s[p]=pots[pIdx[p]].value;
                                changed=true;
                            }
                        }
                        if(changed) applyAllFx();
                    }
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
                                    else if (fxSelected==0 && p==3) {
                                        fxList[0].params[3]=floorf(pots[3+p].value*3.9999f);
                                        s_filtMetaChanged=true;
                                    } else {
                                        fxList[fxSelected].params[p]=mn+(mx-mn)*pots[3+p].value;
                                        if (fxSelected==0) s_filtMetaChanged=true;
                                    }
                                    lp303sfx[p]=pots[3+p].value;
                                    fxChanged=true;
                                }
                            }
                            if (fxChanged && fxSelected!=0) applyFxEffect(fxSelected);
                            else if (fxChanged && fxSelected==0 && fxList[0].active && s_filtMetaChanged) { applyFxEffect(0); s_filtMetaChanged=false; }
                        }
                        for (int p = 0; p < 4; p++) lp303[p] = pots[3+p].value;
                        break;
                    }
                    // P2: per-wave texture — SQR=duty, NAP=width, TRI2/SW3=asymFold, SW2=blend, PINK=none
                    {
                        if(fabsf(pots[1].value-g_lp303swf)>0.005f){
                            float p2=pots[1].value;
                            if(t303IsSubOctWave(t303Wave)){
                                if(audioReady) audioSW2SetBlend(p2);
                            } else if(t303Wave==T303_PINK_WAVE){
                                // P2 sans effet sur le bruit rose (le filtre 303 façonne le son)
                            } else if(t303Wave==PULSE){
                                if(audioReady) audioT303Duty(0.5f-p2*0.48f);
                            } else if(t303Wave==T303_NAP_WAVE){
                                if(audioReady) audioT303Wavefold(p2);
                            } else if(t303Wave==T303_TRI2_WAVE||t303Wave==T303_SAW2_WAVE){
                                if(audioReady) audioT303WavefoldAsym(p2);
                            } else {
                                if(audioReady) audioT303Wavefold(p2);
                            }
                            g_lp303swf=p2;
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
                case MODE_303S2: {
                    // Same pots as 303S
                    {
                        if(fabsf(pots[1].value-g_lp303s2wf)>0.005f){
                            float p2=pots[1].value;
                            if(t303IsSubOctWave(t303Wave))                      { if(audioReady) audioSW2SetBlend(p2); }
                            else if(t303Wave==T303_PINK_WAVE)                {}
                            else if(t303Wave==PULSE)                         { if(audioReady) audioT303Duty(0.5f-p2*0.48f); }
                            else if(t303Wave==T303_NAP_WAVE)                 { if(audioReady) audioT303Wavefold(p2); }
                            else if(t303Wave==T303_TRI2_WAVE||t303Wave==T303_SAW2_WAVE) { if(audioReady) audioT303WavefoldAsym(p2); }
                            else                                             { if(audioReady) audioT303Wavefold(p2); }
                            g_lp303s2wf=p2;
                        }
                    }
                    bool ch303s2=false;
                    if(fabsf(pots[3].value-lp303[0])>0.002f){ t303Reso=1.0f+pots[3].value*2.0f;           lp303[0]=pots[3].value; ch303s2=true; }
                    if(fabsf(pots[4].value-lp303[1])>0.002f){ t303EnvMod=pots[4].value*10.0f;              lp303[1]=pots[4].value; ch303s2=true; }
                    if(fabsf(pots[5].value-lp303[2])>0.002f){ t303Duration=pots[5].value; lp303[2]=pots[5].value; t303Decay=30.0f*powf(100.0f,t303Duration); audioT303SetSustain(0.0f); ch303s2=true; }
                    if(fabsf(pots[6].value-lp303[3])>0.002f){ t303Cutoff=80.0f*powf(25.0f,pots[6].value); lp303[3]=pots[6].value; ch303s2=true; }
                    if(ch303s2 && audioReady) audioT303Params(t303Cutoff,t303Reso,t303EnvMod,t303Decay);
                    break;
                }
                case MODE_I303: {
                    // P2: same wave texture as 303S (PINK: no effect)
                    {
                        static float lpI303wf=-1.0f;
                        if(fabsf(pots[1].value-lpI303wf)>0.005f){
                            float p2=pots[1].value;
                            if(t303IsSubOctWave(t303Wave))                      { if(audioReady) audioSW2SetBlend(p2); }
                            else if(t303Wave==T303_PINK_WAVE)                {}  // PINK: filtre 303 suffit
                            else if(t303Wave==PULSE)                         { if(audioReady) audioT303Duty(0.5f-p2*0.48f); }
                            else if(t303Wave==T303_NAP_WAVE)                 { if(audioReady) audioT303Wavefold(p2); }
                            else if(t303Wave==T303_TRI2_WAVE||t303Wave==T303_SAW2_WAVE) { if(audioReady) audioT303WavefoldAsym(p2); }
                            else                                             { if(audioReady) audioT303Wavefold(p2); }
                            lpI303wf=p2;
                        }
                    }
                    bool chI303=false;
                    if(fabsf(pots[3].value-lp303[0])>0.002f){ t303Reso=1.0f+pots[3].value*2.0f;           lp303[0]=pots[3].value; chI303=true; }
                    if(fabsf(pots[4].value-lp303[1])>0.002f){ t303EnvMod=pots[4].value*10.0f;              lp303[1]=pots[4].value; chI303=true; }
                    if(fabsf(pots[5].value-lp303[2])>0.002f){ t303Duration=pots[5].value; lp303[2]=pots[5].value; audioT303SetSustain(1.0f); chI303=true; }  // I303: sustain while held, Dur unused
                    if(fabsf(pots[6].value-lp303[3])>0.002f){ t303Cutoff=80.0f*powf(25.0f,pots[6].value); lp303[3]=pots[6].value; chI303=true; }
                    if(chI303 && audioReady) audioT303Params(t303Cutoff,t303Reso,t303EnvMod,t303Decay);
                    break;
                }
                case MODE_SS2: {
                    // FX params editable via pots 3-6 whenever any FX is active (not just when overlay is open)
                    bool anyFxSS2 = false;
                    for (int f=0;f<FX_COUNT;f++) if(fxList[f].active){anyFxSS2=true;break;}
                    if (anyFxSS2) {
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
                                else if (fxSelected==0 && p==3) {
                                    fxList[0].params[3]=floorf(pots[3+p].value*3.9999f);
                                    s_filtMetaChanged=true;
                                } else {
                                    fxList[fxSelected].params[p]=mn+(mx-mn)*pots[3+p].value;
                                    if (fxSelected==0) s_filtMetaChanged=true;
                                }
                                lpSS2fx[p]=pots[3+p].value; fxChanged=true;
                            }
                        }
                        if (fxChanged && fxSelected!=0) applyFxEffect(fxSelected);
                        else if (fxChanged && fxSelected==0 && fxList[0].active && s_filtMetaChanged) { applyFxEffect(0); s_filtMetaChanged=false; }
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
                        drum2Volume[drum2SelPad]=pots[5].value*2.0f;  // 0-2x headroom, see GEST2's own comment
                        lpD2[2]=pots[5].value;
                    }
                    break;
                }
                case MODE_DR2: {
                    static float lpDr2fx[4]  = {-1.f,-1.f,-1.f,-1.f};
                    static uint8_t lpDr2FxSel = 0xFF;
                    static float lp303[4]    = {-1.f,-1.f,-1.f,-1.f};
                    static uint8_t lp303Instr = 0xFF;
                    // Same pads/params as DRUM2 (dr2SelPad indexes the same drum2Pitch/
                    // Decay/Volume arrays) — mirrors MODE_DRUM2's pot handling exactly.
                    bool anyFxActive = false;
                    for (int f=0;f<FX_COUNT;f++) if(fxList[f].active){anyFxActive=true;break;}
                    if (anyFxActive) {
                        if (lpDr2FxSel != fxSelected || fxPotNeedSync) {
                            for (int p=0;p<4;p++) lpDr2fx[p]=pots[3+p].value;
                            lpDr2FxSel=fxSelected; fxPotNeedSync=false;
                        }
                        bool fxChanged=false;
                        for (int p=0;p<4;p++) {
                            if (fxList[fxSelected].paramNames[p][0]=='\0') continue;
                            if (fabsf(pots[3+p].value-lpDr2fx[p])>0.001f) {
                                float mn=fxList[fxSelected].paramMin[p];
                                float mx=fxList[fxSelected].paramMax[p];
                                if (fxSelected==0 && p==0)
                                    fxList[0].params[0]=mn*powf(mx/mn, pots[3].value);
                                else
                                    fxList[fxSelected].params[p]=mn+(mx-mn)*pots[3+p].value;
                                lpDr2fx[p]=pots[3+p].value; fxChanged=true;
                            }
                        }
                        if (fxChanged && fxSelected!=0) applyFxEffect(fxSelected);
                        break;
                    }
                    lpDr2FxSel = 0xFF;
                    if (dr2Instrument == DR2_INSTR_T303) {
                        static float lpDr2303wf = -1.0f;
                        if (lp303Instr != DR2_INSTR_T303) {
                            for (int p=0;p<4;p++) lp303[p]=pots[3+p].value;
                            lpDr2303wf = pots[1].value;
                            lp303Instr = DR2_INSTR_T303;
                        }
                        // P2 = per-wave texture — same mapping as MODE_303S/303S2/I303's
                        // own P2 handling (SQR=duty, NAP=width, TRI2/SAW2=asymFold,
                        // sub-oct waves=blend, PINK=no effect).
                        if(fabsf(pots[1].value-lpDr2303wf)>0.005f){
                            float p2=pots[1].value;
                            if(!dr2T303Inited){ audioT303Init(2000.0f,1.5f,2.0f,200.0f,0); dr2T303Inited=true; }
                            if(t303IsSubOctWave(t303Wave)){ if(audioReady) audioSW2SetBlend(p2); }
                            else if(t303Wave==T303_PINK_WAVE){ /* no effect on pink noise */ }
                            else if(t303Wave==PULSE){ if(audioReady) audioT303Duty(0.5f-p2*0.48f); }
                            else if(t303Wave==T303_NAP_WAVE){ if(audioReady) audioT303Wavefold(p2); }
                            else if(t303Wave==T303_TRI2_WAVE||t303Wave==T303_SAW2_WAVE){ if(audioReady) audioT303WavefoldAsym(p2); }
                            else{ if(audioReady) audioT303Wavefold(p2); }
                            lpDr2303wf=p2;
                            dr2PopupUntil = millis() + DR2_POPUP_MS;
                        }
                        // P4-P7 = Reso/EnvMod/Duration(->Decay)/Cutoff — same formulas as
                        // MODE_303S's own pot handling, so the feel matches across modes.
                        bool ch303=false;
                        if(fabsf(pots[3].value-lp303[0])>0.002f){ t303Reso=1.0f+pots[3].value*2.0f;      lp303[0]=pots[3].value; ch303=true; }
                        if(fabsf(pots[4].value-lp303[1])>0.002f){ t303EnvMod=pots[4].value*10.0f;         lp303[1]=pots[4].value; ch303=true; }
                        if(fabsf(pots[5].value-lp303[2])>0.002f){ t303Duration=pots[5].value; t303Decay=30.0f*powf(100.0f,t303Duration); lp303[2]=t303Duration; audioT303SetSustain(0.0f); ch303=true; }
                        if(fabsf(pots[6].value-lp303[3])>0.002f){ t303Cutoff=80.0f*powf(25.0f,pots[6].value); lp303[3]=pots[6].value; ch303=true; }
                        if (ch303) {
                            if (audioReady) audioT303Params(t303Cutoff,t303Reso,t303EnvMod,t303Decay);
                            dr2PopupUntil = millis() + DR2_POPUP_MS;
                        }
                        break;
                    }
                    lp303Instr = 0xFF;
                    if (dr2Instrument == DR2_INSTR_SYNTH) {
                        // P4 = volume (0-2x headroom). Relative (rawDelta), not absolute
                        // pots[3].value*2 — see the Drums block below for why: pots[3].value
                        // is a clamped 0..1 running integral (the encoders are infinite, not a
                        // fixed-travel pot), so an absolute conversion cuts off headroom
                        // depending on wherever that integral happens to sit when you arrive
                        // here (e.g. from adjusting Drums' pitch on the same physical P4).
                        if (fabsf(pots[3].rawDelta) > 0.0001f) {
                            dr2SynthVolume = constrain(dr2SynthVolume + pots[3].rawDelta*2.0f, 0.0f, 2.0f);
                            dr2PopupUntil = millis() + DR2_POPUP_MS;
                        }
                        break;
                    }
                    if (dr2Instrument != DR2_INSTR_DRUMS) break;  // TBD: nothing on P4-P7 yet
                    // No active FX, Drums focused — P3=Pitch  P4=Decay  P5=Volume (per selected
                    // pad). RELATIVE controls, applied via rawDelta (this frame's relative
                    // rotation, unclamped) rather than converting pots[i].value's absolute
                    // position: the encoders are infinite/endless (EncPot accumulates rotation
                    // into value, it's not a fixed-travel pot), and pots[i].value is a single
                    // running integral SHARED across all 8 pads with no inherent tie to any one
                    // pad's stored value. An absolute conversion (pitch=value*127) meant
                    // switching pads and barely touching the knob would instantly snap the new
                    // pad to wherever the OLD pad had left that integral — and using value's
                    // own frame-to-frame DELTA instead still inherited value's clamp, silently
                    // capping how far a pad's parameter could be pushed once the integral pinned
                    // at 0 or 1. rawDelta has no such ceiling, so any value is reachable
                    // regardless of the previous pad's value or the integral's position.
                    if (fabsf(pots[3].rawDelta) > 0.0001f) {
                        drum2Pitch[dr2SelPad] = (uint8_t)constrain((int)drum2Pitch[dr2SelPad] + (int)roundf(pots[3].rawDelta*127.0f), 0, 127);
                        dr2PopupUntil = millis() + DR2_POPUP_MS;
                    }
                    if (fabsf(pots[4].rawDelta) > 0.0001f) {
                        drum2Decay[dr2SelPad] = constrain(drum2Decay[dr2SelPad] + pots[4].rawDelta*2000.0f, 0.0f, 2000.0f);
                        dr2PopupUntil = millis() + DR2_POPUP_MS;
                    }
                    if (fabsf(pots[5].rawDelta) > 0.0001f) {
                        // 0-2x headroom (was capped at unity gain) — the drum engine reads
                        // quieter than the 303 at matched settings, so give it room to go
                        // louder; matches the 0-2x convention audioPlayKey's sample volume
                        // already uses elsewhere.
                        drum2Volume[dr2SelPad] = constrain(drum2Volume[dr2SelPad] + pots[5].rawDelta*2.0f, 0.0f, 2.0f);
                        dr2PopupUntil = millis() + DR2_POPUP_MS;
                    }
                    break;
                }
                case MODE_MOD2: {
                    // P2 = algo selection (discrete, 8 steps)
                    static float lp2_algo=-1.0f;
                    if(mod2PotNeedsSync){ lp2_algo=pots[1].value; mod2PotNeedsSync=false; }
                    if(fabsf(pots[1].value-lp2_algo)>0.005f){
                        uint8_t na=(uint8_t)(pots[1].value*((float)MOD2_ALGO_COUNT-0.01f));
                        if(na!=mod2AlgoIdx){ mod2AlgoIdx=na; mod2AlgoApply(); }
                        lp2_algo=pots[1].value;
                    }
                    // P4-P7 = algo-specific parameters
                    bool changed=false;
                    for(int i=0;i<4;i++){
                        float pv=pots[3+i].value;
                        if(fabsf(pv-mod2PCache[i])>0.003f){
                            mod2P[i]=pv; mod2PCache[i]=pv; changed=true;
                        }
                    }
                    if(changed) mod2ApplyP4P7();
                    break;
                }
                case MODE_STONE: {
                    // FX overlay open: pots 4-7 belong to the active FX's own params instead
                    // of the sample window — same established pattern every other synth-like
                    // mode already uses (see case MODE_303S above). Was previously entirely
                    // missing for STONE, so FX params were unreachable via pots once an FX
                    // was toggled on — the OLED already showed them (GRANULAR2-modeled
                    // footer), just nothing moved them.
                    if (s_overlay == OVERLAY_FX) {
                        if (fxList[fxSelected].active) {
                            static float lp_stoneFx[4] = {-1,-1,-1,-1};
                            static uint8_t lpStoneFxSel = 0xFF;
                            if (lpStoneFxSel != fxSelected || fxPotNeedSync) {
                                for (int p = 0; p < 4; p++) lp_stoneFx[p] = pots[3+p].value;
                                lpStoneFxSel = fxSelected; fxPotNeedSync = false;
                            }
                            bool fxChanged = false;
                            for (int p = 0; p < 4; p++) {
                                if (fxList[fxSelected].paramNames[p][0] == '\0') continue;
                                if (fabsf(pots[3+p].value - lp_stoneFx[p]) > 0.001f) {
                                    float mn = fxList[fxSelected].paramMin[p];
                                    float mx = fxList[fxSelected].paramMax[p];
                                    if (fxSelected==0 && p==0)
                                        fxList[0].params[0] = mn*powf(mx/mn, pots[3].value);
                                    else if (fxSelected==0 && p==3) {
                                        fxList[0].params[3] = floorf(pots[3+p].value*3.9999f);
                                        s_filtMetaChanged = true;
                                    } else {
                                        fxList[fxSelected].params[p] = mn+(mx-mn)*pots[3+p].value;
                                        if (fxSelected==0) s_filtMetaChanged = true;
                                    }
                                    lp_stoneFx[p] = pots[3+p].value;
                                    fxChanged = true;
                                }
                            }
                            if (fxChanged && fxSelected!=0) applyFxEffect(fxSelected);
                            else if (fxChanged && fxSelected==0 && fxList[0].active && s_filtMetaChanged) { applyFxEffect(0); s_filtMetaChanged=false; }
                        }
                        break;
                    }
                    // P2: auto-cycle through the audio files in the currently browsed folder
                    if (stoneAudioCount > 0 && fabsf(pots[1].value-lp_stoneP2)>0.01f) {
                        uint8_t idx = (uint8_t)(pots[1].value * ((float)stoneAudioCount - 0.01f));
                        uint8_t fi = stoneAudioIdx[idx];
                        if (fi < sdFileCount) {
                            sdCursor = fi;
                            String fp = sdPath; if(!fp.endsWith("/")) fp += "/"; fp += sdFiles[fi];
                            if (fp != stoneLoadedPath) {
                                stoneLoadedPath = fp;
                                audioLoadStone(fp.c_str());
                                stoneWin = StoneWinState{};  // new sample: full-range window, waveform recomputed on ready
                            }
                        }
                        lp_stoneP2 = pots[1].value;
                    }
                    // Waveform is only computable once the background load finishes.
                    if (audioIsStoneReady() && !stoneWin.computed) {
                        audioComputeStoneWaveform(stoneWin.waveform);
                        stoneWin.computed = true;
                        audioStoneApplyWindow(stoneWin.start, stoneWin.end, stoneLoopMode);
                    }
                    // P4-P7 = start/end/shift/zoom, applied as unclamped raw deltas straight onto
                    // stoneWin's own start/end — same corrected model as GEST2's per-pad pot control
                    // (no baseline/pickup bookkeeping needed: STONE has only one sample/one window,
                    // ever, so there's no "switching target" scenario for a pot to lose sync over).
                    bool winChanged = false;
                    if (fabsf(pots[3].rawDelta) > 0.0005f) {                      // P4 = start
                        stoneWin.start = constrain(stoneWin.start + pots[3].rawDelta, 0.0f, stoneWin.end - 0.01f);
                        winChanged = true;
                    }
                    if (fabsf(pots[4].rawDelta) > 0.0005f) {                      // P5 = end
                        stoneWin.end = constrain(stoneWin.end + pots[4].rawDelta, stoneWin.start + 0.01f, 1.0f);
                        winChanged = true;
                    }
                    if (fabsf(pots[5].rawDelta) > 0.0005f) {                      // P6 = shift/position
                        float ww = stoneWin.end - stoneWin.start;
                        float ns = constrain(stoneWin.start + pots[5].rawDelta, 0.0f, 1.0f - ww);
                        stoneWin.start = ns; stoneWin.end = ns + ww;
                        winChanged = true;
                    }
                    if (fabsf(pots[6].rawDelta) > 0.0005f) {                      // P7 = zoom/size
                        float center = (stoneWin.start + stoneWin.end) * 0.5f;
                        float halfW = constrain((stoneWin.end - stoneWin.start) * 0.5f + pots[6].rawDelta * 0.5f, 0.005f, 0.5f);
                        stoneWin.start = constrain(center - halfW, 0.0f, 1.0f);
                        stoneWin.end   = constrain(center + halfW, 0.0f, 1.0f);
                        winChanged = true;
                    }
                    if (winChanged) {
                        audioStoneApplyWindow(stoneWin.start, stoneWin.end, stoneLoopMode);
                    }
                    break;
                }
                case MODE_MODULAR: {
                    // P2=OscA wavetable (B3 cycles OscB's independently)  P4=OscA morph pos
                    // P5=OscB morph pos  P6=filter cutoff  P7=LFO depth (wobbles OscB's morph
                    // position — fixed destination for this MVP; see the 10ms tick below for
                    // the actual modulation, which reuses gModSlots[4], the slot reserved for
                    // the modular synth's LFO source in the Section-1 mod engine).
                    static float lpm[5]={-1,-1,-1,-1,-1};
                    if(fabsf(pots[1].value-lpm[0])>0.01f){
                        uint8_t nt=(uint8_t)(pots[1].value*4.99f);
                        if(nt!=modOscTable){
                            modOscTable=nt;
                            audioModularSetTable(MOD3_OSCA_CH, modOscTable);
                        }
                        lpm[0]=pots[1].value;
                    }
                    if(fabsf(pots[3].value-lpm[1])>0.002f){
                        modOscAPos=pots[3].value;
                        audioModularSetWtPos(MOD3_OSCA_CH, modOscAPos);
                        lpm[1]=pots[3].value;
                    }
                    if(fabsf(pots[4].value-lpm[2])>0.002f){
                        modOscBPos=pots[4].value;
                        lpm[2]=pots[4].value;
                    }
                    if(fabsf(pots[5].value-lpm[3])>0.002f){
                        modCutoff=200.0f*powf(90.0f, pots[5].value);
                        audioModularSetFilter(modCutoff, modReso);
                        lpm[3]=pots[5].value;
                    }
                    if(fabsf(pots[6].value-lpm[4])>0.002f){
                        modLfoDepth=pots[6].value;
                        gModSlots[4].depth = modLfoDepth;
                        lpm[4]=pots[6].value;
                    }
                    break;
                }
                case MODE_MIDI: {
#if CONFIG_TINYUSB_MIDI_ENABLED
                    if (!midiActive) break;
                    for (int p = 0; p < 7; p++) {
                        if (midiPotCC[p] > 127) continue;  // 0xFF = disabled
                        if (fabsf(pots[p].value - midiLpPots[p]) > 0.005f) {
                            midiCC(midiPotCC[p], (uint8_t)(pots[p].value * 127.f), midiChannel);
                            midiLpPots[p] = pots[p].value;
                        }
                    }
#endif
                    break;
                }
                case MODE_EXP: {
                    static float lp1ex=-1, lp3ex=-1, lp4ex=-1, lp5ex=-1, lp6ex=-1;
                    // Pot 1: shape (same as SYNTH)
                    if (fabsf(pots[1].value-lp1ex)>0.01f) {
                        lp1ex=pots[1].value;
                        uint8_t si=(uint8_t)(pots[1].value*((uint8_t)SHAPE_COUNT-0.01f));
                        audioSetShape((SynthShape)si);
                    }
                    // Pot 3: filter cutoff (200Hz–8kHz exp) — texture
                    if (fabsf(pots[3].value-lp3ex)>0.005f) {
                        lp3ex=pots[3].value;
                        float cutoff = 200.0f * powf(40.0f, pots[3].value);  // 200..8000 Hz
                        audioSetFilter(cutoff, 2.0f);
                    }
                    // Pot 4: resonance 0.5–4.0
                    if (fabsf(pots[4].value-lp4ex)>0.005f) {
                        lp4ex=pots[4].value;
                        float cutoff = 200.0f * powf(40.0f, lp3ex >= 0.0f ? lp3ex : 0.0f);
                        audioSetFilter(cutoff, 0.5f + pots[4].value * 3.5f);
                    }
                    // Pot 5: wavefold depth 1.0–6.0
                    if (fabsf(pots[5].value-lp5ex)>0.005f) {
                        lp5ex=pots[5].value;
                        audioSetWavefold(1.0f + pots[5].value * 5.0f);
                    }
                    // Pot 6: reverb level
                    if (fabsf(pots[6].value-lp6ex)>0.008f) {
                        lp6ex=pots[6].value;
                        audioSetReverb(pots[6].value*0.85f, 0.78f, 0.45f, 2000.0f);
                    }
                    break;
                }
                case MODE_EXP2: {
                    static float lp1e2=-1, lp3e2=-1, lp4e2=-1, lp5e2=-1, lp6e2=-1;
                    // Pot 2 (pots[1]): shape (same as SYNTH) — also sets shape directly
                    if (fabsf(pots[1].value-lp1e2)>0.01f) {
                        lp1e2=pots[1].value;
                        uint8_t si=(uint8_t)(pots[1].value*((uint8_t)SHAPE_COUNT-0.01f));
                        audioSetShape((SynthShape)si);
                    }
                    // Pot 4 (pots[3]): polygon rotation speed — center=stop, left=reverse, right=forward
                    if (fabsf(pots[3].value-lp3e2)>0.005f) {
                        lp3e2=pots[3].value;
                        exp2RotVel = (pots[3].value - 0.5f) * 0.040f;  // -0.020..+0.020 rad/tick
                    }
                    // Pot 5 (pots[4]): ball speed cap
                    if (fabsf(pots[4].value-lp4e2)>0.005f) {
                        lp4e2=pots[4].value;
                        exp2SpeedCap = 1.5f + pots[4].value * 8.5f;
                    }
                    // Pot 6 (pots[5]): gravity strength
                    if (fabsf(pots[5].value-lp5e2)>0.005f) {
                        lp5e2=pots[5].value;
                        exp2GravStr = pots[5].value * 0.22f;
                    }
                    // Pot 7 (pots[6]): bounciness
                    if (fabsf(pots[6].value-lp6e2)>0.008f) {
                        lp6e2=pots[6].value;
                        exp2Bounce = 0.50f + pots[6].value * 0.50f;
                    }
                    break;
                }
                case MODE_EXP3: {
                    static float lp1e3=-1, lp3e3=-1, lp4e3=-1, lp5e3=-1, lp6e3=-1;
                    // Pot 1: shape (same as SYNTH)
                    if (fabsf(pots[1].value-lp1e3)>0.01f) {
                        lp1e3=pots[1].value;
                        uint8_t si=(uint8_t)(pots[1].value*((uint8_t)SHAPE_COUNT-0.01f));
                        if (audioReady) audioSetShape((SynthShape)si);
                    }
                    // Pot 3: scale selection (4 zones across full range)
                    if (fabsf(pots[3].value-lp3e3)>0.01f) {
                        lp3e3=pots[3].value;
                        exp3Scale = (uint8_t)(pots[3].value * 3.99f);
                    }
                    // Pot 4: reverb
                    if (fabsf(pots[4].value-lp4e3)>0.01f) {
                        lp4e3=pots[4].value;
                        if (audioReady) audioSetReverb(pots[4].value*0.85f, 0.78f, 0.45f, 2000.0f);
                    }
                    // Pot 6 (pots[5]): filter cutoff — was dead, EXP3 had no timbral shaping
                    // at all beyond shape/scale/reverb until this fix.
                    if (fabsf(pots[5].value-lp5e3)>0.005f) {
                        lp5e3=pots[5].value;
                        if (audioReady) audioSetFilter(200.0f*powf(40.0f,pots[5].value), 1.2f);
                    }
                    // Pot 7 (pots[6]): envelope select — also dead, EXP3 had no envelope
                    // control at all (unlike EXP/EXP2, which both expose one via key grid).
                    if (fabsf(pots[6].value-lp6e3)>0.01f) {
                        lp6e3=pots[6].value;
                        static const EnvPreset kExp3Envs[] = {ENV_FAST, ENV_NORMAL, ENV_PAD, ENV_PLUCK};
                        uint8_t ei=(uint8_t)(pots[6].value*3.99f);
                        if (audioReady) audioSetEnvelope(envTable[(uint8_t)kExp3Envs[ei]]);
                    }
                    break;
                }
                case MODE_LIFE: {
                    static float lp1L=-1, lp4L=-1, lp5L=-1, lp6L=-1, lp7L=-1;
                    // Pot 2 (P2): instrument/algorithm — cycles through the same shape set
                    // SYNTH's OVERLAY_INSTR offers, shown by name in the OLED header.
                    if (fabsf(pots[1].value-lp1L)>0.01f) {
                        lp1L=pots[1].value;
                        uint8_t si=(uint8_t)(pots[1].value*((uint8_t)SHAPE_COUNT-0.01f));
                        lifeShape = (SynthShape)si;
                        if (audioReady) audioSetShape(lifeShape);
                    }
                    // Pot 4: rule variant (classic B3/S23 vs HighLife B36/S23)
                    if (fabsf(pots[3].value-lp4L)>0.05f) {
                        lp4L=pots[3].value;
                        lifeRule = (pots[3].value > 0.5f) ? 1 : 0;
                    }
                    // Pot 5: tick rate (BPM subdivision)
                    if (fabsf(pots[4].value-lp5L)>0.02f) {
                        lp5L=pots[4].value;
                        lifeTickDivIdx = (uint8_t)(pots[4].value * (DELAY_SUBDIV_COUNT-0.01f));
                    }
                    // Pot 6: density reseed — re-randomizes the grid at this density on a
                    // meaningful pot move, not continuously (a live reseed every tick would
                    // just be noise, not a usable "seed the automaton" gesture).
                    if (fabsf(pots[5].value-lp6L)>0.08f) {
                        lp6L=pots[5].value;
                        lifeReseed(0.15f + pots[5].value*0.5f);
                    }
                    // Pot 7: envelope/decay style
                    if (fabsf(pots[6].value-lp7L)>0.01f) {
                        lp7L=pots[6].value;
                        static const EnvPreset kLifeEnvs[] = {ENV_FAST, ENV_PLUCK, ENV_NORMAL, ENV_PAD};
                        uint8_t ei=(uint8_t)(pots[6].value*3.99f);
                        lifeEnv = kLifeEnvs[ei];
                        if (audioReady) audioSetEnvelope(envTable[(uint8_t)lifeEnv]);
                    }
                    break;
                }
                case MODE_SWARM: {
                    static float lp1S=-1, lp4S=-1, lp5S=-1, lp6S=-1, lp7S=-1;
                    // Pot 1: shape
                    if (fabsf(pots[1].value-lp1S)>0.01f) {
                        lp1S=pots[1].value;
                        uint8_t si=(uint8_t)(pots[1].value*((uint8_t)SHAPE_COUNT-0.01f));
                        if (audioReady) audioSetShape((SynthShape)si);
                    }
                    // Pot 4: flock tightness (0=max separation/spread out, 1=max cohesion/tight)
                    if (fabsf(pots[3].value-lp4S)>0.01f) {
                        lp4S=pots[3].value; swarmFlockBal = pots[3].value;
                    }
                    // Pot 5: flock speed cap
                    if (fabsf(pots[4].value-lp5S)>0.01f) {
                        lp5S=pots[4].value; swarmSpeedCap = 1.0f + pots[4].value*8.0f;
                    }
                    // Pot 6: attractor pull strength
                    if (fabsf(pots[5].value-lp6S)>0.01f) {
                        lp6S=pots[5].value; swarmAttractStr = pots[5].value*0.4f;
                    }
                    // Pot 7: trigger-zone radius
                    if (fabsf(pots[6].value-lp7S)>0.01f) {
                        lp7S=pots[6].value; swarmZoneRadius = 4.0f + pots[6].value*16.0f;
                    }
                    break;
                }
                case MODE_GEN: {
                    static float lp1G=-1, lp4G=-1, lp5G=-1, lp6G=-1, lp7G=-1;
                    // Pot 2: primary tuning knob for whichever generator (texture) is
                    // currently selected — WALK=wander amount, EUCL=pulse count, DRIFT=chaos.
                    if (fabsf(pots[1].value-lp1G)>0.01f) {
                        lp1G=pots[1].value;
                        switch (genGenerator % GEN_GENERATOR_COUNT) {
                            case 0: genWalkWander = pots[1].value; break;
                            case 1: genEuclPulses = (uint8_t)constrain((int)(pots[1].value*7.99f)+1, 1, 8); break;
                            case 2: genDriftR = 3.5f + pots[1].value*0.49f; break;
                        }
                    }
                    // Pot 4: generator tick rate (BPM subdivision)
                    if (fabsf(pots[3].value-lp4G)>0.02f) {
                        lp4G=pots[3].value;
                        genTickDivIdx = (uint8_t)(pots[3].value * (DELAY_SUBDIV_COUNT-0.01f));
                    }
                    // Pot 5: scale
                    if (fabsf(pots[4].value-lp5G)>0.05f) {
                        lp5G=pots[4].value;
                        genScale = (uint8_t)(pots[4].value * 3.99f);
                    }
                    // Pot 6: octave (register the generator plays in)
                    if (fabsf(pots[5].value-lp6G)>0.05f) {
                        lp6G=pots[5].value;
                        genOctave = (uint8_t)(1.0f + pots[5].value*5.99f); // 1..6
                    }
                    // Pot 7: voice brightness/cutoff — the one tunable knob for whichever
                    // sound-making method (voice) is currently selected.
                    if (fabsf(pots[6].value-lp7G)>0.01f) {
                        lp7G=pots[6].value;
                        genVoiceParam = pots[6].value;
                        if (audioReady) {
                            float cutoff = 300.0f * powf(40.0f, genVoiceParam);
                            audioSetFilter(cutoff, 1.4f);
                        }
                    }
                    break;
                }
                default: break;
            }
        }

        // TREMOLO (10ms tick) — volume LFO, software 10ms tick modulating the volume
        // pot's own output.
        if(fxList[10].active&&audioReady){
            static float tremPhase=0.0f;
            static bool  tremWasActive=false;
            float rate  = fxList[10].params[0];
            float depth = fxList[10].params[1];
            tremPhase += rate * 2.0f * (float)M_PI * 0.01f;
            if(tremPhase > 2.0f*(float)M_PI) tremPhase -= 2.0f*(float)M_PI;
            if(depth > 0.005f){
                tremWasActive = true;
                float tremMod = (1.0f + sinf(tremPhase)) * 0.5f;
                audioSetVolume(volume * (1.0f - depth * tremMod));
            } else if(tremWasActive){
                tremWasActive = false;
                audioSetVolume(volume);  // restore plain pot volume
            }
        }

        // AUTOPAN (10ms tick) — stereo pan LFO, same shape as TREMOLO but driving pan.
        if(fxList[11].active&&audioReady){
            static float panPhase=0.0f;
            static bool  panWasActive=false;
            float rate  = fxList[11].params[0];
            float depth = fxList[11].params[1];
            panPhase += rate * 2.0f * (float)M_PI * 0.01f;
            if(panPhase > 2.0f*(float)M_PI) panPhase -= 2.0f*(float)M_PI;
            if(depth > 0.005f){
                panWasActive = true;
                float pan = 0.5f + 0.5f * depth * sinf(panPhase);
                audioSetPan(pan);
            } else if(panWasActive){
                panWasActive = false;
                audioSetPan(0.5f);  // recenter
            }
        }

        // Generic modulation-slot engine (FX-param automation, Section 2 double-click UI;
        // also drives the modular synth's LFO source, Section 5) — see modSlotsTick10ms().
        if (audioReady) modSlotsTick10ms();

        // Smooth FILT cutoff application — anti-zipper when turning the cutoff encoder.
        // Use audioSetFilterFreq (no filter_type field) to avoid AMY biquad state resets
        // that cause an audible pop/click on each value change.
        //
        // Skipped while a mod slot is automating FILT's cutoff: modSlotApply() (called by
        // modSlotsTick10ms() just above) writes the modulated value, calls applyFxEffect(0)
        // to push it to AMY, then immediately restores fxList[0].params[0] back to the
        // static pot-set base — so by the time THIS block ran right after, it always read
        // that static base (never the modulated value) and smoothly converged lpfSmoothCut
        // toward it, calling audioSetFilterFreq() and overwriting the very cutoff automation
        // had just applied moments earlier. Net effect: the automated sweep was cancelled
        // out within the same 10ms tick it was applied in — the cutoff value genuinely
        // oscillated (visible in the FX0 FILT log line) but the audio never reflected it.
        // While automation owns this param, its own per-tick apply is already the "current"
        // value — the anti-zipper's job (smoothing a human turning a pot) doesn't apply.
        if (fxList[0].active && audioReady) {
            int8_t cutModSlot = modSlotFindFxParam(0, 0);
            bool cutAutomated = (cutModSlot >= 0 && gModSlots[cutModSlot].active);
            if (!cutAutomated) {
                // Converge raw cutoff (anti-zipper)
                float prevCut = lpfSmoothCut;
                lpfSmoothCut += (fxList[0].params[0] - lpfSmoothCut) * 0.5f;
                float   effCut = lpfSmoothCut;
                float   effRes = fxList[0].params[1];
                audioSetFilterFreq(effCut, effRes);
                if (fabsf(lpfSmoothCut - prevCut) > 1.0f)
                    audioSetGranular2FilterFreq(effCut, effRes);
            } else {
                // Keep in sync with the static base so turning automation back off resumes
                // smoothing from a sane starting point instead of an old, stale value.
                lpfSmoothCut = fxList[0].params[0];
            }
        }

        // GR2 SEQ: advance head pointer when current slice's wall-clock deadline passes.
        // The audio event itself is already scheduled in AMY (g_gran2SeqNextAmy), so advancing
        // the head just keeps the queue state in sync and pre-schedules the next-next item.
        if (currentMode==MODE_GRANULAR2 && gran2PlayMode==3 && audioReady) {
            if (g_gran2SeqHead != g_gran2SeqTail && (int32_t)(millis() - g_gran2SeqEndMs) >= 0) {
                g_gran2SeqHead = (g_gran2SeqHead + 1) % GRAN2_SEQ_MAX;
                // If there's another item waiting, pre-schedule it now so its AMY event is
                // queued before the current slice finishes (look-ahead scheduling).
                if (g_gran2SeqHead != g_gran2SeqTail) gran2SeqStartHead();
            }
        }

        if (currentMode==MODE_GRANULAR2 && gran2PlayMode==4 && audioReady) {
            if (g_gran2SqlCount > 0 && (int32_t)(millis() - g_gran2SqlEndMs) >= 0) {
                g_gran2SqlPlayHead = (g_gran2SqlPlayHead + 1) % g_gran2SqlCount;
                gran2SqlStartHead();
            }
        }

        // VID: advance to next frame when due
        if (currentMode==MODE_VID && vidPlaying && vidFileOpen) {
            uint32_t now10 = millis();
            uint32_t frameMs = 1000u / vidFps;
            if (now10 - vidLastFrameMs >= frameMs) {
                vidLastFrameMs = now10;
                int n = vidFile.read(vidFrameBuf, sizeof(vidFrameBuf));
                if (n < (int)sizeof(vidFrameBuf)) {
                    // End of file — loop back to first frame
                    vidFile.seek(sizeof(BvidHeader));
                    vidCurrentFrame = 0;
                    vidFile.read(vidFrameBuf, sizeof(vidFrameBuf));
                }
                vidCurrentFrame++;
            }
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
                float cut = mod2CurrentCutoff * (1.0f - lfoVal * mod2LfoDepth * 0.2f);
                cut = constrain(cut, 80.0f, 8000.0f);
                audioSetFilter(cut, mod2CurrentReso);
            }
        }

        // MODULAR: LFO vibrato only — an internal modulation source, not joystick-driven
        // (the joystick has no performance role in this mode; see the MODE_MODULAR
        // joystick-nav block below for what JX/JY actually do: modify the focused
        // element's parameters). audioModularSetPitchBend() itself was fixed separately:
        // the generic audioSetPitchBend() only ever reaches SYNTH_CH, never this mode's
        // MOD3_OSCA_CH/MOD3_OSCB_CH.
        if(currentMode==MODE_MODULAR && !menuOpen && audioReady){
            modLfoPhase += modLfoRate * 2.0f * (float)M_PI * 0.01f;
            if(modLfoPhase > 2.0f*(float)M_PI) modLfoPhase -= 2.0f*(float)M_PI;
            float vibrato = (modLfoDepth > 0.01f) ? sinf(modLfoPhase) * modLfoDepth : 0.0f;
            audioModularSetPitchBend(powf(2.0f, vibrato / 12.0f));
        }

        // 303 portamento slide: interpolate pitch_bend from t303SlideFrom to t303SlideTo
        if(currentMode==MODE_303S && t303SlideActive && audioReady){
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

        // I303 joystick: X=wave cycle, Y=octave (same as 303S)
        if (currentMode==MODE_I303 && !menuOpen) {
            static unsigned long i303jWvMs=0, i303jOctMs=0;
            static bool i303jWvArm=false, i303jOctArm=false;
            float jx=cachedJoyX/64.0f, jy=cachedJoyY/64.0f;
            unsigned long now=millis();
            bool jxH=(fabsf(jx)>0.45f), jyH=(fabsf(jy)>0.45f);
            if(!jxH) i303jWvArm=false;
            if(!jyH) i303jOctArm=false;
            if(jxH && (!i303jWvArm || now-i303jWvMs>=150)){
                int8_t dir=(jx>0)?1:-1;
                static const uint8_t kI303Waves[]={TRIANGLE, T303_TRI2_WAVE, SAW_DOWN, T303_SAW2_WAVE, PULSE, T303_SAW3_WAVE, T303_SQ2_WAVE, T303_NAP_WAVE, T303_SWF_WAVE, T303_SQF_WAVE, T303_SNF_WAVE, T303_PINK_WAVE};
                static const uint8_t kI303WaveCnt=12;
                uint8_t ci=0;
                for(uint8_t i=0;i<kI303WaveCnt;i++) if(kI303Waves[i]==t303Wave){ci=i;break;}
                uint8_t prevWave=t303Wave;
                ci=(uint8_t)((ci+kI303WaveCnt+dir)%kI303WaveCnt);
                t303Wave=kI303Waves[ci];
                if(audioReady){
                    if(t303IsSubOctWave(prevWave) && !t303IsSubOctWave(t303Wave)) audioSW2Deactivate();
                    audioT303Wave(t303AmyWave(t303Wave));
                    audioT303Feedback(0.0f);
                    float curP2=pots[1].value;
                    if(t303IsSubOctWave(t303Wave)){
                        audioSW2Init(t303Cutoff, t303Reso, t303Decay, 6, t303AmyWave(t303Wave));
                        audioSW2SetBlend(curP2);
                    } else if(t303Wave==T303_PINK_WAVE){
                        audioT303Wavefold(0.0f);
                    } else {
                        if(t303Wave==T303_TRI2_WAVE||t303Wave==T303_SAW2_WAVE) audioT303WavefoldAsym(curP2);
                        else if(t303Wave==PULSE) audioT303Wavefold(0.0f);
                        else audioT303Wavefold(curP2);
                        if(t303Wave==PULSE)          audioT303Duty(0.5f-curP2*0.48f);
                        if(t303Wave==T303_SAW3_WAVE) audioT303Duty(0.5f);
                        if(t303Wave==T303_NAP_WAVE)  audioT303Duty(0.02f);
                    }
                }
                i303jWvMs=now; i303jWvArm=true;
            }
            if(jyH && (!i303jOctArm || now-i303jOctMs>=300)){
                int8_t dir=(jy>0)?-1:1;
                t303Oct=(int8_t)constrain(t303Oct+dir,-3,3);
                i303jOctMs=now; i303jOctArm=true;
            }
        }

        // GEST2 (MODE_DR2) 303 slot joystick: X=wave, Y=octave — same shared
        // t303Wave/t303Oct state and debounce pattern as MODE_303S below, so the
        // 303 slot has full parity with MODE_303S's own controls. Any touch here
        // arms the OLED popup (dr2PopupUntil) that shows wave/params in place
        // of the big beat.step readout for a couple seconds.
        if (currentMode==MODE_DR2 && dr2Instrument==DR2_INSTR_T303 && !menuOpen) {
            static unsigned long dr2jWvMs=0, dr2jOctMs=0;
            static bool dr2jWvArm=false, dr2jOctArm=false;
            float jx=cachedJoyX/64.0f, jy=cachedJoyY/64.0f;
            unsigned long now=millis();
            bool jxH=(fabsf(jx)>0.45f), jyH=(fabsf(jy)>0.45f);
            if(!jxH) dr2jWvArm=false;
            if(!jyH) dr2jOctArm=false;
            if(jxH && (!dr2jWvArm || now-dr2jWvMs>=150)){
                int8_t dir=(jx>0)?1:-1;
                static const uint8_t kT303Waves[]={TRIANGLE, T303_TRI2_WAVE, SAW_DOWN, T303_SAW2_WAVE, PULSE, T303_SAW3_WAVE, T303_SQ2_WAVE, T303_NAP_WAVE, T303_SWF_WAVE, T303_SQF_WAVE, T303_SNF_WAVE, T303_PINK_WAVE};
                static const uint8_t kT303WaveCnt=12;
                uint8_t ci=0;
                for(uint8_t i=0;i<kT303WaveCnt;i++) if(kT303Waves[i]==t303Wave){ci=i;break;}
                uint8_t prevWave=t303Wave;
                ci=(uint8_t)((ci+kT303WaveCnt+dir)%kT303WaveCnt);
                t303Wave=kT303Waves[ci];
                if(!dr2T303Inited){ audioT303Init(2000.0f,1.5f,2.0f,200.0f,0); dr2T303Inited=true; }
                if(audioReady){
                    if(t303IsSubOctWave(prevWave) && !t303IsSubOctWave(t303Wave)) audioSW2Deactivate();
                    audioT303Wave(t303AmyWave(t303Wave));
                    audioT303Feedback(0.0f);
                    float curP2=pots[1].value;
                    if(t303IsSubOctWave(t303Wave)){
                        audioSW2Init(t303Cutoff, t303Reso, t303Decay, 1, t303AmyWave(t303Wave));
                        audioSW2SetBlend(curP2);
                    } else if(t303Wave==T303_PINK_WAVE){
                        audioT303Wavefold(0.0f);
                    } else {
                        if(t303Wave==T303_TRI2_WAVE||t303Wave==T303_SAW2_WAVE)
                            audioT303WavefoldAsym(curP2);
                        else if(t303Wave==PULSE)
                            audioT303Wavefold(0.0f);
                        else
                            audioT303Wavefold(curP2);
                        if(t303Wave==PULSE)          audioT303Duty(0.5f-curP2*0.48f);
                        if(t303Wave==T303_SAW3_WAVE) audioT303Duty(0.5f);
                        if(t303Wave==T303_NAP_WAVE)  audioT303Duty(0.02f);
                    }
                }
                dr2jWvMs=now; dr2jWvArm=true;
                dr2PopupUntil = now + DR2_POPUP_MS;
            }
            if(jyH && (!dr2jOctArm || now-dr2jOctMs>=300)){
                int8_t dir=(jy>0)?-1:1;
                t303Oct=(int8_t)constrain(t303Oct+dir,-3,3);
                dr2jOctMs=now; dr2jOctArm=true;
                dr2PopupUntil = now + DR2_POPUP_MS;
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
                static const uint8_t kT303Waves[]={TRIANGLE, T303_TRI2_WAVE, SAW_DOWN, T303_SAW2_WAVE, PULSE, T303_SAW3_WAVE, T303_SQ2_WAVE, T303_NAP_WAVE, T303_SWF_WAVE, T303_SQF_WAVE, T303_SNF_WAVE, T303_PINK_WAVE};
                static const uint8_t kT303WaveCnt=12;
                uint8_t ci=0;
                for(uint8_t i=0;i<kT303WaveCnt;i++) if(kT303Waves[i]==t303Wave){ci=i;break;}
                uint8_t prevWave=t303Wave;
                ci=(uint8_t)((ci+kT303WaveCnt+dir)%kT303WaveCnt);
                t303Wave=kT303Waves[ci];
                if(audioReady){
                    if(t303IsSubOctWave(prevWave) && !t303IsSubOctWave(t303Wave)) audioSW2Deactivate();
                    audioT303Wave(t303AmyWave(t303Wave));
                    audioT303Feedback(0.0f);
                    float curP2=pots[1].value;
                    if(t303IsSubOctWave(t303Wave)){
                        audioSW2Init(t303Cutoff, t303Reso, t303Decay, 1, t303AmyWave(t303Wave));
                        audioSW2SetBlend(curP2);
                    } else if(t303Wave==T303_PINK_WAVE){
                        audioT303Wavefold(0.0f);
                    } else {
                        if(t303Wave==T303_TRI2_WAVE||t303Wave==T303_SAW2_WAVE)
                            audioT303WavefoldAsym(curP2);
                        else if(t303Wave==PULSE)
                            audioT303Wavefold(0.0f);
                        else
                            audioT303Wavefold(curP2);
                        if(t303Wave==PULSE)          audioT303Duty(0.5f-curP2*0.48f);
                        if(t303Wave==T303_SAW3_WAVE) audioT303Duty(0.5f);
                        if(t303Wave==T303_NAP_WAVE)  audioT303Duty(0.02f);
                    }
                }
                s303jWvMs=now; s303jWvArm=true;
            }
            if(jyH && (!s303jOctArm || now-s303jOctMs>=300)){
                int8_t dir=(jy>0)?-1:1;
                t303Oct=(int8_t)constrain(t303Oct+dir,-3,3);
                s303jOctMs=now; s303jOctArm=true;
            }
        }

        // 303S2 joystick: JX=wave cycle, JY=octave (same as 303S)
        if (currentMode==MODE_303S2 && !menuOpen) {
            static unsigned long s303s2jWvMs=0, s303s2jOctMs=0;
            static bool s303s2jWvArm=false, s303s2jOctArm=false;
            float jx2=cachedJoyX/64.0f, jy2=cachedJoyY/64.0f;
            unsigned long nowJ=millis();
            bool jxH2=(fabsf(jx2)>0.45f), jyH2=(fabsf(jy2)>0.45f);
            if(!jxH2) s303s2jWvArm=false;
            if(!jyH2) s303s2jOctArm=false;
            if(jxH2 && (!s303s2jWvArm || nowJ-s303s2jWvMs>=150)){
                int8_t dir2=(jx2>0)?1:-1;
                static const uint8_t kT303S2Waves[]={TRIANGLE,T303_TRI2_WAVE,SAW_DOWN,T303_SAW2_WAVE,PULSE,T303_SAW3_WAVE,T303_SQ2_WAVE,T303_NAP_WAVE,T303_SWF_WAVE,T303_SQF_WAVE,T303_SNF_WAVE,T303_PINK_WAVE};
                static const uint8_t kT303S2WaveCnt=12;
                uint8_t ci2=0;
                for(uint8_t i=0;i<kT303S2WaveCnt;i++) if(kT303S2Waves[i]==t303Wave){ci2=i;break;}
                uint8_t prevWave2=t303Wave;
                ci2=(uint8_t)((ci2+kT303S2WaveCnt+dir2)%kT303S2WaveCnt);
                t303Wave=kT303S2Waves[ci2];
                if(audioReady){
                    if(t303IsSubOctWave(prevWave2)&&!t303IsSubOctWave(t303Wave)) audioSW2Deactivate();
                    audioT303Wave(t303AmyWave(t303Wave)); audioT303Feedback(0.0f);
                    float curP2b=pots[1].value;
                    if(t303IsSubOctWave(t303Wave)){ audioSW2Init(t303Cutoff,t303Reso,t303Decay,1,t303AmyWave(t303Wave)); audioSW2SetBlend(curP2b); }
                    else if(t303Wave==T303_PINK_WAVE){ audioT303Wavefold(0.0f); }
                    else {
                        if(t303Wave==T303_TRI2_WAVE||t303Wave==T303_SAW2_WAVE) audioT303WavefoldAsym(curP2b);
                        else if(t303Wave==PULSE) audioT303Wavefold(0.0f);
                        else audioT303Wavefold(curP2b);
                        if(t303Wave==PULSE)          audioT303Duty(0.5f-curP2b*0.48f);
                        if(t303Wave==T303_SAW3_WAVE) audioT303Duty(0.5f);
                        if(t303Wave==T303_NAP_WAVE)  audioT303Duty(0.02f);
                    }
                }
                s303s2jWvMs=nowJ; s303s2jWvArm=true;
            }
            if(jyH2 && (!s303s2jOctArm || nowJ-s303s2jOctMs>=300)){
                int8_t dir2=(jy2>0)?-1:1;
                t303Oct=(int8_t)constrain(t303Oct+dir2,-3,3);
                s303s2jOctMs=nowJ; s303s2jOctArm=true;
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
                midiPitchBend(pb, midiChannel);
                midiLastPB = (float)pb;
            }
            // Modulation: X axis, full range — left(-1)=0, center=64, right(+1)=127
            uint8_t mod = (uint8_t)constrain((int)((jx + 1.0f) * 63.5f), 0, 127);
            if (mod != midiLastMod) {
                midiCC(1, mod, midiChannel);
                midiLastMod = mod;
            }
        }
#endif

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
                            else if (fxSelected == 0 && p == 3) {
                                fxList[0].params[3] = floorf(pots[3+p].value * 3.9999f);
                                s_filtMetaChanged = true;
                            } else {
                                fxList[fxSelected].params[p] = mn + (mx - mn) * pots[3+p].value;
                                if (fxSelected==0) s_filtMetaChanged=true;
                            }
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
                // Also sync P5/P6 (static, initialised inside the else branch below)
                // by marking a re-sync needed the next time pot code runs.
                gran2PotNeedsSync = true;
            } else if (gran2ActiveSample >= 0) {
                uint8_t as = (uint8_t)gran2ActiveSample;
                if (as < GRAN2_MAX_SAMPLES && gran2[as].computed && gran2ActiveSlice >= 0) {
                    bool needsSync = gran2PotNeedsSync;
                    if (needsSync) {
                        gran2PotNeedsSync = false;
                        // Recenter all pot baselines so splits can move in either direction.
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

                    // P5 = shift the entire slice window; P6 = zoom (resize around center)
                    static float g2LastPC = 0.5f, g2LastPD = 0.5f;
                    if (needsSync) { pots[5].value = 0.5f; g2LastPC = 0.5f; pots[6].value = 0.5f; g2LastPD = 0.5f; }
                    float dpC = pots[5].value - g2LastPC;
                    float dpD = pots[6].value - g2LastPD;
                    g2LastPC = pots[5].value;
                    g2LastPD = pots[6].value;
                    bool changedCD = false;

                    if (fabsf(dpC) > 0.002f) {
                        // Shift: move both boundaries together, keeping width
                        float w = gs.splits[x + 1] - gs.splits[x];
                        float ns = constrain(gs.splits[x] + dpC, 0.0f, 1.0f - w);
                        gs.splits[x] = ns;
                        gs.splits[x + 1] = ns + w;
                        for (int i = x - 1; i >= 0;          i--) { if (gs.splits[i] > gs.splits[i+1]) gs.splits[i] = gs.splits[i+1]; else break; }
                        for (int i = x + 2; i <= gs.sliceCount; i++) { if (gs.splits[i] < gs.splits[i-1]) gs.splits[i] = gs.splits[i-1]; else break; }
                        changedCD = true;
                    }
                    if (fabsf(dpD) > 0.002f) {
                        // Zoom: expand/contract around center of slice
                        float center = (gs.splits[x] + gs.splits[x + 1]) * 0.5f;
                        float halfW  = (gs.splits[x + 1] - gs.splits[x]) * 0.5f + dpD * 0.5f;
                        halfW = constrain(halfW, 0.005f, 0.5f);
                        gs.splits[x]     = constrain(center - halfW, 0.0f, 1.0f);
                        gs.splits[x + 1] = constrain(center + halfW, 0.0f, 1.0f);
                        for (int i = x - 1; i >= 0;          i--) { if (gs.splits[i] > gs.splits[i+1]) gs.splits[i] = gs.splits[i+1]; else break; }
                        for (int i = x + 2; i <= gs.sliceCount; i++) { if (gs.splits[i] < gs.splits[i-1]) gs.splits[i] = gs.splits[i-1]; else break; }
                        changedCD = true;
                    }
                    if (changedCD) {
                        audioApplyGranular2Splits(as, gs.splits, gs.sliceCount, gran2PlayMode == 1);
                    }
                }
            }
        }

        // Instrument browser (OVERLAY_INSTR): joystick X=category, Y=item, click=confirm
        if (s_overlay == OVERLAY_INSTR) {
            static unsigned long lastInstrNav = 0;
            if (millis() - lastInstrNav >= 160) {
                float nx = cachedJoyX / 64.0f;
                float ny = cachedJoyY / 64.0f;
                bool moved = false;
                if (nx < -0.3f) {
                    if (instrBrCat > 0) { instrBrCat--; instrBrItem = 0; } moved = true;
                } else if (nx > 0.3f) {
                    if (instrBrCat < INSTR_CAT_COUNT - 1) { instrBrCat++; instrBrItem = 0; } moved = true;
                }
                const InstrCategory &cat = instrCategories[instrBrCat];
                if (ny < -0.3f) {
                    if (instrBrItem > 0) { instrBrItem--; } moved = true;
                } else if (ny > 0.3f) {
                    if (instrBrItem + 1 < cat.count) { instrBrItem++; } moved = true;
                }
                if (moved) {
                    // Preview: apply highlighted instrument immediately
                    uint8_t shapeIdx = instrCategories[instrBrCat].start + instrBrItem;
                    if (shapeIdx < SHAPE_COUNT) {
                        currentShape = (SynthShape)shapeIdx;
                        audioSetShape(currentShape);
                    }
                    lastInstrNav = millis();
                }
            }
        }

        // FX automation editor (OVERLAY_FX_MOD): joystick X cycles which of the selected
        // FX's non-empty params is being automated; joystick Y browses curated LFO
        // starting points (kLfoProfiles[]) and applies the picked one immediately —
        // shape/rate/sync set together in one gesture instead of hunting across 3 pots
        // to reconstruct a good combination by hand. Depth stays a separate pot (P4):
        // picking a profile only touches shape/rate/sync, and gives the slot a sensible
        // default depth if it doesn't have one yet so the effect is audible right away.
        if (s_overlay == OVERLAY_FX_MOD) {
            static unsigned long lastFxModNav = 0;
            if (millis() - lastFxModNav >= 200) {
                float nx = cachedJoyX / 64.0f;
                float ny = cachedJoyY / 64.0f;
                FxEffect &fx = fxList[fxSelected];
                int8_t np = -1;
                if (nx < -0.3f) {
                    for (int8_t p = (int8_t)s_fxModEditParam - 1; p >= 0; p--)
                        if (fx.paramNames[p][0] != '\0') { np = p; break; }
                } else if (nx > 0.3f) {
                    for (int8_t p = (int8_t)s_fxModEditParam + 1; p < 4; p++)
                        if (fx.paramNames[p][0] != '\0') { np = p; break; }
                }
                if (np >= 0) {
                    s_fxModEditParam = (uint8_t)np;
                    s_fxModEditSlot = modSlotFindFxParam(fxSelected, s_fxModEditParam);
                    s_fxModProfileIdx = -1;
                    lastFxModNav = millis();
                }

                int8_t nprof = -1;
                if (ny < -0.3f || ny > 0.3f) {
                    if (s_fxModProfileIdx < 0) nprof = 0;
                    else if (ny < -0.3f && s_fxModProfileIdx > 0) nprof = s_fxModProfileIdx - 1;
                    else if (ny > 0.3f && s_fxModProfileIdx < (int8_t)LFO_PROFILE_COUNT - 1) nprof = s_fxModProfileIdx + 1;
                }
                if (nprof >= 0) {
                    s_fxModProfileIdx = nprof;
                    if (s_fxModEditSlot < 0) s_fxModEditSlot = modSlotAllocFxParam();
                    if (s_fxModEditSlot >= 0) {
                        ModSlot &s = gModSlots[s_fxModEditSlot];
                        const LfoProfile &p = kLfoProfiles[nprof];
                        s.destKind = MODDEST_FX_PARAM;
                        s.destA = fxSelected; s.destB = s_fxModEditParam;
                        s.shape = p.shape; s.bpmSync = p.bpmSync; s.rateHz = p.rateHz; s.bpmDivIdx = p.bpmDivIdx;
                        if (s.depth < 0.01f) s.depth = 0.5f;
                        s.active = true;
                    }
                    lastFxModNav = millis();
                }
            }
        }

        // GEN (MODE_GEN): JX browses the procedural texture (generator), JY browses the
        // sound-making method (voice) — two fully independent axes, mirroring how
        // OVERLAY_PKMN below already splits JX/JY across two unrelated lists. Selecting a
        // generator resets its own tick position so switching textures feels immediate
        // rather than picking up mid-pattern from whatever the old generator left behind.
        if (currentMode == MODE_GEN && !menuOpen) {
            static unsigned long lastGenNav = 0;
            if (millis() - lastGenNav >= 220) {
                float nx = constrain(cachedJoyX/64.0f,-1.0f,1.0f);
                float ny = constrain(cachedJoyY/64.0f,-1.0f,1.0f);
                bool changed = false;
                if (nx < -0.4f && genGenerator > 0) { genGenerator--; changed = true; }
                else if (nx > 0.4f && genGenerator < GEN_GENERATOR_COUNT-1) { genGenerator++; changed = true; }
                if (changed) {
                    genWalkPos = 3; genEuclStepIdx = 0; genEuclPulsesBuilt = 0xFF; genDriftX = 0.42f;
                }
                bool vChanged = false;
                if (ny < -0.4f && genVoice > 0) { genVoice--; vChanged = true; }
                else if (ny > 0.4f && genVoice < GEN_VOICE_COUNT-1) { genVoice++; vChanged = true; }
                if (vChanged && audioReady) genApplyVoice();
                if (changed || vChanged) lastGenNav = millis();
            }
        }

        // SERUM (MODE_MODULAR): the joystick has no performance role here (no velocity,
        // no pitch-bend) — JX browses which element's live curve is focused (OscA/OscB/
        // Filter/Env/LFO), JY modifies THAT element's parameter directly. Two of these
        // (filter resonance, LFO rate) had no live control at all before this — pots
        // were already full (P2/P4-P7), so the joystick is what makes them reachable.
        if (currentMode == MODE_MODULAR && !menuOpen) {
            static unsigned long lastModFocusNav = 0, lastModParamNav = 0;
            float nx = constrain(cachedJoyX/64.0f,-1.0f,1.0f);
            float ny = constrain(cachedJoyY/64.0f,-1.0f,1.0f);
            if (millis() - lastModFocusNav >= 220) {
                if (nx < -0.4f && modFocusIdx > 0) { modFocusIdx--; lastModFocusNav = millis(); }
                else if (nx > 0.4f && modFocusIdx < 4) { modFocusIdx++; lastModFocusNav = millis(); }
            }
            if (audioReady && fabsf(ny) > 0.15f) {
                switch (modFocusIdx) {
                    case 0: // OscA morph position — continuous
                        modOscAPos = constrain(modOscAPos + ny*0.02f, 0.0f, 1.0f);
                        audioModularSetWtPos(MOD3_OSCA_CH, modOscAPos);
                        break;
                    case 1: // OscB morph position — continuous
                        modOscBPos = constrain(modOscBPos + ny*0.02f, 0.0f, 1.0f);
                        audioModularSetWtPos(MOD3_OSCB_CH, modOscBPos);
                        break;
                    case 2: // Filter resonance — continuous, was pot-less until now
                        modReso = constrain(modReso + ny*0.08f, 0.5f, 6.0f);
                        audioModularSetFilter(modCutoff, modReso);
                        break;
                    case 3: // Envelope preset — discrete, same debounce as focus-nav
                        if (millis() - lastModParamNav >= 250) {
                            currentEnv = (EnvPreset)((currentEnv + (ny>0?1:ENV_PRESET_COUNT-1)) % ENV_PRESET_COUNT);
                            audioModularSetEnvelope(envTable[currentEnv]);
                            lastModParamNav = millis();
                        }
                        break;
                    default: // LFO rate — continuous, was pot-less until now
                        modLfoRate = constrain(modLfoRate + ny*0.05f, 0.05f, 10.0f);
                        break;
                }
            }
        }

        // Pokémon browser (OVERLAY_PKMN): JX=type, JY=item
        if (s_overlay == OVERLAY_PKMN) {
            static unsigned long lastPkmnNav = 0;
            if (millis() - lastPkmnNav >= 160) {
                float nx = cachedJoyX / 64.0f;
                float ny = cachedJoyY / 64.0f;
                bool moved = false;
                // JX: scroll type
                if (nx > 0.3f) {
                    uint8_t next = pkmnBrType;
                    do { next = (next + 1) % PKMN_TYPE_COUNT; }
                    while (next != pkmnBrType && ![&]{ for(int i=0;i<PKMN_COUNT;i++) if((uint8_t)kPokemon[i].type==next) return true; return false; }());
                    pkmnBrType = next; pkmnBrItem = 0; moved = true;
                } else if (nx < -0.3f) {
                    uint8_t prev = pkmnBrType;
                    do { prev = (prev + PKMN_TYPE_COUNT - 1) % PKMN_TYPE_COUNT; }
                    while (prev != pkmnBrType && ![&]{ for(int i=0;i<PKMN_COUNT;i++) if((uint8_t)kPokemon[i].type==prev) return true; return false; }());
                    pkmnBrType = prev; pkmnBrItem = 0; moved = true;
                }
                // JY: scroll item within type
                if (ny < -0.3f && pkmnBrItem > 0) { pkmnBrItem--; moved = true; }
                else if (ny > 0.3f) {
                    uint8_t cnt=0; for(int i=0;i<PKMN_COUNT;i++) if((uint8_t)kPokemon[i].type==pkmnBrType) cnt++;
                    if (pkmnBrItem+1 < cnt) { pkmnBrItem++; moved = true; }
                }
                if (moved) lastPkmnNav = millis();
            }
        }

        // I303 wave browser (OVERLAY_I303_WAVE): JY=item
        if (s_overlay == OVERLAY_I303_WAVE) {
            static unsigned long lastI303WaveNav = 0;
            if (millis() - lastI303WaveNav >= 150) {
                float ny = cachedJoyY / 64.0f;
                bool moved = false;
                if (ny < -0.3f && i303WaveBr > 0)              { i303WaveBr--; moved = true; }
                else if (ny > 0.3f && i303WaveBr+1 < kI303WaveCount) { i303WaveBr++; moved = true; }
                if (moved) { lastI303WaveNav = millis(); drawScreen(false); }
            }
        }

        // Global pitch bend via JY — SYNTH / HYBRID (MODULAR/MOD2 handle it themselves)
        if((currentMode==MODE_SYNTH||currentMode==MODE_MOD2)&&!menuOpen&&audioReady&&s_overlay!=OVERLAY_INSTR){
            float jy=cachedJoyY/64.0f;
            float bend=(fabsf(jy)<0.15f)?0.0f:-jy*2.0f;  // ±2 semitones, centre deadzone
            audioSetPitchBend(powf(2.0f,bend/12.0f));
        }

        // SD browser joystick navigation (SAMPLE, GRANULAR — always browsable)
        bool needsSdNav = currentMode==MODE_SAMPLE
                       || currentMode==MODE_GRANULAR2
                       || currentMode==MODE_STONE
                       || (currentMode==MODE_VID && !vidPlaying && !mediaAudioPlaying)
                       || (currentMode==MODE_DRUM2 && drum2View==2 && draniBrowse);
        if(needsSdNav&&!menuOpen&&sdReady){
            static unsigned long lastSdNav=0;
            float ny=cachedJoyY/64.0f;
            bool gran2AnyComputed = false;
            if (currentMode==MODE_GRANULAR2) for (uint8_t _s=0;_s<GRAN2_MAX_SAMPLES;_s++) if (gran2[_s].computed) { gran2AnyComputed=true; break; }
            uint8_t visLines=(currentMode==MODE_GRANULAR2&&!gran2AnyComputed)?13
                            :(currentMode==MODE_GRANULAR2)?7
                            :(currentMode==MODE_STONE)?7:8;
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
            while(midiReadPacket(&pkt)){
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

        // Menu joystick navigation (tab bar + item grid)
        if(menuOpen){
            static unsigned long lastNav=0;
            float nx=cachedJoyX/64.0f,ny=cachedJoyY/64.0f;
            if(millis()-lastNav>=200){bool m=false;
                if (menuOnTabBar) {
                    // On tab bar: X switches tab, Y down enters items
                    if(nx<-0.3f&&menuCategory>0){menuCategory--;menuRow=0;menuCol=0;m=true;}
                    if(nx>0.3f&&menuCategory<2){menuCategory++;menuRow=0;menuCol=0;m=true;}
                    if(ny>0.3f){menuOnTabBar=false;menuRow=0;menuCol=0;m=true;}
                } else {
                    // In items: X navigates columns, Y navigates rows (Y up at row 0 → tab bar)
                    uint8_t catRows=(kMenuCatSizes[menuCategory]+MENU_COLS-1)/MENU_COLS;
                    if(ny<-0.3f){
                        if(menuRow>0){menuRow--;m=true;}
                        else{menuOnTabBar=true;m=true;}
                    }
                    if(ny>0.3f&&menuRow<catRows-1){menuRow++;m=true;}
                    if(nx<-0.3f&&menuCol>0){menuCol--;m=true;}
                    if(nx>0.3f&&menuCol<MENU_COLS-1){menuCol++;m=true;}
                }
                if(m){ lastNav=millis(); drawScreen(true); }
            }
        }
    }

    // ---- DISPLAY (100ms, or 200ms at high BPM to reduce sequencer jitter) ----
    // sendBuffer() blocks ~47ms (I2C 400kHz × 128×128px). At 600 BPM step=25ms,
    // one step is always late after each display. Doubling the interval halves jitter frequency.
    static unsigned long lastScr=0;
    static bool s_lastMenuOpen = false;
    if (menuOpen != s_lastMenuOpen) {
        s_lastMenuOpen = menuOpen;
        drawScreen(true);   // immediate redraw when menu opens or closes
        lastScr = millis();
    }
    { unsigned long scrMs = (bpm > 250) ? 200UL : 100UL;
      if(millis()-lastScr>=scrMs){lastScr=millis();drawScreen();} }

#ifdef SIMULATOR
    // ---- SIMULATOR ON-SCREEN LABELS (150ms) ----
    // Keeps pot/button labels live within a mode (FX param names as FX gets toggled/
    // switched, DRUM2's per-pad pitch/decay/volume, etc.) — printCtrlLabels() only
    // fires on an actual mode switch, which alone isn't enough for these.
    { static unsigned long lastLbl=0;
      if(millis()-lastLbl>=150){lastLbl=millis();updateSimCtrlLabels(currentMode);} }
#endif

    // ---- POWER (500ms) ----
    static unsigned long lastPwr=0;
    if(millis()-lastPwr>=500){lastPwr=millis();
        if(!digitalRead(PWR_SENSE)){delay(50);if(!digitalRead(PWR_SENSE))digitalWrite(PWR_ON_EN,LOW);}
    }

    // ---- STONE release fade (cheap no-op when no voice is releasing) ----
    audioStoneFadeTick();

    // ---- PCM CACHE CLEANER (iterative, runs until done) ----
    if (pcmCleanRunning) {
        static bool pcmCleanStarted = false;
        if (!pcmCleanStarted) { pcmCleanStart(); pcmCleanStarted = true; }
        bool done = pcmCleanStep();
        if (done) { pcmCleanRunning = false; pcmCleanPhase = 2; pcmCleanStarted = false; }
    }

#ifdef __ANDROID__
    {
        // Always-visible on-screen "import a folder" button (sim_window.cpp draws/hit-
        // tests it) — one tap jumps straight to MODE_IMPORT and opens the SAF picker
        // immediately, from any mode/menu state, bypassing menu navigation and the
        // B1-inside-MODE_IMPORT gesture entirely (both were hard to discover/reach on
        // a touchscreen: B1 is an unlabeled key among the note grid, and MODE_IMPORT
        // itself sits deep in the AUTRE tab's menu grid).
        extern volatile bool g_simImportTap;
        if (g_simImportTap) {
            g_simImportTap = false;
            if (currentMode != MODE_IMPORT) switchMode(MODE_IMPORT);
            androidPickFolder();
            importPhase = 1;
        }
    }
    if (importPhase == 1) {
        static uint32_t lastPoll = 0;
        if (millis() - lastPoll > 250 && sdReady) {
            lastPoll = millis();
            File f = SD.open("/.grv_import.status");
            if (f) {
                char line[64] = {};
                size_t n = f.read((uint8_t*)line, sizeof(line) - 1);
                line[n] = 0;
                f.close();
                char tag[16] = {};
                uint32_t d = 0, t = 0;
                if (sscanf(line, "%15s %u %u", tag, &d, &t) == 3) {
                    importDone = d; importTotal = t;
                    if (strcmp(tag, "DONE") == 0) importPhase = 2;
                }
            }
        }
    }
#endif
}
