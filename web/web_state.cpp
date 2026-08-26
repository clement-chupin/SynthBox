// web_state.cpp — global simulator state variables (identical to sim_state.cpp)
#include "web_state.h"

float g_simJoyX    = 0.0f;
float g_simJoyY    = 0.0f;
bool  g_simJoySW   = false;
float g_simSlider[16] = {
    0.35f, 0.50f, 0.00f, 0.00f,
    0.00f, 0.00f, 0.00f, 0.50f,
    0.50f, 0.50f, 0.50f, 0.50f,
    0.50f, 0.50f, 0.50f, 0.50f,
};

#include "hal/Arduino.h"
HardwareSerial Serial;
EspClass ESP;

#include "hal/SPI.h"
SPIClass SPI;

#include "hal/Wire.h"
TwoWire Wire;
TwoWire Wire1;

#include "hal/SD.h"
SDClass SD;
