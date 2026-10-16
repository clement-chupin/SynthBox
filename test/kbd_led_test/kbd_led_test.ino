// kbd_led_test.ino — keyboard reactivity + LED test
// Lights up the LED for each key while it is held. Release turns it off.
// No sound, no OLED, no joystick, no ADC/pots.
// Target: ESP32-S3, TCA8418 (Wire1), SK6812 LEDs.

#include <Wire.h>
#include <FastLED.h>
#include <Adafruit_TCA8418.h>

// ---- Hardware pins (from HWConfig.h) ----
#define KBDSDA        42
#define KBDSCL        41
#define LED_DATA_PIN  14

// ---- Matrix dimensions ----
#define KBD_ROWS  5    // 4 note rows + 1 button row
#define KBD_COLS  8
#define NUM_LEDS  36

// ---- TCA8418 register addresses ----
#define TCA_CFG_REG      0x01
#define TCA_INT_STAT     0x02
#define TCA_OVF_MODE     0x04   // CFG bit 2

Adafruit_TCA8418 keyboard;
CRGB leds[NUM_LEDS];
bool keyHeld[KBD_ROWS][KBD_COLS] = {};

// LED routing table — matches the main firmware's crdToIdx() without OOPSIE_LED_FLAG.
// Index = row * KBD_COLS + col (0-based). Code row 0 = physical bottom.
//   Row 0: 8 keys share 4 LEDs (physical bottom, 2 keys/LED)
//   Rows 1-4: 8 keys → 8 LEDs each, serpentine strip
static const int8_t kLedTable[KBD_ROWS * KBD_COLS] = {
     3,  2,  1,  0,  3,  2,  1,  0,  // row 0 (physical bottom, shared 4 LEDs)
     4,  5,  6,  7,  8,  9, 10, 11,  // row 1
    19, 18, 17, 16, 15, 14, 13, 12,  // row 2
    20, 21, 22, 23, 24, 25, 26, 27,  // row 3
    35, 34, 33, 32, 31, 30, 29, 28,  // row 4 (physical top, button row)
};

// Hue per row for visual identification (red, yellow, cyan, blue, magenta)
static const uint8_t kRowHue[KBD_ROWS] = { 0, 32, 128, 160, 192 };

static void tcaWriteReg(uint8_t reg, uint8_t val) {
    Wire1.beginTransmission(TCA8418_DEFAULT_ADDR);
    Wire1.write(reg);
    Wire1.write(val);
    Wire1.endTransmission();
}

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("\n=== KBD+LED test ===");

    // LEDs first so we can signal errors visually
    FastLED.addLeds<SK6812, LED_DATA_PIN, BGR>(leds, NUM_LEDS);
    FastLED.setBrightness(80);
    FastLED.clear(true);

    // TCA8418 on Wire1
    Wire1.begin(KBDSDA, KBDSCL, 400000);
    Wire1.setTimeout(3);
    delay(50);

    if (!keyboard.begin(TCA8418_DEFAULT_ADDR, &Wire1)) {
        Serial.println("TCA8418 FAILED — check wiring");
        for (;;) {
            fill_solid(leds, NUM_LEDS, CRGB::Red);
            FastLED.show(); delay(300);
            FastLED.clear(true);          FastLED.show(); delay(300);
        }
    }
    Serial.println("TCA8418 OK");

    keyboard.matrix(KBD_ROWS, KBD_COLS);
    // Disable hardware debounce: we handle it by tracking held state per key.
    // Gives fastest possible response (~4ms scan period instead of 16ms).
    keyboard.disableDebounce();
    tcaWriteReg(TCA_CFG_REG, TCA_OVF_MODE);

    // Drain any stale events from before init
    while (keyboard.available()) keyboard.getEvent();
    tcaWriteReg(TCA_INT_STAT, 0xFF);

    // Green sweep to confirm ready
    for (int i = 0; i < NUM_LEDS; i++) {
        leds[i] = CRGB::Green;
        FastLED.show();
        delay(15);
        leds[i] = CRGB::Black;
    }
    FastLED.show();
    Serial.println("Ready — press keys");
}

void loop() {
    int avail = keyboard.available();
    if (avail <= 0) return;
    if (avail > 10) avail = 10;

    bool changed = false;

    for (int e = 0; e < avail; e++) {
        int raw = keyboard.getEvent();
        if (raw == 0) continue;

        // TCA8418 convention: bit 7 = 1 → key pressed, bit 7 = 0 → released
        bool pressed = (raw & 0x80) != 0;
        int  kn      = (raw & 0x7F) - 1;  // 0-based key number
        int  row     = kn / 10;
        int  col     = kn % 10;

        if (row < 0 || row >= KBD_ROWS || col < 0 || col >= KBD_COLS) {
            // GPIO event or out-of-matrix key — log and skip
            Serial.printf("  skip raw=0x%02X kn=%d r=%d c=%d\n", raw, kn, row, col);
            continue;
        }

        Serial.printf("  r%d c%d %s\n", row, col, pressed ? "PRESS" : "release");
        keyHeld[row][col] = pressed;
        changed = true;
    }

    if (!changed) return;

    // Rebuild LED array from full held state
    FastLED.clear();
    for (int r = 0; r < KBD_ROWS; r++) {
        for (int c = 0; c < KBD_COLS; c++) {
            if (!keyHeld[r][c]) continue;
            int li = kLedTable[r * KBD_COLS + c];
            if (li >= 0 && li < NUM_LEDS)
                leds[li] = CHSV(kRowHue[r], 220, 255);
        }
    }
    FastLED.show();
}
