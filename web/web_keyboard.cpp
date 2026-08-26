// web_keyboard.cpp — identical to sim_keyboard.cpp; key events injected by JS
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

extern "C" void simKeyPress(uint8_t row, uint8_t col, bool pressed) {
    if (row >= KBD_ROWS || col >= KBD_COLS) return;
    keyState[row][col] = pressed;
    if (row < KBD_NOTE_ROWS && s_noteKeyCb) {
        s_noteKeyCb(row, col, pressed);
    }
    uint8_t next = (s_head + 1) % KEY_QUEUE_SIZE;
    if (next != s_tail) {
        s_queue[s_head] = {row, col, pressed};
        s_head = next;
    }
}
