// web_mux.cpp — replaces src/mux.cpp in the web build. Identical analog-mux-signal
// logic to simulator/sim_mux.cpp (see its own extensive header comment for the
// velocity/angle-accumulation math), just fed from g_simSlider[] written by JS
// instead of SDL.

#include "../include/mux.h"
#include "web_state.h"
#include <stdint.h>
#include <math.h>

// Initial slider positions matching default pot values from main.cpp:
//   pots[0]=0.7 -> slider=0.7/2.0=0.35   (vol, pMax=2)
//   pots[1]=0.5 -> slider=0.5/1.0=0.50   (shape, pMax=1)
//   pots[2..6]=0.0 -> slider=0.0          (bpm, fx params)
static float s_prevSlider[7]    = {0.35f, 0.50f, 0.00f, 0.00f, 0.00f, 0.00f, 0.00f};
static float s_internalAngle[7] = {0.0f,  0.0f,  0.0f,  0.0f,  0.0f,  0.0f,  0.0f};

Multiplexer::Multiplexer(uint8_t s0, uint8_t s1, uint8_t s2, uint8_t s3, uint8_t com) {
    _s0 = s0; _s1 = s1; _s2 = s2; _s3 = s3; _COM = com;
}

uint16_t Multiplexer::getValue(uint8_t addr) {
    if (addr == 0) return 2048;
    if (addr == 1) return 1461;  // ~3.7V battery voltage
    if (addr < 2 || addr > 15) return 2048;

    int  potIdx = (addr - 2) / 2;
    bool isY    = (addr - 2) % 2;
    if (potIdx >= 7) return 2048;

    // Update angle only on X channel so both X and Y use the same angle snapshot.
    if (!isY) {
        float slider = g_simSlider[potIdx];
        float ds = slider - s_prevSlider[potIdx];
        if (ds != 0.0f) {
            float scale;
            if (potIdx == 0)
                scale =  4.0f * (float)M_PI;   // pMax=2, sign=+1
            else if (potIdx == 1)
                scale =  2.0f * (float)M_PI;   // pMax=1, sign=+1
            else
                scale = -2.0f * (float)M_PI;   // pMax=1, sign=-1
            s_internalAngle[potIdx] += ds * scale;
            s_prevSlider[potIdx] = slider;
        }
    }

    const float R = 2000.0f;
    float angle = s_internalAngle[potIdx];
    float component = isY ? sinf(angle) : cosf(angle);
    int raw = (int)(component * R + 2048.0f);
    if (raw < 0)    raw = 0;
    if (raw > 4095) raw = 4095;
    return (uint16_t)raw;
}

bool Multiplexer::dRead(uint8_t addr) {
    return getValue(addr) > 2048;
}
