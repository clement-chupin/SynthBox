#pragma once
// Adafruit TCA8418 stub for GrvEP simulator — keyboard input comes from SDL

#include <stdint.h>

class Adafruit_TCA8418 {
public:
    bool begin(uint8_t addr=0x34, void* wire=nullptr) { return true; }
    void flush() {}
    int  available() { return 0; }
    int  getEvent() { return 0; }
    void configureFIFO(uint8_t) {}
    void configureMultiKey() {}
    bool enableInterrupts() { return true; }
    void enableKeypad() {}
    void matrix(int rows, int cols) {}
};
