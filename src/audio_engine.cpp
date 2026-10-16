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

#include "engine/core/engine.h"
#include "engine/modules/synth_modules.h"

#include <cstring>
#include <cstdint>

using namespace sc;

bool audioReady = false;

namespace {

// Voice count is a placeholder for Phase 1's single shared patch — Phase 3 picks a real
// count per SynthShape graph.
constexpr int kMainVoices = 8;

enum MainNode : int { N_NOTE = 1, N_OSC, N_FILTER, N_ENV, N_VCA, N_VOICE_OUT, N_BUS, N_MASTER };

Heap g_fastHeap;
Engine g_mainEngine;
bool g_mainReady = false;

float g_masterVolume = 0.35f;
EnvParams g_mainEnv = {50, 200, 500, 0.5f};   // matches config.h's envTable[ENV_NORMAL]

bool buildMainGraph(GraphDesc &g) {
    const Registry &reg = g_mainEngine.registry();
    NodeDesc *n;
    if (!g.add_node(reg, N_NOTE, T_NOTE_IN)) return false;
    if (!(n = g.add_node(reg, N_OSC, T_OSC))) return false;
    n->param[OSC_WAVE] = WAVE_SAW_;
    if (!(n = g.add_node(reg, N_FILTER, T_FILTER_V))) return false;
    n->param[FLT_MODE] = FLTM_LP;
    n->param[FLT_CUTOFF] = 96 * 256;     // pitch units: semitones x 256
    n->param[FLT_RES] = 8000;            // q15
    if (!(n = g.add_node(reg, N_ENV, T_ENV))) return false;
    n->param[ENV_ATTACK] = g_mainEnv.atk;
    n->param[ENV_DECAY] = g_mainEnv.dec;
    n->param[ENV_SUSTAIN] = (int32_t)(g_mainEnv.sus * 32767.0f);
    n->param[ENV_RELEASE] = g_mainEnv.rel;
    if (!(n = g.add_node(reg, N_VCA, T_VCA_V))) return false;
    n->param[VCA_LEVEL] = 0;             // closed: the envelope opens it
    if (!(n = g.add_node(reg, N_VOICE_OUT, T_VOICE_OUT))) return false;
    n->param[VO_LEVEL] = (int32_t)(g_masterVolume * 32767.0f);
    n->param[VO_TAIL_MS] = 400;
    if (!g.add_node(reg, N_BUS, T_BUS_IN)) return false;
    if (!g.add_node(reg, N_MASTER, T_MASTER_OUT)) return false;

    bool ok = true;
    ok &= g.connect(N_NOTE, 0, N_OSC, Dst::In, 0);                          // pitch
    ok &= g.connect(N_NOTE, 1, N_ENV, Dst::In, 0);                          // gate
    ok &= g.connect(N_OSC, 0, N_FILTER, Dst::In, 0);
    ok &= g.connect(N_ENV, 0, N_FILTER, Dst::Param, FLT_CUTOFF, 8192);
    ok &= g.connect(N_FILTER, 0, N_VCA, Dst::In, 0);
    ok &= g.connect(N_ENV, 0, N_VCA, Dst::Param, VCA_LEVEL);
    ok &= g.connect(N_VCA, 0, N_VOICE_OUT, Dst::In, 0);
    ok &= g.connect(N_BUS, 0, N_MASTER, Dst::In, 0);
    ok &= g.connect(N_BUS, 1, N_MASTER, Dst::In, 1);
    return ok;
}

}  // namespace

void audioInit() {
    // Generous placeholder size for the simulator; Phase 12 measures and tunes the real
    // ESP32 fast/bulk split (see the friend's own audio_esp32.cpp sizing pattern).
    static uint8_t fastMem[256 * 1024];
    g_fastHeap.init(fastMem, sizeof(fastMem));
    Memory mem{&g_fastHeap, &g_fastHeap};

    if (!g_mainEngine.init(mem, kMainVoices)) { audioReady = false; return; }
    register_synth_modules(g_mainEngine.registry());

    static GraphDesc g;
    g = GraphDesc{};
    if (!buildMainGraph(g)) { audioReady = false; return; }
    if (g_mainEngine.load(g, kMainVoices) != Err::Ok) { audioReady = false; return; }

    g_mainReady = true;
    audioReady = true;

    flashCacheInit();
}

