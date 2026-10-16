#include "engine/modules/sampler_modules.h"
#include "engine/core/sequencer.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include "engine/dsp/block.h"
#include "engine/dsp/interp.h"
#include "engine/dsp/phase.h"
#include "engine/dsp/svf.h"
#include "engine/dsp/util.h"
#include "engine/modules/builtin.h"
#include "engine/sampler/sample_bank.h"

namespace sc {
namespace {

constexpr int32_t kSemi = 256;
inline int32_t scaled(q15 m, int32_t range) { return static_cast<int32_t>((static_cast<int64_t>(m) * range) >> 15); }

/* ------------------------------------------------------------------ Sampler */

class Sampler : public Module {
public:
    const ModuleInfo &info() const override {
        static const ModuleInfo i = {"Sampler", Scope::Voice, 1, 1, SMPR_N, false, {"pitch"}, {"out"},
            {{"sample", -1, -1, 31}, {"instrument", -1, -1, 7}, {"tune", 0, -48 * kSemi, 48 * kSemi}, {"root", -1, -1, 127 * kSemi},
             {"loop", -1, -1, 2}, {"reverse", 0, 0, 1}, {"start", 0, 0, 60000 * 16}, {"slice", -1, -1, 15}, {"slice_mode", 0, 0, 1},
             {"gain", kUnity, 0, kUnity}, {"interp", 1, 0, 1}, {"track", 1, 0, 1},
             {"fstart", -1, -1, 0x7FFFFFFF}, {"fend", 0, 0, 0x7FFFFFFF}, {"region", 0, 0, 1}}};
        return i;
    }
    // The Pads module drives its voices directly (same thread as process()): stop at once, and whether it still sounds (or waits for its sample).
    void halt() { want_start_ = false; active_ = false; if (bank_ && ring_) stream_.stop(); }
    // The pads' loop details, read at the next start: the region loop's start (frames, -1 = the trim start) and a crossfade of `xfade`
    // frames at the end of a forward loop (equal-power; Pre: the fade-in material is the frames before the loop start, Post: the loop's
    // first frames, the wrap then lands after them). Capped at half the loop, and for Pre at the frames that exist before the loop start.
    void set_loop_extra(int32_t lstart, int32_t xfade, bool post) { lstart_ = lstart; xfade_ = xfade; xpost_ = post; }
    bool playing() const { return active_ || want_start_; }
    uint32_t position() const { return static_cast<uint32_t>(pos_ >> 32); }
    bool init(Memory &m) override {
        bank_ = m.bank;
        if (!bank_) return true;                                           // no sampler service: the module stays silent
        ring_heap_ = m.bulk;
        ring_ = static_cast<q15 *>(m.bulk->alloc(static_cast<size_t>(kRingBlocks) * kSmpBlockBytes));
        if (!ring_) return false;
        stream_.attach(ring_);
        bank_->add_stream(&stream_);
        return true;
    }
    ~Sampler() override {
        if (bank_) bank_->remove_stream(&stream_);
        if (ring_heap_) ring_heap_->free(ring_);
    }
    void reset() override { want_start_ = true; retry_ = 0; active_ = false; held_ = 0; xf_ = 32767; underrun_ = false; if (bank_ && ring_) stream_.stop(); }
    void set_param(int idx, int32_t v) override {
        switch (idx) {
            case SMPR_SAMPLE: sample_ = v; break;
            case SMPR_INSTRUMENT: inst_ = v; break;
            case SMPR_TUNE: tune_ = v; break;
            case SMPR_ROOT: root_ = v; break;
            case SMPR_LOOP: loop_param_ = v; break;
            case SMPR_REVERSE: reverse_ = v != 0; break;
            case SMPR_START: start_ = v; break;
            case SMPR_SLICE: slice_ = v; break;
            case SMPR_SLICE_MODE: slice_mode_ = v; break;
            case SMPR_GAIN: gain_ = static_cast<q15>(v); break;
            case SMPR_INTERP: hermite_ = v != 0; break;
            case SMPR_TRACK: track_ = v != 0; break;
            case SMPR_FSTART: fstart_ = v; break;
            case SMPR_FEND: fend_ = v; break;
            case SMPR_REGION: region_ = v != 0; break;
        }
    }
    void process(const ProcessCtx &ctx, const Ports &p) override {
        const int n = ctx.frames;
        if (!bank_ || !ring_) { block_clear(p.out[0], n); return; }
        if (want_start_) {
            if (begin(ctx.voice)) want_start_ = false;
            else if (++retry_ > 64) want_start_ = false;                    // the sample never became ready: stay silent
        }
        if (!active_) { block_clear(p.out[0], n); return; }
        miss_blk_ = -1;                                                     // a block missing in the last call may have arrived since

        const q15 *cv = p.in[0], *mt = p.mod[SMPR_TUNE];
        const bool varying = mt || cv[0] != cv[n - 1];
        int64_t rate = rate_for(cv[0], mt ? mt[0] : 0);
        if (!varying && (fast_block(n, rate, p.out[0]) || starved_block(n, rate, p.out[0]))) {
            stream_.update(static_cast<uint32_t>(pos_ >> 32), dir_, static_cast<int32_t>(rate >> 16));
            return;
        }
        for (int i = 0; i < n; i++) {
            if (varying) rate = rate_for(cv[i], mt ? mt[i] : 0);
            const int64_t f = pos_ >> 32;
            const uint32_t frac = static_cast<uint32_t>((pos_ >> 16) & 0xFFFF);
            q15 y1 = 0, y2 = 0;
            bool ok = fetch(f, y1) & fetch(f + 1, y2);
            int32_t s;
            if (hermite_) {
                q15 y0 = 0, y3 = 0;
                ok &= fetch(f - 1, y0) & fetch(f + 2, y3);
                s = hermite15(y0, y1, y2, y3, frac);
            } else {
                s = lerp15(y1, y2, frac);
            }
            if (lxf_ && dir_ > 0) {                                                 // the loop crossfade: mix in the fade-in material
                const int64_t fs = static_cast<int64_t>(loop_.end - lxf_) << 32;
                if (pos_ >= fs) {
                    const int64_t q = pos_ - (static_cast<int64_t>(loop_.end - loop_.start) << 32);
                    const int32_t s2 = value_at(q);
                    const uint32_t ph = static_cast<uint32_t>(((pos_ - fs) / lxf_) >> 2);         // 0 .. a quarter turn over the fade
                    s = static_cast<int32_t>((static_cast<int64_t>(s) * sine(ph + 0x40000000u) + static_cast<int64_t>(s2) * sine(ph)) >> 15);
                }
            }
            // Underrun: the output keeps the value it had and decays by 1/64 per sample (a half-interpolated value would
            // click); the timeline keeps running. When the data is back, the output crosses over from the held value to
            // the live signal in 64 samples, so it is continuous at both ends whatever the length of the gap.
            int32_t y;
            if (ok) {
                if (underrun_) { underrun_ = false; xf_ = 0; }
                if (xf_ < 32767) {
                    xf_ = xf_ + 512 > 32767 ? 32767 : xf_ + 512;
                    y = held_ + static_cast<int32_t>((static_cast<int64_t>(s - held_) * xf_) >> 15);
                } else {
                    y = s;
                }
                held_ = y;
            } else {
                if (!underrun_) { underrun_ = true; bank_->stats.underruns.fetch_add(1, std::memory_order_relaxed); }
                held_ -= held_ >> 6;
                y = held_;
            }
            p.out[0][i] = mul15(sat16(y), gain_eff_);

            pos_ += dir_ > 0 ? rate : -rate;
            if (!wrap_position(pos_, dir_, loop_, frames_) || (stop_frame_ && dir_ > 0 && (pos_ >> 32) >= stop_frame_)) {
                active_ = false;
                for (int k = i + 1; k < n; k++) p.out[0][k] = 0;
                stream_.stop();
                return;
            }
        }
        stream_.update(static_cast<uint32_t>(pos_ >> 32), dir_, static_cast<int32_t>(rate >> 16));
    }

private:
    // The data of frames lo..hi (inclusive) as one contiguous run: a resident head, or one block of the stream ring. nullptr when it is not in
    // memory (or spans two ring blocks). `first` is the frame that run[0] holds.
    const q15 *run(int64_t lo, int64_t hi, int64_t &first) {
        if (lo >= cache_lo_ && hi < cache_hi_) { first = cache_lo_; return cache_ptr_; }
        for (int h = 0; h < sl_->n_heads; h++) {
            const Head &hd = sl_->head[h];
            const int64_t end = static_cast<int64_t>(hd.start_frame) + hd.frames;
            if (lo >= hd.start_frame && hi < end) { cache_ptr_ = hd.data; cache_lo_ = hd.start_frame; cache_hi_ = end; first = cache_lo_; return cache_ptr_; }
        }
        const int64_t blk = lo / kSmpBlockFrames;
        if (hi / kSmpBlockFrames != blk) return nullptr;
        const q15 *b = stream_.block(gen_, static_cast<uint32_t>(blk));
        if (!b) return nullptr;
        cache_ptr_ = b; cache_lo_ = blk * kSmpBlockFrames; cache_hi_ = cache_lo_ + kSmpBlockFrames;
        first = cache_lo_;
        return cache_ptr_;
    }

