#include "engine/modules/dx7_voice.h"
#include <cstring>
#include "engine/dsp/phase.h"
#include "engine/modules/builtin.h"

namespace sc {
namespace {

// MSFA / AMY algorithm wiring, index 0..5 = operator 6 .. operator 1, algorithms 1..32.
//   bits 0..1 output bus (0 = the voice output, 1 = bus one, 2 = bus two)   bit 2 add to the bus instead of replacing it
//   bits 4..5 input bus (1 = bus one, 2 = bus two)                          bit 6 feedback in, bit 7 feedback out
constexpr uint8_t OUT1 = 1, OUT2 = 2, ADD = 4, IN1 = 0x10, IN2 = 0x20, FBIN = 0x40, FBOUT = 0x80;
const uint8_t kAlgo[32][6] = {
    {0xc1, 0x11, 0x11, 0x14, 0x01, 0x14}, {0x01, 0x11, 0x11, 0x14, 0xc1, 0x14}, {0xc1, 0x11, 0x14, 0x01, 0x11, 0x14},
    {0x41, 0x11, 0x94, 0x01, 0x11, 0x14}, {0xc1, 0x14, 0x01, 0x14, 0x01, 0x14}, {0x41, 0x94, 0x01, 0x14, 0x01, 0x14},
    {0xc1, 0x11, 0x05, 0x14, 0x01, 0x14}, {0x01, 0x11, 0xc5, 0x14, 0x01, 0x14}, {0x01, 0x11, 0x05, 0x14, 0xc1, 0x14},
    {0x01, 0x05, 0x14, 0xc1, 0x11, 0x14}, {0xc1, 0x05, 0x14, 0x01, 0x11, 0x14}, {0x01, 0x05, 0x05, 0x14, 0xc1, 0x14},
    {0xc1, 0x05, 0x05, 0x14, 0x01, 0x14}, {0xc1, 0x05, 0x11, 0x14, 0x01, 0x14}, {0x01, 0x05, 0x11, 0x14, 0xc1, 0x14},
    {0xc1, 0x11, 0x02, 0x25, 0x05, 0x14}, {0x01, 0x11, 0x02, 0x25, 0xc5, 0x14}, {0x01, 0x11, 0x11, 0xc5, 0x05, 0x14},
    {0xc1, 0x14, 0x14, 0x01, 0x11, 0x14}, {0x01, 0x05, 0x14, 0xc1, 0x14, 0x14}, {0x01, 0x14, 0x14, 0xc1, 0x14, 0x14},
    {0xc1, 0x14, 0x14, 0x14, 0x01, 0x14}, {0xc1, 0x14, 0x14, 0x01, 0x14, 0x04}, {0xc1, 0x14, 0x14, 0x14, 0x04, 0x04},
    {0xc1, 0x14, 0x14, 0x04, 0x04, 0x04}, {0xc1, 0x05, 0x14, 0x01, 0x14, 0x04}, {0x01, 0x05, 0x14, 0xc1, 0x14, 0x04},
    {0x04, 0xc1, 0x11, 0x14, 0x01, 0x14}, {0xc1, 0x14, 0x01, 0x14, 0x04, 0x04}, {0x04, 0xc1, 0x11, 0x14, 0x04, 0x04},
    {0xc1, 0x14, 0x04, 0x04, 0x04, 0x04}, {0xc4, 0x04, 0x04, 0x04, 0x04, 0x04},
};

constexpr int32_t kLevelOne = 99 << 16;                        // DX7 envelope level 99 in Q16

class Dx7 : public Module {
public:
    const ModuleInfo &info() const override {
        static const ModuleInfo i = {"Dx7", Scope::Voice, 2, 1, DX7_N, false, {"pitch", "gate"}, {"out"},
                                     {{"gain", 8192, 0, 32767}}};
        return i;
    }
    bool init(Memory &) override {
        a4_ = inc_a4();
        std::memset(&patch_, 0, sizeof patch_);
        patch_.algorithm = 1;
        return true;
    }
    void reset() override {
        for (auto &o : op_) o = OpState{};
        for (int k = 0; k < kDx7Ops; k++) { op_[k].phase = 0x40000000u; enter(k, 0, true); }     // AMY starts operators at a quarter turn
        gate_ = false;
        released_ = false;
    }
    void set_param(int idx, int32_t v) override { if (idx == DX7_GAIN) gain_ = v; }
    void set_blob(const void *data, size_t bytes) override { if (bytes == sizeof(Dx7Patch)) std::memcpy(&patch_, data, sizeof patch_); }

