// web_keyboard.cpp — replaces src/keyboard.cpp in the web build.
// Key events are injected by JS via simKeyPress() (exported in web_main.cpp),
// exactly like simulator/sim_keyboard.cpp is injected by sim_window.cpp's SDL
// event loop. See structure/WEB_SIMULATOR.md's I/O bridge table.

#include "../include/keyboard.h"
#include "web_state.h"

Adafruit_TCA8418 keyboard;
bool keyState[KBD_ROWS][KBD_COLS] = {};

static KeyEvent s_queue[KEY_QUEUE_SIZE];
static volatile uint8_t s_head = 0;
static volatile uint8_t s_tail = 0;
static NoteKeyCallback s_noteKeyCb = nullptr;

void setupKeyboard() {}
void pollKeyboard()  {}
void recoverKeyboard() {}

uint8_t readTcaGpios() { return 0; }

void kbdGetStats(uint32_t& pollCount, uint32_t& maxIntervalMs) {
    pollCount = 0; maxIntervalMs = 0;
}

void setNoteKeyCallback(NoteKeyCallback cb) { s_noteKeyCb = cb; }

bool getNextKeyEvent(uint8_t& row, uint8_t& col, bool& pressed) {
    if (s_head == s_tail) return false;
    row     = s_queue[s_tail].row;
    col     = s_queue[s_tail].col;
    pressed = s_queue[s_tail].pressed;
    s_tail  = (s_tail + 1) % KEY_QUEUE_SIZE;
    return true;
}

// Called from JS (via Module.ccall('simKeyPress', ...)) to inject a key event.
void simKeyPress(uint8_t row, uint8_t col, bool pressed) {
    if (row >= KBD_ROWS || col >= KBD_COLS) return;
    keyState[row][col] = pressed;

    // Note rows: fire the audio callback directly (immediate sound) AND enqueue
    // so loop()'s getNextKeyEvent() can still process overlay/menu intercepts —
    // same split as the SDL simulator's simKeyPress (see its own comment).
    if (row < KBD_NOTE_ROWS && s_noteKeyCb) {
        s_noteKeyCb(row, col, pressed);
    }

    uint8_t next = (s_head + 1) % KEY_QUEUE_SIZE;
    if (next != s_tail) {
        s_queue[s_head] = {row, col, pressed};
        s_head = next;
    }
}