    // One block at a steady rate, played forward, that neither wraps a loop nor ends the sample and whose data is one contiguous run in memory:
    // no per-sample bounds, cache or loop checks (the general loop in process() costs about 490 cycles per sample, this about a fifth of that).
    // Same arithmetic as the general loop, so the output is identical. Returns false when the block does not qualify (the general loop runs it).
    bool fast_block(int n, int64_t rate, q15 *out) {
#ifdef SC_SAMPLER_NO_FAST                                                        // test switch: the general loop only (output comparison)
        (void)n; (void)rate; (void)out;
        return false;
#endif
        if (dir_ < 0 || underrun_ || xf_ < 32767) return false;
        const int64_t pos_end = pos_ + rate * n;                                    // where the block ends: the general loop wraps / stops there
        const int64_t limit = static_cast<int64_t>(loop_.active() ? loop_.end - lxf_ : frames_) << 32;    // (the crossfade runs in the general loop)
        if (pos_end >= limit || (stop_frame_ && (pos_end >> 32) >= stop_frame_)) return false;
        const int64_t f0 = pos_ >> 32, f_last = (pos_ + rate * (n - 1)) >> 32;
        if (f0 < 1 || f_last + 2 >= static_cast<int64_t>(frames_)) return false;    // the neighbours of the interpolator exist (outside the file they read as silence)
        int64_t first;
        const q15 *data = run(f0 - 1, f_last + 2, first);
        if (!data) return false;
        const q15 *base = data - first;                                             // base[f] = frame f
        int64_t pos = pos_;
        int32_t s = 0;
        if (hermite_) {
            for (int i = 0; i < n; i++, pos += rate) {
                const q15 *q = base + (pos >> 32);
                s = hermite15(q[-1], q[0], q[1], q[2], static_cast<uint32_t>(pos >> 16) & 0xFFFFu);
                out[i] = mul15(sat16(s), gain_eff_);
            }
        } else {
            for (int i = 0; i < n; i++, pos += rate) {
                const q15 *q = base + (pos >> 32);
                s = lerp15(q[0], q[1], static_cast<uint32_t>(pos >> 16) & 0xFFFFu);
                out[i] = mul15(sat16(s), gain_eff_);
            }
        }
        pos_ = pos;
        held_ = s;                                                                  // what an underrun in the next block would decay from
        return true;
    }

