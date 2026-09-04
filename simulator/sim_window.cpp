// sim_window.cpp — SDL2 window: OLED display, 4×8 keyboard, 7 pots, joystick
// No LED display (removed). PC keyboard and mouse input.

#include "sim_state.h"
#include "../include/HWConfig.h"
#include "hal/U8g2lib.h"
#include "hal/FastLED.h"
#include "../include/Leds.h"

#include <SDL2/SDL.h>
#include <cstdio>
#include <cstring>
#include <cmath>

extern volatile bool g_oledDirty;
extern U8G2_SH1107_128X128_F_HW_I2C oled;

// LED array (defined in sim_leds.cpp, written by FastLED.show())
extern CRGB g_simLeds[NUM_LEDS];

// LED index table (OOPSIE_LED_FLAG layout), indexed by flat key index (4-row)*8+col
static const int kLedTable[40] = {
    32, 33, 34, 35,                          // row 4 cols 0-3
    32, 33, 34, 35,                          // row 4 cols 4-7
    31, 30, 29, 28, 27, 26, 25, 24,          // row 3 cols 0-7
    16, 17, 18, 19, 20, 21, 22, 23,          // row 2 cols 0-7
    15, 14, 13, 12, 11, 10,  9,  8,          // row 1 cols 0-7
     0,  1,  2,  3,  4,  5,  6,  7,          // row 0 cols 0-7
};

static CRGB keyLedColor(uint8_t row, uint8_t col) {
    // main.cpp computes: gr=4-row, gc=7-col, idx=gr*8+gc — mirror col here to match
    int x = (4 - row) * 8 + (7 - col);
    if (x < 0 || x >= 40) return CRGB(0, 0, 0);
    int idx = kLedTable[x];
    if (idx < 0 || idx >= NUM_LEDS) return CRGB(0, 0, 0);
    return g_simLeds[idx];
}

// Forward from sim_keyboard.cpp
void simKeyPress(uint8_t row, uint8_t col, bool pressed);

// ---- Window ----
#define WIN_W       820
#define WIN_H       460

// ============================================================
// Layout: two vertical panels
//   Left  (0..SPLIT_X)     : OLED screen centered
//   Right (SPLIT_X..WIN_W) : top = keyboard, bottom = controls
// ============================================================
#define SPLIT_X     410

// ---- Left panel: OLED (centered in the 410×560 area) ----
#define OLED_SCALE  3
#define OLED_W      (128 * OLED_SCALE)            // 384
#define OLED_H      (128 * OLED_SCALE)            // 384
#define OLED_X      ((SPLIT_X - OLED_W) / 2)     // 13
#define OLED_Y      ((WIN_H   - OLED_H) / 2)     // 88

// ---- Right panel top: Keyboard (finger-friendly keys) ----
#define KBD_X       (SPLIT_X + 7)    // 417
#define KBD_Y       10
#define KBD_ROWS    5
#define KBD_COLS    8
#define KEY_W       46    // 8 cols × (46+4) - 4 = 396px ≤ 403px available
#define KEY_H       42
#define KEY_GAP     4
// Keyboard total height: 5*(42+4)-4 = 226px → bottom at y=236

// ---- Right panel bottom: Controls (pots + joystick) ----
#define CTRL_Y      (KBD_Y + KBD_ROWS*(KEY_H+KEY_GAP) - KEY_GAP + 14)  // 250

#define POT_W       14
#define POT_H       110    // tall enough for comfortable finger drag
#define POT_COUNT   7

// Main pots (VOL/SHAPE/BPM) — left side of ctrl area
#define MPOT_X      (SPLIT_X + 8)          // 418
#define MPOT_Y      (CTRL_Y + 8)           // 258
#define MPOT_GAP    32
#define MPOT_COUNT  3
// Pots at x: 418, 450, 482 — right edge: 482+14=496

// Joystick — center of ctrl area
#define JOY_CX      (MPOT_X + MPOT_COUNT * MPOT_GAP + 50)  // 564
#define JOY_CY      (MPOT_Y + POT_H / 2)                   // 313
#define JOY_R       36
#define JOY_KNOB_R  10
// Joystick span: 528..600

// JOY_SW on-screen button (Android: rendered and hit-tested; desktop: invisible but position used for SPOT_X)
#define JBTN_X      (JOY_CX + JOY_R + 10)    // 610
#define JBTN_Y      (JOY_CY - 24)             // 289
#define JBTN_W      60
#define JBTN_H      48

// Secondary pots (FX params) — right side of ctrl area
#define SPOT_X      (JBTN_X + JBTN_W + 8)    // 678
#define SPOT_Y      MPOT_Y
#define SPOT_GAP    28
#define SPOT_COUNT  4
// Pots at x: 678, 706, 734, 762 — right edge: 776 ≤ 820 ✓

// ---- PC keyboard → GrvEP key mapping ----
// Uses SDL_Scancode (physical key position, layout-independent: AZERTY/QWERTY/etc.)
// Columns are reversed: leftmost physical key = col 7 (low notes), rightmost = col 0 (high notes)
// This matches the GrvEP hardware where high notes are on the right (towards right hand).
//
// Physical layout on AZERTY (what the user sees on their keyboard):
//   Row 3: &  é  "  '  (  -  è  _   →  cols 7..0
//   Row 2: A  Z  E  R  T  Y  U  I   →  cols 7..0
//   Row 1: Q  S  D  F  G  H  J  K   →  cols 7..0
//   Row 0: W  X  C  V  B  N  ?  .   →  cols 7..0
//   Menu:  F1 F2 F3 F4               →  cols 4..7

