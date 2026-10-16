// The Mutable Instruments model oscillator (mi_osc.h, ADR-039): an adapter between our module interface (q15 blocks, our pitch units, a gate
// input) and the Braids / Plaits algorithms in src/engine/mi. Only the selected model's object lives in the voice: Braids' MacroOscillator or
// one Plaits engine, built in place in `store_` when the model changes (no allocation on the audio path; no rebuild of the graph).
#include "engine/modules/mi_osc.h"
#include <new>
#if defined(ENGINE_PROFILE) && defined(ARDUINO_ARCH_ESP32)
#include <esp_cpu.h>
#endif
#include "engine/dsp/block.h"
#include "engine/dsp/fdsp.h"
#include "engine/modules/builtin.h"
#include "engine/mi/braids/macro_oscillator.h"
#include "engine/mi/plaits/dsp/engine/bass_drum_engine.h"
#include "engine/mi/plaits/dsp/engine/grain_engine.h"
#include "engine/mi/plaits/dsp/engine/hi_hat_engine.h"
#include "engine/mi/plaits/dsp/engine/noise_engine.h"
#include "engine/mi/plaits/dsp/engine/snare_drum_engine.h"
#include "engine/mi/plaits/dsp/engine/virtual_analog_engine.h"
#include "engine/mi/plaits/dsp/engine2/chiptune_engine.h"
#include "engine/mi/plaits/dsp/engine2/phase_distortion_engine.h"
#include "engine/mi/plaits/dsp/engine2/virtual_analog_vcf_engine.h"
#include "engine/mi/plaits/dsp/engine2/wave_terrain_engine.h"

