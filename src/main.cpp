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
// Patch names from Diapasonix (258 Juno+DX7 presets) — wrapped to avoid ODR conflict
namespace { // anonymous namespace: translation-unit local
#include "../other_projects/Diapasonix/display/patch_names.h"
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
SynthShape currentShape = SHAPE_SAW;
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
    OVERLAY_INSTR, OVERLAY_SEQ_OPT, OVERLAY_SAMP_OPT, OVERLAY_303, OVERLAY_303_PRESET
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
static float         t303Reso        = 4.0f;      // resonance 1-4
static float         t303EnvMod      = 0.0f;      // filter EG depth (0=off, classic plat)
static float         t303Decay       = 500.0f;    // EG decay ms (fixed — not pot-controlled)
static uint8_t       t303Wave        = SAW_DOWN;  // AMY wave constant (SAW_DOWN, PULSE, TRIANGLE, SINE…)
static int8_t        t303Oct         = 0;         // octave offset -1..+1
static bool          t303AccentOn    = false;
static bool          t303SustainOn   = false;     // OFF=pluck (clear decay), ON=held at full amp
static bool          t303SlideOn     = false;
static uint8_t       t303CurrentNote = 0;
static int8_t        t303PressedRow  = -1;
static int8_t        t303PressedCol  = -1;
static float         t303Reverb      = 0.0f;      // reverb send level 0→1 (P5)
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


// Light play ripple state (MODE_LIGHTPLAY)
struct Ripple { int8_t ledIdx; float radius; uint8_t hue; uint8_t bright; };
static Ripple ripples[8];
static uint8_t rippleBrightMap[NUM_LEDS] = {};

// Menu
bool    menuOpen       = false;
uint8_t menuRow        = 0, menuCol = 0;
bool    lastClick      = false;
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
    // LPF: 4-pole 24dB/oct (FILTER_LPF24 — Moog-style ladder character)
    // Cut: 65Hz (AMY floor, ~silence at 24dB/oct) to 18kHz (fully open)
    {"LPF",      false, {2000.0f, 4.0f,  0.0f, 0.0f},
     {"Cut","Res","",""},
     {65.0f,   0.5f, 0.0f, 0.0f},
     {18000.0f,  4.0f, 0.0f, 0.0f}},
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
};
static const uint8_t FX_COUNT = 8;

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
        case 0:  // LPF
            Serial.printf("FX0 LPF %s cut=%.0f res=%.2f\n", on?"ON":"off", on?e.params[0]:0.0f, on?e.params[1]:1.5f);
            audioSetAllFilters(on ? e.params[0] : 0.0f, on ? e.params[1] : 1.5f);
            if (!on && noFilterFx()) audioRestoreShapeFilter(currentShape);
            break;
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
        audioSetReverb(t303Reverb * 0.8f, 0.85f, 0.5f, 3000.0f);
    }
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
            currentShape = (SynthShape)opt;
            audioSetShape(currentShape);
        }
        s_overlayCloseAt = millis() + 200;
        return;
    }

    bool is4col = (s_overlay == OVERLAY_SCALE_ARP || s_overlay == OVERLAY_303 || s_overlay == OVERLAY_303_PRESET);
    if (col < (is4col ? 4u : 6u)) { s_overlay = OVERLAY_NONE; s_overlayCloseAt = 0; return; }

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
                    if (currentMode == MODE_303 && opt == 2 && !fxList[2].active && audioReady)
                        audioSetReverb(t303Reverb * 0.8f, 0.85f, 0.5f, 3000.0f);
                }
                Serial.printf("OVL FX[%d] %s\n", opt, fxList[opt].active?"ON":"OFF");
            }
            return;  // FX overlay stays open for multi-toggle (no close timer)
        case OVERLAY_SCALE_ARP:
            // col7 (opts 0-3): scales 0-3  |  col6 (opts 4-7): scales 4-5, empty 6-7
            // col5 (opts 8-11): arp Up/Dn/UD/Rnd (modes 1-4, toggle: re-click → off)
            // col4 (opts 12-15): octave -2/-1/0/+1
            if (opt < 4) { if (opt < (uint8_t)SCALE_COUNT) noteMap.setScale((Scale)opt); }
            else if (opt < 8) { uint8_t si=opt-4; if (si+4 < (uint8_t)SCALE_COUNT) noteMap.setScale((Scale)(si+4)); }
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
                    default: break;
                }
            }
            btn1PressTime=0;
        }
        return;
    }
    if (!pressed && !(currentMode==MODE_SAMPLE&&(samplePlayMode==1||samplePlayMode==2))) return;

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
    pollKeyboard();   // drain TCA8418 FIFO before the ~47ms I2C block
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
        bool is4col = (s_overlay == OVERLAY_SCALE_ARP || s_overlay == OVERLAY_303 || s_overlay == OVERLAY_303_PRESET);
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
                    if (vpc == 0) break;
                    // Special case: Delay shows level + BPM-synced time hint
                    if (i == 5) {
                        char v[8]; fmtFloat(v, sizeof(v), fx.params[vp[0]]);
                        snprintf(b.l2, sizeof(b.l2), "%s:%s", fx.paramNames[vp[0]], v);
                        int dms = (int)(60000.0f / (float)bpm * kDelaySubdiv[4]);
                        snprintf(b.l3, sizeof(b.l3), "1/4=%dms", dms);
                        break;
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
            case OVERLAY_ENV:
                for (int i = 0; i < (int)ENV_PRESET_COUNT; i++) {
                    bx[i].avail = true; bx[i].sel = (currentEnv == (EnvPreset)i);
                    strncpy(bx[i].l1, envNames[i], sizeof(bx[i].l1)-1);
                    snprintf(bx[i].l2, sizeof(bx[i].l2), "A:%d D:%d", envTable[i].atk, envTable[i].dec);
                    snprintf(bx[i].l3, sizeof(bx[i].l3), "S:%d%% R:%d",
                             (int)(envTable[i].sus * 100), envTable[i].rel);
                }
                break;
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
            default: break;
        }

        // Render: 4-col → 32×32px cells; 2-col → 64×32px cells
        int ncols = is4col ? 4 : 2;
        int cw    = 128 / ncols;
        for (int i = 0; i < ncols * 4; i++) {
            int gc = i / 4, gr = i % 4;
            int x = gc * cw, y = gr * 32;
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
                oled.drawStr(0,0,"SAMPLE");
                oled.drawHLine(0,9,128);
                {char pb[22]; snprintf(pb,sizeof(pb),"%.21s",sdPath.c_str()); oled.drawStr(0,12,pb);}
                if(!sdReady){
                    oled.drawStr(10,40,"SD not found");
                    oled.drawStr(10,55,"Click = retry");
                } else {
                    // File list — 8 items, 8px spacing keeps last item (y=76) clear of separator at y=93
                    int vis=8;
                    for(int i=0;i<vis&&(i+sdScroll)<sdFileCount;i++){
                        int idx=i+sdScroll, y=20+i*8; bool sel=(idx==sdCursor);
                        bool mapped=false;
                        if(!sdFileIsDir[idx]){
                            char fpath[80];
                            snprintf(fpath,sizeof(fpath),"%s%s%s",
                                sdPath.c_str(),sdPath.endsWith("/")?"":"/",sdFiles[idx].c_str());
                            for(int r=0;r<KBD_NOTE_ROWS&&!mapped;r++)
                                for(int c=0;c<KBD_COLS&&!mapped;c++)
                                    if(sampleMap[r][c]==fpath) mapped=true;
                        }
                        char line[25];
                        bool isWav = !sdFileIsDir[idx] && isWavFile(sdFiles[idx].c_str());
                        if(sdFileIsDir[idx]) snprintf(line,sizeof(line),"%s[%s]",sel?">":"  ",sdFiles[idx].c_str());
                        else snprintf(line,sizeof(line),"%s%s%s%s",sel?">":"  ",mapped?"*":"",isWav?"":"~",sdFiles[idx].c_str());
                        line[24]='\0';
                        oled.drawStr(0,y,line);
                    }
                    // Mini progress grid: 4 rows × 8 cols, cells 15×3px, 1px gap between rows.
                    // Grid starts at cy=94 so row 0 ends at y=108 — leaves clear gap before status text.
                    // □=unassigned  ▭=loading (frame)  ▬=loaded (filled)
                    oled.drawHLine(0, 93, 128);
                    uint8_t nLoaded=0, nErr=0, errType=KEY_ERR_NONE;
                    for(int r=0;r<KBD_NOTE_ROWS;r++) for(int c=0;c<KBD_COLS;c++){
                        uint8_t kidx=(uint8_t)(r*8+c);
                        int cx=(7-c)*16, cy=94+(3-r)*4;
                        bool assigned=sampleMap[r][c].length()>0;
                        bool loaded=audioKeyLoaded(kidx);
                        uint8_t kerr=audioKeyError(kidx);
                        if(loaded){ oled.drawBox(cx,cy,15,3); nLoaded++; }
                        else if(kerr){
                            // '+' cross = load error (visible even at 15×3px)
                            oled.drawHLine(cx,cy+1,15);
                            oled.drawVLine(cx+7,cy,3);
                            nErr++; errType=kerr;
                        }
                        else if(assigned) oled.drawFrame(cx,cy,15,3);
                    }
                    // Status lines (4x6 font — smaller to stay below grid with safe margin)
                    oled.setFont(u8g2_font_4x6_tf);
                    if(nErr>0){
                        // Error detail on hint line so play-mode stays visible below
                        uint32_t freeKb=(uint32_t)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)/1024);
                        const char* eStr=errType==KEY_ERR_ALLOC?"RAM":errType==KEY_ERR_FORMAT?"fmt":"I/O";
                        snprintf(buf,sizeof(buf),"!%u %s %ukB B3=clr",nErr,eStr,(unsigned)freeKb);
                        oled.drawStr(0,116,buf);
                    } else {
                        // FX line: show selected active effect + up to 3 params; hint if none active
                        const FxEffect& selFx = fxList[fxSelected];
                        if (selFx.active) {
                            char fxLine[33]; snprintf(fxLine, sizeof(fxLine), "[%.3s]", selFx.name);
                            for (int p = 0; p < 4; p++) {
                                if (!selFx.paramNames[p][0]) continue;
                                char fv[6]; fmtFloat(fv, sizeof(fv), selFx.params[p]);
                                char ent[9]; snprintf(ent, sizeof(ent), " %.2s:%.4s", selFx.paramNames[p], fv);
                                if (strlen(fxLine) + strlen(ent) < sizeof(fxLine) - 1)
                                    strcat(fxLine, ent);
                            }
                            oled.drawStr(0, 116, fxLine);
                        } else {
                            if(sdCursor<sdFileCount && !sdFileIsDir[sdCursor])
                                oled.drawStr(0,116,"Key=asgn B2=map Clk=dir");
                            else
                                oled.drawStr(0,116,"Clk=open  B2=mapAll");
                        }
                    }
                    // Play-mode status always on last line
                    snprintf(buf,sizeof(buf),"[%s] %u/%u rdy",kSplayModes[samplePlayMode],nLoaded,SAMPLE_KEY_COUNT);
                    oled.drawStr(0,124,buf);
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
                    char pb[40]; int ppos = 0;
                    for (int p = 0; p < 4; p++) {
                        if (!fxList[fxSelected].paramNames[p][0]) continue;
                        if (fxSelected == 5 && p == 1) {
                            // Delay subdivision: show musical name + computed ms
                            uint8_t si = (uint8_t)constrain((int)roundf(fxList[5].params[1]), 0, DELAY_SUBDIV_COUNT-1);
                            float dms  = constrain(60000.0f/(float)bpm*kDelaySubdiv[si], 30.0f, 700.0f);
                            ppos += snprintf(pb+ppos, sizeof(pb)-ppos, "Div:%s(%.0f) ", kDelaySubdivName[si], dms);
                        } else {
                            ppos += snprintf(pb+ppos, sizeof(pb)-ppos, "%s:%.1f ",
                                             fxList[fxSelected].paramNames[p], fxList[fxSelected].params[p]);
                        }
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
                { static const char* wn[]={"SIN","SQR","SAW","SWU","TRI","NSE","KS"};
                  uint8_t wi=(t303Wave<7)?t303Wave:0;
                  snprintf(buf,sizeof(buf),"303 %s/%s %dBPM%s%s%s",
                           t303Tones[t303ToneIdx].name, wn[wi], bpm,
                           t303AccentOn?" [A]":"",t303SlideOn?" [S]":"",
                           t303SustainOn?" [Sus]":""); }
                oled.drawStr(0,0,buf); oled.drawHLine(0,7,128);
                snprintf(buf,sizeof(buf),"Cut:%4dHz  Res:%.1f",(int)t303Cutoff,t303Reso);
                oled.drawStr(0,17,buf);
                snprintf(buf,sizeof(buf),"Mod:%4dHz  Rev:%.0f%%",(int)t303EnvMod,t303Reverb*100.0f);
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
                oled.drawStr(0,71,"P3=Rs P4=Mod P5=Rev P6=Ct | B1=Wv/Arp B3=Tone");
                oled.setFont(u8g2_font_5x7_tf);
                snprintf(buf,sizeof(buf),"Vol:%.0f%%",pots[0].value*100);
                oled.drawStr(0,118,buf);
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

    noteMap.setScale(SCALE_CHROMATIC);
    noteMap.setOctave(-1);
    for(int i=0;i<7;i++) pots[i].value=0.5f;
    pots[0].value=0.7f;  // volume
    pots[1].value=0.5f;  // shape (mid range)
    pots[2].value=0.0f;  // envelope (Normal)
    pots[3].value=0.0f; pots[4].value=0.0f; pots[5].value=0.0f; pots[6].value=0.0f;
    audioSetEnvelope(envTable[currentEnv]);
    audioSetVolume(pots[0].value);
    Serial.println("Ready");
}

