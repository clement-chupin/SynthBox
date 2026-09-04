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
// Relative to this file (simulator/hal/), other_projects/amy is a sibling directory
// of GrvEP itself — i.e. this expects <GrvEP's parent>/other_projects/amy to exist.
// Portable across machines as long as that sibling layout is kept; see platformio.ini's
// matching ${PROJECT_DIR}/../other_projects/amy for the ESP32 build's own lib_deps entry.
#include "../../../other_projects/amy/src/amy.h"
#ifdef _WIN32
#define INPUT  0
#define OUTPUT 1
#endif
#ifdef __cplusplus
}
#endif
