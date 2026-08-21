#include "keyboard.h"
#include "HWConfig.h"
#include "logger.h"

Adafruit_TCA8418 keyboard;
bool keyState[KBD_ROWS][KBD_COLS] = {};

#define TCA_INT_STAT     0x02
#define TCA_OVR_FLOW_BIT 0x08
#define TCA_CFG_REG      0x01
#define TCA_OVF_MODE     0x04  // CFG bit 2 (OVF_M): FIFO wrap — newest overwrites oldest
// IMPORTANT: 0x20 = CFG bit 5 = K_LCK_EN (Key Lock) — never set, causes keyboard lockouts
#define TCA_GPIO_DAT1    0x14
#define TCA_GPIO_DAT3    0x16
#define TCA_GPIO_DIR1    0x23
#define TCA_GPIO_DIR3    0x25
#define TCA_GPIO_PULL1   0x1A
#define TCA_GPIO_PULL3   0x1C

// 200Hz polling: 5ms period. Hardware debounce disabled (saves 12ms latency).
// SWDEBOUNCE_MS = 0 disables software debounce: at 5ms poll most bounces (< 5ms on
// quality switches) are absorbed within a single poll window by the snFin last-state logic.
// Re-enable with a value > POLL_INTERVAL_MS only if phantom notes accumulate over time.
#define POLL_INTERVAL_MS      2    // 500Hz — 2ms poll + ~4ms TCA scan = ~6ms worst-case
#define SWDEBOUNCE_MS         0    // 0 = disabled
#define KEY_TIMEOUT_MS     30000   // 30s: safety release for physically stuck keys only
#define KEY_TIMEOUT_FAST_MS 3000   // 3s: after FIFO overflow (may have missed a RELEASE event)
#define FAST_TIMEOUT_WINDOW 5000
#define TCA_WATCHDOG_MS     500   // re-verify TCA config twice per second
#define I2C_FAIL_THRESHOLD    5   // consecutive read failures → bus recovery

static uint32_t keyPressTime[KBD_ROWS][KBD_COLS]      = {};
static uint32_t keyLastChangeTime[KBD_ROWS][KBD_COLS]  = {};

// Monitoring: poll interval statistics, reset each second by kbdGetStats().
static volatile uint32_t s_kbdPollCount   = 0;
static volatile uint32_t s_kbdMaxInterval = 0;
static volatile uint32_t s_kbdLastPollMs  = 0;
static NoteKeyCallback s_noteKeyCb = nullptr;
static bool     overflowOccurred = false;  // false until first real overflow (avoids millis()=0 false trigger)
static uint32_t lastOverflowTime = 0;
static uint32_t lastTcaWatchdog  = 0;
static uint8_t  i2cFailCount     = 0;

static KeyEvent keyQueue[KEY_QUEUE_SIZE];
static volatile uint8_t queueHead = 0;
static volatile uint8_t queueTail = 0;

static uint8_t tcaReadReg(uint8_t reg)
{
    Wire1.beginTransmission(TCA8418_DEFAULT_ADDR);
    Wire1.write(reg);
    Wire1.endTransmission(false);
    if (Wire1.requestFrom((uint8_t)TCA8418_DEFAULT_ADDR, (uint8_t)1) == 0) {
        if (i2cFailCount < 255) i2cFailCount++;
        return 0;
    }
    i2cFailCount = 0;
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
    if (next != queueTail) {
        keyQueue[queueHead] = {row, col, pressed};
        queueHead = next;
    }
}

static void applyTcaConfig()
{
    keyboard.matrix(KBD_ROWS, KBD_COLS);
    // Hardware debounce: 4 stable scan cycles × 4ms = 16ms.
    // Events arrive clean — no software debounce needed.
    keyboard.enableDebounce();
    // Write CFG directly (never read-modify-write) to guarantee K_LCK_EN=0.
    tcaWriteReg(TCA_CFG_REG, TCA_OVF_MODE);
}

static void releaseAllKeys()
{
    for (int r = 0; r < KBD_ROWS; r++)
        for (int c = 0; c < KBD_COLS; c++)
            if (keyState[r][c]) {
                keyState[r][c] = false;
                enqueueEvent(r, c, false);
            }
    memset(keyPressTime,      0, sizeof(keyPressTime));
    memset(keyLastChangeTime, 0, sizeof(keyLastChangeTime));
}

