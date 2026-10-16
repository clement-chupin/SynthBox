// Ported from stmlib (Mutable Instruments, (c) 2012-2017 Emilie Gillet, MIT licence: see the notice in src/engine/mi/LICENSE) into the
// framework so the Braids / Plaits algorithms (src/engine/mi) depend on our code only. Namespace sc::fdsp ("float DSP" helpers, plus the
// integer table helpers of Braids), kept bit-identical to the originals: replacing a helper by one of our own (svf.h, util.h ...) is done
// under an A/B test (ENGINE_DESIGN.md ADR-039).
#include "engine/dsp/fdsp_random.h"

namespace sc {
namespace fdsp {

/* static */
uint32_t Random::rng_state_ = 0x21;

}  // namespace fdsp
}  // namespace sc