    SC_HOT void process(const ProcessCtx &ctx, const Ports &p) override {
        const int n = ctx.frames;
        const bool gate = p.in[1][0] > 16384;
        if (gate && !gate_) { for (int k = 0; k < kDx7Ops; k++) enter(k, 0, false); released_ = false; }
        else if (!gate && gate_) { for (int k = 0; k < kDx7Ops; k++) enter(k, 4, false); released_ = true; }
        gate_ = gate;

        const int32_t pitch = kPitchCvCenter + static_cast<int32_t>(static_cast<int64_t>(p.in[0][0]) * kPitchCvSpan / 32768);
        const uint32_t base = pitch_to_inc(pitch, a4_);
        const uint8_t *algo = kAlgo[(patch_.algorithm < 1 || patch_.algorithm > 32 ? 1 : patch_.algorithm) - 1];
        const int32_t fb_amount = patch_.feedback_q15;

        int32_t bus1[kBlock] = {}, bus2[kBlock] = {}, scratch[kBlock] = {}, mix[kBlock] = {};
        for (int slot = 0; slot < kDx7Ops; slot++) {
            const int k = 5 - slot;                                            // table order is op6 .. op1; op_[k] is operator k + 1
            const uint8_t f = algo[slot];
            const Dx7OpCfg &cfg = patch_.op[k];
            OpState &o = op_[k];

            // envelope: one step per block, amplitude interpolated across the block
            step_envelope(o, cfg);
            const int32_t amp_end = amp_of(cfg, o);
            const int32_t amp_start = o.amp;
            o.amp = amp_end;

            const int32_t *in = (f & IN1) ? bus1 : ((f & IN2) ? bus2 : nullptr);
            int32_t *out;
            if ((f & IN1) && (f & OUT1)) out = scratch;                          // reads bus one while replacing it
            else if (f & OUT1) out = bus1;
            else if (f & OUT2) out = bus2;
            else out = mix;
            const bool to_voice = !(f & (OUT1 | OUT2));
            if (!(f & ADD) && !to_voice) std::memset(out, 0, sizeof(int32_t) * kBlock);

            const uint32_t inc = cfg.fixed_inc ? cfg.fixed_inc : static_cast<uint32_t>((static_cast<uint64_t>(base) * cfg.ratio_q16) >> 16);
            const int32_t fb = (f & FBIN) ? fb_amount : 0;
            if (cfg.gain_q28 == 0 || (amp_start == 0 && amp_end == 0)) {
                o.phase += inc * static_cast<uint32_t>(n);
                if (fb) { o.y1 = o.y2 = 0; }
            } else {
                uint32_t ph = o.phase;
                int32_t y1 = o.y1, y2 = o.y2;
                for (int i = 0; i < n; i++) {
                    uint32_t tp = ph;
                    if (in) tp += static_cast<uint32_t>(in[i]) << 4;                       // Q28 cycles -> phase units
                    if (fb) tp += static_cast<uint32_t>((fb * ((y1 + y2) >> 1)) >> 15) << 17;   // Q15 cycles -> phase units
                    const int32_t s = sine(tp);                                           // raw sine, q15
                    y2 = y1;
                    y1 = s;
                    const int32_t a = amp_start + static_cast<int32_t>((static_cast<int64_t>(amp_end - amp_start) * i) >> kLog2Block);
                    const int32_t v = static_cast<int32_t>((static_cast<int64_t>(s) * a) >> 15);   // Q28
                    if (f & ADD || to_voice) out[i] += v; else out[i] = v;
                    ph += inc;
                }
                o.phase = ph;
                o.y1 = y1;
                o.y2 = y2;
            }
            if (out == scratch) std::memcpy(bus1, scratch, sizeof(int32_t) * kBlock);
        }

        // carriers: x 1/4 (as AMY), velocity, trim; Q28 -> q15
        const int32_t vel = ctx.voice ? ctx.voice->velocity : kUnity;
        const int32_t g = (gain_ * vel) >> 13;                                         // q15 (gain Q13 x velocity q15)
        for (int i = 0; i < n; i++)
            p.out[0][i] = sat16(static_cast<int32_t>((static_cast<int64_t>(mix[i]) * g) >> 30));   // Q28 x q15 -> q15, and the 1/4 of the carriers
    }

private:
    static constexpr int kLog2Block = kBlock == 8 ? 3 : kBlock == 16 ? 4 : kBlock == 32 ? 5 : kBlock == 64 ? 6 : kBlock == 128 ? 7 : 8;

