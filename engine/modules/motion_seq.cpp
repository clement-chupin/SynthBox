#include "engine/modules/motion_seq.h"
#include <cstring>

namespace sc {

int64_t motion_lane_value_q16(const MotionBlob &b, int lane, int steps, uint32_t pos_q16) {
    if (steps < 1) steps = 1;
    if (steps > kMsSteps) steps = kMsSteps;
    const unsigned mask = b.active[lane] & ((1u << steps) - 1u);
    if (!mask) return -1;
    int k = static_cast<int>(pos_q16 >> 16);
    if (k >= steps) k = steps - 1;
    const int64_t frac = pos_q16 & 0xFFFF;
    int prev = k;
    for (int d = 0; d < steps; d++) {
        const int i = (k - d + steps) % steps;
        if (mask >> i & 1u) { prev = i; break; }
    }
    if (!b.linear[lane] || (mask & (mask - 1)) == 0) return static_cast<int64_t>(b.val[lane][prev]) << 16;   // STEP, or a single active step
    int next = prev;
    for (int d = 1; d <= steps; d++) {
        const int i = (k + d) % steps;
        if (mask >> i & 1u) { next = i; break; }
    }
    const int dist = (next - prev + steps) % steps;
    const int done = (k - prev + steps) % steps;
    const int64_t t = ((static_cast<int64_t>(done) << 16) + frac) / dist;                       // Q16 position between prev and next
    const int64_t a = static_cast<int64_t>(b.val[lane][prev]) << 16, c = static_cast<int64_t>(b.val[lane][next]) << 16;
    return a + (((c - a) * t) >> 16);
}

namespace {

class MotionSeq : public Module {
public:
    const ModuleInfo &info() const override {
        static const ModuleInfo i = {"MotionSeq", Scope::Global, 0, kMsLanes, 0, false, {}, {"lane1", "lane2", "lane3", "lane4"}, {}};
        return i;
    }
    void reset() override { for (int32_t &v : last_) v = 0; }
    void set_blob(const void *data, size_t bytes) override { if (bytes == sizeof(MotionBlob)) std::memcpy(&blob_, data, sizeof blob_); }

    // Each lane ramps over the block from where it ended to its value at the block's end (0 while the sequencer does not play).
    void process(const ProcessCtx &ctx, const Ports &p) override {
        const SeqPos *sq = ctx.seq;
        const bool run = sq && sq->running;
        for (int l = 0; l < kMsLanes; l++) {
            const int32_t a = last_[l], c = run ? out_of(l, *sq) : 0;
            last_[l] = c;
            if (a == c) { for (int i = 0; i < ctx.frames; i++) p.out[l][i] = static_cast<q15>(a); continue; }
            for (int i = 0; i < ctx.frames; i++) p.out[l][i] = static_cast<q15>(a + static_cast<int32_t>((static_cast<int64_t>(c - a) * (i + 1)) / ctx.frames));
        }
    }

private:
    int32_t out_of(int lane, const SeqPos &sq) const {
        const int steps = motion_steps(sq.steps);
        const uint32_t k = (sq.pos_q16 >> 16) % static_cast<uint32_t>(steps);
        const int64_t v = motion_lane_value_q16(blob_, lane, steps, (k << 16) | (sq.pos_q16 & 0xFFFF));
        if (v < 0) return 0;                                                   // no active step: the lane outputs nothing
        if (blob_.bipolar[lane]) return static_cast<int32_t>(((v - (50 << 16)) * 2 * 32767) / (100ll << 16));
        return static_cast<int32_t>((v * 32767) / (100ll << 16));
    }

    MotionBlob blob_{};
    int32_t last_[kMsLanes] = {};
};

template <typename T>
ModuleType type_of() { static T probe; return {&probe.info(), &create_module<T>}; }

}  // namespace

void register_motion_module(Registry &r) { r.add(T_MSEQ, type_of<MotionSeq>()); }

}  // namespace sc
