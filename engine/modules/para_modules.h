#pragma once
// The paraphonic mode (ADR-036 stage 2): the voices play the sources, one shared filter / amp chain runs after their sum.
//
//   GateIn   (global, runs before the voices)  outs: 0 gate = any key held (legato: a key added while others are held does not retrigger),
//                     1 retrigger gate = the same, but 0 for the block in which a new key starts while others are held (an envelope on it
//                     restarts its attack on every key), 2 pitch CV of the newest held key (as NoteIn), 3 its velocity.
//   ParaGate (voice)  in 0 signal, out 0. Lets a voice through while its key is held; a released key is muted (5 ms ramp) while other keys
//                     are still held, but the keys of the last chord keep sounding after every key is up, so the shared release is heard.
#include "engine/core/module.h"

namespace sc {

constexpr int T_GATE_IN = 72;
constexpr int T_PARA_GATE = 73;

void register_para_modules(Registry &reg);

}  // namespace sc