// I2C bus recovery: bit-bang 9 SCL pulses to release a slave holding SDA low,
// then send a STOP, reinit Wire1, and reinit the TCA8418.
static void recoverI2CBus()
{
    Serial.println("[KBD] I2C recovery");
    Wire1.end();

    pinMode(KBDSCL, OUTPUT);
    pinMode(KBDSDA, INPUT_PULLUP);
    for (int i = 0; i < 9; i++) {
        digitalWrite(KBDSCL, HIGH); delayMicroseconds(10);
        digitalWrite(KBDSCL, LOW);  delayMicroseconds(10);
        if (digitalRead(KBDSDA)) break;  // SDA released: bus free
    }
    // STOP condition
    pinMode(KBDSDA, OUTPUT);
    digitalWrite(KBDSDA, LOW);  delayMicroseconds(10);
    digitalWrite(KBDSCL, HIGH); delayMicroseconds(10);
    digitalWrite(KBDSDA, HIGH); delayMicroseconds(10);

    vTaskDelay(pdMS_TO_TICKS(20));
    Wire1.begin(KBDSDA, KBDSCL, 400000);
    Wire1.setTimeout(3);
    vTaskDelay(pdMS_TO_TICKS(10));

    keyboard.begin(TCA8418_DEFAULT_ADDR, &Wire1);
    applyTcaConfig();

    // Drain stale FIFO events and release all logically-held keys
    int avail = keyboard.available();
    if (avail > 10) avail = 10;
    for (int e = 0; e < avail; e++) keyboard.getEvent();
    tcaWriteReg(TCA_INT_STAT, 0xFF);

    releaseAllKeys();
    i2cFailCount    = 0;
    overflowOccurred = false;
    Serial.println("[KBD] I2C recovery done");
}

static void kbdPollTask(void* arg)
{
    TickType_t lastWake = xTaskGetTickCount();
    for (;;) {
        pollKeyboard();
        // vTaskDelayUntil: absolute wakeup — stays at exactly 50Hz regardless
        // of how long pollKeyboard() took (I2C timeout, recovery, etc.).
        vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(POLL_INTERVAL_MS));
    }
}

void setupKeyboard()
{
    LOG("2ND HW I2C INIT");
    LOG(Wire1.begin(KBDSDA, KBDSCL, 400000));
    Wire1.setTimeout(3);
    delay(200);  // I2C pull-ups and TCA power stabilisation (~10ms needed, 200ms safe)

    bool kbdOk = false;
    for (int attempt = 0; attempt < 5; attempt++) {
        if (keyboard.begin(TCA8418_DEFAULT_ADDR, &Wire1)) { kbdOk = true; break; }
        Serial.printf("Keyboard begin() attempt %d failed, retrying...\n", attempt + 1);
        delay(200);
    }
    if (!kbdOk) {
        Serial.println("Keyboard begin() FAILED after retries");
        while (1) delay(100);
    }
    Serial.println("Keyboard init OK");

    applyTcaConfig();

    // Configure extra GPIO-mode pins (non-key inputs)
    uint8_t dir1  = tcaReadReg(TCA_GPIO_DIR1);  dir1  &= ~0xE0; tcaWriteReg(TCA_GPIO_DIR1,  dir1);
    uint8_t pull1 = tcaReadReg(TCA_GPIO_PULL1); pull1 |=  0xE0; tcaWriteReg(TCA_GPIO_PULL1, pull1);
    uint8_t dir3  = tcaReadReg(TCA_GPIO_DIR3);  dir3  &= ~0x03; tcaWriteReg(TCA_GPIO_DIR3,  dir3);
    uint8_t pull3 = tcaReadReg(TCA_GPIO_PULL3); pull3 |=  0x03; tcaWriteReg(TCA_GPIO_PULL3, pull3);

    memset(keyState, 0, sizeof(keyState));
    memset(keyPressTime, 0, sizeof(keyPressTime));
    queueHead = 0; queueTail = 0;

    // Drain any events queued before init
    int avail = keyboard.available();
    if (avail > 10) avail = 10;
    for (int e = 0; e < avail; e++) keyboard.getEvent();
    tcaWriteReg(TCA_INT_STAT, 0xFF);

    // Core 0: AMY render sleeps between blocks → kbdPollTask gets regular gaps.
    // Core 1: AMY fill buffer runs nearly continuously at priority 23, starving priority-2 tasks.
    xTaskCreatePinnedToCore(kbdPollTask, "kbd", 3072, nullptr, 8, nullptr, 0);
}

uint8_t readTcaGpios()
{
    uint8_t dat1 = tcaReadReg(TCA_GPIO_DAT1);
    uint8_t dat3 = tcaReadReg(TCA_GPIO_DAT3);
    return ((dat1 >> 5) & 0x07) | ((dat3 & 0x03) << 3);
}

bool getNextKeyEvent(uint8_t &row, uint8_t &col, bool &pressed)
{
    if (queueHead == queueTail) return false;
    row     = keyQueue[queueTail].row;
    col     = keyQueue[queueTail].col;
    pressed = keyQueue[queueTail].pressed;
    queueTail = (queueTail + 1) % KEY_QUEUE_SIZE;
    return true;
}