namespace sc {

#if defined(ENGINE_PROFILE) && defined(ARDUINO_ARCH_ESP32)
uint32_t g_mi_prof[MI_MODELS];
#endif

namespace {

enum Src : uint8_t { BRAIDS, PLAITS };
enum PlaitsId : uint8_t { P_NOISE, P_CHIP, P_PD, P_VA, P_VA_VCF, P_GRAIN, P_TERRAIN, P_BD, P_SD, P_HH };

// One row per model: source, Braids shape or Plaits engine, output gain (Plaits' own post-processing gain, negative = soft limit; Braids 1.0),
// percussive (struck by the gate: Plaits gets a trigger, Braids a Strike), short name, aux (a Plaits drum's second model, rendered to its aux
// output: the drum engines render only the output they are given, so each variant costs one model).
struct ModelDef { Src src; uint8_t id; float gain; bool perc; const char *name; bool aux; };
const ModelDef kModels[MI_MODELS] = {
    {BRAIDS, braids::MACRO_OSC_SHAPE_CSAW, 1, false, "CSaw"},
    {BRAIDS, braids::MACRO_OSC_SHAPE_MORPH, 1, false, "Morph"},
    {BRAIDS, braids::MACRO_OSC_SHAPE_SAW_SQUARE, 1, false, "SawSq"},
    {BRAIDS, braids::MACRO_OSC_SHAPE_SINE_TRIANGLE, 1, false, "SinTri"},
    {BRAIDS, braids::MACRO_OSC_SHAPE_BUZZ, 1, false, "Buzz"},
    {BRAIDS, braids::MACRO_OSC_SHAPE_SQUARE_SUB, 1, false, "SqSub"},
    {BRAIDS, braids::MACRO_OSC_SHAPE_SAW_SUB, 1, false, "SawSub"},
    {BRAIDS, braids::MACRO_OSC_SHAPE_SQUARE_SYNC, 1, false, "SqSync"},
    {BRAIDS, braids::MACRO_OSC_SHAPE_SAW_SYNC, 1, false, "SwSync"},
    {BRAIDS, braids::MACRO_OSC_SHAPE_TOY, 1, false, "Toy"},
    {BRAIDS, braids::MACRO_OSC_SHAPE_DIGITAL_FILTER_LP, 1, false, "ZLP"},
    {BRAIDS, braids::MACRO_OSC_SHAPE_DIGITAL_FILTER_PK, 1, false, "ZPk"},
    {BRAIDS, braids::MACRO_OSC_SHAPE_DIGITAL_FILTER_BP, 1, false, "ZBP"},
    {BRAIDS, braids::MACRO_OSC_SHAPE_DIGITAL_FILTER_HP, 1, false, "ZHP"},
    {BRAIDS, braids::MACRO_OSC_SHAPE_VOSIM, 1, false, "Vosim"},
    {BRAIDS, braids::MACRO_OSC_SHAPE_FEEDBACK_FM, 1, false, "FbFM"},
    {BRAIDS, braids::MACRO_OSC_SHAPE_CHAOTIC_FEEDBACK_FM, 1, false, "Chaos"},
    {BRAIDS, braids::MACRO_OSC_SHAPE_KICK, 1, true, "Kick"},
    {BRAIDS, braids::MACRO_OSC_SHAPE_FILTERED_NOISE, 1, false, "FNoise"},
    {BRAIDS, braids::MACRO_OSC_SHAPE_TWIN_PEAKS_NOISE, 1, false, "Twin"},
    {BRAIDS, braids::MACRO_OSC_SHAPE_CLOCKED_NOISE, 1, false, "Clock"},
    {BRAIDS, braids::MACRO_OSC_SHAPE_DIGITAL_MODULATION, 1, false, "DigMod"},
    {BRAIDS, braids::MACRO_OSC_SHAPE_QUESTION_MARK, 1, false, "Morse"},
    {BRAIDS, braids::MACRO_OSC_SHAPE_TRIPLE_SAW, 1, false, "3Saw"},
    {BRAIDS, braids::MACRO_OSC_SHAPE_TRIPLE_SQUARE, 1, false, "3Sq"},
    {BRAIDS, braids::MACRO_OSC_SHAPE_TRIPLE_TRIANGLE, 1, false, "3Tri"},
    {BRAIDS, braids::MACRO_OSC_SHAPE_TRIPLE_SINE, 1, false, "3Sine"},
    {BRAIDS, braids::MACRO_OSC_SHAPE_TRIPLE_RING_MOD, 1, false, "3Ring"},
    {BRAIDS, braids::MACRO_OSC_SHAPE_WAVETABLES, 1, false, "WTbl"},
    {BRAIDS, braids::MACRO_OSC_SHAPE_WAVE_MAP, 1, false, "WMap"},
    {BRAIDS, braids::MACRO_OSC_SHAPE_WAVE_LINE, 1, false, "WLine"},
    {BRAIDS, braids::MACRO_OSC_SHAPE_GRANULAR_CLOUD, 1, false, "Cloud"},
    {BRAIDS, braids::MACRO_OSC_SHAPE_CYMBAL, 1, false, "Cymbal"},   // continuous metallic noise: our envelope shapes it
    {BRAIDS, braids::MACRO_OSC_SHAPE_SNARE, 1, true, "BSnare"},
    {PLAITS, P_NOISE, -1.0f, false, "PNoise"},
    {PLAITS, P_CHIP, 0.5f, false, "Chip"},
    {PLAITS, P_PD, 0.7f, false, "PhDist"},
    {PLAITS, P_VA, 0.8f, false, "VA"},
    {PLAITS, P_VA_VCF, 1.0f, false, "VAVcf"},
    {PLAITS, P_GRAIN, 0.7f, false, "Grain"},
    {PLAITS, P_TERRAIN, 0.7f, false, "Terrn"},
    {PLAITS, P_BD, 0.8f, true, "PBass", false},       // analog (808-like)
    {PLAITS, P_BD, 0.8f, true, "PBassS", true},       // synthetic
    {PLAITS, P_SD, 0.8f, true, "PSnare", false},      // analog
    {PLAITS, P_SD, 0.8f, true, "PSnrS", true},        // synthetic
    {PLAITS, P_HH, 0.8f, true, "PHat", false},        // square-wave metallic noise
    {PLAITS, P_HH, 0.8f, true, "PHat2", true},        // ring-modulated noise
};

template <class... T> constexpr size_t max_size() { size_t m = 0; for (size_t s : {sizeof(T)...}) m = s > m ? s : m; return m; }
constexpr size_t kStore = max_size<braids::MacroOscillator, plaits::NoiseEngine, plaits::ChiptuneEngine, plaits::PhaseDistortionEngine,
                                   plaits::VirtualAnalogEngine, plaits::VirtualAnalogVCFEngine, plaits::GrainEngine, plaits::WaveTerrainEngine,
                                   plaits::BassDrumEngine, plaits::SnareDrumEngine, plaits::HiHatEngine>();
constexpr int kChunk = 16;                                  // samples per call into the model (Braids takes up to 24, Plaits kMaxBlockSize = 24)
constexpr size_t kScratch = sizeof(float) * plaits::kMaxBlockSize * 4 + 16;   // the most a selected Plaits engine allocates (phase distortion, terrain)

constexpr int32_t kSemi = 256;                              // our pitch unit: 1/256 semitone
inline int32_t scaled(q15 m, int32_t range) { return static_cast<int32_t>((static_cast<int64_t>(m) * range) >> 15); }
inline q15 clamp_unit(int32_t v) { return static_cast<q15>(v < 0 ? 0 : (v > 32767 ? 32767 : v)); }

}  // namespace

const char *mi_model_name(int model) { return model >= 0 && model < MI_MODELS ? kModels[model].name : "?"; }
bool mi_model_percussive(int model) { return model >= 0 && model < MI_MODELS && kModels[model].perc; }

class MiOsc : public Module {
public:
    const ModuleInfo &info() const override {
        static const ModuleInfo i = {"MiOsc", Scope::Voice, 2, 1, MI_N, false, {"pitch", "gate"}, {"out"},
            {{"model", MI_CSAW, 0, MI_MODELS - 1}, {"pitch", 60 * kSemi, 0, 127 * kSemi}, {"timbre", 16384, 0, kUnity},
             {"morph", 16384, 0, kUnity}, {"harm", 16384, 0, kUnity}, {"level", kUnity, 0, kUnity}, {"pitch_mod", 12 * kSemi, 0, 96 * kSemi}}};
        return i;
    }
    bool init(Memory &) override { build(model_ < 0 ? MI_CSAW : model_); return true; }
    void reset() override { build(model_); gate_ = false; }   // a fresh model at every voice start (phases, envelopes)
    void set_param(int idx, int32_t v) override {
        switch (idx) {
            case MI_MODEL: if (v != model_) build(v); break;
            case MI_PITCH: pitch_ = v; break;
            case MI_TIMBRE: timbre_ = static_cast<q15>(v); break;
            case MI_MORPH_P: morph_ = static_cast<q15>(v); break;
            case MI_HARM: harm_ = static_cast<q15>(v); break;
            case MI_LEVEL: level_ = static_cast<q15>(v); break;
            case MI_PITCH_MOD: pmod_ = v; break;
        }
    }

