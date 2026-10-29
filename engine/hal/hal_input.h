#pragma once
// HAL: user input. One implementation per board (the first prototype, the desktop panel...).
//
// A board has its own controls and lists them in a table: input_controls(). A control's id is its place in that table, and an event
// says "control <id> was pressed / turned / moved". The HAL knows nothing about what a control does: an app reads the table (the
// kind of each control, its name, its place in the key matrix) and decides, or keeps a list of roles of its own with a map per board
// (the Ritual Machine: core/controls.h). The ids of a board have names in its own header (boards/hwv1/hwv1_controls.h,
// boards/desktop/panel_controls.h).
//
// What each kind of control reports:
//   key / button                   : IN_PRESS and IN_RELEASE (value unused)
//   encoder                        : IN_DELTA, value = detents since the last event (+ clockwise)
//   knob / joystick axis           : IN_VALUE, value = position 0..INPUT_VALUE_MAX (axis centre = INPUT_AXIS_CENTER)
//   endless pot (no end stops)     : IN_DELTA like an encoder, or IN_VALUE like a knob: input_control_mode() says which
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define INPUT_VALUE_MAX   1023                      // full travel of a knob / axis (10 bit, ESP32 ADC values are scaled to it)
#define INPUT_AXIS_CENTER 512

typedef enum { IN_NONE, IN_PRESS, IN_RELEASE, IN_DELTA, IN_VALUE } input_kind_t;

typedef enum {
    CK_KEY,                 // a key of the matrix: `row`, `col` say where (row 0 = the top row, col 0 = the left column)
    CK_BUTTON,              // any other switch: a button, an encoder's push, a joystick's push
    CK_ENCODER,             // detents
    CK_ENDLESS,             // a pot without end stops
    CK_KNOB,                // a pot with end stops
    CK_AXIS,                // a joystick axis (it rests at INPUT_AXIS_CENTER)
} control_kind_t;

// One control of a board. The tables are constants (flash on the ESP32).
typedef struct {
    const char *name;       // the part's name on this board: "F1", "KEY 2.3", "POT 4", "JOY X"
    uint8_t     kind;       // control_kind_t
    uint8_t     row, col;   // CK_KEY: its place in the key matrix (0 for the other kinds)
    uint8_t     px, py;     // the centre of its part on the panel, in half key pitches from the top left (the desktop draws a board from
                            // it; controls at the same place are one part: an encoder and its push, a joystick's axes and its push)
    uint8_t     led;        // 1: an LED sits under it (hal_leds.h)
} input_control_t;

const input_control_t *input_controls(int *count);  // the board's table; a control's id is its index
const char *input_board_name(void);                 // "hwv1", "panel": an app picks its map for the board with it

// How an endless pot (CK_ENDLESS) reports: `reading` = IN_DELTA, steps like an encoder (the default), or IN_VALUE, a position that
// starts at `start` and stops at 0 and INPUT_VALUE_MAX (a full turn sweeps the range). The noise thresholds are the board's. Call it
// before the board starts its scan; the other kinds ignore it.
void input_control_mode(int ctl, input_kind_t reading, int start);

#define INPUT_NO_CONTROL (-1)

typedef struct {
    int          ctl;       // which control: its id in input_controls() (INPUT_NO_CONTROL when kind is IN_NONE)
    input_kind_t kind;      // what happened
    int          value;     // IN_DELTA: signed detents; IN_VALUE: 0..INPUT_VALUE_MAX
    bool         quit;      // window closed / platform asks to exit (kind is IN_NONE)
} input_event_t;

// Non-blocking poll. Returns an event with kind == IN_NONE (and quit == false) when nothing happened.
input_event_t input_poll(void);
bool input_pending(void);                           // more events are queued: the app handles them before it redraws

// The board's reset key was held at power-on (for about 2 s, read before the key scan starts: a key that is already down makes no
// press event). Boards without it return false.
bool input_boot_reset(void);

#ifdef __cplusplus
}
#endif