void recoverKeyboard()
{
    recoverI2CBus();
}

void pollKeyboard()
{
    uint32_t now = millis();

    // Track poll interval for external monitoring.
    if (s_kbdLastPollMs) {
        uint32_t iv = now - s_kbdLastPollMs;
        if (iv > s_kbdMaxInterval) s_kbdMaxInterval = iv;
    }
    s_kbdLastPollMs = now;
    s_kbdPollCount++;

    // Escalate to full bus recovery if I2C is consistently failing
    if (i2cFailCount >= I2C_FAIL_THRESHOLD) {
        recoverI2CBus();
        return;
    }

    // Watchdog: TCA8418 can lose config after a power glitch or I2C error.
    // OVF_M (bit 2) is our canary — re-apply full config if it's gone.
    if (now - lastTcaWatchdog >= TCA_WATCHDOG_MS) {
        lastTcaWatchdog = now;
        if ((tcaReadReg(TCA_CFG_REG) & TCA_OVF_MODE) == 0)
            applyTcaConfig();
    }

    // FIFO overflow flag
    uint8_t intStat = tcaReadReg(TCA_INT_STAT);
    if (intStat & TCA_OVR_FLOW_BIT) {
        overflowOccurred = true;
        lastOverflowTime = now;
        tcaWriteReg(TCA_INT_STAT, TCA_OVR_FLOW_BIT);
    }

    // Drain FIFO: keep only the last event direction per key this poll window.
    // Hardware debounce guarantees each event is already stable for ≥16ms,
    // so no additional software filtering is required.
    bool snSeen[KBD_ROWS][KBD_COLS];
    bool snFin[KBD_ROWS][KBD_COLS];
    memset(snSeen, 0, sizeof(snSeen));
    bool anySeen = false;

    int avail = keyboard.available();
    if (avail > 10) avail = 10;
    for (int e = 0; e < avail; e++) {
        int k = keyboard.getEvent();
        bool press = (k & 0x80) != 0;
        k = (k & 0x7F) - 1;
        int row = k / 10, col = k % 10;
        if (row >= 0 && row < KBD_ROWS && col >= 0 && col < KBD_COLS) {
            snSeen[row][col] = true;
            snFin[row][col]  = press;
            anySeen = true;
        }
    }

    if (anySeen) {
        for (int r = 0; r < KBD_ROWS; r++) {
            for (int c = 0; c < KBD_COLS; c++) {
                if (!snSeen[r][c]) continue;
#if SWDEBOUNCE_MS > 0
                if (now - keyLastChangeTime[r][c] < SWDEBOUNCE_MS) continue;
#endif
                if (snFin[r][c] && !keyState[r][c]) {
                    keyState[r][c]          = true;
                    keyPressTime[r][c]      = now;
                    keyLastChangeTime[r][c]  = now;
                    enqueueEvent(r, c, true);
                    if (s_noteKeyCb && (uint8_t)r < KBD_NOTE_ROWS)
                        s_noteKeyCb((uint8_t)r, (uint8_t)c, true);
                } else if (!snFin[r][c] && keyState[r][c]) {
                    keyState[r][c]          = false;
                    keyLastChangeTime[r][c]  = now;
                    enqueueEvent(r, c, false);
                    if (s_noteKeyCb && (uint8_t)r < KBD_NOTE_ROWS)
                        s_noteKeyCb((uint8_t)r, (uint8_t)c, false);
                }
            }
        }
    }

    // Safety timeout: auto-release any key stuck beyond threshold.
    // recentOverflow only active after a real overflow has been detected — avoids
    // the millis()=0 false trigger that was releasing keys after ~500ms at startup.
    bool recentOverflow = overflowOccurred && (now - lastOverflowTime) < FAST_TIMEOUT_WINDOW;
    uint32_t timeout = recentOverflow ? KEY_TIMEOUT_FAST_MS : KEY_TIMEOUT_MS;
    for (int r = 0; r < KBD_ROWS; r++)
        for (int c = 0; c < KBD_COLS; c++)
            if (keyState[r][c] && (now - keyPressTime[r][c] > timeout)) {
                keyState[r][c] = false;
                enqueueEvent(r, c, false);
            }
}

// Returns accumulated poll stats since the last call, then resets them.
void kbdGetStats(uint32_t& pollCount, uint32_t& maxIntervalMs)
{
    pollCount     = s_kbdPollCount;
    maxIntervalMs = s_kbdMaxInterval;
    s_kbdPollCount   = 0;
    s_kbdMaxInterval = 0;
}

void setNoteKeyCallback(NoteKeyCallback cb) { s_noteKeyCb = cb; }
