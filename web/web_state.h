#pragma once
// Shared web simulator state — mirrors simulator/sim_state.h exactly.
// The JS layer reads/writes these via Emscripten HEAP access or exported functions.

#include <stdint.h>
#include <stdbool.h>

// ---- Joystick / analog inputs ----
extern float g_simJoyX;    // -1.0 .. 1.0
extern float g_simJoyY;    // -1.0 .. 1.0
extern bool  g_simJoySW;   // joystick button pressed

// ---- Pot sliders (0.0 .. 1.0, 16 channels) ----
extern float g_simSlider[16];

// ---- OLED dirty flag ----
extern volatile bool g_oledDirty;

// ---- LED dirty flag ----
extern volatile bool g_ledsDirty;

// ---- Keyboard injection from JS ----
#ifdef __cplusplus
extern "C"
#endif
void simKeyPress(uint8_t row, uint8_t col, bool pressed);
