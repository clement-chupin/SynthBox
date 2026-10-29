// web_main.cpp — entry point for the GrvEP web (Emscripten) build.
// Mirrors simulator/sim_main.cpp's role, but instead of a pthread running
// setup()/loop() alongside an SDL event loop on the main thread, everything runs
// cooperatively on the single browser JS thread via emscripten_set_main_loop().
// See structure/WEB_SIMULATOR.md for the full JS<->WASM I/O bridge protocol this
// file implements (the "Fonctions exportées" section).

#include <emscripten.h>
#include "web_state.h"
#include "web_tasks.h"
#include "../include/oled.h"
#include "../include/Leds.h"

// Forward declarations from the real main.cpp (unmodified ESP32/simulator code).
void setup();
void loop();

// Defined in web_leds.cpp (the per-frame JS-readable copy of main.cpp's leds[]).
extern CRGB g_simLeds[];

// Defined in lib/AMY Synthesizer/src/libminiaudio-audio.c. amy_start() (called
// from src/audio_engine.cpp's audioInit(), itself called from setup()) does NOT
// call this automatically when __EMSCRIPTEN__ is defined (see api.c's amy_start —
// deliberately skipped there so a web host can control exactly when/how audio
// starts instead of amy_live_start_web() hijacking the main loop — see this
// file's grvepBoot() for why we call miniaudio_start() directly instead of
// amy_live_start_web()/amy_live_start_web_audioin(), which would replace our own
// per-frame main loop with AMY's internal-only ticker).
extern "C" void miniaudio_start();

static bool s_booted = false;

static void mainLoopTick() {
    // Give every cooperative "FreeRTOS task" fiber (audioHandlerTask,
    // ledUpdateTask, displayTask, bgServiceTask, djWorkerTask, ssWorkerTask —
    // see web_tasks.h) one turn per browser animation frame...
    webTasksPump();
    // ...then run the real app tick (amy_update(), key/menu handling, mode
    // logic, etc. — unmodified src/main.cpp).
    loop();
}

// Boots the app: runs the real setup() once, starts the web-audio device, then
// hands control to the browser's animation-frame loop. Exported so docs/index.html
// can call it once, right after createGrvEP() resolves (Module.ccall('grvepBoot',
// null, [], [])) — not gated behind a user gesture itself (miniaudio's web-audio
// backend auto-unlocks the AudioContext on the page's first click/keydown/touch,
// so the *browser's* gesture requirement is satisfied by the user's first key
// click on the on-screen keyboard, same as structure/WEB_SIMULATOR.md expects).
extern "C" void grvepBoot() {
    if (s_booted) return;
    s_booted = true;
    setup();
    miniaudio_start();
    emscripten_set_main_loop(mainLoopTick, 0, 1);
}

// ---- Inputs (JS -> WASM) ----

extern "C" float* getJoyXPtr()  { return &g_simJoyX; }
extern "C" float* getJoyYPtr()  { return &g_simJoyY; }
extern "C" bool*  getJoySWPtr() { return &g_simJoySW; }
extern "C" float* getSliderPtr() { return &g_simSlider[0]; }

// simKeyPress() itself is defined in web_keyboard.cpp (declared extern "C" in
// web_state.h) — nothing to add here, just re-documenting it's part of this
// exported surface per structure/WEB_SIMULATOR.md.

// ---- Outputs (WASM -> JS) ----

extern "C" const uint8_t* getOledBuffer() { return oled.getBufferPtr(); }
extern "C" int getOledWidth()  { return (int)oled.getDisplayWidth(); }
extern "C" int getOledHeight() { return (int)oled.getDisplayHeight(); }

extern "C" const uint8_t* getLedsBuffer() { return (const uint8_t*)g_simLeds; }
extern "C" int getLedsCount() { return NUM_LEDS; }

extern "C" bool isOledDirty() {
    bool d = g_oledDirty;
    g_oledDirty = false;
    return d;
}
extern "C" bool isLedsDirty() {
    bool d = g_ledsDirty;
    g_ledsDirty = false;
    return d;
}
