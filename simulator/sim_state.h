#pragma once
// Shared simulator state between SDL window and hardware stubs

#include <stdint.h>
#include <stdbool.h>

// ---- Joystick / analog inputs ----
extern float g_simJoyX;    // -1.0 .. 1.0  (left-right)
extern float g_simJoyY;    // -1.0 .. 1.0  (down-up)
extern bool  g_simJoySW;   // joystick button pressed

// ---- Mux / pot sliders ---- (0.0 .. 1.0, 16 channels)
extern float g_simSlider[16];

// ---- Android-only: always-visible "import a folder" button (sim_window.cpp draws/hit-
// tests it, main.cpp's loop() consumes+resets it). One-shot: true for exactly one frame
// after a tap, regardless of current mode/menu state — bypasses menu navigation entirely.
extern volatile bool g_simImportTap;

// ---- OLED dirty flag (set by SimOled::sendBuffer) ----
extern volatile bool g_oledDirty;

// ---- LED dirty flag (set by simLedsShow) ----
extern volatile bool g_ledsDirty;

// ---- Keyboard injection from SDL ----
void simKeyPress(uint8_t row, uint8_t col, bool pressed);

// ---- Live control labels for P2/P4-P7/B1-B4 ----
// Source of truth is main.cpp's ctrlLabelsFor()/printCtrlLabels() (the same
// switch(currentMode) that actually drives pot/button behavior) — read directly here
// rather than re-parsing this app's own terminal output, since the simulator and
// main.cpp run in the same process/binary. P1 and P3 aren't included: they're always
// "VOL"/"BPM" for every mode (see main.cpp's loop()), so sim_window.cpp hardcodes
// those two directly. "-" means the current mode has no label for that control yet.
extern const char* g_ctrlP2;
extern const char* g_ctrlP4;
extern const char* g_ctrlP5;
extern const char* g_ctrlP6;
extern const char* g_ctrlP7;
extern const char* g_ctrlB1;
extern const char* g_ctrlB2;
extern const char* g_ctrlB3;
extern const char* g_ctrlB4;