struct KeyMap { SDL_Scancode scan; uint8_t row; uint8_t col; };
static const KeyMap kKeyMap[] = {
    // Row 3 — number row, leftmost=col7 (low), rightmost=col0 (high)
    {SDL_SCANCODE_1, 3, 7}, {SDL_SCANCODE_2, 3, 6}, {SDL_SCANCODE_3, 3, 5}, {SDL_SCANCODE_4, 3, 4},
    {SDL_SCANCODE_5, 3, 3}, {SDL_SCANCODE_6, 3, 2}, {SDL_SCANCODE_7, 3, 1}, {SDL_SCANCODE_8, 3, 0},
    // Row 2 — AZERTY: A Z E R T Y U I
    {SDL_SCANCODE_Q, 2, 7}, {SDL_SCANCODE_W, 2, 6}, {SDL_SCANCODE_E, 2, 5}, {SDL_SCANCODE_R, 2, 4},
    {SDL_SCANCODE_T, 2, 3}, {SDL_SCANCODE_Y, 2, 2}, {SDL_SCANCODE_U, 2, 1}, {SDL_SCANCODE_I, 2, 0},
    // Row 1 — AZERTY: Q S D F G H J K
    {SDL_SCANCODE_A, 1, 7}, {SDL_SCANCODE_S, 1, 6}, {SDL_SCANCODE_D, 1, 5}, {SDL_SCANCODE_F, 1, 4},
    {SDL_SCANCODE_G, 1, 3}, {SDL_SCANCODE_H, 1, 2}, {SDL_SCANCODE_J, 1, 1}, {SDL_SCANCODE_K, 1, 0},
    // Row 0 — AZERTY: W X C V B N ? .
    {SDL_SCANCODE_Z, 0, 7}, {SDL_SCANCODE_X, 0, 6}, {SDL_SCANCODE_C, 0, 5}, {SDL_SCANCODE_V, 0, 4},
    {SDL_SCANCODE_B, 0, 3}, {SDL_SCANCODE_N, 0, 2}, {SDL_SCANCODE_M, 0, 1}, {SDL_SCANCODE_COMMA, 0, 0},
    // Menu row (row 4, cols 4-7) = B1(FX) B2(Arp) B3(Env) B4(Oct)
    {SDL_SCANCODE_F1, 4, 4}, {SDL_SCANCODE_F2, 4, 5}, {SDL_SCANCODE_F3, 4, 6}, {SDL_SCANCODE_F4, 4, 7},
};
static const int kKeyMapSize = sizeof(kKeyMap) / sizeof(kKeyMap[0]);

// Fallback keycode table for AZERTY keys that may report non-standard scancodes.
// SDL_Keycode = Unicode codepoint of the character produced (unshifted on AZERTY).
struct KeyMapKC { SDL_Keycode kc; uint8_t row; uint8_t col; };
static const KeyMapKC kKeyMapKC[] = {
    // Number row AZERTY chars (unshifted) — same row/col as scancode table above
    {(SDL_Keycode)'&',      3, 7}, // position 1
    {(SDL_Keycode)0xE9,     3, 6}, // é — position 2
    {(SDL_Keycode)'"',      3, 5}, // " — position 3
    {(SDL_Keycode)'\'',     3, 4}, // ' — position 4
    {(SDL_Keycode)'(',      3, 3}, // ( — position 5
    {(SDL_Keycode)'-',      3, 2}, // - — position 6
    {(SDL_Keycode)0xE8,     3, 1}, // è — position 7
    {(SDL_Keycode)'_',      3, 0}, // _ — position 8
};
static const int kKeyMapKCSize = sizeof(kKeyMapKC) / sizeof(kKeyMapKC[0]);

// Joystick arrow key state
static bool s_arrowLeft  = false;
static bool s_arrowRight = false;
static bool s_arrowUp    = false;
static bool s_arrowDown  = false;

static void updateJoyFromKeys() {
    float x = 0.0f, y = 0.0f;
    if (s_arrowLeft)  x -= 1.0f;
    if (s_arrowRight) x += 1.0f;
    if (s_arrowUp)    y += 1.0f;
    if (s_arrowDown)  y -= 1.0f;
    g_simJoyX = x;
    g_simJoyY = y;
}

// ---- SDL state ----
static SDL_Window*   s_win     = nullptr;
static SDL_Renderer* s_rend    = nullptr;
static SDL_Texture*  s_oledTex = nullptr;

// Mouse drag (desktop) / single-touch controls (Android pots/joy)
static int   s_dragPot = -1;
static bool  s_dragJoy = false;
static bool  s_keyDown[KBD_ROWS][KBD_COLS] = {};
static int   s_mouseRow = -1, s_mouseCol = -1;

// JOY_SW on-screen button — shared state for both desktop and Android
static bool  s_jbtnDown = false;

static bool jbtnHitTest(int lx, int ly) {
    return lx >= JBTN_X && lx < JBTN_X + JBTN_W &&
           ly >= JBTN_Y && ly < JBTN_Y + JBTN_H;
}

// ---- Android multi-touch finger tracking ----
#ifdef __ANDROID__

