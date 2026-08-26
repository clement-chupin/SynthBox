#pragma once
// Wire (I2C) stub for GrvEP simulator

#include <stdint.h>

class TwoWire {
public:
    void begin() {}
    void begin(int sda, int scl) {}
    void setClock(uint32_t) {}
    void beginTransmission(uint8_t) {}
    uint8_t endTransmission(bool = true) { return 0; }
    uint8_t requestFrom(uint8_t, uint8_t) { return 0; }
    int available() { return 0; }
    int read() { return -1; }
    void write(uint8_t) {}
    void write(const uint8_t*, size_t) {}
};

extern TwoWire Wire;
extern TwoWire Wire1;
