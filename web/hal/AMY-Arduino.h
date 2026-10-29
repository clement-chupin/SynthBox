#pragma once
// AMY-Arduino wrapper for GrvEP simulator

// Pull in FreeRTOS stubs before amy.h (audio_engine.cpp uses xQueueCreate etc.)
#include "freertos/FreeRTOS.h"

// Wrap amy.h in extern "C" so that audio_engine.cpp's "extern "C" pcm_load" doesn't conflict
#ifdef __cplusplus
extern "C" {
#endif
#ifdef _WIN32
// amy.h transitively pulls in <windows.h> on this target. If Arduino.h (below, via
// config.h) already ran in this translation unit, its INPUT/OUTPUT macros (pinMode()
// constants) get textually substituted into windows.h's OWN same-named declarations
// (winuser.h's `typedef struct {...} INPUT, *PINPUT, *LPINPUT;` for SendInput()),
// corrupting them into invalid syntax. Undef around the include, then restore
// Arduino's values immediately after so the rest of this TU still sees them.
#undef INPUT
#undef OUTPUT
#endif
// The single AMY copy vendored in the repo (lib/AMY Synthesizer) — the same sources the
// ESP32 build compiles (PlatformIO picks lib/ up automatically), so header and .c files
// can never come from two different trees.
#include "../../lib/AMY Synthesizer/src/amy.h"
#ifdef _WIN32
#define INPUT  0
#define OUTPUT 1
#endif
#ifdef __cplusplus
}
#endif
