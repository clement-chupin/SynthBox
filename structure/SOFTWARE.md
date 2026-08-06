# GrvEP v2 — Software Architecture

## File Structure

```
src/
├── main.cpp            Loop: amy_update → keys → joystick → LEDs → pots → display
├── config.h            Modes, shapes, envelopes, effects, menu, constants
├── audio_engine.h/cpp  AMY init, noteOn/Off, filter, envelope, shape, volume
├── keyboard.cpp        TCA8418 driver + event queue (kept from v1)
├── Leds.cpp            SK6812 driver (kept from v1)
├── mux.cpp             16-ch analog MUX (kept from v1)
├── NoteMap.cpp         Scale mapping (kept from v1)
├── oled.cpp            SH1107 init (kept from v1)

include/
├── HWConfig.h          GPIO pin definitions
├── HWLayout.h          Physical component placement
├── keyboard.h, mux.h, Leds.h, oled.h, joystick.h, NoteMap.h, logger.h

structure/
├── HARDWARE.md         Hardware documentation
└── SOFTWARE.md         This file
```

## Modes

| Mode | Description |
|------|------------|
| SYNTH | Polyphonic synth (20 shapes from clavier_v3) |
| OMNI | Omnichord (chords + joystick strum) |
| DRUMS | Drum machine + sequencer (planned) |
| SAMPLE | Sample player SD + .h (planned) |
| FX | Effects chain (planned) |
| TRACKER | 8-track sequencer (planned) |
| SCENE | Scene save/load (planned) |
| LIGHT | LED light show (planned) |

## Audio Pipeline

```
Key → NoteMap → audioNoteOn(note, vel) → AMY synth channel 1 → I2S → DAC
                                              ↑
Pots → audioSetFilter/Volume/Envelope ────────┘
Joystick → audioSetPitchBend ─────────────────┘
```

## AMY Init Sequence (CRITICAL)

```
1. PWR_ON_EN HIGH
2. I2S_XSMT HIGH  
3. ALL hardware init (LEDs, keyboard, OLED, joystick, SD)
4. amy_start(cfg)     ← i2s_mclk=9, i2s_bclk=9, i2s_lrc=7, i2s_dout=8
5. esp32_setup_i2s()   ← mandatory after amy_start
6. delay(500)
7. Create synth channel 1

DO NOT touch SPK_SD (GPIO 5) after amy_start
amy_update() MUST be called every loop iteration
allNotesOff() on every mode switch
```

## Loop Priorities

1. `amy_update()` — audio rendering (first, always)
2. `pollKeyboard()` + key events — instant response
3. Joystick click — menu toggle
4. LEDs — 20fps with delay(1) protection
5. Pots + joystick analog — 50ms (20Hz)
6. Display — 100ms (10fps)
7. Power check — 500ms
