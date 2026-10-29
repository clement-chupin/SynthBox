#pragma once
// Motion sequencer (the MS rack module): up to four lanes of 16 steps, each lane a value 0..100 per step that may be
// active or not. It outputs one control signal per lane, so "motion" is just a cable into any parameter.
//
// Clock: it has none. It follows the engine's step sequencer (ProcessCtx::seq, engine/core/sequencer.h): the step that plays and how far
// into it, whatever the rate, the swing and the direction are, so both are always in step. A pattern longer than the lanes' 16 steps
// plays them again every 16 steps. It is silent while the sequencer does not play.
//
// Lane model (DEVELOPING.md): interpolation is always between ACTIVE steps. At position p (in steps, fractional)
//   prev = the last active step at or before floor(p), wrapping;  next = the first active step after it, wrapping.
//   STEP:   value of prev.           LINEAR: lerp prev -> next by (p - prev) / (next - prev), distances modulo the step count.
//   One active step = constant; no active step (within the first `steps` steps) = the lane outputs 0.
// Output: bipolar lanes map 0..100 to -1..+1, unipolar lanes to 0..+1 (q15). Inside a block the output ramps linearly
// from where the last block ended to the value at this block's end, so a step edge never clicks.
#include "engine/core/command_queue.h"
#include "engine/core/module.h"

namespace sc {

constexpr int kMsLanes = 4;
constexpr int kMsSteps = 16;

struct MotionBlob {
    uint8_t val[kMsLanes][kMsSteps];     // 0..100
    uint16_t active[kMsLanes];           // bit i = step i is active
    uint8_t linear[kMsLanes];            // 0 = STEP, 1 = LINEAR
    uint8_t bipolar[kMsLanes];           // 0 = unipolar, 1 = bipolar
};
static_assert(sizeof(MotionBlob) <= kCmdBlobMax, "the lanes must fit in one command");

// MotionSeq: out 0..3 = lane 1..4. No parameters. The steps it plays of a pattern of `steps` steps:
inline int motion_steps(int steps) { return steps < 1 ? 1 : (steps > kMsSteps ? kMsSteps : steps); }

// The lane value (0..65536 * 100 fixed point is avoided: returns value * 65536, i.e. Q16 in 0..100) at a point of the
// pattern; exposed for the tests, which compare the module against it. `pos_q16` is the position in steps (Q16).
int64_t motion_lane_value_q16(const MotionBlob &b, int lane, int steps, uint32_t pos_q16);

void register_motion_module(Registry &reg);
constexpr int T_MSEQ = 58;

}  // namespace sc
