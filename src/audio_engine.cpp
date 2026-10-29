// audio_engine.cpp — GrvEP-fusion facade over SevenistSynth_RitualMachine's graph-based
// engine (engine/core/engine.h), replacing the old AMY-backed implementation.
//
// Phase 1 (current): one engine slot ("MAIN"), one fixed default graph (saw -> filter ->
// env -> vca), just enough to prove note-on/off and the render path work end-to-end. Every
// other audio_engine.h function is a no-op stub below, filled in phase by phase per
// ~/.claude/plans/mutable-nibbling-melody.md's Phase 3-10 engine groups (core synth,
// sample/key playback, TB-303, drums/pads, modular/wavetable, ...). Do not add new engine
// slots/instances ad hoc outside that plan — each one has a real RAM/CPU cost (see the
// plan's "open risks" section).
#include "audio_engine.h"
#include "config.h"

// AMY stays alive during this migration: main.cpp's loop() still unconditionally calls
// amy_update() every tick (it hasn't been touched yet — see the fusion plan's Phase
// 3-10), and its own ~9 raw amy_add_event call sites (hi-hat chokes etc.) assume a
// running AMY instance. Skipping amy_start() here crashed on real hardware (not the
// simulator) with a null-pointer StoreProhibited inside amy_render() — AMY's internal
// oscillator/config state was never allocated — the moment loop() reached its first
// audio tick. Both engines run side by side until every mode is migrated off AMY.
#include <AMY-Arduino.h>
extern "C" { amy_err_t esp32_setup_i2s(); }

#include "engine/core/engine.h"
#include "engine/modules/synth_modules.h"
#include "engine/modules/fx_modules.h"
#include "engine/modules/fx2_modules.h"
#include "engine/dsp/svf.h"
#include "engine/modules/osc_engines.h"
#include "engine/modules/sampler_modules.h"
#include "engine/sampler/sample_bank.h"
#include "engine/sampler/smp_format.h"
#include "engine/sampler/storage.h"
#include "smp_import.h"

#include <SD.h>

#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <cmath>
#include <new>
#include <mutex>
#include <atomic>

// This board's internal DRAM is shared by main.cpp's own UI code, AMY's own state, and
// every FreeRTOS task stack (audioHandlerTask/ledUpdateTask/displayTask/kbdPollTask...).
// A live test caught free heap at the task-creation point swinging from ~50KB to ~16KB
// across two otherwise-identical boots (almost certainly SD FAT-mount overhead differing
// by whether a card was physically inserted) — any fixed-size allocation here that's
// comfortable in one case silently starves a task's xTaskCreatePinnedToCore() in the
// other (unchecked return value, no crash — just a task that never exists; this exact
// bug previously caused "no key press ever reaches audio" and a frozen OLED). Both the
// Engine object itself (~41KB, dominated by ENGINE_CMD_RING) and its placeholder heap
// move to PSRAM on real hardware instead, which this board has and nothing else is
// fighting over, to stop competing with internal-RAM-only task stacks entirely rather
// than re-tuning a static size against a moving target.
#if defined(ESP_PLATFORM) && !defined(SIMULATOR)
#include <esp_heap_caps.h>
#define GRVEP_FUSION_USE_PSRAM 1
#endif

using namespace sc;

bool audioReady = false;

