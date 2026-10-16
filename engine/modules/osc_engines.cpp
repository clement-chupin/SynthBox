#include "engine/modules/osc_engines.h"
#include <cmath>
#if defined(ENGINE_PROFILE) && defined(ARDUINO_ARCH_ESP32)
#include <esp_cpu.h>
#endif
#include <cstring>
#include "engine/dsp/block.h"
#include "engine/dsp/interp.h"
#include "engine/dsp/osc.h"
#include "engine/dsp/phase.h"
#include "engine/dsp/svf.h"
#include "engine/dsp/util.h"
#include "engine/dsp/wavetables.h"
#include "engine/modules/builtin.h"

namespace sc {
namespace {

constexpr int32_t kSemi = 256;
constexpr int kKsLen = 2048;                         // string buffer: the lowest pitch is sample rate / 2046 (23 Hz at 48 kHz)
constexpr int kModes = 8, kHarm = 12, kSaws = 5;

inline int32_t scaled(q15 m, int32_t range) { return static_cast<int32_t>((static_cast<int64_t>(m) * range) >> 15); }
inline q15 clamp_unit(int32_t v) { return static_cast<q15>(v < 0 ? 0 : (v > 32767 ? 32767 : v)); }

// Constant tables of the engines, built once and shared by every instance (there are 32 instances in a 4-oscillator, 8-voice patch: per-instance copies
// were 1.3 KB each of the scarce internal RAM).
struct Tabs {
    int32_t g_tab[33] = {}, harm[kModes] = {}, bell[kModes] = {}, amp[kModes] = {}, dtab[17][kModes] = {}, lg[kHarm + 1] = {};
    uint32_t vinc[5][3] = {};
    Tabs() {
        for (int i = 0; i <= 32; i++) g_tab[i] = static_cast<int32_t>(std::lround((1.0 - 0.1 * std::pow(2.0, -10.0 * i / 32.0)) * 32767.0));
        static const double bell_ratio[kModes] = {1, 2.76, 5.40, 8.93, 13.34, 18.64, 25.0, 32.0};
        for (int k = 0; k < kModes; k++) {
            harm[k] = static_cast<int32_t>((k + 1) * 65536);
            bell[k] = static_cast<int32_t>(std::lround(bell_ratio[k] * 65536.0));
            amp[k] = static_cast<int32_t>(std::lround(32767.0 * std::pow(k + 1.0, -0.8) * 0.2));
        }
        for (int mi = 0; mi <= 16; mi++) {
            const double t0 = 40.0 * std::pow(2.0, 6.64 * mi / 16.0);                  // ms: 40 ms .. 4 s
            for (int k = 0; k < kModes; k++) {
                const double tk = t0 / (1.0 + 0.7 * k);
                dtab[mi][k] = static_cast<int32_t>(std::lround(std::exp(-1.0 / (tk * 0.001 * kSampleRate)) * 2147483647.0));
            }
        }
        static const double vow[5][3] = {{800, 1150, 2900}, {400, 1600, 2700}, {270, 2300, 3000}, {450, 800, 2830}, {325, 700, 2700}};
        for (int v = 0; v < 5; v++) for (int f = 0; f < 3; f++) vinc[v][f] = hz_to_inc(static_cast<float>(vow[v][f]));
        for (int k = 1; k <= kHarm; k++) lg[k] = static_cast<int32_t>(std::lround(std::log2(static_cast<double>(k)) * 65536.0));
    }
};
const Tabs &tabs() { static const Tabs t; return t; }

class OscEngines : public Module {
public:
    const ModuleInfo &info() const override {
        static const ModuleInfo i = {"OscEngines", Scope::Voice, 2, 1, OSCX_N, false, {"pitch", "gate"}, {"out"},
            {{"engine", OSCX_KARP, 0, OSCX_ENGINES - 1}, {"pitch", 60 * kSemi, 0, 127 * kSemi}, {"timbre", 16384, 0, kUnity},
             {"morph", 16384, 0, kUnity}, {"level", kUnity, 0, kUnity}, {"pitch_mod", 12 * kSemi, 0, 96 * kSemi}, {"quality", 0, 0, 2}}};
        return i;
    }

