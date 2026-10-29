#include "engine/core/engine.h"
#include <cmath>
#include "engine/dsp/block.h"
#include "engine/dsp/phase.h"
#include "engine/modules/builtin.h"

#ifdef ENGINE_PROFILE
#if defined(ARDUINO_ARCH_ESP32)
#include <esp_cpu.h>
static inline uint32_t prof_now() { return esp_cpu_get_cycle_count(); }
#else
#include <chrono>
static inline uint32_t prof_now() { return static_cast<uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count()); }   // ns on the desktop
#endif
#endif

namespace sc {

#ifdef ENGINE_PROFILE
void Engine::prof_add(const char *name, uint32_t cycles) {
    for (int i = 0; i < prof_n_; i++)
        if (prof_[i].name == name) { prof_[i].cycles += cycles; prof_[i].calls++; return; }       // names are string literals: compare the pointers
    if (prof_n_ < kProfMax) prof_[prof_n_++] = ProfEntry{name, cycles, 1};
}

int Engine::prof_take(ProfEntry *out, int max, uint32_t *blocks) {
    const int n = prof_n_ < max ? prof_n_ : max;
    for (int i = 0; i < n; i++) out[i] = prof_[i];
    for (int i = 0; i < prof_n_; i++) prof_[i].cycles = 0, prof_[i].calls = 0;
    *blocks = prof_blocks_;
    prof_blocks_ = 0;
    return n;
}
#endif

bool Engine::init(Memory mem, int nvoices) {
    mem_ = mem;
    nvoices_ = default_nv_ = nvoices < 1 ? 1 : (nvoices > kMaxVoices ? kMaxVoices : nvoices);
    register_builtin_modules(reg_);
    seq_.clear();
    seq_step_.store(-1, std::memory_order_relaxed);
    seq_where_.store(0, std::memory_order_relaxed);
    seq_taken_.store(0, std::memory_order_relaxed);
    pseq_.clear();
    pseq_node_ = -1;
    pseq_step_.store(-1, std::memory_order_relaxed);
    pseq_taken_.store(0, std::memory_order_relaxed);
    pseq_where_.store(0, std::memory_order_relaxed);
    return true;
}

void Engine::destroy_rec(Rec &r) {
    if (r.inst) {
        for (int v = 0; v < r.nv; v++) destroy_module(*mem_.fast, r.inst[v]);
        if (r.inst != &r.one) mem_.fast->free(r.inst);
        r.inst = nullptr;
        r.one = nullptr;
    }
    r.alive = false;
}

int Engine::find_rec(int id, int type, int nv) const {
    for (int i = 0; i < kMaxRecs; i++) if (rec_[i].alive && rec_[i].id == id && rec_[i].type == type && rec_[i].nv == nv) return i;
    return -1;
}

Err Engine::load(const GraphDesc &g, int nvoices) {
    const int nv = nvoices <= 0 ? default_nv_ : (nvoices > kMaxVoices ? kMaxVoices : nvoices);
    Plan *pl = nullptr;
    Err e = compile_plan(g, reg_, *mem_.fast, nv, &pl);
    if (e != Err::Ok) return e;

    bool created[kMaxRecs] = {};
    auto rollback = [&]() {
        for (int i = 0; i < kMaxRecs; i++) if (created[i]) destroy_rec(rec_[i]);
        free_plan(pl);
    };

    // instances that already exist keep running: their new parameter values are applied by the audio thread at the swap
    int n_updates = 0;
    for (int i = 0; i < g.n_nodes; i++) {
        const ModuleInfo &info = *reg_.get(g.node[i].type)->info;
        const int count = info.scope == Scope::Voice ? nv : 1;
        if (find_rec(g.node[i].id, g.node[i].type, count) < 0) continue;      // (a node whose voice count changed is created anew, not updated)
        n_updates += info.n_param * count;
    }
    if (n_updates > 0) {
        pl->updates = pl->heap->alloc_array<ParamUpdate>(static_cast<size_t>(n_updates));
        if (!pl->updates) { rollback(); return Err::NoMem; }
    }

    for (int i = 0; i < g.n_nodes; i++) {
        const NodeDesc &nd = g.node[i];
        const ModuleType *t = reg_.get(nd.type);
        const ModuleInfo &info = *t->info;
        const int count = info.scope == Scope::Voice ? nv : 1;
        int ri = find_rec(nd.id, nd.type, count);
        bool fresh = ri < 0;
        if (fresh) {
            for (int k = 0; k < kMaxRecs && ri < 0; k++) if (!rec_[k].alive) ri = k;
            if (ri < 0) { rollback(); return Err::TooBig; }
            Rec &r = rec_[ri];
            r = Rec{};
            r.alive = true; r.id = nd.id; r.type = nd.type; r.nv = static_cast<uint8_t>(count);
            created[ri] = true;
            r.inst = count == 1 ? &r.one : mem_.fast->alloc_array<Module *>(static_cast<size_t>(count));
            if (!r.inst) { rollback(); return Err::NoMem; }
            for (int v = 0; v < count; v++) r.inst[v] = nullptr;
            for (int v = 0; v < count; v++) {
                r.inst[v] = t->create(mem_);
                if (!r.inst[v]) { rollback(); return Err::NoMem; }
            }
        }
        Rec &r = rec_[ri];
        for (int v = 0; v < count; v++) {
            if (fresh) {                                     // not visible to the audio thread yet: configure directly
                for (int p = 0; p < info.n_param; p++) r.inst[v]->set_param(p, nd.param[p]);
                r.inst[v]->reset();
            } else {
                for (int p = 0; p < info.n_param; p++) pl->updates[pl->n_updates++] = ParamUpdate{r.inst[v], static_cast<uint8_t>(p), nd.param[p]};
            }
            pl->inst(i, v) = r.inst[v];
        }
        pl->rec[i] = ri;
        if (nd.type == T_MASTER_OUT) pl->has_master = true;
    }

    Plan *old = pending_.exchange(pl, std::memory_order_acq_rel);
    free_plan(old);             // a plan that was published but never rendered (its fresh instances stay recorded: gc() or the next load reuse them)
    return Err::Ok;
}

void Engine::free_retired() {
    Plan *p = retired_.exchange(nullptr, std::memory_order_acq_rel);             // take the whole stack; the audio thread keeps pushing onto a fresh one
    while (p) {
        Plan *next = p->next_retired;
        free_plan(p);
        p = next;
    }
}

void Engine::gc() {
    // The audio thread switches plans between swap_seq_ increments (odd = in progress): read both plans consistently or try later.
    const uint32_t s1 = swap_seq_.load(std::memory_order_acquire);
    if (s1 & 1u) return;
    const Plan *plans[2] = {active_.load(std::memory_order_acquire), pending_.load(std::memory_order_acquire)};
    if (swap_seq_.load(std::memory_order_acquire) != s1) return;

    free_retired();
    bool used[kMaxRecs] = {};
    for (const Plan *pl : plans)
        if (pl) for (int i = 0; i < pl->n_nodes; i++) used[pl->rec[i]] = true;
    for (int i = 0; i < kMaxRecs; i++) if (rec_[i].alive && !used[i]) destroy_rec(rec_[i]);
}

void Engine::shutdown() {
    free_plan(pending_.exchange(nullptr));
    free_plan(active_.exchange(nullptr));
    free_retired();
    for (auto &r : rec_) if (r.alive) destroy_rec(r);
    for (auto &v : voices_) v = VoiceState{};
    Command c;
    while (cmd_.pop(c)) {}
}

/* ---------------- control side: post commands ---------------- */

bool Engine::note_on(int note, int velocity127) {
    Command c;
    c.type = Cmd::NoteOn; c.note = static_cast<uint8_t>(note); c.value = velocity127;
    return cmd_.push(c);
}
bool Engine::note_off(int note) {
    Command c;
    c.type = Cmd::NoteOff; c.note = static_cast<uint8_t>(note);
    return cmd_.push(c);
}
bool Engine::all_notes_off() {
    Command c;
    c.type = Cmd::AllNotesOff;
    return cmd_.push(c);
}
bool Engine::set_param(int node_id, int idx, int32_t value) {
    Command c;
    c.type = Cmd::SetParam; c.node = static_cast<uint8_t>(node_id); c.idx = static_cast<uint8_t>(idx); c.value = value;
    return cmd_.push(c);
}
bool Engine::set_edge_depth(int edge, q15 depth) {
    // the newest plan: a loaded one the audio thread has not switched to yet is the one the command will reach (commands run after the swap)
    const Plan *pl = pending_.load(std::memory_order_acquire);
    if (!pl) pl = active_.load(std::memory_order_acquire);
    if (!pl || edge < 0 || edge >= kMaxEdges || pl->edge_step[edge] == 255 || depth == kUnity) return false;
    Command c;
    c.type = Cmd::SetDepth; c.node = static_cast<uint8_t>(edge); c.value = depth;
    return cmd_.push(c);
}
bool Engine::set_blob(int node_id, const void *data, size_t bytes) {
    if (bytes > static_cast<size_t>(kCmdBlobMax)) return false;
    Command c;
    c.type = Cmd::SetBlob; c.node = static_cast<uint8_t>(node_id); c.size = static_cast<uint16_t>(bytes);
    std::memcpy(c.blob, data, bytes);
    return cmd_.push(c);
}

/* ---------------- voices (audio thread) ---------------- */

int Engine::alloc_voice(int note) {
    int best = -1;
    for (int v = 0; v < nvoices_; v++)                                  // same note: retrigger that voice
        if (voices_[v].active && voices_[v].note == note) return v;
    for (int v = 0; v < nvoices_; v++) if (!voices_[v].active) return v;
    for (int v = 0; v < nvoices_; v++)                                  // steal: longest-released, else oldest
        if (!voices_[v].gate && (best < 0 || voices_[v].release_age > voices_[best].release_age)) best = v;
    if (best >= 0) return best;
    best = 0;
    for (int v = 1; v < nvoices_; v++) if (voices_[v].age > voices_[best].age) best = v;
    return best;
}

// Mono: the single voice plays the key on top of the stack. With legato (and a key already held) only the pitch changes: the envelopes and the
// other module state keep running. Otherwise the voice restarts, and with a glide it slides from the pitch it has now (also during the release tail).
void Engine::mono_play(int note, q15 velocity) {
    VoiceState &s = voices_[0];
    if (s.active && s.gate && legato_) {
        s.note = note;
        s.target_pitch = note * 256;
        s.velocity = velocity;
        if (glide_k_ == 0) s.pitch = s.target_pitch;
        return;
    }
    const int32_t from = s.active && glide_k_ > 0 ? s.pitch : note * 256;
    s = VoiceState{};
    s.note = note;
    s.pitch = from;
    s.target_pitch = note * 256;
    s.velocity = velocity;
    s.gate = true;
    s.started = true;
    s.active = true;
}

void Engine::mono_remove(int note) {
    int k = 0;
    for (int i = 0; i < stack_n_; i++) if (stack_[i] != note) stack_[k++] = stack_[i];
    stack_n_ = k;
}

void Engine::do_note_on(int note, int velocity127) {
    const q15 vel = static_cast<q15>(clamp_i32(velocity127, 1, 127) * 32767 / 127);
    if (mode_ == VoiceMode::Mono) {
        mono_remove(note);                                              // a key pressed again moves to the top
        if (stack_n_ == kMonoStack) { for (int i = 1; i < kMonoStack; i++) stack_[i - 1] = stack_[i]; stack_n_--; }
        stack_[stack_n_++] = static_cast<uint8_t>(note);
        mono_play(note, vel);
        return;
    }
    int v = alloc_voice(note);
    VoiceState &s = voices_[v];
    s = VoiceState{};
    s.note = note;
    s.pitch = s.target_pitch = note * 256;
    s.velocity = vel;
    s.gate = true;
    s.started = true;
    s.active = true;
}

void Engine::do_note_off(int note) {
    if (mode_ == VoiceMode::Mono) {
        mono_remove(note);
        VoiceState &s = voices_[0];
        if (!s.active || !s.gate) return;
        if (stack_n_ == 0) { s.gate = false; return; }                 // the last key went up: release
        if (s.note == note) mono_play(stack_[stack_n_ - 1], s.velocity);   // the sounding key went up while others are held: back to the one below
        return;
    }
    for (int v = 0; v < nvoices_; v++)
        if (voices_[v].active && voices_[v].gate && voices_[v].note == note) voices_[v].gate = false;
}

bool Engine::set_voice_mode(VoiceMode mode, bool legato, int glide_ms) {
    Command c;
    c.type = Cmd::VoiceMode;
    c.node = static_cast<uint8_t>(mode);
    c.idx = legato ? 1 : 0;
    // the fraction of the remaining distance covered per block, for a time constant of glide_ms: 1 - exp(-block_ms / glide_ms)
    const double block_ms = 1000.0 * kBlock / kSampleRate;
    c.value = glide_ms <= 0 ? 0 : static_cast<int32_t>(std::lround(32767.0 * (1.0 - std::exp(-block_ms / glide_ms))));
    if (glide_ms > 0 && c.value < 1) c.value = 1;
    return cmd_.push(c);
}

bool Engine::set_transport(int bpm, bool running, bool click) {
    Command c;
    c.type = Cmd::Transport;
    c.node = running ? 1 : 0;
    c.idx = click ? 1 : 0;
    c.value = bpm < 20 ? 20 : bpm > 400 ? 400 : bpm;
    return cmd_.push(c);
}

bool Engine::seq_setup(const SeqSetup &s) {
    Command c;
    c.type = Cmd::SeqSetup; c.size = sizeof s;
    std::memcpy(c.blob, &s, sizeof s);
    return cmd_.push(c);
}

bool Engine::seq_steps(int first, int count, const SeqStep *steps, bool queue) {
    if (first < 0 || count < 1 || count > kSeqChunk || first + count > kSeqSteps) return false;
    SeqChunk k;
    k.first = static_cast<uint8_t>(first); k.count = static_cast<uint8_t>(count); k.queue = queue ? 1 : 0;
    for (int i = 0; i < count; i++) k.step[i] = steps[i];
    Command c;
    c.type = Cmd::SeqSteps; c.size = sizeof k;
    std::memcpy(c.blob, &k, sizeof k);
    return cmd_.push(c);
}

bool Engine::pseq_setup(const SeqSetup &s, int node) {
    Command c;
    c.type = Cmd::PadSeqSetup; c.node = static_cast<uint8_t>(node); c.size = sizeof s;
    std::memcpy(c.blob, &s, sizeof s);
    return cmd_.push(c);
}

bool Engine::pseq_hits(int first, int count, const PadHit *hits, int total, bool queue) {
    if (first < 0 || count < 0 || count > kPadSeqChunk || first + count > kPadSeqHits || total > kPadSeqHits) return false;
    PadHitChunk k;
    k.first = static_cast<uint8_t>(first); k.count = static_cast<uint8_t>(count); k.total = static_cast<uint8_t>(total); k.queue = queue ? 1 : 0;
    for (int i = 0; i < count; i++) k.hit[i] = hits[i];
    Command c;
    c.type = Cmd::PadSeqHits; c.size = sizeof k;
    std::memcpy(c.blob, &k, sizeof k);
    return cmd_.push(c);
}

// The metronome click (a 1.5 kHz sine burst, 3 kHz on beat 1 of the bar, decaying in a few ms, at about -12 dBFS) is added after the
// master output: the master volume does not change it. Then the position moves by the block.
void Engine::transport_block(q15 *l, q15 *r) {
    if (tr_.running && click_on_) {
        const uint64_t next = (tr_.beat_q32 + 0xFFFFFFFFull) & ~0xFFFFFFFFull;    // the first beat at or after the block start
        const int k = tr_.sample_at(next, kBlock);
        for (int i = 0; i < kBlock; i++) {
            if (i == k) {
                const bool bar = ((next >> 32) & 3) == 0;
                click_inc_ = static_cast<uint32_t>((bar ? 3000.0 : 1500.0) * 4294967296.0 / kSampleRate);
                click_ph_ = 0;
                click_env_ = 8192;
            }
            if (click_env_ <= 0) continue;
            const int32_t s = (static_cast<int32_t>(sine(click_ph_)) * click_env_) >> 15;
            click_ph_ += click_inc_;
            click_env_ -= (click_env_ >> 7) + 1;
            l[i] = sat16(l[i] + s);
            r[i] = sat16(r[i] + s);
        }
    }
    if (tr_.running) tr_.beat_q32 += tr_.inc_q32 * kBlock;
    tr_beat_.store(tr_.beat_q32, std::memory_order_relaxed);
    tr_run_.store(tr_.running, std::memory_order_relaxed);
}

void Engine::apply(const Command &c) {
    switch (c.type) {
    case Cmd::Transport:
        if (c.node && !tr_.running) { tr_.beat_q32 = 0; seq_.rewind(); pseq_.rewind(); }   // a start: bar 1, beat 1 (the sequencers' first step)
        tr_.running = c.node != 0;
        tr_.bpm = c.value;
        tr_.inc_q32 = (static_cast<uint64_t>(c.value) << 32) / (60ull * static_cast<uint64_t>(kSampleRate));
        click_on_ = c.idx != 0;
        if (!click_on_) click_env_ = 0;
        break;
    case Cmd::SeqSetup: { SeqSetup s; std::memcpy(&s, c.blob, sizeof s); seq_.setup(s); break; }
    case Cmd::SeqSteps: { SeqChunk k; std::memcpy(&k, c.blob, sizeof k); seq_.chunk(k); break; }
    case Cmd::PadSeqSetup: { SeqSetup s; std::memcpy(&s, c.blob, sizeof s); pseq_.setup(s); pseq_node_ = c.node; break; }
    case Cmd::PadSeqHits: { PadHitChunk k; std::memcpy(&k, c.blob, sizeof k); pseq_.chunk(k); break; }
    case Cmd::NoteOn: do_note_on(c.note, c.value); break;
    case Cmd::NoteOff: do_note_off(c.note); break;
    case Cmd::AllNotesOff: for (auto &v : voices_) v = VoiceState{}; stack_n_ = 0; break;
    case Cmd::VoiceMode:
        mode_ = c.node == static_cast<uint8_t>(VoiceMode::Mono) ? VoiceMode::Mono : VoiceMode::Poly;
        legato_ = c.idx != 0;
        glide_k_ = c.value;
        stack_n_ = 0;
        break;
    case Cmd::SetParam:
    case Cmd::SetBlob: {
        const Plan *pl = active_.load(std::memory_order_acquire);
        if (!pl) break;
        for (int n = 0; n < pl->n_nodes; n++) {
            if (pl->node_id[n] != c.node) continue;
            const int count = pl->node_scope[n] == Scope::Voice ? pl->nvoices : 1;
            for (int v = 0; v < count; v++) {
                Module *m = pl->inst(n, v);
                if (!m) continue;
                if (c.type == Cmd::SetParam) m->set_param(c.idx, c.value);
                else m->set_blob(c.blob, c.size);
            }
            break;
        }
        break;
    }
    case Cmd::SetDepth: {
        Plan *pl = active_.load(std::memory_order_acquire);
        if (pl && pl->edge_step[c.node] != 255) pl->steps[pl->edge_step[c.node]].gain[pl->edge_slot[c.node]] = static_cast<q15>(c.value);
        break;
    }
    default: break;
    }
}

int Engine::active_voices() const {
    int n = 0;
    for (int v = 0; v < nvoices_; v++) n += voices_[v].active;
    return n;
}

/* ---------------- audio ---------------- */

void Engine::run(const Plan *pl, int first, int count, int voice, ProcessCtx &ctx) {
    const int stride = pl->fbv_stride;
    const q15 *fb0 = pl->fbv, *fb1 = pl->fbv + (stride ? stride * pl->nvoices : 0);
    const ptrdiff_t shift = static_cast<ptrdiff_t>(voice) * stride;
    auto rel = [&](const q15 *p) -> const q15 * { return (p >= fb0 && p < fb1) ? p + shift : p; };

    for (int i = first; i < first + count; i++) {
        const Step &s = pl->steps[i];
        const bool move = s.fb && voice > 0;
        switch (s.kind) {
        case StepKind::Module: {
            Module *m = pl->inst(s.node, voice > 0 ? voice : 0);
#ifdef ENGINE_PROFILE
            const uint32_t t0 = prof_now();
#endif
            if (!move) {
                m->process(ctx, s.p);
            } else {
                Ports p = s.p;
                for (auto &x : p.in) x = rel(x);
                for (auto &x : p.mod) x = rel(x);
                m->process(ctx, p);
            }
#ifdef ENGINE_PROFILE
            prof_add(m->info().name, prof_now() - t0);
#endif
            break;
        }
        case StepKind::Mix: {
            q15 *dst = move ? const_cast<q15 *>(rel(s.dst)) : s.dst;
            int32_t acc[kBlock] = {};
            for (int k = 0; k < s.n; k++) {
                if (s.gain[k] == 0) continue;                                    // a muted source (a modulating oscillator with Mute on)
                const q15 *src = move ? rel(s.src[k]) : s.src[k];
                if (s.gain[k] == kUnity) for (int j = 0; j < kBlock; j++) acc[j] += src[j];
                else for (int j = 0; j < kBlock; j++) acc[j] += (src[j] * s.gain[k] + (1 << 14)) >> 15;
            }
            for (int j = 0; j < kBlock; j++) dst[j] = sat16(acc[j]);
            break;
        }
        case StepKind::Copy: {
            q15 *dst = move ? const_cast<q15 *>(rel(s.dst)) : s.dst;
            block_copy(dst, s.src[0]);
            break;
        }
        }
    }
}

void Engine::render(q15 *l, q15 *r) {
    if (pending_.load(std::memory_order_acquire)) {
        swap_seq_.fetch_add(1, std::memory_order_acq_rel);                       // odd: gc() must not look at the plans now
        if (Plan *np = pending_.exchange(nullptr, std::memory_order_acq_rel)) {
            for (int k = 0; k < np->n_updates; k++) np->updates[k].m->set_param(np->updates[k].idx, np->updates[k].value);
            if (np->nvoices != nvoices_) {                                       // another voice count: the voices of the old plan are not the new plan's
                for (auto &v : voices_) v = VoiceState{};
                stack_n_ = 0;
                nvoices_ = np->nvoices;
            }
            Plan *prev = active_.exchange(np, std::memory_order_acq_rel);
            if (prev) {                                                          // hand the old plan to gc(): push on the retired stack (only this thread pushes)
                prev->next_retired = retired_.load(std::memory_order_relaxed);
                while (!retired_.compare_exchange_weak(prev->next_retired, prev, std::memory_order_release, std::memory_order_relaxed)) {}
            }
        }
        swap_seq_.fetch_add(1, std::memory_order_acq_rel);
    }
    Command c;
    while (cmd_.pop(c)) apply(c);
    {                                                                            // the step sequencer: its notes of this block, like played ones
        SeqEvent ev[kSeqEvents];
        const int n = seq_.advance(tr_, kBlock, ev, kSeqEvents);
        for (int i = 0; i < n; i++) { if (ev[i].on) do_note_on(ev[i].note, ev[i].vel); else do_note_off(ev[i].note); }
        seq_step_.store(seq_.step(), std::memory_order_relaxed);
        seq_taken_.store(seq_.taken(), std::memory_order_relaxed);
        seq_where_.store(seq_.step() < 0 ? 0 : (1ull << 63) | (static_cast<uint64_t>(seq_.count()) << 16) |
                         (static_cast<uint64_t>(seq_.step()) << 8) | (seq_.frac() >> 8), std::memory_order_relaxed);
    }

    {                                                                            // the pads' sequencer: its pushes of this block, to the Pads module
        PadSeqEvent ev[kPadSeqEvents];
        const int n = pseq_.advance(tr_, kBlock, ev, kPadSeqEvents);
        const Plan *pp = n ? active_.load(std::memory_order_acquire) : nullptr;
        Module *pads = nullptr;
        for (int k = 0; pp && k < pp->n_nodes && !pads; k++) if (pp->node_id[k] == pseq_node_) pads = pp->inst(k, 0);
        if (pads && std::strcmp(pads->info().name, "Pads") != 0) pads = nullptr;     // (another graph was loaded since: that id is not the pads player)
        for (int i = 0; pads && i < n; i++) pads->set_blob(&ev[i], sizeof ev[i]);
        const int st = pseq_.step();
        pseq_step_.store(st, std::memory_order_relaxed);
        pseq_taken_.store(pseq_.taken(), std::memory_order_relaxed);
        pseq_where_.store(st < 0 ? 0 : (1ull << 63) | (static_cast<uint64_t>(pseq_.count()) << 16) | (static_cast<uint64_t>(st) << 8) | (pseq_.frac() >> 8),
                          std::memory_order_relaxed);
    }

    block_clear(l);
    block_clear(r);
    for (int i = 0; i < kBlock; i++) { bus_l_[i] = 0; bus_r_[i] = 0; bus2_l_[i] = 0; bus2_r_[i] = 0; }
    const uint64_t t = time_.fetch_add(1, std::memory_order_relaxed) + 1;
#ifdef ENGINE_PROFILE
    prof_blocks_++;
#endif
    const Plan *pl = active_.load(std::memory_order_acquire);
    if (!pl) { transport_block(l, r); return; }

    if (mode_ == VoiceMode::Mono && glide_k_ > 0) {                              // the glide: a fraction of the remaining distance per block
        VoiceState &g = voices_[0];
        if (g.active && g.pitch != g.target_pitch) {
            const int32_t d = g.target_pitch - g.pitch;
            int32_t step = static_cast<int32_t>((static_cast<int64_t>(d) * glide_k_) >> 15);
            if (step == 0) step = d > 0 ? 1 : -1;
            g.pitch += step;
        }
    }
    ProcessCtx ctx{kBlock, t, nullptr, bus_l_, bus_r_, l, r};
    ctx.bus2_l = bus2_l_; ctx.bus2_r = bus2_r_;
    ctx.transport = &tr_;
    ctx.seq = &seq_.pos();
    ctx.voices = voices_;
    ctx.nvoices = nvoices_;
    for (int v = 0; v < nvoices_; v++) ctx.keys_held += voices_[v].active && voices_[v].gate;
    run(pl, 0, pl->n_pre, -1, ctx);
    for (int v = 0; v < nvoices_; v++) {
        VoiceState &vs = voices_[v];
        if (!vs.active) continue;
        if (vs.started) {
            for (int n = 0; n < pl->n_nodes; n++)
                if (pl->node_scope[n] == Scope::Voice && pl->inst(n, v)) pl->inst(n, v)->reset();
        }
        ctx.voice = &vs;
        run(pl, pl->n_pre, pl->n_voice, v, ctx);
        vs.started = false;
        vs.age++;
        if (!vs.gate) vs.release_age++;
        if (vs.done) vs = VoiceState{};
    }
    ctx.voice = nullptr;
    run(pl, pl->n_pre + pl->n_voice, pl->n_post, -1, ctx);
    if (!pl->has_master) for (int i = 0; i < kBlock; i++) { l[i] = sat16(bus_l_[i]); r[i] = sat16(bus_r_[i]); }
    transport_block(l, r);
}

}  // namespace sc