#define MAX_FINGERS 10
struct FingerTarget {
    SDL_FingerID id;
    enum Kind { NONE, KEY, JOY, JBTN, POT } kind;
    int  row, col;    // KEY
    int  potIdx;      // POT
    float lastX, lastY; // JOY & POT — physical pixel position
};
static FingerTarget s_fingers[MAX_FINGERS] = {};

static FingerTarget* fingerFind(SDL_FingerID id) {
    for (int i = 0; i < MAX_FINGERS; i++)
        if (s_fingers[i].kind != FingerTarget::NONE && s_fingers[i].id == id)
            return &s_fingers[i];
    return nullptr;
}
static FingerTarget* fingerAlloc(SDL_FingerID id) {
    for (int i = 0; i < MAX_FINGERS; i++)
        if (s_fingers[i].kind == FingerTarget::NONE) {
            s_fingers[i] = {};
            s_fingers[i].id = id;
            return &s_fingers[i];
        }
    return nullptr;
}

// Convert normalized finger [0..1] to logical window coordinates [0..WIN_W/WIN_H]
// taking SDL's letterbox (from SDL_RenderSetLogicalSize) into account.
static float s_lboxScale = 1.0f;  // physical pixels per logical pixel (from letterbox)

static void fingerToLogical(SDL_TouchFingerEvent const& tf, int* lx, int* ly) {
    int pw, ph;
    SDL_GetWindowSize(s_win, &pw, &ph);
    float physX = tf.x * pw, physY = tf.y * ph;
    float fx, fy;
    SDL_RenderWindowToLogical(s_rend, (int)physX, (int)physY, &fx, &fy);
    *lx = (int)fx; *ly = (int)fy;
}

#endif // __ANDROID__

// ---- OLED rendering ----
static void drawOled() {
    if (!g_oledDirty) return;
    g_oledDirty = false;

    uint8_t* buf = oled.getBufferPtr();
    if (!buf) return;

    // u8g2 full-buffer layout: 16 tile rows × 128 columns
    // Each byte = 8 vertical pixels (bit 0 = top)
    // SDL_PIXELFORMAT_ARGB8888: uint32_t = (A<<24)|(R<<16)|(G<<8)|B
    uint32_t pixels[128 * 128];
    for (int ty = 0; ty < 16; ty++) {
        for (int x = 0; x < 128; x++) {
            uint8_t byte = buf[ty * 128 + x];
            for (int bit = 0; bit < 8; bit++) {
                int py = ty * 8 + bit;
                bool on = (byte >> bit) & 1;
                // White text on black background (ARGB8888, opaque)
                pixels[py * 128 + x] = on ? 0xFFFFFFFF : 0xFF000000;
            }
        }
    }
    SDL_UpdateTexture(s_oledTex, nullptr, pixels, 128 * sizeof(uint32_t));
}

static void renderOled() {
    // Black background for OLED area
    SDL_Rect bg = {OLED_X - 2, OLED_Y - 2, OLED_W + 4, OLED_H + 4};
    SDL_SetRenderDrawColor(s_rend, 10, 10, 10, 255);
    SDL_RenderFillRect(s_rend, &bg);

    SDL_Rect dst = {OLED_X, OLED_Y, OLED_W, OLED_H};
    SDL_RenderCopy(s_rend, s_oledTex, nullptr, &dst);

    // Thin border
    SDL_SetRenderDrawColor(s_rend, 60, 60, 60, 255);
    SDL_RenderDrawRect(s_rend, &bg);
}

// ---- Keyboard rendering ----
static const char* menuLabel(int col) {
    switch (col) {
        case 4: return "FX";
        case 5: return "Arp";
        case 6: return "Env";
        case 7: return "Oct";
    }
    return "";
}

static void renderKeyboard() {
    for (int row = 0; row < KBD_ROWS; row++) {
        int colStart = (row == 4) ? 4 : 0;
        for (int col = colStart; col < 8; col++) {
            // Columns reversed: col 7 on left (low notes), col 0 on right (high notes)
            int x = KBD_X + (7 - col) * (KEY_W + KEY_GAP);
            int y = KBD_Y + (4 - row) * (KEY_H + KEY_GAP);
            bool pressed = s_keyDown[row][col];
            SDL_Rect r = {x, y, KEY_W, KEY_H};

            CRGB led = keyLedColor(row, col);
            bool hasLed = (led.r | led.g | led.b) != 0;

            if (hasLed) {
                SDL_SetRenderDrawColor(s_rend, led.r, led.g, led.b, 255);
            } else if (row == 4) {
                SDL_SetRenderDrawColor(s_rend, 45, 45, 65, 255);
            } else {
                uint8_t shade = 35 + row * 5;
                SDL_SetRenderDrawColor(s_rend, shade, shade, shade + 8, 255);
            }
            SDL_RenderFillRect(s_rend, &r);

            if (hasLed)
                SDL_SetRenderDrawColor(s_rend, 150, 150, 150, 255);
            else
                SDL_SetRenderDrawColor(s_rend, 80, 80, 90, 255);
            SDL_RenderDrawRect(s_rend, &r);
        }
    }

    // PC keyboard hint labels (row labels on the left)
    // Small dots to show which physical keys correspond
}

// ---- Pot sliders ----

