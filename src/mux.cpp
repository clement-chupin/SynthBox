#include "mux.h"

Multiplexer::Multiplexer(uint8_t PIN_s0, uint8_t PIN_s1, uint8_t PIN_s2, uint8_t PIN_s3, uint8_t PIN_COM)
{
    _s0 = PIN_s0;
    _s1 = PIN_s1;
    _s2 = PIN_s2;
    _s3 = PIN_s3;
    _COM = PIN_COM;

    pinMode(PIN_s0, OUTPUT);
    pinMode(PIN_s1, OUTPUT);
    pinMode(PIN_s2, OUTPUT);
    pinMode(PIN_s3, OUTPUT);

    pinMode(PIN_COM, INPUT);
}

uint16_t Multiplexer::getValue(uint8_t addr)
{
    digitalWrite(this->_s0, (addr >> 0) & 0x01);
    digitalWrite(this->_s1, (addr >> 1) & 0x01);
    digitalWrite(this->_s2, (addr >> 2) & 0x01);
    digitalWrite(this->_s3, (addr >> 3) & 0x01);
    return analogRead(_COM);
}

bool Multiplexer::dRead(uint8_t addr)
{   
    digitalWrite(this->_s0, (addr >> 0) & 0x01);
    digitalWrite(this->_s1, (addr >> 1) & 0x01);
    digitalWrite(this->_s2, (addr >> 2) & 0x01);
    digitalWrite(this->_s3, (addr >> 3) & 0x01);
    if(digitalRead(_COM)) return true;
    return false;
}
