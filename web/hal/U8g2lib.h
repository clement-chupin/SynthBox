#pragma once
// U8g2lib HAL stub for GrvEP simulator
// Uses the real u8g2 C library for rendering, but with null I2C callbacks.
// The framebuffer lives in RAM; SDL reads it via getBufferPtr().

#include "../../.pio/libdeps/esp32-s3-devkitc-1/U8g2/src/clib/u8g2.h"
#include <stdint.h>
#include <string.h>
#include <unistd.h>

// Null byte-send callback — does nothing (buffer stays in RAM)
// u8x8_msg_cb signature: (u8x8_t*, uint8_t msg, uint8_t arg_int, void* arg_ptr)
static uint8_t u8x8_byte_null(u8x8_t*, uint8_t, uint8_t, void*) { return 1; }
// GPIO/delay callback has same signature
static uint8_t u8x8_gpio_delay_null(u8x8_t*, uint8_t, uint8_t, void*) { return 1; }

// U8X8_PIN_NONE
#ifndef U8X8_PIN_NONE
#define U8X8_PIN_NONE 255
#endif

// Shared dirty flag — set by sendBuffer(), cleared by SDL renderer
extern volatile bool g_oledDirty;

// SimOled wraps u8g2_t with null callbacks
class SimOled {
public:
    u8g2_t u8g2;

    SimOled(const u8g2_cb_t* rot, uint8_t reset=U8X8_PIN_NONE,
            uint8_t clock=U8X8_PIN_NONE, uint8_t data=U8X8_PIN_NONE)
    {
        (void)reset; (void)clock; (void)data;
        u8g2_Setup_sh1107_i2c_128x128_f(&u8g2, rot,
            u8x8_byte_null, u8x8_gpio_delay_null);
    }

    bool begin() {
        u8g2_InitDisplay(&u8g2);
        u8g2_SetPowerSave(&u8g2, 0);
        u8g2_ClearBuffer(&u8g2);
        return true;
    }

    // Simulate sendF — ignore vendor commands
    void sendF(const char*, ...) {}

    void clearBuffer()   { u8g2_ClearBuffer(&u8g2); }
    void sendBuffer()    {
        u8g2_SendBuffer(&u8g2);
        g_oledDirty = true;
        usleep(30000); // 30ms: let SDL thread render this frame before overwriting it
    }

    void setDrawColor(uint8_t c)  { u8g2_SetDrawColor(&u8g2, c); }
    void setFont(const uint8_t* f){ u8g2_SetFont(&u8g2, f); }
    void setFontMode(uint8_t m)   { u8g2_SetFontMode(&u8g2, m); }
    void setFontDirection(uint8_t d){ u8g2_SetFontDirection(&u8g2, d); }
    void setFontPosTop()          { u8g2_SetFontPosTop(&u8g2); }
    void setFontPosBottom()       { u8g2_SetFontPosBottom(&u8g2); }
    void setFontPosBaseline()     { u8g2_SetFontPosBaseline(&u8g2); }
    void setFontPosCenter()       { u8g2_SetFontPosCenter(&u8g2); }
    void setFontRefHeightText()          { u8g2_SetFontRefHeightText(&u8g2); }
    void setFontRefHeightExtendedText()  { u8g2_SetFontRefHeightExtendedText(&u8g2); }
    void setFontRefHeightAll()           { u8g2_SetFontRefHeightAll(&u8g2); }

    void setMaxClipWindow()       { u8g2_SetMaxClipWindow(&u8g2); }
    void setClipWindow(u8g2_uint_t x0, u8g2_uint_t y0,
                       u8g2_uint_t x1, u8g2_uint_t y1) {
        u8g2_SetClipWindow(&u8g2, x0, y0, x1, y1);
    }

    void drawPixel(u8g2_uint_t x, u8g2_uint_t y)                      { u8g2_DrawPixel(&u8g2, x, y); }
    void drawHLine(u8g2_uint_t x, u8g2_uint_t y, u8g2_uint_t w)       { u8g2_DrawHLine(&u8g2, x, y, w); }
    void drawVLine(u8g2_uint_t x, u8g2_uint_t y, u8g2_uint_t h)       { u8g2_DrawVLine(&u8g2, x, y, h); }
    void drawFrame(u8g2_uint_t x, u8g2_uint_t y, u8g2_uint_t w, u8g2_uint_t h) { u8g2_DrawFrame(&u8g2, x, y, w, h); }
    void drawBox  (u8g2_uint_t x, u8g2_uint_t y, u8g2_uint_t w, u8g2_uint_t h) { u8g2_DrawBox(&u8g2, x, y, w, h); }
    void drawRBox  (u8g2_uint_t x, u8g2_uint_t y, u8g2_uint_t w, u8g2_uint_t h, u8g2_uint_t r) { u8g2_DrawRBox(&u8g2, x, y, w, h, r); }
    void drawRFrame(u8g2_uint_t x, u8g2_uint_t y, u8g2_uint_t w, u8g2_uint_t h, u8g2_uint_t r) { u8g2_DrawRFrame(&u8g2, x, y, w, h, r); }
    void drawCircle(u8g2_uint_t x0, u8g2_uint_t y0, u8g2_uint_t rad, uint8_t opt=15) { u8g2_DrawCircle(&u8g2, x0, y0, rad, opt); }
    void drawLine(u8g2_uint_t x1, u8g2_uint_t y1, u8g2_uint_t x2, u8g2_uint_t y2) { u8g2_DrawLine(&u8g2, x1, y1, x2, y2); }

    void drawStr (u8g2_uint_t x, u8g2_uint_t y, const char* s) { u8g2_DrawStr(&u8g2, x, y, s); }
    int  getStrWidth(const char* s) { return (int)u8g2_GetStrWidth(&u8g2, s); }

    u8g2_uint_t getDisplayWidth()  { return u8g2_GetDisplayWidth(&u8g2); }
    u8g2_uint_t getDisplayHeight() { return u8g2_GetDisplayHeight(&u8g2); }

    uint8_t* getBufferPtr() { return u8g2_GetBufferPtr(&u8g2); }
    uint8_t  getBufferTileHeight() { return u8g2_GetBufferTileHeight(&u8g2); }
    uint8_t  getBufferTileWidth()  { return u8g2_GetBufferTileWidth(&u8g2); }
};

// oled.h declares: extern U8G2_SH1107_128X128_F_HW_I2C oled;
// We replace that type with SimOled
typedef SimOled U8G2_SH1107_128X128_F_HW_I2C;

// Font declarations — real fonts from the clib
extern "C" {
    extern const uint8_t u8g2_font_4x6_tf[];
    extern const uint8_t u8g2_font_5x7_tf[];
    extern const uint8_t u8g2_font_6x10_tf[];
    extern const uint8_t u8g2_font_9x18_tf[];
}
