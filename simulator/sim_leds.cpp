// sim_leds.cpp — replaces Leds.cpp in simulator build
// LED colors are stored in g_simLeds[]; SDL reads them via g_ledsDirty flag.

#include "../include/Leds.h"
#include "sim_state.h"
#include <string.h>

// The real leds[] array is declared in main.cpp (CRGB leds[NUM_LEDS])
// g_simLeds is a copy for the SDL thread to read safely
CRGB g_simLeds[NUM_LEDS] = {};
volatile bool g_ledsDirty = false;

// FastLED singleton (template methods compiled here)
CFastLED FastLED;

// CRGB named constants
const CRGB CRGB::Black(0, 0, 0);
const CRGB CRGB::Red  (255, 0, 0);
const CRGB CRGB::Green(0, 255, 0);
const CRGB CRGB::Blue (0, 0, 255);
const CRGB CRGB::White(255, 255, 255);

// Called by FastLED.show()
void simLedsShow(const CRGB* leds, int count) {
    if (count > NUM_LEDS) count = NUM_LEDS;
    memcpy(g_simLeds, leds, count * sizeof(CRGB));
    g_ledsDirty = true;
}

void setup_leds() {
    // Register the real leds[] array (declared in main.cpp, extern in Leds.h).
    // FastLED.show() will call simLedsShow(leds, NUM_LEDS) which copies to g_simLeds.
    FastLED.addLeds<SK6812, 14, BGR>(leds, NUM_LEDS);
    FastLED.setBrightness(BRIGHTNESS);
}

void test_leds() {
    // no-op in simulator
}

int crdToIdx(int x, int y) {
    (void)y;
#ifdef OOPSIE_LED_FLAG
    static const int table[NUM_LEDS + 4] = {
        32, 33, 34, 35,
        32, 33, 34, 35,
        31, 30, 29, 28, 27, 26, 25, 24,
        16, 17, 18, 19, 20, 21, 22, 23,
        15, 14, 13, 12, 11, 10,  9,  8,
         0,  1,  2,  3,  4,  5,  6,  7,
    };
#else
    static const int table[NUM_LEDS + 4] = {
         3,  2,  1,  0,
         3,  2,  1,  0,
         4,  5,  6,  7,  8,  9, 10, 11,
        19, 18, 17, 16, 15, 14, 13, 12,
        20, 21, 22, 23, 24, 25, 26, 27,
        35, 34, 33, 32, 31, 30, 29, 28,
    };
#endif
    return table[x];
}