    // One block whose playhead stays inside a ring block that has not arrived (an underrun that lasts the whole block): every sample of the
    // general loop would miss its y1 frame and output the held value decaying by 1/64. Same output for a few cycles per sample: the general loop
    // spent ~490 cycles per sample on it (~31k per starved voice and block), so a few starved voices overran the audio budget and the whole synth
    // stuttered. Same conditions as fast_block: forward, steady rate, no loop wrap or end inside the block.
    bool starved_block(int n, int64_t rate, q15 *out) {
#ifdef SC_SAMPLER_NO_FAST
        (void)n; (void)rate; (void)out;
        return false;
#endif
        if (dir_ < 0) return false;
        const int64_t pos_end = pos_ + rate * n;
        const int64_t limit = static_cast<int64_t>(loop_.active() ? loop_.end - lxf_ : frames_) << 32;    // (the crossfade runs in the general loop)
        if (pos_end >= limit || (stop_frame_ && (pos_end >> 32) >= stop_frame_)) return false;
        const int64_t f0 = pos_ >> 32, f_last = (pos_ + rate * (n - 1)) >> 32;
        if (f0 < 1 || f_last + 2 >= static_cast<int64_t>(frames_)) return false;
        if (f0 / kSmpBlockFrames != f_last / kSmpBlockFrames) return false;        // y1 of every sample is in the same ring block ...
        q15 probe;
        if (fetch(f0, probe)) return false;                                         // ... and that block is missing
        if (!underrun_) { underrun_ = true; bank_->stats.underruns.fetch_add(1, std::memory_order_relaxed); }
        int32_t h = held_;
        for (int i = 0; i < n; i++) {
            h -= h >> 6;
            out[i] = mul15(sat16(h), gain_eff_);
        }
        held_ = h;
        pos_ = pos_end;
        return true;
    }

    int64_t rate_for(q15 cv, q15 mod) const {
        int32_t pitch;
        if (track_) pitch = kPitchCvCenter + scaled(cv, kPitchCvSpan) + tune_total_ + (mod ? scaled(mod, 96 * kSemi) : 0);
        else pitch = root_pitch_ + tune_total_ + (mod ? scaled(mod, 96 * kSemi) : 0);
        const int64_t oct_q16 = static_cast<int64_t>(pitch - root_pitch_) * 64 / 3;                 // 65536 / (12 * 256)
        const int32_t x = static_cast<int32_t>(oct_q16 < -(1 << 21) ? -(1 << 21) : (oct_q16 > (1 << 21) ? (1 << 21) : oct_q16));
        const uint32_t ratio_q28 = exp2_scale(base_q28_, x);                                         // source frames per output sample
        return static_cast<int64_t>(ratio_q28) << 4;                                                 // Q32
    }

