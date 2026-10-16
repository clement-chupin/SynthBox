#pragma once
// Sample bank + background loader (ADR-022).
//
//  * load() opens a .smp file and brings in its RAM heads: the start of the sample (default 150 ms) and a short head
//    for every slice, so a note-on or a slice trigger starts at once whatever the card latency is. head_ms = 0 makes
//    the whole sample resident (granular playback, short one-shots need no streaming).
//  * Every Sampler voice owns a Stream (ring of blocks). pump() keeps the rings filled ahead of the playheads with
//    a small number of outstanding reads, always serving the stream that will run out first (earliest deadline).
//  * pump() is the loader's body: call it from the I/O task (ESP32, other core) or from the test loop. It only
//    touches atomics shared with the audio thread (see stream.h); load / unload / add_zone are control-time.
#include <atomic>
#include <cstdint>
#include "engine/core/module.h"
#include "engine/sampler/smp_format.h"
#include "engine/sampler/storage.h"
#include "engine/sampler/stream.h"

namespace sc {

enum SlotState : uint8_t { SLOT_FREE, SLOT_HEADER, SLOT_HEADS, SLOT_READY, SLOT_FAILED };

struct Head {
    uint32_t start_frame = 0;       // block aligned
    uint32_t frames = 0;
    q15 *data = nullptr;
};

// An attack found by the onset scan (SampleBank::scan): the frame where the energy rises and how much (log10 of the energy ratio x 1000).
struct Onset { uint32_t frame = 0; uint16_t strength = 0; };
constexpr int kMaxOnsets = 64;

struct SampleSlot {
    SlotState state = SLOT_FREE;
    Onset *onsets = nullptr;        // the scan's result (bulk heap), n_onsets of them; n_onsets -1 = not scanned, scan_want = asked for
    int16_t n_onsets = -1;
    bool scan_want = false;
    FileHandle file = -1;
    SmpHeader h;
    Head head[2 + kSmpMaxSlices];            // start, one per slice, and the tail (reverse playback starts there)
    int n_heads = 0;
    bool resident = false;          // the whole sample is in head[0]
    uint32_t head_ms = 0;           // requested head length (0 = resident)
    int next_issue = 0, pending = 0;
    uint8_t *hdr_tmp = nullptr;
};

struct Zone {
    uint8_t lo_note = 0, hi_note = 127, lo_vel = 1, hi_vel = 127;
    int16_t sample = -1;
    int16_t root = -1;              // MIDI note, -1 = from the sample header
    int16_t tune_cents = 0;
    q15 gain = kUnity;
};

struct SamplerStats {
    std::atomic<uint32_t> underruns{0};     // playheads that found a block missing
    std::atomic<uint32_t> block_reads{0};
    std::atomic<uint32_t> late_reads{0};    // blocks that arrived after the playhead needed them
};

class SampleBank {
public:
    static constexpr int kMaxSamples = 32;
    static constexpr int kMaxStreams = 32;
    static constexpr int kMaxJobs = 64;
    static constexpr int kMaxInstruments = 8;
    static constexpr int kMaxZones = 64;
    static constexpr int kQueueDepth = 2;    // outstanding reads

    bool init(StorageDevice &dev, Memory &mem);
    void shutdown();

    // --- control time ---
    int load(const char *name, int head_ms = 150);          // slot id or -1; the heads arrive through pump()
    // The onset scan (the pads' transient chop): pump() reads the whole sample, one block at a time when the streams leave room (a resident
    // one from memory), and keeps its kMaxOnsets strongest attacks. scan() asks for it (once per slot); onsets() copies the result sorted by
    // frame and returns how many, or -1 while it is not done (a failed sample: 0).
    void scan(int id);
    int onsets(int id, Onset *out, int max) const;
    // The zero crossing (a sign change between two frames) nearest to `frame` (dir 0), or the first one after (+1) / before (-1) it, within
    // 2048 frames, in the sample's data held in memory (its heads; a resident sample: all of it). -1 when there is none there.
    int64_t zero_near(int id, uint32_t frame, int dir) const;
    bool ready(int id) const { return id >= 0 && id < kMaxSamples && slot_[id].state == SLOT_READY; }
    bool failed(int id) const { return id >= 0 && id < kMaxSamples && slot_[id].state == SLOT_FAILED; }
    const SampleSlot *slot(int id) const { return id >= 0 && id < kMaxSamples ? &slot_[id] : nullptr; }
    void unload(int id);
    // True when nothing reads the sample any more: it is loaded (or failed), no head read is in flight, the onset scan is not on it and
    // no stream plays it. Only then may it be unloaded (the caller also makes sure no voice can start on it: engine_synth's catalog).
    bool can_unload(int id) const;
    int add_instrument();
    bool add_zone(int inst, const Zone &z);
    const Zone *pick(int inst, int note, int velocity127) const;

    // --- loader ---
    void pump(uint64_t now_us);
    bool idle() const { return jobs_in_flight_ == 0; }
    void add_stream(Stream *s);
    void remove_stream(Stream *s);
    SamplerStats stats;

    Memory &memory() { return mem_; }

private:
    enum JobKind : uint8_t { J_FREE, J_HEADER, J_HEAD, J_BLOCK, J_SCAN };
    struct Job { JobKind kind = J_FREE; int16_t slot = 0, stream = 0, ring_slot = 0, head = 0; uint32_t gen = 0, blk = 0; int sample = 0; };
    struct Instrument { Zone zone[kMaxZones]; int n = 0; bool used = false; };

    int alloc_job();
    void complete(const IoDone &d);
    void issue_admin(uint64_t now_us);
    void issue_streams(uint64_t now_us);
    void issue_scan(uint64_t now_us);
    void scan_feed(const q15 *data, uint32_t frames);       // the detector, block by block
    void scan_finish();
    // the scan in progress (one at a time)
    int scan_slot_ = -1;
    uint32_t scan_blk_ = 0;
    bool scan_busy_ = false;
    q15 *scan_buf_ = nullptr;
    int64_t scan_acc_ = 0;                                  // energy of the window being summed
    uint32_t scan_n_ = 0, scan_frame_ = 0;                  // its frames so far, and the frame where it starts
    float scan_prev_ = 0.0f;                                // the previous window's log energy
    Onset scan_pend_;                                       // the candidate waiting for a stronger neighbour (strength 0: none)
    Onset scan_found_[kMaxOnsets];
    int scan_count_ = 0;
    void finish_slot_heads(SampleSlot &s);
    void free_heads(SampleSlot &s);
    int desired_blocks(const StreamSnap &sn, int64_t out_blk[], int64_t out_until[]) const;

    StorageDevice *dev_ = nullptr;
    Memory mem_;
    SampleSlot slot_[kMaxSamples];
    Stream *streams_[kMaxStreams] = {};
    Job jobs_[kMaxJobs];
    int jobs_in_flight_ = 0;
    Instrument inst_[kMaxInstruments];
};

}  // namespace sc
