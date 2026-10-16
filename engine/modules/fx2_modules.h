#pragma once
// More effects. All global scope and stereo (ins L, R; outs L, R) except Comb (a voice-scope processor). Parameters are int32 in the
// units given below; see ENGINE_DESIGN.md (ADR-031) for the choices.
#include "engine/core/command_queue.h"
#include "engine/core/module.h"

namespace sc {

enum Fx2Type : int { T_PHASER = 61, T_FLANGER, T_TREMOLO, T_COMP, T_EQ3, T_SHIFTER, T_CONV, T_COMB,
                     // per-voice (mono: in 0 / out 0, same parameters) versions of the cheap ones, for a branch of the rack before its Para point (ADR-041)
                     T_PHASER_V = 74, T_FLANGER_V, T_TREMOLO_V, T_COMP_V, T_EQ3_V, T_SHIFTER_V };

// Phaser: 2..8 first-order all-pass stages (stages 1..4 = 2, 4, 6, 8) swept by a triangle LFO (R channel 90 degrees ahead).
//   rate  pitch units relative to 1 Hz (12 * 256 per octave)     depth q15 (x 4 octaves of sweep)     center pitch units (MIDI * 256)
//   feedback q15 signed     mix q15 (0.5 = deepest notches)
enum { PHS_RATE, PHS_DEPTH, PHS_CENTER, PHS_FEEDBACK, PHS_STAGES, PHS_MIX, PHS_N };

// Flanger: a modulated delay of the input with feedback.   delay 1/16 ms (center, 0.5 .. 10 ms)   depth q15 (fraction of the delay)
//   rate like the phaser     feedback q15 signed     mix q15 (wet added to the dry signal)
enum { FLG_RATE, FLG_DEPTH, FLG_DELAY, FLG_FEEDBACK, FLG_MIX, FLG_N };

// Tremolo / auto-pan.   rate like the phaser   depth q15   shape 0 sine, 1 triangle, 2 soft square   mode 0 tremolo (both channels), 1 auto-pan
enum { TRM_RATE, TRM_DEPTH, TRM_SHAPE, TRM_MODE, TRM_N };

// Compressor (stereo linked, feed-forward peak detector).   threshold dB (-60 .. 0)   ratio x10 (10 .. 200)   attack ms   release ms
//   makeup dB (0 .. 24)   mix q15 (parallel compression)
enum { CMP_THRESH, CMP_RATIO, CMP_ATTACK, CMP_RELEASE, CMP_MAKEUP, CMP_MIX, CMP_N };

// 3-band EQ: low shelf (150 Hz), peaking mid, high shelf (6 kHz).   gains in 1/10 dB (+-15 dB)   midf pitch units
enum { EQ_LOW, EQ_MID, EQ_MIDF, EQ_HIGH, EQ_N };

// Ring modulator / frequency shifter.   mode 0 ring, 1 shift up, 2 shift down   freq pitch units relative to 1 Hz   mix q15
enum { SFT_MODE, SFT_FREQ, SFT_MIX, SFT_N };

// Convolver (short impulse response, direct form, stereo): built-in cabinets and body resonances, or any IR, played from set_blob.
//   ir / length: what the mapper asked for (the module plays the IR it last received whole, see ConvBlob)     mix q15
//   level q15 (output trim, 0.5 = unity of the normalised IR)
// The IR is computed at control time (conv_builtin_ir: double maths, too slow for the audio thread on the S3) and sent in ConvBlob chunks;
// the module switches IR and length together when the last chunk has arrived. Until then it plays the 1x12 cab at 256 taps (from init).
// On the ESP32-S3 the dot products run on PIE (pie_conv2_s16 in fft_s3.S, 8 multiply-adds per instruction), bit-identical to the C path.
enum { CNV_IR, CNV_LENGTH, CNV_MIX, CNV_LEVEL, CNV_N };
enum { CNVIR_CAB_1X12, CNVIR_CAB_4X12, CNVIR_CAB_BRIGHT, CNVIR_CAB_DARK, CNVIR_ACOUSTIC, CNVIR_VIOLIN, CNVIR_DRUM, CNVIR_PHONE, CNVIR_CUSTOM, CNVIR_COUNT };
constexpr int kConvMaxTaps = 512;
constexpr int kConvChunk = 120;
// One chunk of an IR of `total` taps: taps [start, start + count). The IR becomes active when a chunk ends at `total`.
struct ConvBlob { uint16_t start, count, total, pad; int16_t tap[kConvChunk]; };
static_assert(sizeof(ConvBlob) <= kCmdBlobMax, "a chunk must fit in one command");
// The built-in IR `which` (CNVIR_*) at `len` taps (16 .. 512) as the q15 taps the Convolver plays, into out[kConvMaxTaps] (zeros after len).
// Control time only: double maths, about 5 KB of stack.
void conv_builtin_ir(int which, int len, int16_t *out);
// Fills `out` with the chunks that send `n` taps (n <= kConvMaxTaps); returns how many (at most 5).
int conv_blobs(const int16_t *taps, int n, ConvBlob *out);

// Comb (voice scope): in 0 audio, in 1 pitch CV; out 0. A tuned feedback comb (resonator) that follows the key.
//   tune pitch units (the note plays at `tune` + CV)   feedback q15 signed (negative = odd harmonics only)   damp cutoff pitch units
//   interval semitones of a second comb added to the first (0 = off)   mix q15
enum { CMB_TUNE, CMB_FEEDBACK, CMB_DAMP, CMB_INTERVAL, CMB_MIX, CMB_N };

void register_fx2_modules(Registry &reg);

}  // namespace sc
