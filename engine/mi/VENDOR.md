# Mutable Instruments code in this tree

Algorithms by Émilie Gillet (Mutable Instruments), MIT licence (`LICENSE` here; credits in `THIRD_PARTY_NOTICES.md` at the project root).
Used by the `MiOsc` module (`src/engine/modules/mi_osc.*`, ENGINE_DESIGN.md ADR-039).

| Folder | Origin | Commit |
|---|---|---|
| `braids/` | github.com/pichenettes/eurorack `braids/` | 08460a69a7e1f7a81c5a2abcc7189c9a6b7208d4 |
| `plaits/` | github.com/pichenettes/eurorack `plaits/` | 08460a69a7e1f7a81c5a2abcc7189c9a6b7208d4 |
| (stmlib, not kept) | github.com/pichenettes/stmlib | d18def816c51d1da0c108236928b2bbd25c17481 |

Only the files the selected models need are here (the include closure of `braids/macro_oscillator` and of the ten Plaits engines of tiers 1-2).
The `.cc` files are renamed `.cpp` so the three builds (simulator, engine tests, PlatformIO) compile them with the rest of `src/engine`.

## No stmlib: its helpers live in our framework

Every `#include "stmlib/..."` now names one of our headers, and `stmlib::` is `sc::fdsp::`:

| stmlib | framework |
|---|---|
| `stmlib.h`, `dsp/dsp.h`, `utils/dsp.h`, `dsp/parameter_interpolator.h`, `dsp/polyblep.h`, `dsp/rsqrt.h`, `dsp/hysteresis_quantizer.h`, `utils/buffer_allocator.h` | `engine/dsp/fdsp.h` |
| `dsp/filter.h` (only `OnePole` and `Svf`) | `engine/dsp/fdsp_filter.h` |
| `dsp/units.h` / `.cc` | `engine/dsp/fdsp_units.h` / `.cpp` |
| `utils/random.h` / `.cc` | `engine/dsp/fdsp_random.h` / `.cpp` |

They are ported unchanged (bit-identical), except: `Clip16` is our `sat16` (identical); `Sqrt` is `sqrtf` (stmlib used an ARM instruction);
the ARM-only `ssat` / `usat` paths are gone. Replacing more helpers by our own (svf.h, util.h ...) is done one at a time under an A/B test.
`braids/` and `plaits/` includes are absolute (`engine/mi/braids/...`), so no include path is added to the builds.

## Local changes (search "SevenSynth" in the sources)

1. **Sample rate.** `plaits/dsp/dsp.h`: `kSampleRate` = `ENGINE_SR`, `kCorrectedSampleRate` = `kSampleRate` (no hardware clock correction).
   Braids: the tables that depend on the rate (`lut_oscillator_increments`, `_delays`, `lut_resonator_*`, `lut_svf_*`, `wav_bandlimited_comb_*`)
   are removed from `braids/resources.cpp` and generated for each supported `ENGINE_SR` into `braids/resources_sr.cpp` by
   `framework/tools/gen_mi_tables.py` (same formulas; checked equal to the original 96 kHz values). Braids' per-sample time constants that are not in
   tables (kick decay, noise clocks ...) were written for 96 kHz and run slower at 44.1 kHz.
2. **Braids delay lines.** `DigitalOscillator`'s 16 KB member union of delay lines is now a pointer (`set_delay_lines`, NULL after `Init`):
   the waveguide / comb / plucked models (not used yet) render silence without one. An instance is 688 bytes instead of ~17 KB.
3. **Braids settings.** `braids/settings.h` keeps only the `MacroOscillatorShape` enum (the module's UI settings and flash storage are not used).
4. **Plaits drums.** `bass_drum_engine`, `snare_drum_engine`, `hi_hat_engine`: each of the two models (main / aux output) is rendered only when
   its output pointer is not NULL. `MiOsc` exposes them as separate models (PBass / PBassS, PSnare / PSnrS, PHat / PHat2): one model's cost each.
5. **M_PI.** Defined in `engine/dsp/fdsp.h` (strict C++17 does not define it).

## Updating from upstream

Copy the new upstream file over the one here, then redo the changes above (they are marked "SevenSynth"), rename `.cc` to `.cpp`, rewrite the
includes (`stmlib/...` -> `engine/dsp/fdsp*.h`, `braids/...` -> `engine/mi/braids/...`) and `stmlib::` -> `sc::fdsp::`; if a table of
`braids/resources/lookup_tables.py` changed, update `framework/tools/gen_mi_tables.py` and run `python framework/tools/gen_mi_tables.py --check <upstream folder>`.
