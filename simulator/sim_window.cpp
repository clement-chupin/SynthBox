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

// ---- Window size ----
#define WIN_W  820
#define WIN_H  560

// ---- OLED (128×128 ×3 = 384×384) ----
#define OLED_SCALE 3
#define OLED_X     10
#define OLED_Y     10
#define OLED_W     (128 * OLED_SCALE)
#define OLED_H     (128 * OLED_SCALE)

// ---- Keyboard (4 note rows + 1 menu row) ----
// Positioned to the right of the OLED
#define KBD_X      (OLED_X + OLED_W + 18)
#define KBD_Y      10
#define KEY_W      36
#define KEY_H      30
#define KEY_GAP    3
#define KBD_ROWS   5
#define KBD_COLS   8

// ---- Pot sliders ----
#define POT_COUNT  7
#define POT_W      14
#define POT_H      80

// Main pots (0,1,2) — VOL / SHAPE / BPM — left group
#define MPOT_X     (OLED_X)
#define MPOT_Y     (OLED_Y + OLED_H + 20)
#define MPOT_GAP   30
#define MPOT_COUNT 3

// ---- Joystick (between the two pot groups) ----
#define JOY_CX     (MPOT_X + MPOT_COUNT * MPOT_GAP + 46)
#define JOY_CY     (MPOT_Y + POT_H / 2)
#define JOY_R      32
#define JOY_KNOB_R  9

// Secondary pots (3,4,5,6) — FX params — right of joystick
#define SPOT_X     (JOY_CX + JOY_R + 18)
#define SPOT_Y     MPOT_Y
#define SPOT_GAP   26
#define SPOT_COUNT 4

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

// Mouse drag
static int   s_dragPot = -1;
static bool  s_dragJoy = false;
static bool  s_keyDown[KBD_ROWS][KBD_COLS] = {};
static int   s_mouseRow = -1, s_mouseCol = -1;

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

// ---- Help text ----
static void renderHelp() {
    // We don't have font rendering in SDL without SDL_ttf.
    // Draw small indicator dots next to keyboard rows.
    // Just draw colored row labels using thin lines.
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
    s_win = SDL_CreateWindow("GrvEP Simulator",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        WIN_W, WIN_H, SDL_WINDOW_SHOWN);
    if (!s_win) {
        fprintf(stderr, "CreateWindow: %s\n", SDL_GetError());
        return false;
    }
    s_rend = SDL_CreateRenderer(s_win, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!s_rend) return false;

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
                            break;
                        }
                    }
                }
                if (!found) {
                    fprintf(stderr, "[sim] unrecognized key: scancode=%d sym=0x%x\n",
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
            int mx = e.button.x, my = e.button.y;
            int r, c, pidx;
            if (e.button.button == SDL_BUTTON_LEFT) {
                if (keyHitTest(mx, my, &r, &c)) {
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

        case SDL_MOUSEMOTION:
            if (s_dragPot >= 0 && s_dragPot < POT_COUNT && s_dragPot < 16) {
                float delta = -(float)e.motion.yrel / (float)(POT_H - POT_W);
                g_simSlider[s_dragPot] = fmaxf(0.0f,
                    fminf(1.0f, g_simSlider[s_dragPot] + delta));
            }
            if (s_dragJoy) {
                float dx = (float)e.motion.xrel / (float)JOY_R;
                float dy = -(float)e.motion.yrel / (float)JOY_R;
                g_simJoyX = fmaxf(-1.0f, fminf(1.0f, g_simJoyX + dx));
                g_simJoyY = fmaxf(-1.0f, fminf(1.0f, g_simJoyY + dy));
            }
            break;

        case SDL_MOUSEWHEEL: {
            int mx, my;
            SDL_GetMouseState(&mx, &my);
            int pidx;
            if (potHitTest(mx, my, &pidx) && pidx < 16) {
                float delta = e.wheel.y * 0.025f;
                g_simSlider[pidx] = fmaxf(0.0f,
                    fminf(1.0f, g_simSlider[pidx] + delta));
            }
            break;
        }

        } // switch
    }
    return true;
}

void simWindowRender() {
    drawOled();

    SDL_SetRenderDrawColor(s_rend, 18, 18, 22, 255);
    SDL_RenderClear(s_rend);

    renderOled();
    renderKeyboard();
    renderPots();
    renderJoystick();

    SDL_RenderPresent(s_rend);
}
