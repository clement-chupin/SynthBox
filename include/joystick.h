#pragma once
#include <Arduino.h>
#include "HWConfig.h"
#include "U8g2lib.h"

extern U8G2_SH1107_128X128_F_HW_I2C oled;
extern void oled_prepare();

//define Y_NEG and X_NEG to invert the X/Y joystick controls
//#define Y_NEG
//#define X_NEG

void oled_prepare()
{
  oled.setFont(u8g2_font_6x10_tf);
  oled.setFontRefHeightExtendedText();
  oled.setDrawColor(1);
  oled.setFontPosTop();
  oled.setFontDirection(0);
}

void setupJoystick()
{
    pinMode(JOYY, INPUT);
    pinMode(JOYX, INPUT);
}

int calX, calY;
int rawX, rawY;
float xFactP, xFactN, yFactP, yFactN;
void joystickCalibration()
{
    bool err = false;
    int xOff = analogRead(JOYX);
    int yOff = analogRead(JOYY);
    rawX = xOff;
    rawY = yOff;

    if (xOff > (2048 - 192) && xOff < (2048 + 192))
    {
        calX = xOff;
        xFactP = (xOff / 2048.0f);
        xFactN = ((4096 - xOff) / 2048.0f);
    }
    else
    {
        oled_prepare();
        oled.drawStr(10, 10, "JoyStick damaged ?");
        oled.drawStr(10, 20, "let go of JoyStick");
        oled.drawStr(10, 30, "and reboot");
        err = true;
    }

    if (yOff > (2048 - 192) && yOff < (2048 + 192))
    {
        calY = yOff;
        yFactP = (yOff / 2048.0f);
        yFactN = ((4096 - yOff) / 2048.0f);
    }
    else
    {
        oled_prepare();
        oled.drawStr(10, 10, "JoyStick damaged ?");
        oled.drawStr(10, 20, "let go of JoyStick");
        oled.drawStr(10, 30, "and reboot");
        err = true;
    }
    oled.sendBuffer();
}

int getJoyX()
{
    rawX = analogRead(JOYX);
    if (rawX <= calX)
    {
        #ifdef X_NEG
        return -((rawX - calX) * xFactN) / 32;
        #endif
        #ifndef X_NEG
        return ((rawX - calX) * xFactN) / 32;
        #endif
    }
    else
    {
        #ifdef X_NEG
        return -((rawX - calX) * xFactP) / 32;
        #endif
        #ifndef X_NEG
        return ((rawX - calX) * xFactP) / 32;
        #endif
    }
}
int getJoyY()
{
    rawY = analogRead(JOYY);
    if (rawY <= calY)
    {
#ifdef Y_NEG
        return -((rawY - calY) * yFactN) / 32;
#endif
#ifndef Y_NEG
        return ((rawY - calY) * yFactN) / 32;
#endif
    }
    else
    {
#ifdef Y_NEG
return -((rawY - calY) * yFactP) / 32;
#endif
#ifndef Y_NEG
return ((rawY - calY) * yFactP) / 32;
#endif
    }
}