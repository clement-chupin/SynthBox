#pragma once
#include <Arduino.h>

class Multiplexer
{
public:
    uint8_t _s0;
    uint8_t _s1;
    uint8_t _s2;
    uint8_t _s3;
    uint8_t _COM;
    Multiplexer(uint8_t PIN_s0, uint8_t PIN_s1, uint8_t PIN_s2, uint8_t PIN_s3, uint8_t PIN_COM);

    uint16_t getValue(uint8_t addr);
    bool dRead(uint8_t addr);
};