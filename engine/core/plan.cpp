#include "engine/core/plan.h"

namespace sc {

const char *err_name(Err e) {
    switch (e) {
        case Err::Ok: return "ok";
        case Err::NoMem: return "out of memory";
        case Err::UnknownType: return "unknown module type";
        case Err::DupId: return "duplicate node id";
        case Err::BadEdge: return "bad cable (unknown node or port)";
        case Err::Scope: return "voice module cannot feed a global module (use VoiceOut)";
        case Err::Cycle: return "feedback loop without a delayed cable";
        case Err::FanIn: return "too many cables into one input";
        case Err::TooBig: return "graph too large";
        case Err::BusLoop: return "voice input depends on the voice sum (use a delayed cable)";
    }
    return "?";
}

namespace {

constexpr int kMaxSteps = 192;
constexpr int kMaxBufs = 384;
constexpr int kMaxFb = 32;
constexpr int16_t kNone = -1;
constexpr int16_t kSilence = -2;
constexpr int16_t kFbVoice = 1000;      // + k
constexpr int16_t kFbGlobal = 2000;     // + k

struct Sym {                            // a step with symbolic buffer ids
    StepKind kind;
    uint8_t node, n;
    int16_t in[kMaxIn], mod[kMaxParams], out[kMaxOut];
    int16_t dst;
    int16_t src[kMaxFanIn];
    q15 gain[kMaxFanIn];
};

struct FbEntry { int node, port; bool voice; };

struct Scratch {
    const ModuleType *type[kMaxNodes];
    Scope scope[kMaxNodes];
    int16_t idx_of[256];
    uint8_t fanin[kMaxNodes][2][kMaxParams];
    int order_voice[kMaxNodes], n_order_voice;
    int order_pre[kMaxNodes], n_pre_nodes;
    int order_post[kMaxNodes], n_post_nodes;
    bool post[kMaxNodes];
    int16_t outbuf[kMaxNodes][kMaxOut];
    Sym sym[kMaxSteps];
    int n_sym;
    int buf_def[kMaxBufs], buf_last[kMaxBufs], slot_of[kMaxBufs], slot_last[kMaxBufs];
    int n_buf, n_slots;
    FbEntry fb[kMaxFb];
    int n_fb, n_fbv, n_fbg;
    int16_t fb_id[kMaxFb];
    int sec_end[3];
    uint8_t edge_step[kMaxEdges], edge_slot[kMaxEdges];
};

struct Compiler {
    const GraphDesc &g;
    Scratch &S;
    Err err = Err::Ok;
    explicit Compiler(const GraphDesc &gd, Scratch &s) : g(gd), S(s) {}

    int16_t new_buf(int def) {
        if (S.n_buf >= kMaxBufs) { err = Err::TooBig; return kNone; }
        S.buf_def[S.n_buf] = def;
        S.buf_last[S.n_buf] = def;
        return static_cast<int16_t>(S.n_buf++);
    }
    void use(int16_t id, int pos) { if (id >= 0 && S.buf_last[id] < pos) S.buf_last[id] = pos; }

    Sym *append() {
        if (S.n_sym >= kMaxSteps) { err = Err::TooBig; return nullptr; }
        Sym *s = &S.sym[S.n_sym++];
        *s = Sym{};
        for (auto &v : s->in) v = kNone;
        for (auto &v : s->mod) v = kNone;
        for (auto &v : s->out) v = kNone;
        s->dst = kNone;
        for (auto &v : s->src) v = kNone;
        return s;
    }

    int16_t fb_buffer(int node, int port) {
        for (int k = 0; k < S.n_fb; k++)
            if (S.fb[k].node == node && S.fb[k].port == port) return S.fb_id[k];
        if (S.n_fb >= kMaxFb) { err = Err::TooBig; return kNone; }
        bool voice = S.scope[node] == Scope::Voice;
        S.fb[S.n_fb] = {node, port, voice};
        S.fb_id[S.n_fb] = static_cast<int16_t>(voice ? kFbVoice + S.n_fbv++ : kFbGlobal + S.n_fbg++);
        return S.fb_id[S.n_fb++];
    }

    int16_t edge_source(const EdgeDesc &e) {
        int si = S.idx_of[e.src_id];
        if (e.delayed) return fb_buffer(si, e.src_port);
        return S.outbuf[si][e.src_port];
    }

