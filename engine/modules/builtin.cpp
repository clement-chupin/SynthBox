#include "engine/modules/builtin.h"
#include "engine/sampler/capture.h"
#include "engine/dsp/block.h"

namespace sc {
namespace {

class NoteIn : public Module {
public:
    const ModuleInfo &info() const override {
        static const ModuleInfo i = {"NoteIn", Scope::Voice, 0, 3, 0, false, {}, {"pitch", "gate", "vel"}, {}};
        return i;
    }
    SC_HOT void process(const ProcessCtx &ctx, const Ports &p) override {
        const VoiceState *v = ctx.voice;
        q15 pitch = v ? pitch_to_cv(v->pitch) : 0, gate = v && v->gate ? kUnity : 0, vel = v ? v->velocity : 0;
        for (int i = 0; i < ctx.frames; i++) { p.out[0][i] = pitch; p.out[1][i] = gate; p.out[2][i] = vel; }
    }
};

class VoiceOut : public Module {
public:
    const ModuleInfo &info() const override {
        static const ModuleInfo i = {"VoiceOut", Scope::Voice, 2, 0, 4, false, {"in", "in2"}, {},
                                     {{"level", kUnity, 0, kUnity}, {"pan", 0, -32768, 32767}, {"tail_ms", 4000, 0, 60000}, {"in2", 0, 0, 1}}};
        return i;
    }
    void reset() override { silent_ = 0; }
    void set_param(int idx, int32_t v) override {
        if (idx == VO_LEVEL) level_ = static_cast<q15>(v);
        else if (idx == VO_PAN) pan_ = static_cast<q15>(v);
        else if (idx == VO_TAIL_MS) tail_blocks_ = static_cast<uint32_t>(v) * kControlRate / 1000u;
        else if (idx == VO_IN2) in2_ = v != 0;
    }
    SC_HOT void process(const ProcessCtx &ctx, const Ports &p) override {
        // pan: linear, the louder side stays at `level` (centre = both at level)
        int32_t pan = pan_;
        q15 gl = pan > 0 ? mul15(level_, static_cast<q15>(32767 - pan)) : level_;
        q15 gr = pan < 0 ? mul15(level_, static_cast<q15>(32767 + pan)) : level_;
        // The bus is 32 bits: the voices add without clipping, BusIn saturates the sum once (a q15 bus clipped after every voice).
        add(ctx.frames, p.in[0], gl, gr, ctx.bus_l, ctx.bus_r);
        if (in2_ && ctx.bus2_l) add(ctx.frames, p.in[1], gl, gr, ctx.bus2_l, ctx.bus2_r);

        VoiceState *v = ctx.voice;
        if (!v || v->gate) { silent_ = 0; return; }
        const bool quiet = block_peak(p.in[0], ctx.frames) <= 4 && (!in2_ || block_peak(p.in[1], ctx.frames) <= 4);
        silent_ = quiet ? silent_ + 1 : 0;
        if (silent_ >= 8 || v->release_age >= tail_blocks_) v->done = true;
    }
private:
    static void add(int n, const q15 *x, q15 gl, q15 gr, int32_t *bl, int32_t *br) {
        if (gl == gr) {                                              // centred (the usual case): one product for both sides
            for (int i = 0; i < n; i++) { const int32_t m = mul15(x[i], gl); bl[i] += m; br[i] += m; }
        } else {
            for (int i = 0; i < n; i++) { bl[i] += mul15(x[i], gl); br[i] += mul15(x[i], gr); }
        }
    }
    q15 level_ = kUnity, pan_ = 0;
    bool in2_ = false;
    uint32_t silent_ = 0, tail_blocks_ = 4000u * kControlRate / 1000u;
};

class BusIn : public Module {
public:
    const ModuleInfo &info() const override {
        static const ModuleInfo i = {"BusIn", Scope::Global, 0, 2, 1, true, {}, {"L", "R"}, {{"bus", 0, 0, 1}}};
        return i;
    }
    void set_param(int idx, int32_t v) override { if (idx == BUS_SEL) bus2_ = v != 0; }
    SC_HOT void process(const ProcessCtx &ctx, const Ports &p) override {
        const int32_t *bl = bus2_ && ctx.bus2_l ? ctx.bus2_l : ctx.bus_l, *br = bus2_ && ctx.bus2_r ? ctx.bus2_r : ctx.bus_r;
        for (int i = 0; i < ctx.frames; i++) { p.out[0][i] = sat16(bl[i]); p.out[1][i] = sat16(br[i]); }
    }
private:
    bool bus2_ = false;
};

class MasterOut : public Module {
public:
    const ModuleInfo &info() const override {
        static const ModuleInfo i = {"MasterOut", Scope::Global, 2, 0, 3, true, {"L", "R"}, {},
                                     {{"level", kUnity, 0, kUnity}, {"boost", 0, 0, 1}, {"mono", 0, 0, 1}}};
        return i;
    }
    // level: q15 gain; boost = 1 doubles it (saturating), so the master volume reaches 2.0; mono = 1 sends (L + R) / 2 to both outputs.
    // A new gain is reached by a linear ramp over one block: the volume knob moves in 5 % steps, applied at once they were audible as zipper noise.
    void set_param(int idx, int32_t v) override {
        if (idx == 0) level_ = static_cast<q15>(v);
        else if (idx == 1) boost_ = v != 0;
        else if (idx == 2) mono_ = v != 0;
        target_ = boost_ ? 2 * static_cast<int32_t>(level_) : level_;     // q15 gain, up to 2.0
        if (first_) { gain_ = target_; }
    }
    SC_HOT void process(const ProcessCtx &ctx, const Ports &p) override {
        const q15 *l = p.in[0], *r = p.in[1];
        first_ = false;
        const int n = ctx.frames;
        tap_.feed(l, r, n, ctx.transport);                                    // a recording takes what comes in, before the volume
        const int32_t step = (target_ - gain_) / n;                           // the last sample lands exactly on the target
        int32_t g = gain_;
        for (int i = 0; i < n; i++) {
            g = i == n - 1 ? target_ : g + step;
            q15 a = l[i], b = r[i];
            if (mono_) a = b = static_cast<q15>((static_cast<int32_t>(a) + b) >> 1);
            ctx.out_l[i] = sat16((static_cast<int32_t>(a) * g + (1 << 14)) >> 15);
            ctx.out_r[i] = sat16((static_cast<int32_t>(b) * g + (1 << 14)) >> 15);
        }
        gain_ = target_;
    }
private:
    CaptureTap &tap_ = capture_tap();
    q15 level_ = kUnity;
    bool boost_ = false, mono_ = false, first_ = true;
    int32_t target_ = kUnity, gain_ = kUnity;                                 // q15 gain (0..65534): the set one, and where the ramp is
};

template <typename T>
ModuleType type_of() { static T probe; return {&probe.info(), &create_module<T>}; }

}  // namespace

void register_builtin_modules(Registry &reg) {
    reg.add(T_NOTE_IN, type_of<NoteIn>());
    reg.add(T_VOICE_OUT, type_of<VoiceOut>());
    reg.add(T_BUS_IN, type_of<BusIn>());
    reg.add(T_MASTER_OUT, type_of<MasterOut>());
}

}  // namespace sc
