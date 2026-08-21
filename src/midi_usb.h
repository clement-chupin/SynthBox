#pragma once
#include <stdint.h>

// MIDI channel assignments (1-indexed, GM-compatible)
#define MIDI_CH_SYNTH  1   // all melodic modes
#define MIDI_CH_BASS   2   // 303 / acid bass
#define MIDI_CH_DRUM  10   // drums (GM channel 10)

void midiNoteOn(uint8_t note, uint8_t vel, uint8_t ch);
void midiNoteOff(uint8_t note, uint8_t ch);
void midiAllNotesOff(uint8_t ch);
void midiCC(uint8_t cc, uint8_t val, uint8_t ch);
void midiPitchBend(double val, uint8_t ch);

// Pad 0-7 → GM drum note, channel 10 (noteOn + immediate noteOff)
void midiDrum(uint8_t pad, uint8_t vel);

#if CONFIG_TINYUSB_MIDI_ENABLED
#include "USB.h"
#include "USBMIDI.h"
bool midiReadPacket(midiEventPacket_t* pkt);
#endif
