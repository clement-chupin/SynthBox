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

// ---- OLED dirty flag (set by SimOled::sendBuffer) ----
extern volatile bool g_oledDirty;

// ---- LED dirty flag (set by simLedsShow) ----
extern volatile bool g_ledsDirty;

// ---- Keyboard injection from SDL ----
void simKeyPress(uint8_t row, uint8_t col, bool pressed);
