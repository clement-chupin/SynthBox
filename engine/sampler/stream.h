#pragma once
// One streaming playback cursor (ADR-022). The audio thread owns the playhead; the loader thread keeps a small ring
// of 4 KB blocks filled ahead of it. The two sides only share atomics:
//   - the audio thread publishes (generation, sample, play frame, direction, rate, loop) with release stores;
//   - the loader stores a block into a ring slot, then publishes the slot's tag = (generation << 32 | block) with
//     a release store, so a block that was read for a previous generation can never match the current one.
// The audio thread never waits: a missing block is an underrun that the player fades over (see Sampler).
#include <atomic>
#include <cstdint>
#include "engine/dsp/q.h"
#include "engine/sampler/smp_format.h"

#ifndef ENGINE_RING_BLOCKS
#define ENGINE_RING_BLOCKS 12       // 48 KB per stream. The blocks wanted at any time are the current one, one for the interpolator,
                                    // and the path of the next kLookaheadBlocks blocks (<= kLookaheadBlocks + 4 when it wraps a loop).
                                    // 8 (3 ahead) underran at 8x pitch on the prototype: a block lasts 5.3 ms there, a slow card read 22 ms.
#endif

namespace sc {

constexpr int kRingBlocks = ENGINE_RING_BLOCKS;
constexpr int kLookaheadBlocks = kRingBlocks - 5;
static_assert(kRingBlocks >= 6, "the ring needs at least 6 blocks");

struct LoopInfo {
    uint32_t start = 0, end = 0;        // frames; end exclusive
    uint8_t mode = SMP_LOOP_OFF;
    bool active() const { return mode != SMP_LOOP_OFF && end > start + 1; }
};

// What the loader needs to know, copied consistently from the audio thread's published state.
struct StreamSnap {
    uint32_t gen = 0;
    int32_t sample = -1;
    uint32_t frames = 0;
    LoopInfo loop;
    uint32_t play_frame = 0;
    int32_t rate_q16 = 65536;           // |source frames per output sample| in Q16
    int32_t dir = 1;
};

class Stream {
public:
    Stream() { for (auto &t : tag_) t.store(0, std::memory_order_relaxed); }
    Stream(const Stream &) = delete;
    Stream &operator=(const Stream &) = delete;

    void attach(q15 *ring_memory) { ring_ = ring_memory; }      // kRingBlocks * kSmpBlockFrames samples
    q15 *ring() { return ring_; }
    bool attached() const { return ring_ != nullptr; }

    // [AUDIO] (re)start on a sample. Everything the loader reads is published before the generation changes.
    void start(int sample, uint32_t frames, uint32_t play_frame, int dir, int rate_q16, const LoopInfo &loop) {
        frames_ = frames;
        loop_ = loop;
        play_.store(play_frame, std::memory_order_relaxed);
        rate_.store(rate_q16, std::memory_order_relaxed);
        dir_.store(dir, std::memory_order_relaxed);
        sample_.store(sample, std::memory_order_relaxed);
        gen_.fetch_add(1, std::memory_order_release);
    }
    void stop() { sample_.store(-1, std::memory_order_relaxed); gen_.fetch_add(1, std::memory_order_release); }
    // [AUDIO] once per block
    void update(uint32_t play_frame, int dir, int rate_q16) {
        play_.store(play_frame, std::memory_order_relaxed);
        dir_.store(dir, std::memory_order_relaxed);
        rate_.store(rate_q16, std::memory_order_relaxed);
    }

    uint32_t generation() const { return gen_.load(std::memory_order_acquire); }

    // [AUDIO] the ring block `blk` of the current generation, or nullptr when it has not arrived. A block can sit in any ring slot
    // (the loader picks a free one), so the slots are searched by tag: a loop whose end blocks and start blocks would have shared
    // a slot under a fixed block % kRingBlocks placement can have both resident.
    const q15 *block(uint32_t gen, uint32_t blk) const {
        const uint64_t want = (static_cast<uint64_t>(gen) << 32) | blk | 0x8000000000000000ull;
        for (int s = 0; s < kRingBlocks; s++)
            if (tag_[s].load(std::memory_order_acquire) == want) return ring_ + static_cast<size_t>(s) * kSmpBlockFrames;
        return nullptr;
    }

    // [LOADER]
    bool snapshot(StreamSnap &o) const {
        const uint32_t g1 = gen_.load(std::memory_order_acquire);
        o.gen = g1;
        o.sample = sample_.load(std::memory_order_relaxed);
        o.frames = frames_;
        o.loop = loop_;
        o.play_frame = play_.load(std::memory_order_relaxed);
        o.rate_q16 = rate_.load(std::memory_order_relaxed);
        o.dir = dir_.load(std::memory_order_relaxed);
        return g1 == gen_.load(std::memory_order_acquire);
    }
    uint64_t slot_tag(int s) const { return tag_[s].load(std::memory_order_acquire); }
    void invalidate_slot(int s) { tag_[s].store(0, std::memory_order_release); }
    void publish_slot(int s, uint32_t gen, uint32_t blk) {
        tag_[s].store(make_tag(gen, blk), std::memory_order_release);
    }
    q15 *slot_memory(int s) { return ring_ + static_cast<size_t>(s) * kSmpBlockFrames; }

    // loader-private bookkeeping
    bool busy[kRingBlocks] = {};            // a read into this slot is in flight
    uint64_t fetching[kRingBlocks] = {};    // ... and the tag (generation, block) it will publish
    static uint64_t make_tag(uint32_t gen, uint32_t blk) { return (static_cast<uint64_t>(gen) << 32) | blk | 0x8000000000000000ull; }

private:
    q15 *ring_ = nullptr;
    std::atomic<uint32_t> gen_{0};
    std::atomic<int32_t> sample_{-1};
    std::atomic<uint32_t> play_{0};
    std::atomic<int32_t> rate_{65536}, dir_{1};
    std::atomic<uint64_t> tag_[kRingBlocks];
    uint32_t frames_ = 0;
    LoopInfo loop_;
};

// Moves a playhead position (frames, Q32) after a step, applying the loop rules. Returns false when a non-looping
// sample ran past its end (or its start when played backwards). `dir` flips at the ends of a ping-pong loop.
inline bool wrap_position(int64_t &pos_q32, int &dir, const LoopInfo &loop, uint32_t frames) {
    const int64_t end_q = static_cast<int64_t>(frames) << 32;
    if (loop.active()) {
        const int64_t ls = static_cast<int64_t>(loop.start) << 32, le = static_cast<int64_t>(loop.end) << 32;
        if (loop.mode == SMP_LOOP_FWD) {
            if (dir > 0 && pos_q32 >= le) pos_q32 -= le - ls;
            else if (dir < 0 && pos_q32 < ls && pos_q32 >= 0 && (pos_q32 + (le - ls)) < le) pos_q32 += le - ls;   // reverse playback loops the same region
        } else {
            if (dir > 0 && pos_q32 >= le) { pos_q32 = 2 * le - pos_q32; dir = -1; }
            else if (dir < 0 && pos_q32 < ls && pos_q32 >= ls - (le - ls)) { pos_q32 = 2 * ls - pos_q32; dir = 1; }
        }
    }
    return pos_q32 >= 0 && pos_q32 < end_q;
}

}  // namespace sc
