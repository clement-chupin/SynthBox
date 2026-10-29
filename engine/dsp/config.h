#pragma once
// Engine-wide compile-time constants (ADR-004). Override with -D on the compiler command line.
// Everything time-based in the engine is derived from these; nothing hardcodes 48000 or 32.

#ifndef ENGINE_SR
#define ENGINE_SR 48000           // sample rate, Hz
#endif

#ifndef ENGINE_BLOCK
#define ENGINE_BLOCK 32           // frames per processing block (power of two)
#endif

// ENGINE_FX_MONO=1: the delay and the reverb compute one channel and send it to both outputs (the dry signal keeps its stereo). It halves the
// delay's cost and its memory and saves about a third of the reverb's cost; the effects lose their stereo width. Off by default.
#ifndef ENGINE_FX_MONO
#define ENGINE_FX_MONO 0
#endif

// ENGINE_FILTER_EXACT=1: the filter recomputes its coefficients for every sample whenever its cutoff is modulated. By default it does that only
// when the modulation is not a smooth line over the block (audio-rate FM, sample & hold) and interpolates the coefficients otherwise.
#ifndef ENGINE_FILTER_EXACT
#define ENGINE_FILTER_EXACT 0
#endif

// ENGINE_REVERB_HALF=1: the reverb tank (delay lines, diffusers, damping, output taps) runs at half the sample rate, 2:1 decimated and 1:2
// interpolated with half-band / cubic FIRs. It halves the reverb's cost and the memory of its lines; the reverb loses some of the top octave
// (its own damping filters are usually below that). The rest of the engine stays at ENGINE_SR. Off by default.
#ifndef ENGINE_REVERB_HALF
#define ENGINE_REVERB_HALF 0
#endif

// The voice ceiling: 32 for the Strings type (ADR-037). The engine's pointer tables are sized by it (about 13 KB of static RAM and 6 KB per
// live plan at 32 on the ESP32-S3); a patch only builds the voices it asks for.
#ifndef ENGINE_MAX_VOICES
#define ENGINE_MAX_VOICES 32
#endif

// Entries of the control -> audio command ring (a power of two). Each entry is about 270 bytes (it can carry a 256 byte blob), so the
// default of 512 costs 137 KB: on a target with little internal RAM lower it (the ESP32 build uses 128).
#ifndef ENGINE_CMD_RING
#define ENGINE_CMD_RING 512
#endif

// SC_HOT marks the audio functions that every patch runs (module process() methods). On the ESP32 they are placed in internal instruction RAM:
// the code of flash is read through a 16 KB cache that the display code on the other core shares, so a redraw of the screen evicts the engine's
// code and slows the audio core (and the engine's own code is bigger than the cache, so every block starts with cold misses). IRAM costs the same
// amount of internal RAM as it uses. Define ENGINE_NO_IRAM to turn it off.
#if defined(ARDUINO_ARCH_ESP32) && !defined(ENGINE_NO_IRAM)
#include <esp_attr.h>
#define SC_HOT IRAM_ATTR
#else
#define SC_HOT
#endif

static_assert((ENGINE_BLOCK & (ENGINE_BLOCK - 1)) == 0 && ENGINE_BLOCK >= 8 && ENGINE_BLOCK <= 256,
              "ENGINE_BLOCK must be a power of two in 8..256");
static_assert(ENGINE_SR >= 8000 && ENGINE_SR <= 96000, "ENGINE_SR out of range");

namespace sc {
constexpr int kSampleRate = ENGINE_SR;
constexpr int kBlock      = ENGINE_BLOCK;
constexpr int kControlRate = ENGINE_SR / ENGINE_BLOCK;   // Hz: one control tick per block
constexpr int kMaxVoices  = ENGINE_MAX_VOICES;
}  // namespace sc