    bool begin(const VoiceState *v) {
        const int note = v ? v->note : 60;
        const int vel = v ? (v->velocity * 127 + 16383) / 32767 : 100;
        int slot = sample_;
        const Zone *zone = nullptr;
        if (inst_ >= 0) {
            zone = bank_->pick(inst_, note, vel);
            if (!zone) return true;                                                                  // nothing mapped here: silent, done
            slot = zone->sample;
        }
        const SampleSlot *sl = bank_->slot(slot);
        if (!sl || sl->state == SLOT_FAILED) return true;
        if (sl->state != SLOT_READY) return false;                                                   // still loading: try again next block
        const SmpHeader &h = sl->h;
        frames_ = h.frames;
        const int mode = loop_param_ >= 0 ? loop_param_ : h.loop_mode;
        loop_ = LoopInfo{};
        loop_.mode = static_cast<uint8_t>(mode);
        if (mode != SMP_LOOP_OFF) { loop_.start = h.has_loop() ? h.loop_start : 0; loop_.end = h.has_loop() ? h.loop_end : h.frames; }
        loop_.mode = loop_.active() ? static_cast<uint8_t>(mode) : static_cast<uint8_t>(SMP_LOOP_OFF);

        uint32_t start = 0;
        stop_frame_ = 0;
        if (slice_ >= 0 && slice_ < h.slice_count) {
            start = h.slice[slice_];
            if (slice_mode_ == 1 && slice_ + 1 < h.slice_count) stop_frame_ = h.slice[slice_ + 1];
        }
        start += static_cast<uint32_t>(static_cast<int64_t>(start_) * h.sample_rate / 16000);
        if (fstart_ >= 0) {                                                                          // a trim in frames (the pads)
            start = static_cast<uint32_t>(fstart_);
            stop_frame_ = 0;
            const uint32_t end = fend_ > 0 && static_cast<uint32_t>(fend_) < frames_ ? static_cast<uint32_t>(fend_) : frames_;
            if (region_ && end > start + 1) {
                uint32_t ls = lstart_ >= 0 ? static_cast<uint32_t>(lstart_) : start;
                if (ls < start) ls = start;
                if (ls + 1 >= end) ls = end - 2;
                loop_ = LoopInfo{}; loop_.start = ls; loop_.end = end; loop_.mode = SMP_LOOP_FWD;
            }
            else if (end < frames_) stop_frame_ = end;
        }
        if (start >= frames_) return true;
        lxf_ = 0;
        if (xfade_ > 0 && loop_.mode == SMP_LOOP_FWD && loop_.active()) {
            uint32_t x = static_cast<uint32_t>(xfade_);
            const uint32_t len = loop_.end - loop_.start;
            if (x > len / 2) x = len / 2;
            if (xpost_) loop_.start += x;                                            // the first x frames of the loop are the fade-in
            else if (x > loop_.start) x = loop_.start;                               // Pre: only what exists before the loop start
            lxf_ = x;
        }
        dir_ = reverse_ ? -1 : 1;
        pos_ = (reverse_ ? static_cast<int64_t>(frames_ > 0 ? frames_ - 1 : 0) : static_cast<int64_t>(start)) << 32;

        int32_t root = zone && zone->root >= 0 ? zone->root * kSemi : (root_ >= 0 ? root_ : h.root_note * kSemi);
        root_pitch_ = root;
        tune_total_ = tune_ + (h.tune_cents + (zone ? zone->tune_cents : 0)) * kSemi / 100;
        gain_eff_ = zone ? mul15(gain_, zone->gain) : gain_;
        base_q28_ = static_cast<uint32_t>((static_cast<uint64_t>(h.sample_rate) << 28) / kSampleRate);
        sl_ = sl;
        cache_lo_ = cache_hi_ = 0;
        cache_ptr_ = nullptr;
        held_ = 0;
        xf_ = 32767;
        underrun_ = false;
        active_ = true;
        stream_.start(slot, frames_, static_cast<uint32_t>(pos_ >> 32), dir_, static_cast<int32_t>(rate_for(0, 0) >> 16) , loop_);
        gen_ = stream_.generation();
        return true;
    }

    // The interpolated value at a position (Q32 frames); a frame that has not arrived reads as silence.
    int32_t value_at(int64_t pos) {
        const int64_t f = pos >> 32;
        const uint32_t frac = static_cast<uint32_t>((pos >> 16) & 0xFFFF);
        q15 y0 = 0, y1 = 0, y2 = 0, y3 = 0;
        fetch(f, y1); fetch(f + 1, y2);
        if (!hermite_) return lerp15(y1, y2, frac);
        fetch(f - 1, y0); fetch(f + 2, y3);
        return hermite15(y0, y1, y2, y3, frac);
    }

    // One source frame; out-of-range frames read as silence. false = the data has not arrived (underrun).
    // A ring block found missing is not searched again in the same process() call (miss_blk_): the search reads every slot's 64-bit atomic
    // tag, a locked library call on the ESP32, and a starved voice did it 4 times per sample (182k cycles per block for one voice at 8x pitch,
    // measured). Data that lands during the block is picked up at the next block, at most 64 samples later.
    bool fetch(int64_t f, q15 &out) {
        if (f < 0 || f >= static_cast<int64_t>(frames_)) { out = 0; return true; }
        if (f >= cache_lo_ && f < cache_hi_) { out = cache_ptr_[f - cache_lo_]; return true; }
        for (int h = 0; h < sl_->n_heads; h++) {
            const Head &hd = sl_->head[h];
            if (f >= hd.start_frame && f < static_cast<int64_t>(hd.start_frame) + hd.frames) {
                cache_ptr_ = hd.data; cache_lo_ = hd.start_frame; cache_hi_ = static_cast<int64_t>(hd.start_frame) + hd.frames;
                out = cache_ptr_[f - cache_lo_];
                return true;
            }
        }
        const uint32_t blk = static_cast<uint32_t>(f / kSmpBlockFrames);
        if (static_cast<int64_t>(blk) == miss_blk_) return false;
        const q15 *b = stream_.block(gen_, blk);
        if (!b) { miss_blk_ = blk; return false; }
        cache_ptr_ = b; cache_lo_ = static_cast<int64_t>(blk) * kSmpBlockFrames; cache_hi_ = cache_lo_ + kSmpBlockFrames;
        out = cache_ptr_[f - cache_lo_];
        return true;
    }

