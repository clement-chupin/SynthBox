// web_oled.cpp — replaces src/oled.cpp in the web build.
// Defines the 'oled' global using SimOled (null I2C callbacks, RAM buffer) —
// identical to simulator/sim_oled.cpp. The buffer is exposed to JS via
// getOledBuffer()/getOledWidth()/getOledHeight()/isOledDirty() in web_main.cpp.

#include "../include/oled.h"

U8G2_SH1107_128X128_F_HW_I2C oled(U8G2_R0,
    U8X8_PIN_NONE, 0, 0);  // reset=NONE, clock/data ignored

volatile bool g_oledDirty = false;

void setupOled() {
    oled.begin();
}
