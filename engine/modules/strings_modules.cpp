#include "engine/modules/strings_modules.h"
#include <cmath>
#include "engine/dsp/delay.h"
#include "engine/dsp/phase.h"
#include "engine/dsp/wavetables.h"
#include "engine/modules/builtin.h"

namespace sc {
namespace {

constexpr int kLog2Block = kBlock == 8 ? 3 : kBlock == 16 ? 4 : kBlock == 32 ? 5 : kBlock == 64 ? 6 : kBlock == 128 ? 7 : 8;
static_assert((1 << kLog2Block) == kBlock, "kBlock must be a power of two");
constexpr int32_t kEnvOne = 1 << 30;                    // envelope full scale (Q30)

/* ------------------------------------------------------------------ Strings voice */

static_assert(int(STRW_SAW) == int(WT_SAW) && int(STRW_PULSE) == int(WT_PULSE) && int(STRW_TRI) == int(WT_TRI), "the Strings waves are the table oscillator's");
template <int W, bool Mip>
inline int32_t osc(uint32_t ph, uint32_t pw_off, const int16_t *tab) { return wt_osc<W, Mip>(ph, pw_off, tab); }

class Strings : public Module {
public:
    const ModuleInfo &info() const override {
        static const ModuleInfo i = {"Strings", Scope::Voice, 2, 1, STR_N, false, {"pitch", "gate"}, {"out"},
            {{"wave", STRW_SAW, 0, STRW_N - 1}, {"osc", STRO_MIP, 0, STRO_N - 1}, {"detune", 40, 0, 512}, {"mix", kUnity, 0, kUnity},
             {"pw", 16384, 1638, 31130},
             {"attack", 300, 1, 10000}, {"decay", 500, 1, 10000}, {"sustain", 26000, 0, kUnity}, {"release", 900, 1, 10000},
             {"lp_on", 0, 0, 1}, {"lp_cut", 96 * 256, 24 * 256, 135 * 256}, {"lp_env", 24 * 256, 0, 96 * 256}, {"lp_key", 16384, 0, kUnity},
             {"level", 16384, 0, kUnity}}};
        return i;
    }
    bool init(Memory &) override {
        a4_ = inc_a4();
        for (int k = 0; k < STR_N; k++) set_param(k, info().param[k].def);
        static uint32_t seed = 0x9e3779b9u;                                // a different phase sequence per instance
        seed = seed * 1664525u + 1013904223u;
        rng_ = seed;
        return true;
    }
    void reset() override {                                                // voice start (and a new instance)
        rng_ = rng_ * 1664525u + 1013904223u;
        ph1_ = rng_;                                                       // random start phases: detuned voices never start in phase
        rng_ = rng_ * 1664525u + 1013904223u;
        ph2_ = rng_;
        env_ = 0;
        stage_ = kIdle;
        gate_ = false;
        lp_y_ = 0;
        amp_ = 0;
    }
    void set_param(int idx, int32_t v) override {
        switch (idx) {
            case STR_WAVE:    wave_ = v < 0 ? 0 : (v >= STRW_N ? STRW_N - 1 : v); break;
            case STR_OSC:     mip_ = v != 0; break;
            case STR_DETUNE:  detune_ = v; break;
            case STR_MIX:     mix_ = v; break;
            case STR_PW:      pw_off_ = static_cast<uint32_t>(v) << 17; break;     // q15 duty -> phase offset (2^32 = one cycle)
            case STR_ATTACK:  att_step_ = step_for(v, kEnvOne); break;
            case STR_DECAY:   dec_ms_ = v; break;
            case STR_SUSTAIN: sus_ = static_cast<int32_t>(static_cast<int64_t>(v) * kEnvOne / kUnity); break;
            case STR_RELEASE: rel_step_ = step_for(v, kEnvOne); break;
            case STR_LP_ON:   lp_on_ = v != 0; break;
            case STR_LP_CUT:  lp_cut_ = v; break;
            case STR_LP_ENV:  lp_env_ = v; break;
            case STR_LP_KEY:  lp_key_ = v; break;
            case STR_LEVEL:   level_ = v; break;
            default: break;
        }
        dec_step_ = step_for(dec_ms_, kEnvOne - sus_);
    }

