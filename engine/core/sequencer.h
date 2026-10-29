#pragma once
// The step sequencers (ADR-042): a pattern of steps played on the machine clock (Transport, module.h) by the audio thread, so its
// timing does not depend on the control thread. The engine owns two: one plays notes (the synth's: Engine::seq_setup / seq_steps post
// the pattern as commands, its events become notes), one pushes pads (the Pads section's: Engine::pseq_setup / pseq_hits, its events
// go to the Pads module). StepSeq is what they share: the step times, the direction, the queued pattern, the position.
//
// Timing: a step lasts 1 / steps-per-beat of a beat (SeqRate); every second step is late by `swing` % of a step. Step times are
// computed from the step count since the start (no accumulation: it cannot drift against the clock). Like the pads' start on the
// grid, an event sounds in the block nearest to its time (within half a block).
// A run starts when the clock runs and `run` is set: at once when the clock starts with it (beat 0), else on the next bar line.
// So the control thread posts the setup BEFORE it starts the clock.
//
// A queued pattern (several patterns, a chain): the control thread sends the next pattern ahead (its content and a setup with
// `queue` set); the audio thread goes on to it when the playing one has played all its steps, so the change is on time whatever the
// control thread does then. taken() says so (a serial and the queued pattern's id).
//
// A step of notes: up to kSeqChord notes, a gate length in steps, a velocity, a probability and a ratchet (1..4 hits inside the step:
// each hit lasts until the next one, the last one until the gate's end). Notes are keyed by number: a pitch that still sounds is
// released before it starts again. At most kSeqHeld notes are held; the one that ends first gives way.
// A pad pattern is a list of hits (a pad on a step, with a level, a probability, a ratchet and a length after which the pad is let
// go: the Pads module stops a Gate or Toggle pad then, a one-shot pad plays on).
#include <cstdint>
#include "engine/core/command_queue.h"
#include "engine/core/module.h"

namespace sc {

constexpr int kSeqSteps = 64;           // the longest pattern
constexpr int kSeqChord = 4;            // notes of one step
constexpr int kSeqRatchet = 4;          // hits of one step at most
constexpr int kSeqHeld = 16;            // notes held at once
constexpr int kSeqChunk = 16;           // steps per SeqChunk
constexpr int kSeqEvents = 32;          // note events of one block (what does not fit sounds in the next one)

struct SeqStep {
    uint8_t note[kSeqChord] = {};       // MIDI notes, 0 = none (all 0: a rest)
    uint8_t len = 1;                    // the gate in steps, 1..kSeqSteps
    uint8_t vel = 100;                  // 1..127
    uint8_t prob = 100;                 // the chance in % that the step plays
    uint8_t ratchet = 1;                // 1..kSeqRatchet
};

enum SeqRate : uint8_t { SEQ_RATE_8, SEQ_RATE_8T, SEQ_RATE_16, SEQ_RATE_16T, SEQ_RATE_32, SEQ_RATES };   // 2, 3, 4, 6, 8 steps per beat
enum SeqDir : uint8_t { SEQ_FORWARD, SEQ_REVERSE, SEQ_PINGPONG, SEQ_RANDOM, SEQ_DIRS };
int seq_steps_per_beat(int rate);

struct SeqSetup {
    uint8_t run = 0;                    // 1: it plays while the clock runs
    uint8_t steps = 16;                 // the pattern's length, 1..kSeqSteps
    uint8_t rate = SEQ_RATE_16;
    uint8_t dir = SEQ_FORWARD;
    uint8_t swing = 0;                  // 0..75 % of a step
    int8_t transpose = 0;               // semitones added to every note
    uint8_t queue = 0;                  // SEQ_LIVE: the playing pattern's settings; SEQ_QUEUE: the queued one's (it is now complete); SEQ_UNQUEUE
    uint8_t id = 0;                     // SEQ_QUEUE: what taken() reports when it becomes the playing one
};
enum : uint8_t { SEQ_LIVE, SEQ_QUEUE, SEQ_UNQUEUE };

struct SeqChunk {                       // steps first .. first + count - 1 (one command)
    uint8_t first = 0, count = 0;
    uint8_t queue = 0;                  // 1: of the queued pattern
    SeqStep step[kSeqChunk];
};
static_assert(sizeof(SeqChunk) <= kCmdBlobMax, "a chunk of steps must fit in one command");

struct SeqEvent { uint8_t on, note, vel; };

// The pads' pattern: its hits, in any order.
constexpr int kPadSeqHits = 160;        // hits of one pattern
constexpr int kPadSeqChunk = 60;        // hits per PadHitChunk
constexpr int kPadSeqHeld = 16;         // pads waiting to be let go
constexpr int kPadSeqRatchets = 8;      // pads ratcheting at once
constexpr int kPadSeqEvents = 160;      // pad events of one block
struct PadHit {
    uint32_t pad     : 6;               // 0..63
    uint32_t step    : 6;               // 0..63
    uint32_t level   : 4;               // 0..15: (level + 1) / 16 of the pad's level
    uint32_t prob    : 4;               // 0..10: the chance in tens of %
    uint32_t ratchet : 2;               // 0..3: 1..4 hits
    uint32_t len     : 6;               // 0..63: let go after 1..64 steps
};
struct PadHitChunk {                    // hits first .. first + count - 1 of a pattern of `total` hits (one command)
    uint8_t first = 0, count = 0, total = 0;
    uint8_t queue = 0;                  // 1: of the queued pattern
    PadHit hit[kPadSeqChunk];
};
static_assert(sizeof(PadHitChunk) <= kCmdBlobMax, "a chunk of hits must fit in one command");
struct PadSeqEvent { uint8_t pad, level, release; };    // a push at a level 0..15, or the pad let go

// The time of step slot k of a run that started at `base` (beats Q32): k steps later, the odd ones late by the swing.
uint64_t seq_slot_time(uint64_t base, uint32_t k, int steps_per_beat, int swing);

// What the two sequencers share. A subclass holds the pattern and plays it.
class StepSeq {
public:
    virtual ~StepSeq() = default;
    void setup(const SeqSetup &s);                      // [AUDIO]
    void rewind() { rewind_ = true; }                   // [AUDIO] the clock was started again: the next block starts a new run
    const SeqPos &pos() const { return pos_; }
    int step() const { return pos_.running ? cur_ : -1; }   // the step that plays, -1 when none does
    uint32_t taken() const { return taken_; }               // (how many queued patterns were taken) << 8 | the last one's id
    uint32_t count() const { return slot_ ? slot_ - 1 : 0; }   // how many steps played before it (it starts again at another rate)
    uint32_t frac() const { return frac_; }                 // how far into it the block ends, 0..0xFFFF (in time: also going backwards)

protected:
    void reset();                                       // stopped, no queue (the subclass clears its pattern)
    void run(const Transport &tr, int frames);          // one block: what is due in it, then the position at its end
    int steps() const { return set_.steps < 1 ? 1 : (set_.steps > kSeqSteps ? kSeqSteps : set_.steps); }
    uint64_t slot_time(uint32_t k) const { return seq_slot_time(base_, k, seq_steps_per_beat(set_.rate), set_.swing); }
    uint32_t rnd() { rng_ = rng_ * 1664525u + 1013904223u; return rng_ >> 8; }

