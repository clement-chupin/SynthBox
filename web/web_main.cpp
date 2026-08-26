// web_main.cpp — Emscripten entry point for GrvEP web build.
// Analogous to sim_main.cpp but uses emscripten_set_main_loop instead of SDL.
//
// JS I/O bridge (inputs → WASM, WASM → outputs):
//   Inputs:  Module.ccall('simKeyPress', ...) sets keyboard events
//             Module.HEAPF32[joyXPtr/4]       sets joystick X/Y
//             Module.HEAPF32[sliderPtr/4 + i] sets pot sliders
//   Outputs: Module.ccall('getOledBuffer') → ptr into Module.HEAPU8
//             Module.ccall('getLedsBuffer') → ptr into Module.HEAPU8

#include <emscripten.h>
#include <cstdio>
#include <cstdlib>
#include "web_state.h"
#include "../include/oled.h"
#include "../include/Leds.h"
#include "../include/HWConfig.h"

// Forward declarations from real GrvEP source
void setup();
void loop();

// Called once per animation frame by emscripten_set_main_loop
static void webFrame() {
    loop();
}

int main() {
    printf("[web] GrvEP starting\n");
    setup();
    printf("[web] setup() done, entering main loop\n");
    // 0 fps = let the browser decide (requestAnimationFrame)
    emscripten_set_main_loop(webFrame, 0, 1);
    return 0;
}

// g_simLeds defined in web_leds.cpp
extern CRGB g_simLeds[];

// ── Exported functions — extern "C" so wasm-ld finds them by unmangled name ──
extern "C" {

EMSCRIPTEN_KEEPALIVE void simKeyPress(uint8_t row, uint8_t col, bool pressed);

EMSCRIPTEN_KEEPALIVE float*        getJoyXPtr()   { return &g_simJoyX; }
EMSCRIPTEN_KEEPALIVE float*        getJoyYPtr()   { return &g_simJoyY; }
EMSCRIPTEN_KEEPALIVE bool*         getJoySWPtr()  { return &g_simJoySW; }
EMSCRIPTEN_KEEPALIVE float*        getSliderPtr() { return g_simSlider; }

EMSCRIPTEN_KEEPALIVE const uint8_t* getOledBuffer()  { return oled.getBufferPtr(); }
EMSCRIPTEN_KEEPALIVE int            getOledWidth()   { return 128; }
EMSCRIPTEN_KEEPALIVE int            getOledHeight()  { return 128; }

EMSCRIPTEN_KEEPALIVE const uint8_t* getLedsBuffer()  { return reinterpret_cast<const uint8_t*>(g_simLeds); }
EMSCRIPTEN_KEEPALIVE int            getLedsCount()   { return NUM_LEDS; }

EMSCRIPTEN_KEEPALIVE bool isOledDirty() { bool d=g_oledDirty; g_oledDirty=false; return d; }
EMSCRIPTEN_KEEPALIVE bool isLedsDirty() { bool d=g_ledsDirty; g_ledsDirty=false; return d; }

} // extern "C"
