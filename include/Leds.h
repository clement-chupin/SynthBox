#pragma once
#include "HWConfig.h"
#include <FastLED.h>

#define OOPSIE_LED_FLAG

extern CRGB leds[NUM_LEDS];

void setup_leds();
void test_leds();
int crdToIdx(int x, int y);