    // Not SC_HOT (IRAM): the work is in the Braids / Plaits render functions, which stay in flash; the adapter in IRAM alone gains nothing (and its
    // literal pool would land after it: the link fails). Moving the models' code to IRAM is a decision for after the board measurements.
    void process(const ProcessCtx &ctx, const Ports &p) override {
#if defined(ENGINE_PROFILE) && defined(ARDUINO_ARCH_ESP32)
        const uint32_t prof_t0 = esp_cpu_get_cycle_count();
#endif
        const int n = ctx.frames;
        const q15 *cv = p.in[0], *gate = p.in[1], *mp = p.mod[MI_PITCH];
        const q15 *mt = p.mod[MI_TIMBRE], *mm = p.mod[MI_MORPH_P], *mh = p.mod[MI_HARM];
        const q15 timbre = clamp_unit(timbre_ + (mt ? mt[0] : 0)), morph = clamp_unit(morph_ + (mm ? mm[0] : 0));
        const q15 harm = clamp_unit(harm_ + (mh ? mh[0] : 0));
        const ModelDef &d = kModels[model_];
        q15 *out = p.out[0];
        for (int c = 0; c < n; c += kChunk) {
            const int len = n - c < kChunk ? n - c : kChunk;
            const int32_t pitch = pitch_ + scaled(cv[c], kPitchCvSpan) + (mp ? scaled(mp[c], pmod_) : 0);   // ours: 1/256 semitone, MIDI note * 256
            bool rise = false;
            for (int i = c; i < c + len; i++) { const bool g = gate[i] > 16384; rise |= g && !gate_; gate_ = g; }
            if (d.src == BRAIDS) {
                braids::MacroOscillator *o = braids_osc();
                if (rise && d.perc) o->Strike();
                o->set_pitch(static_cast<int16_t>(clamp_i32(pitch >> 1, 0, 32767)));             // Braids: 1/128 semitone
                o->set_parameters(timbre, morph);
                int16_t buf[kChunk];
                o->Render(sync_, buf, static_cast<size_t>(len));
                for (int i = 0; i < len; i++) out[c + i] = mul15(buf[i], level_);
            } else {
                plaits::EngineParameters ep;
                ep.trigger = d.perc ? (rise ? plaits::TRIGGER_RISING_EDGE : plaits::TRIGGER_LOW) | (gate_ ? plaits::TRIGGER_HIGH : 0)
                                    : plaits::TRIGGER_UNPATCHED;
                ep.note = static_cast<float>(pitch) * (1.0f / 256.0f);
                ep.timbre = q15_to_float(timbre);
                ep.morph = q15_to_float(morph);
                ep.harmonics = q15_to_float(harm);
                ep.accent = ctx.voice ? 0.5f + 0.5f * q15_to_float(ctx.voice->velocity) : 0.8f;
                float o[kChunk], aux[kChunk];
                bool enveloped = d.perc;
                if (d.aux) plaits_engine()->Render(ep, nullptr, o, static_cast<size_t>(len), &enveloped);
                else plaits_engine()->Render(ep, o, d.perc ? nullptr : aux, static_cast<size_t>(len), &enveloped);   // a drum skips its other model
                const float g = (d.gain < 0 ? -d.gain : d.gain) * q15_to_float(level_);
                for (int i = 0; i < len; i++) {
                    const float v = d.gain < 0 ? fdsp::SoftLimit(o[i] * g) : o[i] * g;
                    out[c + i] = sat16(static_cast<int32_t>(v * 32767.0f));
                }
            }
        }
#if defined(ENGINE_PROFILE) && defined(ARDUINO_ARCH_ESP32)
        g_mi_prof[model_] += esp_cpu_get_cycle_count() - prof_t0;
#endif
    }

private:
    braids::MacroOscillator *braids_osc() { return reinterpret_cast<braids::MacroOscillator *>(store_); }
    plaits::Engine *plaits_engine() { return reinterpret_cast<plaits::Engine *>(store_); }

