#include "NoteMap.h"

static const char* scaleNames[] = {
    "Chr", "Maj", "Min", "Pen", "Blu", "HMn"
};

const char* scaleName(Scale s) {
    return scaleNames[s < SCALE_COUNT ? s : 0];
}

static const uint8_t scaleIntervals[][12] = {
    {0,1,2,3,4,5,6,7,8,9,10,11},   // Chromatic (12 notes)
    {0,2,4,5,7,9,11,0,0,0,0,0},    // Major (7)
    {0,2,3,5,7,8,10,0,0,0,0,0},    // Minor (7)
    {0,3,5,7,10,0,0,0,0,0,0,0},    // Pentatonic (5)
    {0,3,5,6,7,10,0,0,0,0,0,0},    // Blues (6)
    {0,2,3,5,7,8,11,0,0,0,0,0},    // Harmonic Minor (7)
};

static const uint8_t scaleSize[] = {12, 7, 7, 5, 6, 7};

void NoteMap::setScale(Scale s) {
    _scale = s;
    buildMap();
}

void NoteMap::nextScale() {
    _scale = (Scale)((_scale + 1) % SCALE_COUNT);
    buildMap();
}

void NoteMap::setOctave(int8_t oct) {
    _octave = constrain(oct, -2, 1);
    buildMap();
}

void NoteMap::nextOctave() {
    _octave++;
    if (_octave > 1) _octave = -2;
    buildMap();
}

void NoteMap::buildMap() {
    uint8_t sz = scaleSize[_scale];
    const uint8_t *intervals = scaleIntervals[_scale];
    uint8_t baseNote = 48 + (_octave * 12); // C3 = 48

    // Layout: columns right-to-left (low notes right, high left), rows top-to-bottom
    for (int r = 0; r < KBD_NOTE_ROWS; r++) {
        for (int c = 0; c < KBD_COLS; c++) {
            int idx = (KBD_COLS - 1 - c) * KBD_NOTE_ROWS + r;
            int octaveOffset = idx / sz;
            int noteInScale = idx % sz;
            int midiNote = baseNote + octaveOffset * 12 + intervals[noteInScale];
            _noteMap[r][c] = constrain(midiNote, 0, 127);
        }
    }
}

uint8_t NoteMap::getMidiNote(uint8_t row, uint8_t col) const {
    if (row >= KBD_NOTE_ROWS || col >= KBD_COLS)
        return 0;
    return _noteMap[row][col];
}

bool NoteMap::isNoteKey(uint8_t row, uint8_t col) const {
    return row < KBD_NOTE_ROWS && col < KBD_COLS;
}

bool NoteMap::isMenuKey(uint8_t row, uint8_t col) const {
    return row == KBD_MENU_ROW && col >= KBD_MENU_COL_START && col < KBD_MENU_COL_START + KBD_MENU_COLS;
}

uint8_t NoteMap::getMenuButton(uint8_t row, uint8_t col) const {
    if (!isMenuKey(row, col)) return 0xFF;
    return col - KBD_MENU_COL_START;
}
