#pragma once
// Graph description: what the UI / rack builds and the plan compiler consumes. Plain data, no pointers.
#include <cstdint>
#include "engine/core/module.h"

namespace sc {

constexpr int kMaxNodes = 64;
constexpr int kMaxEdges = 160;
constexpr int kMaxFanIn = 8;        // cables into one input/parameter

enum class Dst : uint8_t { In, Param };

struct NodeDesc {
    uint8_t id = 0;                 // unique and stable: keeps the module state across rebuilds
    uint8_t type = 0;               // Registry id
    int32_t param[kMaxParams] = {};
};

struct EdgeDesc {
    uint8_t src_id = 0, src_port = 0;
    uint8_t dst_id = 0, dst_port = 0;
    Dst dst_kind = Dst::In;
    q15 depth = kUnity;             // signed weight of the cable
    bool delayed = false;           // one-block delay: the only way to close a feedback loop
};

struct GraphDesc {
    NodeDesc node[kMaxNodes];
    int n_nodes = 0;
    EdgeDesc edge[kMaxEdges];
    int n_edges = 0;

    // Adds a node with the type's default parameters. Returns nullptr when full or the type is unknown.
    NodeDesc *add_node(const Registry &reg, int id, int type) {
        const ModuleType *t = reg.get(type);
        if (!t || n_nodes >= kMaxNodes) return nullptr;
        NodeDesc &n = node[n_nodes++];
        n = NodeDesc{};
        n.id = static_cast<uint8_t>(id);
        n.type = static_cast<uint8_t>(type);
        for (int i = 0; i < t->info->n_param; i++) n.param[i] = t->info->param[i].def;
        return &n;
    }
    NodeDesc *find(int id) {
        for (int i = 0; i < n_nodes; i++) if (node[i].id == id) return &node[i];
        return nullptr;
    }
    bool connect(int src_id, int src_port, int dst_id, Dst kind, int dst_port, q15 depth = kUnity, bool delayed = false) {
        if (n_edges >= kMaxEdges) return false;
        EdgeDesc &e = edge[n_edges++];
        e.src_id = static_cast<uint8_t>(src_id); e.src_port = static_cast<uint8_t>(src_port);
        e.dst_id = static_cast<uint8_t>(dst_id); e.dst_port = static_cast<uint8_t>(dst_port);
        e.dst_kind = kind; e.depth = depth; e.delayed = delayed;
        return true;
    }
};

}  // namespace sc