    // Builds the model's object in place over the previous one (the Braids and Plaits objects own no resources: nothing to destroy). Plaits
    // engines take their scratch buffers from scratch_.
    void build(int m) {
        model_ = m < 0 || m >= MI_MODELS ? 0 : m;
        const ModelDef &d = kModels[model_];
        if (d.src == BRAIDS) {
            auto *o = new (store_) braids::MacroOscillator();
            o->Init();
            o->set_shape(static_cast<braids::MacroOscillatorShape>(d.id));
            return;
        }
        plaits::Engine *e = nullptr;
        switch (d.id) {
            case P_NOISE:   e = new (store_) plaits::NoiseEngine(); break;
            case P_CHIP:    e = new (store_) plaits::ChiptuneEngine(); break;
            case P_PD:      e = new (store_) plaits::PhaseDistortionEngine(); break;
            case P_VA:      e = new (store_) plaits::VirtualAnalogEngine(); break;
            case P_VA_VCF:  e = new (store_) plaits::VirtualAnalogVCFEngine(); break;
            case P_GRAIN:   e = new (store_) plaits::GrainEngine(); break;
            case P_TERRAIN: e = new (store_) plaits::WaveTerrainEngine(); break;
            case P_BD:      e = new (store_) plaits::BassDrumEngine(); break;
            case P_SD:      e = new (store_) plaits::SnareDrumEngine(); break;
            default:        e = new (store_) plaits::HiHatEngine(); break;
        }
        fdsp::BufferAllocator alloc(scratch_, sizeof scratch_);
        e->Init(&alloc);
        e->Reset();
    }

    alignas(8) unsigned char store_[kStore];
    alignas(8) unsigned char scratch_[kScratch];
    uint8_t sync_[kChunk] = {};                         // Braids' hard-sync input: never used here
    int32_t model_ = -1, pitch_ = 60 * kSemi, pmod_ = 12 * kSemi;
    q15 timbre_ = 16384, morph_ = 16384, harm_ = 16384, level_ = kUnity;
    bool gate_ = false;
};

template <typename T>
ModuleType type_of() { static T probe; return {&probe.info(), &create_module<T>}; }

void register_mi_osc(Registry &r) { r.add(T_MIOSC, type_of<MiOsc>()); }

void mi_render_preview(int model, q15 timbre, q15 morph, q15 harm, int32_t pitch, int settle, int decim, q15 *out, int n) {
    static MiOsc osc;                                       // ~1.1 KB: static, not on the UI task's stack
    osc.set_param(MI_MODEL, model);
    osc.reset();
    osc.set_param(MI_PITCH, pitch);
    osc.set_param(MI_TIMBRE, timbre);
    osc.set_param(MI_MORPH_P, morph);
    osc.set_param(MI_HARM, harm);
    osc.set_param(MI_LEVEL, kUnity);
    q15 cv[kChunk] = {}, gate[kChunk], buf[kChunk];
    for (int i = 0; i < kChunk; i++) gate[i] = 32767;
    Ports p{};
    p.in[0] = cv; p.in[1] = gate; p.out[0] = buf;
    ProcessCtx ctx{};
    ctx.frames = kChunk;
    if (decim < 1) decim = 1;
    const int skip = mi_model_percussive(model) ? 0 : settle, total = n * decim;
    for (int i = 0; i < n; i++) out[i] = 0;
    for (int done = -skip; done < total; done += kChunk) {
        osc.process(ctx, p);
        for (int i = 0; i < kChunk; i++) {
            const int k = done + i;
            if (k < 0 || k >= total) continue;
            q15 &o = out[k / decim];
            if (k % decim == 0 || (buf[i] < 0 ? -buf[i] : buf[i]) > (o < 0 ? -o : o)) o = buf[i];
        }
    }
}

}  // namespace sc
