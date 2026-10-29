#pragma once
// DX7-style 6-operator FM voice (ADR-025). One module = one complete voice; the patch is a plain struct sent with
// Engine::set_blob() so a whole patch edit is one atomic message to the audio thread.
//
// Sound model (the same laws as AMY's FM oscillators, which the previous backend used):
//   operator output  = amp * sin(2 pi (phase + modulation + feedback))      modulation in cycles = the modulator's output
//   feedback         = fb * (y[n-1] + y[n-2]) / 2 of the operator's raw sine
//   amp              = level_gain * env,  level_gain = 2 * 2^((level - 99) / 8),  env = 2^((egLevel - 99) / 8)
//   carriers         = summed, scaled by 1/4 and by the note velocity
// The 32 algorithms are the MSFA wiring tables (op6 .. op1). Envelopes move linearly in the DX7 level domain (6 dB per
// 8 steps), which is how the DX7 envelope behaves; AMY's extra shaping of rising segments is not reproduced.
#include "engine/core/command_queue.h"
#include "engine/core/module.h"

namespace sc {

constexpr int kDx7Ops = 6;

struct Dx7OpCfg {
    uint32_t ratio_q16;         // frequency ratio of the note (Q16.16); ignored when fixed_inc != 0
    uint32_t fixed_inc;         // phase increment per sample of a fixed-frequency operator, 0 = follow the note
    int32_t gain_q28;           // 2 * 2^((level - 99) / 8) in Q28 (max 2.0); 0 = operator off
    uint8_t eg_l[4];            // envelope levels 0..99
    uint32_t eg_ms[4];          // stage times
};

struct Dx7Patch {
    uint8_t algorithm;          // 1..32
    uint8_t reserved[3];
    int32_t feedback_q15;       // 0..1
    uint32_t release_ms;        // the tail the host should allow after note-off (informational)
    Dx7OpCfg op[kDx7Ops];       // op[0] = operator 1 ... op[5] = operator 6
};
static_assert(sizeof(Dx7Patch) <= kCmdBlobMax, "a patch must fit in one command");

// Dx7: in 0 pitch CV, in 1 gate, out 0.   gain (Q13: 8192 = 1.0, up to 4.0) is the per-patch loudness trim.
enum { DX7_GAIN, DX7_N };

void register_dx7_module(Registry &reg);
constexpr int T_DX7 = 57;

}  // namespace sc
