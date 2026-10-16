#pragma once

// ============================================================================
// GrvEP Hardware Layout — Physical placement of all components
// ============================================================================
// This file describes the PHYSICAL layout and mapping between components.
// For electrical pin assignments, see HWConfig.h
//
// Use this file to adapt the firmware to a different PCB layout.
// ============================================================================

// --- KEYBOARD MATRIX (TCA8418) ---
// 5 rows × 8 columns, row 4 has only 4 keys (menu buttons)
//
// Physical layout (top view, keys facing you):
//
//   Row 4 (menu):  [Scale] [Arp] [Env] [Oct]           ← 4 buttons
//   Row 3 (notes): [  0  ] [  1 ] [ 2 ] [ 3 ] [ 4 ] [ 5 ] [ 6 ] [ 7 ]
//   Row 2 (notes): [  0  ] [  1 ] [ 2 ] [ 3 ] [ 4 ] [ 5 ] [ 6 ] [ 7 ]
//   Row 1 (notes): [  0  ] [  1 ] [ 2 ] [ 3 ] [ 4 ] [ 5 ] [ 6 ] [ 7 ]
//   Row 0 (notes): [  0  ] [  1 ] [ 2 ] [ 3 ] [ 4 ] [ 5 ] [ 6 ] [ 7 ]
//
// NOTE: Display and LEDs use inverted row/col (KBD_ROWS-1-r, KBD_COLS-1-c)
// NOTE: One column has a hardware issue causing full-row ghost events

#define KBD_DISPLAY_INVERT_ROWS 1
#define KBD_DISPLAY_INVERT_COLS 1

// --- MENU BUTTONS (in keyboard matrix row 4, cols 4-7) ---
// Physical col → button index: col - 4
#define MENU_BTN_SCALE  0   // Col 4 in row 4 — cycles through scales
#define MENU_BTN_ARP    1   // Col 5 in row 4 — cycles arp mode
#define MENU_BTN_ENV    2   // Col 6 in row 4 — cycles envelope preset
#define MENU_BTN_OCT    3   // Col 7 in row 4 — cycles octave

// --- ENCODER POTENTIOMETERS (via MUX, 2 channels each) ---
// 7 encoders total, sin/cos tracking via atan2
// MUX channels: pot N uses CH(2+N*2) and CH(2+N*2+1)
//
// Physical layout (top view):
//
//   TOP ROW (3 encoders, aligned horizontally above keyboard):
//     [Pot 0: Volume]   [Pot 1: Cutoff]   [Pot 2: Resonance]
//
//   BOTTOM GROUP (4 encoders, around joystick):
//        [Pot 3]          [Pot 4]
//            [ JOYSTICK ]
//        [Pot 5]          [Pot 6]
//
// Pot function mapping (index → parameter):
#define POT_VOLUME      0   // Top-left     — always: master volume
#define POT_CUTOFF      1   // Top-center   — always: filter cutoff
#define POT_RESONANCE   2   // Top-right    — always: filter resonance
#define POT_CTX_TL      3   // Bottom top-left  — contextual (ADSR page: Attack)
#define POT_CTX_TR      4   // Bottom top-right — contextual (ADSR page: Decay)
#define POT_CTX_BL      5   // Bottom bot-left  — contextual (ADSR page: Sustain)
#define POT_CTX_BR      6   // Bottom bot-right — contextual (ADSR page: Release)

// Context pot page labels (displayed on OLED, matches physical position):
//   Page ADSR:   TL=Release TR=Attack  BL=Sustain  BR=Decay
//   Page FX:     TL=Reverb  TR=Delay   BL=LFO Frq  BR=LFO Amp
//   Page CUSTOM: TL=Wave1   TR=Wave2   BL=Detune   BR=Balance
//
// Physical pot → MUX index mapping (verified by user):
//   TL (top-left)     = MUX pot 6 = context index 3
//   TR (top-right)    = MUX pot 3 = context index 0
//   BL (bottom-left)  = MUX pot 5 = context index 2
//   BR (bottom-right) = MUX pot 4 = context index 1
//
// ADSR page display (matches physical layout):
//   [TL: Release]  [TR: Attack ]
//   [BL: Sustain]  [BR: Decay  ]
#define CTX_POT_TL 3   // MUX pot 6
#define CTX_POT_TR 0   // MUX pot 3
#define CTX_POT_BL 2   // MUX pot 5
#define CTX_POT_BR 1   // MUX pot 4

// --- JOYSTICK (analog X/Y + click) ---
//
//   Located center-bottom, surrounded by 4 context pots
//   X axis: pitch bend (±2 semitones)
//   Y axis: modulation (filter sweep)
//   Click:  toggle instrument menu (pin TBD — testing all TCA GPIOs)

// --- LED STRIP (SK6812, 36 LEDs) ---
// Serpentine layout under keyboard keys, mapped via crdToIdx()
// See Leds.cpp for the index table (OOPSIE_LED_FLAG variant)
//
// LED grid (physical, top view — matches key grid):
//   Row 4 (menu):  LED 32  33  34  35          ← 4 LEDs
//   Row 3 (notes): LED 31  30  29  28  27  26  25  24  ← reversed
//   Row 2 (notes): LED 16  17  18  19  20  21  22  23
//   Row 1 (notes): LED 15  14  13  12  11  10   9   8  ← reversed
//   Row 0 (notes): LED  0   1   2   3   4   5   6   7

// --- OLED SCREEN ---
// SH1107 128×128, I2C on Wire0, positioned above keyboard
// Display offset: sendF("ca", 0xD3, -32) required for this panel

// --- SPEAKER / DAC ---
// MAX98357A I2S amplifier — no MCLK needed
// Speaker output + 3.5mm jack

// --- SD CARD ---
// SPI bus for MP3/WAV sample loading
// Directly below or beside the main PCB
