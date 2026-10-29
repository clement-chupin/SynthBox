#include "engine/sampler/sample_bank.h"
#include <cmath>
#include <cstring>

namespace sc {

namespace {
constexpr int64_t kFar = 1ll << 50;

// Moves along the playback path by `delta` frames (>= 0), following the loop rules. `wrapped` is set when the
// path jumped (loop wrap) or turned around (ping-pong) during the step.
void path_step(int64_t &f, int &dir, int64_t delta, const StreamSnap &s, bool &wrapped) {
    const int64_t frames = s.frames;
    const bool loop = s.loop.active();
    for (int guard = 0; delta > 0 && guard < 8; guard++) {
        if (dir > 0) {
            const int64_t limit = (loop && f < s.loop.end) ? s.loop.end : frames;
            const int64_t room = limit - f;
            if (delta < room) { f += delta; return; }
            delta -= room;
            if (loop && limit == s.loop.end) {
                wrapped = true;
                if (s.loop.mode == SMP_LOOP_FWD) f = s.loop.start;
                else { f = limit - 1; dir = -1; }
            } else { f = frames > 0 ? frames - 1 : 0; return; }
        } else {
            const int64_t limit = (loop && s.loop.mode == SMP_LOOP_PINGPONG && f > s.loop.start) ? s.loop.start :
                                  (loop && s.loop.mode == SMP_LOOP_FWD && f > s.loop.start) ? s.loop.start : 0;
            const int64_t room = f - limit;
            if (delta <= room) { f -= delta; return; }
            delta -= room;
            if (loop && limit == s.loop.start) {
                wrapped = true;
                if (s.loop.mode == SMP_LOOP_PINGPONG) { f = limit; dir = 1; }
                else f = s.loop.end > 0 ? s.loop.end - 1 : 0;
            } else { f = 0; return; }
        }
    }
}
}  // namespace

bool SampleBank::init(StorageDevice &dev, Memory &mem) {
    dev_ = &dev;
    mem_ = mem;
    return true;
}

void SampleBank::shutdown() {
    for (auto &s : slot_) { free_heads(s); s = SampleSlot{}; }
    if (scan_buf_) { mem_.bulk->free(scan_buf_); scan_buf_ = nullptr; }
    scan_slot_ = -1; scan_busy_ = false;
    // the loader's state too: a read still in flight on the old device would keep its job (and the queue "full") in the next bank
    for (auto &j : jobs_) j = Job{};
    jobs_in_flight_ = 0;
    for (auto &st : streams_) st = nullptr;
    for (auto &in : inst_) in = Instrument{};
    stats.underruns.store(0); stats.block_reads.store(0); stats.late_reads.store(0);
}

void SampleBank::free_heads(SampleSlot &s) {
    for (int i = 0; i < s.n_heads; i++) { mem_.bulk->free(s.head[i].data); s.head[i].data = nullptr; }
    s.n_heads = 0;
    if (s.hdr_tmp) { mem_.fast->free(s.hdr_tmp); s.hdr_tmp = nullptr; }
    if (s.onsets) { mem_.bulk->free(s.onsets); s.onsets = nullptr; }
    s.n_onsets = -1;
}

int SampleBank::alloc_job() {
    for (int i = 0; i < kMaxJobs; i++) if (jobs_[i].kind == J_FREE) return i;
    return -1;
}

int SampleBank::load(const char *name, int head_ms) {
    int id = -1;
    for (int i = 0; i < kMaxSamples; i++) if (slot_[i].state == SLOT_FREE) { id = i; break; }
    if (id < 0) return -1;
    SampleSlot &s = slot_[id];
    s = SampleSlot{};
    s.file = dev_->open(name);
    if (s.file < 0) return -1;
    s.hdr_tmp = static_cast<uint8_t *>(mem_.fast->alloc(kSmpBlockBytes));
    if (!s.hdr_tmp) { dev_->forget(s.file); return -1; }
    s.state = SLOT_HEADER;
    s.head_ms = static_cast<uint32_t>(head_ms);             // converted to blocks when the header is known
    s.next_issue = 0;
    return id;
}

void SampleBank::unload(int id) {
    if (id < 0 || id >= kMaxSamples) return;
    if (slot_[id].state != SLOT_FREE && slot_[id].file >= 0 && dev_) dev_->forget(slot_[id].file);
    free_heads(slot_[id]);
    slot_[id] = SampleSlot{};
}

bool SampleBank::can_unload(int id) const {
    if (id < 0 || id >= kMaxSamples) return false;
    const SampleSlot &s = slot_[id];
    if (s.state == SLOT_FREE) return true;
    if (s.state == SLOT_HEADER || s.state == SLOT_HEADS || s.pending > 0 || id == scan_slot_) return false;
    for (const Job &j : jobs_)
        if ((j.kind == J_HEADER || j.kind == J_HEAD || j.kind == J_SCAN) && j.slot == id) return false;
    for (const Stream *st : streams_) {
        StreamSnap sn;
        if (st && (!st->snapshot(sn) || sn.sample == id)) return false;     // (a stream that changed while it was read: ask again later)
    }
    return true;
}

/* ---------------- instruments ---------------- */

int SampleBank::add_instrument() {
    for (int i = 0; i < kMaxInstruments; i++) if (!inst_[i].used) { inst_[i] = Instrument{}; inst_[i].used = true; return i; }
    return -1;
}
bool SampleBank::add_zone(int inst, const Zone &z) {
    if (inst < 0 || inst >= kMaxInstruments || !inst_[inst].used || inst_[inst].n >= kMaxZones) return false;
    inst_[inst].zone[inst_[inst].n++] = z;
    return true;
}
const Zone *SampleBank::pick(int inst, int note, int vel) const {
    if (inst < 0 || inst >= kMaxInstruments || !inst_[inst].used) return nullptr;
    for (int i = 0; i < inst_[inst].n; i++) {
        const Zone &z = inst_[inst].zone[i];
        if (note >= z.lo_note && note <= z.hi_note && vel >= z.lo_vel && vel <= z.hi_vel) return &z;
    }
    return nullptr;
}

/* ---------------- streams ---------------- */

void SampleBank::add_stream(Stream *s) {
    for (auto &p : streams_) if (!p) { p = s; return; }
}
void SampleBank::remove_stream(Stream *s) {
    for (auto &p : streams_) if (p == s) p = nullptr;
}

/* ---------------- loader ---------------- */

void SampleBank::finish_slot_heads(SampleSlot &s) {
    if (s.hdr_tmp) { mem_.fast->free(s.hdr_tmp); s.hdr_tmp = nullptr; }
    s.state = SLOT_READY;
}

void SampleBank::complete(const IoDone &d) {
    if (d.tag >= static_cast<uint32_t>(kMaxJobs) || jobs_[d.tag].kind == J_FREE) return;
    Job j = jobs_[d.tag];
    jobs_[d.tag].kind = J_FREE;
    jobs_in_flight_--;
    switch (j.kind) {
    case J_HEADER: {
        SampleSlot &s = slot_[j.slot];
        if (s.state != SLOT_HEADER) break;
        SmpHeader h;
        if (!d.ok || !smp_parse_header(s.hdr_tmp, kSmpBlockBytes, h)) { s.state = SLOT_FAILED; break; }
        s.h = h;
        const uint32_t head_ms = s.head_ms;
        const uint32_t nblocks = h.blocks();
        s.resident = head_ms == 0;
        auto blocks_for = [&](uint32_t ms) { return (static_cast<uint64_t>(ms) * h.sample_rate / 1000 + kSmpBlockFrames - 1) / kSmpBlockFrames; };
        uint32_t main_blocks = s.resident ? nblocks : static_cast<uint32_t>(blocks_for(head_ms));
        if (main_blocks < 1) main_blocks = 1;
        if (main_blocks >= nblocks) { main_blocks = nblocks; s.resident = true; }
        s.n_heads = 0;
        auto add_head = [&](uint32_t first_block, uint32_t count) {
            if (first_block >= nblocks) return;
            if (first_block + count > nblocks) count = nblocks - first_block;
            Head &hd = s.head[s.n_heads];
            hd.start_frame = first_block * kSmpBlockFrames;
            hd.frames = count * kSmpBlockFrames;
            hd.data = static_cast<q15 *>(mem_.bulk->alloc(static_cast<size_t>(count) * kSmpBlockBytes));
            if (hd.data) s.n_heads++;
        };
        add_head(0, main_blocks);
        if (!s.resident) {
            const uint32_t sl_blocks = static_cast<uint32_t>(blocks_for(head_ms / 3 + 1)) + 1;
            for (int i = 0; i < h.slice_count; i++) add_head(h.slice[i] / kSmpBlockFrames, sl_blocks);
            if (nblocks > sl_blocks) add_head(nblocks - sl_blocks, sl_blocks);
        }
        if (s.n_heads == 0) { s.state = SLOT_FAILED; break; }
        s.pending = s.n_heads;
        s.next_issue = 0;
        s.state = SLOT_HEADS;
        break;
    }
    case J_HEAD: {
        SampleSlot &s = slot_[j.slot];
        if (s.state != SLOT_HEADS) break;
        if (!d.ok) { s.state = SLOT_FAILED; break; }
        if (--s.pending == 0) finish_slot_heads(s);
        break;
    }
    case J_SCAN:
        scan_busy_ = false;
        if (j.slot != scan_slot_) break;
        if (d.ok) { scan_feed(scan_buf_, kSmpBlockFrames); scan_blk_++; }
        else scan_finish();                                                          // a read failed: what was found so far
        break;
    case J_BLOCK: {
        Stream *st = streams_[j.stream];
        if (!st) break;
        st->busy[j.ring_slot] = false;
        StreamSnap sn;
        if (d.ok && st->snapshot(sn) && sn.gen == j.gen && sn.sample == j.sample) {
            st->publish_slot(j.ring_slot, j.gen, j.blk);
            stats.block_reads.fetch_add(1, std::memory_order_relaxed);
        }
        break;
    }
    default: break;
    }
}

void SampleBank::issue_admin(uint64_t now_us) {
    for (int i = 0; i < kMaxSamples; i++) {
        SampleSlot &s = slot_[i];
        if (s.state == SLOT_HEADER && s.next_issue == 0) {
            const int jid = alloc_job();
            if (jid < 0 || jobs_in_flight_ >= kQueueDepth + 2) return;
            IoRead r{s.file, 0, kSmpBlockBytes, s.hdr_tmp, static_cast<uint32_t>(jid)};
            r.pinned = true;
            if (!dev_->submit(r, now_us)) return;
            jobs_[jid] = Job{J_HEADER, static_cast<int16_t>(i), 0, 0, 0, 0, 0, 0};
            jobs_in_flight_++;
            s.next_issue = 1;
        } else if (s.state == SLOT_HEADS) {
            while (s.next_issue < s.n_heads) {
                const int jid = alloc_job();
                if (jid < 0 || jobs_in_flight_ >= kQueueDepth + 2) return;
                const Head &hd = s.head[s.next_issue];
                IoRead r{s.file, (hd.start_frame / kSmpBlockFrames + 1) * kSmpBlockBytes, hd.frames / kSmpBlockFrames * kSmpBlockBytes, hd.data, static_cast<uint32_t>(jid)};
                r.pinned = true;
                if (!dev_->submit(r, now_us)) return;
                jobs_[jid] = Job{J_HEAD, static_cast<int16_t>(i), 0, 0, static_cast<int16_t>(s.next_issue), 0, 0, 0};
                jobs_in_flight_++;
                s.next_issue++;
            }
        }
    }
}

// The blocks a stream will need, in path order, with the time (in output samples) until each is needed.
int SampleBank::desired_blocks(const StreamSnap &sn, int64_t blk[], int64_t until[]) const {
    int n = 0;
    auto add = [&](int64_t b, int64_t u) {
        if (b < 0) return;
        for (int i = 0; i < n; i++) if (blk[i] == b) { if (u < until[i]) until[i] = u; return; }
        blk[n] = b; until[n] = u; n++;
    };
    int64_t f = sn.play_frame;
    int dir = sn.dir;
    const int64_t rate = sn.rate_q16 > 64 ? sn.rate_q16 : 64;
    const int64_t cur = f / kSmpBlockFrames, off = f % kSmpBlockFrames;
    add(cur, 0);
    if (dir > 0 && off < 4) add(cur - 1, 0);                                         // the interpolator looks 1 frame back (forward) or ahead (reverse)
    if (dir < 0 && off > static_cast<int64_t>(kSmpBlockFrames) - 4) add(cur + 1, 0);
    // walk the path in half-block steps so that no block it touches is skipped; remember where it wrapped
    const int64_t step = kSmpBlockFrames / 2;
    for (int j = 1; j <= kLookaheadBlocks * 2; j++) {
        bool wrapped = false;
        path_step(f, dir, step, sn, wrapped);
        const int64_t u = (static_cast<int64_t>(j) * step << 16) / rate;
        add(f / kSmpBlockFrames, u);
        if (wrapped) {                                                               // both ends of the loop are about to be needed
            add(static_cast<int64_t>(sn.loop.start) / kSmpBlockFrames, u);
            add((static_cast<int64_t>(sn.loop.end) - 1) / kSmpBlockFrames, u);
        }
    }
    return n;
}

void SampleBank::issue_streams(uint64_t now_us) {
    while (jobs_in_flight_ < kQueueDepth) {
        // best candidate over all streams: the block with the smallest deadline that is neither present nor on its way
        int best_stream = -1, best_ring = 0;
        int64_t best_blk = 0, best_until = kFar;
        StreamSnap best_snap;
        for (int si = 0; si < kMaxStreams; si++) {
            Stream *st = streams_[si];
            if (!st || !st->attached()) continue;
            StreamSnap sn;
            if (!st->snapshot(sn) || sn.sample < 0 || sn.sample >= kMaxSamples) continue;
            const SampleSlot &sl = slot_[sn.sample];
            if (sl.state != SLOT_READY || sl.resident) continue;
            int64_t blk[kRingBlocks + 4], until[kRingBlocks + 4];
            const int n = desired_blocks(sn, blk, until);
            const int64_t nblocks = sl.h.blocks();
            // the ring's tags, read once: each is a 64-bit atomic (a locked library call on the ESP32), and the matching below looks at every
            // slot for every wanted block. Reading them inside the loops cost milliseconds per issued read with a few voices.
            uint64_t tag[kRingBlocks];
            for (int s = 0; s < kRingBlocks; s++) tag[s] = st->slot_tag(s);
            for (int i = 0; i < n; i++) {
                const int64_t b = blk[i];
                if (b >= nblocks) continue;
                bool in_head = false;                                                 // heads are block aligned
                for (int h = 0; h < sl.n_heads && !in_head; h++) {
                    const int64_t lo = sl.head[h].start_frame / kSmpBlockFrames, cnt = sl.head[h].frames / kSmpBlockFrames;
                    in_head = b >= lo && b < lo + cnt;
                }
                if (in_head) continue;
                const uint64_t want = Stream::make_tag(sn.gen, static_cast<uint32_t>(b));
                bool have = false;                                                    // already there, or on its way
                for (int s = 0; s < kRingBlocks && !have; s++) have = tag[s] == want || (st->busy[s] && st->fetching[s] == want);
                if (have) continue;
                // any slot that is not being read and does not hold a block that is still wanted (empty ones and stale generations first)
                int rs = -1;
                for (int s = 0; s < kRingBlocks; s++) {
                    if (st->busy[s]) continue;
                    const uint64_t cur = tag[s];
                    bool wanted = false;
                    if (cur) {
                        const uint32_t cb = static_cast<uint32_t>(cur & 0xFFFFFFFFu), cg = static_cast<uint32_t>((cur >> 32) & 0x7FFFFFFFu);
                        if (cg == (sn.gen & 0x7FFFFFFFu)) for (int k = 0; k < n; k++) if (blk[k] == cb) wanted = true;
                    }
                    if (wanted) continue;
                    if (rs < 0 || cur == 0) rs = s;
                    if (cur == 0) break;
                }
                if (rs < 0) continue;                                                 // every slot holds a wanted block: nothing can be placed now
                if (until[i] < best_until) { best_until = until[i]; best_stream = si; best_ring = rs; best_blk = b; best_snap = sn; }
            }
        }
        if (best_stream < 0) return;
        Stream *st = streams_[best_stream];
        const int jid = alloc_job();
        if (jid < 0) return;
        const SampleSlot &sl = slot_[best_snap.sample];
        st->invalidate_slot(best_ring);
        IoRead r{sl.file, static_cast<uint32_t>((best_blk + 1) * kSmpBlockBytes), kSmpBlockBytes, st->slot_memory(best_ring), static_cast<uint32_t>(jid)};
        if (!dev_->submit(r, now_us)) return;
        st->busy[best_ring] = true;
        st->fetching[best_ring] = Stream::make_tag(best_snap.gen, static_cast<uint32_t>(best_blk));
        jobs_[jid] = Job{J_BLOCK, 0, static_cast<int16_t>(best_stream), static_cast<int16_t>(best_ring), 0, best_snap.gen, static_cast<uint32_t>(best_blk), best_snap.sample};
        jobs_in_flight_++;
    }
}

/* ---------------- zero crossings ---------------- */

int64_t SampleBank::zero_near(int id, uint32_t frame, int dir) const {
    if (id < 0 || id >= kMaxSamples || slot_[id].state != SLOT_READY) return -1;
    const SampleSlot &s = slot_[id];
    for (int h = 0; h < s.n_heads; h++) {
        const Head &hd = s.head[h];
        if (frame < hd.start_frame || frame >= hd.start_frame + hd.frames) continue;
        const q15 *d = hd.data;
        const int64_t lo = hd.start_frame + 1, hi = static_cast<int64_t>(hd.start_frame) + hd.frames - 1;
        auto cross = [&](int64_t f) { return f >= lo && f <= hi && ((d[f - 1 - hd.start_frame] < 0) != (d[f - hd.start_frame] < 0)); };
        for (int64_t k = 0; k <= 2048; k++) {
            if (dir >= 0 && cross(static_cast<int64_t>(frame) + k + (dir > 0))) return static_cast<int64_t>(frame) + k + (dir > 0);
            if (dir <= 0 && cross(static_cast<int64_t>(frame) - k - (dir < 0))) return static_cast<int64_t>(frame) - k - (dir < 0);
        }
        return -1;
    }
    return -1;
}

/* ---------------- the onset scan ---------------- */

void SampleBank::scan(int id) {
    if (id < 0 || id >= kMaxSamples || slot_[id].state == SLOT_FREE) return;
    SampleSlot &s = slot_[id];
    if (s.n_onsets < 0 && id != scan_slot_) s.scan_want = true;
}

int SampleBank::onsets(int id, Onset *out, int max) const {
    if (id < 0 || id >= kMaxSamples) return 0;
    const SampleSlot &s = slot_[id];
    if (s.state == SLOT_FAILED) return 0;
    if (s.n_onsets < 0) return -1;
    const int n = s.n_onsets < max ? s.n_onsets : max;
    for (int i = 0; i < n; i++) out[i] = s.onsets[i];
    return n;
}

// The detection function: the rise of the log energy from one 256-frame window to the next. A rise of more than 3 dB is a candidate; of
// the candidates closer than 40 ms only the strongest stays; the kMaxOnsets strongest are kept.
void SampleBank::scan_feed(const q15 *data, uint32_t frames) {
    constexpr uint32_t kHop = 256;
    const uint32_t gap = slot_[scan_slot_].h.sample_rate / 25;                       // 40 ms
    auto keep = [&](const Onset &o) {
        if (scan_count_ < kMaxOnsets) { scan_found_[scan_count_++] = o; return; }
        int weakest = 0;
        for (int i = 1; i < kMaxOnsets; i++) if (scan_found_[i].strength < scan_found_[weakest].strength) weakest = i;
        if (o.strength > scan_found_[weakest].strength) scan_found_[weakest] = o;
    };
    for (uint32_t i = 0; i < frames; i++) {
        scan_acc_ += static_cast<int32_t>(data[i]) * data[i];
        if (++scan_n_ < kHop) continue;
        const float le = std::log10(static_cast<float>(scan_acc_) / kHop + 1.0f);
        const float rise = le - scan_prev_;
        scan_prev_ = le;
        if (rise > 0.3f) {
            const Onset o{scan_frame_, static_cast<uint16_t>(rise * 1000.0f > 65535.0f ? 65535.0f : rise * 1000.0f)};
            if (scan_pend_.strength && o.frame - scan_pend_.frame < gap) { if (o.strength > scan_pend_.strength) scan_pend_ = o; }
            else { if (scan_pend_.strength) keep(scan_pend_); scan_pend_ = o; }
        }
        scan_acc_ = 0; scan_n_ = 0; scan_frame_ += kHop;
    }
}

void SampleBank::scan_finish() {
    SampleSlot &s = slot_[scan_slot_];
    if (scan_pend_.strength) {                                                       // the last candidate
        if (scan_count_ < kMaxOnsets) scan_found_[scan_count_++] = scan_pend_;
        scan_pend_ = Onset{};
    }
    for (int i = 1; i < scan_count_; i++)                                            // by frame
        for (int k = i; k > 0 && scan_found_[k].frame < scan_found_[k - 1].frame; k--) {
            const Onset t = scan_found_[k]; scan_found_[k] = scan_found_[k - 1]; scan_found_[k - 1] = t;
        }
    s.onsets = scan_count_ ? static_cast<Onset *>(mem_.bulk->alloc(sizeof(Onset) * static_cast<size_t>(scan_count_))) : nullptr;
    const int n = s.onsets ? scan_count_ : 0;
    for (int i = 0; i < n; i++) s.onsets[i] = scan_found_[i];
    s.n_onsets = static_cast<int16_t>(n);
    scan_slot_ = -1;
}

// One step of the scan when the streams leave room: blocks held in memory (heads) at once, the others read from the card.
void SampleBank::issue_scan(uint64_t now_us) {
    if (scan_busy_) return;
    if (scan_slot_ < 0) {
        for (int i = 0; i < kMaxSamples && scan_slot_ < 0; i++)
            if (slot_[i].scan_want && slot_[i].state == SLOT_READY) {
                slot_[i].scan_want = false;
                scan_slot_ = i; scan_blk_ = 0; scan_acc_ = 0; scan_n_ = 0; scan_frame_ = 0; scan_prev_ = 0.0f; scan_pend_ = Onset{}; scan_count_ = 0;
            }
        if (scan_slot_ < 0) return;
    }
    const SampleSlot &s = slot_[scan_slot_];
    const uint32_t nblocks = s.h.blocks();
    for (int guard = 0; guard < 16; guard++) {                                       // at most 16 blocks from memory per pump
        if (scan_blk_ >= nblocks) { scan_finish(); return; }
        const uint32_t first = scan_blk_ * kSmpBlockFrames;
        const uint32_t frames = s.h.frames - first < kSmpBlockFrames ? s.h.frames - first : kSmpBlockFrames;
        const q15 *mem = nullptr;
        for (int h = 0; h < s.n_heads && !mem; h++)
            if (first >= s.head[h].start_frame && first + frames <= s.head[h].start_frame + s.head[h].frames) mem = s.head[h].data + (first - s.head[h].start_frame);
        if (!mem) break;
        scan_feed(mem, frames);
        scan_blk_++;
    }
    if (scan_blk_ >= nblocks) { scan_finish(); return; }
    if (jobs_in_flight_ >= kQueueDepth) return;                                     // the streams first
    if (!scan_buf_) scan_buf_ = static_cast<q15 *>(mem_.bulk->alloc(kSmpBlockBytes));
    const int jid = alloc_job();
    if (!scan_buf_ || jid < 0) return;
    IoRead r{s.file, (scan_blk_ + 1) * kSmpBlockBytes, kSmpBlockBytes, scan_buf_, static_cast<uint32_t>(jid)};
    if (!dev_->submit(r, now_us)) return;
    jobs_[jid] = Job{J_SCAN, static_cast<int16_t>(scan_slot_), 0, 0, 0, 0, scan_blk_, 0};
    jobs_in_flight_++;
    scan_busy_ = true;
}

void SampleBank::pump(uint64_t now_us) {
    IoDone d;
    while (dev_->poll(d, now_us)) complete(d);
    issue_admin(now_us);
    issue_streams(now_us);
    issue_scan(now_us);
}

}  // namespace sc