    bool init(Memory &m) override {
        a4_ = inc_a4();
        heap_ = m.bulk;                                   // the string buffer is read and written sequentially: PSRAM is fine, internal RAM is not to be spent on 32 of them
        ks_ = static_cast<int16_t *>(heap_->alloc(sizeof(int16_t) * kKsLen));
        if (!ks_) return false;
        std::memset(ks_, 0, sizeof(int16_t) * kKsLen);
        t_ = &tabs();
        one_hz_ = hz_to_inc(1.0f);
        return true;
    }
    ~OscEngines() override { if (heap_) heap_->free(ks_); }

    void reset() override {
        std::memset(ph_, 0, sizeof ph_);
        std::memset(mx_, 0, sizeof mx_);
        std::memset(my_, 0, sizeof my_);
        // (the Karplus string buffer is not cleared here: trigger() clears it when the string is struck, and no other engine reads it; clearing
        // 4 KB of PSRAM per oscillator at every voice start cost about 1.5 ms of the audio block on the board)
        lp_ = 0; w_ = 0; gate_ = false; fm_ph_ = 0;
        for (auto &s : sv_) s = SvfStateF{};
        ds_ = SvfStateF{};
    }
    void set_param(int idx, int32_t v) override {
        switch (idx) {
            case OSCX_ENGINE: engine_ = v; break;
            case OSCX_PITCH: pitch_ = v; break;
            case OSCX_TIMBRE: timbre_ = static_cast<q15>(v); break;
            case OSCX_MORPH: morph_ = static_cast<q15>(v); break;
            case OSCX_LEVEL: level_ = static_cast<q15>(v); break;
            case OSCX_PITCH_MOD: pmod_ = v; break;
            case OSCX_QUAL: qual_ = v; prep_ok_ = false; break;
        }
    }