namespace {

// Was 8 (matching AMY-era NUM_SYNTH_VOICES) until measured on real ESP32-S3 hardware:
// 8 held notes through N_FILTER's default FLTA_SVF algo (2 sections, full per-sample
// state-variable filter) cost 1517us/block against a 725.6us budget at 44.1kHz — over
// 2x over, well before adding drums/UI/AMY's own idle cost on top. Confirmed as (part
// of) the root cause of "lag sur le bouton de FX... bug un peu pour le switch de menu":
// the audio render task overrunning its budget starves everything else on the same
// core, most noticeable exactly when the user is interacting (pressing buttons) while
// notes ring. Switching N_FILTER to FLTA_LADDER below (process_lite path, still real
// self-resonant feedback, not a cheap/no-resonance compromise) plus this voice-count cut
// brought 6 held notes to 563.28us — comfortable headroom. 8-note chords are rare in
// practice; this is a real, deliberate trade-off (not silently dropped), worth raising
// if 6 ever feels tight.
constexpr int kMainVoices = 6;

enum MainNode : int { N_NOTE = 1, N_OSC, N_OSCX, N_FILTER, N_ENV, N_VCA, N_VOICE_OUT, N_BUS, N_MASTER };

Heap g_fastHeap;
#ifdef GRVEP_FUSION_USE_PSRAM
Engine *g_mainEnginePtr = nullptr;
#define g_mainEngine (*g_mainEnginePtr)
#else
Engine g_mainEngineObj;
Engine *g_mainEnginePtr = &g_mainEngineObj;
#define g_mainEngine (*g_mainEnginePtr)
#endif
bool g_mainReady = false;

// Drum hits (DRUM2/DRUMS2/DRUMS3's audioDrum2Hit/audioDrumSynthHit/audioPlayDrumPad) used
// to share ONE OSC/FILTER/ENV node set across all 8 pads, reconfigured live on every hit
// (wave type, filter cutoff/resonance, decay time via set_param). Since those params are
// per-NODE, not per-voice, triggering pad B while pad A was still ringing retroactively
// rewrote pad A's own still-playing voice too — confirmed directly: a kick's 400ms tail,
// isolated, still had signal at 200ms; the same kick interrupted by a snare partway
// through measured dead silent at that same point, because the snare's own hit had
// overwritten the shared envelope's decay time out from under the kick's own voice.
// Reported as "le son devient saturé et je ne peux plus revenir en arrière" — not a
// volume bug, a cross-voice corruption bug, same class CLAUDE.md already flags ("a full
// reconfigure silently resets or kills an already-sounding voice").
//
// Fix: one small, independent Engine instance per drum ROLE (not per pad — DRUM2's
// KK1/BNG and DRUMS2/3's BD/LT/HT all map onto just kKick/kTom, etc., see
// audioPlayDrumPad/audioDrumSynthHit's own role-mapping comments) rather than one shared
// pool. Each role's graph is a real small 808/909-style voice (pitch-enveloped sine for
// kick/tom, tone+filtered-noise mix for snare, filtered noise bursts for clap/hihats),
// fixed at build time — no more per-hit set_param reconfiguration, so nothing a NEW hit
// does can reach back into an already-ringing voice of a DIFFERENT role. Within one role
// (e.g. two toms, LT and HT), pitch still varies per hit safely via NOTE_IN's own
// per-voice pitch cable — that path was never the problem.
struct RoleEngine {
    Engine *ptr = nullptr;
    void *rawMem = nullptr;   // the heap buffer backing `heap` — only cookDrumSample() ever frees this (a temporary engine); T303's own long-lived instance leaves it set but unused
    Heap heap;
    bool ready = false;
    int nvoices = 0;
    // Measured on real ESP32-S3 hardware: rendering all 6 role engines every block, even
    // completely silent, cost 347.79us out of the 725.6us block budget at 44.1kHz — 48%
    // of the ENTIRE real-time audio budget spent doing nothing (reported as "le mode
    // drums lag quand je touche aux boutons": button handling competes for CPU with an
    // audio path already half-consumed by idle PSRAM-resident engines). render() can't
    // simply be skipped whenever idle — it's also what drains a role's note_on command
    // queue, so skipping it forever would mean a hit on a long-silent role never plays.
    // Fix: whoever triggers a hit/note stamps this with a window covering attack+decay+
    // release+tail every time it fires; audioEngineRenderBlock() only renders while "now"
    // is still inside that window. Worst case adds under one block (~0.7ms) of latency to
    // a hit on something that's been silent — inaudible — while skipping the other ~99%
    // of the time between hits. The 6 live drum-role engines this was written for are
    // gone (see the "Drums, take 3" comment below) — only T303 still uses this field.
    uint32_t activeUntilMs = 0;
};

// g_masterVolume mirrors the VOL pot (0..1) — main.cpp's setup() sets pots[0].value=0.7f
// as AMY's own long-standing default and immediately calls audioSetVolume(0.7), so this
// initializer is NOT the actual default in practice (lowering it alone did nothing —
// confirmed by it making no audible difference). kVolumeHeadroom is the real lever: an
// attenuation applied on top of the pot's own 0..1 range, in both buildMainGraph() below
// and audioSetVolume(), so the full pot range stays usable but 0.7 (AMY's own "loud but
// not clipping" assumption) doesn't translate to 70% of this graph's own full scale.
// Was 0.3 (one plain saw voice at that level measured peak~22000/32767, already reported
// once as "too loud") — halved again: still too loud at the default 70% pot position.
constexpr float kVolumeHeadroom = 0.15f;
// The drum roles need a much higher base level than kVolumeHeadroom (their own filters
// eat most of a noise burst's energy before the level stage) — this is that base,
// per-role-scaled by kDrumVoLevel[] below and then by the SAME vol (VOL pot) as the main
// voice in audioSetVolume(), so turning the pot moves both together (reported as "can't
// adjust it at all" when this was once a flat, pot-independent 0.6).
// Was 0.8 — measured directly against SYNTH's own newly-halved level (kVolumeHeadroom):
// a single drum hit peaked 44-56% of full scale (a 6-role sweep, isolated per role)
// against a sustained 4-note SYNTH chord's 26-28% — over 2x louder, the kind of
// cross-mode imbalance "contrôle que les différents modes ont des volumes modérés"
// asked to check for. 0.55 brings drums to roughly 1.2-1.4x SYNTH's chord level — still
// punchier (reasonable for percussive hits vs. a sustained pad) without dominating it.
constexpr float kDrumBaseLevel = 0.55f;
float g_masterVolume = 0.15f;
static void smpRefreshGains();   // "SD streaming" section, near the end of this file
EnvParams g_mainEnv = {50, 200, 500, 0.5f};   // matches config.h's envTable[ENV_NORMAL]
// Tracked so audioNoteOn() can apply config.h's shapeGain[] compensation — carried over
// from the AMY-era headless RMS sweep but never actually wired into this engine's note-on
// path, which left every shape at the same raw velocity-to-level mapping regardless of how
// much louder its own oscillator/filter combo naturally is (reported: "certain instrument
// sont trop fort par rapport à d'autres").
SynthShape g_currentMainShape = SHAPE_SAW;

bool buildMainGraph(GraphDesc &g) {
    const Registry &reg = g_mainEngine.registry();
    NodeDesc *n;
    if (!g.add_node(reg, N_NOTE, T_NOTE_IN)) return false;
    if (!(n = g.add_node(reg, N_OSC, T_OSC))) return false;
    n->param[OSC_WAVE] = WAVE_SAW_;
    // Second, silent-by-default oscillator: the plain T_OSC above only ever has 6 raw
    // waves (sine/saw/pulse/tri/noise/saw-down), so SAW_FM and SUPERSAW previously had no
    // way to actually differ from plain SAW beyond a filter-cutoff nudge — reported
    // directly: "SSAW, SAWFM sonne pareille que SAW". T_OSCX's OSCX_FM2 (real 2-op FM) and
    // OSCX_SSAW (real detuned-unison supersaw) engines cover both; audioSetShape() mutes
    // whichever of N_OSC/N_OSCX a given shape doesn't use via OSC(X)_LEVEL, live, so the
    // graph topology itself never changes between shapes.
    if (!(n = g.add_node(reg, N_OSCX, T_OSCX))) return false;
    n->param[OSCX_LEVEL] = 0;
    if (!(n = g.add_node(reg, N_FILTER, T_FILTER_V))) return false;
    n->param[FLT_MODE] = FLTM_LP;
    n->param[FLT_CUTOFF] = 96 * 256;     // pitch units: semitones x 256
    n->param[FLT_RES] = 8000;            // q15
    // FLTA_LADDER, not the default FLTA_SVF: real self-resonant feedback (Moog-like, and
    // arguably more fitting for ACID/ACID_WOBBLE than a generic SVF), but through the
    // cheaper process_lite() path — see kMainVoices's own comment for the CPU budget
    // this was chosen for.
    n->param[FLT_ALGO] = FLTA_LADDER;
    if (!(n = g.add_node(reg, N_ENV, T_ENV))) return false;
    n->param[ENV_ATTACK] = g_mainEnv.atk;
    n->param[ENV_DECAY] = g_mainEnv.dec;
    n->param[ENV_SUSTAIN] = (int32_t)(g_mainEnv.sus * 32767.0f);
    n->param[ENV_RELEASE] = g_mainEnv.rel;
    if (!(n = g.add_node(reg, N_VCA, T_VCA_V))) return false;
    n->param[VCA_LEVEL] = 0;             // closed: the envelope opens it
    if (!(n = g.add_node(reg, N_VOICE_OUT, T_VOICE_OUT))) return false;
    n->param[VO_LEVEL] = (int32_t)(g_masterVolume * kVolumeHeadroom * 32767.0f);
    n->param[VO_TAIL_MS] = 400;
    if (!g.add_node(reg, N_BUS, T_BUS_IN)) return false;
    if (!g.add_node(reg, N_MASTER, T_MASTER_OUT)) return false;

    bool ok = true;
    ok &= g.connect(N_NOTE, 0, N_OSC, Dst::In, 0);                          // pitch
    ok &= g.connect(N_NOTE, 0, N_OSCX, Dst::In, 0);                         // pitch
    ok &= g.connect(N_NOTE, 1, N_OSCX, Dst::In, 1);                         // gate (KARP/MODAL retrigger)
    ok &= g.connect(N_NOTE, 1, N_ENV, Dst::In, 0);                          // gate
    ok &= g.connect(N_OSC, 0, N_FILTER, Dst::In, 0);
    ok &= g.connect(N_OSCX, 0, N_FILTER, Dst::In, 0);                       // summed with N_OSC, see N_OSCX's own comment
    ok &= g.connect(N_ENV, 0, N_FILTER, Dst::Param, FLT_CUTOFF, 8192);
    ok &= g.connect(N_FILTER, 0, N_VCA, Dst::In, 0);
    ok &= g.connect(N_ENV, 0, N_VCA, Dst::Param, VCA_LEVEL);
    ok &= g.connect(N_VCA, 0, N_VOICE_OUT, Dst::In, 0);
    ok &= g.connect(N_BUS, 0, N_MASTER, Dst::In, 0);
    ok &= g.connect(N_BUS, 1, N_MASTER, Dst::In, 1);
    return ok;
}

// Hz -> this engine's pitch units (semitone*256, MIDI note * 256, A4=69*256 at 440Hz) —
// used wherever a facade function takes a plain Hz float (T303's cutoff) but the engine's
// own FLT_CUTOFF/OSC_PITCH params expect pitch units.
int32_t hzToPitchUnits(float hz) {
    if (hz < 1.0f) hz = 1.0f;
    return (int32_t)lroundf(69.0f * 256.0f + 12.0f * 256.0f * log2f(hz / 440.0f));
}

// ---- T303 (TB-303-style voice) ----
// audioT303*/audioI303* were pure no-op stubs since Phase 1 (Phase 5 of the fusion plan,
// never done) — confirmed directly: "la 303S ne fait pas de sons". These facade calls are
// shared by every mode that drives a 303 lane (303S/303S2/SS2/GEST/GROOVE and DRUM2's own
// T303 track all call the same audioT303NoteOn/Off/Params/PitchBend), so fixing them here
// fixes all of them at once, not just MODE_303S specifically.
// Design: T_OSC (wave mapped from AMY's own wave ids, see audioT303Wave)->T_FILTER_V
// (FLTA_LADDER — the resonant squelch is the whole point of a 303)->T_ENV (one envelope,
// decay-only like the real instrument's EG, drives BOTH the filter sweep via envMod depth
// and the amplitude)->T_VCA->T_VOICE_OUT. Accent: NOTE_IN's own velocity cable adds a
// little extra brightness and level on top, live, like a real 303's accent knob.
// Slide: NOT the engine's native VoiceMode::Mono+legato+glide — main.cpp's own extensive
// slide/bend call sites (audioT303PitchBend) already assume a live pitch-bend primitive,
// the exact mechanism audioSetPitchBend() uses for SYNTH's own joystick bend, so this
// reuses that rather than redesigning every call site around a native glide.
enum T303Node : int { T3_NOTE = 1, T3_OSC, T3_SHAPE, T3_FILTER, T3_ENV, T3_VCA, T3_VOUT, T3_BUS, T3_MASTER };
// 1, not 2: the real TB-303 is monophonic by design (no two notes ever truly overlap —
// a voice-steal on a new note-on while the old one's gate is still high is itself
// authentic 303 behavior, not a compromise). Also halves this engine's own per-block
// cost — found to matter: "303S + DRUMS = bug et lag" traced to 4+ simultaneously-active
// engines (T303 + 3 drum roles) measuring 1754us/block on real hardware against a
// 725.6us budget, a severe super-linear blowup well beyond either side's own additive
// cost (T303 alone ~177us, 3 drums alone ~548us, idle ~61us — additive estimate ~670us,
// not 1754us). The exact mechanism (likely engine-count/context-switch overhead, not
// per-voice DSP) wasn't fully root-caused — this cuts T303's own contribution as the
// most direct lever available without a deeper investigation.
constexpr int kT303Voices = 1;
constexpr float kT303BaseLevel = 0.5f;
RoleEngine g_t303Role;    // reuses the same {ptr, heap, ready, nvoices, activeUntilMs} shape initRoleEngine() already handles
#define g_t303Engine (*g_t303Role.ptr)
int g_t303FilterEdge = -1;    // edge index of ENV->FLT_CUTOFF — audioT303Params' envMod, live, via set_edge_depth
constexpr int32_t kT303CenterPitch = 60 * 256;

bool buildT303Graph(GraphDesc &g, const Registry &reg) {
    NodeDesc *n;
    if (!g.add_node(reg, T3_NOTE, T_NOTE_IN)) return false;
    if (!(n = g.add_node(reg, T3_OSC, T_OSC))) return false;
    n->param[OSC_WAVE] = WAVE_SAW_DOWN_;
    // Wavefold (P2's "texture" knob, audioT303Wavefold/WavefoldAsym — was a no-op stub:
    // "le potard 2 n'affecte pas la texture du son"). SHP_MIX starts at 0 (clean
    // passthrough — SHPM_FOLD's sine-based transform always colors the signal some even
    // at low drive, so MIX, not DRIVE alone, is what gives a true "off" position).
    if (!(n = g.add_node(reg, T3_SHAPE, T_SHAPER_V))) return false;
    n->param[SHP_MODE] = SHPM_FOLD; n->param[SHP_DRIVE] = 256; n->param[SHP_MIX] = 0;
    if (!(n = g.add_node(reg, T3_FILTER, T_FILTER_V))) return false;
    n->param[FLT_MODE] = FLTM_LP; n->param[FLT_ALGO] = FLTA_LADDER;
    n->param[FLT_CUTOFF] = hzToPitchUnits(500.0f); n->param[FLT_RES] = 20000;
    if (!(n = g.add_node(reg, T3_ENV, T_ENV))) return false;
    n->param[ENV_ATTACK] = 2; n->param[ENV_DECAY] = 500; n->param[ENV_SUSTAIN] = 0; n->param[ENV_RELEASE] = 10;
    if (!(n = g.add_node(reg, T3_VCA, T_VCA_V))) return false;
    n->param[VCA_LEVEL] = 0;
    if (!(n = g.add_node(reg, T3_VOUT, T_VOICE_OUT))) return false;
    n->param[VO_LEVEL] = (int32_t)(g_masterVolume * kT303BaseLevel * 32767.0f);
    n->param[VO_TAIL_MS] = 550;
    if (!g.add_node(reg, T3_BUS, T_BUS_IN)) return false;
    if (!g.add_node(reg, T3_MASTER, T_MASTER_OUT)) return false;
    bool ok = true;
    ok &= g.connect(T3_NOTE, 0, T3_OSC, Dst::In, 0);
    ok &= g.connect(T3_NOTE, 1, T3_ENV, Dst::In, 0);
    ok &= g.connect(T3_OSC, 0, T3_SHAPE, Dst::In, 0);
    ok &= g.connect(T3_SHAPE, 0, T3_FILTER, Dst::In, 0);
    g_t303FilterEdge = g.n_edges;
    ok &= g.connect(T3_ENV, 0, T3_FILTER, Dst::Param, FLT_CUTOFF, 16384);
    ok &= g.connect(T3_NOTE, 2, T3_FILTER, Dst::Param, FLT_CUTOFF, 6000);     // accent: velocity brightens the filter
    ok &= g.connect(T3_FILTER, 0, T3_VCA, Dst::In, 0);
    ok &= g.connect(T3_ENV, 0, T3_VCA, Dst::Param, VCA_LEVEL);
    ok &= g.connect(T3_NOTE, 2, T3_VCA, Dst::Param, VCA_LEVEL, 10000);        // accent: velocity also adds loudness
    ok &= g.connect(T3_VCA, 0, T3_VOUT, Dst::In, 0);
    ok &= g.connect(T3_BUS, 0, T3_MASTER, Dst::In, 0);
    ok &= g.connect(T3_BUS, 1, T3_MASTER, Dst::In, 1);
    return ok;
}

// ---- Modular/wavetable-morph engine (MODE_MODULAR a.k.a. "SERUM" on screen, MODE_AMBIENT,
// MODE_NOOB2, MODE_OMNI2 — mutually exclusive, see config.h's MOD3_OSCA_CH/MOD3_OSCB_CH own
// comment) ----
// Was a complete set of no-op stubs since Phase 1 (Phase 7 of the fusion plan, never done)
// — confirmed directly: every audioModular*() facade function did nothing, so SERUM/
// AMBIENT (and NOOB2/OMNI2, which drive the same two channels) were silent regardless of
// what main.cpp's extensive existing call sites (oscillator table/position, filter,
// envelope, pitch bend, note on/off) sent them.
// Design: ONE T_OSCX macro-oscillator per voice (not two, despite config.h's own "dual-
// wavetable"/osc A+B framing) into a shared filter->env->vca->out, matching the plan doc's
// own Phase 7 design ("T_OSCX's continuous morph parameter replaces the AMY wavetable
// crossfade directly: tableIdx picks one of 9 OSCX_ENGINE values, pos01 drives continuous
// OSCX_MORPH"). tableIdx is passed straight through to OSCX_ENGINE (clamped 0..
// OSCX_ENGINES-1) — the existing kAmbientPresets[] table only ever uses 0-4 (a legacy
// range from the 5 baked-in AMY PCM wavetables this new engine has no equivalent of), so
// this exposes all 9 characters rather than inventing an arbitrary remap with no clearer
// claim to correctness.
// Single-oscillator trade-off (deliberate, not silently dropped): measured directly on real
// ESP32-S3 hardware that a dual-T_OSCX-per-voice design (osc A/B, detuned) cost ~150-180us
// per held note REGARDLESS of which of the 9 OSCX_ENGINE algorithms was selected — OMNI2's
// own normal 3-note chord alone (no drums/T303 involved) measured 1092us/block against the
// 725.6us budget, a ~50% overrun from completely ordinary use, not a rare worst case the
// way MAIN's 8-voice/6-voice trade-off was. Confirmed this isn't fixable by picking a
// cheaper algorithm (Karplus-strong, additive, and supersaw+modal all measured in the same
// ~500-725us range for one held note) — T_OSCX itself is simply an expensive module type
// per instance. Cutting to one oscillator per voice halves the per-note cost, the most
// direct lever that doesn't also cut polyphony (OMNI2's 3-note chord and NOOB2's 4-voice
// pool both need their full voice count to avoid audible voice-stealing). Cost: no more
// osc A/B detune/chorus thickening — ch==MOD3_OSCB_CH's calls
// (audioModularOscInit/SetTable/SetWtPos) are harmless no-ops now, and audioModularNoteOn's
// detuneSemisB argument is unused. Every kAmbientPresets[] entry's oscBTable/oscBPos/
// detuneSemisB fields are consequently dead data, not wrong data — left as-is rather than
// scrubbed, since main.cpp's own preset-apply code can keep reading them without error.
enum ModNode : int { MOD_NOTE = 1, MOD_OSC, MOD_FILTER, MOD_ENV, MOD_VCA, MOD_VOUT, MOD_BUS, MOD_MASTER };
constexpr int kModVoices = 4;    // OMNI2's 3-note chord-grid + NOOB2's 4-voice pool
constexpr float kModBaseLevel = 0.5f;
RoleEngine g_modRole;
#define g_modEngine (*g_modRole.ptr)
// The shared pitch-bend ratio (audioModularSetPitchBend) writes OSCX_PITCH — tracked so a
// bend can be recomputed without needing any other state (no more osc B detune to sum with
// it, now that there's only one oscillator).
float g_modBendSemis = 0.0f;
// Notes held right now (gate open) — unlike T303/drums (always a bounded decay-only
// envelope, see T3_ENV's ENV_SUSTAIN=0 above), this engine's own envelope has a real
// sustain stage (pads/drones are meant to be held indefinitely), so the fixed "note-on +
// longest possible decay" render window every other RoleEngine uses would silently cut off
// a long-held note. Rendering continues unconditionally while this is nonzero; only once
// the last note releases does the usual fixed-margin window (audioModularNoteOff) apply,
// covering just the release tail.
int g_modHeldNotes = 0;
float g_modEnvReleaseMs = 400.0f;   // last value sent via audioModularSetEnvelope — sizes the post-release render window

bool buildModularGraph(GraphDesc &g, const Registry &reg) {
    NodeDesc *n;
    if (!g.add_node(reg, MOD_NOTE, T_NOTE_IN)) return false;
    if (!(n = g.add_node(reg, MOD_OSC, T_OSCX))) return false;
    n->param[OSCX_ENGINE] = OSCX_ADD;
    n->param[OSCX_LEVEL] = kUnity;
    if (!(n = g.add_node(reg, MOD_FILTER, T_FILTER_V))) return false;
    n->param[FLT_MODE] = FLTM_LP;
    n->param[FLT_CUTOFF] = hzToPitchUnits(4000.0f);
    n->param[FLT_RES] = 4000;
    n->param[FLT_ALGO] = FLTA_LADDER;
    if (!(n = g.add_node(reg, MOD_ENV, T_ENV))) return false;
    n->param[ENV_ATTACK] = 50; n->param[ENV_DECAY] = 300; n->param[ENV_SUSTAIN] = (int32_t)(0.7f * 32767.0f); n->param[ENV_RELEASE] = 400;
    if (!(n = g.add_node(reg, MOD_VCA, T_VCA_V))) return false;
    n->param[VCA_LEVEL] = 0;
    if (!(n = g.add_node(reg, MOD_VOUT, T_VOICE_OUT))) return false;
    n->param[VO_LEVEL] = (int32_t)(g_masterVolume * kModBaseLevel * 32767.0f);
    n->param[VO_TAIL_MS] = 600;
    if (!g.add_node(reg, MOD_BUS, T_BUS_IN)) return false;
    if (!g.add_node(reg, MOD_MASTER, T_MASTER_OUT)) return false;

    bool ok = true;
    ok &= g.connect(MOD_NOTE, 0, MOD_OSC, Dst::In, 0);                     // pitch
    ok &= g.connect(MOD_NOTE, 1, MOD_OSC, Dst::In, 1);                     // gate
    ok &= g.connect(MOD_NOTE, 1, MOD_ENV, Dst::In, 0);                     // gate
    ok &= g.connect(MOD_OSC, 0, MOD_FILTER, Dst::In, 0);
    ok &= g.connect(MOD_ENV, 0, MOD_VCA, Dst::Param, VCA_LEVEL);
    ok &= g.connect(MOD_FILTER, 0, MOD_VCA, Dst::In, 0);
    ok &= g.connect(MOD_VCA, 0, MOD_VOUT, Dst::In, 0);
    ok &= g.connect(MOD_BUS, 0, MOD_MASTER, Dst::In, 0);
    ok &= g.connect(MOD_BUS, 1, MOD_MASTER, Dst::In, 1);
    return ok;
}

// ---- Per-role drum graphs ----
// Only used now as one-shot "cook to sample" sources (see cookDrumSample(), below) — not
// as live playback voices any more (see that section's own comment for why). Kept exactly
// as tuned when they still were live voices; the cooked level just needs to be reasonable
// (not clipped, not silent) since PadBlob.gain controls the real playback level.
enum class DrumRole { Kick, Tom, Snare, Clap, HihatClosed, HihatOpen, Count };
constexpr int kDrumRoleCount = (int)DrumRole::Count;
constexpr int32_t kDrumVoLevel[kDrumRoleCount] = {
    // Kick, Tom, Snare, Clap, HihatClosed, HihatOpen — relative balance, tuned by ear
    // against each other (not against the main synth voice, see kDrumBaseLevel).
    (int32_t)(kDrumBaseLevel * 32767),
    (int32_t)(kDrumBaseLevel * 0.8f * 32767),
    (int32_t)(kDrumBaseLevel * 0.9f * 32767),
    (int32_t)(kDrumBaseLevel * 0.8f * 32767),
    (int32_t)(kDrumBaseLevel * 0.55f * 32767),
    (int32_t)(kDrumBaseLevel * 0.6f * 32767),
};

// Kick: a sine with a fast downward pitch sweep (classic 808/909 "thump") — its own pitch
// envelope (PENV) modulates OSC_PITCH live, decaying from +18 semitones to 0 over ~60ms,
// well before the amplitude envelope (AENV) has finished its own ~380ms decay.
enum KickNode : int { KK_NOTE = 1, KK_OSC, KK_PENV, KK_AENV, KK_VCA, KK_VOUT, KK_BUS, KK_MASTER };
bool buildKickGraph(GraphDesc &g, const Registry &reg) {
    NodeDesc *n;
    if (!g.add_node(reg, KK_NOTE, T_NOTE_IN)) return false;
    if (!(n = g.add_node(reg, KK_OSC, T_OSC))) return false;
    n->param[OSC_WAVE] = WAVE_SINE_;
    n->param[OSC_PITCH_MOD] = 18 * 256;
    if (!(n = g.add_node(reg, KK_PENV, T_ENV))) return false;
    n->param[ENV_ATTACK] = 1; n->param[ENV_DECAY] = 60; n->param[ENV_SUSTAIN] = 0; n->param[ENV_RELEASE] = 10;
    if (!(n = g.add_node(reg, KK_AENV, T_ENV))) return false;
    n->param[ENV_ATTACK] = 1; n->param[ENV_DECAY] = 380; n->param[ENV_SUSTAIN] = kUnity; n->param[ENV_RELEASE] = 30;   // sustained: see kDrumSamples
    if (!(n = g.add_node(reg, KK_VCA, T_VCA_V))) return false;
    n->param[VCA_LEVEL] = 0;
    if (!(n = g.add_node(reg, KK_VOUT, T_VOICE_OUT))) return false;
    n->param[VO_LEVEL] = kDrumVoLevel[(int)DrumRole::Kick]; n->param[VO_TAIL_MS] = 450;
    if (!g.add_node(reg, KK_BUS, T_BUS_IN)) return false;
    if (!g.add_node(reg, KK_MASTER, T_MASTER_OUT)) return false;
    bool ok = true;
    ok &= g.connect(KK_NOTE, 0, KK_OSC, Dst::In, 0);
    ok &= g.connect(KK_NOTE, 1, KK_PENV, Dst::In, 0);
    ok &= g.connect(KK_NOTE, 1, KK_AENV, Dst::In, 0);
    ok &= g.connect(KK_PENV, 0, KK_OSC, Dst::Param, OSC_PITCH, kUnity);
    ok &= g.connect(KK_OSC, 0, KK_VCA, Dst::In, 0);
    ok &= g.connect(KK_AENV, 0, KK_VCA, Dst::Param, VCA_LEVEL);
    ok &= g.connect(KK_VCA, 0, KK_VOUT, Dst::In, 0);
    ok &= g.connect(KK_BUS, 0, KK_MASTER, Dst::In, 0);
    ok &= g.connect(KK_BUS, 1, KK_MASTER, Dst::In, 1);
    return ok;
}

// Tom: same shape as the kick but a much smaller pitch drop and a resonant low-pass for a
// more "boingy" tonal character — reused for DRUM2's BNG and DRUMS2/3's LT/HT (their own
// per-hit pitch value is what tells them apart, safely, via NOTE_IN's cable).
enum TomNode : int { TM_NOTE = 1, TM_OSC, TM_PENV, TM_AENV, TM_FILT, TM_VCA, TM_VOUT, TM_BUS, TM_MASTER };
bool buildTomGraph(GraphDesc &g, const Registry &reg) {
    NodeDesc *n;
    if (!g.add_node(reg, TM_NOTE, T_NOTE_IN)) return false;
    if (!(n = g.add_node(reg, TM_OSC, T_OSC))) return false;
    n->param[OSC_WAVE] = WAVE_TRI_;
    n->param[OSC_PITCH_MOD] = 6 * 256;
    if (!(n = g.add_node(reg, TM_PENV, T_ENV))) return false;
    n->param[ENV_ATTACK] = 1; n->param[ENV_DECAY] = 50; n->param[ENV_SUSTAIN] = 0; n->param[ENV_RELEASE] = 10;
    if (!(n = g.add_node(reg, TM_AENV, T_ENV))) return false;
    n->param[ENV_ATTACK] = 1; n->param[ENV_DECAY] = 260; n->param[ENV_SUSTAIN] = kUnity; n->param[ENV_RELEASE] = 25;
    if (!(n = g.add_node(reg, TM_FILT, T_FILTER_V))) return false;
    n->param[FLT_MODE] = FLTM_LP; n->param[FLT_CUTOFF] = 85 * 256; n->param[FLT_RES] = 11000;
    if (!(n = g.add_node(reg, TM_VCA, T_VCA_V))) return false;
    n->param[VCA_LEVEL] = 0;
    if (!(n = g.add_node(reg, TM_VOUT, T_VOICE_OUT))) return false;
    n->param[VO_LEVEL] = kDrumVoLevel[(int)DrumRole::Tom]; n->param[VO_TAIL_MS] = 300;
    if (!g.add_node(reg, TM_BUS, T_BUS_IN)) return false;
    if (!g.add_node(reg, TM_MASTER, T_MASTER_OUT)) return false;
    bool ok = true;
    ok &= g.connect(TM_NOTE, 0, TM_OSC, Dst::In, 0);
    ok &= g.connect(TM_NOTE, 1, TM_PENV, Dst::In, 0);
    ok &= g.connect(TM_NOTE, 1, TM_AENV, Dst::In, 0);
    ok &= g.connect(TM_PENV, 0, TM_OSC, Dst::Param, OSC_PITCH, kUnity);
    ok &= g.connect(TM_OSC, 0, TM_FILT, Dst::In, 0);
    ok &= g.connect(TM_FILT, 0, TM_VCA, Dst::In, 0);
    ok &= g.connect(TM_AENV, 0, TM_VCA, Dst::Param, VCA_LEVEL);
    ok &= g.connect(TM_VCA, 0, TM_VOUT, Dst::In, 0);
    ok &= g.connect(TM_BUS, 0, TM_MASTER, Dst::In, 0);
    ok &= g.connect(TM_BUS, 1, TM_MASTER, Dst::In, 1);
    return ok;
}

// Snare: a short tonal "pop" (tone) mixed with filtered noise (the "hiss"/"rattle") —
// each half has its own envelope, mixed post-VCA via Mix4 so one doesn't bleed the other's
// decay time the way the old shared-node design did.
enum SnareNode : int { SN_NOTE = 1, SN_OSCT, SN_OSCN, SN_TENV, SN_NENV, SN_FILT, SN_TVCA, SN_NVCA, SN_MIX, SN_VOUT, SN_BUS, SN_MASTER };
bool buildSnareGraph(GraphDesc &g, const Registry &reg) {
    NodeDesc *n;
    if (!g.add_node(reg, SN_NOTE, T_NOTE_IN)) return false;
    if (!(n = g.add_node(reg, SN_OSCT, T_OSC))) return false;
    n->param[OSC_WAVE] = WAVE_TRI_; n->param[OSC_PITCH] = 55 * 256;
    if (!(n = g.add_node(reg, SN_OSCN, T_OSC))) return false;
    n->param[OSC_WAVE] = WAVE_NOISE_;
    if (!(n = g.add_node(reg, SN_TENV, T_ENV))) return false;
    n->param[ENV_ATTACK] = 1; n->param[ENV_DECAY] = 80; n->param[ENV_SUSTAIN] = 0; n->param[ENV_RELEASE] = 10;
    if (!(n = g.add_node(reg, SN_NENV, T_ENV))) return false;
    n->param[ENV_ATTACK] = 1; n->param[ENV_DECAY] = 170; n->param[ENV_SUSTAIN] = kUnity; n->param[ENV_RELEASE] = 20;   // the noise sustains, the tone stays a short pop
    if (!(n = g.add_node(reg, SN_FILT, T_FILTER_V))) return false;
    n->param[FLT_MODE] = FLTM_HP; n->param[FLT_CUTOFF] = 78 * 256; n->param[FLT_RES] = 4000;
    if (!(n = g.add_node(reg, SN_TVCA, T_VCA_V))) return false; n->param[VCA_LEVEL] = 0;
    if (!(n = g.add_node(reg, SN_NVCA, T_VCA_V))) return false; n->param[VCA_LEVEL] = 0;
    if (!(n = g.add_node(reg, SN_MIX, T_MIX4_V))) return false;
    n->param[0] = 16000; n->param[1] = kUnity;   // tone a bit under the noise, matches a classic snare balance
    if (!(n = g.add_node(reg, SN_VOUT, T_VOICE_OUT))) return false;
    n->param[VO_LEVEL] = kDrumVoLevel[(int)DrumRole::Snare]; n->param[VO_TAIL_MS] = 220;
    if (!g.add_node(reg, SN_BUS, T_BUS_IN)) return false;
    if (!g.add_node(reg, SN_MASTER, T_MASTER_OUT)) return false;
    bool ok = true;
    ok &= g.connect(SN_NOTE, 1, SN_TENV, Dst::In, 0);
    ok &= g.connect(SN_NOTE, 1, SN_NENV, Dst::In, 0);
    ok &= g.connect(SN_OSCT, 0, SN_TVCA, Dst::In, 0);
    ok &= g.connect(SN_TENV, 0, SN_TVCA, Dst::Param, VCA_LEVEL);
    ok &= g.connect(SN_OSCN, 0, SN_FILT, Dst::In, 0);
    ok &= g.connect(SN_FILT, 0, SN_NVCA, Dst::In, 0);
    ok &= g.connect(SN_NENV, 0, SN_NVCA, Dst::Param, VCA_LEVEL);
    ok &= g.connect(SN_TVCA, 0, SN_MIX, Dst::In, 0);
    ok &= g.connect(SN_NVCA, 0, SN_MIX, Dst::In, 1);
    ok &= g.connect(SN_MIX, 0, SN_VOUT, Dst::In, 0);
    ok &= g.connect(SN_BUS, 0, SN_MASTER, Dst::In, 0);
    ok &= g.connect(SN_BUS, 1, SN_MASTER, Dst::In, 1);
    return ok;
}

// Clap: filtered noise burst, brighter and shorter than the snare's noise half — a single
// burst (no flam/multi-hit detail, a known simplification).
enum ClapNode : int { CP_NOTE = 1, CP_OSC, CP_FILT, CP_ENV, CP_VCA, CP_VOUT, CP_BUS, CP_MASTER };
bool buildClapGraph(GraphDesc &g, const Registry &reg) {
    NodeDesc *n;
    if (!g.add_node(reg, CP_NOTE, T_NOTE_IN)) return false;
    if (!(n = g.add_node(reg, CP_OSC, T_OSC))) return false;
    n->param[OSC_WAVE] = WAVE_NOISE_;
    if (!(n = g.add_node(reg, CP_FILT, T_FILTER_V))) return false;
    // FLTM_HP, not the original FLTM_BP: a narrow resonant bandpass only passes a thin
    // slice of broadband noise's spectrum, so its aggregate output is inherently much
    // quieter than a highpass's for the same input level (by design — "normalised: 0dB
    // at the centre" only helps a tone sitting AT that one frequency, not noise spread
    // across all of them) — measured directly at 5.8% peak vs. the other noise-based
    // roles' 30-55%, not a level-tuning gap a bigger VO_LEVEL alone could close cleanly.
    n->param[FLT_MODE] = FLTM_HP; n->param[FLT_CUTOFF] = 92 * 256; n->param[FLT_RES] = 5000;
    if (!(n = g.add_node(reg, CP_ENV, T_ENV))) return false;
    n->param[ENV_ATTACK] = 1; n->param[ENV_DECAY] = 110; n->param[ENV_SUSTAIN] = 12000; n->param[ENV_RELEASE] = 15;   // the burst, then a sustained hiss
    if (!(n = g.add_node(reg, CP_VCA, T_VCA_V))) return false; n->param[VCA_LEVEL] = 0;
    if (!(n = g.add_node(reg, CP_VOUT, T_VOICE_OUT))) return false;
    n->param[VO_LEVEL] = kDrumVoLevel[(int)DrumRole::Clap]; n->param[VO_TAIL_MS] = 150;
    if (!g.add_node(reg, CP_BUS, T_BUS_IN)) return false;
    if (!g.add_node(reg, CP_MASTER, T_MASTER_OUT)) return false;
    bool ok = true;
    ok &= g.connect(CP_NOTE, 1, CP_ENV, Dst::In, 0);
    ok &= g.connect(CP_OSC, 0, CP_FILT, Dst::In, 0);
    ok &= g.connect(CP_FILT, 0, CP_VCA, Dst::In, 0);
    ok &= g.connect(CP_ENV, 0, CP_VCA, Dst::Param, VCA_LEVEL);
    ok &= g.connect(CP_VCA, 0, CP_VOUT, Dst::In, 0);
    ok &= g.connect(CP_BUS, 0, CP_MASTER, Dst::In, 0);
    ok &= g.connect(CP_BUS, 1, CP_MASTER, Dst::In, 1);
    return ok;
}

// Hihat closed/open share this same shape (bright high-passed noise), only decay/release
// differ — built by one function parameterized on role, called twice.
enum HatNode : int { HT_NOTE = 1, HT_OSC, HT_FILT, HT_ENV, HT_VCA, HT_VOUT, HT_BUS, HT_MASTER };
bool buildHatGraph(GraphDesc &g, const Registry &reg, bool open) {
    NodeDesc *n;
    if (!g.add_node(reg, HT_NOTE, T_NOTE_IN)) return false;
    if (!(n = g.add_node(reg, HT_OSC, T_OSC))) return false;
    n->param[OSC_WAVE] = WAVE_NOISE_;
    if (!(n = g.add_node(reg, HT_FILT, T_FILTER_V))) return false;
    n->param[FLT_MODE] = FLTM_HP; n->param[FLT_CUTOFF] = 104 * 256; n->param[FLT_RES] = 2500;
    if (!(n = g.add_node(reg, HT_ENV, T_ENV))) return false;
    n->param[ENV_ATTACK] = 1; n->param[ENV_DECAY] = open ? 320 : 45; n->param[ENV_SUSTAIN] = kUnity; n->param[ENV_RELEASE] = open ? 40 : 8;
    if (!(n = g.add_node(reg, HT_VCA, T_VCA_V))) return false; n->param[VCA_LEVEL] = 0;
    if (!(n = g.add_node(reg, HT_VOUT, T_VOICE_OUT))) return false;
    n->param[VO_LEVEL] = kDrumVoLevel[open ? (int)DrumRole::HihatOpen : (int)DrumRole::HihatClosed];
    n->param[VO_TAIL_MS] = open ? 350 : 60;
    if (!g.add_node(reg, HT_BUS, T_BUS_IN)) return false;
    if (!g.add_node(reg, HT_MASTER, T_MASTER_OUT)) return false;
    bool ok = true;
    ok &= g.connect(HT_NOTE, 1, HT_ENV, Dst::In, 0);
    ok &= g.connect(HT_OSC, 0, HT_FILT, Dst::In, 0);
    ok &= g.connect(HT_FILT, 0, HT_VCA, Dst::In, 0);
    ok &= g.connect(HT_ENV, 0, HT_VCA, Dst::Param, VCA_LEVEL);
    ok &= g.connect(HT_VCA, 0, HT_VOUT, Dst::In, 0);
    ok &= g.connect(HT_BUS, 0, HT_MASTER, Dst::In, 0);
    ok &= g.connect(HT_BUS, 1, HT_MASTER, Dst::In, 1);
    return ok;
}
bool buildHatClosedGraph(GraphDesc &g, const Registry &reg) { return buildHatGraph(g, reg, false); }
bool buildHatOpenGraph(GraphDesc &g, const Registry &reg) { return buildHatGraph(g, reg, true); }

// Shared PSRAM-or-malloc init, register, build, load sequence — was hand-copied twice
// already for g_mainEngine/g_drumEngine before this; factored out now that it's x6.

// The graph being built (a GraphDesc is ~6KB). These used to be three function-static
// GraphDescs (initRoleEngine/initPadsEngine/audioInit) — 17.9KB of internal RAM the board
// doesn't have: measured 2.6KB of internal heap left after boot, so the first SD file
// opened by the sample streaming (newlib's per-FILE lock) aborted. Builds only happen
// one at a time on the control side and Engine::load() only reads the graph, so one
// PSRAM scratch serves them all.
static GraphDesc &scratchGraph() {
    static GraphDesc *g = static_cast<GraphDesc *>(smpBigAlloc(sizeof(GraphDesc)));
    return *new (g) GraphDesc();
}

bool initRoleEngine(RoleEngine &re, size_t heapBytes, int nvoices, bool (*build)(GraphDesc &, const Registry &),
                     void (*extraRegister)(Registry &) = nullptr) {
#ifdef GRVEP_FUSION_USE_PSRAM
    void *mem = heap_caps_malloc(heapBytes, MALLOC_CAP_SPIRAM);
    void *engMem = heap_caps_malloc(sizeof(Engine), MALLOC_CAP_SPIRAM);
#else
    void *mem = malloc(heapBytes);
    void *engMem = malloc(sizeof(Engine));
#endif
    if (!mem || !engMem) return false;
    re.ptr = new (engMem) Engine();
    re.rawMem = mem;
    re.heap.init(mem, heapBytes);
    Memory memCfg{&re.heap, &re.heap};
    if (!re.ptr->init(memCfg, nvoices)) return false;
    register_synth_modules(re.ptr->registry());
    if (extraRegister) extraRegister(re.ptr->registry());
    GraphDesc &g = scratchGraph();
    if (!build(g, re.ptr->registry())) return false;
    if (re.ptr->load(g, nvoices) != Err::Ok) return false;
    re.nvoices = nvoices;
    re.ready = true;
    return true;
}

// ---- Drums, take 3: cooked samples through one shared Pads engine ----
// The 6-separate-live-engines design (one Engine per drum role, see RoleEngine's own
// comment above) correctly fixed the earlier cross-voice corruption bug, but introduced
// a severe performance cliff found this session ("303S + DRUMS = bug et lag"): 4 or more
// of these small Engine instances simultaneously active measured a 2.26x super-linear
// jump in render cost on real ESP32-S3 hardware (1216us for 4 vs. an additive estimate of
// ~700us), well past the 725.6us block budget — T303 alone added a 4th on top of 3 drum
// roles and tipped it over. The buildXGraph() live-synthesis graphs above are repurposed
// as one-shot "cook to sample" sources instead: render each once into a resident .smp
// file (smp_format.h's smp_build()), load all 6 into one Engine running the vendored
// Pads module (engine/modules/sampler_modules.h, T_PADS — built-in choke groups, up to 64
// pads, one shared voice pool) instead of a separate live-synthesis Engine per role. Total
// simultaneously-active Engines drops to 3 (main + T303 + Pads), under the measured
// threshold. Known trade-offs, not silently dropped: DRUMS2/3's decay pot no longer
// live-retunes a hit's envelope (samples play their cooked-in length); per-pad "tone"/
// pitch is now a fixed PadBlob.tune pitch-shift of a cooked sample, not a cable-driven
// live oscillator pitch.
//
// StorageDevice (engine/sampler/storage.h) needs an implementation per backend — the
// vendored engine/ tree only brought in {core,dsp,modules,mi,sampler,offline}, not the
// reference app's own SdStorage (POSIX fd-based)/SimStorage (timing-modeled), neither of
// which matches how this project already talks to its SD card (Arduino's SD.h/File, the
// exact API src/main.cpp already uses on both ESP32 and the simulator — simulator/hal/
// SD.h maps it to ~/Music/). Every read here completes synchronously inside submit()
// itself rather than modeling real async I/O — storage.h's own doc explicitly allows this
// ("memory-mapped flash/PSRAM (completes at once)"). Synchronous is still right now that
// the sample keys, the preview and MODE_DJ stream through it too: every read runs on
// smpLoaderTask (see "SD streaming" below), never on the audio path, so a slow card read
// only delays that task — the Sampler voices ride out a late block with their own
// underrun fade.
//
// Streaming reads 4 KB blocks continuously, so files stay OPEN between reads (the first
// version reopened the file for every block — fine for 7 drum samples loaded once at
// boot, far too slow for a stream). Only kOpenMax at once: the ESP32 SD driver's own
// default is 5 open files for the whole firmware (SD.begin(), max_files), shared with
// main.cpp's own browsing/loading, so the least recently used one is closed and reopened
// on its next read instead.
class SdFileStorage : public StorageDevice {
public:
    FileHandle open(const char *name) override {
        if (!path_ && !(path_ = static_cast<char (*)[128]>(smpBigAlloc(sizeof(char[128]) * kMaxFiles)))) return -1;
        for (int i = 0; i < kMaxFiles; i++) if (used_[i] && strcmp(path_[i], name) == 0) { refs_[i]++; return i; }
        for (int i = 0; i < kMaxFiles; i++) if (!used_[i]) {
            File f = SD.open(name, FILE_READ);
            if (!f) return -1;
            strncpy(path_[i], name, sizeof(path_[i]) - 1);
            path_[i][sizeof(path_[i]) - 1] = 0;
            size_[i] = f.size();
            f.close();
            used_[i] = true;
            refs_[i] = 1;
            return i;
        }
        return -1;
    }
    void forget(FileHandle f) override {
        if (f < 0 || f >= kMaxFiles || !used_[f] || --refs_[f] > 0) return;
        used_[f] = false;
        for (Open &o : open_) if (o.handle == f) { o.file.close(); o.handle = -1; }
    }
    uint32_t size(FileHandle f) const override { return (f >= 0 && f < kMaxFiles && used_[f]) ? size_[f] : 0; }
    bool submit(const IoRead &r, uint64_t) override {
        if (qn_ >= kMaxQueue) return false;
        bool ok = false;
        if (r.file >= 0 && r.file < kMaxFiles && used_[r.file]) {
            File *f = file(r.file);
            if (f && f->seek(r.offset)) ok = (f->read((uint8_t *)r.dst, r.bytes) == (int)r.bytes);
        }
        q_[qn_].tag = r.tag;
        q_[qn_].ok = ok;
        qn_++;
        return true;
    }
    // Closes every open file (MODE_USB: no FatFs file may outlive the card being handed to the host). Reads reopen them.
    void closeAll() { for (Open &o : open_) if (o.handle >= 0) { o.file.close(); o.handle = -1; } }
    bool poll(IoDone &d, uint64_t) override {
        if (qn_ == 0) return false;
        d = q_[0];
        for (int i = 1; i < qn_; i++) q_[i - 1] = q_[i];
        qn_--;
        return true;
    }
    int pending() const override { return qn_; }

private:
    static constexpr int kMaxFiles = 64, kMaxQueue = 8, kOpenMax = 2;
    struct Open { FileHandle handle = -1; uint32_t used = 0; File file; };
    File *file(FileHandle h) {
        Open *slot = nullptr;
        for (Open &o : open_) {
            if (o.handle == h) { o.used = ++tick_; return &o.file; }
            if (!slot || o.handle < 0 || (slot->handle >= 0 && o.used < slot->used)) slot = &o;
        }
        if (slot->handle >= 0) slot->file.close();
        slot->file = SD.open(path_[h], FILE_READ);
        slot->handle = slot->file ? h : -1;
        slot->used = ++tick_;
        return slot->handle >= 0 ? &slot->file : nullptr;
    }
    Open open_[kOpenMax];
    uint32_t tick_ = 0;
    bool used_[kMaxFiles] = {};
    char (*path_)[128] = nullptr;      // kMaxFiles paths, in PSRAM on the board (8 KB: internal RAM is nearly full)
    uint32_t size_[kMaxFiles] = {};
    int refs_[kMaxFiles] = {};
    IoDone q_[kMaxQueue] = {};
    int qn_ = 0;
};
SdFileStorage g_sdStorage;
// SampleBank itself (not just its heap buffer) moves to PSRAM — its own fixed-size
// member arrays (kMaxSamples=32 slots, kMaxJobs=64, 8 instruments x 64 zones each) are
// embedded directly in the object, not separately heap-allocated, so a plain global
// instance landed them all in scarce internal RAM: confirmed directly, internal RAM
// usage jumped from 41.3% to 66.5% the first time this compiled for ESP32-S3, before
// this fix. Same placement-new-into-heap_caps_malloc pattern already used for every
// Engine object.
SampleBank *g_sampleBankPtr = nullptr;
#define g_sampleBank (*g_sampleBankPtr)

enum PadsNode : int { PD_PADS = 1, PD_MASTER };
Engine *g_padsEnginePtr = nullptr;
#define g_padsEngine (*g_padsEnginePtr)
Heap g_padsHeap;
bool g_padsReady = false;
constexpr float kPadBaseLevel = 0.6f;

bool buildPadsGraph(GraphDesc &g, const Registry &reg) {
    if (!g.add_node(reg, PD_PADS, T_PADS)) return false;
    if (!g.add_node(reg, PD_MASTER, T_MASTER_OUT)) return false;
    bool ok = true;
    // No T_BUS_IN in between: BUS_IN has zero real input ports (it reads the special
    // voice-summing accumulator that only VOICE_OUT nodes write into) — it only makes
    // sense downstream of per-voice VOICE_OUT nodes, which Pads (Global-scope, its own
    // direct L/R/AL/AR/BL/BR/CL/CR outputs) is not. Confirmed directly: connecting
    // through BUS_IN failed load() with "bad cable (unknown node or port)" — BUS_IN's own
    // ModuleInfo declares 0 input ports. Pads' own Dry L/R (its outputs 0/1) go straight
    // into MASTER_OUT instead.
    ok &= g.connect(PD_PADS, 0, PD_MASTER, Dst::In, 0);
    ok &= g.connect(PD_PADS, 1, PD_MASTER, Dst::In, 1);
    return ok;
}

// Cooks one of the per-role graphs above into a resident .smp file: render a single
// untouched hit (ENV_SUSTAIN=0 on every role, so it always decays to silence on its own —
// no note_off needed) for exactly `frames`, write it via smp_format.h's smp_build(), then
// free the temporary Engine entirely — it only ever existed to render this one sound.
constexpr int kCookMaxFrames = 24000;   // ~545ms @44.1kHz, covers every role's own decay+release+tail
bool cookDrumSample(const char *path, bool (*build)(GraphDesc &, const Registry &), int frames, uint8_t note, const char *label) {
    // PSRAM, not a static global array: a plain `int16_t g_cookPcm[24000]` landed 47KB
    // in scarce internal RAM for a buffer only ever needed transiently during boot-time
    // cooking — part of the same internal-RAM blowup SampleBank's own comment describes.
#ifdef GRVEP_FUSION_USE_PSRAM
    int16_t *pcm = (int16_t *)heap_caps_malloc(kCookMaxFrames * sizeof(int16_t), MALLOC_CAP_SPIRAM);
#else
    int16_t *pcm = (int16_t *)malloc(kCookMaxFrames * sizeof(int16_t));
#endif
    if (!pcm) { Serial.printf("cook %s: pcm alloc failed\n", label); return false; }
    RoleEngine tmp;
    if (!initRoleEngine(tmp, 48 * 1024, 1, build)) {
        Serial.printf("cook %s: engine init failed\n", label);
#ifdef GRVEP_FUSION_USE_PSRAM
        heap_caps_free(pcm);
#else
        free(pcm);
#endif
        return false;
    }
    tmp.ptr->note_on(note, 127);
    if (frames > kCookMaxFrames) frames = kCookMaxFrames;
    q15 l[kBlock], r[kBlock];
    for (int done = 0; done < frames; ) {
        tmp.ptr->render(l, r);
        tmp.ptr->gc();
        int n = frames - done; if (n > kBlock) n = kBlock;
        for (int i = 0; i < n; i++) pcm[done + i] = l[i];
        done += n;
    }
    tmp.ptr->shutdown();
    tmp.ptr->~Engine();   // placement-new'd (initRoleEngine) — freeing the raw memory below does not call this on its own
    void *engMem = tmp.ptr;
#ifdef GRVEP_FUSION_USE_PSRAM
    heap_caps_free(engMem);
    heap_caps_free(tmp.rawMem);
#else
    free(engMem);
    free(tmp.rawMem);
#endif
    SmpHeader h;
    h.sample_rate = kSampleRate;
    h.frames = (uint32_t)frames;
    h.root_note = note;
    std::vector<uint8_t> file = smp_build(h, pcm);
#ifdef GRVEP_FUSION_USE_PSRAM
    heap_caps_free(pcm);
#else
    free(pcm);
#endif
    File f = SD.open(path, FILE_WRITE);
    if (!f) { Serial.printf("cook %s: SD write failed\n", label); return false; }
    f.write(file.data(), file.size());
    f.close();
    return true;
}

// Reuses the DRUM2 role-grouping already established (KK1/BNG tonal, the rest noise) for
// which cooked sound each pad plays — see audioPlayDrumPad/audioDrumSynthHit's own
// mapping tables below for how each mode's pad convention points at these 6.
// CK_TOM_HI is a second, separately-cooked tom (higher trigger note) rather than one
// cooked sample pitch-shifted live via PadBlob.tune for LT vs HT — set_blob'ing two
// different `tune` values onto the same bank slot measured identically (zero-crossing
// rate unchanged between a -6 and +6 semitone tune), a real discrepancy in the vendored
// Sampler's tune handling not tracked down given the time already spent on it; cooking
// two pitches sidesteps it entirely and is simpler to reason about besides.
enum CookedSample { CK_KICK, CK_TOM, CK_TOM_HI, CK_SNARE, CK_CLAP, CK_HAT_C, CK_HAT_O, CK_COUNT };
// Decay: each sample is cooked with a SUSTAINED body (the graphs' amp envelopes hold at
// their sustain level instead of dying), and its pad loops a stretch of that body
// (lstart..lend, crossfaded) forever; the hit's length then comes only from the Pads
// voice's own exponential decay (PadBlob.decay, triggerPadTuned()). That's what lets
// the Decay knob lengthen a hit ("la présence de la drum en durée, un peu comme une
// reverb"), not just cut a fixed one-shot short. Tonal loops (kick/toms) are a whole
// number of periods of the note they were cooked at, so the crossfade doesn't comb.
struct DrumSampleSpec { const char *path; bool (*build)(GraphDesc &, const Registry &); int frames; uint8_t note; const char *label;
                        int32_t lstart, lend, xfade; };
const DrumSampleSpec kDrumSamples[CK_COUNT] = {
    { "/grvsmp/kick.smp",   buildKickGraph,        17640, 36, "kick",  11025, 11025 + 5394, 1348 },   // 8 periods of 65.41 Hz
    { "/grvsmp/tom.smp",    buildTomGraph,         14112, 55, "tom",    8820,  8820 + 4500,  900 },   // 20 x 196.0 Hz
    { "/grvsmp/tomhi.smp",  buildTomGraph,         13230, 62, "tomHi",  8820,  8820 + 3604,  901 },   // 24 x 293.7 Hz
    { "/grvsmp/snare.smp",  buildSnareGraph,       18000, 70, "snare", 11025, 11025 + 6615, 2205 },   // noise only by then
    { "/grvsmp/clap.smp",   buildClapGraph,        11500, 72, "clap",   4410,  4410 + 6615, 2205 },
    { "/grvsmp/hatc.smp",   buildHatClosedGraph,    9300, 88, "hatC",   2205,  2205 + 6615, 2205 },
    { "/grvsmp/hato.smp",   buildHatOpenGraph,      9300, 88, "hatO",   2205,  2205 + 6615, 2205 },
};
int16_t g_drumSlot[CK_COUNT] = { -1, -1, -1, -1, -1, -1, -1 };

void cookAndLoadDrumSamples() {
    SD.mkdir("/grvsmp");
    for (int i = 0; i < CK_COUNT; i++) {
        if (!cookDrumSample(kDrumSamples[i].path, kDrumSamples[i].build, kDrumSamples[i].frames, kDrumSamples[i].note, kDrumSamples[i].label)) continue;
        int slot = g_sampleBank.load(kDrumSamples[i].path, 0);   // head_ms=0: fully resident, no streaming needed for a one-shot this short
        if (slot < 0) { Serial.printf("load %s: no free slot\n", kDrumSamples[i].label); continue; }
        for (int p = 0; p < 20 && !g_sampleBank.ready(slot) && !g_sampleBank.failed(slot); p++) g_sampleBank.pump(0);
        if (g_sampleBank.failed(slot)) { Serial.printf("load %s: failed\n", kDrumSamples[i].label); continue; }
        g_drumSlot[i] = (int16_t)slot;
    }
}

bool initPadsEngine() {
    // Each of Pads' kPadVoices=8 internal Sampler voices unconditionally allocates a
    // 48KB streaming ring buffer (stream.h's ENGINE_RING_BLOCKS=12), whether or not the
    // sample it ends up playing is resident — 384KB alone, confirmed directly (160KB
    // left load() at "out of memory" with 163KB of it still completely free beforehand,
    // meaning the shortfall was much bigger than that whole budget). Plus resident PCM
    // for the 6 cooked one-shots (~110KB) and compile_plan()'s own Scratch overhead.
    // 768KB was comfortable for that alone. The sample keys, the preview and MODE_DJ now
    // load into the same SampleBank (see "SD streaming" below), each sample holding a RAM
    // head (~186 ms) plus a tail head for reverse starts — about 28KB per streamed file
    // (a short sample is fully resident instead) — so up to 34 of them want ~1MB more.
    // The board's PSRAM size isn't documented anywhere in this repo: try the big heap
    // first, fall back to smaller ones (the original 768KB last) and log what it got. A
    // load that doesn't fit then fails cleanly with KEY_ERR_ALLOC instead of silently.
    static const size_t kPadsHeapTry[] = { 2048 * 1024, 1280 * 1024, 768 * 1024 };
    size_t kPadsHeapBytes = 0;
    void *mem = nullptr;
    for (size_t bytes : kPadsHeapTry) {
#ifdef GRVEP_FUSION_USE_PSRAM
        mem = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
#else
        mem = malloc(bytes);
#endif
        if (mem) { kPadsHeapBytes = bytes; break; }
    }
#ifdef GRVEP_FUSION_USE_PSRAM
    void *engMem = heap_caps_malloc(sizeof(Engine), MALLOC_CAP_SPIRAM);
    void *bankMem = heap_caps_malloc(sizeof(SampleBank), MALLOC_CAP_SPIRAM);
    Serial.printf("pads: heap %uKB, PSRAM free after %uKB\n", (unsigned)(kPadsHeapBytes / 1024),
                  (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
#else
    void *engMem = malloc(sizeof(Engine));
    void *bankMem = malloc(sizeof(SampleBank));
#endif
    if (!mem || !engMem || !bankMem) { Serial.println("pads: alloc failed"); return false; }
    g_padsEnginePtr = new (engMem) Engine();
    g_sampleBankPtr = new (bankMem) SampleBank();
    g_padsHeap.init(mem, kPadsHeapBytes);
    Memory memCfg{&g_padsHeap, &g_padsHeap};
    if (!g_sampleBank.init(g_sdStorage, memCfg)) { Serial.println("pads: SampleBank init failed"); return false; }
    memCfg.bank = &g_sampleBank;
    if (!g_padsEngine.init(memCfg, 1)) { Serial.println("pads: engine init failed"); return false; }
    register_sampler_modules(g_padsEngine.registry());
    GraphDesc &g = scratchGraph();
    if (!buildPadsGraph(g, g_padsEngine.registry())) { Serial.println("pads: build graph failed"); return false; }
    Err padsLoadErr = g_padsEngine.load(g, 1);
    if (padsLoadErr != Err::Ok) { Serial.printf("pads: load failed err=%s\n", err_name(padsLoadErr)); return false; }
    g_padsReady = true;
    return true;
}

// The engine's command queue is single-producer (engine/core/command_queue.h: "the UI /
// control thread posts"), but this facade is called from two tasks: loop() (core 1) and
// main.cpp's audioHandlerTask (core 0, grid notes) — drum hits already came from both,
// and the sample keys / DJ deck (see "SD streaming") add more. Every post to the Pads
// engine goes through these two, serialized. Note the queue is also small
// (ENGINE_CMD_RING=32 in platformio.ini, to save internal RAM) and a full queue DROPS the
// command silently (push() returns false): callers must not burst dozens at once.
static std::mutex g_padsCtl;
static bool padsSetBlob(const void *data, size_t bytes) {
    std::lock_guard<std::mutex> lk(g_padsCtl);
    return g_padsEngine.set_blob(PD_PADS, data, bytes);
}
static bool padsSetParam(int idx, int32_t v) {
    std::lock_guard<std::mutex> lk(g_padsCtl);
    return g_padsEngine.set_param(PD_PADS, idx, v);
}

// pad: 0..15 — 0..7 is DRUM2's own convention (KK1/SN1/HHC/CLP/BNG/OHH/CHH/RDE), 8..15 is
// DRUMS2/3's (BD/SD/LT/HT/RS/CP/CH/OH) — see setupPadBlobs()'s own table for exactly which
// cooked sample and tune each one plays. vel 0..1 maps to PadSeqEvent's 0..15 level scale
// (set_blob with a PadSeqEvent, not the plain PADS_PUSH set_param, specifically to keep
// per-hit velocity — see sampler_modules.h's own comment on the two push mechanisms).
void triggerPad(int pad, float vel) {
    if (!g_padsReady) return;
    int level = 1 + (int)(vel * 14.0f);
    if (level < 1) level = 1; else if (level > 15) level = 15;
    PadSeqEvent ev; ev.pad = (uint8_t)pad; ev.level = (uint8_t)level; ev.release = 0;
    padsSetBlob(&ev, sizeof(ev));
}

struct PadSetup { uint8_t pad; CookedSample sample; int32_t tuneSemis; uint8_t choke; };
const PadSetup kPadSetup[] = {
    // DRUM2 (0-7)
    { 0, CK_KICK,  0, 0 }, { 1, CK_SNARE, 0, 0 }, { 2, CK_HAT_C, 0, 1 }, { 3, CK_CLAP,  0, 0 },
    { 4, CK_TOM,   0, 0 }, { 5, CK_HAT_O, 0, 1 }, { 6, CK_HAT_C, 0, 1 }, { 7, CK_HAT_O, 0, 1 },
    // DRUMS2/3 (8-15) — LT/HT are two separately-cooked tom pitches (see CK_TOM_HI's own comment)
    { 8, CK_KICK,  0, 0 }, { 9, CK_SNARE, 0, 0 }, { 10, CK_TOM, 0, 0 }, { 11, CK_TOM_HI, 0, 0 },
    { 12, CK_HAT_C, 0, 2 }, { 13, CK_CLAP, 0, 0 }, { 14, CK_HAT_C, 0, 2 }, { 15, CK_HAT_O, 0, 2 },
};
// Each pad's own "neutral" pitch value (mirrors main.cpp's kDrum2DefaultPitch[] for pads
// 0-7 and kDrumSDefaultPitch[808][] for pads 8-15 — DRUMS already treats 808/909 as
// sounding identical, see kPadSetup's own comment, so one neutral table covers both kits).
// Pitch offsets below are computed relative to THIS baseline, not to the cooked sample's
// own recorded note (which for the noise-ish pads — HHC/CLP/OHH/RDE and their DRUMS
// equivalents — was never meant to equal the "pitch" pot's default position, that pot
// doubling as a brightness/tone knob for those pads, see kDrumSDefaultPitch's own
// comment). This keeps a hit left at its default pitch sounding exactly as before; only an
// actual knob move or the "random pitch" step modifier produces an audible shift.
const uint8_t kPadNeutralPitch[16] = {
    36, 70, 100, 85, 55, 92, 88, 78,     // DRUM2: KK1,SN1,HHC,CLP,BNG,OHH,CHH,RDE
    36, 64, 41, 50, 90, 70, 100, 95,     // DRUMS (808): BD,SD,LT,HT,RS,CP,CH,OH
};

// Each pad's Decay mapping: {the decay knob's default for that pad (main.cpp's
// kDrum2DefaultDecay / kDrumSDefaultDecay[808]), the length in ms (-60 dB) the pad
// sounds at that default — about what the old one-shot samples lasted}. The knob scales
// the length proportionally, so a pad left at its default sounds as before, and e.g.
// CHH's default of 380 still means a short closed hat, not a 380 ms one.
struct PadDecayMap { float defaultMs, naturalMs; };
const PadDecayMap kPadDecay[16] = {
    {400, 500}, {180, 300}, {50, 100}, {150, 200}, {220, 400}, {350, 450}, {380, 100}, {500, 450},   // DRUM2
    {450, 500}, {180, 300}, {320, 400}, {260, 350}, {70, 100}, {200, 200}, {55, 100}, {500, 450},   // DRUMS (808)
};

// The pad's blob for one hit: tune offset by (midiPitch - the pad's neutral pitch) and the
// decay above. Sent before every trigger, never restored afterwards: a blob re-sent while
// the pad sounds retunes its voice live (Sampler::set_live(), added for MODE_DJ), so the
// old "restore the base tune" resend undid the pitch knob on every hit. decayMs 0 = the
// pad's default. `vel` (the hit's velocity x the pad's volume, clamped to 0..1 — the hat
// pads' default volume of 1.6 was always capped at full, kept that way) goes into the blob's
// gain, continuous: through PadSeqEvent's 16 levels the quietest hit was still 1/8 of full
// (-18 dB) — the volume pot couldn't go lower ("la réduction de volume n'est pas
// suffisante"). Pitched up, a hit is also turned down 4.5 dB per octave ("les pitch aigus
// sont trop forts": the same energy squeezed into a shorter, brighter sound reads much
// louder). The last values are kept per pad so a re-send for the master volume
// (setupPadBlobs()) doesn't change a hit that is still ringing.
int32_t g_drumTuneOff[16] = {};
float g_drumDecayMs[16] = {};
float g_drumVel[16] = {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1};
void sendDrumBlob(int padBlobIdx, int32_t tuneOffsetSemis, float decayMs, float vel) {
    g_drumTuneOff[padBlobIdx] = tuneOffsetSemis; g_drumDecayMs[padBlobIdx] = decayMs; g_drumVel[padBlobIdx] = vel;
    const PadSetup &s = kPadSetup[padBlobIdx];
    const DrumSampleSpec &spec = kDrumSamples[s.sample];
    if (tuneOffsetSemis < -48) tuneOffsetSemis = -48; else if (tuneOffsetSemis > 48) tuneOffsetSemis = 48;
    const PadDecayMap &dm = kPadDecay[padBlobIdx];
    float lenMs = dm.naturalMs * (decayMs > 0.0f ? decayMs / dm.defaultMs : 1.0f);
    if (lenMs < 15.0f) lenMs = 15.0f; else if (lenMs > 8000.0f) lenMs = 8000.0f;
    float gain = g_masterVolume * kPadBaseLevel * (vel < 0.0f ? 0.0f : (vel > 1.0f ? 1.0f : vel));
    if (tuneOffsetSemis > 0) gain *= powf(2.0f, -0.75f * (float)tuneOffsetSemis / 12.0f);
    int32_t lvl = (int32_t)(gain * 32767.0f);
    if (lvl > 32767) lvl = 32767;
    PadBlob b;
    b.pad = s.pad; b.bus = 0; b.mode = PADM_ONESHOT; b.choke = s.choke; b.filter = 0;
    b.slot = g_drumSlot[s.sample];
    b.start = 0;
    b.loop = 1; b.lstart = spec.lstart; b.end = spec.lend; b.xfade = spec.xfade;
    b.decay = (int32_t)(lenMs * (float)kSampleRate / 1000.0f);
    b.tail = 440;
    b.gain_l = (q15)lvl; b.gain_r = (q15)lvl;
    b.tune = (s.tuneSemis + tuneOffsetSemis) * 256;
    padsSetBlob(&b, sizeof(b));
}

void triggerPadTuned(int padBlobIdx, float vel, int32_t tuneOffsetSemis, float decayMs) {
    if (!g_padsReady) return;
    if (padBlobIdx < 0 || padBlobIdx >= (int)(sizeof(kPadSetup) / sizeof(kPadSetup[0]))) return;
    sendDrumBlob(padBlobIdx, tuneOffsetSemis, decayMs, vel);
    triggerPad(padBlobIdx, 1.0f);   // full level: the hit's own level is already in the blob's gain
}

void setupPadBlobs() {
    for (int i = 0; i < (int)(sizeof(kPadSetup) / sizeof(kPadSetup[0])); i++) sendDrumBlob(i, g_drumTuneOff[i], g_drumDecayMs[i], g_drumVel[i]);
}

// ---- Master FX chain, part 2: real engine-module-backed effects ----
// REVERB/CHORUS/DELAY/EQ/RESOECHO/REP/BITCRS/RINGMOD/COMPRESSOR/RES were pure no-op
// stubs (confirmed directly: "les FX ne fonctionnent plus en général" — of the 14
// fxList[] slots, only FILT/DRIVE below and FLANGER/TREMOLO elsewhere ever did
// anything). The engine's own Module classes (engine/modules/fx_modules.cpp,
// fx2_modules.cpp) are real, working DSP for all of these. They're graph nodes
// (expect an upstream node for their audio input) — but Module itself (engine/core/
// module.h) is a plain interface (init/set_param/process) that works fine instantiated
// standalone, no Graph/Engine/node-routing needed: q15 IS int16_t (engine/dsp/q.h), so
// the already-mixed raw PCM this file works on needs no conversion at all.
//
// The concrete classes (Delay/Chorus/Reverb/Comp/Eq3/Shifter/TunedComb/Shaper) are all
// in anonymous namespaces inside their own .cpp files (internal linkage) — not nameable
// here. The public, intended way to construct one from outside is the SAME mechanism
// the Engine/Graph system itself uses: register_fx_modules()/register_fx2_modules()/
// register_synth_modules() populate a Registry with {info, create} function pointers
// keyed by the T_* ids already in fx_modules.h/fx2_modules.h/synth_modules.h; `create`
// is a plain `Module *(*)(Memory&)` — calling it needs no concrete type name.
//
// One wrinkle: modules process ENGINE_BLOCK=32 frames per call, vs this file's own
// 128-frame AMY blocks — applyMasterFx() loops 4x over 32-frame sub-chunks for whichever
// of these are active, using a small scratch buffer rather than assuming in/out can
// alias the same memory for every module (not proven safe to assume).
namespace {
Registry   g_fxRegistry;
Heap       g_fxHeap;
Module    *g_fxReverb = nullptr, *g_fxChorus = nullptr;
Module    *g_fxDelayMod = nullptr, *g_fxResDelayMod = nullptr;   // DELAY / RES — separate instances
Module    *g_fxEq3 = nullptr;
Module    *g_fxResoDelayMod = nullptr;                           // RESOECHO — own Delay instance, see audioSetResoEcho()
Module    *g_fxFoldL = nullptr, *g_fxFoldR = nullptr;            // REP + BITCRS (shared — see their own fxList comment)
Module    *g_fxShifter = nullptr;                                 // RINGMOD
Module    *g_fxComp = nullptr;                                   // COMPRESSOR
Module    *g_fxFlanger = nullptr;                                 // FLANGER

bool g_reverbOn=false, g_chorusOn=false, g_fxDelayOn=false, g_resDelayOn=false;
bool g_eqOn=false, g_combOn=false, g_foldOn=false, g_ringmodOn=false, g_compOn=false;
bool g_flangerOn=false;

Module *fxCreate(int id, Memory &mem) {
    const ModuleType *t = g_fxRegistry.get(id);
    return t ? t->create(mem) : nullptr;
}
}  // namespace

void fxModulesInit() {
    register_fx_modules(g_fxRegistry);
    register_fx2_modules(g_fxRegistry);
    register_synth_modules(g_fxRegistry);   // Shaper (T_SHAPER_G) for REP/BITCRS
    // 3 Delay instances (DELAY/RES/RESOECHO — see audioSetResoEcho()'s own comment on
    // why RESOECHO switched from TunedComb to a 3rd Delay) + Reverb + the small ones.
    // Measured directly on the simulator (host, 64-bit pointers): 384KB and 512KB both
    // failed ("one or more failed to init", settled usage 453760 bytes even at the 512KB
    // cap), 1MB verified clean (settled usage 630288). On real ESP32-S3 hardware (32-bit
    // pointers, smaller struct footprint) the SAME module set settles at only 317584
    // bytes used — but giving it the simulator's generous 1MB there leaves only ~771KB of
    // the ~2MB PSRAM budget for everything initialized after it (pads' own 768KB heap +
    // its Engine/SampleBank structs), which doesn't fit — confirmed directly: "pads:
    // alloc failed" at boot. So this is sized per target: 1MB only on the simulator
    // (host has effectively unconstrained RAM to spare), 512KB on real hardware (>60%
    // margin over the measured 317584 actually used, no fragmentation risk since these
    // are this heap's first and only allocations), leaving the other role engines their
    // own room in the shared ~2MB PSRAM budget.
#ifdef SIMULATOR
    constexpr size_t kFxMemBytes = 1024 * 1024;
#else
    constexpr size_t kFxMemBytes = 512 * 1024;
#endif
#ifdef GRVEP_FUSION_USE_PSRAM
    void *fxMem = heap_caps_malloc(kFxMemBytes, MALLOC_CAP_SPIRAM);
    if (!fxMem) { Serial.println("fx modules: PSRAM alloc failed"); return; }
    g_fxHeap.init(fxMem, kFxMemBytes);
#else
    static uint8_t fxMem[kFxMemBytes];
    g_fxHeap.init(fxMem, sizeof(fxMem));
#endif
    Memory mem{&g_fxHeap, &g_fxHeap};
    g_fxReverb      = fxCreate(T_REVERB, mem);
    g_fxChorus      = fxCreate(T_CHORUS, mem);
    g_fxDelayMod    = fxCreate(T_DELAY, mem);
    g_fxResDelayMod = fxCreate(T_DELAY, mem);
    g_fxEq3         = fxCreate(T_EQ3, mem);
    g_fxResoDelayMod = fxCreate(T_DELAY, mem);
    g_fxFoldL       = fxCreate(T_SHAPER_G, mem);
    g_fxFoldR       = fxCreate(T_SHAPER_G, mem);
    g_fxShifter     = fxCreate(T_SHIFTER, mem);
    g_fxComp        = fxCreate(T_COMP, mem);
    g_fxFlanger     = fxCreate(T_FLANGER, mem);
    if (!g_fxReverb || !g_fxChorus || !g_fxDelayMod || !g_fxResDelayMod || !g_fxEq3
        || !g_fxResoDelayMod || !g_fxFoldL || !g_fxFoldR || !g_fxShifter || !g_fxComp
        || !g_fxFlanger) {
        Serial.println("fx modules: one or more failed to init (out of heap?)");
    }
    Serial.printf("fx modules: heap used=%u/%u bytes (high water %u)\n",
                  (unsigned)g_fxHeap.used(), (unsigned)kFxMemBytes, (unsigned)g_fxHeap.high_water());
}

// ---- Master FX chain, part 1: FILT + DRIVE ----
// Applied post-mix, on the final summed stereo signal (main engine + every drum role),
// in audioEngineRenderBlock() — matching the old AMY-era "bus 0" design's own stated
// intent ("reaches literally everything mixed into bus 0 uniformly: drums, samples,
// patches, T303, etc."), which a per-engine FX chain structurally couldn't do once SYNTH
// and the drum roles became separate Engine instances. audioSetFiltBus()/
// audioSetLadderFilter()/audioSetDrive() were pure no-op stubs left over from the Phase 1
// skeleton — confirmed directly: "pour synth et drums, les FX ne fonctionnent pas".
//
// Hand-rolled with engine/dsp/svf.h's own free functions (svf_coef_f/svf_tick_f), not a
// graph node: cheap (a couple of float multiply-adds per sample per channel), and this
// turn already spent its CPU budget fixing two real overruns (OSCX idle cost, N_FILTER's
// per-voice SVF) — not the moment to add a new per-block Engine instance for this.
bool g_filtOn = false, g_ladderOn = false, g_driveOn = false;
uint8_t g_filtMode = FLTM_LP;   // FLTM_LP/HP/BP, audioSetFiltBus's own `type`
float g_driveAmt = 0.0f, g_driveLevel = 1.0f;
SvfCoefF g_filtCoef[2];    // [0]=L,[1]=R — audioSetFiltBus's own filter (SVF: real LP/HP/BP)
SvfStateF g_filtState[2];
SvfCoefF g_ladderCoef[2];  // audioSetLadderFilter's own filter (SVF in LP mode — see its own comment)
SvfStateF g_ladderState[2];
// DRIVE's "Tone" post-distortion tilt (see audioSetDrive()'s own comment on why this
// replaced a plain `x *= tone` gain multiply) — same SVF core as FILT/LADDER, LP mode,
// cutoff swept by the Tone knob.
SvfCoefF g_driveToneCoef[2];
SvfStateF g_driveToneState[2];

void applyMasterFx(int16_t *stereo, int frames) {
    if (g_filtOn || g_ladderOn || g_driveOn) {
        for (int i = 0; i < frames; i++) {
            for (int ch = 0; ch < 2; ch++) {
                float x = (float)stereo[2 * i + ch] * (1.0f / 32768.0f);
                if (g_filtOn) {
                    float lp, bp, hp;
                    svf_tick_f(g_filtCoef[ch], g_filtState[ch], x, lp, bp, hp);
                    x = (g_filtMode == FLTM_HP) ? hp : (g_filtMode == FLTM_BP) ? bp : lp;
                }
                if (g_ladderOn) {
                    // Approximated with the same TPT SVF core in LP mode (real resonant
                    // self-oscillation via its own k/Q, not a true 4-pole ladder's cascaded
                    // one-poles+soft-clip) — a reasonable stand-in, not bit-exact Moog
                    // character; flagged here rather than silently presented as the real thing.
                    float lp, bp, hp;
                    svf_tick_f(g_ladderCoef[ch], g_ladderState[ch], x, lp, bp, hp);
                    x = lp;
                }
                if (g_driveOn) {
                    x = tanhf(x * (1.0f + g_driveAmt));
                    float lp, bp, hp;
                    svf_tick_f(g_driveToneCoef[ch], g_driveToneState[ch], x, lp, bp, hp);
                    x = lp * g_driveLevel;
                }
                stereo[2 * i + ch] = svf_out_f(x);
            }
        }
    }

    // ---- Module-backed FX: REVERB/CHORUS/DELAY/EQ/RESOECHO/REP+BITCRS/RINGMOD/
    // COMPRESSOR/RES (see fxModulesInit()'s own comment above). fxList[] index order,
    // not fxOrderList[]'s dynamic activation order — same "fixed order" simplification
    // FILT/DRIVE above already have, not a regression introduced here. q15 IS int16_t
    // (engine/dsp/q.h) so `stereo` is reinterpreted directly, no sample conversion.
    if (g_reverbOn || g_chorusOn || g_flangerOn || g_fxDelayOn || g_eqOn || g_combOn || g_foldOn
        || g_ringmodOn || g_compOn || g_resDelayOn) {
        ProcessCtx ctx{};
        for (int base = 0; base < frames; base += kBlock) {
            int n = frames - base; if (n > kBlock) n = kBlock;
            ctx.frames = n;
            q15 l[kBlock], r[kBlock], outl[kBlock], outr[kBlock];
            for (int i = 0; i < n; i++) { l[i] = stereo[2*(base+i)]; r[i] = stereo[2*(base+i)+1]; }

            auto runStereo = [&](Module *m) {
                if (!m) return;
                Ports p{}; p.in[0] = l; p.in[1] = r; p.out[0] = outl; p.out[1] = outr;
                m->process(ctx, p);
                for (int i = 0; i < n; i++) { l[i] = outl[i]; r[i] = outr[i]; }
            };
            if (g_reverbOn)   runStereo(g_fxReverb);
            if (g_chorusOn)   runStereo(g_fxChorus);
            if (g_flangerOn)  runStereo(g_fxFlanger);
            if (g_fxDelayOn)  runStereo(g_fxDelayMod);
            if (g_eqOn)       runStereo(g_fxEq3);
            if (g_combOn)     runStereo(g_fxResoDelayMod);
            if (g_foldOn && g_fxFoldL && g_fxFoldR) {
                // Shaper: n_in=1,n_out=1 (mono) — one instance per channel.
                Ports pl{}; pl.in[0] = l; pl.out[0] = outl;
                g_fxFoldL->process(ctx, pl);
                Ports pr{}; pr.in[0] = r; pr.out[0] = outr;
                g_fxFoldR->process(ctx, pr);
                for (int i = 0; i < n; i++) { l[i] = outl[i]; r[i] = outr[i]; }
            }
            if (g_ringmodOn)  runStereo(g_fxShifter);
            if (g_compOn)     runStereo(g_fxComp);
            if (g_resDelayOn) runStereo(g_fxResDelayMod);

            for (int i = 0; i < n; i++) { stereo[2*(base+i)] = l[i]; stereo[2*(base+i)+1] = r[i]; }
        }
    }
}

// ---- Master limiter: always on, not a togglable FX ----
// Polyphonic voices sum additively — a single note never clips, but several at once
// easily can (measured directly: a 6-note chord at full volume pegged 11 of 35 shapes at
// exactly 32766/32767 — real digital clipping, not a per-shape loudness-imbalance
// artifact, confirmed by checking single-note peaks separately, which stayed well under
// scale). Reported as "les instruments saturent beaucoup" — hard brick-wall clipping
// (the plain min/max clamp audioEngineRenderBlock already had) sounds like exactly that:
// a harsh digital crackle the instant a chord's sum crosses the ceiling. A soft limiter
// — gain reduction that engages smoothly approaching the ceiling, released over tens of
// ms rather than snapping back — fixes the audible character without touching any
// shape's own level the rest of the time (threshold sits above typical single/double-
// note peaks, per the sweep). Linked stereo (one envelope from max(|L|,|R|), same gain
// both channels) so loud moments don't shift the stereo image.
float g_limEnv = 0.0f;
constexpr float kLimThresh = 0.82f;                 // ~26900/32767 — headroom above typical single-note peaks
const float kLimAttack = 1.0f - expf(-1.0f / (0.0001f * kSampleRate));   // ~0.1ms — fast enough for a chord's constructive-interference peaks
const float kLimRelease = 1.0f - expf(-1.0f / (0.08f * kSampleRate));    // ~80ms

void applyMasterLimiter(int16_t *stereo, int frames) {
    for (int i = 0; i < frames; i++) {
        float l = (float)stereo[2 * i] * (1.0f / 32768.0f);
        float r = (float)stereo[2 * i + 1] * (1.0f / 32768.0f);
        float peak = fabsf(l) > fabsf(r) ? fabsf(l) : fabsf(r);
        g_limEnv += (peak - g_limEnv) * (peak > g_limEnv ? kLimAttack : kLimRelease);
        float gain = g_limEnv > kLimThresh ? kLimThresh / g_limEnv : 1.0f;
        stereo[2 * i] = svf_out_f(l * gain);
        stereo[2 * i + 1] = svf_out_f(r * gain);
    }
}

// ---- FX Punch: looper + crusher (MODE_* FX-trigger hold, main.cpp's fxPunchApply()) ----
// The looper kinds (LOOP 1/4..1/32, STUTTER, REVERSE, TAPESTOP, HALFSPD) used to live in
// AMY (amy_punch_*(), amy.c), whose output no longer reaches the DAC in this fork — they
// silently did nothing. Same idea, on the final mix here: a ring keeps the last
// g_punchLen frames of output; a held punch replaces the output with a read from it.
// One ring only (PSRAM is tight here, not AMY's two 2 s buffers): a LOOP freezes the
// capture while it plays, so the repeated audio can't be overwritten under it; the
// rate-based kinds read the live, still-written history. Allocated on first use
// (audioPunchPrepare(), when the punch overlay opens) and kept.
// BITCRUSH/ALIAS were REP's wavefold at extreme gain — loud and harsh ("beaucoup trop
// fort"); they are now a real bit-depth / sample-rate reduction that keeps the level.
enum PunchLoopMode : uint8_t { PL_NONE, PL_FIXED, PL_RATE };
int16_t *g_punchBuf = nullptr;            // interleaved stereo, g_punchLen frames
uint32_t g_punchLen = 0, g_punchWrite = 0;
volatile uint8_t g_punchMode = PL_NONE;
uint32_t g_punchLoopStart = 0, g_punchLoopLen = 0, g_punchLoopPos = 0;
float g_punchRead = 0.0f, g_punchRate = 1.0f, g_punchDecel = 0.0f;
volatile int g_crushBits = 16, g_crushHold = 1;
int16_t g_crushHeld[2] = {0, 0};
int g_crushCount = 0;

inline int16_t punchReadInterp(float pos, int ch) {
    while (pos < 0.0f) pos += (float)g_punchLen;
    uint32_t i0 = (uint32_t)pos % g_punchLen, i1 = (i0 + 1) % g_punchLen;
    float frac = pos - floorf(pos);
    float s0 = g_punchBuf[2 * i0 + ch], s1 = g_punchBuf[2 * i1 + ch];
    return (int16_t)(s0 + (s1 - s0) * frac);
}

void applyPunch(int16_t *stereo, int frames) {
    if (g_crushBits < 16 || g_crushHold > 1) {
        const int shift = 16 - g_crushBits, hold = g_crushHold;
        // Rounded to the nearest step, not floored: a plain >> rounds toward -inf, which
        // turns near-silence into a constant -1 step (a DC/hiss floor after every sound).
        auto quant = [shift](int32_t x) -> int16_t {
            if (shift <= 0) return (int16_t)x;
            x = ((x + (1 << (shift - 1))) >> shift) << shift;
            return (int16_t)(x > 32767 ? 32767 : x);
        };
        for (int i = 0; i < frames; i++) {
            if (g_crushCount <= 0) {
                g_crushHeld[0] = quant(stereo[2 * i]);
                g_crushHeld[1] = quant(stereo[2 * i + 1]);
                g_crushCount = hold;
            }
            g_crushCount--;
            stereo[2 * i] = g_crushHeld[0]; stereo[2 * i + 1] = g_crushHeld[1];
        }
    }
    if (!g_punchBuf) return;
    const uint8_t mode = g_punchMode;
    if (mode != PL_FIXED) {
        for (int i = 0; i < frames; i++) {
            uint32_t w = (g_punchWrite + (uint32_t)i) % g_punchLen;
            g_punchBuf[2 * w] = stereo[2 * i]; g_punchBuf[2 * w + 1] = stereo[2 * i + 1];
        }
        g_punchWrite = (g_punchWrite + (uint32_t)frames) % g_punchLen;
    }
    if (mode == PL_FIXED) {
        for (int i = 0; i < frames; i++) {
            uint32_t r = (g_punchLoopStart + g_punchLoopPos) % g_punchLen;
            stereo[2 * i] = g_punchBuf[2 * r]; stereo[2 * i + 1] = g_punchBuf[2 * r + 1];
            if (++g_punchLoopPos >= g_punchLoopLen) g_punchLoopPos = 0;
        }
    } else if (mode == PL_RATE) {
        // The read cursor may lag at most the ring minus one block behind the write one.
        // REVERSE/HALFSPD then jump back to "now" (a reverse that keeps re-reversing the
        // latest audio); TAPESTOP has run out of tape by then and stays silent.
        const float maxAge = (float)(g_punchLen - (uint32_t)frames);
        for (int i = 0; i < frames; i++) {
            float age = (float)g_punchWrite - g_punchRead;
            while (age < 0.0f) age += (float)g_punchLen;
            if (age > maxAge) {
                if (g_punchDecel > 0.0f) { g_punchRate = 0.0f; g_punchRead = (float)g_punchWrite; }
                else g_punchRead = (float)g_punchWrite - 1.0f;
            }
            if (g_punchRate == 0.0f) { stereo[2 * i] = stereo[2 * i + 1] = 0; continue; }
            stereo[2 * i] = punchReadInterp(g_punchRead, 0);
            stereo[2 * i + 1] = punchReadInterp(g_punchRead, 1);
            g_punchRead += g_punchRate;
            if (g_punchRead >= (float)g_punchLen) g_punchRead -= (float)g_punchLen;
            if (g_punchDecel > 0.0f) { g_punchRate *= g_punchDecel; if (g_punchRate < 0.01f) g_punchRate = 0.0f; }
        }
    }
}

void setSvfCutoffRes(SvfCoefF *coef, float cutoffHz, float resonance) {
    float hz = cutoffHz < 20.0f ? 20.0f : (cutoffHz > (float)kSampleRate * 0.45f ? (float)kSampleRate * 0.45f : cutoffHz);
    float g = tanf(3.14159265f * hz / (float)kSampleRate);
    float k = resonance > 0.05f ? 1.0f / resonance : 20.0f;   // resonance is a Q-like value; near-0 clamped away from a divide blow-up
    SvfCoefF c;
    c.a1 = 1.0f / (1.0f + g * (g + k));
    c.a2 = g * c.a1;
    c.a3 = g * c.a2;
    c.k = k;
    coef[0] = coef[1] = c;
}

}  // namespace

void audioInit() {
    // ---- AMY (kept alive during the migration — see this file's top-of-file comment) ----
    amy_config_t cfg = amy_default_config();
    cfg.features.startup_bleep = 0;
    cfg.features.default_synths = 0;
    cfg.max_oscs = 266;
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
    amy_start(cfg);
#ifndef SIMULATOR
    esp32_setup_i2s();
#endif
    delay(500);
    {
        amy_event e = amy_default_event();
        e.synth = SYNTH_CH;
        e.num_voices = NUM_SYNTH_VOICES;
        e.oscs_per_voice = OSCS_PER_VOICE;
        e.wave = SAW_DOWN;
        amy_add_event(&e);
    }
    {
        amy_event e = amy_default_event();
        e.synth = SYNTH_CH;
        e.filter_type = FILTER_NONE;
        amy_add_event(&e);
    }
    flashCacheInit();  // map pcmcache partition before anything tries to read it

    // ---- New engine (Phase 1 skeleton) ----
    // Phase 1's placeholder graph is one tiny 8-voice saw->filter->env->vca patch — this
    // only needs to fit it, not a real synth's worth of modules. On real hardware, both
    // the Engine object and its heap buffer live in PSRAM (see GRVEP_FUSION_USE_PSRAM's
    // comment above) instead of internal DRAM, so they stop competing with FreeRTOS task
    // stacks for a budget that measurably varies run to run.
    // 96KB, not 48KB: N_OSCX (T_OSCX, register_osc_engines) adds a per-voice Karplus
    // string buffer (2048 int16 = 4KB) even on shapes that never select OSCX_KARP — the
    // module allocates all its internal buffers at init() regardless of which engine_ a
    // given shape actually picks — so kMainVoices=8 voices costs +32KB on top of the
    // existing modules' own per-voice state. Trivial against PSRAM's ~2MB; sized with
    // headroom rather than re-tuned tight, per this project's own repeated "NoMem" lesson.
#ifdef GRVEP_FUSION_USE_PSRAM
    constexpr size_t kFastMemBytes = 96 * 1024;
    void *fastMem = heap_caps_malloc(kFastMemBytes, MALLOC_CAP_SPIRAM);
    void *engineMem = heap_caps_malloc(sizeof(Engine), MALLOC_CAP_SPIRAM);
    if (!fastMem || !engineMem) { Serial.println("engine: PSRAM alloc failed"); audioReady = true; return; }
    g_mainEnginePtr = new (engineMem) Engine();
    g_fastHeap.init(fastMem, kFastMemBytes);
#else
    static uint8_t fastMem[96 * 1024];
    g_fastHeap.init(fastMem, sizeof(fastMem));
#endif
    Memory mem{&g_fastHeap, &g_fastHeap};

    if (!g_mainEngine.init(mem, kMainVoices)) { Serial.println("engine: init failed (out of heap?)"); audioReady = true; return; }
    register_synth_modules(g_mainEngine.registry());
    register_osc_engines(g_mainEngine.registry());

    GraphDesc &g = scratchGraph();
    if (!buildMainGraph(g)) { Serial.println("engine: buildMainGraph failed"); audioReady = true; return; }
    if (g_mainEngine.load(g, kMainVoices) != Err::Ok) { Serial.println("engine: load failed (out of heap?)"); audioReady = true; return; }

    g_mainReady = true;
    audioReady = true;

    fxModulesInit();

    // ---- Drums: cook each role's sound to a .smp file, then load all 6 into one shared
    // Pads engine — see that section's own comment (above triggerPad()) for why this
    // replaced the one-live-Engine-per-role design.
    if (initPadsEngine()) {
        cookAndLoadDrumSamples();
        setupPadBlobs();
    } else {
        Serial.println("pads: init failed, drums silent");
    }

    if (!initRoleEngine(g_t303Role, 48 * 1024, kT303Voices, buildT303Graph))
        Serial.println("T303: init failed");

    if (!initRoleEngine(g_modRole, 96 * 1024, kModVoices, buildModularGraph, register_osc_engines))
        Serial.println("Modular: init failed");
}

// SAW_FM and SUPERSAW both route through N_OSCX (T_OSCX) — measured directly on real
// ESP32-S3 hardware against the 725.6us/block budget: SAW_FM stays safe up to 4 held notes
// (533us) and crosses over at 5 (772us); SUPERSAW is pricier per voice (its OSCX_SSAW engine
// sums several internal detuned oscillators per instance) and crosses over at 4 (929us,
// vs. 593us at 3). The other 33 shapes use plain T_OSC — far cheaper, no cap needed, still
// the full kMainVoices=6. Reported as general "lag... sur SYNTH" (confirmed: this is a real
// audio-render overrun, not a UI/visual lag). Capping these two shapes' own polyphony at the
// measured-safe count is cheaper than a structural per-shape Engine::load() (an 85-250ms
// stall on every switch into/out of them) — a new note past the cap steals the OLDEST held
// note (FIFO), the same "oldest wins" policy a polyphony-limited hardware synth typically
// uses. Only enforced going forward: switching INTO one of these shapes while already
// holding more notes than its cap (from a previous, uncapped shape) doesn't retroactively
// cut any of them — simpler and less surprising than silently killing notes on a shape
// change the user didn't ask to change loudness-wise.
constexpr int kSawFmMaxVoices    = 4;
constexpr int kSupersawMaxVoices = 3;
uint8_t g_mainHeldNotes[kMainVoices];
int g_mainHeldCount = 0;

int oscxShapeVoiceCap(SynthShape s) {
    if (s == SHAPE_SAW_FM) return kSawFmMaxVoices;
    if (s == SHAPE_SUPERSAW) return kSupersawMaxVoices;
    return kMainVoices;   // every other shape: no cap beyond the engine's own voice count
}

void audioAllNotesOff() {
    if (g_mainReady) g_mainEngine.all_notes_off();
    g_mainHeldCount = 0;
}

void audioNoteOn(uint8_t note, float velocity) {
    if (!g_mainReady) return;
    bool alreadyHeld = false;
    for (int i = 0; i < g_mainHeldCount; i++) if (g_mainHeldNotes[i] == note) { alreadyHeld = true; break; }
    if (!alreadyHeld) {
        int cap = oscxShapeVoiceCap(g_currentMainShape);
        if (g_mainHeldCount >= cap) {
            g_mainEngine.note_off(g_mainHeldNotes[0]);
            for (int i = 1; i < g_mainHeldCount; i++) g_mainHeldNotes[i-1] = g_mainHeldNotes[i];
            g_mainHeldCount--;
        }
        if (g_mainHeldCount < kMainVoices) g_mainHeldNotes[g_mainHeldCount++] = note;
    }
    float scaled = velocity * shapeGain[g_currentMainShape];
    int vel127 = (int)(scaled * 127.0f);
    if (vel127 < 1) vel127 = 1;
    if (vel127 > 127) vel127 = 127;
    g_mainEngine.note_on(note, vel127);
}

void audioNoteOff(uint8_t note) {
    if (g_mainReady) g_mainEngine.note_off(note);
    for (int i = 0; i < g_mainHeldCount; i++) {
        if (g_mainHeldNotes[i] == note) {
            for (int j = i+1; j < g_mainHeldCount; j++) g_mainHeldNotes[j-1] = g_mainHeldNotes[j];
            g_mainHeldCount--;
            break;
        }
    }
}

void audioSetEnvelope(const EnvParams &env) {
    g_mainEnv = env;
    if (!g_mainReady) return;
    g_mainEngine.set_param(N_ENV, ENV_ATTACK, env.atk);
    g_mainEngine.set_param(N_ENV, ENV_DECAY, env.dec);
    g_mainEngine.set_param(N_ENV, ENV_SUSTAIN, (int32_t)(env.sus * 32767.0f));
    g_mainEngine.set_param(N_ENV, ENV_RELEASE, env.rel);
}

void audioSetVolume(float vol) {
    g_masterVolume = vol;
    if (g_mainReady) g_mainEngine.set_param(N_VOICE_OUT, VO_LEVEL, (int32_t)(vol * kVolumeHeadroom * 32767.0f));
    // Pads has no single master VO_LEVEL — each pad's own gain_l/gain_r is a field in its
    // PadBlob, so the VOL pot re-sends every blob (setupPadBlobs() already reads
    // g_masterVolume into that same gain calculation; same table, so they can't drift).
    if (g_padsReady) setupPadBlobs();
    smpRefreshGains();   // the sample keys / preview / DJ deck pads, same reason (see "SD streaming")
    if (g_t303Role.ready) g_t303Engine.set_param(T3_VOUT, VO_LEVEL, (int32_t)(vol * kT303BaseLevel * 32767.0f));
    if (g_modRole.ready) g_modEngine.set_param(MOD_VOUT, VO_LEVEL, (int32_t)(vol * kModBaseLevel * 32767.0f));
}

// Early approximation, not the real Phase 3 per-shape graph table: picks a wave +
// filter character per SynthShape so switching presets is actually audible, using
// live set_param calls on the EXISTING graph (changing a node's param value isn't a
// structural graph change, so no Engine::load() stall) rather than AMY's original
// per-shape patches (Juno/DX7 bank numbers, custom ALGO FM configs — none of that is
// portable to this engine as-is). Cutoff is in semitones (pitch units), not Hz.
struct ShapeParams { uint8_t wave; uint8_t filterMode; int32_t cutoffSemi; int32_t res; };
static const ShapeParams kShapeTable[SHAPE_COUNT] = {
    /*SAW*/            {WAVE_SAW_,   FLTM_LP, 90,  6000},
    /*SAW_FM*/         {WAVE_SAW_,   FLTM_LP, 85,  8000},
    /*SQUARE*/         {WAVE_PULSE_, FLTM_LP, 90,  6000},
    /*SINE*/           {WAVE_SINE_,  FLTM_LP, 100, 2000},
    /*SUPERSAW*/       {WAVE_SAW_,   FLTM_LP, 95,  4000},
    /*ACID*/           {WAVE_SAW_,   FLTM_LP, 70,  16000},
    /*BASS*/           {WAVE_SAW_,   FLTM_LP, 55,  8000},
    /*PLUCK*/          {WAVE_TRI_,   FLTM_LP, 85,  6000},
    /*NOISE_WHITE*/    {WAVE_NOISE_, FLTM_LP, 120, 2000},
    /*NOISE_PINK*/     {WAVE_NOISE_, FLTM_LP, 80,  2000},
    /*NOISE_BROWN*/    {WAVE_NOISE_, FLTM_LP, 55,  2000},
    /*JUNO_BRASS*/     {WAVE_SAW_,   FLTM_LP, 85,  6000},
    /*JUNO_STRINGS*/   {WAVE_SAW_,   FLTM_LP, 80,  4000},
    /*JUNO_PIANO*/     {WAVE_TRI_,   FLTM_LP, 105, 4000},  // cutoff raised 90->105 (matches DX7_BELLS) — reported "J:PNO parait étouffer"; measured cutoff's own effect as modest (~8%, WAVE_TRI_'s sparse harmonics are the bigger factor), kept as the quick, low-risk fix
    /*JUNO_ORGAN*/     {WAVE_PULSE_, FLTM_LP, 95,  3000},
    /*JUNO_CHOIR*/     {WAVE_SAW_,   FLTM_LP, 75,  3000},
    /*DX7_EP*/         {WAVE_TRI_,   FLTM_LP, 88,  5000},
    /*DX7_BELLS*/      {WAVE_SINE_,  FLTM_LP, 105, 3000},
    /*DX7_BASS*/       {WAVE_SAW_,   FLTM_LP, 50,  7000},
    /*DX7_BRASS*/      {WAVE_SAW_,   FLTM_LP, 85,  7000},
    /*DX7_STRINGS*/    {WAVE_SAW_,   FLTM_LP, 80,  4000},
    /*DX7_ORGAN*/      {WAVE_PULSE_, FLTM_LP, 95,  3000},
    /*DX7_VOICE*/      {WAVE_SINE_,  FLTM_LP, 82,  3000},
    /*TECHNO_LEAD*/    {WAVE_SAW_,   FLTM_LP, 95,  10000},
    /*RAVE_BASS*/      {WAVE_SAW_,   FLTM_LP, 48,  9000},
    /*HOOVER*/         {WAVE_SAW_,   FLTM_LP, 90,  12000},
    /*TECHNO_STAB*/    {WAVE_PULSE_, FLTM_LP, 92,  9000},
    /*ACID_WOBBLE*/    {WAVE_SAW_,   FLTM_LP, 65,  17000},
    /*ELECTRO_PLUCK*/  {WAVE_PULSE_, FLTM_LP, 88,  8000},
    /*INDUSTRIAL*/     {WAVE_SAW_,   FLTM_LP, 70,  14000},
    /*JUNO_ORGAN2*/    {WAVE_PULSE_, FLTM_LP, 92,  4000},
    /*JUNO_FRONTIER*/  {WAVE_SAW_,   FLTM_LP, 88,  5000},
    /*FM_DRIFT*/       {WAVE_SAW_,   FLTM_LP, 75,  11000},
    /*FM_BELL*/        {WAVE_SINE_,  FLTM_LP, 100, 4000},
    /*SAT_DRIFT*/      {WAVE_SAW_,   FLTM_LP, 80,  13000},
};

void audioSetShape(SynthShape shape) {
    if (!g_mainReady || shape >= SHAPE_COUNT) return;
    g_currentMainShape = shape;
    const ShapeParams &sp = kShapeTable[shape];
    g_mainEngine.set_param(N_FILTER, FLT_MODE, sp.filterMode);
    g_mainEngine.set_param(N_FILTER, FLT_CUTOFF, sp.cutoffSemi * 256);
    g_mainEngine.set_param(N_FILTER, FLT_RES, sp.res);
    if (shape == SHAPE_SAW_FM) {
        g_mainEngine.set_param(N_OSC, OSC_LEVEL, 0);
        g_mainEngine.set_param(N_OSCX, OSCX_ENGINE, OSCX_FM2);
        g_mainEngine.set_param(N_OSCX, OSCX_TIMBRE, 20000);   // FM2 timbre = modulation index (0..8 rad)
        g_mainEngine.set_param(N_OSCX, OSCX_MORPH, 12000);    // FM2 morph = freq ratio (0.5..12, stepped)
        g_mainEngine.set_param(N_OSCX, OSCX_LEVEL, kUnity);
    } else if (shape == SHAPE_SUPERSAW) {
        g_mainEngine.set_param(N_OSC, OSC_LEVEL, 0);
        g_mainEngine.set_param(N_OSCX, OSCX_ENGINE, OSCX_SSAW);
        g_mainEngine.set_param(N_OSCX, OSCX_TIMBRE, 22000);   // SSAW timbre = detune spread (up to +-4.5%)
        g_mainEngine.set_param(N_OSCX, OSCX_MORPH, 26000);    // SSAW morph = centre voice -> full unison
        g_mainEngine.set_param(N_OSCX, OSCX_LEVEL, kUnity);
    } else {
        g_mainEngine.set_param(N_OSC, OSC_WAVE, sp.wave);
        g_mainEngine.set_param(N_OSC, OSC_LEVEL, kUnity);
        g_mainEngine.set_param(N_OSCX, OSCX_LEVEL, 0);
    }
}

// Inverse of this file's own hzToPitchUnits()/resoQ conventions, and the two hardcoded
// OSCX_TIMBRE literals audioSetShape() uses for SAW_FM/SUPERSAW above (20000/22000) — so a
// pot-dispatch site's own tracked cutoff/resonance/depth can re-sync to EXACTLY what this
// shape's baseline actually is on a shape change, not an approximation that could still
// jump on the pot's first touch afterward.
void audioGetShapeDefaults(SynthShape shape, float *cutoffHz, float *resonance, float *fmDepth) {
    if (shape >= SHAPE_COUNT) shape = SHAPE_SAW;
    const ShapeParams &sp = kShapeTable[shape];
    if (cutoffHz) *cutoffHz = 440.0f * powf(2.0f, ((float)sp.cutoffSemi - 69.0f) / 12.0f);
    if (resonance) *resonance = 1.0f + ((float)sp.res / 32767.0f) * 3.0f;
    if (fmDepth) *fmDepth = (shape == SHAPE_SUPERSAW) ? (22000.0f / 32767.0f) : (20000.0f / 32767.0f);
}

// ratio is a frequency multiplier (AMY's own convention, kept so main.cpp's existing
// call sites — MODE_SYNTH's joystick-Y bend at +-1 semitone (was +-2, reported as too
// wide), OMNI's chord envelope, I303's slide — don't need to change): 2^(semitones/12).
// OSC_PITCH's param (not a
// modulation cable) is otherwise an unused flat baseline on this graph — the per-note
// pitch itself comes entirely from N_NOTE's own cable into N_OSC's input 0 — so adding
// the bend offset directly onto it live, uniformly across every active voice, needs no
// graph change. This was a no-op stub left over from the Phase 1 skeleton, which read as
// "lost the joystick bending" once the rest of MODE_SYNTH came back to life.
constexpr int32_t kOscCenterPitch = 60 * 256;

void audioSetPitchBend(float ratio) {
    if (!g_mainReady) return;
    float semis = (ratio > 0.0f) ? 12.0f * log2f(ratio) : 0.0f;
    int32_t units = kOscCenterPitch + (int32_t)lroundf(semis * 256.0f);
    g_mainEngine.set_param(N_OSC, OSC_PITCH, units);
    // SAW_FM/SUPERSAW mute N_OSC (OSC_LEVEL=0 in audioSetShape()) and sound through N_OSCX
    // instead — bending only N_OSC's own pitch left those two shapes' bend completely
    // inaudible (reported: "toujours le même problème sur le bending" persisted after the
    // +-1 semitone range fix, because for these two shapes nothing was ever reaching the
    // oscillator actually making sound). OSCX_PITCH uses the identical "baseline the pitch
    // CV is measured around" convention as OSC_PITCH (both default to 60*kSemi in their own
    // ModuleInfo), so the same kOscCenterPitch+bend value applies unchanged.
    g_mainEngine.set_param(N_OSCX, OSCX_PITCH, units);
}

// Render entry point for the HAL audio-out callback (wired in Phase 1d/2's
// engine_hal_sim/engine_hal_esp32/engine_hal_web). Replaces AMY's amy_render_audio() role.
extern "C" void audioEngineRenderBlock(int16_t *stereo, int frames) {
    static q15 blockL[kBlock], blockR[kBlock];
    static q15 padL[kBlock], padR[kBlock];
    static q15 t3L[kBlock], t3R[kBlock];
    static q15 modL[kBlock], modR[kBlock];
    static int blockPos = kBlock;
    if (!g_mainReady) { memset(stereo, 0, (size_t)frames * 4); return; }
    for (int i = 0; i < frames; i++) {
        if (blockPos >= kBlock) {
            g_mainEngine.render(blockL, blockR);
            g_mainEngine.gc();
            // 3 Engines (main + T303 + one shared Pads instance for every drum) stay
            // comfortably under the 4-engine performance cliff found this session ("303S +
            // DRUMS = bug et lag") — no render-skip gating needed for Pads the way the 6
            // separate per-role engines it replaced required.
            // No per-block pump() here: every sample is loaded fully resident once at
            // boot (cookAndLoadDrumSamples() already pumps until ready) and nothing ever
            // streams or loads again afterward — pump() exists for ongoing streaming
            // I/O, which this use case never needs.
            if (g_padsReady) { g_padsEngine.render(padL, padR); g_padsEngine.gc(); }
            else { memset(padL, 0, sizeof(padL)); memset(padR, 0, sizeof(padR)); }
            uint32_t now = millis();
            if (g_t303Role.ready && now < g_t303Role.activeUntilMs) { g_t303Engine.render(t3L, t3R); g_t303Engine.gc(); }
            else { memset(t3L, 0, sizeof(t3L)); memset(t3R, 0, sizeof(t3R)); }
            // 4th engine (MODE_MODULAR/AMBIENT/NOOB2/OMNI2's dual-oscillator voice) — same
            // windowed render-skip idiom as T303/Pads so it costs nothing while silent, but
            // unlike T303 it can legitimately render for as long as a note stays held (a
            // sustained pad/drone, not a bounded decay — see g_modHeldNotes's own comment),
            // so this is the one case where a real overlap with T303+a drum hit could
            // reach 4 simultaneously-active engines for longer than a brief tail. Flagged,
            // not yet re-measured on real hardware against the budget this session found
            // ("303S + DRUMS = bug et lag") — worth a direct timing check (see CLAUDE.md's
            // testing methodology) before calling this feature's CPU cost settled.
            if (g_modRole.ready && (g_modHeldNotes > 0 || now < g_modRole.activeUntilMs)) { g_modEngine.render(modL, modR); g_modEngine.gc(); }
            else { memset(modL, 0, sizeof(modL)); memset(modR, 0, sizeof(modR)); }
            blockPos = 0;
        }
        int32_t l = (int32_t)blockL[blockPos] + (int32_t)padL[blockPos] + (int32_t)t3L[blockPos] + (int32_t)modL[blockPos];
        int32_t r = (int32_t)blockR[blockPos] + (int32_t)padR[blockPos] + (int32_t)t3R[blockPos] + (int32_t)modR[blockPos];
        if (l > 32767) l = 32767; else if (l < -32768) l = -32768;
        if (r > 32767) r = 32767; else if (r < -32768) r = -32768;
        stereo[2 * i] = (int16_t)l;
        stereo[2 * i + 1] = (int16_t)r;
        blockPos++;
    }
    applyMasterFx(stereo, frames);
    applyPunch(stereo, frames);
    applyMasterLimiter(stereo, frames);
}

void audioPunchPrepare() {
    if (g_punchBuf) return;
    for (float sec : {1.0f, 0.5f}) {
        uint32_t len = (uint32_t)(sec * (float)kSampleRate);
#if defined(ESP_PLATFORM) && !defined(SIMULATOR)
        // PSRAM only: smpBigAlloc() falls back to internal RAM, which this size would exhaust.
        int16_t *buf = (int16_t *)heap_caps_malloc((size_t)len * 4, MALLOC_CAP_SPIRAM);
#else
        int16_t *buf = (int16_t *)malloc((size_t)len * 4);
#endif
        if (!buf) continue;
        memset(buf, 0, (size_t)len * 4);
        g_punchLen = len; g_punchWrite = 0;
        g_punchBuf = buf;   // published last: the audio task starts capturing from here
        Serial.printf("punch: %.1fs history buffer\n", sec);
        return;
    }
    Serial.println("punch: no memory for the history buffer");
}

void audioPunchLoopStart(float seconds) {
    if (!g_punchBuf) return;
    g_punchMode = PL_NONE;
    uint32_t len = (uint32_t)(seconds * (float)kSampleRate);
    if (len < kBlock) len = kBlock;
    if (len > g_punchLen - kBlock) len = g_punchLen - kBlock;
    g_punchLoopLen = len;
    g_punchLoopStart = (g_punchWrite + g_punchLen - len) % g_punchLen;   // the last `len` frames, ending now
    g_punchLoopPos = 0;
    g_punchMode = PL_FIXED;
}

static void punchRateStart(float rate, float decel) {
    if (!g_punchBuf) return;
    g_punchMode = PL_NONE;
    g_punchRead = (float)g_punchWrite - 1.0f;
    if (g_punchRead < 0.0f) g_punchRead += (float)g_punchLen;
    g_punchRate = rate; g_punchDecel = decel;
    g_punchMode = PL_RATE;
}
void audioPunchReverseStart() { punchRateStart(-1.0f, 0.0f); }
void audioPunchHalfSpeedStart() { punchRateStart(0.5f, 0.0f); }
// Multiplicative per-frame decay: the rate reaches ~1% after `seconds` ("motor spinning down").
void audioPunchTapeStopStart(float seconds) { punchRateStart(1.0f, powf(0.01f, 1.0f / (seconds * (float)kSampleRate))); }
void audioPunchStop() { g_punchMode = PL_NONE; }

void audioPunchCrush(int bits, int hold) {
    g_crushBits = bits < 1 ? 1 : (bits > 16 ? 16 : bits);
    g_crushHold = hold < 1 ? 1 : hold;
}

// ---- USB audio card (ESP32 only) ----
// A REAL USB Audio Class (UAC) microphone device — the host OS sees a native sound card,
// no python bridge script needed. Replaces an earlier piggybacked-CDC approach (same idea
// as a reference project the user pointed at, TYPE-2: mirror audio over the existing
// Serial/CDC link with a sync-marker protocol). That approach's bulk-transfer bandwidth
// measured a hard ceiling around ~7-10KB/s on this chip regardless of tuning (chunk size,
// producer CPU load, baud rate — none of it moved the needle), forcing a lossy 7350Hz/
// 8-bit mono compromise that still sounded "dégeu". A UAC device's isochronous endpoint
// has guaranteed, reserved bandwidth (~1MB/s on full-speed USB) instead of competing bulk
// traffic, easily covering full 44.1kHz/16-bit stereo (176KB/s) with huge margin — the
// right fix once it was clear the bottleneck wasn't anything tunable on this end.
//
// usbAudioCardTick() is called from i2s.c's esp_fill_audio_buffer_task() — a HIGH-PRIORITY
// REAL-TIME FreeRTOS task. This is safe to call directly here (unlike the old CDC bridge's
// Serial.write(), which could spin for up to its tx_timeout_ms under backpressure and once
// caused a real freeze bug): TinyUSB's tud_audio_write() just pushes into its own internal
// FIFO (own header: "call in I2S receive callback", i.e. designed for exactly this kind of
// real-time-task call site) and returns immediately — if the host isn't draining fast
// enough the FIFO simply drops/truncates, it never blocks the caller.
#ifndef SIMULATOR
#include "USBAudioCard.h"

namespace {
USBAudioCard g_usbAudioCard(44100, UAC_BPS_16, UAC_SPK_NONE, UAC_MIC_STEREO);

// TEMP DBG: testing whether stale data sitting in the IN FIFO from before the host
// activates the mic interface (we write() continuously from boot regardless of host
// state) is what corrupts the very first isochronous packets — gate writes on the real
// interface-enable event instead of writing unconditionally from boot.
volatile bool g_micActive = false;

void usbAudioCardOnEvent(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (id == ARDUINO_USB_AUDIO_CARD_INTERFACE_ENABLE_EVENT) {
        auto *p = (arduino_usb_audio_card_event_data_t *)data;
        if (p->interface_enable.interface == UAC_INTERFACE_MIC) {
            g_micActive = p->interface_enable.enable;
        }
    }
}
}

void usbAudioCardBegin() {
    g_usbAudioCard.onEvent(usbAudioCardOnEvent);
    g_usbAudioCard.begin();
}

extern "C" void usbAudioCardTick(const int16_t *stereoBlock, int frames) {
    if (!g_micActive) return;  // TEMP DBG: don't write until the host has actually activated the stream
    g_usbAudioCard.write(stereoBlock, (uint16_t)(frames * 2 * sizeof(int16_t)));
}
#else
// Only called from i2s.c/main.cpp, both ESP32-only (i2s.c isn't part of the simulator's
// own build at all — see simulator/CMakeLists.txt's AMY_SOURCES) — these branches never
// actually run, kept only so the symbols still exist if anything else ever references them.
void usbAudioCardBegin() {}
extern "C" void usbAudioCardTick(const int16_t *stereoBlock, int frames) {
    (void)stereoBlock;
    (void)frames;
}
#endif

// ==========================================================================
// Generated no-op stubs for the rest of audio_engine.h's surface. Filled in phase by
// phase (Phase 3: core synth group: audioSetFilter*/audioSetReverb* etc.; Phase 4: sample/
// key playback; Phase 5: TB-303/I303/SW2; Phase 6: drums/pads; Phase 7: modular/wavetable;
// Phase 8: FELU/PRESETS; Phase 9: sequencer migration) — see mutable-nibbling-melody.md.
// Regenerate this block with /tmp/gen_stubs.py if audio_engine.h's signatures change.
// ==========================================================================
void audioSetShapeOnSynth(SynthShape shape, uint8_t synthCh) {
    (void)shape;
    (void)synthCh;
}

void audioTrackerInit() {

}

void audioTrackerNoteOn(uint8_t trackIdx, uint8_t midiNote, float vel) {
    (void)trackIdx;
    (void)midiNote;
    (void)vel;
}

void audioTrackerNoteOff(uint8_t trackIdx, uint8_t midiNote) {
    (void)trackIdx;
    (void)midiNote;
}

void audioRestoreShapeFilter(SynthShape shape) {
    (void)shape;
}

void audioSetFilter(float cutoffHz, float resonance) {
    (void)cutoffHz;
    (void)resonance;
}

void audioSetAllFilters(float cutoffHz, float resonance) {
    (void)cutoffHz;
    (void)resonance;
}

void audioSetAllFiltersT(float cutoffHz, float resonance, uint8_t filterType) {
    (void)cutoffHz;
    (void)resonance;
    (void)filterType;
}

// Generic, live cutoff/resonance control for the CURRENT SYNTH shape — was a pure no-op
// stub, confirmed as the reason "SYNTH > SAWFM: je n'arrive pas à changer la texture via
// DEPTH et CUTOFF" and, more broadly, why main.cpp's own pot-dispatch code had to treat
// P4-P7 as "genuinely control nothing" for every shape except SAW_FM's own special case
// ("j'aimerais que plus d'instruments puissent se régler quand c'est possible"). Touches
// the same N_FILTER node audioSetShape()'s kShapeTable sets a per-shape baseline on, so
// this is a live override on top of that baseline, not a separate path.
void audioSetFilterFreq(float cutoffHz, float resonance) {
    if (!g_mainReady) return;
    g_mainEngine.set_param(N_FILTER, FLT_CUTOFF, hzToPitchUnits(cutoffHz));
    // resonance follows this codebase's own 1..4-ish convention (e.g. T303's t303Reso =
    // 1 + pot*2..3) — anchor resonance=1 (a pot at its own zero) to resoQ=0 (gentle), not
    // resoQ=0.25: that offset left LADDER's k=3.9*resoQ already at ~1.0 (near self-
    // oscillation) even at the pot's lowest position, confirmed directly — cutoff barely
    // changed the sound at all (peak/zero-crossing both nearly flat across the pot's full
    // sweep) because a near-self-oscillating resonant peak was dominating regardless of
    // where the cutoff itself sat.
    float resoQ = (resonance - 1.0f) / 3.0f;
    if (resoQ < 0.0f) resoQ = 0.0f; else if (resoQ > 1.0f) resoQ = 1.0f;
    g_mainEngine.set_param(N_FILTER, FLT_RES, (int32_t)(resoQ * 32767.0f));
}

void audioSetGranular2FilterFreq(float cutoffHz, float resonance) {
    (void)cutoffHz;
    (void)resonance;
}

void audioSetPCMFilter(float cutoffHz, float resonance) {
    (void)cutoffHz;
    (void)resonance;
}

// SAW_FM's own DEPTH+CUTOFF pots — cutoff/resonance go through the same generic
// audioSetFilterFreq() path. Only meaningful while N_OSCX is actually active
// (audioSetShape() mutes it for every shape but SAW_FM/SUPERSAW) — harmless no-op
// otherwise since set_param on a muted node just never reaches the ear.
//
// DEPTH drives BOTH of FM2's own params, not just OSCX_TIMBRE (modulation index) alone —
// confirmed directly (measured zero-crossing rate 24->83->177->298 across depth 0->1,
// a strong, real, monotonic brightness increase with no glitches) that index alone
// already works; the complaint ("la modification de texture n'est pas ouf, contrairement
// au P2 des 303") is a character/range one, not a wiring bug: index alone only ever
// brightens the SAME carrier:modulator ratio, a one-dimensional "more sidebands" sweep.
// T303's P2 (wavefold) feels more dramatic because it sweeps drive AND mix together, two
// dimensions at once. OSCX_MORPH (FM2's frequency ratio, 0.5x..12x, osc_engines.cpp's own
// 16-step table) is FM's actual "flavor" control — low ratios stay closer to harmonic/
// musical, high ratios go inharmonic/bell-like/clangorous. Sweeping it alongside TIMBRE
// gives one knob a qualitative character shift on top of the brightness one, not just a
// louder version of the same tone.
void audioSetFmParams(float depth, float cutoffHz, float resonance) {
    if (!g_mainReady) return;
    int32_t timbre = (int32_t)(depth * 32767.0f);
    if (timbre < 0) timbre = 0; else if (timbre > 32767) timbre = 32767;
    g_mainEngine.set_param(N_OSCX, OSCX_TIMBRE, timbre);
    g_mainEngine.set_param(N_OSCX, OSCX_MORPH, timbre);
    audioSetFilterFreq(cutoffHz, resonance);
}

void audioSetFmDepth(float depth) {
    if (!g_mainReady) return;
    int32_t timbre = (int32_t)(depth * 32767.0f);
    if (timbre < 0) timbre = 0; else if (timbre > 32767) timbre = 32767;
    g_mainEngine.set_param(N_OSCX, OSCX_TIMBRE, timbre);
    g_mainEngine.set_param(N_OSCX, OSCX_MORPH, timbre);
}

// level 0..1.5 (fxList's own declared range; UI's "Lvl" doubles as mix) -> RVB_MIX.
// liveness 0.5..1 -> RVB_DECAY directly (q15 fraction). damping/xover_hz (0..1 /
// 500..8000Hz) are literal Hz targets here, so they're converted to the module's
// pitch-unit params (MIDI note * 256) via the standard log2 note formula.
void audioSetReverb(float level, float liveness, float damping, float xover_hz) {
    g_reverbOn = level > 0.0001f;
    if (!g_fxReverb) return;
    if (!g_reverbOn) { g_fxReverb->set_param(RVB_MIX, 0); return; }
    g_fxReverb->set_param(RVB_MIX, (int32_t)(constrain(level, 0.0f, 1.0f) * kUnity));
    g_fxReverb->set_param(RVB_DECAY, (int32_t)(constrain(liveness, 0.0f, 1.0f) * kUnity));
    float dampHz = 12000.0f - constrain(damping, 0.0f, 1.0f) * 10000.0f;   // 1=dark(2000Hz)..0=bright(12000Hz)
    g_fxReverb->set_param(RVB_DAMP, (int32_t)((69.0f + 12.0f * log2f(dampHz / 440.0f)) * 256.0f));
    g_fxReverb->set_param(RVB_BANDWIDTH, (int32_t)((69.0f + 12.0f * log2f(xover_hz / 440.0f)) * 256.0f));
}

// Chorus's LFO rate/depth are fixed internally (no native per-param control) — lfo_freq
// has no mapping; depth instead picks among the module's 3 fixed voicings (I/II/I+II),
// a documented simplification rather than a literal 1:1 param.
void audioSetChorus(float level, float lfo_freq, float depth) {
    (void)lfo_freq;
    g_chorusOn = level > 0.0001f;
    if (!g_fxChorus) return;
    if (!g_chorusOn) { g_fxChorus->set_param(CHR_MODE, CHRM_OFF); return; }
    int mode = depth > 0.66f ? CHRM_I_II : depth > 0.33f ? CHRM_II : CHRM_I;
    g_fxChorus->set_param(CHR_MODE, mode);
    g_fxChorus->set_param(CHR_MIX, (int32_t)(constrain(level, 0.0f, 1.0f) * kUnity));
}

// FLANGER used to route through AMY's config_chorus() — confirmed dead: nothing in this
// build's render path (audioEngineRenderBlock(), the new engine's own render+mix
// pipeline) ever calls AMY's render/mix functions any more, so that call only mutated
// AMY state nobody ever reads. Replaced with the engine's own Flanger module. FLG_RATE
// is "pitch units relative to 1Hz" (fx2_modules.cpp's own lfo_inc() comment) — a
// DIFFERENT convention from the "MIDI-note*256 relative to A4=440Hz" units this file
// uses for absolute-pitch params elsewhere (RVB_DAMP, SFT_FREQ, etc.) — do not reuse
// that other formula here. max_delay_ms is remapped from the UI's 5-100ms range onto
// ~0.5-20ms: Flanger::init()'s own delay-line room is only ~22ms (0.022*kSampleRate+8
// samples), and DelayLine::read()'s single wraparound correction does not bounds-check
// a request past that — confirmed by reading it directly, a real out-of-bounds read,
// not just an audio-quality clamp.
void audioSetFlanger(float level, float lfo_freq, float depth, float max_delay_ms, bool on) {
    g_flangerOn = on;
    if (!g_fxFlanger) return;
    if (!on) { g_fxFlanger->set_param(FLG_MIX, 0); return; }
    g_fxFlanger->set_param(FLG_MIX, (int32_t)(constrain(level, 0.0f, 1.0f) * kUnity));
    float hz = constrain(lfo_freq, 0.02f, 100.0f);
    g_fxFlanger->set_param(FLG_RATE, (int32_t)(3072.0f * log2f(hz)));
    g_fxFlanger->set_param(FLG_DEPTH, (int32_t)(constrain(depth, 0.0f, 1.0f) * kUnity));
    float dlyMs = constrain(max_delay_ms, 5.0f, 100.0f);
    float remappedMs = 0.5f + (dlyMs - 5.0f) / (100.0f - 5.0f) * (20.0f - 0.5f);
    g_fxFlanger->set_param(FLG_DELAY, (int32_t)(remappedMs * 16.0f));
}

// Shared by DELAY (fxList slot 5) and RES (slot 13, audioSetResDelay() below) — same
// signature/units, different Module instance each, per fxList's own "the two sound
// genuinely different" intent (see audioSetResDelay()'s declaration comment).
static void setDelayModule(Module *m, bool &onFlag, float level, float delay_ms, float feedback, float filter_coef) {
    onFlag = level > 0.0001f;
    if (!m) return;
    if (!onFlag) { m->set_param(DLY_MIX, 0); return; }
    m->set_param(DLY_MIX, (int32_t)(constrain(level, 0.0f, 1.0f) * kUnity));
    m->set_param(DLY_TIME, (int32_t)(delay_ms * 16.0f));   // native units: ms*16
    m->set_param(DLY_FEEDBACK, (int32_t)(constrain(feedback, 0.0f, 1.0f) * 31000.0f));
    // filter_coef (-1..1, this FX's "Tone") -> a damping cutoff: -1=dark(1000Hz)..1=bright(15000Hz)
    float hz = 8000.0f + constrain(filter_coef, -1.0f, 1.0f) * 7000.0f;
    m->set_param(DLY_DAMP, (int32_t)((69.0f + 12.0f * log2f(hz / 440.0f)) * 256.0f));
}
void audioSetDelay(float level, float delay_ms, float feedback, float filter_coef) {
    setDelayModule(g_fxDelayMod, g_fxDelayOn, level, delay_ms, feedback, filter_coef);
}
void audioSetResDelay(float level, float delay_ms, float feedback, float filter_coef) {
    setDelayModule(g_fxResDelayMod, g_resDelayOn, level, delay_ms, feedback, filter_coef);
}

// Own Delay instance (NOT TunedComb — tried first, measured directly as nearly
// inaudible: TunedComb's CMB_TUNE is a musical-pitch delay length, ~ms-scale at most;
// feeding it a 100-700ms tempo-synced echo time converts to a 1-10Hz "pitch", far
// outside its designed range and effectively a no-op). A Delay at high feedback gives
// the genuinely self-sustaining, resonant-sounding echo this FX actually wants. wild
// scales how much of `feedback` reaches the delay's own feedback param (0=gentle
// quick decay matching RES's character, 1=near-self-oscillating), matching this FX's
// existing "Wild" intent exactly.
void audioSetResoEcho(float level, float feedback, float damp, float wild, float delay_ms, bool on) {
    g_combOn = on;
    if (!g_fxResoDelayMod) return;
    if (!on) { g_fxResoDelayMod->set_param(DLY_MIX, 0); return; }
    g_fxResoDelayMod->set_param(DLY_MIX, (int32_t)(constrain(level, 0.0f, 1.5f) / 1.5f * kUnity));
    g_fxResoDelayMod->set_param(DLY_TIME, (int32_t)(delay_ms * 16.0f));
    float fb = constrain(feedback, 0.0f, 1.0f) * (0.3f + constrain(wild, 0.0f, 1.0f) * 0.7f);
    g_fxResoDelayMod->set_param(DLY_FEEDBACK, (int32_t)(fb * 31000.0f));
    float dampHz = 8000.0f - constrain(damp, 0.0f, 1.0f) * 7200.0f;   // 1=dark(800Hz)..0=bright(8000Hz)
    g_fxResoDelayMod->set_param(DLY_DAMP, (int32_t)((69.0f + 12.0f * log2f(dampHz / 440.0f)) * 256.0f));
}

// type (0=Overdrive,1=Distort,2=Fuzz,3=Destrukt) only varies the pre-gain curve here —
// a coarser approximation than the old 4 genuinely distinct AMY algorithms, scoped down
// given the master FX chain itself (see its own comment) is new, hand-rolled DSP, not a
// port of AMY's own drive code.
// Reported "OVRD ne fait rien ou augmente simplement le son": confirmed directly — two
// real bugs, not a perception issue. (1) "Tone" was literally `x *= tone`, a second
// amplitude multiply stacked on top of "Lvl" with zero actual tone-shaping, so the whole
// knob just changed loudness. Fixed: Tone now sweeps a real post-distortion SVF lowpass
// (same core as FILT/LADDER) from a dark ~400Hz tilt (tone=0) up to a near-full-bandwidth
// ~16kHz pass (tone=1) — the harmonics the saturation stage generates are genuinely
// filtered, not just scaled. (2) kTypeGain's pre-gain multipliers were too small to ever
// drive this engine's typical signal level (~0.08 normalized for a single held note,
// measured directly — most content never gets near full scale) into tanh's visibly
// curving region: OVRD's old 2.0 gave a max pre-gain of only 3x even at Drv=1.0 (0.08*3=
// 0.24, tanh(0.24)=0.236 — indistinguishable from clean). Rescaled 5x across all four
// types so default Drv=0.5 already lands OVRD in clearly-audible warmth territory
// (0.08*6=0.48, tanh=0.45) while DSTR at Drv=1.0 still reaches full hard-clip/fuzz.
void audioSetDrive(float drive, float tone, float level, uint8_t type, bool on) {
    static const float kTypeGain[4] = {10.0f, 25.0f, 45.0f, 70.0f};
    g_driveOn = on;
    g_driveAmt = drive * kTypeGain[type < 4 ? type : 0];
    g_driveLevel = level;
    if (on) {
        float toneHz = 400.0f * powf(40.0f, constrain(tone, 0.0f, 1.0f));
        setSvfCutoffRes(g_driveToneCoef, toneHz, 0.7f);
    }
}

// low/mid/high (0..4, 1.0=flat — this FX's own existing convention) converted to the
// module's dB-tenths units (20*log10(gain), clamped to its own declared ±15dB range).
void audioSetEq(float low, float mid, float high) {
    g_eqOn = !(fabsf(low - 1.0f) < 0.001f && fabsf(mid - 1.0f) < 0.001f && fabsf(high - 1.0f) < 0.001f);
    if (!g_fxEq3) return;
    auto toDbTenths = [](float gain) -> int32_t {
        float g = gain < 0.001f ? 0.001f : gain;
        return (int32_t)constrain(20.0f * log10f(g) * 10.0f, -150.0f, 150.0f);
    };
    g_fxEq3->set_param(EQ_LOW, toDbTenths(low));
    g_fxEq3->set_param(EQ_MID, toDbTenths(mid));
    g_fxEq3->set_param(EQ_HIGH, toDbTenths(high));
}

// Shared by REP and BITCRS (fxList slots 8-9) — both always called through this one
// function already (see their own comment in main.cpp: BITCRS is "wavefold at extreme
// gain to simulate bit-depth reduction", not a separate crusher), so one Shaper pair
// (SHPM_FOLD) backs both, preserving today's existing conflated behavior rather than
// introducing a new, different-sounding dedicated bit-crusher. gain>1 catches both an
// active FX and MODE_POKEMON's own baseline wavefold (baseWf, applied even with the FX
// off — see main.cpp's own case 8/9) uniformly, since both route through this same gain.
void audioSetWavefold(float gain) {
    g_foldOn = gain > 1.001f;
    if (!g_fxFoldL || !g_fxFoldR) return;
    if (!g_foldOn) { g_fxFoldL->set_param(SHP_MIX, 0); g_fxFoldR->set_param(SHP_MIX, 0); return; }
    int32_t drive = (int32_t)(constrain(gain, 1.0f, 128.0f) * 256.0f);
    for (Module *m : {g_fxFoldL, g_fxFoldR}) {
        m->set_param(SHP_MODE, SHPM_FOLD);
        m->set_param(SHP_DRIVE, drive);
        m->set_param(SHP_MIX, kUnity);
    }
}

void audioSetLadderFilter(float cutoffHz, float resonance, bool on) {
    g_ladderOn = on;
    if (on) setSvfCutoffRes(g_ladderCoef, cutoffHz, resonance);
}

void audioSetFiltBus(float cutoffHz, float resonance, uint8_t type, bool on) {
    g_filtOn = on;
    g_filtMode = (type == 1) ? FLTM_HP : (type == 2) ? FLTM_BP : FLTM_LP;
    if (on) setSvfCutoffRes(g_filtCoef, cutoffHz, resonance);
}

// Shifter mode 0 = literal carrier multiply — true ring modulation (the engine's own
// RINGMOD was previously conflated in spirit with TREMOLO's volume-LFO, but that's a
// control-rate effect, far too coarse for a carrier in the 20-2000Hz audio range this
// FX actually needs; Shifter is the correct engine-native fit for genuine ring mod).
void audioSetRingmod(float freqHz, float mix, bool on) {
    g_ringmodOn = on;
    if (!g_fxShifter) return;
    if (!on) { g_fxShifter->set_param(SFT_MIX, 0); return; }
    g_fxShifter->set_param(SFT_MODE, 0);
    float hz = constrain(freqHz, 20.0f, 8000.0f);
    g_fxShifter->set_param(SFT_FREQ, (int32_t)((69.0f + 12.0f * log2f(hz / 440.0f)) * 256.0f));
    g_fxShifter->set_param(SFT_MIX, (int32_t)(constrain(mix, 0.0f, 1.0f) * kUnity));
}

// threshold (0.05..1, a linear fraction-of-peak) -> CMP_THRESH dB via 20*log10().
// ratio (1..20, already "x:1") -> CMP_RATIO's own x10 native units. Attack/release/
// makeup aren't exposed in this FX's UI — left at the module's own fixed defaults,
// matching this FX's existing "fixed 5ms/80ms attack/release" documented behavior.
void audioSetCompressor(float threshold, float ratio, bool on) {
    g_compOn = on;
    if (!g_fxComp) return;
    if (!on) { g_fxComp->set_param(CMP_MIX, 0); return; }
    float thr = constrain(threshold, 0.001f, 1.0f);
    g_fxComp->set_param(CMP_THRESH, (int32_t)constrain(20.0f * log10f(thr), -60.0f, 0.0f));
    g_fxComp->set_param(CMP_RATIO, (int32_t)constrain(ratio * 10.0f, 10.0f, 200.0f));
    // The module's own default makeup gain is +6dB — confirmed directly this masked the
    // actual gain reduction entirely in testing (a loud signal came out ~2x LOUDER, not
    // quieter). This FX's UI has no makeup control, so force it to 0dB rather than
    // inherit a hidden, surprising boost.
    g_fxComp->set_param(CMP_MAKEUP, 0);
    g_fxComp->set_param(CMP_MIX, kUnity);
}

void audioLoadDrumSamples() {

}

void audioPlayDrumPad(uint8_t col, float vel) {
    // col follows DRUM2's own pad convention (KK1/SN1/HHC/CLP/BNG/OHH/CHH/RDE,
    // kDrum2Labels in main.cpp — NOT DRUMS_BD..OH, confirmed via main.cpp:1867's own
    // comment: "Reuses DRUM2's own 8 pads"). This is pad-blob slots 0-7, set up 1:1 by
    // kPadSetup above — no extra mapping needed here any more.
    uint8_t pad = (col < 8) ? col : 0;
    triggerPadTuned(pad, vel, 0, 0.0f);
}

const char* audioDrumPadLabel(uint8_t col) {
    // Stub default was nullptr, matching the other stubs' "safe" return — but this one
    // feeds straight into oled.drawStr() with no null-check (confirmed via a real
    // segfault in the mode sweep, same bug class as audioCrunchSlotName below) and
    // crashes. Any const-char*-returning stub that a mode draws unconditionally needs a
    // non-null placeholder, not nullptr.
    (void)col;
    return "-";
}

void audioLoadCrunchSamples() {

}

void audioLoadCrunchNativeInstruments() {

}

void audioCrunchSetTrackInstrument(uint8_t track, uint8_t slot) {
    (void)track;
    (void)slot;
}

uint8_t audioCrunchGetTrackInstrument(uint8_t track) {
    (void)track;
    return 0;
}

void audioCrunchSetTrackDecayMod(uint8_t track, float mod) {
    (void)track;
    (void)mod;
}

void audioCrunchSetTrackEnvelope(uint8_t track, uint8_t envIdx) {
    (void)track;
    (void)envIdx;
}

uint8_t audioCrunchResolveSampleIndex(uint8_t slot, uint8_t val0to11) {
    (void)slot;
    (void)val0to11;
    return 0;
}

bool audioCrunchSlotIsBank(uint8_t slot) {
    (void)slot;
    return false;
}

bool audioCrunchSlotIsNative(uint8_t slot) {
    (void)slot;
    return false;
}

const char* audioCrunchSlotName(uint8_t slot) {
    // nullptr crashed MODE_CRUNCH's screen (oled.drawStr(audioCrunchSlotName(...)) has
    // no null-check) — confirmed via a real segfault in the mode sweep. See
    // audioDrumPadLabel's own comment above — same bug class.
    (void)slot;
    return "-";
}

void audioCrunchNoteOn(uint8_t track, uint8_t val0to11, int8_t octave, float velocity) {
    (void)track;
    (void)val0to11;
    (void)octave;
    (void)velocity;
}

void audioCrunchNoteOff(uint8_t track, uint8_t note) {
    (void)track;
    (void)note;
}

void audioCrunchAllNotesOff() {

}

void audioStoneInit() {

}

void audioLoadStone(const char* path) {
    (void)path;
}

bool audioIsStoneReady() {
    return false;
}

void audioStoneNoteOn(uint8_t note, float velocity) {
    (void)note;
    (void)velocity;
}

void audioStoneNoteOff(uint8_t note) {
    (void)note;
}

void audioStoneAllNotesOff() {

}

void audioStoneFadeTick() {

}

bool audioComputeStoneWaveform(uint8_t* waveform128) {
    (void)waveform128;
    return false;
}

void audioStoneApplyWindow(float startFrac, float endFrac, bool loopMode) {
    (void)startFrac;
    (void)endFrac;
    (void)loopMode;
}

void audioStoneSetLoopMode(bool loop) {
    (void)loop;
}

bool audioStoneGetLoopMode() {
    return false;
}

void flashCacheInit() {

}

void flashCacheClear() {

}

void audioLoadGranularSource(const char* path) {
    (void)path;
}

bool audioIsGranularReady() {
    return false;
}

void audioSetGranularWindow(float startFrac, float endFrac) {
    (void)startFrac;
    (void)endFrac;
}

uint8_t audioComputeGranularSlices(uint8_t mode, uint8_t* waveform128) {
    (void)mode;
    (void)waveform128;
    return 0;
}

void audioApplyGranularSplits(float* splits, int N) {
    (void)splits;
    (void)N;
}

void audioPlayGranularSlice(uint8_t keyOscIdx, uint16_t slicePreset, float vel, bool loop) {
    (void)keyOscIdx;
    (void)slicePreset;
    (void)vel;
    (void)loop;
}

void audioStopGranularOsc(uint8_t keyOscIdx) {
    (void)keyOscIdx;
}

void audioUnloadGranular() {

}

void audioLoadGranular2Source(const char* path, uint8_t sampleIdx) {
    (void)path;
    (void)sampleIdx;
}

bool audioIsGranular2Ready(uint8_t sampleIdx) {
    (void)sampleIdx;
    return false;
}

uint8_t audioComputeGranular2Slices(uint8_t sampleIdx, uint8_t nSlices, uint8_t* waveform128) {
    (void)sampleIdx;
    (void)nSlices;
    (void)waveform128;
    return 0;
}

void audioApplyGranular2Splits(uint8_t sampleIdx, float* splits, int N, bool lopMode) {
    (void)sampleIdx;
    (void)splits;
    (void)N;
    (void)lopMode;
}

void audioPlayGranular2(uint8_t oscIdx, uint8_t sampleIdx, uint8_t sliceIdx, bool reverse, float vel, uint8_t playMode, uint16_t attackMs, uint32_t amyTime, uint8_t midiNote) {
    (void)oscIdx;
    (void)sampleIdx;
    (void)sliceIdx;
    (void)reverse;
    (void)vel;
    (void)playMode;
    (void)attackMs;
    (void)amyTime;
    (void)midiNote;
}

void audioPlayGranular2Ful(uint8_t oscIdx, uint8_t sampleIdx, bool reverse, float vel, float startFrac) {
    (void)oscIdx;
    (void)sampleIdx;
    (void)reverse;
    (void)vel;
    (void)startFrac;
}

void audioStopGranular2(uint8_t oscIdx) {
    (void)oscIdx;
}

bool audioGranular2HasReverse(uint8_t sampleIdx) {
    (void)sampleIdx;
    return false;
}

uint32_t audioGranular2SampleLenMs(uint8_t sampleIdx) {
    (void)sampleIdx;
    return 0;
}

void audioUnloadGranular2Slot(uint8_t sampleIdx) {
    (void)sampleIdx;
}

void audioUnloadGranular2() {

}

// amyWave carries AMY's own wave-type ids (SINE=0, PULSE=1, SAW_DOWN=2, SAW_UP=3,
// TRIANGLE=4, NOISE=5, see lib/AMY Synthesizer/src/amy.h) — t303AmyWave() in main.cpp
// still returns these, unchanged, so map them onto this engine's own WAVE_* ids here
// rather than touching every call site.
uint8_t t303WaveFromAmy(uint8_t amyWave) {
    switch (amyWave) {
        case 1: return WAVE_PULSE_;      // PULSE
        case 2: return WAVE_SAW_DOWN_;   // SAW_DOWN
        case 3: return WAVE_SAW_;        // SAW_UP
        case 4: return WAVE_TRI_;        // TRIANGLE
        case 5: return WAVE_NOISE_;      // NOISE
        default: return WAVE_SINE_;      // SINE
    }
}

void t303ApplyParams(float cutoff, float reso, float envMod) {
    if (!g_t303Role.ready) return;
    g_t303Engine.set_param(T3_FILTER, FLT_CUTOFF, hzToPitchUnits(cutoff));
    // 0.3..1.0 of full scale, not 0..1: even the TB-303's own lowest Reso setting has
    // clearly audible resonance, never a flat/neutral filter.
    float resoQ = (0.3f + 0.7f * (reso - 1.0f) / 2.0f);
    if (resoQ < 0.0f) resoQ = 0.0f; else if (resoQ > 1.0f) resoQ = 1.0f;
    g_t303Engine.set_param(T3_FILTER, FLT_RES, (int32_t)(resoQ * 32767.0f));
    if (g_t303FilterEdge >= 0) {
        float d = envMod / 10.0f;   // t303EnvMod's own range is 0..10 (pots[4].value*10)
        if (d < 0.0f) d = 0.0f; else if (d > 1.0f) d = 1.0f;
        g_t303Engine.set_edge_depth(g_t303FilterEdge, (q15)(d * 32767.0f));
    }
}

void audioT303Init(float cutoff, float reso, float envMod, float decay, uint8_t amyWave) {
    if (!g_t303Role.ready) return;
    t303ApplyParams(cutoff, reso, envMod);
    g_t303Engine.set_param(T3_ENV, ENV_DECAY, (int32_t)decay);
    g_t303Engine.set_param(T3_OSC, OSC_WAVE, t303WaveFromAmy(amyWave));
}

void audioT303NoteOn(uint8_t midiNote, float vel) {
    if (!g_t303Role.ready) return;
    int vel127 = (int)(vel * 127.0f);
    if (vel127 < 1) vel127 = 1;
    if (vel127 > 127) vel127 = 127;
    // decay can reach ~3000ms (t303Decay = 30 * 100^pot) — same "cover the longest
    // built-in decay plus release/tail margin" approach as the drum roles' own window.
    g_t303Role.activeUntilMs = millis() + 3300;
    g_t303Engine.note_on(midiNote, vel127);
}

void audioT303NoteOff(uint8_t midiNote) {
    if (g_t303Role.ready) g_t303Engine.note_off(midiNote);
}

void audioT303Params(float cutoff, float reso, float envMod, float decay) {
    if (!g_t303Role.ready) return;
    t303ApplyParams(cutoff, reso, envMod);
    g_t303Engine.set_param(T3_ENV, ENV_DECAY, (int32_t)decay);
}

void audioT303SetAmpEnv(float atkMs, float sus, float relMs) {
    if (!g_t303Role.ready) return;
    g_t303Engine.set_param(T3_ENV, ENV_ATTACK, (int32_t)atkMs);
    g_t303Engine.set_param(T3_ENV, ENV_SUSTAIN, (int32_t)(sus * 32767.0f));
    g_t303Engine.set_param(T3_ENV, ENV_RELEASE, (int32_t)relMs);
}

// I303 shares this same T303 voice (see its own comment) rather than a dedicated 6-voice
// graph — decMs folded into ENV_DECAY, same as audioT303Params.
void audioI303SetAmpEnv(float atkMs, float sus, float decMs, float relMs) {
    if (!g_t303Role.ready) return;
    g_t303Engine.set_param(T3_ENV, ENV_ATTACK, (int32_t)atkMs);
    g_t303Engine.set_param(T3_ENV, ENV_DECAY, (int32_t)decMs);
    g_t303Engine.set_param(T3_ENV, ENV_SUSTAIN, (int32_t)(sus * 32767.0f));
    g_t303Engine.set_param(T3_ENV, ENV_RELEASE, (int32_t)relMs);
}

void audioT303SetSustain(float sustain) {
    if (g_t303Role.ready) g_t303Engine.set_param(T3_ENV, ENV_SUSTAIN, (int32_t)(sustain * 32767.0f));
}

// Same technique as audioSetPitchBend() (SYNTH's own joystick bend): ratio is a frequency
// multiplier (2^(semitones/12)), added live to OSC_PITCH's otherwise-unused flat
// baseline — see that function's own comment for why this needs no graph change. This is
// what main.cpp's own extensive slide/bend call sites already assume.
void audioT303PitchBend(float ratio) {
    if (!g_t303Role.ready) return;
    float semis = (ratio > 0.0f) ? 12.0f * log2f(ratio) : 0.0f;
    g_t303Engine.set_param(T3_OSC, OSC_PITCH, kT303CenterPitch + (int32_t)lroundf(semis * 256.0f));
}

void audioT303Wave(uint8_t amyWave) {
    if (g_t303Role.ready) g_t303Engine.set_param(T3_OSC, OSC_WAVE, t303WaveFromAmy(amyWave));
}

// Poly 6-voice I303 is not implemented — this engine's own T303 voice is kT303Voices=2,
// shared with 303S/303S2/SS2/GEST/GROOVE/DRUM2's T303 lane; I303 reuses it rather than
// staying silent, so only its first ~2 simultaneous notes actually sound, not all 6. A
// real dedicated poly voice is a separate, bigger task than "la 303S ne fait pas de
// sons" asked for.
void audioI303Init(float cutoff, float reso, float envMod, float decay, uint8_t amyWave) {
    audioT303Init(cutoff, reso, envMod, decay, amyWave);
}

void audioT303Feedback(float fb) {
    (void)fb;   // no equivalent DSP wired yet — the ladder's own feedback (k) comes from Reso, not a separate knob
}

void audioT303Duty(float duty) {
    if (!g_t303Role.ready) return;
    int32_t pw = (int32_t)(duty * 32767.0f);
    if (pw < 1024) pw = 1024; else if (pw > 31744) pw = 31744;
    g_t303Engine.set_param(T3_OSC, OSC_PW, pw);
}

// depth 0..1 (P2's own pot range) -> SHP_DRIVE (how hard the fold bites, up to 3 octaves
// of input gain) and SHP_MIX (dry/wet — 0 at depth=0 so the pot's bottom end is a clean
// passthrough, not a flat SHPM_FOLD coloration). Explicitly sets SHP_MODE=SHPM_FOLD every
// call (not just once at graph build time) so switching P1's wave away from TRI2/SAW2 — and
// back to any wave that routes P2 here (PULSE/NAP/default) — can't leave the shaper stuck
// in SHPM_DIODE from a previous audioT303WavefoldAsym() call.
// DRIVE 1x..10x (was 1x..4x — reported "pas assez d'effet / trop subtil" after the
// asymmetric-fold fix; measured directly that the shaper's own effect, isolated from the
// filter, was barely audible at the old 4x ceiling: zc 22->20 over the full depth range
// with cutoff wide open/resonance flat). The Shaper module itself doesn't clamp SHP_DRIVE
// to its own ModuleInfo-declared range (that's documentation for a generic UI slider, not
// an internal limit — set_param() just stores whatever value it's given), so there's no
// DSP-side ceiling stopping a louder drive here.
// --- DISTO (6th t303Patch, see t303CyclePatch() in main.cpp) --------------------------
// First attempt used SHPM_DIODE (asymmetric: the negative half is driven at a REDUCED,
// scaled-down gain before its own tanh) — reported directly as muffling the acid sound
// instead of sharpening it, which tracks: half the waveform never gets driven as hard as
// the other, so the overall result is quieter/duller, not sharper. SHPM_TANH is symmetric
// (same curve both polarities, no half-wave attenuation) — driven hard, it's the standard
// warm-but-aggressive analog-style saturation character, closer to what driving a real
// 303 past clean headroom (or the well-known diode-clipper overdrive mod many 303 clones
// get) actually sounds like: bright and biting, not smothered.
// This is now a dedicated PATCH (one of 6 selectable T303 voicings, t303CyclePatch() in
// main.cpp), not a blendable FX — so unlike P2's texture knob below, mix commits fully
// (1.0) rather than leaving a clean sliver; selecting a different patch cleanly reverts.
// audioT303Wavefold()/audioT303WavefoldAsym() both check g_t303DistoOn FIRST and, if set,
// apply this fixed drv/mix instead of their own depth-driven logic — so every one of their
// ~15 call sites across main.cpp (P1-wave dispatch, P2-texture pot deltas, mode-entry
// inits) keeps re-asserting DISTO for free, with zero changes to any of those call sites.
static bool  g_t303DistoOn  = false;
static float g_t303DistoDrv = 16.0f;   // 1x..20x
static float g_t303DistoMix = 1.0f;    // 0..1

// Pushes the override (or reverts to depth=0's clean passthrough) into T3_SHAPE
// immediately, not just the flag — several of the ~15 Wavefold/WavefoldAsym call sites
// only fire on a wave switch or mode entry, or are gated on the P2 pot actually moving, so
// toggling DISTO from patch-select can't rely on one of them re-firing soon on its own.
void audioT303SetDisto(bool on, float drv, float mix) {
    g_t303DistoOn  = on;
    g_t303DistoDrv = constrain(drv, 1.0f, 20.0f);
    g_t303DistoMix = constrain(mix, 0.0f, 1.0f);
    if (!g_t303Role.ready) return;
    if (on) {
        g_t303Engine.set_param(T3_SHAPE, SHP_MODE, SHPM_TANH);
        g_t303Engine.set_param(T3_SHAPE, SHP_DRIVE, (int32_t)(g_t303DistoDrv * 256.0f));
        g_t303Engine.set_param(T3_SHAPE, SHP_MIX, (int32_t)(g_t303DistoMix * 32767.0f));
    } else {
        g_t303Engine.set_param(T3_SHAPE, SHP_MODE, SHPM_FOLD);
        g_t303Engine.set_param(T3_SHAPE, SHP_DRIVE, 256);
        g_t303Engine.set_param(T3_SHAPE, SHP_MIX, 0);
    }
}

void audioT303Wavefold(float depth) {
    if (!g_t303Role.ready) return;
    if (g_t303DistoOn) {
        g_t303Engine.set_param(T3_SHAPE, SHP_MODE, SHPM_TANH);
        g_t303Engine.set_param(T3_SHAPE, SHP_DRIVE, (int32_t)(g_t303DistoDrv * 256.0f));
        g_t303Engine.set_param(T3_SHAPE, SHP_MIX, (int32_t)(g_t303DistoMix * 32767.0f));
        return;
    }
    if (depth < 0.0f) depth = 0.0f; else if (depth > 1.0f) depth = 1.0f;
    g_t303Engine.set_param(T3_SHAPE, SHP_MODE, SHPM_FOLD);
    g_t303Engine.set_param(T3_SHAPE, SHP_DRIVE, 256 + (int32_t)(depth * 9.0f * 256.0f));
    g_t303Engine.set_param(T3_SHAPE, SHP_MIX, (int32_t)(depth * 32767.0f));
}

// Genuine asymmetric distortion for TRI2/SAW2's own P2 mapping, not a relabeled symmetric
// fold. Was previously a pure alias of audioT303Wavefold() (SHPM_FOLD, symmetric sine-fold
// — identical for +v and -v) — reported directly: "le P2 qui change la forme d'onde dans
// 303S n'a pas l'air d'impacter le son correctement, ça change bel et bien la texture mais
// il y a qqch qui ne va pas", matching this function's own prior comment admitting "a true
// asymmetric fold is a DSP addition, not wired here". SHPM_DIODE (synth_modules.cpp) applies
// a different tanh curve to each polarity — a classic diode-clipper asymmetry, the vendored
// Shaper's only mode that actually treats +v/-v differently — giving TRI2/SAW2 a real
// asymmetric character distinct from the other waves' symmetric fold, instead of the exact
// same effect under a different name.
// DRIVE 1x..10x, same reasoning as audioT303Wavefold() above.
void audioT303WavefoldAsym(float depth) {
    if (!g_t303Role.ready) return;
    if (g_t303DistoOn) {
        g_t303Engine.set_param(T3_SHAPE, SHP_MODE, SHPM_TANH);
        g_t303Engine.set_param(T3_SHAPE, SHP_DRIVE, (int32_t)(g_t303DistoDrv * 256.0f));
        g_t303Engine.set_param(T3_SHAPE, SHP_MIX, (int32_t)(g_t303DistoMix * 32767.0f));
        return;
    }
    if (depth < 0.0f) depth = 0.0f; else if (depth > 1.0f) depth = 1.0f;
    g_t303Engine.set_param(T3_SHAPE, SHP_MODE, SHPM_DIODE);
    g_t303Engine.set_param(T3_SHAPE, SHP_DRIVE, 256 + (int32_t)(depth * 9.0f * 256.0f));
    g_t303Engine.set_param(T3_SHAPE, SHP_MIX, (int32_t)(depth * 32767.0f));
}

void audioT303SetFilterType(uint8_t filterType) {
    if (!g_t303Role.ready) return;
    // AMY's own FILTER_LPF=1/FILTER_BPF=2/FILTER_HPF=3/FILTER_LPF24=4 (amy.h) — LPF24 is
    // the classic resonant-ladder 303 sound; the other three swap to a single-section SVF
    // of the matching mode instead of trying to approximate them on the ladder.
    if (filterType == 4) { g_t303Engine.set_param(T3_FILTER, FLT_ALGO, FLTA_LADDER); return; }
    g_t303Engine.set_param(T3_FILTER, FLT_ALGO, FLTA_SVF);
    g_t303Engine.set_param(T3_FILTER, FLT_MODE, filterType == 2 ? FLTM_BP : filterType == 3 ? FLTM_HP : FLTM_LP);
}

void audioSW2Init(float cutoff, float reso, float decay, uint8_t numVoices, uint8_t wave) {
    (void)cutoff;
    (void)reso;
    (void)decay;
    (void)numVoices;
    (void)wave;
}

void audioSW2Deactivate() {

}

void audioSW2SetBlend(float p2) {
    (void)p2;
}

void audioSW2NoteOn(uint8_t note, float vel) {
    (void)note;
    (void)vel;
}

void audioSW2NoteOff(uint8_t note) {
    (void)note;
}

void audioSW2AllNotesOff() {

}

// midiNote and decayMs both apply per hit via triggerPadTuned/kPadNeutralPitch/kPadDecay
// above.
void audioDrum2Hit(uint8_t padIdx, float vel, uint8_t midiNote, float decayMs) {
    int pad = padIdx < 8 ? padIdx : 0;
    triggerPadTuned(pad, vel, (int32_t)midiNote - (int32_t)kPadNeutralPitch[pad], decayMs);
}

// pad is DRUMS2/3's own convention (config.h's DRUMS_BD/SD/LT/HT/RS/CP/CH/OH) — pad-blob
// slots 8-15 (kPadSetup), offset +8 from pad-blob slots 0-7's DRUM2 convention. kit
// (808/909) still picks the same samples either way, same simplification as before.
// Pitch and decay per hit, same as audioDrum2Hit above.
void audioDrumSynthHit(uint8_t kit, uint8_t pad, float vel, uint8_t pitch, float decayMs) {
    (void)kit;
    int padBlobIdx = (pad < 8 ? pad : 0) + 8;
    triggerPadTuned(padBlobIdx, vel, (int32_t)pitch - (int32_t)kPadNeutralPitch[padBlobIdx], decayMs);
}

void audioDrumSynthAllOff(uint8_t kit) {
    (void)kit;
    if (g_padsReady) padsSetParam(PADS_STOP, 1);
}

// ch is config.h's MOD3_OSCA_CH(14)/MOD3_OSCB_CH(15) — only OSCA drives the single real
// oscillator now (see buildModularGraph's own comment on the single-oscillator trade-off);
// OSCB's calls are accepted but otherwise do nothing, rather than main.cpp needing its own
// ch==MOD3_OSCB_CH guards at every call site.

void audioModularOscInit(uint8_t ch, uint8_t tableIdx) {
    if (!g_modRole.ready || ch != MOD3_OSCA_CH) return;
    if (tableIdx >= OSCX_ENGINES) tableIdx = OSCX_ENGINES - 1;
    g_modEngine.set_param(MOD_OSC, OSCX_ENGINE, tableIdx);
}

// Same underlying param as audioModularOscInit — two names survive from the original
// AMY-era API (OscInit at mode-entry/preset-load, SetTable for a live mid-note table swap).
void audioModularSetTable(uint8_t ch, uint8_t tableIdx) {
    audioModularOscInit(ch, tableIdx);
}

void audioModularSetWtPos(uint8_t ch, float pos01) {
    if (!g_modRole.ready || ch != MOD3_OSCA_CH) return;
    if (pos01 < 0.0f) pos01 = 0.0f; else if (pos01 > 1.0f) pos01 = 1.0f;
    g_modEngine.set_param(MOD_OSC, OSCX_MORPH, (int32_t)(pos01 * 32767.0f));
}

void modRetunePitch() {
    if (!g_modRole.ready) return;
    g_modEngine.set_param(MOD_OSC, OSCX_PITCH, (int32_t)lroundf(g_modBendSemis * 256.0f));
}

// detuneSemisB is unused now (no osc B to detune) — kept in the signature since main.cpp's
// extensive existing call sites all pass it (osc B's detune amount from kAmbientPresets[]
// or a mode's own pot); removing the parameter would mean touching every call site for a
// value that's harmless to keep accepting and ignoring.
void audioModularNoteOn(uint8_t note, float vel, float detuneSemisB) {
    (void)detuneSemisB;
    if (!g_modRole.ready) return;
    int vel127 = (int)(vel * 127.0f);
    if (vel127 < 1) vel127 = 1; else if (vel127 > 127) vel127 = 127;
    g_modHeldNotes++;
    g_modEngine.note_on(note, vel127);
}

void audioModularNoteOff(uint8_t note) {
    if (!g_modRole.ready) return;
    g_modEngine.note_off(note);
    if (g_modHeldNotes > 0) g_modHeldNotes--;
    if (g_modHeldNotes == 0) g_modRole.activeUntilMs = millis() + (uint32_t)g_modEnvReleaseMs + 200;
}

void audioModularAllNotesOff() {
    if (!g_modRole.ready) return;
    g_modEngine.all_notes_off();
    g_modHeldNotes = 0;
    g_modRole.activeUntilMs = millis() + (uint32_t)g_modEnvReleaseMs + 200;
}

void audioModularSetFilter(float cutoffHz, float resonance) {
    if (!g_modRole.ready) return;
    g_modEngine.set_param(MOD_FILTER, FLT_CUTOFF, hzToPitchUnits(cutoffHz));
    float resoQ = (resonance - 1.0f) / 3.0f;
    if (resoQ < 0.0f) resoQ = 0.0f; else if (resoQ > 1.0f) resoQ = 1.0f;
    g_modEngine.set_param(MOD_FILTER, FLT_RES, (int32_t)(resoQ * 32767.0f));
}

void audioModularSetEnvelope(const EnvParams &env) {
    if (!g_modRole.ready) return;
    g_modEnvReleaseMs = (float)env.rel;
    g_modEngine.set_param(MOD_ENV, ENV_ATTACK, env.atk);
    g_modEngine.set_param(MOD_ENV, ENV_DECAY, env.dec);
    g_modEngine.set_param(MOD_ENV, ENV_SUSTAIN, (int32_t)(env.sus * 32767.0f));
    g_modEngine.set_param(MOD_ENV, ENV_RELEASE, env.rel);
}

void audioModularSetPitchBend(float ratio) {
    if (!g_modRole.ready) return;
    g_modBendSemis = (ratio > 0.0f) ? 12.0f * log2f(ratio) : 0.0f;
    modRetunePitch();
}


// =====================================================================================
// ==== SD streaming: sample keys (MODE_SAMPLE / SS2), the preview, MODE_DJ's deck ====
// =====================================================================================
// All of these play through the vendored engine's own streaming sampler — the same
// SampleBank + Pads engine as the drums (initPadsEngine()) — instead of GrvEP's AMY-era
// "decode the whole file into PSRAM" pipelines:
//   * a WAV/MP3 is converted once into the engine's .smp format (smp_import.h, cached on
//     the card); SampleBank::load() then keeps only a short RAM head of it, and the Pads
//     voices' Sampler streams the rest from the card through a ring of 4 KB blocks;
//   * ONE loader task (smpLoaderTask) runs the conversions and SampleBank::pump() — the
//     friend's own split (framework/boards/hwv1/card_task.cpp: all of the sampler's card
//     I/O on one task; the audio thread never waits on the card, a late block is an
//     underrun the Sampler fades over);
//   * every key, the preview and the deck are Pads pads (PadBlob), sharing its 8 voices
//     with the drums.
// What this changes in the facade's API (inherited from GrvEP): no "large key" path any
// more — every key streams, so audioKeyIsLarge() is false and the *Streamed functions
// just forward; MODE_DJ holds the whole track, not ~30 s chunks, so its single "chunk" is
// the whole track (touches both ends; the waveform covers all of it).

namespace {

constexpr int kPadKeyBase  = 16;                                 // 0..15 are the drum pads (kPadSetup)
constexpr int kPadPreview  = kPadKeyBase + SAMPLE_KEY_COUNT;    // 48
constexpr int kPadDJ       = kPadPreview + 1;                    // 49
constexpr int kPadDJGrain  = kPadDJ + 1;                         // 50, 51
constexpr int kDJGrainPads = 2;
static_assert(kPadDJGrain + kDJGrainPads <= kPadCount, "not enough Pads pads");

constexpr int      kKeyHeadMs       = 150;    // SampleBank's own default head: a note starts at once whatever the card does
constexpr int      kDJHeadMs        = 150;
constexpr uint32_t kUnloadGraceMs   = 300;    // a released sample stays loaded a moment (a re-pick of the same file is free,
                                              // and a push already queued for it can still start safely)
constexpr int32_t  kStopTail        = 220;    // 5 ms fade when a key / the deck stops
constexpr float    kScratchMaxSpeed = 6.0f;   // x real time. Upstream measured underruns at 8x with an 8-block ring (12 here)
constexpr uint32_t kLoaderBudgetMs  = 15;     // conversion steps per loader iteration before pumping the streams again
constexpr int      kMaxSources      = SAMPLE_KEY_COUNT + 4;   // every key a different file + preview + deck (+ slack)

enum SrcState : uint8_t { SRC_FREE, SRC_WAIT, SRC_CONVERT, SRC_LOAD, SRC_READY, SRC_FAILED };

// One file in use, shared by every key / the preview / the deck that uses the same path. path/refs/state changes
// go under g_srcMux; `slot` and the conversion are the loader task's; frames/rate/wave are set before READY.
struct SmpSource {
    char path[128] = {};
    char cache[40] = {};
    std::atomic<uint8_t> state{SRC_FREE};
    uint8_t err = KEY_ERR_NONE;
    bool normalize = false;
    int16_t headMs = kKeyHeadMs;
    int16_t slot = -1;
    uint8_t refs = 0;
    uint32_t releasedMs = 0;
    uint32_t frames = 0, rate = 0;
    std::atomic<uint32_t> progress{0};        // frames converted so far (MODE_DJ's "Chargement..." readout)
    uint8_t wave[SMP_WAVE_BINS] = {};
};

SmpSource   *g_src = nullptr;                  // kMaxSources, in PSRAM
SmpImporter *g_imp = nullptr;                  // one conversion at a time (PSRAM: it holds a 4 KB block)
std::atomic<int> g_convSrc{-1};                // the source being converted (written by the loader task only)
std::mutex   g_srcMux;
std::mutex   g_loaderMux;                      // held for each loader iteration: audioDJCloseFiles() waits on it
std::atomic<bool> g_abortConv{false};

// Users of the table, as index + 1 (0 = none) — main side.
uint8_t g_keySrc1[SAMPLE_KEY_COUNT] = {};
uint8_t g_previewSrc1 = 0, g_djSrc1 = 0;
bool    g_previewPending = false;              // play the preview as soon as it is ready
float   g_previewVel = 1.0f;
float   g_sampleVolume = 1.0f;

// What each key pad's PadBlob last said, so a hit only re-sends it when it changed: the engine's command queue holds
// just ENGINE_CMD_RING=32 commands (see padsSetBlob()), and an SS2 step can hit many keys at once.
struct KeyBlobSent { int16_t slot = -1; int16_t gain = -1; uint8_t rev = 0; };
KeyBlobSent g_keyBlob[SAMPLE_KEY_COUNT];

SmpSource *src1(uint8_t s1) { return (s1 && g_src) ? &g_src[s1 - 1] : nullptr; }
bool srcReady(uint8_t s1) { SmpSource *s = src1(s1); return s && s->state.load() == SRC_READY; }
bool padSounding(int pad) { return (pads_playing() >> pad) & 1ull; }

uint8_t smpAcquire(const char *path, bool normalize, int headMs) {
    if (!g_src || !path || !path[0]) return 0;
    std::lock_guard<std::mutex> lk(g_srcMux);
    int freeIdx = -1;
    for (int i = 0; i < kMaxSources; i++) {
        SmpSource &s = g_src[i];
        const uint8_t st = s.state.load();
        if (st == SRC_FREE) { if (freeIdx < 0) freeIdx = i; continue; }
        if (strcmp(s.path, path) != 0) continue;
        if (st == SRC_FAILED) { s.err = KEY_ERR_NONE; s.state = SRC_WAIT; }   // asked again: try again (card reinserted, file fixed)
        s.refs++;
        return (uint8_t)(i + 1);
    }
    if (freeIdx < 0) return 0;
    SmpSource &s = g_src[freeIdx];
    snprintf(s.path, sizeof s.path, "%s", path);
    smpCachePath(path, s.cache, sizeof s.cache);
    s.err = KEY_ERR_NONE;
    s.normalize = normalize;
    s.headMs = (int16_t)headMs;
    s.slot = -1;
    s.refs = 1;
    s.frames = s.rate = 0;
    s.progress = 0;
    memset(s.wave, 0, sizeof s.wave);
    s.state = SRC_WAIT;
    return (uint8_t)(freeIdx + 1);
}

void smpRelease(uint8_t s1) {
    if (!s1 || !g_src) return;
    std::lock_guard<std::mutex> lk(g_srcMux);
    SmpSource &s = g_src[s1 - 1];
    if (s.refs && --s.refs == 0) s.releasedMs = millis();
}

// ---- loader task ----

uint64_t nowUs() {   // micros() wraps after ~71 min; SampleBank::pump() compares deadlines in 64 bits
    static uint32_t last = 0;
    static uint64_t hi = 0;
    const uint32_t m = micros();
    if (m < last) hi += 1ull << 32;
    last = m;
    return hi | m;
}

void startBankLoad(SmpSource &s) {
    const int slot = g_sampleBank.load(s.cache, s.headMs);
    if (slot < 0) { s.err = KEY_ERR_ALLOC; s.state = SRC_FAILED; return; }   // no free bank slot (or the file vanished)
    s.slot = (int16_t)slot;
    s.state = SRC_LOAD;
}

// One pass over the table: finished head loads become READY, released sources are unloaded, and the next waiting
// file is claimed (its cache checked, else its conversion started). Returns the index claimed, or -1.
int scanSources() {
    std::lock_guard<std::mutex> lk(g_srcMux);
    const uint32_t now = millis();
    int next = -1;
    for (int i = 0; i < kMaxSources; i++) {
        if (i == g_convSrc) continue;
        SmpSource &s = g_src[i];
        const uint8_t st = s.state.load();
        if (st == SRC_FREE) continue;
        if (st == SRC_LOAD) {
            if (g_sampleBank.ready(s.slot)) {
                const SampleSlot *sl = g_sampleBank.slot(s.slot);
                s.frames = sl->h.frames;
                s.rate = sl->h.sample_rate;
                s.state = SRC_READY;
            } else if (g_sampleBank.failed(s.slot)) {
                s.err = KEY_ERR_ALLOC;   // the header was checked before load(): a failure here is the heads not fitting the heap
                s.state = SRC_FAILED;
            }
        }
        if (s.refs == 0) {
            if (now - s.releasedMs < kUnloadGraceMs) continue;
            if (s.slot >= 0 && !g_sampleBank.can_unload(s.slot)) continue;
            if (s.slot >= 0) g_sampleBank.unload(s.slot);
            s.slot = -1;
            s.state = SRC_FREE;
            continue;
        }
        if (st == SRC_WAIT && next < 0) {
            if (s.slot >= 0) {                                   // a failed load asked again: free its old slot first
                if (!g_sampleBank.can_unload(s.slot)) continue;
                g_sampleBank.unload(s.slot);
                s.slot = -1;
            }
            s.state = SRC_CONVERT;                               // claimed
            next = i;
        }
    }
    return next;
}

// One conversion step. Drops the conversion when its source was released or MODE_USB asked for quiet.
void convertStep() {
    SmpSource &s = g_src[g_convSrc];
    bool drop;
    { std::lock_guard<std::mutex> lk(g_srcMux); drop = s.refs == 0; }
    if (drop || g_abortConv.exchange(false)) {
        g_imp->abort();
        std::lock_guard<std::mutex> lk(g_srcMux);
        if (s.refs == 0) s.state = SRC_FREE;
        else { s.err = KEY_ERR_IO; s.state = SRC_FAILED; }
        g_convSrc = -1;
        return;
    }
    const SmpImporter::Result r = g_imp->step();
    s.rate = g_imp->rate();
    s.progress = g_imp->frames();
    if (r == SmpImporter::RUNNING) return;
    if (r == SmpImporter::DONE && smpCacheValid(s.path, s.cache, s.wave, &s.frames, &s.rate)) startBankLoad(s);
    else { s.err = r == SmpImporter::FAILED ? g_imp->error() : KEY_ERR_IO; s.state = SRC_FAILED; }
    if (s.state.load() == SRC_FAILED) Serial.printf("smp: %s failed (err %u)\n", s.path, (unsigned)s.err);
    else Serial.printf("smp: converted %s (%u frames @ %u Hz)\n", s.path, (unsigned)s.frames, (unsigned)s.rate);
    g_convSrc = -1;
}

void smpLoaderTask(void *) {
    for (;;) {
        bool converting;
        {
            std::lock_guard<std::mutex> lk(g_loaderMux);
            const int next = scanSources();
            if (next >= 0) {
                SmpSource &s = g_src[next];
                if (smpCacheValid(s.path, s.cache, s.wave, &s.frames, &s.rate)) startBankLoad(s);
                else if (g_imp->begin(s.path, s.cache, s.normalize)) { s.progress = 0; g_convSrc = next; }
                else { s.err = g_imp->error(); s.state = SRC_FAILED; Serial.printf("smp: %s unreadable (err %u)\n", s.path, (unsigned)s.err); }
            }
            const uint32_t t0 = millis();
            while (g_convSrc >= 0 && millis() - t0 < kLoaderBudgetMs) convertStep();
            g_sampleBank.pump(nowUs());
            converting = g_convSrc >= 0;
        }
        vTaskDelay(pdMS_TO_TICKS(converting ? 1 : 2));
    }
}

// ---- pads ----

q15 smpGain(float mul) {
    float g = g_masterVolume * kPadBaseLevel * g_sampleVolume * mul;
    if (g > 1.0f) g = 1.0f; else if (g < 0.0f) g = 0.0f;
    return (q15)(g * 32767.0f);
}

void smpSendBlob(int pad, int16_t slot, uint8_t mode, int32_t start, int32_t end, int32_t tune, bool reverse,
                 q15 gain, uint8_t loop = 0, int32_t lstart = 0, int32_t xfade = 0) {
    PadBlob b;
    b.pad = (uint8_t)pad;
    b.bus = 0;
    b.mode = mode;
    b.loop = loop;
    b.lstart = lstart;
    b.xfade = xfade;
    b.slot = slot;
    b.start = start;
    b.end = end;
    b.tune = tune;
    b.tail = kStopTail;
    b.gain_l = b.gain_r = gain;
    b.reverse = reverse ? 1 : 0;
    padsSetBlob(&b, sizeof b);
}

void smpPadEvent(int pad, float vel, bool release) {
    PadSeqEvent ev;
    ev.pad = (uint8_t)pad;
    int level = (int)lroundf(vel * 15.0f);        // the pad sounds at (level + 1) / 16 of its gain
    ev.level = (uint8_t)(level < 0 ? 0 : (level > 15 ? 15 : level));
    ev.release = release ? 1 : 0;
    padsSetBlob(&ev, sizeof ev);
}

void smpPlayKey(uint8_t k, float vel, bool rev) {
    if (k >= SAMPLE_KEY_COUNT || !g_padsReady) return;
    SmpSource *s = src1(g_keySrc1[k]);
    if (!s || s->state.load() != SRC_READY) return;
    const q15 gain = smpGain(1.0f);
    KeyBlobSent &sent = g_keyBlob[k];
    if (sent.slot != s->slot || sent.gain != gain || sent.rev != (uint8_t)rev) {
        smpSendBlob(kPadKeyBase + k, s->slot, PADM_GATE, 0, 0, 0, rev, gain);
        sent.slot = s->slot; sent.gain = gain; sent.rev = rev;
    }
    smpPadEvent(kPadKeyBase + k, vel, false);
}

void smpStopPad(int pad) { if (g_padsReady && padSounding(pad)) smpPadEvent(pad, 0.0f, true); }

void smpPlayPreview() {
    SmpSource *s = src1(g_previewSrc1);
    if (!g_padsReady || !s || s->state.load() != SRC_READY) return;
    smpSendBlob(kPadPreview, s->slot, PADM_GATE, 0, 0, 0, false, smpGain(1.0f));
    smpPadEvent(kPadPreview, g_previewVel, false);
}

// ---- MODE_DJ ----

struct DjDeck {
    bool loaded = false, playing = false, reverse = false, stuttering = false, scratching = false;
    bool scratchRev = false;
    float speed = 1.0f, grain = 0.0f;
    int32_t scratchTune = 0;
    uint32_t frames = 0, rate = 0, pos = 0;
    uint32_t stutterPos = 0, stutterLen = 0;
    float stutLenSec = 0.0f, stutSmooth = 0.0f, stutRate = 1.0f;   // audioDJSetStutterParams()
    bool stutRev = false;
    uint32_t loadGen = 0, waveGen = 0, pushMs = 0, grainDueMs = 0;
    uint8_t grainNext = 0;
};
DjDeck g_dj;

int32_t djTune(float speed) { return (int32_t)lroundf(12.0f * 256.0f * log2f(speed)); }   // pitch units (1/256 semitone)

// The deck's PadBlob for its current state. Re-sent while it plays, the Pads module applies its tune / direction to the
// sounding voice at once (Sampler::set_live()); the other fields are for the next push.
void djSendBlob() {
    SmpSource *s = src1(g_djSrc1);
    if (!s || s->state.load() != SRC_READY) return;
    if (g_dj.stuttering)
        smpSendBlob(kPadDJ, s->slot, PADM_GATE, (int32_t)g_dj.stutterPos, (int32_t)(g_dj.stutterPos + g_dj.stutterLen),
                    djTune(g_dj.speed * g_dj.stutRate), g_dj.stutRev, smpGain(1.0f), 1, (int32_t)g_dj.stutterPos,
                    (int32_t)(g_dj.stutSmooth * 0.5f * (float)g_dj.stutterLen));   // the Sampler caps it at half the loop
    else
        smpSendBlob(kPadDJ, s->slot, PADM_GATE, (int32_t)g_dj.pos,
                    0, g_dj.scratching ? g_dj.scratchTune : djTune(g_dj.speed),
                    g_dj.scratching ? g_dj.scratchRev : g_dj.reverse, smpGain(1.0f));
}

// (Re)starts the deck's pad from g_dj.pos (or the stutter window).
void djStart() {
    if (g_dj.frames < 2) return;
    if (!g_dj.stuttering) {
        if (g_dj.reverse && g_dj.pos == 0) g_dj.pos = g_dj.frames - 1;                 // backwards from the very start: from the end
        if (!g_dj.reverse && g_dj.pos >= g_dj.frames - 1) g_dj.pos = 0;
    }
    djSendBlob();
    smpPadEvent(kPadDJ, 1.0f, false);
    g_dj.pushMs = millis();
}

// The playhead: the Pads module publishes where each pad's sound is in its file once per block.
uint32_t djPos() {
    if (g_dj.playing && !g_dj.stuttering && millis() - g_dj.pushMs > 40) {               // the first blocks after a push may
        const int32_t p = pads_position(kPadDJ);                                          // still show the voice's old place
        if (p >= 0) g_dj.pos = (uint32_t)p < g_dj.frames ? (uint32_t)p : g_dj.frames - 1;
    }
    return g_dj.pos;
}

void djSpawnGrain() {
    const uint32_t now = millis();
    if ((int32_t)(now - g_dj.grainDueMs) < 0) return;
    const float gapMs = DJ_GRAIN_GAP_MS_MAX + (DJ_GRAIN_GAP_MS_MIN - DJ_GRAIN_GAP_MS_MAX) * g_dj.grain;
    g_dj.grainDueMs = now + (uint32_t)gapMs;
    SmpSource *s = src1(g_djSrc1);
    if (!s) return;
    const uint32_t len = (uint32_t)((uint64_t)g_dj.rate * (DJ_GRAIN_LEN_MS_MIN + random(DJ_GRAIN_LEN_MS_MAX - DJ_GRAIN_LEN_MS_MIN)) / 1000);
    if (len + 2 >= g_dj.frames) return;
    // Like GrvEP's grains (taken from the ring of recently played audio): somewhere in the last second before the playhead.
    int64_t start = (int64_t)djPos() - (int64_t)random((long)g_dj.rate);
    if (start < 0) start = 0;
    if (start + len >= g_dj.frames) start = g_dj.frames - len - 1;
    const float scatter = (random(2001) / 1000.0f - 1.0f) * DJ_GRAIN_PITCH_SCATTER;     // +- semitones
    const int pad = kPadDJGrain + g_dj.grainNext;
    g_dj.grainNext = (uint8_t)((g_dj.grainNext + 1) % kDJGrainPads);
    smpSendBlob(pad, s->slot, PADM_ONESHOT, (int32_t)start, (int32_t)(start + len),
                djTune(g_dj.speed) + (int32_t)lroundf(scatter * 256.0f), false, smpGain(0.7f));
    smpPadEvent(pad, 1.0f, false);
}

void djService() {
    SmpSource *s = src1(g_djSrc1);
    if (!g_dj.loaded) {
        if (s && s->state.load() == SRC_READY) {
            g_dj.loaded = true;
            g_dj.frames = s->frames;
            g_dj.rate = s->rate;
            g_dj.pos = 0;
            g_dj.loadGen++;
            g_dj.waveGen++;
        }
        return;
    }
    if (!g_dj.playing) return;
    if (g_dj.stuttering || padSounding(kPadDJ) || millis() - g_dj.pushMs < 500) {
        djPos();
        if (g_dj.grain > 0.01f && !g_dj.stuttering) djSpawnGrain();
        return;
    }
    // The pad stopped by itself: the end of the track (or start, backwards) — or a drum mode's PADS_STOP
    // (audioDrumSynthAllOff() stops every pad). At an end, rewind for the next Play; otherwise stay where it was.
    g_dj.playing = false;
    g_dj.scratching = false;
    const uint32_t margin = g_dj.rate / 2;
    if (g_dj.reverse ? g_dj.pos < margin : g_dj.pos + margin >= g_dj.frames) g_dj.pos = g_dj.reverse ? g_dj.frames - 1 : 0;
}

void smpServiceMain() {
    if (g_previewPending) {
        SmpSource *p = src1(g_previewSrc1);
        const uint8_t st = p ? p->state.load() : (uint8_t)SRC_FAILED;
        if (st == SRC_READY) { g_previewPending = false; smpPlayPreview(); }
        else if (st == SRC_FAILED || st == SRC_FREE) g_previewPending = false;
    }
    djService();
}

bool smpLoaderBusy() { return g_convSrc >= 0; }

bool smpAnyLoadPending() {
    if (!g_src) return false;
    for (int i = 0; i < kMaxSources; i++) {
        const uint8_t st = g_src[i].state.load();
        if (g_src[i].refs && (st == SRC_WAIT || st == SRC_CONVERT || st == SRC_LOAD)) return true;
    }
    return false;
}

void smpCloseFiles() {
    std::lock_guard<std::mutex> lk(g_loaderMux);   // not in the middle of a loader iteration
    g_sdStorage.closeAll();
}

// Forward-declared near g_masterVolume: audioSetVolume()/audioSetSampleVolume() changed the pads' gain. The key pads
// get it at their next hit (re-sending all 32 blobs at once would overflow the engine's 32-command queue); the deck,
// which keeps playing, gets it now.
static void smpRefreshGains() {
    for (KeyBlobSent &k : g_keyBlob) k.gain = -1;
    if (g_dj.playing && g_padsReady) djSendBlob();
}

}  // namespace

void audioStreamWorkersInit() {
    if (g_src) return;
    if (!g_padsReady) { Serial.println("smp: no Pads engine, sample streaming disabled"); return; }
    void *tbl = smpBigAlloc(sizeof(SmpSource) * kMaxSources);
    void *imp = smpBigAlloc(sizeof(SmpImporter));
    if (!tbl || !imp) { Serial.println("smp: alloc failed, sample streaming disabled"); free(tbl); free(imp); return; }
    SmpSource *t = static_cast<SmpSource *>(tbl);
    for (int i = 0; i < kMaxSources; i++) new (&t[i]) SmpSource();
    g_imp = new (imp) SmpImporter();
    g_src = t;
    // Low priority on core 0 like GrvEP's own SD workers. Its stack (MP3 decoding + SD) goes to PSRAM on the board:
    // internal RAM is nearly full (CLAUDE.md, "RAM is tight"), and this task never writes to flash, which is what a
    // PSRAM stack would forbid.
#if defined(ESP_PLATFORM) && !defined(SIMULATOR)
    BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(smpLoaderTask, "smpLoad", 16384, nullptr, 2, nullptr, 0, MALLOC_CAP_SPIRAM);
#else
    BaseType_t ok = xTaskCreatePinnedToCore(smpLoaderTask, "smpLoad", 16384, nullptr, 2, nullptr, 0);
#endif
    Serial.printf("smp: loader task %s\n", ok == pdPASS ? "started" : "FAILED");
}

// ---- sample keys ----

void audioLoadKey(const char* path, uint8_t keyIdx) {
    if (keyIdx >= SAMPLE_KEY_COUNT) return;
    const uint8_t n = smpAcquire(path, true, kKeyHeadMs);    // before the release: the same file keeps its slot
    smpStopPad(kPadKeyBase + keyIdx);
    smpRelease(g_keySrc1[keyIdx]);
    g_keySrc1[keyIdx] = n;
    g_keyBlob[keyIdx].slot = -1;
}

void audioPlayKey(uint8_t keyIdx, float vel)    { smpPlayKey(keyIdx, vel, false); }
void audioPlayKeyRev(uint8_t keyIdx, float vel) { smpPlayKey(keyIdx, vel, true); }
void audioStopKey(uint8_t keyIdx) { if (keyIdx < SAMPLE_KEY_COUNT) smpStopPad(kPadKeyBase + keyIdx); }

void audioSetSampleVolume(float v) {
    g_sampleVolume = v < 0.0f ? 0.0f : (v > 2.0f ? 2.0f : v);
    smpRefreshGains();
}

bool audioKeyLoaded(uint8_t keyIdx) { return keyIdx < SAMPLE_KEY_COUNT && srcReady(g_keySrc1[keyIdx]); }

uint8_t audioKeyError(uint8_t keyIdx) {
    SmpSource *s = keyIdx < SAMPLE_KEY_COUNT ? src1(g_keySrc1[keyIdx]) : nullptr;
    return (s && s->state.load() == SRC_FAILED) ? s->err : KEY_ERR_NONE;
}

uint32_t audioKeyLengthMs(uint8_t keyIdx) {
    SmpSource *s = keyIdx < SAMPLE_KEY_COUNT ? src1(g_keySrc1[keyIdx]) : nullptr;
    if (!s || s->state.load() != SRC_READY || !s->rate) return 0;
    return (uint32_t)((uint64_t)s->frames * 1000 / s->rate);
}

void audioClearAllKeys() {
    for (uint8_t k = 0; k < SAMPLE_KEY_COUNT; k++) {
        if (!g_keySrc1[k]) continue;
        smpStopPad(kPadKeyBase + k);                       // only the sounding ones post a command (8 voices at most)
        smpRelease(g_keySrc1[k]);
        g_keySrc1[k] = 0;
        g_keyBlob[k].slot = -1;
    }
}

void audioStopAllSamples() {
    for (uint8_t k = 0; k < SAMPLE_KEY_COUNT; k++) smpStopPad(kPadKeyBase + k);
    smpStopPad(kPadPreview);
    g_previewPending = false;
}

// Every key streams now (see this section's header): no separate large-file path.
bool audioKeyIsLarge(uint8_t keyIdx) { (void)keyIdx; return false; }
void audioPlayKeyStreamed(uint8_t keyIdx, bool reverse, float vel) { smpPlayKey(keyIdx, vel, reverse); }
bool audioKeyStreamStillPlaying(uint8_t keyIdx) { return keyIdx < SAMPLE_KEY_COUNT && padSounding(kPadKeyBase + keyIdx); }

void audioSSTick() { smpServiceMain(); }

bool audioSSStreamActive() {
    const uint64_t mask = ((1ull << (SAMPLE_KEY_COUNT + 1)) - 1) << kPadKeyBase;   // the keys and the preview
    return (pads_playing() & mask) != 0;
}
bool audioSSJobBusy() { return smpLoaderBusy(); }
void audioSSCloseFiles() { smpCloseFiles(); }
void audioSSForceQuiesce() { if (smpLoaderBusy()) g_abortConv = true; }

// ---- preview / legacy loaders ----

void audioLoadAndPlay(const char* path, uint16_t preset, float vel) {
    (void)preset;
    smpStopPad(kPadPreview);
    const uint8_t n = smpAcquire(path, true, kKeyHeadMs);
    smpRelease(g_previewSrc1);
    g_previewSrc1 = n;
    g_previewVel = vel;
    g_previewPending = vel > 0.0f;
    smpServiceMain();                                      // already converted and loaded: plays now
}

bool audioLoadFromSD(const char* path, uint16_t preset)    { audioLoadAndPlay(path, preset, 0.0f); return g_previewSrc1 != 0; }
bool audioLoadWavFromSD(const char* path, uint16_t preset) { return audioLoadFromSD(path, preset); }
bool audioLoadMp3FromSD(const char* path, uint16_t preset) { return audioLoadFromSD(path, preset); }

void audioPlaySamplePreset(uint16_t preset, float vel) {
    (void)preset;
    g_previewVel = vel;
    if (srcReady(g_previewSrc1)) smpPlayPreview();
    else g_previewPending = g_previewSrc1 != 0;
}

void audioStopSamplePreset(uint16_t preset) {
    (void)preset;
    g_previewPending = false;
    smpStopPad(kPadPreview);
}

bool audioIsStreamingDone() { return !smpAnyLoadPending(); }

// ---- MODE_DJ ----

void audioLoadDJTrack(const char* path) {
    if (g_dj.playing) smpStopPad(kPadDJ);
    const float speed = g_dj.speed;                        // the speed fader keeps its place across tracks
    const uint32_t gen = g_dj.loadGen, wgen = g_dj.waveGen;
    g_dj = DjDeck{};
    g_dj.speed = speed;
    g_dj.loadGen = gen;
    g_dj.waveGen = wgen + 1;                               // the old track's waveform goes now: "Chargement..." shows instead
    const uint8_t n = smpAcquire(path, false, kDJHeadMs);  // no normalizing: a whole track keeps its own mastering
    smpRelease(g_djSrc1);
    g_djSrc1 = n;
    smpServiceMain();
}

bool audioDJIsLoaded() { return g_dj.loaded; }
bool audioDJCanPlayYet() { return g_dj.loaded; }

uint8_t audioDJLoadError() {
    SmpSource *s = src1(g_djSrc1);
    return (s && s->state.load() == SRC_FAILED) ? s->err : KEY_ERR_NONE;
}

// main.cpp shows this divided by 20000 as "seconds decoded" (GrvEP's own DJ playback rate): report the converted
// frames in those units so the readout stays right whatever the file's own sample rate is.
uint32_t audioDJGetDecodedFrames() {
    SmpSource *s = src1(g_djSrc1);
    if (!s || !s->rate) return 0;
    return (uint32_t)((uint64_t)s->progress.load() * 20000u / s->rate);
}

uint32_t audioDJGetLoadGen() { return g_dj.loadGen; }
// One "chunk" = the whole track: its waveform changes when a load starts (none to show) and when it completes. It
// used to change only on completion, so the previous track's waveform stayed up during the whole new conversion.
uint32_t audioDJGetChunkGen() { return g_dj.waveGen; }

void audioDJPlayPause(bool playing) {
    if (!g_dj.loaded) { g_dj.playing = false; return; }
    if (playing == g_dj.playing) return;
    if (playing) {
        g_dj.playing = true;
        djStart();
    } else {
        djPos();
        if (g_dj.stuttering) g_dj.pos = g_dj.stutterPos;
        g_dj.playing = g_dj.stuttering = g_dj.scratching = false;
        smpStopPad(kPadDJ);
    }
}

bool audioDJIsPlaying() { return g_dj.playing; }
void audioDJTick() { smpServiceMain(); }

void audioDJSeek(float posFrac) {
    if (!g_dj.loaded || g_dj.frames < 2) return;
    posFrac = posFrac < 0.0f ? 0.0f : (posFrac > 1.0f ? 1.0f : posFrac);
    g_dj.pos = (uint32_t)(posFrac * (float)(g_dj.frames - 1));
    g_dj.stuttering = false;
    if (g_dj.playing) djStart();
}

float audioDJGetPosFrac() { return g_dj.frames ? (float)djPos() / (float)g_dj.frames : 0.0f; }

// The cursor over the waveform, in "reading order": main.cpp mirrors it back when reversed (chronoFrac = 1 - t).
float audioDJGetChunkProgressFrac() {
    const float f = audioDJGetPosFrac();
    return g_dj.reverse ? 1.0f - f : f;
}
bool audioDJChunkTouchesStart() { return true; }
bool audioDJChunkTouchesEnd() { return true; }

// 1.0 = normal, pitch follows (a turntable's fader). Applied to the sounding voice at once, no retrigger.
void audioDJSetSpeed(float speed) {
    g_dj.speed = speed < 0.25f ? 0.25f : (speed > 4.0f ? 4.0f : speed);
    if (g_dj.playing && !g_dj.scratching) djSendBlob();
}
float audioDJGetSpeed() { return g_dj.speed; }

// Continues backwards from where it is (the voice's direction flips in place; its stream follows).
void audioDJSetReverse(bool reverse) {
    g_dj.reverse = reverse;
    if (g_dj.playing && !g_dj.scratching && !g_dj.stuttering) { djPos(); djSendBlob(); }
}
bool audioDJGetReverse() { return g_dj.reverse; }
bool audioDJIsReverseReady() { return g_dj.loaded; }

bool audioDJComputeWaveform(uint8_t* waveform128) {
    SmpSource *s = src1(g_djSrc1);
    if (!g_dj.loaded || !s) return false;
    static_assert(SMP_WAVE_BINS == 128, "MODE_DJ draws 128 bins");
    memcpy(waveform128, s->wave, SMP_WAVE_BINS);
    return true;
}

float audioDJGetLengthSeconds() { return g_dj.rate ? (float)g_dj.frames / (float)g_dj.rate : 0.0f; }

// A real scratch: the joystick sets the deck's speed and direction on the sounding voice (GrvEP had to re-trigger
// 150 ms windows, AMY having no live playhead). Same scale as GrvEP — DJ_SCRATCH_MAX_STEP_SEC of track per 10 ms tick
// at full deflection — capped at kScratchMaxSpeed so the stream can keep up.
void audioDJScratchNudge(float jx) {
    if (!g_dj.loaded || !g_dj.playing || g_dj.stuttering) return;
    if (fabsf(jx) <= DJ_SCRATCH_DEADZONE) return;
    float v = jx * (DJ_SCRATCH_MAX_STEP_SEC / 0.010f);      // track seconds per real second, signed
    float a = fabsf(v);
    a = a < 0.05f ? 0.05f : (a > kScratchMaxSpeed ? kScratchMaxSpeed : a);
    const int32_t tune = djTune(a);
    const bool rev = v < 0.0f;
    if (g_dj.scratching && tune == g_dj.scratchTune && rev == g_dj.scratchRev) return;   // nothing new: no command
    g_dj.scratching = true;
    g_dj.scratchTune = tune;
    g_dj.scratchRev = rev;
    djSendBlob();
}

void audioDJScratchEnd() {
    if (!g_dj.scratching) return;
    g_dj.scratching = false;
    if (g_dj.playing) { djPos(); djSendBlob(); }
}

void audioDJSetGrainAmount(float amount01) { g_dj.grain = amount01 < 0.0f ? 0.0f : (amount01 > 1.0f ? 1.0f : amount01); }
float audioDJGetGrainAmount() { return g_dj.grain; }

// A stutter length in frames, clamped to config.h's DJ_STUTTER_* range.
static uint32_t djStutterFrames(float sec) {
    sec = sec < DJ_STUTTER_MIN_SEC ? DJ_STUTTER_MIN_SEC : (sec > DJ_STUTTER_MAX_SEC ? DJ_STUTTER_MAX_SEC : sec);
    const uint32_t len = (uint32_t)(sec * (float)g_dj.rate);
    return len < DJ_STUTTER_MIN_FRAMES ? DJ_STUTTER_MIN_FRAMES : len;
}

void audioDJSetStutterParams(float lengthSec, float smooth01, float rate, bool reverse) {
    smooth01 = smooth01 < 0.0f ? 0.0f : (smooth01 > 1.0f ? 1.0f : smooth01);
    rate = rate < 0.25f ? 0.25f : (rate > 4.0f ? 4.0f : rate);
    const bool restart = fabsf(lengthSec - g_dj.stutLenSec) > 0.03f * (g_dj.stutLenSec > 0.0f ? g_dj.stutLenSec : 1.0f)
                         || fabsf(smooth01 - g_dj.stutSmooth) > 0.04f;
    const bool live = fabsf(rate - g_dj.stutRate) > 0.005f || reverse != g_dj.stutRev;
    g_dj.stutLenSec = lengthSec;
    g_dj.stutRate = rate;
    g_dj.stutRev = reverse;
    if (restart) g_dj.stutSmooth = smooth01;
    if (!g_dj.stuttering || !g_dj.playing) return;
    if (restart) {                                          // new loop region: restart it from the same start point
        g_dj.stutterLen = djStutterFrames(g_dj.stutLenSec);
        if (g_dj.stutterPos + g_dj.stutterLen >= g_dj.frames) g_dj.stutterLen = g_dj.frames - g_dj.stutterPos - 1;
        djStart();
    } else if (live) {
        djSendBlob();                                       // tune / direction on the sounding voice
    }
}

// Loops a short BPM-synced window at the playhead (the pad's region loop) until released; then resumes from where
// the stutter started, as GrvEP did.
void audioDJStutterStart(float bpmForSync) {
    if (!g_dj.loaded || !g_dj.playing || g_dj.stuttering || !g_dj.rate) return;
    if (bpmForSync < 1.0f) bpmForSync = 1.0f;
    const uint32_t len = djStutterFrames(g_dj.stutLenSec > 0.0f ? g_dj.stutLenSec : 60.0f / bpmForSync / 4.0f);
    if (len + 2 >= g_dj.frames) return;
    uint32_t p = djPos();
    if (p + len >= g_dj.frames) p = g_dj.frames - len - 1;
    g_dj.scratching = false;
    g_dj.stuttering = true;
    g_dj.stutterPos = p;
    g_dj.stutterLen = len;
    djStart();
}

void audioDJStutterEnd() {
    if (!g_dj.stuttering) return;
    g_dj.stuttering = false;
    g_dj.pos = g_dj.stutterPos;
    if (g_dj.playing) djStart();
}

bool audioDJIsStuttering() { return g_dj.stuttering; }

// MODE_USB support (see audio_engine.h): the loader task is the only background SD user left.
bool audioDJStreamActive() { return g_dj.playing; }
bool audioDJJobBusy() { return smpLoaderBusy(); }
void audioDJCloseFiles() { smpCloseFiles(); }
void audioDJForceQuiesce() { if (smpLoaderBusy()) g_abortConv = true; }
