#include "engine/core/sequencer.h"

namespace sc {

int seq_steps_per_beat(int rate) {
    static const uint8_t spb[SEQ_RATES] = {2, 3, 4, 6, 8};
    return spb[rate >= 0 && rate < SEQ_RATES ? rate : SEQ_RATE_16];
}

uint64_t seq_slot_time(uint64_t base, uint32_t k, int steps_per_beat, int swing) {
    const uint64_t spb = static_cast<uint64_t>(steps_per_beat);
    uint64_t t = base + (static_cast<uint64_t>(k) << 32) / spb;
    if (k & 1u) t += ((1ull << 32) / spb) * static_cast<uint64_t>(swing < 0 ? 0 : swing) / 100;
    return t;
}

/* ---------------- what the sequencers share ---------------- */

void StepSeq::reset() {
    set_ = SeqSetup{};
    next_set_ = SeqSetup{};
    queued_ = false; played_ = 0; taken_ = 0;
    playing_ = false; first_ = true; rewind_ = false;
    cur_ = 0; bounce_ = 1; slot_ = 0; base_ = 0; step_at_ = next_at_ = 0;
    rng_ = 0x2545F491u;
    pos_ = SeqPos{};
    frac_ = 0;
}

void StepSeq::setup(const SeqSetup &s) {
    if (s.queue == SEQ_UNQUEUE) { queued_ = false; return; }
    if (s.queue == SEQ_QUEUE) { next_set_ = s; queued_ = true; return; }
    if (playing_ && !first_ && s.rate != set_.rate) {   // another rate while it plays: the next step keeps its time, the new rate counts from it
        base_ = next_at_;
        slot_ = 0;
    }
    set_ = s;
}

// The next step is due at t: which one it is (the direction; the queued pattern when the playing one has played its steps), then it plays.
void StepSeq::step_due(uint64_t t) {
    if (!first_ && played_ >= steps()) {                // the pattern has played all its steps: again, or on to the queued one
        played_ = 0;
        if (queued_) {
            const uint8_t run = set_.run;
            set_ = next_set_;
            set_.run = run;
            set_.queue = SEQ_LIVE;
            take();
            queued_ = false;
            taken_ = ((taken_ >> 8) + 1) << 8 | next_set_.id;
            first_ = true;                              // it starts at its first step, its time counted from here
            base_ = t;
            slot_ = 0;
        }
    }
    played_++;
    const int n = steps();
    if (first_) {
        first_ = false;
        bounce_ = 1;
        cur_ = set_.dir == SEQ_REVERSE ? n - 1 : (set_.dir == SEQ_RANDOM ? static_cast<int>(rnd() % static_cast<uint32_t>(n)) : 0);
    } else {
        switch (set_.dir) {
            case SEQ_REVERSE: cur_ = cur_ < 1 || cur_ > n ? n - 1 : cur_ - 1; break;     // (cur_ > n: the pattern got shorter)
            case SEQ_PINGPONG:
                if (n < 2) { cur_ = 0; break; }
                if (cur_ >= n - 1) bounce_ = -1; else if (cur_ <= 0) bounce_ = 1;
                cur_ += bounce_;
                if (cur_ > n - 1) cur_ = n - 1;
                break;
            case SEQ_RANDOM: cur_ = static_cast<int>(rnd() % static_cast<uint32_t>(n)); break;
            default: cur_ = cur_ + 1 >= n ? 0 : cur_ + 1; break;
        }
    }
    step_at_ = t;
    const uint32_t slot = slot_++;
    next_at_ = slot_time(slot_);
    play(t, slot);
}

void StepSeq::run(const Transport &tr, int frames) {
    if (rewind_ && playing_) { stop_all(); playing_ = false; }
    rewind_ = false;
    if (!tr.running || !set_.run) {
        if (playing_) { stop_all(); playing_ = false; }
        pos_.running = false;
        return;
    }
    if (!playing_) {                                    // a run starts: on the next bar line (beat 0 when the clock starts with it)
        const uint64_t bar = 4ull << 32;
        playing_ = true; first_ = true;
        base_ = (tr.beat_q32 + bar - 1) / bar * bar;
        slot_ = 0;
        played_ = 0;
    }
    const uint64_t lim = tr.beat_q32 + tr.inc_q32 * static_cast<uint64_t>(frames / 2);   // what is due before the middle of the block starts with it
    while (room()) {
        // the earliest thing due: what the pattern still owes, then the next step (in this order when they share an instant)
        uint64_t t_pend = 0;
        const bool pend = pending(&t_pend);
        const uint64_t t_step = slot_time(slot_);
        if (pend && t_pend <= lim && t_pend <= t_step) fire();
        else if (t_step <= lim) step_due(t_step);
        else break;
    }
    // where the block ends: the step and how far into it (going backwards the fraction runs down, so a lane that follows it is continuous)
    pos_.running = !first_;
    pos_.steps = static_cast<uint8_t>(steps());
    if (!first_) {
        const uint64_t end = tr.beat_q32 + tr.inc_q32 * static_cast<uint64_t>(frames);
        const uint64_t len = next_at_ > step_at_ ? next_at_ - step_at_ : 1;
        uint64_t f = end > step_at_ ? ((end - step_at_) << 16) / len : 0;
        if (f > 0xFFFF) f = 0xFFFF;
        frac_ = static_cast<uint32_t>(f);
        const bool back = set_.dir == SEQ_REVERSE || (set_.dir == SEQ_PINGPONG && bounce_ < 0);
        pos_.pos_q16 = (static_cast<uint32_t>(cur_) << 16) | static_cast<uint32_t>(back ? 0xFFFF - f : f);
    }
}

/* ---------------- notes ---------------- */

void Sequencer::clear() {
    reset();
    for (SeqStep &s : step_) s = SeqStep{};
    for (SeqStep &s : next_step_) s = SeqStep{};
    for (Held &h : held_) h = Held{};
    rt_left_ = 0;
}

void Sequencer::chunk(const SeqChunk &c) {
    SeqStep *to = c.queue ? next_step_ : step_;
    for (int i = 0; i < c.count && i < kSeqChunk && c.first + i < kSeqSteps; i++) to[c.first + i] = c.step[i];
}

void Sequencer::take() { for (int i = 0; i < kSeqSteps; i++) step_[i] = next_step_[i]; }

void Sequencer::emit(bool on, int note, int vel) {
    if (n_ >= max_) return;
    out_[n_++] = SeqEvent{static_cast<uint8_t>(on ? 1 : 0), static_cast<uint8_t>(note), static_cast<uint8_t>(vel)};
}

void Sequencer::stop_all() {
    for (Held &h : held_) {
        if (h.note) emit(false, h.note, 0);
        h = Held{};
    }
    rt_left_ = 0;
}

// One hit of a step: its notes start, each held until off_at.
void Sequencer::hit(const SeqStep &s, uint64_t off_at) {
    for (int k = 0; k < kSeqChord; k++) {
        if (!s.note[k]) continue;
        bool twice = false;
        for (int j = 0; j < k; j++) twice |= s.note[j] == s.note[k];
        if (twice) continue;
        int note = s.note[k] + set_.transpose;
        note = note < 1 ? 1 : (note > 127 ? 127 : note);
        int slot = -1, first_off = 0;
        for (int i = 0; i < kSeqHeld; i++) {
            if (held_[i].note == note) { emit(false, note, 0); held_[i].note = 0; }      // the same pitch is retriggered
            if (!held_[i].note) { if (slot < 0) slot = i; }
            else if (!held_[first_off].note || held_[i].off_at < held_[first_off].off_at) first_off = i;
        }
        if (slot < 0) { slot = first_off; emit(false, held_[slot].note, 0); }            // all busy: the one that ends first gives way
        held_[slot].note = static_cast<uint8_t>(note);
        held_[slot].off_at = off_at;
        const int vel = s.vel < 1 ? 1 : (s.vel > 127 ? 127 : s.vel);
        emit(true, note, vel);
    }
}

void Sequencer::play(uint64_t t, uint32_t slot) {
    rt_left_ = 0;
    const SeqStep &s = step_[cur_];
    bool any = false;
    for (int k = 0; k < kSeqChord; k++) any |= s.note[k] != 0;
    if (!any) return;
    if (s.prob < 100 && rnd() % 100u >= s.prob) return;
    const int len = s.len < 1 ? 1 : (s.len > kSeqSteps ? kSeqSteps : s.len);
    const uint64_t end = slot_time(slot + static_cast<uint32_t>(len));
    const int r = s.ratchet < 1 ? 1 : (s.ratchet > kSeqRatchet ? kSeqRatchet : s.ratchet);
    if (r == 1) { hit(s, end); return; }
    rt_sub_ = (next_at_ - t) / static_cast<uint64_t>(r);
    rt_left_ = r - 1;
    rt_step_ = cur_;
    rt_at_ = t + rt_sub_;
    rt_end_ = end;
    hit(s, rt_at_);
}

// What is owed: a note's end, or a ratchet hit; a note's end first when they share an instant.
bool Sequencer::pending(uint64_t *at) const {
    bool any = false;
    for (int i = 0; i < kSeqHeld; i++)
        if (held_[i].note && (!any || held_[i].off_at < *at)) { *at = held_[i].off_at; any = true; }
    if (rt_left_ > 0 && (!any || rt_at_ < *at)) { *at = rt_at_; any = true; }
    return any;
}

void Sequencer::fire() {
    int h = -1;
    for (int i = 0; i < kSeqHeld; i++)
        if (held_[i].note && (h < 0 || held_[i].off_at < held_[h].off_at)) h = i;
    if (h >= 0 && (rt_left_ <= 0 || held_[h].off_at <= rt_at_)) {
        emit(false, held_[h].note, 0);
        held_[h].note = 0;
        return;
    }
    if (rt_left_ <= 0) return;
    rt_left_--;
    const uint64_t at = rt_at_;
    rt_at_ += rt_sub_;
    hit(step_[rt_step_], rt_left_ > 0 ? at + rt_sub_ : rt_end_);
}

int Sequencer::advance(const Transport &tr, int frames, SeqEvent *out, int max) {
    out_ = out; n_ = 0; max_ = max;
    run(tr, frames);
    return n_;
}

/* ---------------- pads ---------------- */

void PadSequencer::clear() {
    reset();
    for (PadHit &h : hit_) h = PadHit{};
    for (PadHit &h : next_hit_) h = PadHit{};
    n_hits_ = n_next_ = 0;
    for (Held &h : held_) h = Held{};
    for (Ratchet &r : rt_) r = Ratchet{};
}

void PadSequencer::chunk(const PadHitChunk &c) {
    PadHit *to = c.queue ? next_hit_ : hit_;
    for (int i = 0; i < c.count && i < kPadSeqChunk && c.first + i < kPadSeqHits; i++) to[c.first + i] = c.hit[i];
    (c.queue ? n_next_ : n_hits_) = c.total < kPadSeqHits ? c.total : kPadSeqHits;
}

void PadSequencer::take() {
    for (int i = 0; i < n_next_; i++) hit_[i] = next_hit_[i];
    n_hits_ = n_next_;
}

void PadSequencer::emit(int pad, int level, bool release) {
    if (n_ >= max_) return;
    out_[n_++] = PadSeqEvent{static_cast<uint8_t>(pad), static_cast<uint8_t>(level), static_cast<uint8_t>(release ? 1 : 0)};
}

void PadSequencer::stop_all() {
    for (Held &h : held_) {
        if (h.on) emit(h.pad, 0, true);
        h = Held{};
    }
    for (Ratchet &r : rt_) r = Ratchet{};
}

// The pad is let go at off_at (a pad pushed again before that: at the later hit's time).
void PadSequencer::hold(int pad, uint64_t off_at) {
    int slot = -1, first_off = -1;
    for (int i = 0; i < kPadSeqHeld; i++) {
        if (held_[i].on && held_[i].pad == pad) { slot = i; break; }
        if (!held_[i].on) { if (slot < 0) slot = i; }
        else if (first_off < 0 || held_[i].off_at < held_[first_off].off_at) first_off = i;
    }
    if (slot < 0) { slot = first_off; emit(held_[slot].pad, 0, true); }      // all busy: the one that ends first is let go now
    held_[slot].on = 1;
    held_[slot].pad = static_cast<uint8_t>(pad);
    held_[slot].off_at = off_at;
}

void PadSequencer::play(uint64_t t, uint32_t slot) {
    for (int i = 0; i < n_hits_; i++) {
        const PadHit &h = hit_[i];
        if (static_cast<int>(h.step) != cur_) continue;
        if (h.prob < 10 && rnd() % 10u >= h.prob) continue;
        emit(static_cast<int>(h.pad), static_cast<int>(h.level), false);
        hold(static_cast<int>(h.pad), slot_time(slot + h.len + 1u));
        if (!h.ratchet) continue;
        for (Ratchet &r : rt_) {                            // its further hits inside the step (no free place: it plays once)
            if (r.left) continue;
            r.left = static_cast<uint8_t>(h.ratchet);
            r.pad = static_cast<uint8_t>(h.pad);
            r.level = static_cast<uint8_t>(h.level);
            r.sub = (next_at_ - t) / (h.ratchet + 1u);
            r.at = t + r.sub;
            break;
        }
    }
}

// What is owed: a pad to let go, or a ratchet hit; the release first when they share an instant.
bool PadSequencer::pending(uint64_t *at) const {
    bool any = false;
    for (const Held &h : held_) if (h.on && (!any || h.off_at < *at)) { *at = h.off_at; any = true; }
    for (const Ratchet &r : rt_) if (r.left && (!any || r.at < *at)) { *at = r.at; any = true; }
    return any;
}

void PadSequencer::fire() {
    int h = -1, r = -1;
    for (int i = 0; i < kPadSeqHeld; i++) if (held_[i].on && (h < 0 || held_[i].off_at < held_[h].off_at)) h = i;
    for (int i = 0; i < kPadSeqRatchets; i++) if (rt_[i].left && (r < 0 || rt_[i].at < rt_[r].at)) r = i;
    if (h >= 0 && (r < 0 || held_[h].off_at <= rt_[r].at)) {
        emit(held_[h].pad, 0, true);
        held_[h].on = 0;
        return;
    }
    if (r < 0) return;
    emit(rt_[r].pad, rt_[r].level, false);
    rt_[r].left--;
    rt_[r].at += rt_[r].sub;
}

int PadSequencer::advance(const Transport &tr, int frames, PadSeqEvent *out, int max) {
    out_ = out; n_ = 0; max_ = max;
    run(tr, frames);
    return n_;
}

}  // namespace sc