void audioAllNotesOff() {
    if (g_mainReady) g_mainEngine.all_notes_off();
}

void audioNoteOn(uint8_t note, float velocity) {
    if (!g_mainReady) return;
    int vel127 = (int)(velocity * 127.0f);
    if (vel127 < 1) vel127 = 1;
    if (vel127 > 127) vel127 = 127;
    g_mainEngine.note_on(note, vel127);
}

void audioNoteOff(uint8_t note) {
    if (g_mainReady) g_mainEngine.note_off(note);
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
    if (!g_mainReady) return;
    g_mainEngine.set_param(N_VOICE_OUT, VO_LEVEL, (int32_t)(vol * 32767.0f));
}

void audioSetShape(SynthShape shape) {
    (void)shape;   // Phase 3 builds the real per-shape graph table; Phase 1 keeps one fixed voice.
}

void audioSetPitchBend(float ratio) {
    (void)ratio;   // Phase 3+
}

// Render entry point for the HAL audio-out callback (wired in Phase 1d/2's
// engine_hal_sim/engine_hal_esp32/engine_hal_web). Replaces AMY's amy_render_audio() role.
void audioEngineRenderBlock(int16_t *stereo, int frames) {
    static q15 blockL[kBlock], blockR[kBlock];
    static int blockPos = kBlock;
    if (!g_mainReady) { memset(stereo, 0, (size_t)frames * 4); return; }
    for (int i = 0; i < frames; i++) {
        if (blockPos >= kBlock) { g_mainEngine.render(blockL, blockR); g_mainEngine.gc(); blockPos = 0; }
        stereo[2 * i] = blockL[blockPos];
        stereo[2 * i + 1] = blockR[blockPos];
        blockPos++;
    }
}

// ==========================================================================
// Generated no-op stubs for the rest of audio_engine.h's surface. Filled in phase by
// phase (Phase 3: core synth group: audioSetFilter*/audioSetReverb* etc.; Phase 4: sample/
// key playback; Phase 5: TB-303/I303/SW2; Phase 6: drums/pads; Phase 7: modular/wavetable;
// Phase 8: FELU/PRESETS; Phase 9: sequencer migration) — see mutable-nibbling-melody.md.
// Regenerate this block with /tmp/gen_stubs.py if audio_engine.h's signatures change.
// ==========================================================================
void audioLoadKey(const char* path, uint8_t keyIdx) {
    (void)path;
    (void)keyIdx;
}

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

void audioSetFilterFreq(float cutoffHz, float resonance) {
    (void)cutoffHz;
    (void)resonance;
}

void audioSetGranular2FilterFreq(float cutoffHz, float resonance) {
    (void)cutoffHz;
    (void)resonance;
}

void audioSetPCMFilter(float cutoffHz, float resonance) {
    (void)cutoffHz;
    (void)resonance;
}

void audioSetFmParams(float depth, float cutoffHz, float resonance) {
    (void)depth;
    (void)cutoffHz;
    (void)resonance;
}

void audioSetFmDepth(float depth) {
    (void)depth;
}

void audioStopAllSamples() {

}

void audioSetReverb(float level, float liveness, float damping, float xover_hz) {
    (void)level;
    (void)liveness;
    (void)damping;
    (void)xover_hz;
}

void audioSetChorus(float level, float lfo_freq, float depth) {
    (void)level;
    (void)lfo_freq;
    (void)depth;
}

void audioSetDelay(float level, float delay_ms, float feedback, float filter_coef) {
    (void)level;
    (void)delay_ms;
    (void)feedback;
    (void)filter_coef;
}

void audioSetResoEcho(float level, float feedback, float damp, float wild, float delay_ms, bool on) {
    (void)level;
    (void)feedback;
    (void)damp;
    (void)wild;
    (void)delay_ms;
    (void)on;
}

void audioSetDrive(float drive, float tone, float level, uint8_t type, bool on) {
    (void)drive;
    (void)tone;
    (void)level;
    (void)type;
    (void)on;
}

void audioSetEq(float low, float mid, float high) {
    (void)low;
    (void)mid;
    (void)high;
}

