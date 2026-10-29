#pragma once
// USBMIDI stub for GrvEP simulator

#include <stdint.h>

struct midiEventPacket_t {
    uint8_t header;
    uint8_t byte1;
    uint8_t byte2;
    uint8_t byte3;
};

class USBMIDIClass {
public:
    void begin() {}
    bool readPacket(midiEventPacket_t*) { return false; }
    void sendNoteOn(uint8_t note, uint8_t vel, uint8_t ch) {}
    void sendNoteOff(uint8_t note, uint8_t vel, uint8_t ch) {}
    void sendControlChange(uint8_t cc, uint8_t val, uint8_t ch) {}
    void sendPitchBend(int16_t val, uint8_t ch) {}
};
extern USBMIDIClass USBMIDI;
