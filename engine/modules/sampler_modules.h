#pragma once
// Sample playback modules (ADR-021..023). They need a SampleBank in Memory::bank (see sampler/sample_bank.h).
#include "engine/core/module.h"

namespace sc {

enum SamplerType : int { T_SAMPLER = 54, T_GRANULAR_V, T_GRANULAR_G };

// Sampler (voice): in 0 pitch CV (like Osc), out 0.   mod: tune (+-96 semitones at full scale)
//   sample / instrument  bank slot id / instrument id (an instrument picks the zone for the note and velocity;
//                        -1 = unused). Zones also override root, tune and gain.
//   tune                 pitch units (1/256 semitone) added to the played pitch
//   root                 pitch units of the note that plays the sample at its original speed; -1 = from the file
//   loop                 -1 = file default, 0 off, 1 forward, 2 ping-pong      reverse  0 / 1
//   start                offset in 1/16 ms from the start of the sample or slice
//   slice                -1 = whole sample, else start at slice n        slice_mode  0 = play on, 1 = stop at the next slice
//   gain q15, interp 0 = linear / 1 = Hermite, track 1 = follow the keyboard pitch / 0 = always original speed (drums)
//   fstart / fend        a trim in frames (the pads): fstart >= 0 replaces start / slice; fend > 0 stops there (0 = the file's end)
//   region               1 = loop fstart..fend forward (replaces the loop setting)
enum { SMPR_SAMPLE, SMPR_INSTRUMENT, SMPR_TUNE, SMPR_ROOT, SMPR_LOOP, SMPR_REVERSE, SMPR_START, SMPR_SLICE, SMPR_SLICE_MODE,
       SMPR_GAIN, SMPR_INTERP, SMPR_TRACK, SMPR_FSTART, SMPR_FEND, SMPR_REGION, SMPR_N };

// Granular (voice or global): in 0 pitch CV, out 0. Reads the RAM head of a sample (load it with head_ms = 0 to make
// it fully resident). position q15 = place in the sample, speed Q8 (256 = real time, 0 = frozen, negative = backwards),
// size ms, density grains/s, pitch in pitch units, jitter q15 = random position spread, level q15.
enum { GRN_SAMPLE, GRN_POSITION, GRN_SPEED, GRN_SIZE, GRN_DENSITY, GRN_PITCH, GRN_JITTER, GRN_LEVEL, GRN_N };

// Pads (global, ADR-041 decision 21): the Pads section's player. 8 voices, each a Sampler at a fixed pitch, then a low-pass filter, a fade
// and the pad's level / pan into the stereo bus it is sent to (PadBlob.bus): out 0 / 1 = Dry, 2 / 3 = A, 4 / 5 = B, 6 / 7 = C, the entries
// of the Pads section's FX chains (ADR-041 decision 23; the graph decides how the chains follow each other). Each pad is mono: a push restarts it (the old sound fades in kPadCutMs). A choke group
// stops its other pads; the oldest voice is stolen when all are busy. The pads' settings come as PadBlob (set_blob, one per pad, at build and
// on every change); the pushes as parameters whose value is (n << 8) | pad, n a counter that starts at 1 (0 = nothing: the value a rebuild
// re-applies), so two pushes of the same pad in one block are two events. A pad with a grid (beat / bar) waits for the next beat / bar of
// the running clock (ProcessCtx::transport), block-accurate (it starts in the block nearest to it), or with Sync early enough for its loop
// start to land on the grid (a bar when it has no grid of its own); with the clock stopped it starts at
// once. A gate pad released while it waits, or a toggle pad pushed again, does not start; a sounding toggle pad stops at its push.
enum { T_PADS = 80 };
enum { PADS_PUSH, PADS_RELEASE, PADS_STOP, PADS_N };
constexpr int kPadCount = 64;
constexpr int kPadVoices = 8;
constexpr int kPadBuses = 4;                    // Dry, A, B, C
constexpr int kPadCutMs = 5;                    // the fade of a restarted, choked or stolen pad
enum PadMode : uint8_t { PADM_ONESHOT, PADM_GATE, PADM_TOGGLE };
struct PadBlob {
    uint8_t pad = 0;                            // 0..kPadCount-1
    uint8_t bus = 0;                            // where it sounds: 0 Dry, 1 FXA, 2 FXB, 3 FXC
    uint8_t mode = PADM_ONESHOT, loop = 0, choke = 0, mute = 0, filter = 0;   // loop: 0 off, 1 start..end, 2 the file's loop points
    uint8_t grid = 0;                           // start on the grid: 0 off (the push), else its unit in beats (1, 2, 4 .. 32)
    uint8_t sync = 0;                           // 1: the loop start lands on the grid (the pad starts `preroll` samples before it)
    int32_t preroll = 0;                        // output samples from the start to the loop start (at the pad's tune)
    uint8_t from = 0;                           // a grid start plays from 0 the start, 1 the loop start (when it loops)
    uint8_t xpost = 0;                          // the crossfade's fade-in: 0 before the loop start, 1 from it
    int32_t lstart = 0;                         // the loop start in frames (the Trim loop's own, or the file's)
    int32_t xfade = 0;                          // the loop crossfade in frames (0: none)
    int8_t next = -1;                           // the pad started when this one ends by itself or is stopped by hand (toggle off, gate
                                                // release; not Stop all, a choke or a restart), -1 = none (user, 2026-10-10)
    int16_t slot = -1;                          // bank slot of the sample, -1 = an empty pad
    int32_t start = 0, end = 0;                 // frames; end 0 = the file's end
    int32_t tune = 0;                           // pitch units (1/256 semitone)
    int32_t tail = 0;                           // fade after a stop, in samples
    q15 gain_l = 0, gain_r = 0;                 // level x pan
    float a1 = 1.0f, a2 = 0.0f, a3 = 0.0f, k = 1.0f;   // low-pass SVF coefficients (dsp/svf.h, SvfCoefF), filter = 1
};
// A push from the pads' sequencer (engine/core/sequencer.h PadSeqEvent, by set_blob: its size tells it from a PadBlob): the pad starts
// at once whatever its grid setting says, at (level + 1) / 16 of its level; a Toggle pad starts too (it is not switched off by the
// push). `release`: the pad is let go: a Gate or Toggle pad stops with its tail, a one-shot plays on. Its Next is not started.
// [any thread] the pads sounding now, bit i = pad i (published by the Pads module once per block; 0 when there is none), and where a pad's
// sound is in its file (frames; -1 when it does not sound).
uint64_t pads_playing();
int32_t pads_position(int pad);

void register_sampler_modules(Registry &reg);

}  // namespace sc
