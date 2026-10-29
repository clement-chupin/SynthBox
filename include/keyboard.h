#pragma once
#include <Arduino.h>
#include "Adafruit_TCA8418.h"

#define KBD_ROWS 5
#define KBD_COLS 8
#define KBD_NOTE_ROWS 4
#define KBD_MENU_ROW 4
#define KBD_MENU_COL_START 4
#define KBD_MENU_COLS 4
#define KEY_QUEUE_SIZE 32

struct KeyEvent {
    uint8_t row;
    uint8_t col;
    bool pressed;
};

extern Adafruit_TCA8418 keyboard;
extern bool keyState[KBD_ROWS][KBD_COLS];

void setupKeyboard();
void pollKeyboard();
void recoverKeyboard();  // soft re-init after FIFO overflow or I2C lockup
bool getNextKeyEvent(uint8_t &row, uint8_t &col, bool &pressed);
uint8_t readTcaGpios();
void kbdGetStats(uint32_t& pollCount, uint32_t& maxIntervalMs);

// Called from kbdPollTask immediately after a note-row event is detected.
// Bypasses the main-loop queue to eliminate the 200ms fill-buffer starvation delay.
typedef void (*NoteKeyCallback)(uint8_t row, uint8_t col, bool pressed);
void setNoteKeyCallback(NoteKeyCallback cb);
