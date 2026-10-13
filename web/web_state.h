#pragma once
// Shared web-simulator state between the JS I/O bridge and the hardware stubs.
// Mirrors simulator/sim_state.h — see structure/WEB_SIMULATOR.md for the full
// JS<->WASM protocol (pointers returned by web_main.cpp's getJoyXPtr() etc. index
// directly into these globals via Module.HEAPF32/HEAPU8).

#include <stdint.h>
#include <stdbool.h>

// ---- Joystick / analog inputs ----
extern float g_simJoyX;    // -1.0 .. 1.0  (left-right)
extern float g_simJoyY;    // -1.0 .. 1.0  (down-up)
extern bool  g_simJoySW;   // joystick button pressed

// ---- Mux / pot sliders ---- (0.0 .. 1.0, 16 channels)
extern float g_simSlider[16];

// ---- OLED dirty flag (set by SimOled::sendBuffer) ----
extern volatile bool g_oledDirty;

// ---- LED dirty flag (set by simLedsShow) ----
extern volatile bool g_ledsDirty;

// ---- Keyboard injection from JS (exported to JS as simKeyPress in web_main.cpp) ----
extern "C" void simKeyPress(uint8_t row, uint8_t col, bool pressed);

// ---- Live control labels for P2/P4-P7/B1-B4 — same mechanism as the SDL
// simulator (see sim_state.h's own comment); used only if docs/index.html wants
// to show them (not required by the spec, kept for parity/debugging).
extern const char* g_ctrlP2;
extern const char* g_ctrlP4;
extern const char* g_ctrlP5;
extern const char* g_ctrlP6;
extern const char* g_ctrlP7;
extern const char* g_ctrlB1;
extern const char* g_ctrlB2;
extern const char* g_ctrlB3;
extern const char* g_ctrlB4;
