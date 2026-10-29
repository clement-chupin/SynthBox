#include "Leds.h"

void setup_leds()
{
    FastLED.addLeds<SK6812, LED_DATA_PIN, BGR>(leds, NUM_LEDS);
    FastLED.setBrightness(BRIGHTNESS);
    FastLED.clear(true);
    Serial.printf("Driving %d LEDs on GPIO %d\n\r", NUM_LEDS, LED_DATA_PIN);
}

void test_leds()
{
    // TEST 1: allume une seule LED, les autres non touchées
    Serial.println("LED TEST 1: single LED only");
    for (int i = 0; i < NUM_LEDS; i++)
    {
        leds[i] = CRGB::Green;
        FastLED.show();
        delay(80);
        leds[i] = CRGB::Black;
        FastLED.show();
        delay(20);
    }
    delay(500);

    // TEST 2: allume une seule LED, force toutes les autres à noir
    Serial.println("LED TEST 2: single LED + explicit black");
    for (int i = 0; i < NUM_LEDS; i++)
    {
        for (int j = 0; j < NUM_LEDS; j++)
            leds[j] = CRGB::Black;
        leds[i] = CRGB::Red;
        FastLED.show();
        delay(80);
    }
    for (int j = 0; j < NUM_LEDS; j++)
        leds[j] = CRGB::Black;
    FastLED.show();
}

int crdToIdx(int x, int y)
{
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