    SC_HOT void process(const ProcessCtx &ctx, const Ports &p) override {
        const bool gate = p.in[1][0] > 16384;
        if (gate && !gate_) stage_ = kAttack;                              // from the current level: a stolen voice does not click
        else if (!gate && gate_ && stage_ != kIdle) stage_ = kRelease;
        gate_ = gate;
        if (stage_ == kIdle) {
            for (int i = 0; i < ctx.frames; i++) p.out[0][i] = 0;
            amp_ = 0;
            return;
        }
        step_env();

        const int32_t pitch = kPitchCvCenter + static_cast<int32_t>(static_cast<int64_t>(p.in[0][0]) * kPitchCvSpan / 32768);
        const uint32_t inc1 = pitch_to_inc(pitch - detune_ / 2, a4_), inc2 = pitch_to_inc(pitch + (detune_ - detune_ / 2), a4_);
        const int32_t vel = ctx.voice ? ctx.voice->velocity : kUnity;
        const int32_t gain = (level_ * vel) >> 15;                                        // q15
        const int32_t amp_end = static_cast<int32_t>((static_cast<int64_t>(env_ >> 15) * gain) >> 15);   // q15
        if (lp_on_) {
            const int32_t env_q15 = env_ >> 15;
            const int32_t cut = lp_cut_ + static_cast<int32_t>((static_cast<int64_t>(pitch - kPitchCvCenter) * lp_key_) >> 15)
                                + static_cast<int32_t>((static_cast<int64_t>(lp_env_) * env_q15) >> 15);
            const float w = static_cast<float>(pitch_to_inc(cut, a4_)) * (6.2831853f / 4294967296.0f);
            lp_g_ = static_cast<int32_t>((1.0f - std::exp(-w)) * 32767.0f);              // one-pole coefficient, q15
        }
        Ctx c{inc1, inc2, pw_off_, mix_, amp_, (amp_end - amp_) >> kLog2Block, lp_g_};
        c.t1 = table(inc1);
        c.t2 = table(inc2);
        (this->*kRender[wave_][mip_ ? 1 : 0][lp_on_ ? 1 : 0])(c, p.out[0], ctx.frames);
        amp_ = amp_end;
    }

private:
    enum Stage : uint8_t { kIdle, kAttack, kDecay, kSustain, kRelease };
    struct Ctx {
        uint32_t inc1, inc2, pw_off;
        int32_t mix, amp, damp, lp_g;
        const int16_t *t1 = nullptr, *t2 = nullptr;
    };

    // Envelope step per block for a ramp of `ms` milliseconds over `span` (Q30); at least 1.
    static int32_t step_for(int32_t ms, int32_t span) {
        const int64_t blocks = static_cast<int64_t>(ms < 1 ? 1 : ms) * kControlRate / 1000;
        const int64_t s = span / (blocks < 1 ? 1 : blocks);
        return static_cast<int32_t>(s < 1 ? 1 : s);
    }

    // One block of the linear ADSR (the level at the END of this block; the samples ramp to it from the previous one).
    void step_env() {
        switch (stage_) {
            case kAttack:
                env_ += att_step_;
                if (env_ >= kEnvOne) { env_ = kEnvOne; stage_ = sus_ >= kEnvOne ? kSustain : kDecay; }
                break;
            case kDecay:
                env_ -= dec_step_;
                if (env_ <= sus_) { env_ = sus_; stage_ = kSustain; }
                break;
            case kSustain:
                env_ = sus_;                                               // follows a live sustain change
                break;
            case kRelease:
                env_ -= rel_step_;
                if (env_ <= 0) { env_ = 0; stage_ = kIdle; }
                break;
            default: break;
        }
    }

    const int16_t *table(uint32_t inc) const { return mip_ ? wt_table(wave_, inc) : nullptr; }

    template <int W, bool Mip, bool Lp>
    SC_HOT void render(Ctx &c, q15 *out, int n) {
        uint32_t p1 = ph1_, p2 = ph2_;
        int32_t amp = c.amp, y = lp_y_;
        for (int i = 0; i < n; i++) {
            const int32_t o1 = osc<W, Mip>(p1, c.pw_off, c.t1), o2 = osc<W, Mip>(p2, c.pw_off, c.t2);
            int32_t s = (o1 + ((o2 * c.mix) >> 15)) >> 1;
            if (Lp) { y += ((s - y) * c.lp_g) >> 15; s = y; }
            amp += c.damp;
            out[i] = static_cast<q15>((s * amp) >> 15);
            p1 += c.inc1;
            p2 += c.inc2;
        }
        ph1_ = p1;
        ph2_ = p2;
        lp_y_ = y;
    }

    using RenderFn = void (Strings::*)(Ctx &, q15 *, int);
    static const RenderFn kRender[STRW_N][2][2];