    SampleBank *bank_ = nullptr;
    Heap *ring_heap_ = nullptr;
    q15 *ring_ = nullptr;
    Stream stream_;
    const SampleSlot *sl_ = nullptr;
    uint32_t gen_ = 0, frames_ = 0, stop_frame_ = 0, base_q28_ = 1u << 28;
    LoopInfo loop_;
    int64_t pos_ = 0, cache_lo_ = 0, cache_hi_ = 0;
    int64_t miss_blk_ = -1;                     // the ring block found missing in this process() call (-1: none)
    const q15 *cache_ptr_ = nullptr;
    int dir_ = 1, retry_ = 0;
    int32_t held_ = 0, xf_ = 32767, root_pitch_ = 60 * kSemi, tune_total_ = 0;
    bool want_start_ = true, active_ = false, underrun_ = false;
    // parameters
    int32_t sample_ = -1, inst_ = -1, tune_ = 0, root_ = -1, start_ = 0, slice_ = -1, slice_mode_ = 0;
    int32_t loop_param_ = -1, fstart_ = -1, fend_ = 0, lstart_ = -1, xfade_ = 0;
    uint32_t lxf_ = 0;                          // the loop crossfade of this start, in frames (0: none)
    bool xpost_ = false;
    q15 gain_ = kUnity, gain_eff_ = kUnity;
    bool reverse_ = false, hermite_ = true, track_ = true, region_ = false;
};

/* ------------------------------------------------------------------ Granular */

template <Scope S>
class Granular : public Module {
public:
    const ModuleInfo &info() const override {
        static const ModuleInfo i = {"Granular", S, 1, 1, GRN_N, false, {"pitch"}, {"out"},
            {{"sample", -1, -1, 31}, {"position", 0, 0, kUnity}, {"speed", 256, -1024, 1024}, {"size", 80, 5, 500},
             {"density", 20, 1, 200}, {"pitch", 0, -48 * kSemi, 48 * kSemi}, {"jitter", 4000, 0, kUnity}, {"level", kUnity, 0, kUnity}}};
        return i;
    }
    bool init(Memory &m) override { bank_ = m.bank; return true; }
    void reset() override { for (auto &g : grain_) g.active = false; timer_ = 0; scan_ = 0; }
    void set_param(int idx, int32_t v) override {
        switch (idx) {
            case GRN_SAMPLE: sample_ = v; break;
            case GRN_POSITION: position_ = v; break;
            case GRN_SPEED: speed_ = v; break;
            case GRN_SIZE: size_ = v; break;
            case GRN_DENSITY: density_ = v; break;
            case GRN_PITCH: pitch_ = v; break;
            case GRN_JITTER: jitter_ = v; break;
            case GRN_LEVEL: level_ = static_cast<q15>(v); break;
        }
        const double overlap = std::fmax(1.0, density_ * size_ / 1000.0);          // grains sounding at once on average
        norm_ = static_cast<q15>(32767.0 / std::sqrt(overlap));
    }
    void process(const ProcessCtx &ctx, const Ports &p) override {
        const int n = ctx.frames;
        const SampleSlot *sl = bank_ ? bank_->slot(sample_) : nullptr;
        if (!sl || sl->state != SLOT_READY || sl->n_heads == 0) { block_clear(p.out[0], n); return; }
        const Head &hd = sl->head[0];
        const int64_t region = static_cast<int64_t>(std::min<uint32_t>(hd.frames, sl->h.frames));
        if (region < 64) { block_clear(p.out[0], n); return; }
        const uint32_t grain_len = static_cast<uint32_t>(static_cast<int64_t>(size_) * kSampleRate / 1000);
        const int64_t interval_q16 = (static_cast<int64_t>(kSampleRate) << 16) / (density_ < 1 ? 1 : density_);   // mean samples between grains, Q16
        const int64_t src_per_out = (static_cast<int64_t>(sl->h.sample_rate) << 28) / kSampleRate;   // Q28 frames per output sample at original speed
        const int32_t pitch = pitch_ + (S == Scope::Voice ? kPitchCvCenter + scaled(p.in[0][0], kPitchCvSpan) - 60 * kSemi : 0);
        const int64_t inc = static_cast<int64_t>(exp2_scale(static_cast<uint32_t>(src_per_out), static_cast<int32_t>(static_cast<int64_t>(pitch) * 64 / 3))) << 4;   // Q32
        // the scan position moves at `speed` x real time through the sample, starting at `position`
        const int64_t scan_inc = (static_cast<int64_t>(speed_) * src_per_out) >> 4;               // Q32 frames per output sample (speed Q8, src Q28)

        for (int i = 0; i < n; i++) {
            timer_ -= 65536;
            if (timer_ <= 0) {
                spawn(region, grain_len, inc, scan_);
                // randomised onsets (0.5 .. 1.5 x the mean interval): a regular grain train is a comb filter that
                // puts strong peaks at multiples of the density and turns a tone into a buzz
                const int64_t u = noise_.next() + 32768;                                          // 0 .. 65535
                timer_ += (interval_q16 * (32768 + u)) >> 16;
            }
            scan_ += scan_inc;
            int32_t acc = 0;
            for (auto &g : grain_) {
                if (!g.active) continue;
                const uint32_t ph = static_cast<uint32_t>((static_cast<uint64_t>(g.age) << 31) / g.len);   // half a turn over the grain
                const int32_t sn = sine(ph);
                const int32_t w = (sn * sn) >> 15;                                                       // Hann = sin^2
                const int64_t f = g.pos >> 32;
                const int64_t f0 = ((f % region) + region) % region, f1 = (f0 + 1) % region;
                const q15 y = lerp15(hd.data[f0], hd.data[f1], static_cast<uint32_t>((g.pos >> 16) & 0xFFFF));
                acc += (y * w) >> 15;
                g.pos += g.inc;
                if (++g.age >= g.len) g.active = false;
            }
            p.out[0][i] = mul15(mul15(sat16(acc), norm_), level_);
        }
        if (scan_ < 0 || (scan_ >> 32) >= region) scan_ = ((scan_ % (region << 32)) + (region << 32)) % (region << 32);
    }

private:
    struct Grain { bool active = false; int64_t pos = 0, inc = 0; uint32_t age = 0, len = 1; };
    void spawn(int64_t region, uint32_t len, int64_t inc, int64_t scan) {
        int slot = -1;
        for (int k = 0; k < kGrains; k++) if (!grain_[k].active) { slot = k; break; }
        if (slot < 0) return;                                                                         // all busy: skip this grain
        const int64_t base = (static_cast<int64_t>(position_) * region >> 15) << 32;
        const int32_t rnd = static_cast<int32_t>(noise_.next());                                      // -32768..32767
        const int64_t jit = ((static_cast<int64_t>(rnd) * jitter_ >> 15) * region) << 16;               // +-jitter x half the region, Q32 frames
        Grain &g = grain_[slot];
        g.pos = base + scan + jit;
        g.inc = inc;
        g.age = 0;
        g.len = len < 16 ? 16 : len;
        g.active = true;
    }
    static constexpr int kGrains = 16;
    SampleBank *bank_ = nullptr;
    Grain grain_[kGrains];
    Noise noise_{0x6A5Fu};
    int64_t timer_ = 0, scan_ = 0;
    int32_t sample_ = -1, position_ = 0, speed_ = 256, size_ = 80, density_ = 20, pitch_ = 0, jitter_ = 4000;
    q15 level_ = kUnity, norm_ = 8000;
};

/* ------------------------------------------------------------------ Pads */

std::atomic<uint64_t> g_pads_playing{0};
std::atomic<int32_t> g_pads_pos[kPadCount];                                  // frame + 1 of each pad's sound, 0 = none

class Pads : public Module {
public:
    const ModuleInfo &info() const override {
        static const ModuleInfo i = {"Pads", Scope::Global, 0, 2 * kPadBuses, PADS_N, false, {}, {"L", "R", "AL", "AR", "BL", "BR", "CL", "CR"},
            {{"push", 0, 0, 0x7FFFFFFF}, {"release", 0, 0, 0x7FFFFFFF}, {"stop", 0, 0, 0x7FFFFFFF}}};
        return i;
    }
    bool init(Memory &m) override {
        mem_ = m;
        for (Voice &v : voice_) {
            Module *s = create_module<Sampler>(m);
            if (!s) return false;
            v.smp = static_cast<Sampler *>(s);
            v.smp->set_param(SMPR_TRACK, 0);                                     // fixed pitch: the pad's tune only
            v.smp->set_param(SMPR_LOOP, 0);                                      // the pad's Loop decides at every push
            v.smp->halt();
        }
        return true;
    }
    ~Pads() override {
        for (Voice &v : voice_) if (v.smp) destroy_module(*mem_.fast, v.smp);
        g_pads_playing.store(0, std::memory_order_relaxed);
    }
    void reset() override { for (Voice &v : voice_) free_voice(v); for (uint8_t &w : want_) w = 0; }
    void set_param(int idx, int32_t v) override {
        if (v <= 0) return;                                                      // 0: no event (what a rebuild re-applies)
        const int pad = v & 0xFF;
        if (idx == PADS_STOP) {
            for (Voice &x : voice_) if (x.pad >= 0 && !x.stopping) stop(x, pads_[x.pad].tail);
            for (uint8_t &w : want_) w = 0;
            return;
        }
        if (pad >= kPadCount) return;
        if (idx == PADS_PUSH) push(pad);
        else if (idx == PADS_RELEASE && pads_[pad].mode == PADM_GATE) {
            if (want_[pad]) want_[pad] = 0;                                      // released before its beat: it does not start
            else if (Voice *x = sounding(pad)) { stop(*x, pads_[pad].tail); follow(pad); }
        }
    }
    void set_blob(const void *data, size_t bytes) override {
        if (bytes == sizeof(PadSeqEvent)) {                                      // the sequencer: a push, or the pad let go
            PadSeqEvent e;
            std::memcpy(&e, data, sizeof e);
            if (e.pad >= kPadCount) return;
            if (!e.release) { want_[e.pad] = 0; start(e.pad, false, true, e.level); return; }
            if (pads_[e.pad].mode == PADM_ONESHOT) return;
            if (Voice *x = sounding(e.pad)) stop(*x, pads_[e.pad].tail);
            return;
        }
        if (bytes != sizeof(PadBlob)) return;
        PadBlob b;
        std::memcpy(&b, data, sizeof b);
        if (b.pad >= kPadCount) return;
        pads_[b.pad] = b;
        if (b.slot < 0) { for (Voice &x : voice_) if (x.pad == b.pad) cut(x); want_[b.pad] = 0; }    // the sample was taken away
    }
    void process(const ProcessCtx &ctx, const Ports &p) override {
        const int n = ctx.frames;
        start_due(ctx.transport, n);
        int32_t acc[2 * kPadBuses][kBlock];                                      // L / R of each bus
        bool used[kPadBuses] = {};
        q15 buf[kBlock];
        uint64_t playing = 0;
        int ended[kPadVoices], n_ended = 0;                                      // pads that reached their end: their Next starts after the loop
        Ports vp{};
        vp.in[0] = silence_;
        vp.out[0] = buf;
        ProcessCtx vc = ctx;
        for (Voice &v : voice_) {
            if (v.pad < 0) continue;
            const PadBlob &b = pads_[v.pad];
            vc.voice = &v.vs;
            v.smp->process(vc, vp);
            if (b.filter) {
                const SvfCoefF c{b.a1, b.a2, b.a3, b.k};
                for (int i = 0; i < n; i++) {
                    float lp, bp, hp;
                    svf_tick_f(c, v.svf, buf[i] * (1.0f / 32768.0f), lp, bp, hp);
                    buf[i] = svf_out_f(lp);
                }
            }
            // level / pan / mute: a ramp over the block to the pad's current gains; the envelope: fade-in after a trimmed start, fade-out after a stop
            const int bus = b.bus < kPadBuses ? b.bus : 0;
            int32_t *acc_l = acc[2 * bus], *acc_r = acc[2 * bus + 1];
            if (!used[bus]) { used[bus] = true; std::memset(acc_l, 0, sizeof(int32_t) * n); std::memset(acc_r, 0, sizeof(int32_t) * n); }
            const int32_t tl = b.mute ? 0 : (b.gain_l * v.lvl) >> 15, tr = b.mute ? 0 : (b.gain_r * v.lvl) >> 15;
            const int32_t dl = (tl - v.gl) / n, dr = (tr - v.gr) / n;
            int32_t gl = v.gl, gr = v.gr, env = v.env;
            for (int i = 0; i < n; i++) {
                gl = i == n - 1 ? tl : gl + dl;
                gr = i == n - 1 ? tr : gr + dr;
                env += v.env_step;
                if (env >= 32767) { env = 32767; if (v.env_step > 0) v.env_step = 0; }
                if (env < 0) env = 0;
                const int32_t x = (static_cast<int32_t>(buf[i]) * env) >> 15;
                acc_l[i] += (x * gl) >> 15;
                acc_r[i] += (x * gr) >> 15;
            }
            v.gl = tl; v.gr = tr; v.env = env;
            if (env == 0 && v.env_step < 0) { free_voice(v); continue; }        // faded out
            if (!v.smp->playing()) {                                             // reached the end of its sample
                if (!v.stopping) ended[n_ended++] = v.pad;
                free_voice(v);
                continue;
            }
            if (!v.stopping) { playing |= 1ull << v.pad; g_pads_pos[v.pad].store(static_cast<int32_t>(v.smp->position()) + 1, std::memory_order_relaxed); }
        }
        for (int i = 0; i < n_ended; i++) follow(ended[i]);
        for (int pad = 0; pad < kPadCount; pad++)
            if (!((playing >> pad) & 1)) g_pads_pos[pad].store(0, std::memory_order_relaxed);
        for (int bus = 0; bus < kPadBuses; bus++) {
            q15 *ol = p.out[2 * bus], *orr = p.out[2 * bus + 1];
            if (!used[bus]) { std::memset(ol, 0, sizeof(q15) * n); std::memset(orr, 0, sizeof(q15) * n); continue; }
            for (int i = 0; i < n; i++) { ol[i] = sat16(acc[2 * bus][i]); orr[i] = sat16(acc[2 * bus + 1][i]); }
        }
        g_pads_playing.store(playing, std::memory_order_relaxed);
    }

private:
    struct Voice {
        Sampler *smp = nullptr;
        int pad = -1;                                                            // -1 = free
        uint32_t order = 0;                                                      // start order (the oldest is stolen)
        bool stopping = false;                                                   // fading out: no longer the pad's sound
        int32_t env = 32767, env_step = 0;                                       // q15 envelope and its step per sample
        int32_t gl = 0, gr = 0;                                                  // the gains reached at the end of the last block
        int32_t lvl = 32768;                                                     // the push's level (Q15: 32768 = the pad's own; a sequenced hit: less)
        SvfStateF svf;
        VoiceState vs;
    };