    struct OpState {
        uint32_t phase = 0;
        int32_t y1 = 0, y2 = 0;
        int32_t lv = 0;              // envelope level, Q16 (99 << 16 = full)
        int32_t target = 0, step = 0;
        int stage = 3;               // 0..2 attack stages, 3 = sustain, 4 = release, 5 = finished
        int32_t amp = 0;             // amplitude at the end of the previous block, Q28
    };

    // Starts stage `s`. s = 0 on note-on also resets the level to the release level (stage 4's target), as the patches do.
    void enter(int k, int s, bool initial) {
        OpState &o = op_[k];
        const Dx7OpCfg &cfg = patch_.op[k];
        if (s == 0) {
            if (initial || true) o.lv = static_cast<int32_t>(cfg.eg_l[3]) << 16;
            if (initial) o.amp = 0;
        }
        o.stage = s;
        start_segment(o, cfg);
    }

    void start_segment(OpState &o, const Dx7OpCfg &cfg) {
        if (o.stage > 2 && o.stage != 4) { o.step = 0; return; }
        const int idx = o.stage == 4 ? 3 : o.stage;
        o.target = static_cast<int32_t>(cfg.eg_l[idx]) << 16;
        const uint32_t blocks = cfg.eg_ms[idx] * static_cast<uint32_t>(kControlRate) / 1000u;
        o.step = static_cast<int32_t>((static_cast<int64_t>(o.target) - o.lv) / static_cast<int64_t>(blocks < 1 ? 1 : blocks));
        if (o.step == 0 && o.target != o.lv) o.step = o.target > o.lv ? 1 : -1;
    }

    void step_envelope(OpState &o, const Dx7OpCfg &cfg) {
        if (o.stage > 2 && o.stage != 4) return;                                        // sustaining or finished
        const bool up = o.target >= o.lv;
        const int64_t next = static_cast<int64_t>(o.lv) + o.step;
        if ((up && next >= o.target) || (!up && next <= o.target) || o.step == 0) {
            o.lv = o.target;
            if (o.stage == 4) o.stage = 5;
            else { o.stage++; if (o.stage <= 2) start_segment(o, cfg); }
        } else {
            o.lv = static_cast<int32_t>(next);
        }
    }

    int32_t amp_of(const Dx7OpCfg &cfg, const OpState &o) const {
        if (cfg.gain_q28 == 0 || o.lv <= 0) return 0;
        const int32_t x = static_cast<int32_t>((static_cast<int64_t>(o.lv - kLevelOne) * 65536) / (8 * 65536) );     // (lv - 99) / 8 octaves, Q16
        const uint32_t env_q16 = exp2_scale(65536u, x);                                      // <= 1.0
        return static_cast<int32_t>((static_cast<int64_t>(cfg.gain_q28) * env_q16) >> 16);
    }

    Dx7Patch patch_{};
    OpState op_[kDx7Ops];
    uint32_t a4_ = 0;
    bool gate_ = false, released_ = false;
    int32_t gain_ = 8192;                                       // Q13: 8192 = unity, up to 4.0
};

template <typename T>
ModuleType type_of() { static T probe; return {&probe.info(), &create_module<T>}; }

}  // namespace

void register_dx7_module(Registry &r) { r.add(T_DX7, type_of<Dx7>()); }

}  // namespace sc
