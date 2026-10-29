#include "engine/modules/para_modules.h"
#include "engine/modules/builtin.h"

namespace sc {
namespace {

class GateIn : public Module {
public:
    const ModuleInfo &info() const override {
        static const ModuleInfo i = {"GateIn", Scope::Global, 0, 4, 0, false, {}, {"gate", "retrig", "pitch", "vel"}, {}};
        return i;
    }
    SC_HOT void process(const ProcessCtx &ctx, const Ports &p) override {
        bool any = false, fresh = false;
        uint32_t newest_age = 0xFFFFFFFFu;
        for (int v = 0; v < ctx.nvoices; v++) {
            const VoiceState &vs = ctx.voices[v];
            if (!vs.active || !vs.gate) continue;
            any = true;
            fresh |= vs.started;
            if (vs.age <= newest_age) { newest_age = vs.age; pitch_ = pitch_to_cv(vs.pitch); vel_ = vs.velocity; }   // the newest key leads
        }
        // a new key while others were already held: the retrigger output drops for this block (an envelope restarts its attack)
        const bool dip = fresh && held_before_;
        held_before_ = any;
        const q15 g = any ? kUnity : 0, r = any && !dip ? kUnity : 0;
        for (int i = 0; i < ctx.frames; i++) { p.out[0][i] = g; p.out[1][i] = r; p.out[2][i] = pitch_; p.out[3][i] = vel_; }
    }
private:
    q15 pitch_ = 0, vel_ = 0;
    bool held_before_ = false;
};

class ParaGate : public Module {
public:
    const ModuleInfo &info() const override {
        static const ModuleInfo i = {"ParaGate", Scope::Voice, 1, 1, 0, false, {"in"}, {"out"}, {}};
        return i;
    }
    void reset() override { g_ = 0; muted_ = false; }
    SC_HOT void process(const ProcessCtx &ctx, const Ports &p) override {
        const VoiceState *v = ctx.voice;
        const bool gate = v && v->gate;
        if (gate) muted_ = false;
        else if (ctx.keys_held > 0) muted_ = true;                       // released while other keys play: this key stops
        const int32_t target = gate || !muted_ ? kUnity : 0;
        const q15 *in = p.in[0];
        q15 *out = p.out[0];
        if (g_ == target) {                                              // steady: open (copy) or closed (silence)
            if (target) for (int i = 0; i < ctx.frames; i++) out[i] = in[i];
            else for (int i = 0; i < ctx.frames; i++) out[i] = 0;
            return;
        }
        for (int i = 0; i < ctx.frames; i++) {                          // 5 ms linear ramp towards the target
            g_ = g_ < target ? (g_ + kStep > target ? target : g_ + kStep) : (g_ - kStep < target ? target : g_ - kStep);
            out[i] = static_cast<q15>((in[i] * g_) >> 15);
        }
    }
private:
    static constexpr int32_t kStep = kUnity / (kSampleRate / 200) + 1;   // 0 -> 1 in about 5 ms
    int32_t g_ = 0;
    bool muted_ = false;
};

template <typename T>
ModuleType type_of() { static T probe; return {&probe.info(), &create_module<T>}; }

}  // namespace

void register_para_modules(Registry &reg) {
    reg.add(T_GATE_IN, type_of<GateIn>());
    reg.add(T_PARA_GATE, type_of<ParaGate>());
}

}  // namespace sc
