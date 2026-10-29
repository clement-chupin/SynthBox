#pragma once
// Structural modules every patch needs: they connect the graph to the voice manager and the output.
//
//   NoteIn    (voice)  outs: pitch CV, gate, velocity. Pitch CV: q15, 1.0 = 72 semitones above C4 (note 60),
//                      -1.0 = 72 below; one LSB = 0.5625 / 256 semitone = 0.22 cent. Gate: 0 / 32767.
//   VoiceOut  (voice)  ins: signal, signal 2. Params: Level (q15), Pan (-32768..32767), Tail (ms): longest ring-out after
//                      note-off before the voice is stolen back; In2 (0 / 1): input 2 is used. Accumulates input 1 into the
//                      voice bus and input 2 into the second bus (the rack's branch 2, ADR-041), and frees the voice once its
//                      inputs have been silent for a short time after note-off.
//   BusIn     (global) outs: L, R = the sum of all voices (runs after them). Param: Bus (0 = the voice bus, 1 = the second one).
//   MasterOut (global) ins: L, R. Params: Level (q15). Writes the engine output.
#include "engine/core/module.h"

namespace sc {

enum BuiltinType : int { T_NOTE_IN = 0, T_VOICE_OUT = 1, T_BUS_IN = 2, T_MASTER_OUT = 3, T_FIRST_USER = 16 };

constexpr int32_t kPitchCvSpan = 72 * 256;      // pitch units for CV = 1.0
constexpr int32_t kPitchCvCenter = 60 * 256;    // CV = 0 at middle C

constexpr q15 pitch_to_cv(int32_t pitch) {
    int64_t v = static_cast<int64_t>(pitch - kPitchCvCenter) * 32768 / kPitchCvSpan;
    return sat16(static_cast<int32_t>(v));
}

enum { VO_LEVEL, VO_PAN, VO_TAIL_MS, VO_IN2 };
enum { BUS_SEL };

void register_builtin_modules(Registry &reg);

}  // namespace sc
