#include "keyboard.h"
#include "HWConfig.h"
#include "logger.h"

Adafruit_TCA8418 keyboard;
bool keyState[KBD_ROWS][KBD_COLS] = {};

#define TCA_INT_STAT    0x02
#define TCA_OVR_FLOW_BIT 0x02
#define TCA_GPIO_DAT1   0x14
#define TCA_GPIO_DAT3   0x16
#define TCA_GPIO_DIR1   0x23
#define TCA_GPIO_DIR3   0x25
#define TCA_GPIO_PULL1  0x1A
#define TCA_GPIO_PULL3  0x1C

#define KEY_TIMEOUT_MS      30000  // safety fallback for lost release events; does NOT affect press latency
#define KEY_TIMEOUT_FAST_MS  500
#define FAST_TIMEOUT_WINDOW 3000

static uint32_t keyPressTime[KBD_ROWS][KBD_COLS] = {};
static uint32_t lastOverflowTime = 0;

static KeyEvent keyQueue[KEY_QUEUE_SIZE];
static volatile uint8_t queueHead = 0;
static volatile uint8_t queueTail = 0;

static uint8_t tcaReadReg(uint8_t reg)
{
    Wire1.beginTransmission(TCA8418_DEFAULT_ADDR);
    Wire1.write(reg);
    Wire1.endTransmission(false);
    Wire1.requestFrom((uint8_t)TCA8418_DEFAULT_ADDR, (uint8_t)1);
    return Wire1.read();
}

static void tcaWriteReg(uint8_t reg, uint8_t val)
{
    Wire1.beginTransmission(TCA8418_DEFAULT_ADDR);
    Wire1.write(reg);
    Wire1.write(val);
    Wire1.endTransmission();
}

static void enqueueEvent(uint8_t row, uint8_t col, bool pressed)
{
    uint8_t next = (queueHead + 1) % KEY_QUEUE_SIZE;
    if (next != queueTail)
    {
        keyQueue[queueHead] = {row, col, pressed};
        queueHead = next;
    }
}

// Re-establish a known TCA8418 state: clear interrupt flags, drain FIFO,
// re-apply matrix scan config, and generate synthetic release events for
// any keys that were tracked as pressed.  Called on FIFO overflow or
// whenever the I2C bus recovers from a timeout.
static void tcaSoftRecover()
{
    // Clear all interrupt flags first so the TCA8418 resumes queuing events.
    tcaWriteReg(TCA_INT_STAT, 0xFF);
    // Drain whatever is left in the FIFO (may be garbage after overflow).
    for (int i = 0; i < 16; i++) {
        if (!keyboard.available()) break;
        keyboard.getEvent();
    }
    // Re-apply matrix scan config in case the TCA8418 lost its settings.
    keyboard.matrix(KBD_ROWS, KBD_COLS);
    keyboard.enableDebounce();
    // Synthesise release events for any key that our software thinks is held.
    for (int r = 0; r < KBD_ROWS; r++)
        for (int c = 0; c < KBD_COLS; c++)
            if (keyState[r][c]) {
                keyState[r][c] = false;
                enqueueEvent(r, c, false);
            }
    memset(keyPressTime, 0, sizeof(keyPressTime));
    Serial.println("[KBD] soft recover done");
}

void setupKeyboard()
{
    LOG("2ND HW I2C INIT");
    LOG(Wire1.begin(KBDSDA, KBDSCL, 400000));
    Wire1.setTimeout(3);  // 3ms I2C timeout to prevent bus lockup from hanging the firmware
    delay(500);

    if (!keyboard.begin(TCA8418_DEFAULT_ADDR, &Wire1))
    {
        Serial.println("Keyboard begin() FAILED");
        while (1) delay(100);
    }
    Serial.println("Keyboard init OK");

    if (keyboard.matrix(KBD_ROWS, KBD_COLS))
        LOG("Keyboard matrix config OK");
    keyboard.enableDebounce();  // explicit reset — TCA8418 retains register state across firmware flashes

    uint8_t dir1 = tcaReadReg(TCA_GPIO_DIR1);
    dir1 &= ~0xE0;
    tcaWriteReg(TCA_GPIO_DIR1, dir1);
    uint8_t pull1 = tcaReadReg(TCA_GPIO_PULL1);
    pull1 |= 0xE0;
    tcaWriteReg(TCA_GPIO_PULL1, pull1);

    uint8_t dir3 = tcaReadReg(TCA_GPIO_DIR3);
    dir3 &= ~0x03;
    tcaWriteReg(TCA_GPIO_DIR3, dir3);
    uint8_t pull3 = tcaReadReg(TCA_GPIO_PULL3);
    pull3 |= 0x03;
    tcaWriteReg(TCA_GPIO_PULL3, pull3);

    memset(keyState, 0, sizeof(keyState));
    memset(keyPressTime, 0, sizeof(keyPressTime));
    queueHead = 0;
    queueTail = 0;

    while (keyboard.available() > 0)
        keyboard.getEvent();
    tcaWriteReg(TCA_INT_STAT, 0xFF);
}

uint8_t readTcaGpios()
{
    uint8_t dat1 = tcaReadReg(TCA_GPIO_DAT1);
    uint8_t dat3 = tcaReadReg(TCA_GPIO_DAT3);
    return ((dat1 >> 5) & 0x07) | ((dat3 & 0x03) << 3);
}

bool getNextKeyEvent(uint8_t &row, uint8_t &col, bool &pressed)
{
    if (queueHead == queueTail)
        return false;
    row = keyQueue[queueTail].row;
    col = keyQueue[queueTail].col;
    pressed = keyQueue[queueTail].pressed;
    queueTail = (queueTail + 1) % KEY_QUEUE_SIZE;
    return true;
}

void recoverKeyboard()
{
    tcaSoftRecover();
}

void pollKeyboard()
{
    uint8_t intStat = tcaReadReg(TCA_INT_STAT);
    if (intStat & TCA_OVR_FLOW_BIT)
    {
        lastOverflowTime = millis();
        tcaSoftRecover();  // clear flags, drain FIFO, re-init scanner, release stuck keys
        return;            // skip normal event processing this tick
    }

    int maxEvents = 20;
    while (keyboard.available() > 0 && maxEvents-- > 0)
    {
        int k = keyboard.getEvent();
        bool press = k & 0x80;
        k = (k & 0x7F) - 1;
        int row = k / 10;
        int col = k % 10;
        if (row >= 0 && row < KBD_ROWS && col >= 0 && col < KBD_COLS)
        {
            keyState[row][col] = press;
            if (press)
                keyPressTime[row][col] = millis();
            enqueueEvent(row, col, press);
        }
    }

    uint32_t now = millis();
    bool recentOverflow = (now - lastOverflowTime) < FAST_TIMEOUT_WINDOW;
    uint32_t timeout = recentOverflow ? KEY_TIMEOUT_FAST_MS : KEY_TIMEOUT_MS;

    for (int r = 0; r < KBD_ROWS; r++)
        for (int c = 0; c < KBD_COLS; c++)
            if (keyState[r][c] && (now - keyPressTime[r][c] > timeout))
            {
                keyState[r][c] = false;
                enqueueEvent(r, c, false);
            }
}