    SC_HOT void process(const ProcessCtx &ctx, const Ports &p) override {
#if defined(ENGINE_PROFILE) && defined(ARDUINO_ARCH_ESP32)
        const uint32_t prof_t0 = esp_cpu_get_cycle_count();
#endif
        const int n = ctx.frames;
        const q15 *cv = p.in[0], *gate = p.in[1], *mp = p.mod[OSCX_PITCH];
        const q15 *mt = p.mod[OSCX_TIMBRE], *mm = p.mod[OSCX_MORPH];
        const q15 timbre = clamp_unit(timbre_ + (mt ? mt[0] : 0)), morph = clamp_unit(morph_ + (mm ? mm[0] : 0));
        const bool varying = mp || cv[0] != cv[n - 1];
        uint32_t inc = pitch_to_inc(pitch_ + scaled(cv[0], kPitchCvSpan) + (mp ? scaled(mp[0], pmod_) : 0), a4_);
        // The per-block setup is a pure function of these four values: with a held note and no modulation it runs once, not every block
        if (!(prep_ok_ && prep_engine_ == engine_ && prep_inc_ == inc && prep_timbre_ == timbre && prep_morph_ == morph)) {
            prepare(inc, timbre, morph);
            prep_ok_ = true; prep_engine_ = engine_; prep_inc_ = inc; prep_timbre_ = timbre; prep_morph_ = morph;
        }
        // Fast path: a steady pitch and a gate that does not change inside the block (a held note, the usual case) need no per-sample gate test,
        // trigger check or engine dispatch: one tight loop per engine. The loop around the engines was about 50 cycles per sample.
        bool edge = false;
        if (!varying) for (int i = 0; i < n; i++) if ((gate[i] > 16384) != gate_) { edge = true; break; }
        if (!varying && !edge) {
            q15 *out = p.out[0];
            const q15 lv = level_;
            switch (engine_) {
                case OSCX_KARP: for (int i = 0; i < n; i++) out[i] = mul15(sat16(karp()), lv); break;
                case OSCX_MODAL: for (int i = 0; i < n; i++) out[i] = mul15(sat16(modal(inc, false, i)), lv); break;
                case OSCX_FM2: for (int i = 0; i < n; i++) out[i] = mul15(sat16(fm2(inc, timbre)), lv); break;
                case OSCX_FOLD: for (int i = 0; i < n; i++) out[i] = mul15(sat16(fold(inc, timbre, morph)), lv); break;
                case OSCX_SSAW: for (int i = 0; i < n; i++) out[i] = mul15(sat16(ssaw(inc, timbre, morph)), lv); break;
                case OSCX_VOWEL: for (int i = 0; i < n; i++) out[i] = mul15(sat16(vowel(inc)), lv); break;
                case OSCX_ADD: for (int i = 0; i < n; i++) out[i] = mul15(sat16(additive(inc)), lv); break;
                case OSCX_STR: str_block(out, n, lv); break;
                default: for (int i = 0; i < n; i++) out[i] = mul15(sat16(dust()), lv); break;
            }
#if defined(ENGINE_PROFILE) && defined(ARDUINO_ARCH_ESP32)
            g_osc_prof[engine_ < OSCX_ENGINES ? engine_ : 0] += esp_cpu_get_cycle_count() - prof_t0;
#endif
            return;
        }
        for (int i = 0; i < n; i++) {
            if (varying) inc = pitch_to_inc(pitch_ + scaled(cv[i], kPitchCvSpan) + (mp ? scaled(mp[i], pmod_) : 0), a4_);
            const bool g = gate[i] > 16384;
            if (g && !gate_) trigger(inc, timbre);
            gate_ = g;
            int32_t y;
            switch (engine_) {
                case OSCX_KARP: y = karp(); break;
                case OSCX_MODAL: y = modal(inc, varying, i); break;
                case OSCX_FM2: y = fm2(inc, timbre); break;
                case OSCX_FOLD: y = fold(inc, timbre, morph); break;
                case OSCX_SSAW: y = ssaw(inc, timbre, morph); break;
                case OSCX_VOWEL: y = vowel(inc); break;
                case OSCX_ADD: y = additive(inc); break;
                case OSCX_STR: y = str(); break;
                default: y = dust(); break;
            }
            p.out[0][i] = mul15(sat16(y), level_);
        }
#if defined(ENGINE_PROFILE) && defined(ARDUINO_ARCH_ESP32)
        g_osc_prof[engine_ < OSCX_ENGINES ? engine_ : 0] += esp_cpu_get_cycle_count() - prof_t0;
#endif
    }

private:
    /* ---- per block: derived values ---- */
    void prepare(uint32_t inc, q15 timbre, q15 morph) {
        switch (engine_) {
            case OSCX_KARP: {
                const uint64_t d = inc ? (1ull << 48) / inc : (static_cast<uint64_t>(kKsLen - 2) << 16);
                const int64_t dq = static_cast<int64_t>(d) - 32768;                      // the loop filter delays by half a sample
                ks_d_ = static_cast<int32_t>(dq < (2 << 16) ? (2 << 16) : (dq > (static_cast<int64_t>(kKsLen - 2) << 16) ? (static_cast<int64_t>(kKsLen - 2) << 16) : dq));
                ks_a_ = 3000 + ((timbre * 29000) >> 15);
                const int gi = morph >> 10;
                ks_g_ = t_->g_tab[gi] + (((t_->g_tab[gi + 1 > 32 ? 32 : gi + 1] - t_->g_tab[gi]) * (morph & 1023)) >> 10);
                break;
            }
            case OSCX_MODAL: {
                const int mi = morph >> 11;                                              // 0..15
                const int fr = morph & 2047;
                for (int k = 0; k < kModes; k++) {
                    dec_[k] = t_->dtab[mi][k] + static_cast<int32_t>((static_cast<int64_t>(t_->dtab[mi + 1][k] - t_->dtab[mi][k]) * fr) >> 11);
                    ratio_[k] = t_->harm[k] + static_cast<int32_t>((static_cast<int64_t>(t_->bell[k] - t_->harm[k]) * timbre) >> 15);
                }
                modal_coefs(inc);
                break;
            }
            case OSCX_FM2: {
                static const int32_t ratios[16] = {32768, 49152, 65536, 98304, 131072, 163840, 196608, 229376, 262144, 327680, 393216, 458752, 524288, 589824, 655360, 786432};
                fm_ratio_ = ratios[morph >> 11];
                break;
            }
            case OSCX_SSAW: {
                static const int32_t off[kSaws] = {0, -737, 737, -1475, 1475};                  // relative increment at spread 1 (Q15): +-4.5 % at the edge, evenly spaced
                for (int k = 0; k < kSaws; k++) {
                    const int32_t rel = static_cast<int32_t>((static_cast<int64_t>(off[k]) * timbre) >> 15);
                    saw_inc_[k] = static_cast<uint32_t>(static_cast<int64_t>(inc) + ((static_cast<int64_t>(inc) * rel) >> 15));
                }
                break;
            }
            case OSCX_VOWEL: {
                const int32_t pos = timbre * 4;                                          // 0 .. 4 vowels apart, Q15
                const int vi = pos >> 15;                                                // 0..3 (timbre < 32768)
                const int32_t fr = pos & 32767;
                static const int32_t wgt[3] = {32767, 22000, 14000};
                for (int f = 0; f < 3; f++) {
                    const uint32_t a = t_->vinc[vi][f], b = t_->vinc[vi + 1][f];
                    const uint32_t fi = a + static_cast<uint32_t>((static_cast<int64_t>(static_cast<int64_t>(b) - a) * fr) >> 15);
                    const int32_t k = (1 << 29) / 3 - static_cast<int32_t>((static_cast<int64_t>(morph) * ((1 << 29) / 3 - (1 << 29) / 30)) >> 15);   // Q 3 .. 30
                    vc_[f] = svf_to_float(svf_coef(svf_g(fi), k));
                    vw_[f] = wgt[f];
                    vwf_[f] = static_cast<float>(wgt[f]) * 3.0f;                              // weight x the make-up gain of 3
                }
                break;
            }
            case OSCX_ADD: {
                const int32_t slope = 2 * 65536 - static_cast<int32_t>((static_cast<int64_t>(timbre) * 115000) >> 15);       // 2.0 .. 0.25 octaves of level per octave of pitch
                int32_t sum = 0;
                for (int k = 1; k <= kHarm; k++) {
                    const bool even = (k & 1) == 0;
                    int32_t a = static_cast<int32_t>(exp2_scale(32767u, -static_cast<int32_t>((static_cast<int64_t>(slope) * t_->lg[k]) >> 16)));
                    if (even) a = static_cast<int32_t>((static_cast<int64_t>(a) * (32767 - morph)) >> 15);
                    if (static_cast<uint64_t>(inc) * static_cast<uint64_t>(k) >= 2147483648ull) a = 0;
                    harm_amp_[k - 1] = a; sum += a;
                }
                const int32_t norm = sum > 0 ? static_cast<int32_t>((32767ll * 24000) / (sum < 1 ? 1 : sum)) : 0;          // total about 0.73 of full scale
                add_top_ = 0;
                for (int k = 0; k < kHarm; k++) {
                    harm_amp_[k] = static_cast<int32_t>((static_cast<int64_t>(harm_amp_[k]) * norm) >> 15);
                    if (harm_amp_[k]) add_top_ = k + 1;
                }
                break;
            }
            case OSCX_STR: {
                // detune: up to 50 cents between the pair, half each side (Q16 octaves: cents / 1200 / 2)
                const int32_t x = static_cast<int32_t>((static_cast<int64_t>(timbre) * 50 * 65536) / (1200LL * 2 * 32767));
                str_inc_[0] = exp2_scale(inc, -x);
                str_inc_[1] = exp2_scale(inc, x);
                str_wave_ = morph < 10923 ? WT_SAW : (morph < 21846 ? WT_PULSE : WT_TRI);
                str_mip_ = qual_ != 2;
                str_tab_[0] = str_mip_ ? wt_table(str_wave_, str_inc_[0]) : nullptr;
                str_tab_[1] = str_mip_ ? wt_table(str_wave_, str_inc_[1]) : nullptr;
                break;
            }
            case OSCX_DUST: {
                dust_thr_ = exp2_scale(one_hz_ * 2u, static_cast<int32_t>((static_cast<int64_t>(timbre) * 11 * 65536) >> 15));          // impulses per sample (as a phase increment)
                const int32_t k = (1 << 29) / 2 - static_cast<int32_t>((static_cast<int64_t>(morph) * ((1 << 29) / 2 - (1 << 29) / 60)) >> 15);   // Q 2 .. 60
                ds_c_ = svf_to_float(svf_coef(svf_g(inc), k));
                // A unity-peak band-pass rings with an amplitude ~ 1/Q after an impulse, so the loudness fell by ~sqrt(Q) towards Morph = 1 (Q = 60).
                // Make-up gain of sqrt(Q / 2) (1.0 at the lowest Q of 2) keeps the level about constant. [control rate: float is fine]
                ds_gain_ = 32768.0f * 14.0f * std::sqrt(static_cast<float>(1 << 28) / static_cast<float>(k));
                break;
            }
            default: break;
        }
    }