// Returns screen rects for pot i (knob at current slider position, track = full slider range)
static void potRect(int i, SDL_Rect* knob, SDL_Rect* track) {
    int x, y0;
    if (i < MPOT_COUNT) {
        x  = MPOT_X + i * MPOT_GAP;
        y0 = MPOT_Y;
    } else {
        x  = SPOT_X + (i - MPOT_COUNT) * SPOT_GAP;
        y0 = SPOT_Y;
    }
    float val = g_simSlider[i];
    int ky = y0 + (int)((1.0f - val) * (POT_H - POT_W));
    if (track) { track->x = x + POT_W/2 - 2; track->y = y0; track->w = 4;     track->h = POT_H; }
    if (knob)  { knob->x  = x;                knob->y  = ky; knob->w  = POT_W; knob->h  = POT_W; }
}

static void renderPots() {
    // Main group background (VOL / SHAPE / BPM)
    {
        SDL_Rect bg = {MPOT_X - 4, MPOT_Y - 4, MPOT_COUNT * MPOT_GAP + 4, POT_H + 8};
        SDL_SetRenderDrawColor(s_rend, 42, 38, 25, 255);
        SDL_RenderFillRect(s_rend, &bg);
    }
    // Secondary group background (FX params)
    {
        SDL_Rect bg = {SPOT_X - 4, SPOT_Y - 4, SPOT_COUNT * SPOT_GAP + 4, POT_H + 8};
        SDL_SetRenderDrawColor(s_rend, 22, 38, 42, 255);
        SDL_RenderFillRect(s_rend, &bg);
    }

    for (int i = 0; i < POT_COUNT; i++) {
        SDL_Rect track, knob;
        potRect(i, &knob, &track);

        SDL_SetRenderDrawColor(s_rend, 28, 28, 28, 255);
        SDL_RenderFillRect(s_rend, &track);

        bool isMain = (i < MPOT_COUNT);
        // Main: gold;  Secondary: teal
        SDL_SetRenderDrawColor(s_rend,
            isMain ? 160 :  60,
            isMain ? 160 : 160,
            isMain ?  90 : 140, 255);
        SDL_RenderFillRect(s_rend, &knob);
        SDL_SetRenderDrawColor(s_rend,
            isMain ? 210 : 100,
            isMain ? 210 : 210,
            isMain ? 130 : 185, 255);
        SDL_RenderDrawRect(s_rend, &knob);
    }
}

// ---- Joystick ----
static void renderJoystick() {
    // Background ring
    for (int dy = -JOY_R; dy <= JOY_R; dy++)
        for (int dx = -JOY_R; dx <= JOY_R; dx++) {
            int d2 = dx*dx + dy*dy;
            if (d2 <= JOY_R*JOY_R && d2 >= (JOY_R-3)*(JOY_R-3)) {
                SDL_SetRenderDrawColor(s_rend, 70, 70, 70, 255);
                SDL_RenderDrawPoint(s_rend, JOY_CX+dx, JOY_CY+dy);
            } else if (d2 < (JOY_R-3)*(JOY_R-3)) {
                SDL_SetRenderDrawColor(s_rend, 25, 25, 25, 255);
                SDL_RenderDrawPoint(s_rend, JOY_CX+dx, JOY_CY+dy);
            }
        }

    // Center cross
    SDL_SetRenderDrawColor(s_rend, 50, 50, 50, 255);
    SDL_RenderDrawLine(s_rend, JOY_CX-JOY_R+4, JOY_CY, JOY_CX+JOY_R-4, JOY_CY);
    SDL_RenderDrawLine(s_rend, JOY_CX, JOY_CY-JOY_R+4, JOY_CX, JOY_CY+JOY_R-4);

    // Knob
    int kx = JOY_CX + (int)(g_simJoyX * (JOY_R - JOY_KNOB_R - 2));
    int ky = JOY_CY - (int)(g_simJoyY * (JOY_R - JOY_KNOB_R - 2));
    uint8_t jc = g_simJoySW ? 240 : 160;
    for (int dy = -JOY_KNOB_R; dy <= JOY_KNOB_R; dy++)
        for (int dx = -JOY_KNOB_R; dx <= JOY_KNOB_R; dx++)
            if (dx*dx + dy*dy <= JOY_KNOB_R*JOY_KNOB_R) {
                SDL_SetRenderDrawColor(s_rend, jc, jc, jc/3, 255);
                SDL_RenderDrawPoint(s_rend, kx+dx, ky+dy);
            }
}

// ---- JOY_SW button (desktop + Android) ----
static void renderJoyBtn() {
    // Outer fill
    uint8_t r = s_jbtnDown ? 240 : 180;
    uint8_t g = s_jbtnDown ?  80 :  60;
    uint8_t b = s_jbtnDown ?  40 :  20;
    SDL_Rect btn = {JBTN_X, JBTN_Y, JBTN_W, JBTN_H};
    SDL_SetRenderDrawColor(s_rend, r/3, g/3, b/3, 255);
    SDL_RenderFillRect(s_rend, &btn);
    SDL_SetRenderDrawColor(s_rend, r, g, b, 255);
    SDL_RenderDrawRect(s_rend, &btn);
    // Inner border for depth
    SDL_Rect inner = {JBTN_X+3, JBTN_Y+3, JBTN_W-6, JBTN_H-6};
    SDL_SetRenderDrawColor(s_rend, r, g, b, 180);
    SDL_RenderDrawRect(s_rend, &inner);
    // Draw "OK" as simple geometric shapes (no SDL_ttf)
    int cx = JBTN_X + JBTN_W / 2, cy = JBTN_Y + JBTN_H / 2;
    // "O" — circle approximated as squares
    int rr = 12;
    SDL_SetRenderDrawColor(s_rend, 255, 220, 180, 255);
    for (int dy = -rr; dy <= rr; dy++)
        for (int dx = -rr; dx <= rr; dx++) {
            int d2 = dx*dx + dy*dy;
            if (d2 >= (rr-3)*(rr-3) && d2 <= rr*rr)
                SDL_RenderDrawPoint(s_rend, cx - 20 + dx, cy + dy);
        }
    // "K" — three lines (vertical bar + two diagonals)
    SDL_RenderDrawLine(s_rend, cx+4, cy-rr+2, cx+4, cy+rr-2); // vertical
    SDL_RenderDrawLine(s_rend, cx+4, cy,       cx+20, cy-rr+2); // upper diagonal
    SDL_RenderDrawLine(s_rend, cx+4, cy,       cx+20, cy+rr-2); // lower diagonal
}