    Voice *sounding(int pad) {
        for (Voice &v : voice_) if (v.pad == pad && !v.stopping) return &v;
        return nullptr;
    }
    void free_voice(Voice &v) { if (v.smp) v.smp->halt(); v.pad = -1; v.stopping = false; v.env_step = 0; }
    static int32_t fade_step(int32_t samples) { return -(32767 / (samples > 1 ? samples : 1)) - 1; }
    void cut(Voice &v) { if (v.pad >= 0) { v.stopping = true; v.env_step = fade_step(kPadCutMs * kSampleRate / 1000); } }
    void stop(Voice &v, int32_t tail) { v.stopping = true; v.env_step = fade_step(tail > 32 ? tail : 32); }

    // A pad ended by itself or was stopped by hand: its Next starts (as if pushed: a grid pad waits for its beat).
    void follow(int pad) {
        const int nx = pads_[pad].next;
        if (nx >= 0 && nx < kPadCount && nx != pad) push(nx);
    }

    void push(int pad) {
        const PadBlob &b = pads_[pad];
        if (b.slot < 0) return;
        if (b.grid || b.sync) {
            if (want_[pad]) { if (b.mode == PADM_TOGGLE) want_[pad] = 0; return; }   // waiting: a toggle pushed again cancels, the others wait on
            if (b.mode == PADM_TOGGLE) if (Voice *cur = sounding(pad)) { stop(*cur, b.tail); follow(pad); return; }
            want_[pad] = 1;                                                      // its beat is found at the next process() (the clock is there)
            return;
        }
        start(pad);
    }