// ==================== LOOP ====================
void loop() {
    pollKeyboard();   // keyboard first — shortest path to audioNoteOn
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

        switch(currentMode){
            case MODE_SYNTH:{
                uint8_t note=noteMap.getMidiNote(row,col);
                activeNotes[row][col]=pressed?note:0;
                if(arpMode==0){
                    if(pressed){
                        float jx=constrain(cachedJoyX/64.0f,-1.0f,1.0f);
                        audioNoteOn(note, constrain(0.8f+jx*0.6f, 0.05f, 1.5f));
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
                            // Assigned but not loaded (e.g. load was aborted by a preview click).
                            // Re-queue the load; key will play on next press once ready.
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
                uint8_t base = noteMap.getMidiNote(row, col);
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
            default: break;
        }
    }
    if(btn1PressTime>0&&!btn1Handled&&(millis()-btn1PressTime>=600)){
        btn1Handled=true; menuOpen=!menuOpen;
        if(menuOpen){audioAllNotesOff();omniRoot=0xFF;menuRow=0;menuCol=0;}
    }

    // ---- JOYSTICK CLICK ----
    bool click=!digitalRead(JOYSW);
    if(click&&!lastClick){
        if(menuOpen){
            selectMenuItem();
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
        } else {
            audioAllNotesOff(); omniRoot=0xFF; omniStrumPos=-1;
            menuOpen=true; menuRow=0; menuCol=0;
        }
    }
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

    // ---- LEDS (20fps) ----
    static unsigned long lastLed=0;
    if(millis()-lastLed>=8){
        lastLed=millis();
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
                        uint8_t base = noteMap.getMidiNote(r, c);
                        uint8_t note = (uint8_t)constrain((int)base + (int)t303Oct*12, 0, 127);
                        bool playing = (note == t303CurrentNote && t303CurrentNote != 0);
                        bool pressed = keyState[r][c];
                        int gr=KBD_ROWS-1-r, gc=KBD_COLS-1-c, idx=gr*KBD_COLS+gc;
                        int li=(idx>=0&&idx<NUM_LEDS+4)?crdToIdx(idx,0):-1;
                        if(li<0||li>=NUM_LEDS) continue;
                        if(playing)       leds[li] = t303AccentOn ? CHSV(40,255,255) : CHSV(0,255,255);
                        else if(pressed)  leds[li] = CHSV(15, 220, 180);  // held but not current: warm amber
                        else              leds[li] = CHSV(10, 180, 15);
                    }
                    break;
                }
                default:
                    for(int r=0;r<KBD_NOTE_ROWS;r++) for(int c=0;c<KBD_COLS;c++)
                        if(keyState[r][c]){
                            int gr=KBD_ROWS-1-r,gc=KBD_COLS-1-c,idx=gr*KBD_COLS+gc;
                            if(idx>=0&&idx<NUM_LEDS+4){int li=crdToIdx(idx,0);
                                if(li>=0&&li<NUM_LEDS) leds[li]=CHSV((r*8+c)*37+80,255,255);}
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
                        else if (opt >= 4 && opt < 8 && (opt-4+4) < (uint8_t)SCALE_COUNT) { show=true; sel=(noteMap.getScale()==(Scale)(opt-4+4)); hue=100; }
                        else if (opt >= 8 && opt < 12)  { show=true; sel=(arpMode==opt-7);                  hue=20;  }
                        else if (opt >= 12 && opt < 16) { show=true; sel=(noteMap.getOctave()==kOctOpts[opt-12]); hue=200; }
                        break;
                    case OVERLAY_ENV:
                        if (opt < ENV_PRESET_COUNT) { show=true; sel=(currentEnv==(EnvPreset)opt); hue=170; }
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
                    default: break;
                }
                // 100% brightness; amber pour sélectionné, hue catégorie pour disponible
                if (show) leds[ledIdx] = sel ? CHSV(35, 255, 255) : CHSV(hue, 255, 255);
            }
        }

        FastLED.show();
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
            }
        }

        if(audioReady&&!menuOpen){
            switch(currentMode){
                case MODE_SYNTH: {
                    static float lp1=-1, lp2=-1, lp3fm=-1, lp4fm=-1;
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
                        modReso=0.5f+pots[4].value*3.5f;  // 0.5→4.0
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
                    // P3=Reso 1→10  P4=EnvMod 0→10Hz  P5=Reverb 0→1  P6=Cutoff 80→2000Hz(exp)
                    // P2=BPM (global — not used here)
                    bool ch303=false;
                    if(fabsf(pots[3].value-lp303[0])>0.002f){ t303Reso=1.0f+pots[3].value*3.0f;           lp303[0]=pots[3].value; ch303=true; }  // 1→4
                    if(fabsf(pots[4].value-lp303[1])>0.002f){ t303EnvMod=pots[4].value*10.0f;              lp303[1]=pots[4].value; ch303=true; }
                    if(fabsf(pots[5].value-lp303[2])>0.002f){ t303Reverb=pots[5].value;                    lp303[2]=pots[5].value; if(audioReady) audioSetReverb(t303Reverb*0.8f,0.85f,0.5f,3000.0f); }
                    if(fabsf(pots[6].value-lp303[3])>0.002f){ t303Cutoff=80.0f*powf(25.0f,pots[6].value); lp303[3]=pots[6].value; ch303=true; }
                    if(ch303 && audioReady) audioT303Params(t303Cutoff,t303Reso,t303EnvMod,t303Decay);
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
                        mod2Reso=0.5f+pots[4].value*3.5f;  // 0.5→4.0
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

        // Smooth LPF cutoff application — anti-zipper when turning the cutoff encoder.
        // Use audioSetFilterFreq (no filter_type field) to avoid AMY biquad state resets
        // that cause an audible pop/click on each value change.
        if (fxList[0].active && !fxList[6].active && audioReady) {
            float target = fxList[0].params[0];
            lpfSmoothCut += (target - lpfSmoothCut) * 0.15f; // ~60ms time constant, smoother
            audioSetFilterFreq(lpfSmoothCut, fxList[0].params[1]);
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
        if(currentMode==MODE_303 && t303SlideActive && audioReady){
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

        // Global pitch bend via JY — SYNTH / SYNTH2 / HYBRID (MODULAR/MOD2 handle it themselves)
        if((currentMode==MODE_SYNTH||currentMode==MODE_SYNTH2||currentMode==MODE_HYBRID)&&!menuOpen&&audioReady){
            float jy=cachedJoyY/64.0f;
            float bend=(fabsf(jy)<0.15f)?0.0f:-jy*2.0f;  // ±2 semitones, centre deadzone
            audioSetPitchBend(powf(2.0f,bend/12.0f));
        }

        // SD browser joystick navigation (SAMPLE and SEQ modes)
        if((currentMode==MODE_SAMPLE||currentMode==MODE_SEQ)&&!menuOpen&&sdReady){
            static unsigned long lastSdNav=0;
            float ny=cachedJoyY/64.0f;
            uint8_t visLines=(currentMode==MODE_SEQ)?7:8;
            if(millis()-lastSdNav>=150){
                if(ny<-0.3f&&sdCursor>0){sdCursor--;if(sdCursor<sdScroll)sdScroll=sdCursor;lastSdNav=millis();}
                if(ny>0.3f&&sdCursor<sdFileCount-1){sdCursor++;if(sdCursor>=(int)(sdScroll+visLines))sdScroll=sdCursor-visLines+1;lastSdNav=millis();}
            }
        }

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

    // ---- DISPLAY (100ms) ----
    static unsigned long lastScr=0;
    if(millis()-lastScr>=100){lastScr=millis();drawScreen();}

    // ---- POWER (500ms) ----
    static unsigned long lastPwr=0;
    if(millis()-lastPwr>=500){lastPwr=millis();
        if(!digitalRead(PWR_SENSE)){delay(50);if(!digitalRead(PWR_SENSE))digitalWrite(PWR_ON_EN,LOW);}
    }
}
