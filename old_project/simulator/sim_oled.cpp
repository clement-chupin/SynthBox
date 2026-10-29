// sim_oled.cpp — replaces oled.cpp in simulator build
// Defines the 'oled' global using SimOled (null I2C callbacks, RAM buffer).

#include "../include/oled.h"

// u8g2_cb_r0 is declared in clib/u8g2.h and defined in clib sources
// U8G2_R0 macro = &u8g2_cb_r0

U8G2_SH1107_128X128_F_HW_I2C oled(U8G2_R0,
    U8X8_PIN_NONE, 0, 0);  // reset=NONE, clock/data ignored

volatile bool g_oledDirty = false;

void setupOled() {
    oled.begin();
}
