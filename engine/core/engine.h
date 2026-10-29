#pragma once
// The engine: owns the module instances, the voice manager and the active plan, and renders blocks.
//
// Threading contract (ADR-024):
//   [CONTROL] load(), gc(), set_param(), set_blob(), note_on(), note_off(), all_notes_off()   -- one control thread
//   [AUDIO]   render()                                                                       -- one audio thread, never allocates
// The control calls that touch sounding state do not touch it: they post a command into a lock-free ring that
// render() drains at the start of every block. load() compiles a new plan on the control thread and publishes it
// with one atomic pointer; render() switches to it at the next block start and applies, at that moment, the new
// parameter values of the module instances it shares with the plan that was running (so a rebuild never changes
// a module in the middle of a block). Instances with the same (id, type) are reused: filter memory, delay tails
// and sampler playheads survive. gc() frees retired plans and instances no plan references any more, and backs
// off while the audio thread is switching (seqlock), so it can never free what the next block is about to use.
#include <atomic>
#include "engine/core/command_queue.h"
#include "engine/core/graph.h"
#include "engine/core/heap.h"
#include "engine/core/module.h"
#include "engine/core/plan.h"
#include "engine/core/sequencer.h"

namespace sc {

// How note events are given to the voices. Poly: the voice allocator (steals the oldest released voice). Mono: one voice, last-note priority
// with a stack of held keys (releasing the top key returns to the one below it); `legato` keeps the envelopes running when the pitch changes
// while a key is held, `glide` slides the pitch (exponential, per block).
enum class VoiceMode : uint8_t { Poly, Mono };

class Engine {
public:
    bool init(Memory mem, int nvoices);                 // registers the builtin modules; nvoices = the default voice count of load()
    bool init(Heap &heap, int nvoices) { return init(Memory{&heap, &heap}, nvoices); }   // one heap for everything
    void shutdown();                                    // frees everything the engine allocated

    Registry &registry() { return reg_; }
    // `nvoices` (1..kMaxVoices, 0 = the count given to init) is how many copies of every voice-scope module this graph gets: memory and CPU scale with it.
    // Changing it creates new voice instances (their state starts clean; the old ones are freed by gc()) and silences the voices at the switch.
    Err load(const GraphDesc &g, int nvoices = 0);      // [CONTROL]
    bool set_voice_mode(VoiceMode mode, bool legato, int glide_ms);   // [CONTROL] posted like a note; glide_ms 0 = jump
    void gc();                                          // [CONTROL]
    bool switching() const { return pending_.load(std::memory_order_acquire) != nullptr; }   // a loaded plan waits for the audio thread

    bool note_on(int note, int velocity127 = 100);      // [CONTROL] false = the command queue was full
    bool note_off(int note);
    bool all_notes_off();
    bool set_param(int node_id, int idx, int32_t value);   // live parameter change, all voices of the node
    bool set_edge_depth(int edge, q15 depth);           // live depth of cable `edge` (index in the loaded GraphDesc); false = needs load() (aliased unity cable, or the new depth is unity)
    bool set_blob(int node_id, const void *data, size_t bytes);   // structured settings, at most kCmdBlobMax bytes
    // [CONTROL] the machine clock: tempo (20..400 BPM), running (stopped -> running starts at beat 0), the metronome click (beat 1 higher).
    bool set_transport(int bpm, bool running, bool click);
    uint64_t transport_beat() const { return tr_beat_.load(std::memory_order_relaxed); }    // [any thread] the position, beats Q32
    bool transport_running() const { return tr_run_.load(std::memory_order_relaxed); }
    // [CONTROL] the step sequencer (sequencer.h): it plays its pattern on the machine clock and its notes go to the voices like played
    // ones. To start it with the clock, post the setup (run = 1) before set_transport. seq_steps sends steps first .. first + count - 1.
    // A pattern to go on to at the end of the playing one: its steps with queue = true, then seq_setup with queue = SEQ_QUEUE and an
    // id; seq_taken() changes when the audio thread has taken it (a count << 8 | that id). SEQ_UNQUEUE drops it.
    bool seq_setup(const SeqSetup &s);
    bool seq_steps(int first, int count, const SeqStep *steps, bool queue = false);
    uint32_t seq_taken() const { return seq_taken_.load(std::memory_order_relaxed); }      // [any thread]
    // [CONTROL] the pads' sequencer, the same way: its hits push the pads of the Pads module `node` (set_blob with a PadSeqEvent: a
    // push at a level, or the pad let go). pseq_hits sends hits first .. first + count - 1 of a pattern of `total` hits.
    bool pseq_setup(const SeqSetup &s, int node);
    bool pseq_hits(int first, int count, const PadHit *hits, int total, bool queue = false);
    int pseq_step() const { return pseq_step_.load(std::memory_order_relaxed); }
    uint32_t pseq_taken() const { return pseq_taken_.load(std::memory_order_relaxed); }
    bool pseq_where(int *step, int *frac, uint32_t *count) const {
        const uint64_t w = pseq_where_.load(std::memory_order_relaxed);
        if (!(w >> 63)) return false;
        *frac = static_cast<int>(w & 0xFF); *step = static_cast<int>((w >> 8) & 0xFF); *count = static_cast<uint32_t>(w >> 16);
        return true;
    }
    int seq_step() const { return seq_step_.load(std::memory_order_relaxed); }             // [any thread] the step that plays, -1 = none
    // [any thread] the same with how far into the step it is (0..255) and how many steps played before it; false = none plays
    bool seq_where(int *step, int *frac, uint32_t *count) const {
        const uint64_t w = seq_where_.load(std::memory_order_relaxed);
        if (!(w >> 63)) return false;
        *frac = static_cast<int>(w & 0xFF); *step = static_cast<int>((w >> 8) & 0xFF); *count = static_cast<uint32_t>(w >> 16);
        return true;
    }