    // The buffer a node input / parameter reads: nothing, an alias of one cable, or a MIX of several.
    int16_t resolve(int node, Dst kind, int port) {
        int found[kMaxFanIn], nf = 0;
        for (int k = 0; k < g.n_edges; k++) {
            const EdgeDesc &e = g.edge[k];
            if (S.idx_of[e.dst_id] == node && e.dst_kind == kind && e.dst_port == port && nf < kMaxFanIn) found[nf++] = k;
        }
        if (nf == 0) return kind == Dst::In ? kSilence : kNone;
        if (nf == 1 && g.edge[found[0]].depth == kUnity) return edge_source(g.edge[found[0]]);
        int pos = S.n_sym;
        int16_t tmp = new_buf(pos);
        Sym *m = append();
        if (!m || tmp == kNone) return kNone;
        m->kind = StepKind::Mix;
        m->n = static_cast<uint8_t>(nf);
        m->dst = tmp;
        for (int i = 0; i < nf; i++) {
            m->src[i] = edge_source(g.edge[found[i]]);
            m->gain[i] = g.edge[found[i]].depth;
            S.edge_step[found[i]] = static_cast<uint8_t>(pos);
            S.edge_slot[found[i]] = static_cast<uint8_t>(i);
            use(m->src[i], pos);
        }
        return tmp;
    }

    void emit_node(int node) {
        const ModuleInfo &info = *S.type[node]->info;
        int16_t in[kMaxIn], mod[kMaxParams];
        for (int p = 0; p < kMaxIn; p++) in[p] = p < info.n_in ? resolve(node, Dst::In, p) : kNone;
        for (int p = 0; p < kMaxParams; p++) mod[p] = p < info.n_param ? resolve(node, Dst::Param, p) : kNone;
        int pos = S.n_sym;
        Sym *s = append();
        if (!s) return;
        s->kind = StepKind::Module;
        s->node = static_cast<uint8_t>(node);
        for (int p = 0; p < kMaxIn; p++) { s->in[p] = in[p]; use(in[p], pos); }
        for (int p = 0; p < kMaxParams; p++) { s->mod[p] = mod[p]; use(mod[p], pos); }
        for (int o = 0; o < info.n_out; o++) { s->out[o] = new_buf(pos); S.outbuf[node][o] = s->out[o]; }
    }