    void trigger(uint32_t, q15 timbre) {
        if (engine_ == OSCX_KARP) {
            const int d = ks_d_ >> 16;
            int32_t lp = 0;
            lp_ = 0;
            const int32_t a = 3000 + ((timbre * 29000) >> 15);
            std::memset(ks_, 0, sizeof(int16_t) * kKsLen);
            for (int i = 0; i < d; i++) {
                lp += ((static_cast<int32_t>(noise_.next()) - lp) * a) >> 15;
                ks_[i] = static_cast<int16_t>((lp * 23000) >> 15);
            }
            w_ = d & (kKsLen - 1);
        } else if (engine_ == OSCX_STR) {
            ph_[0] = noise_.next_u32();                                          // random start phases: the pair never starts in phase
            ph_[1] = noise_.next_u32();
        } else if (engine_ == OSCX_MODAL) {
            for (int k = 0; k < kModes; k++) { mx_[k] = 0; my_[k] = t_->amp[k] << 15; }          // start in sine phase, at full amplitude
        }
    }

    /* ---- engines (one sample each) ---- */
    int32_t karp() {
        const uint32_t pos = (static_cast<uint32_t>(w_) << 16) - static_cast<uint32_t>(ks_d_);
        const int i0 = (pos >> 16) & (kKsLen - 1), i1 = (i0 + 1) & (kKsLen - 1);
        const int32_t fr = static_cast<int32_t>(pos & 0xFFFF);
        const int32_t out = ks_[i0] + (((ks_[i1] - ks_[i0]) * fr) >> 16);
        lp_ += ((out - lp_) * ks_a_) >> 15;
        ks_[w_] = static_cast<int16_t>(sat16((lp_ * ks_g_) >> 15));
        w_ = (w_ + 1) & (kKsLen - 1);
        return out;
    }

