// web_oled.cpp — identical to sim_oled.cpp; uses U8g2 RAM buffer
#include "../include/oled.h"

U8G2_SH1107_128X128_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE, 0, 0);

volatile bool g_oledDirty = false;

void setupOled() { oled.begin(); }
