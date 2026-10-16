// sim_keyboard.cpp — replaces keyboard.cpp in simulator build
// Key events are injected by sim_window via simKeyPress()

#include "../include/keyboard.h"
#include "sim_state.h"

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

// Called from SDL event loop (sim_window.cpp) to inject a key event
void simKeyPress(uint8_t row, uint8_t col, bool pressed) {
    if (row >= KBD_ROWS || col >= KBD_COLS) return;
    keyState[row][col] = pressed;

    // For note rows: fire the audio callback (sends to s_audioEventQueue for audio playback)
    // AND enqueue so loop()'s getNextKeyEvent() can process overlay intercepts.
    // On real hardware both paths exist; the simulator was missing the enqueue for note rows.
    if (row < KBD_NOTE_ROWS && s_noteKeyCb) {
        s_noteKeyCb(row, col, pressed);
        // fall through to also enqueue
    }

    // Enqueue for loop() processing (overlay selection, menu handling)
    uint8_t next = (s_head + 1) % KEY_QUEUE_SIZE;
    if (next != s_tail) {
        s_queue[s_head] = {row, col, pressed};
        s_head = next;
    }
}
