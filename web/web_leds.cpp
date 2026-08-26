// web_leds.cpp — identical to sim_leds.cpp; JS reads g_simLeds[] via WASM HEAP
#include "../include/Leds.h"
#include "web_state.h"
#include <string.h>

CRGB g_simLeds[NUM_LEDS] = {};
volatile bool g_ledsDirty = false;

CFastLED FastLED;

const CRGB CRGB::Black(0, 0, 0);
const CRGB CRGB::Red  (255, 0, 0);
const CRGB CRGB::Green(0, 255, 0);
const CRGB CRGB::Blue (0, 0, 255);
const CRGB CRGB::White(255, 255, 255);

void simLedsShow(const CRGB* leds, int count) {
    if (count > NUM_LEDS) count = NUM_LEDS;
    memcpy(g_simLeds, leds, count * sizeof(CRGB));
    g_ledsDirty = true;
}

void setup_leds() {
    FastLED.addLeds<SK6812, 14, BGR>(leds, NUM_LEDS);
    FastLED.setBrightness(BRIGHTNESS);
}

void test_leds() {}

int crdToIdx(int x, int y) {
    (void)y;
    static const int table[NUM_LEDS + 4] = {
         3,  2,  1,  0,
         3,  2,  1,  0,
         4,  5,  6,  7,  8,  9, 10, 11,
        19, 18, 17, 16, 15, 14, 13, 12,
        20, 21, 22, 23, 24, 25, 26, 27,
        35, 34, 33, 32, 31, 30, 29, 28,
    };
    return table[x];
}
