#pragma once
// USB / TinyUSB stubs for GrvEP simulator

#include <stdint.h>

class USBClass {
public:
    void productName(const char*) {}
    void manufacturerName(const char*) {}
    void begin() {}
    operator bool() const { return false; }
};
extern USBClass USB;
