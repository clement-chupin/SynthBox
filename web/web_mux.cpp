// web_mux.cpp — identical to sim_mux.cpp; sliders set by JS via g_simSlider[]
#include "../include/mux.h"
#include "web_state.h"
#include <stdint.h>
#include <math.h>

static float s_prevSlider[7]    = {0.35f, 0.50f, 0.00f, 0.00f, 0.00f, 0.00f, 0.00f};
static float s_internalAngle[7] = {};

Multiplexer::Multiplexer(uint8_t s0, uint8_t s1, uint8_t s2, uint8_t s3, uint8_t com) {
    _s0=s0; _s1=s1; _s2=s2; _s3=s3; _COM=com;
}

uint16_t Multiplexer::getValue(uint8_t addr) {
    if (addr == 0) return 2048;
    if (addr == 1) return 1461;
    if (addr < 2 || addr > 15) return 2048;
    int potIdx = (addr-2)/2;
    bool isY   = (addr-2)%2;
    if (potIdx >= 7) return 2048;
    if (!isY) {
        float slider = g_simSlider[potIdx];
        float ds = slider - s_prevSlider[potIdx];
        if (ds != 0.0f) {
            float scale = (potIdx==0) ?  4.0f*(float)M_PI
                        : (potIdx==1) ?  2.0f*(float)M_PI
                                      : -2.0f*(float)M_PI;
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

bool Multiplexer::dRead(uint8_t addr) { return getValue(addr) > 2048; }
