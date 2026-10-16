#include "oled.h"
#include <Wire.h>

U8G2_SH1107_128X128_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE, OLEDSCL, OLEDSDA);

void setupOled()
{
    oled.begin();
    Wire.setClock(400000);  // 400kHz → ~47ms/frame (was 190ms at 100kHz default)
    oled.sendF("ca", 0xD3, -32);
    oled.clearBuffer();
    oled.setFont(u8g2_font_6x10_tf);
    oled.drawStr(20, 60, "GrvEP Synth...");
    oled.sendBuffer();
}
