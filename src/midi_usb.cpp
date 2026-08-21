#include "midi_usb.h"

#if CONFIG_TINYUSB_MIDI_ENABLED
#include "USB.h"
#include "USBMIDI.h"

USBMIDI usbMIDI("GrvEP");

// GM drum notes for pads 0-7: Kick, Snare, ClosedHH, OpenHH, Clap, FloorTom, MidTom, Crash
static const uint8_t kDrumGM[8] = { 36, 38, 42, 46, 39, 41, 43, 49 };

void midiNoteOn(uint8_t note, uint8_t vel, uint8_t ch) {
    usbMIDI.noteOn(note, vel ? vel : 1, ch);
}
void midiNoteOff(uint8_t note, uint8_t ch) {
    usbMIDI.noteOff(note, 0, ch);
}
void midiAllNotesOff(uint8_t ch) {
    for (int n = 0; n < 128; n++) usbMIDI.noteOff((uint8_t)n, 0, ch);
}
void midiCC(uint8_t cc, uint8_t val, uint8_t ch) {
    usbMIDI.controlChange(cc, val, ch);
}
void midiPitchBend(double val, uint8_t ch) {
    usbMIDI.pitchBend(val, ch);
}
void midiDrum(uint8_t pad, uint8_t vel) {
    uint8_t gm = (pad < 8) ? kDrumGM[pad] : 36;
    usbMIDI.noteOn(gm, vel ? vel : 1, 10);
    usbMIDI.noteOff(gm, 0, 10);
}
bool midiReadPacket(midiEventPacket_t* pkt) {
    return usbMIDI.readPacket(pkt);
}

#else
void midiNoteOn(uint8_t, uint8_t, uint8_t)  {}
void midiNoteOff(uint8_t, uint8_t)           {}
void midiAllNotesOff(uint8_t)                {}
void midiCC(uint8_t, uint8_t, uint8_t)       {}
void midiPitchBend(double, uint8_t)          {}
void midiDrum(uint8_t, uint8_t)              {}
#endif