    // Copies fed-back outputs into their persistent buffers at the end of a section.
    void emit_fb_copies(bool voice_section, bool post_section) {
        for (int k = 0; k < S.n_fb; k++) {
            int node = S.fb[k].node;
            bool mine;
            if (voice_section) mine = S.fb[k].voice;
            else mine = !S.fb[k].voice && S.post[node] == post_section;
            if (!mine) continue;
            int pos = S.n_sym;
            Sym *c = append();
            if (!c) return;
            c->kind = StepKind::Copy;
            c->dst = S.fb_id[k];
            c->src[0] = S.outbuf[node][S.fb[k].port];
            use(c->src[0], pos);
        }
    }
};

bool is_voice(const Scratch &S, int i) { return S.scope[i] == Scope::Voice; }

// Kahn's algorithm over the non-delayed cables inside one scope; ties broken by declaration order.
bool topo(const GraphDesc &g, Scratch &S, int n, bool voice, int *order, int *count) {
    int indeg[kMaxNodes] = {};
    bool done[kMaxNodes] = {};
    int total = 0;
    for (int i = 0; i < n; i++) if (is_voice(S, i) == voice) total++;
    for (int k = 0; k < g.n_edges; k++) {
        const EdgeDesc &e = g.edge[k];
        int si = S.idx_of[e.src_id], di = S.idx_of[e.dst_id];
        if (!e.delayed && is_voice(S, si) == voice && is_voice(S, di) == voice) indeg[di]++;
    }
    *count = 0;
    while (*count < total) {
        int pick = -1;
        for (int i = 0; i < n && pick < 0; i++) if (is_voice(S, i) == voice && !done[i] && indeg[i] == 0) pick = i;
        if (pick < 0) return false;
        done[pick] = true;
        order[(*count)++] = pick;
        for (int k = 0; k < g.n_edges; k++) {
            const EdgeDesc &e = g.edge[k];
            if (!e.delayed && S.idx_of[e.src_id] == pick && is_voice(S, S.idx_of[e.dst_id]) == voice) indeg[S.idx_of[e.dst_id]]--;
        }
    }
    return true;
}

Err compile_impl(const GraphDesc &g, const Registry &reg, Heap &heap, int nvoices, Scratch &S, Plan **out) {
    const int n = g.n_nodes;
    for (int i = 0; i < 256; i++) S.idx_of[i] = -1;
    for (int k = 0; k < kMaxEdges; k++) { S.edge_step[k] = 255; S.edge_slot[k] = 0; }
    for (int i = 0; i < n; i++) {
        S.type[i] = reg.get(g.node[i].type);
        if (!S.type[i]) return Err::UnknownType;
        if (S.idx_of[g.node[i].id] >= 0) return Err::DupId;
        S.idx_of[g.node[i].id] = static_cast<int16_t>(i);
        S.scope[i] = S.type[i]->info->scope;
    }
    for (int k = 0; k < g.n_edges; k++) {
        const EdgeDesc &e = g.edge[k];
        int si = S.idx_of[e.src_id], di = S.idx_of[e.dst_id];
        if (si < 0 || di < 0) return Err::BadEdge;
        const ModuleInfo &s = *S.type[si]->info, &d = *S.type[di]->info;
        if (e.src_port >= s.n_out) return Err::BadEdge;
        if (e.dst_kind == Dst::In ? e.dst_port >= d.n_in : e.dst_port >= d.n_param) return Err::BadEdge;
        if (S.scope[si] == Scope::Voice && S.scope[di] == Scope::Global) return Err::Scope;
        if (++S.fanin[di][e.dst_kind == Dst::In ? 0 : 1][e.dst_port] > kMaxFanIn) return Err::FanIn;
    }

    int gorder[kMaxNodes], ng = 0;
    if (!topo(g, S, n, true, S.order_voice, &S.n_order_voice)) return Err::Cycle;
    if (!topo(g, S, n, false, gorder, &ng)) return Err::Cycle;

    // Globals that read the voice sum (directly or through other globals) run after the voices.
    for (int a = 0; a < ng; a++) {
        int i = gorder[a];
        S.post[i] = S.type[i]->info->reads_bus;
        for (int k = 0; k < g.n_edges; k++) {
            const EdgeDesc &e = g.edge[k];
            if (!e.delayed && S.idx_of[e.dst_id] == i && !is_voice(S, S.idx_of[e.src_id]) && S.post[S.idx_of[e.src_id]]) S.post[i] = true;
        }
        if (S.post[i]) S.order_post[S.n_post_nodes++] = i; else S.order_pre[S.n_pre_nodes++] = i;
    }
    for (int k = 0; k < g.n_edges; k++) {
        const EdgeDesc &e = g.edge[k];
        int si = S.idx_of[e.src_id], di = S.idx_of[e.dst_id];
        if (!e.delayed && !is_voice(S, si) && S.post[si] && is_voice(S, di)) return Err::BusLoop;
    }

    // Symbolic steps
    Compiler c(g, S);
    for (int a = 0; a < S.n_pre_nodes; a++) c.emit_node(S.order_pre[a]);
    c.emit_fb_copies(false, false);
    S.sec_end[0] = S.n_sym;
    for (int a = 0; a < S.n_order_voice; a++) c.emit_node(S.order_voice[a]);
    c.emit_fb_copies(true, false);
    S.sec_end[1] = S.n_sym;
    for (int a = 0; a < S.n_post_nodes; a++) c.emit_node(S.order_post[a]);
    c.emit_fb_copies(false, true);
    S.sec_end[2] = S.n_sym;
    if (c.err != Err::Ok) return c.err;

    // Buffer pool: first fit by liveness (a slot is reusable only after the last reader has run)
    for (int b = 0; b < S.n_buf; b++) {
        int s = 0;
        while (s < S.n_slots && S.slot_last[s] >= S.buf_def[b]) s++;
        if (s == S.n_slots) S.n_slots++;
        S.slot_of[b] = s;
        S.slot_last[s] = S.buf_last[b];
    }

    // Allocate and resolve
    Plan *pl = heap.make<Plan>();
    if (!pl) return Err::NoMem;
    pl->heap = &heap;
    pl->nvoices = nvoices > 0 ? nvoices : 1;
    pl->n_nodes = n;
    for (int i = 0; i < n; i++) { pl->node_id[i] = g.node[i].id; pl->node_type[i] = g.node[i].type; pl->node_scope[i] = S.scope[i]; }
    for (int k = 0; k < kMaxEdges; k++) { pl->edge_step[k] = S.edge_step[k]; pl->edge_slot[k] = S.edge_slot[k]; }
    pl->n_pre = S.sec_end[0];
    pl->n_voice = S.sec_end[1] - S.sec_end[0];
    pl->n_post = S.sec_end[2] - S.sec_end[1];
    pl->n_slots = S.n_slots;
    pl->fbv_stride = S.n_fbv * kBlock;
    pl->steps = heap.alloc_array<Step>(S.n_sym > 0 ? static_cast<size_t>(S.n_sym) : 1);
    pl->pool = heap.alloc_array<q15>(static_cast<size_t>(S.n_slots > 0 ? S.n_slots : 1) * kBlock);
    pl->silence = heap.alloc_array<q15>(kBlock);
    pl->fbv = heap.alloc_array<q15>(static_cast<size_t>(S.n_fbv > 0 ? S.n_fbv : 1) * kBlock * static_cast<size_t>(nvoices > 0 ? nvoices : 1));
    pl->fbg = heap.alloc_array<q15>(static_cast<size_t>(S.n_fbg > 0 ? S.n_fbg : 1) * kBlock);
    pl->inst_ = heap.alloc_array<Module *>(static_cast<size_t>(n > 0 ? n : 1) * static_cast<size_t>(pl->nvoices));
    if (pl->inst_) for (int k = 0; k < (n > 0 ? n : 1) * pl->nvoices; k++) pl->inst_[k] = nullptr;
    if (!pl->steps || !pl->pool || !pl->silence || !pl->fbv || !pl->fbg || !pl->inst_) { free_plan(pl); return Err::NoMem; }

    auto ptr = [&](int16_t id) -> q15 * {
        if (id >= kFbGlobal) return pl->fbg + (id - kFbGlobal) * kBlock;
        if (id >= kFbVoice) return pl->fbv + (id - kFbVoice) * kBlock;
        if (id == kSilence) return pl->silence;
        if (id == kNone) return nullptr;
        return pl->pool + S.slot_of[id] * kBlock;
    };
    auto is_vfb = [](int16_t id) { return id >= kFbVoice && id < kFbGlobal; };

    for (int i = 0; i < S.n_sym; i++) {
        const Sym &y = S.sym[i];
        Step &st = pl->steps[i];
        st = Step{};
        st.kind = y.kind;
        st.node = y.node;
        st.n = y.n;
        bool in_voice = i >= S.sec_end[0] && i < S.sec_end[1];
        bool fb = false;
        for (int p = 0; p < kMaxIn; p++) { st.p.in[p] = ptr(y.in[p]); fb |= is_vfb(y.in[p]); }
        for (int p = 0; p < kMaxParams; p++) { st.p.mod[p] = ptr(y.mod[p]); fb |= is_vfb(y.mod[p]); }
        for (int o = 0; o < kMaxOut; o++) st.p.out[o] = ptr(y.out[o]);
        st.dst = ptr(y.dst);
        fb |= is_vfb(y.dst);
        for (int k = 0; k < kMaxFanIn; k++) { st.src[k] = ptr(y.src[k]); st.gain[k] = y.gain[k]; fb |= is_vfb(y.src[k]); }
        st.fb = in_voice && fb;
    }
    *out = pl;
    return Err::Ok;
}

}  // namespace

Err compile_plan(const GraphDesc &g, const Registry &reg, Heap &heap, int nvoices, Plan **out) {
    *out = nullptr;
    Scratch *S = heap.make<Scratch>();
    if (!S) return Err::NoMem;
    Err e = compile_impl(g, reg, heap, nvoices, *S, out);
    heap.free(S);
    return e;
}

void free_plan(Plan *p) {
    if (!p) return;
    Heap *h = p->heap;
    h->free(p->steps);
    h->free(p->pool);
    h->free(p->silence);
    h->free(p->fbv);
    h->free(p->fbg);
    h->free(p->updates);
    h->free(p->inst_);
    h->free(p);
}

}  // namespace sc