// ---- Hit tests ----
static bool potHitTest(int mx, int my, int* idx) {
    for (int i = 0; i < POT_COUNT; i++) {
        SDL_Rect track, knob;
        potRect(i, &knob, &track);
        if ((mx >= knob.x  && mx < knob.x  + knob.w  && my >= knob.y  && my < knob.y  + knob.h) ||
            (mx >= track.x && mx < track.x + track.w && my >= track.y && my < track.y + track.h)) {
            *idx = i;
            return true;
        }
    }
    return false;
}

static bool keyHitTest(int mx, int my, int* row, int* col) {
    for (int r = 0; r < KBD_ROWS; r++) {
        int colStart = (r == 4) ? 4 : 0;
        for (int c = colStart; c < 8; c++) {
            int x = KBD_X + (7 - c) * (KEY_W + KEY_GAP); // reversed columns
            int y = KBD_Y + (4 - r) * (KEY_H + KEY_GAP);
            if (mx >= x && mx < x + KEY_W &&
                my >= y && my < y + KEY_H) {
                *row = r; *col = c;
                return true;
            }
        }
    }
    return false;
}

static bool joyHitTest(int mx, int my) {
    int dx = mx - JOY_CX, dy = my - JOY_CY;
    return dx*dx + dy*dy <= JOY_R*JOY_R;
}

// ---- Public API ----
bool simWindowInit() {
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return false;
    }
#ifdef __ANDROID__
    // Force landscape on Android, disable touch-to-mouse (we handle SDL_FINGER directly)
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    // Fullscreen window — actual resolution determined by device
    s_win = SDL_CreateWindow("GrvEP",
        SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
        0, 0, SDL_WINDOW_FULLSCREEN_DESKTOP);
#else
    s_win = SDL_CreateWindow("GrvEP Simulator",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        WIN_W, WIN_H, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
#endif
    if (!s_win) {
        fprintf(stderr, "CreateWindow: %s\n", SDL_GetError());
        return false;
    }
    // Disable text input mode so the input method (IBus/Fcitx) does not intercept
    // extended Latin keys (é, è, etc.) and swallow their SDL_KEYDOWN events.
    SDL_StopTextInput();
    s_rend = SDL_CreateRenderer(s_win, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!s_rend) return false;

    // Map our fixed logical canvas onto whatever the window size is (letterboxed).
    // Mouse coords must be converted via SDL_RenderWindowToLogical (done in event handlers).
    SDL_RenderSetLogicalSize(s_rend, WIN_W, WIN_H);

#ifdef __ANDROID__
    // Compute letterbox scale for converting physical finger deltas to logical units
    {
        int pw, ph;
        SDL_GetWindowSize(s_win, &pw, &ph);
        float sx = (float)pw / WIN_W, sy = (float)ph / WIN_H;
        s_lboxScale = (sx < sy) ? sx : sy;
    }
#endif

    // ARGB8888: 0xFF000000 = opaque black, 0xFFFFFFFF = opaque white
    s_oledTex = SDL_CreateTexture(s_rend,
        SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, 128, 128);
    if (!s_oledTex) return false;

    return true;
}

void simWindowDestroy() {
    if (s_oledTex) { SDL_DestroyTexture(s_oledTex); s_oledTex = nullptr; }
    if (s_rend)    { SDL_DestroyRenderer(s_rend); s_rend = nullptr; }
    if (s_win)     { SDL_DestroyWindow(s_win); s_win = nullptr; }
    SDL_Quit();
}

// Manual window→logical conversion that mirrors SDL_RenderSetLogicalSize letterboxing.
// Avoids SDL_RenderWindowToLogical quirks on some platforms/DPI settings.
static float getLetterboxScale() {
    int winW, winH;
    SDL_GetWindowSize(s_win, &winW, &winH);
    float sx = (float)winW / WIN_W, sy = (float)winH / WIN_H;
    return fminf(sx, sy);
}

static void windowToLogical(int wx, int wy, int* lx, int* ly) {
    int winW, winH;
    SDL_GetWindowSize(s_win, &winW, &winH);
    float scale = getLetterboxScale();
    int offsetX = (winW - (int)(WIN_W * scale)) / 2;
    int offsetY = (winH - (int)(WIN_H * scale)) / 2;
    *lx = (int)((wx - offsetX) / scale);
    *ly = (int)((wy - offsetY) / scale);
}

