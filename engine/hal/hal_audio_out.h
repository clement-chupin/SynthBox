#pragma once
// HAL: the audio output. The board owns the output only: the thread that asks for sound, the converter, its start-up gate. The app
// owns its engine and hands over the function that renders it (ENGINE_DESIGN.md ADR-043 decision 9). One implementation per board.
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Fills `out` with `frames` stereo frames (left, right, 16 bit each). Called on the board's audio thread, which has a deadline:
// no allocation, no lock, no print in it.
typedef void (*audio_render_fn)(int16_t *out, int frames);

// Starts the output at `sample_rate`: from here on the board calls `render` for every buffer it sends. False when it could not
// start. A board may keep the output closed while the machine starts (the first prototype: silence until its loading screen has
// ended); `render` is called all the same.
bool audio_out_start(audio_render_fn render, int sample_rate);
// Stops it: `render` is not called any more when this returns.
void audio_out_stop(void);

// Dev: what the output measured since the last report. A board that measures (the first prototype built with HWV1_DEBUG_AUDIO) calls
// `report` about once a second, on the audio thread, when the serial port has room for a few lines: the app prints what it wants to
// see next to its own numbers. Set it before audio_out_start. Boards that do not measure never call it.
typedef struct {
    uint32_t blocks;        // buffers rendered
    uint32_t late;          // ... of which took longer than the time one buffer plays
    uint32_t avg_us;        // render time of a buffer: average
    uint32_t worst_us;      // ... and the longest
    uint32_t budget_us;     // the time one buffer plays
    uint32_t frames;        // frames per buffer
    uint32_t underruns;     // the converter ran out of data and repeated a buffer (what is heard as a dropout)
} audio_out_stats_t;
typedef void (*audio_report_fn)(const audio_out_stats_t *s);
void audio_out_set_report(audio_report_fn report);

#ifdef __cplusplus
}
#endif