    void render(q15 *l, q15 *r);                        // [AUDIO] exactly kBlock frames
    int active_voices() const;

#ifdef ENGINE_PROFILE
    // Optional profiler (-DENGINE_PROFILE): CPU cycles spent in each module type, summed over voices. Read and reset it from the audio thread.
    struct ProfEntry { const char *name; uint64_t cycles; uint32_t calls; };
    static constexpr int kProfMax = 40;
    int prof_take(ProfEntry *out, int max, uint32_t *blocks);       // copies and clears; *blocks = blocks rendered since the last call
#endif
    uint64_t blocks() const { return time_.load(std::memory_order_relaxed); }
    const VoiceState &voice(int v) const { return voices_[v]; }

private:
    // nv = how many instances were made (the voice count of the load that created it); inst = nv pointers from the fast heap (a fixed
    // kMaxVoices array here cost 128 x 32 pointers = 16 KB of internal RAM at the 32-voice ceiling, ADR-037)
    // A single instance (global nodes, one-voice patches) lives in `one`: no tiny heap block for it.
    struct Rec { bool alive; uint8_t id, type, nv; Module **inst; Module *one; };
    static constexpr int kMaxRecs = 2 * kMaxNodes;
    static constexpr int kMonoStack = 16;

    int find_rec(int id, int type, int nv) const;
    void destroy_rec(Rec &r);
    void run(const Plan *pl, int first, int count, int voice, ProcessCtx &ctx);
    int alloc_voice(int note);
    void apply(const Command &c);                       // [AUDIO]
    void do_note_on(int note, int velocity127);
    void do_note_off(int note);
    void mono_play(int note, q15 velocity);             // [AUDIO] start / retrigger / retarget the single voice
    void mono_remove(int note);
    void free_retired();

    Memory mem_;
    Registry reg_;
    int nvoices_ = 0;                                   // voices of the ACTIVE plan: written and read by the audio thread only (set at the plan switch)
    int default_nv_ = 0;                                // the count load() uses when it is not given one
    VoiceMode mode_ = VoiceMode::Poly;                  // audio thread
    bool legato_ = false;
    int32_t glide_k_ = 0;                               // q15 fraction of the distance covered per block (0 = jump)
    uint8_t stack_[kMonoStack] = {};                    // held keys, last on top (Mono)
    int stack_n_ = 0;
    Rec rec_[kMaxRecs] = {};
    VoiceState voices_[kMaxVoices];
    std::atomic<Plan *> active_{nullptr};
    std::atomic<Plan *> pending_{nullptr};
    std::atomic<uint32_t> swap_seq_{0};                 // odd while the audio thread switches plans (seqlock for gc)
    CommandRing<ENGINE_CMD_RING> cmd_;
    std::atomic<Plan *> retired_{nullptr};              // lock-free stack: pushed by the audio thread, taken whole by gc()
    alignas(16) int32_t bus_l_[kBlock] = {}, bus_r_[kBlock] = {};     // the voice sum, 32 bits: saturated once when read (BusIn)
    alignas(16) int32_t bus2_l_[kBlock] = {}, bus2_r_[kBlock] = {};   // the second voice bus (branch 2 of the rack, ADR-041)
    std::atomic<uint64_t> time_{0};
    Transport tr_;                                      // audio thread
    bool click_on_ = false;
    uint32_t click_ph_ = 0, click_inc_ = 0;             // the metronome click: a sine burst ...
    int32_t click_env_ = 0;                             // ... with a fast decay (q15)
    std::atomic<uint64_t> tr_beat_{0};
    std::atomic<bool> tr_run_{false};
    Sequencer seq_;                                     // audio thread
    std::atomic<int> seq_step_{-1};
    std::atomic<uint32_t> seq_taken_{0};
    PadSequencer pseq_;                                 // audio thread
    int pseq_node_ = -1;                                // the Pads module its events go to
    std::atomic<int> pseq_step_{-1};
    std::atomic<uint32_t> pseq_taken_{0};
    std::atomic<uint64_t> pseq_where_{0};
    std::atomic<uint64_t> seq_where_{0};                // bit 63 = a step plays; count << 16 | step << 8 | fraction
    void transport_block(q15 *l, q15 *r);               // [AUDIO] the click into the output, then the clock moves on
#ifdef ENGINE_PROFILE
    ProfEntry prof_[kProfMax] = {};
    int prof_n_ = 0;
    uint32_t prof_blocks_ = 0;
    void prof_add(const char *name, uint32_t cycles);
#endif
};

}  // namespace sc