    // Each mode is a decaying phasor: (x, y) is rotated by the mode's frequency and scaled by its decay every sample, so it rings as r^n sin(n theta) with
    // four multiplies instead of a sine lookup. The rotation (cos, sin of theta, from the table) is first normalised to exactly unit length (one Newton
    // step of 1/sqrt), because a length error of 1e-5 per sample would otherwise change the decay time of a slow mode by a large factor.
    // Computed once per block (the prepare cache) and, if the pitch moves inside a block, every 4 samples. A mode above Nyquist gets zero coefficients.
    void modal_coefs(uint32_t inc) {
        for (int k = 0; k < kModes; k++) {
            const uint64_t ik = (static_cast<uint64_t>(inc) * static_cast<uint64_t>(ratio_[k])) >> 16;
            if (ik > 0x7FFFFFFFull) { mrc_[k] = mrs_[k] = 0; continue; }
            const uint32_t th = static_cast<uint32_t>(ik);
            const int32_t c = sine(th + 0x40000000u) * 32768, sn = sine(th) * 32768;        // cos, sin: Q30
            const int32_t n2 = mulh(c, c) + mulh(sn, sn);                                   // |.|^2, Q28 (about 1.0 = 2^28)
            const int32_t corr = (3 << 27) - (n2 >> 1);                                     // 1.5 - n2 / 2 = 1 / sqrt(n2) to second order
            const int32_t cn = shl_sat(mulh(c, corr << 2), 2), sg = shl_sat(mulh(sn, corr << 2), 2);   // unit-length cos, sin: Q30 (corr is about 2^28: << 2 stays in 32 bits)
            mrc_[k] = shl_sat(mulh(dec_[k], cn), 1);                                        // r cos theta, Q30 (dec_ = r in Q31)
            mrs_[k] = shl_sat(mulh(dec_[k], sg), 1);
        }
    }