    virtual bool room() const = 0;                      // the block's output can take what one more step or pending event may emit
    virtual bool pending(uint64_t *at) const = 0;       // the earliest thing the pattern still owes (a note's end, a ratchet hit) and its time
    virtual void fire() = 0;                            // ... it happens
    virtual void play(uint64_t t, uint32_t slot) = 0;   // step cur_ starts at t; it is slot `slot` of the run, next_at_ is its end
    virtual void stop_all() = 0;                        // the run ends: everything held is let go
    virtual void take() = 0;                            // the queued pattern's content becomes the playing one's

    SeqSetup set_;
    int cur_ = 0;                       // the step that plays
    uint64_t next_at_ = 0;              // its end

private:
    void step_due(uint64_t t);

    SeqSetup next_set_;                 // the queued pattern's settings ...
    bool queued_ = false;               // ... it is complete and waits for the playing one's end
    int played_ = 0;                    // steps of the playing pattern played in this pass
    uint32_t taken_ = 0;
    bool playing_ = false, first_ = true, rewind_ = false;
    int bounce_ = 1;                    // ping-pong: the way it goes
    uint32_t slot_ = 0;                 // the slot of the NEXT step
    uint64_t base_ = 0;                 // the beat slot 0 is on
    uint64_t step_at_ = 0;              // the playing step's start
    uint32_t rng_ = 0x2545F491u;
    SeqPos pos_;
    uint32_t frac_ = 0;
};

// The synth's: steps of notes.
class Sequencer : public StepSeq {
public:
    void clear();                                       // [INIT] an empty pattern, stopped
    void chunk(const SeqChunk &c);                      // [AUDIO]
    // [AUDIO] one block: the note events due in it, in order (at most `max`, at least 2 * kSeqChord). Then pos() is where the block ends.
    int advance(const Transport &tr, int frames, SeqEvent *out, int max);

private:
    struct Held { uint8_t note = 0; uint64_t off_at = 0; };
    bool room() const override { return n_ + 2 * kSeqChord <= max_; }
    bool pending(uint64_t *at) const override;
    void fire() override;
    void play(uint64_t t, uint32_t slot) override;
    void stop_all() override;
    void take() override;
    void emit(bool on, int note, int vel);
    void hit(const SeqStep &s, uint64_t off_at);

    SeqStep step_[kSeqSteps];
    SeqStep next_step_[kSeqSteps];      // the queued pattern
    Held held_[kSeqHeld];
    int rt_left_ = 0, rt_step_ = 0;     // the ratchet hits still to come of step rt_step_ ...
    uint64_t rt_at_ = 0, rt_sub_ = 0, rt_end_ = 0;   // ... the next one's time, their spacing, the last one's end
    SeqEvent *out_ = nullptr;
    int n_ = 0, max_ = 0;
};

// The Pads section's: a list of hits.
class PadSequencer : public StepSeq {
public:
    void clear();                                       // [INIT] an empty pattern, stopped
    void chunk(const PadHitChunk &c);                   // [AUDIO]
    // [AUDIO] one block: the pad events due in it, in order (`max` at least kPadSeqEvents: a step may push every pad).
    int advance(const Transport &tr, int frames, PadSeqEvent *out, int max);

private:
    struct Held { uint8_t on = 0, pad = 0; uint64_t off_at = 0; };                           // a pad to let go
    struct Ratchet { uint8_t left = 0, pad = 0, level = 0; uint64_t at = 0, sub = 0; };       // hits still to come inside the step
    bool room() const override { return n_ + 128 <= max_; }       // (a step: a push per pad, and as many pads let go early)
    bool pending(uint64_t *at) const override;
    void fire() override;
    void play(uint64_t t, uint32_t slot) override;
    void stop_all() override;
    void take() override;
    void emit(int pad, int level, bool release);
    void hold(int pad, uint64_t off_at);

    PadHit hit_[kPadSeqHits];
    PadHit next_hit_[kPadSeqHits];      // the queued pattern
    int n_hits_ = 0, n_next_ = 0;
    Held held_[kPadSeqHeld];
    Ratchet rt_[kPadSeqRatchets];
    PadSeqEvent *out_ = nullptr;
    int n_ = 0, max_ = 0;
};

}  // namespace sc
