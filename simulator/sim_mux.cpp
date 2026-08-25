// sim_mux.cpp — replaces mux.cpp in simulator build
//
// readPots() in main.cpp:
//   x = muxCache[2+i*2]-2048, y = muxCache[3+i*2]-2048
//   angle = atan2(y,x); delta = angle - prevAngle  (with wrap fix)
//   accum += sign * delta * 50/PI;  sign=(i<=1)?+1:-1
//   value += accum * 0.01            pMax=(i==0)?2:1
//
// Velocity-based approach: track internal angle per pot and accumulate
// slider deltas as angle changes, so the full slider range maps to the
// full pot range regardless of wrap-around issues.
//
// Mapping: slider 0→1 should produce pot 0→pMax.
//   pot_change = sign * delta_angle * 50/PI * 0.01 = sign * delta_angle * 0.5/PI
//   To get pot_change = pMax when slider goes 0→1:
//     pot0 (pMax=2, sign=+1):   delta_angle = 2 * PI / 0.5 = 4*PI
//     pot1 (pMax=1, sign=+1):   delta_angle = 1 * PI / 0.5 = 2*PI
//     pots2-6 (pMax=1,sign=-1): delta_angle = -2*PI  (negative for sign=-1)

#include "../include/mux.h"
#include "sim_state.h"
#include <stdint.h>
#include <math.h>

// Initial slider positions matching default pot values from main.cpp:
//   pots[0]=0.7 → slider=0.7/2.0=0.35   (vol, pMax=2)
//   pots[1]=0.5 → slider=0.5/1.0=0.50   (shape, pMax=1)
//   pots[2..6]=0.0 → slider=0.0          (bpm, fx params)
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