void audioSetWavefold(float gain) {
    (void)gain;
}

void audioSetLadderFilter(float cutoffHz, float resonance, bool on) {
    (void)cutoffHz;
    (void)resonance;
    (void)on;
}

void audioSetFiltBus(float cutoffHz, float resonance, uint8_t type, bool on) {
    (void)cutoffHz;
    (void)resonance;
    (void)type;
    (void)on;
}

void audioSetRingmod(float freqHz, float mix, bool on) {
    (void)freqHz;
    (void)mix;
    (void)on;
}

void audioSetCompressor(float threshold, float ratio, bool on) {
    (void)threshold;
    (void)ratio;
    (void)on;
}

void audioLoadAndPlay(const char* path, uint16_t preset, float vel) {
    (void)path;
    (void)preset;
    (void)vel;
}

void audioPlayKey(uint8_t keyIdx, float vel) {
    (void)keyIdx;
    (void)vel;
}

void audioPlayKeyRev(uint8_t keyIdx, float vel) {
    (void)keyIdx;
    (void)vel;
}

void audioStopKey(uint8_t keyIdx) {
    (void)keyIdx;
}

void audioSetSampleVolume(float v) {
    (void)v;
}

bool audioKeyLoaded(uint8_t keyIdx) {
    (void)keyIdx;
    return false;
}

uint8_t audioKeyError(uint8_t keyIdx) {
    (void)keyIdx;
    return 0;
}

uint32_t audioKeyLengthMs(uint8_t keyIdx) {
    (void)keyIdx;
    return 0;
}

void audioClearAllKeys() {

}

void audioLoadDrumSamples() {

}

void audioPlayDrumPad(uint8_t col, float vel) {
    (void)col;
    (void)vel;
}