bool simWindowPollEvents() {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {

        case SDL_QUIT:
            return false;

        // ---- PC Keyboard (scancodes = physical positions, AZERTY/QWERTY independent) ----
        case SDL_KEYDOWN:
            if (e.key.repeat) break;
            if (e.key.keysym.scancode == SDL_SCANCODE_ESCAPE) return false;

            switch (e.key.keysym.scancode) {
            case SDL_SCANCODE_LEFT:  s_arrowLeft  = true; updateJoyFromKeys(); break;
            case SDL_SCANCODE_RIGHT: s_arrowRight = true; updateJoyFromKeys(); break;
            case SDL_SCANCODE_UP:    s_arrowUp    = true; updateJoyFromKeys(); break;
            case SDL_SCANCODE_DOWN:  s_arrowDown  = true; updateJoyFromKeys(); break;
            // Numpad as redundant joystick (layout: 8=up, 2=down, 4=left, 6=right, 5=center, diagonals=7/9/1/3)
            case SDL_SCANCODE_KP_4: s_arrowLeft  = true;  s_arrowRight = false; updateJoyFromKeys(); break;
            case SDL_SCANCODE_KP_6: s_arrowRight = true;  s_arrowLeft  = false; updateJoyFromKeys(); break;
            case SDL_SCANCODE_KP_8: s_arrowUp    = true;  s_arrowDown  = false; updateJoyFromKeys(); break;
            case SDL_SCANCODE_KP_2: s_arrowDown  = true;  s_arrowUp    = false; updateJoyFromKeys(); break;
            case SDL_SCANCODE_KP_7: s_arrowLeft  = true;  s_arrowRight = false; s_arrowUp   = true;  s_arrowDown = false; updateJoyFromKeys(); break;
            case SDL_SCANCODE_KP_9: s_arrowRight = true;  s_arrowLeft  = false; s_arrowUp   = true;  s_arrowDown = false; updateJoyFromKeys(); break;
            case SDL_SCANCODE_KP_1: s_arrowLeft  = true;  s_arrowRight = false; s_arrowDown = true;  s_arrowUp   = false; updateJoyFromKeys(); break;
            case SDL_SCANCODE_KP_3: s_arrowRight = true;  s_arrowLeft  = false; s_arrowDown = true;  s_arrowUp   = false; updateJoyFromKeys(); break;
            case SDL_SCANCODE_KP_5:
                s_arrowLeft = s_arrowRight = s_arrowUp = s_arrowDown = false;
                updateJoyFromKeys(); break;
            case SDL_SCANCODE_SPACE:
            case SDL_SCANCODE_RETURN: g_simJoySW = true; break;
            case SDL_SCANCODE_TAB:
                // Close any active overlay: inject a note key at col=0 (below all colMin thresholds).
                // The overlay handler returns early after clearing the overlay, so no note plays.
                simKeyPress(0, 0, true);
                simKeyPress(0, 0, false);
                break;
            default: {
                bool found = false;
                for (int i = 0; i < kKeyMapSize; i++) {
                    if (kKeyMap[i].scan == e.key.keysym.scancode) {
                        uint8_t r = kKeyMap[i].row, c = kKeyMap[i].col;
                        if (!s_keyDown[r][c]) {
                            s_keyDown[r][c] = true;
                            simKeyPress(r, c, true);
                        }
                        found = true;
                        fprintf(stderr, "[sim] key DOWN: scancode=%d sym=0x%x → row=%d col=%d (scancode match)\n",
                                (int)e.key.keysym.scancode, (unsigned)e.key.keysym.sym, r, c);
                        break;
                    }
                }
                if (!found) {
                    // Fallback: match by keycode (Unicode codepoint) for AZERTY special chars
                    for (int i = 0; i < kKeyMapKCSize; i++) {
                        if (kKeyMapKC[i].kc == e.key.keysym.sym) {
                            uint8_t r = kKeyMapKC[i].row, c = kKeyMapKC[i].col;
                            if (!s_keyDown[r][c]) {
                                s_keyDown[r][c] = true;
                                simKeyPress(r, c, true);
                            }
                            found = true;
                            fprintf(stderr, "[sim] key DOWN: scancode=%d sym=0x%x → row=%d col=%d (keycode match)\n",
                                    (int)e.key.keysym.scancode, (unsigned)e.key.keysym.sym, r, c);
                            break;
                        }
                    }
                }
                if (!found) {
                    fprintf(stderr, "[sim] key DOWN UNRECOGNIZED: scancode=%d sym=0x%x\n",
                            (int)e.key.keysym.scancode, (unsigned)e.key.keysym.sym);
                }
                break;
            }
            }
            break;

        case SDL_KEYUP:
            switch (e.key.keysym.scancode) {
            case SDL_SCANCODE_LEFT:  s_arrowLeft  = false; updateJoyFromKeys(); break;
            case SDL_SCANCODE_RIGHT: s_arrowRight = false; updateJoyFromKeys(); break;
            case SDL_SCANCODE_UP:    s_arrowUp    = false; updateJoyFromKeys(); break;
            case SDL_SCANCODE_DOWN:  s_arrowDown  = false; updateJoyFromKeys(); break;
            case SDL_SCANCODE_KP_4: s_arrowLeft  = false; updateJoyFromKeys(); break;
            case SDL_SCANCODE_KP_6: s_arrowRight = false; updateJoyFromKeys(); break;
            case SDL_SCANCODE_KP_8: s_arrowUp    = false; updateJoyFromKeys(); break;
            case SDL_SCANCODE_KP_2: s_arrowDown  = false; updateJoyFromKeys(); break;
            case SDL_SCANCODE_KP_7: s_arrowLeft  = false; s_arrowUp   = false; updateJoyFromKeys(); break;
            case SDL_SCANCODE_KP_9: s_arrowRight = false; s_arrowUp   = false; updateJoyFromKeys(); break;
            case SDL_SCANCODE_KP_1: s_arrowLeft  = false; s_arrowDown = false; updateJoyFromKeys(); break;
            case SDL_SCANCODE_KP_3: s_arrowRight = false; s_arrowDown = false; updateJoyFromKeys(); break;
            case SDL_SCANCODE_KP_5: break; // center handled on press
            case SDL_SCANCODE_SPACE:
            case SDL_SCANCODE_RETURN: g_simJoySW = false; break;
            default: {
                bool found = false;
                for (int i = 0; i < kKeyMapSize; i++) {
                    if (kKeyMap[i].scan == e.key.keysym.scancode) {
                        uint8_t r = kKeyMap[i].row, c = kKeyMap[i].col;
                        if (s_keyDown[r][c]) {
                            s_keyDown[r][c] = false;
                            simKeyPress(r, c, false);
                        }
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    for (int i = 0; i < kKeyMapKCSize; i++) {
                        if (kKeyMapKC[i].kc == e.key.keysym.sym) {
                            uint8_t r = kKeyMapKC[i].row, c = kKeyMapKC[i].col;
                            if (s_keyDown[r][c]) {
                                s_keyDown[r][c] = false;
                                simKeyPress(r, c, false);
                            }
                            break;
                        }
                    }
                }
                break;
            }
            }
            break;

        // ---- Mouse ----
        case SDL_MOUSEBUTTONDOWN: {
            int mx, my;
            windowToLogical(e.button.x, e.button.y, &mx, &my);
            int r, c, pidx;
            if (e.button.button == SDL_BUTTON_LEFT) {
                if (jbtnHitTest(mx, my)) {
                    g_simJoySW = true;
                    s_jbtnDown = true;
                } else if (keyHitTest(mx, my, &r, &c)) {
                    s_mouseRow = r; s_mouseCol = c;
                    if (!s_keyDown[r][c]) {
                        s_keyDown[r][c] = true;
                        simKeyPress(r, c, true);
                    }
                } else if (potHitTest(mx, my, &pidx)) {
                    s_dragPot = pidx;
                    SDL_SetRelativeMouseMode(SDL_TRUE);
                } else if (joyHitTest(mx, my)) {
                    s_dragJoy = true;
                    SDL_SetRelativeMouseMode(SDL_TRUE);
                }
            } else if (e.button.button == SDL_BUTTON_RIGHT) {
                if (joyHitTest(mx, my))
                    g_simJoySW = true;
            }
            break;
        }

        case SDL_MOUSEBUTTONUP:
            if (e.button.button == SDL_BUTTON_LEFT) {
                if (s_jbtnDown) {
                    g_simJoySW = false;
                    s_jbtnDown = false;
                }
                if (s_mouseRow >= 0) {
                    if (s_keyDown[s_mouseRow][s_mouseCol]) {
                        s_keyDown[s_mouseRow][s_mouseCol] = false;
                        simKeyPress(s_mouseRow, s_mouseCol, false);
                    }
                    s_mouseRow = s_mouseCol = -1;
                }
                if (s_dragPot >= 0) {
                    SDL_SetRelativeMouseMode(SDL_FALSE);
                    s_dragPot = -1;
                }
                if (s_dragJoy) {
                    SDL_SetRelativeMouseMode(SDL_FALSE);
                    s_dragJoy = false;
                    g_simJoyX = 0.0f;
                    g_simJoyY = 0.0f;
                }
            } else if (e.button.button == SDL_BUTTON_RIGHT) {
                g_simJoySW = false;
            }
            break;

        case SDL_MOUSEMOTION: {
            // Relative mouse deltas are in physical pixels; divide by letterbox scale to get logical.
            float sc = getLetterboxScale();
            if (s_dragPot >= 0 && s_dragPot < POT_COUNT && s_dragPot < 16) {
                float delta = -(float)e.motion.yrel / sc / (float)(POT_H - POT_W);
                g_simSlider[s_dragPot] = fmaxf(0.0f,
                    fminf(1.0f, g_simSlider[s_dragPot] + delta));
            }
            if (s_dragJoy) {
                float dx = (float)e.motion.xrel / sc / (float)JOY_R;
                float dy = -(float)e.motion.yrel / sc / (float)JOY_R;
                g_simJoyX = fmaxf(-1.0f, fminf(1.0f, g_simJoyX + dx));
                g_simJoyY = fmaxf(-1.0f, fminf(1.0f, g_simJoyY + dy));
            }
            break;
        }

        case SDL_MOUSEWHEEL: {
            int wx, wy;
            SDL_GetMouseState(&wx, &wy);
            int mx, my;
            windowToLogical(wx, wy, &mx, &my);
            int pidx;
            if (potHitTest(mx, my, &pidx) && pidx < 16) {
                float delta = e.wheel.y * 0.025f;
                g_simSlider[pidx] = fmaxf(0.0f,
                    fminf(1.0f, g_simSlider[pidx] + delta));
            }
            break;
        }

        // ---- Android multi-touch (handles all input on mobile) ----
#ifdef __ANDROID__
        case SDL_FINGERDOWN: {
            int lx, ly;
            fingerToLogical(e.tfinger, &lx, &ly);
            FingerTarget* ft = fingerAlloc(e.tfinger.fingerId);
            if (!ft) break;
            ft->kind = FingerTarget::NONE;
            int r, c, pidx;
            if (keyHitTest(lx, ly, &r, &c)) {
                ft->kind = FingerTarget::KEY;
                ft->row = r; ft->col = c;
                if (!s_keyDown[r][c]) {
                    s_keyDown[r][c] = true;
                    simKeyPress(r, c, true);
                }
            } else if (jbtnHitTest(lx, ly)) {
                ft->kind = FingerTarget::JBTN;
                g_simJoySW = true;
                s_jbtnDown = true;
            } else if (joyHitTest(lx, ly)) {
                ft->kind = FingerTarget::JOY;
                int pw, ph; SDL_GetWindowSize(s_win, &pw, &ph);
                ft->lastX = e.tfinger.x * pw;
                ft->lastY = e.tfinger.y * ph;
            } else if (potHitTest(lx, ly, &pidx)) {
                ft->kind = FingerTarget::POT;
                ft->potIdx = pidx;
                int pw, ph; SDL_GetWindowSize(s_win, &pw, &ph);
                ft->lastY = e.tfinger.y * ph;
                // Jump pot to touched position
                SDL_Rect knob, track;
                potRect(pidx, &knob, &track);
                float rel = 1.0f - (float)(ly - track.y) / (float)track.h;
                g_simSlider[pidx] = fmaxf(0.0f, fminf(1.0f, rel));
            }
            break;
        }
        case SDL_FINGERUP: {
            FingerTarget* ft = fingerFind(e.tfinger.fingerId);
            if (!ft) break;
            if (ft->kind == FingerTarget::KEY) {
                if (s_keyDown[ft->row][ft->col]) {
                    s_keyDown[ft->row][ft->col] = false;
                    simKeyPress(ft->row, ft->col, false);
                }
            } else if (ft->kind == FingerTarget::JBTN) {
                g_simJoySW = false;
                s_jbtnDown = false;
            } else if (ft->kind == FingerTarget::JOY) {
                g_simJoyX = 0.0f;
                g_simJoyY = 0.0f;
            }
            ft->kind = FingerTarget::NONE;
            break;
        }
        case SDL_FINGERMOTION: {
            FingerTarget* ft = fingerFind(e.tfinger.fingerId);
            if (!ft) break;
            int pw, ph; SDL_GetWindowSize(s_win, &pw, &ph);
            float physX = e.tfinger.x * pw, physY = e.tfinger.y * ph;
            if (ft->kind == FingerTarget::JOY) {
                float dx = (physX - ft->lastX) / (JOY_R * s_lboxScale);
                float dy = -(physY - ft->lastY) / (JOY_R * s_lboxScale);
                g_simJoyX = fmaxf(-1.0f, fminf(1.0f, g_simJoyX + dx));
                g_simJoyY = fmaxf(-1.0f, fminf(1.0f, g_simJoyY + dy));
                ft->lastX = physX; ft->lastY = physY;
            } else if (ft->kind == FingerTarget::POT) {
                float dy = -(physY - ft->lastY) / (POT_H * s_lboxScale);
                g_simSlider[ft->potIdx] = fmaxf(0.0f,
                    fminf(1.0f, g_simSlider[ft->potIdx] + dy));
                ft->lastY = physY;
            }
            break;
        }
#endif // __ANDROID__

        } // switch
    }
    return true;
}

