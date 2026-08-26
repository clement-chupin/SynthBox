// web_midi.cpp — MIDI stubs (no-op for web build)
#include "../src/midi_usb.h"
#include "hal/USB.h"
#include "hal/USBMIDI.h"

USBClass    USB;
USBMIDIClass USBMIDI;

void midiNoteOn(uint8_t, uint8_t, uint8_t)   {}
void midiNoteOff(uint8_t, uint8_t)            {}
void midiAllNotesOff(uint8_t)                 {}
void midiCC(uint8_t, uint8_t, uint8_t)        {}
void midiPitchBend(double, uint8_t)           {}
void midiDrum(uint8_t, uint8_t)               {}
bool midiReadPacket(midiEventPacket_t*)        { return false; }