    uint32_t a4_ = 0, rng_ = 0, ph1_ = 0, ph2_ = 0, pw_off_ = 0x80000000u;
    int32_t env_ = 0, att_step_ = 1, dec_step_ = 1, rel_step_ = 1, sus_ = 0, dec_ms_ = 500;
    int32_t amp_ = 0, lp_y_ = 0, lp_g_ = 32767;
    int32_t wave_ = STRW_SAW, detune_ = 40, mix_ = kUnity, lp_cut_ = 96 * 256, lp_env_ = 0, lp_key_ = 0, level_ = 16384;
    Stage stage_ = kIdle;
    bool mip_ = true, lp_on_ = false, gate_ = false;
};

#define SC_STR_ROW(W) {{&Strings::render<W, false, false>, &Strings::render<W, false, true>}, {&Strings::render<W, true, false>, &Strings::render<W, true, true>}}
const Strings::RenderFn Strings::kRender[STRW_N][2][2] = {SC_STR_ROW(STRW_SAW), SC_STR_ROW(STRW_PULSE), SC_STR_ROW(STRW_TRI)};
#undef SC_STR_ROW

/* ------------------------------------------------------------------ Ensemble */

class Ensemble : public Module {
public:
    const ModuleInfo &info() const override {
        static const ModuleInfo i = {"Ensemble", Scope::Global, 2, 2, ENS_N, false, {"L", "R"}, {"L", "R"},
            {{"rate", 60, 5, 500}, {"depth", 22000, 0, kUnity}, {"shimmer", 12000, 0, kUnity}, {"mix", 22000, 0, kUnity}}};
        return i;
    }
    bool init(Memory &m) override {
        base_ = static_cast<int32_t>(kBaseMs * 1e-3 * kSampleRate * 65536.0);
        slow_amp_ = static_cast<int32_t>(kSlowMs * 1e-3 * kSampleRate * 65536.0);
        fast_amp_ = static_cast<int32_t>(kFastMs * 1e-3 * kSampleRate * 65536.0);
        for (int k = 0; k < ENS_N; k++) set_param(k, info().param[k].def);
        for (auto &d : d_) d = base_;
        const int room = static_cast<int>((kBaseMs + kSlowMs + kFastMs) * 1e-3 * kSampleRate) + 8;
        return line_.init(*m.fast, room);
    }
    void reset() override { line_.clear(); }
    void set_param(int idx, int32_t v) override {
        switch (idx) {
            case ENS_RATE:    slow_inc_ = block_inc(static_cast<float>(v) / 100.0f); fast_inc_ = block_inc(static_cast<float>(v) / 100.0f * kFastRatio); break;
            case ENS_DEPTH:   depth_ = v; break;
            case ENS_SHIMMER: shimmer_ = v; break;
            case ENS_MIX:     mix_ = v; break;
            default: break;
        }
    }
    SC_HOT void process(const ProcessCtx &ctx, const Ports &p) override {
        const int n = ctx.frames;
        // Delay of each tap at the end of this block (Q16 samples); the samples ramp to it from the previous block's value.
        slow_ph_ += slow_inc_;
        fast_ph_ += fast_inc_;
        int32_t d_end[3], d_step[3];
        for (int k = 0; k < 3; k++) {
            const uint32_t off = static_cast<uint32_t>(k) * 0x55555555u;                   // 0 / 120 / 240 degrees
            const int32_t s = sine(slow_ph_ + off), f = sine(fast_ph_ + off);
            const int32_t slow = static_cast<int32_t>((static_cast<int64_t>(slow_amp_) * ((s * depth_) >> 15)) >> 15);
            const int32_t fast = static_cast<int32_t>((static_cast<int64_t>(fast_amp_) * ((f * shimmer_) >> 15)) >> 15);
            d_end[k] = base_ + slow + fast;
            d_step[k] = (d_end[k] - d_[k]) >> kLog2Block;
        }
        const int32_t wet = mix_, dry = kUnity - mix_;
        int32_t d0 = d_[0], d1 = d_[1], d2 = d_[2];
        for (int i = 0; i < n; i++) {
            const int32_t inl = p.in[0][i], inr = p.in[1][i];
            const q15 t0 = line_.read_lerp(static_cast<uint32_t>(d0)), t1 = line_.read_lerp(static_cast<uint32_t>(d1)), t2 = line_.read_lerp(static_cast<uint32_t>(d2));
            line_.write(static_cast<q15>((inl + inr) >> 1));
            const int32_t wl = (static_cast<int32_t>(t0) + t1) >> 1, wr = (static_cast<int32_t>(t1) + t2) >> 1;
            p.out[0][i] = sat16((inl * dry + wl * wet) >> 15);
            p.out[1][i] = sat16((inr * dry + wr * wet) >> 15);
            d0 += d_step[0];
            d1 += d_step[1];
            d2 += d_step[2];
        }
        for (int k = 0; k < 3; k++) d_[k] = d_end[k];
    }
private:
    static constexpr double kBaseMs = 7.0, kSlowMs = 3.5, kFastMs = 0.35;     // centre delay, slow and fast modulation depth at 100 %
    static constexpr float kFastRatio = 10.5f;                                  // the fast LFO (vibrato shimmer) relative to the slow one
    static uint32_t block_inc(float hz) { return static_cast<uint32_t>(static_cast<double>(hz) / kControlRate * 4294967296.0); }
    DelayLine line_;
    int32_t base_ = 0, slow_amp_ = 0, fast_amp_ = 0, depth_ = 0, shimmer_ = 0, mix_ = 0;
    int32_t d_[3] = {0, 0, 0};
    uint32_t slow_ph_ = 0, fast_ph_ = 0, slow_inc_ = 0, fast_inc_ = 0;
};

template <typename T>
ModuleType type_of() { static T probe; return {&probe.info(), &create_module<T>}; }

}  // namespace

void register_strings_modules(Registry &reg) {
    reg.add(T_STRINGS, type_of<Strings>());
    reg.add(T_ENSEMBLE, type_of<Ensemble>());
}

}  // namespace sc