static void renderPanels() {
    // Left panel background
    SDL_Rect lp = {0, 0, SPLIT_X, WIN_H};
    SDL_SetRenderDrawColor(s_rend, 16, 16, 20, 255);
    SDL_RenderFillRect(s_rend, &lp);

    // Right panel top (keyboard area)
    SDL_Rect rt = {SPLIT_X, 0, WIN_W - SPLIT_X, CTRL_Y};
    SDL_SetRenderDrawColor(s_rend, 20, 20, 26, 255);
    SDL_RenderFillRect(s_rend, &rt);

    // Right panel bottom (ctrl area)
    SDL_Rect rb = {SPLIT_X, CTRL_Y, WIN_W - SPLIT_X, WIN_H - CTRL_Y};
    SDL_SetRenderDrawColor(s_rend, 18, 22, 28, 255);
    SDL_RenderFillRect(s_rend, &rb);

    // Vertical divider
    SDL_SetRenderDrawColor(s_rend, 50, 50, 60, 255);
    SDL_RenderDrawLine(s_rend, SPLIT_X, 0, SPLIT_X, WIN_H);

    // Horizontal divider (right panel)
    SDL_RenderDrawLine(s_rend, SPLIT_X, CTRL_Y, WIN_W, CTRL_Y);
}

void simWindowRender() {
    drawOled();

    SDL_SetRenderDrawColor(s_rend, 18, 18, 22, 255);
    SDL_RenderClear(s_rend);

    renderPanels();
    renderOled();
    renderKeyboard();
    renderPots();
    renderJoystick();
    renderJoyBtn();

    SDL_RenderPresent(s_rend);
}