    int32_t modal(uint32_t inc, bool varying, int i) {
        if (varying && (i & 3) == 0) modal_coefs(inc);
        int32_t sum = 0;                                                                    // 8 modes of at most 8192 each: fits 32 bits
        for (int k = 0; k < kModes; k++) {
            const int32_t x = mx_[k], y = my_[k];                                           // Q28: the amplitude is the length of (x, y)
            mx_[k] = (mulh(mrc_[k], x) - mulh(mrs_[k], y)) * 4;
            my_[k] = (mulh(mrs_[k], x) + mulh(mrc_[k], y)) * 4;
            sum += my_[k] >> 15;
        }
        return sum;
    }

    int32_t fm2(uint32_t inc, q15 timbre) {
        fm_ph_ += static_cast<uint32_t>((static_cast<uint64_t>(inc) * static_cast<uint64_t>(fm_ratio_)) >> 16);
        ph_[0] += inc;
        const int64_t prod = static_cast<int64_t>(sine(fm_ph_)) * timbre;                      // Q30
        const uint32_t offset = static_cast<uint32_t>(static_cast<int32_t>((prod * 5214) >> 10));      // up to 8 rad in phase units
        return (static_cast<int32_t>(sine(ph_[0] + offset)) * 23000) >> 15;
    }

    int32_t fold(uint32_t inc, q15 timbre, q15 morph) {
        ph_[0] += inc;
        const int32_t sn = sine(ph_[0]), sw = static_cast<int32_t>(ph_[0] >> 16) - 32768;
        const int32_t src = sn + (static_cast<int32_t>((static_cast<int64_t>(sw - sn) * morph) >> 15));
        const int32_t gq8 = 256 + ((timbre * 2048) >> 15);                                    // 1x .. 9x
        const int32_t v = src * gq8;                                                          // Q23
        return (static_cast<int32_t>(sine(static_cast<uint32_t>(v) << 7)) * 23000) >> 15;
    }

