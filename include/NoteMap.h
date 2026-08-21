#pragma once
#include <Arduino.h>
#include "keyboard.h"

enum Scale : uint8_t {
    SCALE_CHROMATIC,
    SCALE_MAJOR,
    SCALE_MINOR,
    SCALE_PENTATONIC,
    SCALE_BLUES,
    SCALE_HARMONIC_MINOR,
    SCALE_COUNT
};

const char* scaleName(Scale s);

class NoteMap {
public:
    void setScale(Scale s);
    void nextScale();
    Scale getScale() const { return _scale; }

    void setOctave(int8_t oct);
    void nextOctave();
    int8_t getOctave() const { return _octave; }

    uint8_t getMidiNote(uint8_t row, uint8_t col) const;
    uint8_t getMidiNoteByIdx(int idx) const;
    bool isNoteKey(uint8_t row, uint8_t col) const;
    bool isMenuKey(uint8_t row, uint8_t col) const;
    uint8_t getMenuButton(uint8_t row, uint8_t col) const;

private:
    Scale _scale = SCALE_CHROMATIC;
    int8_t _octave = 0;
    void buildMap();
    uint8_t _noteMap[KBD_NOTE_ROWS][KBD_COLS] = {};
};
