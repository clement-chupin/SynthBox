# GrvEP — Hardware Documentation

## MCU
- **ESP32-S3** (16MB Flash, PSRAM)
- Framework: Arduino via PlatformIO

## Pin Map

### Audio (I2S → PCM5102A DAC + MAX98357A speaker amp)
| Signal | GPIO | Notes |
|--------|------|-------|
| BCLK   | 9    | Bit clock (also routed as MCLK in AMY config) |
| LRCK   | 7    | Word select |
| DOUT   | 8    | Data out |
| XSMT   | 13   | PCM5102A soft mute — **MUST be HIGH** for audio |
| SPK_SD | 5    | MAX98357A enable — HIGH = speaker on |

### Keyboard (TCA8418 via I2C Wire1)
| Signal | GPIO |
|--------|------|
| SDA    | 42   |
| SCL    | 41   |

Matrix: 5 rows × 8 columns
- Rows 0-3: 32 note keys
- Row 4, cols 4-7: 4 menu buttons

### OLED Display (SH1107 128×128 via I2C Wire0)
| Signal | GPIO |
|--------|------|
| SDA    | 38   |
| SCL    | 39   |

Rotation: U8G2_R0, offset: sendF("ca", 0xD3, -32)

### LED Strip (36 × SK6812)
| Signal    | GPIO |
|-----------|------|
| DATA      | 14   |
| Brightness| 50 (configurable) |

Serpentine layout under keyboard keys. Mapping via `crdToIdx()` table.
Display/LED inversion: rows (KBD_ROWS-1-r), cols (KBD_COLS-1-c).

### Joystick (Analog + Click)
| Signal | GPIO |
|--------|------|
| X axis | 2    |
| Y axis | 1    |
| Click  | 40   |

Click: INPUT_PULLUP, active LOW.

### Encoder Potentiometers (7 × sin/cos via MUX)
| Signal | GPIO |
|--------|------|
| MUX S0 | 18   |
| MUX S1 | 33   |
| MUX S2 | 35   |
| MUX S3 | 34   |
| MUX COM| 4    |

7 encoders, 2 MUX channels each (CH2-CH15):
- Pot 0 (CH2+3): Volume — top left
- Pot 1 (CH4+5): Cutoff — top center
- Pot 2 (CH6+7): Resonance — top right
- Pot 3 (CH8+9): Context — bottom, top-right of joystick
- Pot 4 (CH10+11): Context — bottom, bottom-right of joystick
- Pot 5 (CH12+13): Context — bottom, bottom-left of joystick
- Pot 6 (CH14+15): Context — bottom, top-left of joystick

Rotation tracking: atan2(Y-2048, X-2048), delta accumulation.
Pots 0-1: normal rotation. Pots 2-6: inverted rotation.

### SD Card (SPI)
| Signal | GPIO |
|--------|------|
| SCLK   | 15   |
| MOSI   | 36   |
| MISO   | 16   |
| CS     | 37   |

### Power
| Signal    | GPIO | Notes |
|-----------|------|-------|
| PWR_ON_EN | 6    | Latch — set HIGH immediately on boot |
| PWR_SENSE | 12   | Button sense — active LOW (1=idle, 0=pressed) |