    // STR: the two table oscillators summed (x 0.5); one sample, or a whole steady block with the wave / quality resolved once
    int32_t str() {
        const int32_t y = (wt_osc_any(str_wave_, str_mip_, ph_[0], 0x80000000u, str_tab_[0]) + wt_osc_any(str_wave_, str_mip_, ph_[1], 0x80000000u, str_tab_[1])) >> 1;
        ph_[0] += str_inc_[0];
        ph_[1] += str_inc_[1];
        return y;
    }
    template <int W, bool Mip>
    void str_loop(q15 *out, int n, q15 lv) {
        uint32_t p0 = ph_[0], p1 = ph_[1];
        const uint32_t i0 = str_inc_[0], i1 = str_inc_[1];
        const int16_t *t0 = str_tab_[0], *t1 = str_tab_[1];
        for (int i = 0; i < n; i++) {
            const int32_t y = (wt_osc<W, Mip>(p0, 0x80000000u, t0) + wt_osc<W, Mip>(p1, 0x80000000u, t1)) >> 1;
            out[i] = mul15(static_cast<q15>(y), lv);
            p0 += i0;
            p1 += i1;
        }
        ph_[0] = p0;
        ph_[1] = p1;
    }
    void str_block(q15 *out, int n, q15 lv) {
        switch (str_wave_ * 2 + (str_mip_ ? 1 : 0)) {
            case WT_SAW * 2:       str_loop<WT_SAW, false>(out, n, lv); break;
            case WT_SAW * 2 + 1:   str_loop<WT_SAW, true>(out, n, lv); break;
            case WT_PULSE * 2:     str_loop<WT_PULSE, false>(out, n, lv); break;
            case WT_PULSE * 2 + 1: str_loop<WT_PULSE, true>(out, n, lv); break;
            case WT_TRI * 2:       str_loop<WT_TRI, false>(out, n, lv); break;
            default:               str_loop<WT_TRI, true>(out, n, lv); break;
        }
    }

    int32_t ssaw(uint32_t, q15, q15 morph) {
        // The band-limiting correction only matters within one step of the saw's wrap point (about 1 sample in 100 per saw), so the plain ramp is
        // taken directly and osc_saw() only when a saw is at its edge: same output, without the call for the other samples.
        int32_t side = 0;
        for (int k = 1; k < kSaws; k++) {
            const uint32_t t = (ph_[k] += saw_inc_[k]), dt = saw_inc_[k];
            side += (t < dt || t > 0u - dt) ? osc_saw(t, dt) : static_cast<int32_t>(t >> 16) - 32768;
        }
        ph_[0] += saw_inc_[0];
        const uint32_t t0 = ph_[0], dt0 = saw_inc_[0];
        const int32_t centre = (t0 < dt0 || t0 > 0u - dt0) ? osc_saw(t0, dt0) : static_cast<int32_t>(t0 >> 16) - 32768;
        const int32_t wc = 32767 - (morph >> 1), ws = (5000 + ((morph * 6000) >> 15)) * 3 / 2;      // four side saws carry what six did: x 6/4
        return (((centre * wc) >> 15) * 9 / 20) + (((side * ws) >> 15) * 9 / 20);
    }

    int32_t vowel(uint32_t inc) {
        ph_[0] += inc;
        const float x = static_cast<float>(osc_saw(ph_[0], inc)) * (0.5f / 32768.0f);
        float sum = 0.0f;
        for (int f = 0; f < 3; f++) {
            float lp, bp, hp;
            svf_tick_f(vc_[f], sv_[f], x, lp, bp, hp);                                       // (single-precision float: the FPU does this in ~25 cycles)
            sum += vc_[f].k * bp * vwf_[f];                                                  // band-pass normalised to unity peak, weighted (Q15) x 3 make-up gain
        }
        return static_cast<int32_t>(sum);                                                     // float 1.0 x weight Q15 = q15
    }

