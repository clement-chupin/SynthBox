#pragma once
#include <Arduino.h>
#include "HWConfig.h"
#include "U8g2lib.h"
#include "Wire.h"

extern U8G2_SH1107_128X128_F_HW_I2C oled;
void setupOled();
