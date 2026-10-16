#include "engine/modules/synth_modules.h"
#include <cmath>
#include "engine/dsp/block.h"
#include "engine/dsp/curve.h"
#include "engine/dsp/interp.h"
#include "engine/dsp/osc.h"
#include "engine/dsp/phase.h"
#include "engine/dsp/smooth.h"
#include "engine/dsp/svf.h"
#include "engine/dsp/util.h"
#include "engine/dsp/wavetables.h"

namespace sc {
namespace {

constexpr int32_t kSemi = 256;                       // pitch units per semitone (12 * 256 = one octave)

inline int32_t scaled(q15 m, int32_t range) { return static_cast<int32_t>((static_cast<int64_t>(m) * range) >> 15); }

/* ------------------------------------------------------------------ Osc */

template <Scope S>
class Osc : public Module {
public:
    const ModuleInfo &info() const override {
        static const ModuleInfo i = {"Osc", S, 1, 1, OSC_N, false, {"pitch"}, {"out"},
            {{"wave", WAVE_SAW_, 0, 5}, {"pitch", 60 * kSemi, 0, 127 * kSemi}, {"pw", 16384, 1024, 31744},
             {"level", kUnity, 0, kUnity}, {"pitch_mod", 12 * kSemi, 0, 96 * kSemi}, {"quality", 0, 0, 2}}};
        return i;
    }
    bool init(Memory &) override { a4_ = inc_a4(); return true; }
    void reset() override { ph_ = 0; }
    void set_param(int idx, int32_t v) override {
        switch (idx) {
            case OSC_WAVE: wave_ = v; break;
            case OSC_PITCH: pitch_ = v; break;
            case OSC_PW: pw_ = static_cast<q15>(v); break;
            case OSC_LEVEL: level_ = static_cast<q15>(v); break;
            case OSC_PITCH_MOD: pmod_ = v; break;
            case OSC_QUAL: qual_ = v; break;
        }
    }
    SC_HOT void process(const ProcessCtx &ctx, const Ports &p) override {
        const int n = ctx.frames;
        const q15 *cv = p.in[0], *mp = p.mod[OSC_PITCH], *mw = p.mod[OSC_PW], *ml = p.mod[OSC_LEVEL];
        const bool varying = mp || cv[0] != cv[n - 1];
        uint32_t inc = pitch_to_inc(pitch_ + scaled(cv[0], kPitchCvSpan), a4_);
        if (qual_ != 0 && wave_ != WAVE_SINE_ && wave_ != WAVE_NOISE_) {   // table / naive waves (the table band is chosen once per block)
            const int w = wave_ == WAVE_PULSE_ ? WT_PULSE : (wave_ == WAVE_TRI_ ? WT_TRI : WT_SAW);
            const bool mip = qual_ == 1, down = wave_ == WAVE_SAW_DOWN_;
            const int16_t *tab = mip ? wt_table(w, inc) : nullptr;
            for (int i = 0; i < n; i++) {
                if (varying) inc = pitch_to_inc(pitch_ + scaled(cv[i], kPitchCvSpan) + (mp ? scaled(mp[i], pmod_) : 0), a4_);
                q15 pw = mw ? sat16(pw_ + mw[i]) : pw_;
                if (pw < 1024) pw = 1024;
                int32_t s = wt_osc_any(w, mip, ph_, static_cast<uint32_t>(pw) << 17, tab);
                if (down) s = -s;
                const q15 lv = ml ? sat16(level_ + ml[i]) : level_;
                p.out[0][i] = mul15(sat16(s), lv);
                ph_ += inc;
            }
            return;
        }
        for (int i = 0; i < n; i++) {
            if (varying) inc = pitch_to_inc(pitch_ + scaled(cv[i], kPitchCvSpan) + (mp ? scaled(mp[i], pmod_) : 0), a4_);
            q15 pw = mw ? sat16(pw_ + mw[i]) : pw_;
            if (pw < 1024) pw = 1024;
            q15 s;
            switch (wave_) {
                case WAVE_SINE_: s = sine(ph_); break;
                case WAVE_SAW_: s = osc_saw(ph_, inc); break;
                case WAVE_PULSE_: s = osc_pulse(ph_, inc, static_cast<uint32_t>(pw) << 17); break;
                case WAVE_TRI_: s = osc_tri(ph_, inc); break;
                case WAVE_SAW_DOWN_: s = neg15(osc_saw(ph_, inc)); break;
                default: s = noise_.next(); break;
            }
            q15 lv = ml ? sat16(level_ + ml[i]) : level_;
            p.out[0][i] = mul15(s, lv);
            ph_ += inc;
        }
    }
private:
    int32_t wave_ = WAVE_SAW_, pitch_ = 60 * kSemi, pmod_ = 12 * kSemi, qual_ = 0;
    q15 pw_ = 16384, level_ = kUnity;
    uint32_t ph_ = 0, a4_ = 0;
    Noise noise_{0xC0FFEEu};
};

/* ------------------------------------------------------------------ Env */

// One envelope segment: from -> to over `frames` samples along a curve (dsp/curve.h). All levels are Q31 (0 .. 2^31 - 1, so a difference fits int32).
struct Segment {
    int32_t from = 0, to = 0;
    uint32_t p = 0, inc = 0;
    const CurveLut *lut = nullptr;
    void begin(int32_t y, int32_t target, int32_t frames, const CurveLut *l) {
        from = y; to = target; p = 0; lut = l;
        inc = frames <= 1 ? 0xFFFFFFFFu : static_cast<uint32_t>((1ull << 32) / static_cast<uint64_t>(frames));
    }
    // advances one sample; returns false when the segment has just finished (y = target)
    bool step(int32_t &y) {
        uint32_t pn;
        if (__builtin_add_overflow(p, inc, &pn)) { y = to; return false; }
        p = pn;
        y = from + mulh(to - from, lut->at(p) << 16) * 2;                  // (to - from) * progress (q15) in 32-bit arithmetic: one multiply
        return true;
    }
};

inline int32_t ms_frames(int32_t ms) {
    const int64_t f = static_cast<int64_t>(ms) * kSampleRate / 1000;
    return f < 1 ? 1 : (f > 0x3FFFFFFF ? 0x3FFFFFFF : static_cast<int32_t>(f));
}

// ADSR with a hold stage, a start level and a curve (tension) on attack, decay and release. Segments have an exact length: the
// attack reaches 1.0 after `attack` ms, the release reaches 0 after `release` ms, whatever the curve. Global variant (T_ENV_G): the shared
// filter / amp envelope of the paraphonic mode, gated by GateIn.
template <Scope S>
class Env : public Module {
public:
    const ModuleInfo &info() const override {
        static const ModuleInfo i = {S == Scope::Voice ? "Env" : "EnvG", S, 1, 1, ENV_N, false, {"gate"}, {"out"},
            {{"attack", 5, 1, 20000}, {"decay", 200, 1, 20000}, {"sustain", 20000, 0, kUnity}, {"release", 300, 1, 20000},
             {"hold", 0, 0, 20000}, {"start", 0, 0, kUnity}, {"a_curve", 18000, -kUnity, kUnity}, {"d_curve", 20000, -kUnity, kUnity},
             {"r_curve", 20000, -kUnity, kUnity}}};
        return i;
    }
    bool init(Memory &) override { for (int i = 0; i < 3; i++) lut_[i].build(i == 0 ? 18000 : 20000); return true; }
    void reset() override { y_ = 0; stage_ = Idle; gate_ = false; }
    void set_param(int idx, int32_t v) override {
        switch (idx) {
            case ENV_ATTACK: fa_ = ms_frames(v); break;
            case ENV_DECAY: fd_ = ms_frames(v); break;
            case ENV_SUSTAIN: sus_ = static_cast<int32_t>(v) << 16; break;
            case ENV_RELEASE: fr_ = ms_frames(v); break;
            case ENV_HOLD: fh_ = v <= 0 ? 0 : ms_frames(v); break;
            case ENV_START: start_ = static_cast<int32_t>(v) << 16; break;
            case ENV_A_CURVE: lut_[0].build(static_cast<q15>(v)); break;
            case ENV_D_CURVE: lut_[1].build(static_cast<q15>(v)); break;
            case ENV_R_CURVE: lut_[2].build(static_cast<q15>(v)); break;
        }
    }
    SC_HOT void process(const ProcessCtx &ctx, const Ports &p) override {
        for (int i = 0; i < ctx.frames; i++) {
            const bool g = p.in[0][i] > 16384;
            if (g && !gate_) { if (y_ < start_) y_ = start_; seg_.begin(y_, kMax, fa_, &lut_[0]); stage_ = Attack; }
            else if (!g && gate_ && stage_ != Idle) { seg_.begin(y_, 0, fr_, &lut_[2]); stage_ = Release; }
            gate_ = g;
            switch (stage_) {
                case Attack:
                    if (!seg_.step(y_)) { if (fh_ > 0) { hold_ = 0; stage_ = Hold; } else { seg_.begin(y_, sus_, fd_, &lut_[1]); stage_ = Decay; } }
                    break;
                case Hold:
                    if (++hold_ >= fh_) { seg_.begin(y_, sus_, fd_, &lut_[1]); stage_ = Decay; }
                    break;
                case Decay:
                    if (!seg_.step(y_)) stage_ = Sustain;
                    break;
                case Sustain:
                    y_ += (sus_ - y_) >> 8;                                   // follows live edits of the sustain level without a step
                    break;
                case Release:
                    if (!seg_.step(y_)) { y_ = 0; stage_ = Idle; }
                    break;
                case Idle: break;
            }
            p.out[0][i] = static_cast<q15>(y_ >> 16);
        }
    }
private:
    enum Stage { Idle, Attack, Hold, Decay, Sustain, Release };
    static constexpr int32_t kMax = 0x7FFFFFFF;
    CurveLut lut_[3];
    Segment seg_;
    int32_t y_ = 0, sus_ = 20000 << 16, start_ = 0;
    int32_t fa_ = ms_frames(5), fd_ = ms_frames(200), fr_ = ms_frames(300), fh_ = 0, hold_ = 0;
    Stage stage_ = Idle;
    bool gate_ = false;
};

/* ------------------------------------------------------------------ Eg (multi-stage envelope) */

// Four breakpoints, each with a time (ms, 0 = unused), a level and a curve, played in order from 0 (or from the current level on a
// retrigger). `sustain` = how many points are played before the envelope holds until the gate goes off (then it releases to 0 over
// `release` ms with its own curve). `oneshot` ignores the gate: after the last played point it releases at once, so a kick's pitch or
// amplitude shape always plays out in full.
class Eg : public Module {
public:
    const ModuleInfo &info() const override {
        static const ModuleInfo i = {"Eg", Scope::Voice, 1, 1, EG_N, false, {"gate"}, {"out"},
            {{"t1", 1, 0, 20000}, {"l1", kUnity, 0, kUnity}, {"c1", 0, -kUnity, kUnity},
             {"t2", 100, 0, 20000}, {"l2", 16384, 0, kUnity}, {"c2", 20000, -kUnity, kUnity},
             {"t3", 0, 0, 20000}, {"l3", 0, 0, kUnity}, {"c3", 20000, -kUnity, kUnity},
             {"t4", 0, 0, 20000}, {"l4", 0, 0, kUnity}, {"c4", 20000, -kUnity, kUnity},
             {"sustain", 2, 1, 4}, {"release", 200, 1, 20000}, {"r_curve", 20000, -kUnity, kUnity}, {"oneshot", 0, 0, 1}}};
        return i;
    }
    bool init(Memory &) override { for (auto &l : lut_) l.build(0); lut_[4].build(20000); return true; }
    void reset() override { y_ = 0; stage_ = Idle; gate_ = false; }
    void set_param(int idx, int32_t v) override {
        if (idx < 12) {
            const int pt = idx / 3;
            switch (idx % 3) {
                case 0: fr_t_[pt] = v <= 0 ? 0 : ms_frames(v); break;
                case 1: lvl_[pt] = static_cast<int32_t>(v) << 16; break;
                default: lut_[pt].build(static_cast<q15>(v)); break;
            }
            return;
        }
        switch (idx) {
            case EG_SUSTAIN: sus_n_ = v < 1 ? 1 : (v > 4 ? 4 : v); break;
            case EG_RELEASE: rel_f_ = ms_frames(v); break;
            case EG_RCURVE: lut_[4].build(static_cast<q15>(v)); break;
            case EG_ONESHOT: oneshot_ = v != 0; break;
        }
    }
    SC_HOT void process(const ProcessCtx &ctx, const Ports &p) override {
        for (int i = 0; i < ctx.frames; i++) {
            const bool g = p.in[0][i] > 16384;
            if (g && !gate_) { next_point(0); }
            else if (!g && gate_ && stage_ != Idle && stage_ != Release && !oneshot_) { seg_.begin(y_, 0, rel_f_, &lut_[4]); stage_ = Release; }
            gate_ = g;
            switch (stage_) {
                case Point:
                    if (!seg_.step(y_)) next_point(pt_ + 1);
                    break;
                case Sustain:
                    if (oneshot_) { seg_.begin(y_, 0, rel_f_, &lut_[4]); stage_ = Release; }
                    break;
                case Release:
                    if (!seg_.step(y_)) { y_ = 0; stage_ = Idle; }
                    break;
                case Idle: break;
            }
            p.out[0][i] = static_cast<q15>(y_ >> 16);
        }
    }
private:
    enum Stage { Idle, Point, Sustain, Release };
    void next_point(int from_pt) {                               // starts the next used point before the sustain point, or sustains
        for (int k = from_pt; k < sus_n_; k++) {
            if (fr_t_[k] <= 0) continue;
            pt_ = k;
            seg_.begin(y_, lvl_[k], fr_t_[k], &lut_[k]);
            stage_ = Point;
            return;
        }
        stage_ = Sustain;
    }
    CurveLut lut_[5];                                            // four points + the release
    Segment seg_;
    int32_t y_ = 0, lvl_[4] = {0x7FFFFFFF, 16384 << 16, 0, 0};
    int32_t fr_t_[4] = {ms_frames(1), ms_frames(100), 0, 0}, rel_f_ = ms_frames(200), sus_n_ = 2, pt_ = 0;
    Stage stage_ = Idle;
    bool gate_ = false, oneshot_ = false;
};

/* ------------------------------------------------------------------ Lfo */

template <Scope S>
class Lfo : public Module {
public:
    const ModuleInfo &info() const override {
        static const ModuleInfo i = {"Lfo", S, 0, 1, LFO_N, false, {}, {"out"},
            {{"shape", LFOS_SINE, 0, 5}, {"rate", 2 * 12 * kSemi, -6 * 12 * kSemi, 8 * 12 * kSemi}, {"level", kUnity, 0, kUnity},
             {"unipolar", 0, 0, 1}, {"rate_mod", 2 * 12 * kSemi, 0, 8 * 12 * kSemi}}};
        return i;
    }
    bool init(Memory &) override { one_hz_ = hz_to_inc(1.0f); return true; }
    void reset() override { if (S == Scope::Voice) ph_ = 0; held_ = 0; }
    void set_param(int idx, int32_t v) override {
        switch (idx) {
            case LFO_SHAPE: shape_ = v; break;
            case LFO_RATE: rate_ = v; break;
            case LFO_LEVEL: level_ = static_cast<q15>(v); break;
            case LFO_UNIPOLAR: uni_ = v != 0; break;
            case LFO_RATE_MOD: rmod_ = v; break;
        }
    }
    SC_HOT void process(const ProcessCtx &ctx, const Ports &p) override {
        const q15 *mr = p.mod[LFO_RATE];
        uint32_t inc = rate_inc(rate_);
        for (int i = 0; i < ctx.frames; i++) {
            if (mr) inc = rate_inc(rate_ + scaled(mr[i], rmod_));
            int32_t s;
            switch (shape_) {
                case LFOS_SINE: s = sine(ph_); break;
                case LFOS_TRI: s = ph_ < 0x80000000u ? 32767 - static_cast<int32_t>(ph_ >> 15) : static_cast<int32_t>(ph_ >> 15) - 98303; break;
                case LFOS_SAW_DOWN: s = 32767 - static_cast<int32_t>(ph_ >> 16); break;
                case LFOS_SAW_UP: s = static_cast<int32_t>(ph_ >> 16) - 32768; break;
                case LFOS_SQUARE: s = ph_ < 0x80000000u ? 32767 : -32768; break;
                default: s = held_; break;
            }
            uint32_t prev = ph_;
            ph_ += inc;
            if (shape_ == LFOS_SH && ph_ < prev) held_ = static_cast<q15>(noise_.next());
            if (uni_) s = (s + 32768) >> 1;
            p.out[0][i] = mul15(sat16(s), level_);
        }
    }
private:
    uint32_t rate_inc(int32_t r) const { return exp2_scale(one_hz_, clamp_i32(r, -12 * 12 * kSemi, 12 * 12 * kSemi) * 256 / 12); }
    int32_t shape_ = LFOS_SINE, rate_ = 2 * 12 * kSemi, rmod_ = 2 * 12 * kSemi;
    q15 level_ = kUnity, held_ = 0;
    bool uni_ = false;
    uint32_t ph_ = 0, one_hz_ = 0;
    Noise noise_{0xBEEF1u};
};

/* ------------------------------------------------------------------ Filter */

template <Scope S>
class Filter : public Module {
public:
    const ModuleInfo &info() const override {
        static const ModuleInfo i = {"Filter", S, 1, 1, FLT_N, false, {"in"}, {"out"},
            {{"mode", FLTM_LP, 0, FLTM_AP}, {"sections", 2, 1, 4}, {"cutoff", 96 * kSemi, 0, 135 * kSemi}, {"res", 0, 0, kUnity},
             {"cut_mod", 60 * kSemi, 0, 120 * kSemi}, {"algo", FLTA_SVF, FLTA_SVF, FLTA_CHAM}}};
        return i;
    }
    bool init(Memory &) override { a4_ = inc_a4(); update_k(); return true; }
    void reset() override {
        for (auto &s : st_) s = SvfStateF{};
        for (auto &s : lad_) s = 0.0f;
        lp6_ = ch_lp_ = ch_bp_ = 0.0f;
        cut_s_ = cutoff_; cut_q_ = cutoff_ << 8;
    }
    void set_param(int idx, int32_t v) override {
        switch (idx) {
            case FLT_MODE: mode_ = v; break;
            case FLT_SECTIONS: n_ = clamp_i32(v, 1, 4); update_k(); break;
            case FLT_CUTOFF: cutoff_ = v; break;
            case FLT_RES: res_ = v; update_k(); break;
            case FLT_CUT_MOD: cmod_ = v; break;
            case FLT_ALGO: algo_ = clamp_i32(v, FLTA_SVF, FLTA_CHAM); break;
        }
    }
    SC_HOT void process(const ProcessCtx &ctx, const Ports &p) override {
        const q15 *mc = p.mod[FLT_CUTOFF];
        // Knob moves: a one-pole glide of about 20 ms at any block size (a knob turns in detents; at 6 ms the steps were audible), with 8 fraction
        // bits so a slow approach does not stall. The coefficients follow it across the block (prev -> cut_s_), not in one step per block.
        const int32_t prev = cut_s_;
        cut_q_ += ((cutoff_ << 8) - cut_q_) / kCutGlideDiv;
        cut_s_ = cut_q_ >> 8;
        if (cut_s_ - cutoff_ < 2 && cutoff_ - cut_s_ < 2) { cut_s_ = cutoff_; cut_q_ = cutoff_ << 8; }
        if (algo_ != FLTA_SVF) { process_lite(ctx, p, prev); return; }
        SvfCoefF c[4], dc[4];
        const int frames = ctx.frames;
        // Cutoff modulation. The coefficients cost about 150 cycles per section set, five times the filter itself, so they are not recomputed per
        // sample when the modulation is smooth (envelopes, LFOs, ramps): they are computed at the first and the last sample of the block and
        // interpolated linearly in between, which moves smoothly (no stepping). Anything that is not close to a straight line over the block
        // (audio-rate FM of the cutoff, sample & hold) is detected and keeps the exact per-sample computation.
        bool glide = false;
        const bool moving = mc || prev != cut_s_;
        if (!moving) {
            coefs(c, pitch_to_inc(cut_s_, a4_));
        } else {
#if !ENGINE_FILTER_EXACT
            const int32_t p0 = prev + (mc ? scaled(mc[0], cmod_) : 0), pe = cut_s_ + (mc ? scaled(mc[frames - 1], cmod_) : 0);
            glide = frames >= 8;
            for (int q = 1; q < 4 && glide && mc; q++) {
                const int j = frames * q / 4;
                const int32_t want = p0 + (pe - p0) * j / (frames - 1);
                const int32_t diff = prev + (cut_s_ - prev) * j / (frames - 1) + scaled(mc[j], cmod_) - want;
                if (diff > 6 || diff < -6) glide = false;              // more than 6/256 semitone off the straight line
            }
            if (glide) {
                SvfCoefF c1[4];
                coefs(c, pitch_to_inc(p0, a4_));
                coefs(c1, pitch_to_inc(pe, a4_));
                const float step = 1.0f / static_cast<float>(frames - 1);
                for (int s = 0; s < n_; s++) {
                    dc[s].a1 = (c1[s].a1 - c[s].a1) * step;
                    dc[s].a2 = (c1[s].a2 - c[s].a2) * step;
                    dc[s].a3 = (c1[s].a3 - c[s].a3) * step;
                }
            }
#endif
        }
        for (int i = 0; i < frames; i++) {
            if (moving && !glide) coefs(c, pitch_to_inc(cut_s_ + (mc ? scaled(mc[i], cmod_) : 0), a4_));
            float x = static_cast<float>(p.in[0][i]) * (1.0f / 32768.0f);
            for (int s = 0; s < n_; s++) {
                float lp, bp, hp;
                svf_tick_f(c[s], st_[s], x, lp, bp, hp);
                switch (mode_) {
                    case FLTM_LP: x = lp; break;
                    case FLTM_BP: x = c[s].k * bp; break;                 // normalised: 0 dB at the centre
                    case FLTM_HP: x = hp; break;
                    case FLTM_AP: x -= 2.0f * c[s].k * bp; break;         // lp - k bp + hp: one multiply-add over the SVF
                    default: x = lp + hp; break;
                }
                if (glide) { c[s].a1 += dc[s].a1; c[s].a2 += dc[s].a2; c[s].a3 += dc[s].a3; }
            }
            p.out[0][i] = svf_out_f(x);
        }
    }
private:
    // The light low-passes (FLT_ALGO). Float, one coefficient per block end, interpolated.
    static float lite_coef(int algo, uint32_t inc) {
        const float x = static_cast<float>(inc) * 2.3283064e-10f;                 // cycles per sample
        if (algo == FLTA_CHAM) { const float f = 2.0f * std::sin(3.14159265f * x); return f < 1.0f ? f : 1.0f; }   // f <= 1: cutoff <= fs / 6
        const float g = 1.0f - std::exp(-6.2831853f * x);                       // one-pole: exact pole at the cutoff
        return algo == FLTA_LADDER && g > 0.95f ? 0.95f : g;
    }
    SC_HOT void process_lite(const ProcessCtx &ctx, const Ports &p, int32_t prev) {
        const q15 *mc = p.mod[FLT_CUTOFF];
        const int frames = ctx.frames;
        const int32_t p0 = prev + (mc ? scaled(mc[0], cmod_) : 0), pe = cut_s_ + (mc ? scaled(mc[frames - 1], cmod_) : 0);
        float c = lite_coef(algo_, pitch_to_inc(p0, a4_));
        const float dc = frames > 1 && p0 != pe ? (lite_coef(algo_, pitch_to_inc(pe, a4_)) - c) / static_cast<float>(frames - 1) : 0.0f;
        const float r = static_cast<float>(res_) * (1.0f / 32768.0f);
        const q15 *in = p.in[0];
        q15 *out = p.out[0];
        if (algo_ == FLTA_LP6) {
            float y = lp6_;
            for (int i = 0; i < frames; i++) { y += c * (static_cast<float>(in[i]) * (1.0f / 32768.0f) - y); out[i] = svf_out_f(y); c += dc; }
            lp6_ = y;
        } else if (algo_ == FLTA_LADDER) {
            const float k = 3.9f * r, comp = 1.0f + 0.5f * k;                     // feedback near self-oscillation at full resonance; level partly restored
            float s0 = lad_[0], s1 = lad_[1], s2 = lad_[2], s3 = lad_[3];
            for (int i = 0; i < frames; i++) {
                float u = static_cast<float>(in[i]) * (1.0f / 32768.0f) - k * s3;
                u = u > 1.5f ? 1.5f : (u < -1.5f ? -1.5f : u);
                u = u - 0.14814815f * u * u * u;                                  // cubic soft clip: 1.0 at 1.5, bounds the resonance
                s0 += c * (u - s0); s1 += c * (s0 - s1); s2 += c * (s1 - s2); s3 += c * (s2 - s3);
                out[i] = svf_out_f(s3 * comp);
                c += dc;
            }
            lad_[0] = s0; lad_[1] = s1; lad_[2] = s2; lad_[3] = s3;
        } else {
            const float q = 1.0f / (0.5f + 9.5f * r);                             // res 0..1 = Q 0.5..10, like the rack's Res
            float lp = ch_lp_, bp = ch_bp_;
            for (int i = 0; i < frames; i++) {
                lp += c * bp;
                const float hp = static_cast<float>(in[i]) * (1.0f / 32768.0f) - lp - q * bp;
                bp += c * hp;
                lp = lp > 2.0f ? 2.0f : (lp < -2.0f ? -2.0f : lp);               // keep states bounded: prevents resonance blow-up and loud reset transients
                bp = bp > 2.0f ? 2.0f : (bp < -2.0f ? -2.0f : bp);
                out[i] = svf_out_f(lp);
                c += dc;
            }
            ch_lp_ = lp; ch_bp_ = bp;
        }
    }
    // Coefficients are computed in integer (table + reciprocal) and converted; the per-sample filter itself runs in single-precision float,
    // which the ESP32-S3's FPU does in about 25 cycles against 200 for the saturating fixed-point version.
    void coefs(SvfCoefF *c, uint32_t inc) const {
        const float g = static_cast<float>(static_cast<int32_t>(svf_g(inc))) * (1.0f / 268435456.0f);     // (signed convert: tan stays below 2^31)
        svf_coef_batch(g, kf_, n_, c);
    }
    void update_k() {
        static const float q[4][4] = {{0.70711f}, {0.54120f, 1.30656f}, {0.51764f, 0.70711f, 1.93185f}, {0.50979f, 0.60134f, 0.89998f, 2.56292f}};
        for (int s = 0; s < n_; s++) {
            float qs = q[n_ - 1][s];
            if (s == n_ - 1) qs *= 1.0f + 15.0f * static_cast<float>(res_) / 32768.0f;
            kf_[s] = 1.0f / qs;                                     // 1/Q
        }
    }
    static constexpr int kCutGlideDiv = kControlRate / 50 > 1 ? kControlRate / 50 : 1;     // blocks in 20 ms: the knob glide's time constant
    int32_t mode_ = FLTM_LP, n_ = 2, cutoff_ = 96 * kSemi, cut_s_ = 96 * kSemi, cut_q_ = 96 * kSemi << 8, cmod_ = 60 * kSemi, res_ = 0;
    float kf_[4] = {};
    SvfStateF st_[4];
    int32_t algo_ = FLTA_SVF;
    float lad_[4] = {}, lp6_ = 0.0f, ch_lp_ = 0.0f, ch_bp_ = 0.0f;
    uint32_t a4_ = 0;
};

/* ------------------------------------------------------------------ Shaper (naive, ADR-013) */

template <Scope S>
class Shaper : public Module {
public:
    const ModuleInfo &info() const override {
        static const ModuleInfo i = {"Shaper", S, 1, 1, SHP_N, false, {"in"}, {"out"},
            {{"mode", SHPM_TANH, 0, SHPM_N - 1}, {"drive", 256, 0, 4 * 256}, {"mix", kUnity, 0, kUnity}, {"bits", 8, 1, 15}, {"drive_mod", 2 * 256, 0, 6 * 256}}};
        return i;
    }
    void set_param(int idx, int32_t v) override {
        switch (idx) {
            case SHP_MODE: mode_ = v; break;
            case SHP_DRIVE: drive_ = v; break;
            case SHP_MIX: mix_ = static_cast<q15>(v); break;
            case SHP_BITS: bits_ = clamp_i32(v, 1, 15); break;
            case SHP_DRIVE_MOD: dmod_ = v; break;
        }
    }
    bool init(Memory &) override { dc_.set_corner(15.0f); return true; }
    void reset() override { dc_.reset(); lp_ = 0; hold_ = 0; cnt_ = 0; }
    SC_HOT void process(const ProcessCtx &ctx, const Ports &p) override {
        const q15 *md = p.mod[SHP_DRIVE];
        int32_t gain = gain_q8(drive_);
        for (int i = 0; i < ctx.frames; i++) {
            if (md) gain = gain_q8(drive_ + scaled(md[i], dmod_));
            const q15 x = p.in[0][i];
            const int32_t v = x * gain;                              // Q23 (q15 * Q8)
            q15 y;
            switch (mode_) {
                case SHPM_TANH: y = tanh_q23(v); break;
                case SHPM_CLIP: y = sat16(v >> 8); break;
                case SHPM_FOLD: y = sine(static_cast<uint32_t>(v) << 7); break;
                case SHPM_TUBE: y = dc_.process(sub15(tanh_q23(v + 2516582), 9546)); break;            // bias 0.3: tanh(0.3) = 0.2913 = 9546 (+ trim)
                case SHPM_TAPE: { const q15 t = tanh_q23(v); lp_ += ((static_cast<int32_t>(t) - lp_) * 16000) >> 15; y = sat16(lp_); break; }
                case SHPM_DIODE: y = dc_.process(v >= 0 ? tanh_q23(v) : static_cast<q15>((tanh_q23((v * 7) >> 4) * 19660) >> 15)); break;
                case SHPM_CHEB: {
                    const int32_t c = sat16(v >> 8);                                                    // clamp to +-1
                    const int32_t c2 = (c * c) >> 15, c3 = (c2 * c) >> 15, c5 = (((c3 * c) >> 15) * c) >> 15;
                    y = sat16(((4 * c3 - 3 * c) + (16 * c5 - 20 * c3 + 5 * c)) >> 1);                   // (T3 + T5) / 2
                    break;
                }
                case SHPM_RECT: { const int32_t c = sat16(v >> 8); y = dc_.process(sat16(c < 0 ? -2 * c : 2 * c)); break; }
                default: {                                                                              // decimate + crush
                    const int32_t every = gain >> 8 < 1 ? 1 : (gain >> 8 > 16 ? 16 : gain >> 8);
                    if (++cnt_ >= every) { cnt_ = 0; hold_ = x; }
                    const int sh = 16 - bits_;
                    y = sat16(((hold_ + (1 << (sh - 1))) >> sh) << sh);
                    break;
                }
            }
            p.out[0][i] = mix_ == kUnity ? y : static_cast<q15>(x + (((static_cast<int32_t>(y) - x) * mix_) >> 15));
        }
    }
private:
    static q15 tanh_q23(int32_t v) {
        const int32_t c = clamp_i32(v, -(4 << 23) + 1, (4 << 23) - 1);
        const uint32_t u = static_cast<uint32_t>(c + (4 << 23));
        return lerp15(kTanhTab[u >> 16], kTanhTab[(u >> 16) + 1], u & 0xFFFFu);
    }
    static int32_t gain_q8(int32_t drive) { return static_cast<int32_t>(exp2_scale(256u, clamp_i32(drive, 0, 6 * 256) * 256)); }
    int32_t mode_ = SHPM_TANH, drive_ = 256, bits_ = 8, dmod_ = 2 * 256, lp_ = 0, cnt_ = 0;
    q15 mix_ = kUnity, hold_ = 0;
    DcBlocker dc_;
};

/* ------------------------------------------------------------------ small utilities */

template <Scope S>
class Const : public Module {
public:
    const ModuleInfo &info() const override {
        static const ModuleInfo i = {"Const", S, 0, 1, 1, false, {}, {"out"}, {{"value", 0, -32768, 32767}}};
        return i;
    }
    void set_param(int, int32_t v) override { v_ = static_cast<q15>(v); }
    void process(const ProcessCtx &ctx, const Ports &p) override { for (int i = 0; i < ctx.frames; i++) p.out[0][i] = v_; }
private:
    q15 v_ = 0;
};

template <Scope S>
class Vca : public Module {
public:
    const ModuleInfo &info() const override {
        static const ModuleInfo i = {"Vca", S, 1, 1, 1, false, {"in"}, {"out"}, {{"level", kUnity, -32768, kUnity}}};
        return i;
    }
    void set_param(int, int32_t v) override { g_ = static_cast<q15>(v); }
    SC_HOT void process(const ProcessCtx &ctx, const Ports &p) override {
        const q15 *m = p.mod[VCA_LEVEL];
        if (!m) { block_gain(p.out[0], p.in[0], g_, ctx.frames); return; }
        for (int i = 0; i < ctx.frames; i++) p.out[0][i] = mul15(p.in[0][i], sat16(g_ + m[i]));
    }
private:
    q15 g_ = kUnity;
};

template <Scope S>
class Mult : public Module {
public:
    const ModuleInfo &info() const override {
        static const ModuleInfo i = {"Mult", S, 2, 1, 0, false, {"a", "b"}, {"out"}, {}};
        return i;
    }
    void process(const ProcessCtx &ctx, const Ports &p) override { block_mul(p.out[0], p.in[0], p.in[1], ctx.frames); }
};

template <Scope S>
class Mix4 : public Module {
public:
    const ModuleInfo &info() const override {
        static const ModuleInfo i = {"Mix4", S, 4, 1, 4, false, {"1", "2", "3", "4"}, {"out"},
            {{"lvl1", 8192, -32768, kUnity}, {"lvl2", 8192, -32768, kUnity}, {"lvl3", 8192, -32768, kUnity}, {"lvl4", 8192, -32768, kUnity}}};
        return i;
    }
    void set_param(int idx, int32_t v) override { if (idx >= 0 && idx < 4) g_[idx] = static_cast<q15>(v); }
    SC_HOT void process(const ProcessCtx &ctx, const Ports &p) override {
        const q15 *mod[4] = {p.mod[0], p.mod[1], p.mod[2], p.mod[3]};
        if (!(mod[0] || mod[1] || mod[2] || mod[3])) {                  // the usual case: constant gains, and unused inputs (gain 0) cost nothing
            int act[4], na = 0;
            for (int k = 0; k < 4; k++) if (g_[k] != 0) act[na++] = k;
            if (na == 0) { for (int i = 0; i < ctx.frames; i++) p.out[0][i] = 0; return; }
            if (na == 2) {
                const q15 *a = p.in[act[0]], *b = p.in[act[1]];
                const int32_t ga = g_[act[0]], gb = g_[act[1]];
                for (int i = 0; i < ctx.frames; i++) p.out[0][i] = sat16(((a[i] * ga + (1 << 14)) >> 15) + ((b[i] * gb + (1 << 14)) >> 15));
                return;
            }
            for (int i = 0; i < ctx.frames; i++) {
                int32_t acc = 0;
                for (int j = 0; j < na; j++) acc += (p.in[act[j]][i] * g_[act[j]] + (1 << 14)) >> 15;
                p.out[0][i] = sat16(acc);
            }
            return;
        }
        for (int i = 0; i < ctx.frames; i++) {
            int32_t acc = 0;
            for (int k = 0; k < 4; k++) acc += (p.in[k][i] * (mod[k] ? sat16(g_[k] + mod[k][i]) : g_[k]) + (1 << 14)) >> 15;
            p.out[0][i] = sat16(acc);
        }
    }
private:
    q15 g_[4] = {8192, 8192, 8192, 8192};
};

template <typename T>
ModuleType type_of() { static T probe; return {&probe.info(), &create_module<T>}; }

}  // namespace

void register_synth_modules(Registry &r) {
    r.add(T_OSC, type_of<Osc<Scope::Voice>>());
    r.add(T_OSC_G, type_of<Osc<Scope::Global>>());
    r.add(T_ENV, type_of<Env<Scope::Voice>>());
    r.add(T_ENV_G, type_of<Env<Scope::Global>>());
    r.add(T_EG, type_of<Eg>());
    r.add(T_LFO_V, type_of<Lfo<Scope::Voice>>());
    r.add(T_LFO_G, type_of<Lfo<Scope::Global>>());
    r.add(T_FILTER_V, type_of<Filter<Scope::Voice>>());
    r.add(T_FILTER_G, type_of<Filter<Scope::Global>>());
    r.add(T_VCA_V, type_of<Vca<Scope::Voice>>());
    r.add(T_VCA_G, type_of<Vca<Scope::Global>>());
    r.add(T_MIX4_V, type_of<Mix4<Scope::Voice>>());
    r.add(T_MIX4_G, type_of<Mix4<Scope::Global>>());
    r.add(T_MULT_V, type_of<Mult<Scope::Voice>>());
    r.add(T_MULT_G, type_of<Mult<Scope::Global>>());
    r.add(T_SHAPER_V, type_of<Shaper<Scope::Voice>>());
    r.add(T_SHAPER_G, type_of<Shaper<Scope::Global>>());
    r.add(T_CONST_V, type_of<Const<Scope::Voice>>());
    r.add(T_CONST_G, type_of<Const<Scope::Global>>());
}

}  // namespace sc