const char* audioDrumPadLabel(uint8_t col) {
    (void)col;
    return nullptr;
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
    (void)slot;
    return nullptr;
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

void audioLoadDJTrack(const char* path) {
    (void)path;
}

bool audioDJIsLoaded() {
    return false;
}

bool audioDJCanPlayYet() {
    return false;
}

uint32_t audioDJGetDecodedFrames() {
    return 0;
}

uint32_t audioDJGetLoadGen() {
    return 0;
}

uint32_t audioDJGetChunkGen() {
    return 0;
}

void audioDJPlayPause(bool playing) {
    (void)playing;
}

bool audioDJIsPlaying() {
    return false;
}

void audioDJTick() {

}

void audioDJSeek(float posFrac) {
    (void)posFrac;
}

float audioDJGetPosFrac() {
    return 0.0f;
}

float audioDJGetChunkProgressFrac() {
    return 0.0f;
}

bool audioDJChunkTouchesStart() {
    return false;
}

bool audioDJChunkTouchesEnd() {
    return false;
}

void audioDJSetSpeed(float speed) {
    (void)speed;
}

float audioDJGetSpeed() {
    return 0.0f;
}

void audioDJSetReverse(bool reverse) {
    (void)reverse;
}

bool audioDJGetReverse() {
    return false;
}

bool audioDJIsReverseReady() {
    return false;
}

bool audioDJComputeWaveform(uint8_t* waveform128) {
    (void)waveform128;
    return false;
}

float audioDJGetLengthSeconds() {
    return 0.0f;
}

void audioDJScratchNudge(float jx) {
    (void)jx;
}

void audioDJScratchEnd() {

}

void audioDJSetGrainAmount(float amount01) {
    (void)amount01;
}

float audioDJGetGrainAmount() {
    return 0.0f;
}

void audioDJStutterStart(float bpmForSync) {
    (void)bpmForSync;
}

void audioDJStutterEnd() {

}

bool audioDJIsStuttering() {
    return false;
}

bool audioDJStreamActive() {
    return false;
}

bool audioDJJobBusy() {
    return false;
}

void audioDJCloseFiles() {

}

void audioDJForceQuiesce() {

}

bool audioKeyIsLarge(uint8_t keyIdx) {
    (void)keyIdx;
    return false;
}

void audioPlayKeyStreamed(uint8_t keyIdx, bool reverse, float vel) {
    (void)keyIdx;
    (void)reverse;
    (void)vel;
}

bool audioKeyStreamStillPlaying(uint8_t keyIdx) {
    (void)keyIdx;
    return false;
}

void audioSSTick() {

}

void audioStreamWorkersInit() {

}

bool audioSSStreamActive() {
    return false;
}

bool audioSSJobBusy() {
    return false;
}

void audioSSCloseFiles() {

}

void audioSSForceQuiesce() {

}

bool audioLoadFromSD(const char* path, uint16_t preset) {
    (void)path;
    (void)preset;
    return false;
}

bool audioLoadWavFromSD(const char* path, uint16_t preset) {
    (void)path;
    (void)preset;
    return false;
}

bool audioLoadMp3FromSD(const char* path, uint16_t preset) {
    (void)path;
    (void)preset;
    return false;
}

void audioPlaySamplePreset(uint16_t preset, float vel) {
    (void)preset;
    (void)vel;
}

void audioStopSamplePreset(uint16_t preset) {
    (void)preset;
}

bool audioIsStreamingDone() {
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

void audioT303Init(float cutoff, float reso, float envMod, float decay, uint8_t amyWave) {
    (void)cutoff;
    (void)reso;
    (void)envMod;
    (void)decay;
    (void)amyWave;
}

void audioT303NoteOn(uint8_t midiNote, float vel) {
    (void)midiNote;
    (void)vel;
}

void audioT303NoteOff(uint8_t midiNote) {
    (void)midiNote;
}

void audioT303Params(float cutoff, float reso, float envMod, float decay) {
    (void)cutoff;
    (void)reso;
    (void)envMod;
    (void)decay;
}

void audioT303SetAmpEnv(float atkMs, float sus, float relMs) {
    (void)atkMs;
    (void)sus;
    (void)relMs;
}

void audioI303SetAmpEnv(float atkMs, float sus, float decMs, float relMs) {
    (void)atkMs;
    (void)sus;
    (void)decMs;
    (void)relMs;
}

void audioT303SetSustain(float sustain) {
    (void)sustain;
}

void audioT303PitchBend(float ratio) {
    (void)ratio;
}

void audioT303Wave(uint8_t amyWave) {
    (void)amyWave;
}

void audioI303Init(float cutoff, float reso, float envMod, float decay, uint8_t amyWave) {
    (void)cutoff;
    (void)reso;
    (void)envMod;
    (void)decay;
    (void)amyWave;
}

void audioT303Feedback(float fb) {
    (void)fb;
}

void audioT303Duty(float duty) {
    (void)duty;
}

void audioT303Wavefold(float depth) {
    (void)depth;
}

void audioT303WavefoldAsym(float depth) {
    (void)depth;
}

void audioT303SetFilterType(uint8_t filterType) {
    (void)filterType;
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

void audioDrum2Hit(uint8_t padIdx, float vel, uint8_t midiNote, float decayMs) {
    (void)padIdx;
    (void)vel;
    (void)midiNote;
    (void)decayMs;
}

void audioDrumSynthHit(uint8_t kit, uint8_t pad, float vel, uint8_t pitch, float decayMs) {
    (void)kit;
    (void)pad;
    (void)vel;
    (void)pitch;
    (void)decayMs;
}

void audioDrumSynthAllOff(uint8_t kit) {
    (void)kit;
}

void audioModularOscInit(uint8_t ch, uint8_t tableIdx) {
    (void)ch;
    (void)tableIdx;
}

void audioModularSetTable(uint8_t ch, uint8_t tableIdx) {
    (void)ch;
    (void)tableIdx;
}

void audioModularSetWtPos(uint8_t ch, float pos01) {
    (void)ch;
    (void)pos01;
}

void audioModularNoteOn(uint8_t note, float vel, float detuneSemisB) {
    (void)note;
    (void)vel;
    (void)detuneSemisB;
}

void audioModularNoteOff(uint8_t note) {
    (void)note;
}

void audioModularAllNotesOff() {

}

void audioModularSetFilter(float cutoffHz, float resonance) {
    (void)cutoffHz;
    (void)resonance;
}

void audioModularSetEnvelope(const EnvParams &env) {
    (void)env;
}

void audioModularSetPitchBend(float ratio) {
    (void)ratio;
}

