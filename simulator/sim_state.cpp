// sim_state.cpp — global simulator state variables

#include "sim_state.h"

float g_simJoyX    = 0.0f;
float g_simJoyY    = 0.0f;
bool  g_simJoySW   = false;
float g_simSlider[16] = {
    // Pots 0-6: initial positions matching default pot values in main.cpp
    // pot0=vol(0.7/2.0=0.35), pot1=shape(0.5), pot2-6=0.0
    0.35f, 0.50f, 0.00f, 0.00f,
    0.00f, 0.00f, 0.00f, 0.50f,
    0.50f, 0.50f, 0.50f, 0.50f,
    0.50f, 0.50f, 0.50f, 0.50f,
};

// Arduino HAL singletons
#include "hal/Arduino.h"
HardwareSerial Serial;
EspClass ESP;

// SPI / Wire singletons
#include "hal/SPI.h"
SPIClass SPI;

#include "hal/Wire.h"
TwoWire Wire;
TwoWire Wire1;

// SD singleton
#include "hal/SD.h"
SDClass SD;