    // The grid pads that wait: their beat / bar is the first one at or after the block they were pushed in; they start in the block nearest
    // to it. A stopped clock starts them at once.
    void start_due(const Transport *tr, int n) {
        for (int pad = 0; pad < kPadCount; pad++) {
            if (!want_[pad]) continue;
            if (!tr || !tr->running) { want_[pad] = 0; start(pad); continue; }
            if (want_[pad] == 1) {
                const PadBlob &b = pads_[pad];
                const uint64_t unit = static_cast<uint64_t>(b.grid ? b.grid : 4) << 32;           // Sync without a grid: the bar
                const uint64_t pre = b.sync ? static_cast<uint64_t>(b.preroll > 0 ? b.preroll : 0) * tr->inc_q32 : 0;   // beats before the anchor
                const uint64_t line = (tr->beat_q32 + pre + unit - 1) / unit * unit;              // the first grid line the anchor can reach
                at_[pad] = line - pre;
                want_[pad] = 2;
            }
            if (tr->sample_at(at_[pad], n) <= n / 2) { want_[pad] = 0; start(pad, pads_[pad].from && pads_[pad].loop); }
        }
    }

    void start(int pad, bool at_loop = false, bool seq = false, int level = 15) {
        const PadBlob &b = pads_[pad];
        if (b.slot < 0) return;
        if (Voice *cur = sounding(pad)) {
            if (b.mode == PADM_TOGGLE && !seq) { stop(*cur, b.tail); follow(pad); return; }   // a toggle pad: the push stops it (and starts its Next)
            cut(*cur);                                                           // mono: the new start replaces it
        }
        if (b.choke)
            for (Voice &v : voice_) if (v.pad >= 0 && v.pad != pad && !v.stopping && pads_[v.pad].choke == b.choke) cut(v);
        Voice *v = nullptr;
        for (Voice &x : voice_) if (x.pad < 0) { v = &x; break; }
        if (!v)                                                                  // all busy: the quietest fading voice, else the oldest
            for (Voice &x : voice_) {
                if (!v) { v = &x; continue; }
                const bool better = x.stopping != v->stopping ? x.stopping : (x.stopping ? x.env < v->env : x.order < v->order);
                if (better) v = &x;
            }
        free_voice(*v);
        v->pad = pad;
        v->order = ++order_;
        v->smp->set_param(SMPR_SAMPLE, b.slot);
        v->smp->set_param(SMPR_FSTART, at_loop ? b.lstart : b.start);          // a grid launch may start at the loop start
        v->smp->set_loop_extra(b.loop == 1 ? b.lstart : -1, b.xfade, b.xpost != 0);
        v->smp->set_param(SMPR_FEND, b.end);
        v->smp->set_param(SMPR_LOOP, b.loop == 2 ? -1 : 0);                     // the file's loop points, or none (then maybe the region)
        v->smp->set_param(SMPR_REGION, b.loop == 1 ? 1 : 0);
        v->smp->set_param(SMPR_TUNE, b.tune);
        v->smp->reset();                                                         // starts at its next process()
        v->svf = SvfStateF{};
        const bool mid = (at_loop ? b.lstart : b.start) > 0;
        v->env = mid ? 0 : 32767;                                                // a trimmed start may be mid-wave: 1 ms fade-in
        v->env_step = mid ? 32767 / (kSampleRate / 1000) + 1 : 0;
        v->lvl = (level < 0 ? 1 : (level > 15 ? 16 : level + 1)) * 2048;
        v->gl = b.mute ? 0 : (b.gain_l * v->lvl) >> 15;
        v->gr = b.mute ? 0 : (b.gain_r * v->lvl) >> 15;
        v->vs.note = 60; v->vs.velocity = 32767; v->vs.gate = true; v->vs.active = true;
    }

    Memory mem_;
    Voice voice_[kPadVoices];
    PadBlob pads_[kPadCount];
    uint8_t want_[kPadCount] = {};                                               // a grid pad waits: 1 = its beat is not known yet, 2 = at_
    uint64_t at_[kPadCount] = {};                                                // ... the beat (Q32) it starts on
    uint32_t order_ = 0;
    q15 silence_[kBlock] = {};
};

template <typename T>
ModuleType type_of() { static T probe; return {&probe.info(), &create_module<T>}; }

}  // namespace

uint64_t pads_playing() { return g_pads_playing.load(std::memory_order_relaxed); }
int32_t pads_position(int pad) { return pad >= 0 && pad < kPadCount ? g_pads_pos[pad].load(std::memory_order_relaxed) - 1 : -1; }

void register_sampler_modules(Registry &r) {
    r.add(T_SAMPLER, type_of<Sampler>());
    r.add(T_GRANULAR_V, type_of<Granular<Scope::Voice>>());
    r.add(T_GRANULAR_G, type_of<Granular<Scope::Global>>());
    r.add(T_PADS, type_of<Pads>());
}

}  // namespace sc