    // The harmonics sin(k phi) come from two table reads and the Chebyshev recurrence s(k) = 2 cos(phi) s(k-1) - s(k-2), instead of one table read each.
    // States are Q29, cos(phi) Q30; the amplitudes sum to at most 24000, so the weighted sum fits 32 bits.
    int32_t additive(uint32_t inc) {
        ph_[0] += inc;
        const uint32_t ph = ph_[0];
        if (add_top_ == 0) return 0;
        const int32_t c = sine(ph + 0x40000000u) * 32768;                                   // cos(phi), Q30
        int32_t s_prev = 0, s = sine(ph) * 16384;                                           // sin(0 phi), sin(phi): Q29
        int32_t sum = harm_amp_[0] * (s >> 14);
        for (int k = 1; k < add_top_; k++) {
            const int32_t next = mulh(c, s) * 8 - s_prev;                                  // 2 cos(phi) s(k) - s(k-1)
            s_prev = s;
            s = next;
            sum += harm_amp_[k] * (s >> 14);
        }
        return sum >> 15;
    }

    int32_t dust() {
        const uint32_t r = noise_.next_u32();
        int32_t imp = 0;
        if (r < dust_thr_) imp = (static_cast<int32_t>(noise_.next()) >> 1) + (noise_.next() < 0 ? -8192 : 8192);
        float lp, bp, hp;
        svf_tick_f(ds_c_, ds_, static_cast<float>(imp) * (1.0f / 32768.0f), lp, bp, hp);
        return sat16(static_cast<int32_t>(ds_c_.k * bp * ds_gain_));
    }

    Heap *heap_ = nullptr;
    int16_t *ks_ = nullptr;
    uint32_t a4_ = 0, one_hz_ = 0;
    int32_t engine_ = OSCX_KARP, pitch_ = 60 * kSemi, pmod_ = 12 * kSemi;
    q15 timbre_ = 16384, morph_ = 16384, level_ = kUnity;
    bool gate_ = false;
    Noise noise_{0xA11CE5u};
    // tables (init)
    const Tabs *t_ = nullptr;
    // per block
    int32_t ks_d_ = 100 << 16, ks_a_ = 16000, ks_g_ = 32000;
    int32_t dec_[kModes] = {}, ratio_[kModes] = {}, fm_ratio_ = 65536, harm_amp_[kHarm] = {}, vw_[3] = {};
    uint32_t saw_inc_[kSaws] = {}, dust_thr_ = 0;
    int32_t mrc_[kModes] = {}, mrs_[kModes] = {};            // modal: r cos theta, r sin theta per mode (Q30)
    bool prep_ok_ = false;                                    // the last prepare() was for these values
    int32_t prep_engine_ = 0;
    uint32_t prep_inc_ = 0;
    q15 prep_timbre_ = 0, prep_morph_ = 0;
    int add_top_ = 0;                                         // highest harmonic with a non-zero amplitude
    SvfCoefF vc_[3], ds_c_;
    float vwf_[3] = {};
    float ds_gain_ = 32768.0f * 14.0f;
    // state
    uint32_t ph_[kSaws] = {}, fm_ph_ = 0;
    int32_t qual_ = 0, str_wave_ = WT_SAW;                    // STR: quality (0 Blep = Mip here, 1 Mip, 2 Naive), wave from Morph
    bool str_mip_ = true;
    uint32_t str_inc_[2] = {};
    const int16_t *str_tab_[2] = {nullptr, nullptr};
    int32_t mx_[kModes] = {}, my_[kModes] = {}, lp_ = 0;      // modal phasors
    int w_ = 0;
    SvfStateF sv_[3], ds_;
};

template <typename T>
ModuleType type_of() { static T probe; return {&probe.info(), &create_module<T>}; }

}  // namespace

#if defined(ENGINE_PROFILE) && defined(ARDUINO_ARCH_ESP32)
uint32_t g_osc_prof[OSCX_ENGINES];
#endif

void register_osc_engines(Registry &r) { r.add(T_OSCX, type_of<OscEngines>()); }

}  // namespace sc
