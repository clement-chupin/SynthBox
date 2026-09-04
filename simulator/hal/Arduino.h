#pragma once
// Arduino HAL stub for the GrvEP simulator (Linux/SDL2)

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <unistd.h>
#include <time.h>
#include <cstdarg>
#include <string>
#include <functional>

// ---- Simulator joystick/analog hook (set by sim_window) ----
extern float g_simJoyX;   // -1.0 .. 1.0
extern float g_simJoyY;   // -1.0 .. 1.0
extern bool  g_simJoySW;  // true = pressed
extern float g_simSlider[16]; // 0.0 .. 1.0

// ---- ESP32 PSRAM attribute (no-op on simulator) ----
#ifndef EXT_RAM_ATTR
#define EXT_RAM_ATTR
#endif

// ---- Types ----
typedef uint8_t  byte;
typedef bool     boolean;
typedef uint16_t word;
typedef uint32_t dword;

// ---- GPIO pin constants (used as integers) ----
#define GPIO_NUM_0   0
#define GPIO_NUM_1   1
#define GPIO_NUM_2   2
#define GPIO_NUM_4   4
#define GPIO_NUM_5   5
#define GPIO_NUM_6   6
#define GPIO_NUM_7   7
#define GPIO_NUM_8   8
#define GPIO_NUM_9   9
#define GPIO_NUM_10  10
#define GPIO_NUM_12  12
#define GPIO_NUM_13  13
#define GPIO_NUM_14  14
#define GPIO_NUM_15  15
#define GPIO_NUM_16  16
#define GPIO_NUM_18  18
#define GPIO_NUM_33  33
#define GPIO_NUM_34  34
#define GPIO_NUM_35  35
#define GPIO_NUM_36  36
#define GPIO_NUM_37  37
#define GPIO_NUM_38  38
#define GPIO_NUM_39  39
#define GPIO_NUM_40  40
#define GPIO_NUM_41  41
#define GPIO_NUM_42  42

#define INPUT  0
#define OUTPUT 1
#define INPUT_PULLUP 2
#define HIGH 1
#define LOW  0

#define PROGMEM
#define F(x) (x)
#define PSTR(x) (x)

#define PI 3.14159265358979323846f
#define TWO_PI (2.0f * PI)
#define HALF_PI (PI / 2.0f)
#define DEG_TO_RAD (PI / 180.0f)
#define RAD_TO_DEG (180.0f / PI)

// ---- Time ----
static inline uint32_t millis() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}
static inline uint32_t micros() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000000 + ts.tv_nsec / 1000);
}
static inline void delay(uint32_t ms)     { usleep((useconds_t)ms * 1000); }
static inline void delayMicroseconds(uint32_t us) { usleep((useconds_t)us); }
static inline void yield() {}

// ---- Math helpers ----
template<typename T> static inline T min(T a, T b) { return a < b ? a : b; }
template<typename T> static inline T max(T a, T b) { return a > b ? a : b; }
template<typename T> static inline T abs(T v)       { return v < 0 ? -v : v; }
template<typename T> static inline T sq(T v)        { return v * v; }
template<typename T, typename L, typename H> static inline T constrain(T x, L lo, H hi) {
    return x < (T)lo ? (T)lo : (x > (T)hi ? (T)hi : x);
}
template<typename T, typename F, typename TL, typename TH, typename FL, typename FH>
static inline F map(T x, TL inLo, TH inHi, FL outLo, FH outHi) {
    return (F)(outLo + ((long long)(x - inLo) * (outHi - outLo) / (inHi - inLo)));
}
static inline long map(long x, long il, long ih, long ol, long oh) {
    return ol + (x - il) * (oh - ol) / (ih - il);
}
static inline float mapf(float x, float il, float ih, float ol, float oh) {
    return ol + (x - il) * (oh - ol) / (ih - il);
}

// ---- GPIO ----
static inline void pinMode(uint8_t, uint8_t) {}
static inline void digitalWrite(uint8_t, uint8_t) {}
static inline int  digitalRead(uint8_t pin) {
    extern bool g_simJoySW;
    // JOYSW = GPIO 40, PWR_SENSE = GPIO 12
    if (pin == 40) return g_simJoySW ? LOW : HIGH; // active low
    if (pin == 12) return HIGH; // power always good
    return HIGH;
}

// ---- Analog read ----
// Returns 0-4095 (12-bit ADC)
static inline uint16_t analogRead(uint8_t pin) {
    extern float g_simJoyX;
    extern float g_simJoyY;
    // JOYX=GPIO2, JOYY=GPIO1
    if (pin == 2) return (uint16_t)((g_simJoyX * 0.5f + 0.5f) * 4095.0f);
    if (pin == 1) return (uint16_t)((-g_simJoyY * 0.5f + 0.5f) * 4095.0f); // Y inverted on hardware
    return 2048; // midpoint for anything else
}

static inline void analogReadResolution(uint8_t) {}
static inline void analogSetAttenuation(int) {}

// ESP32 temperature sensor stub
static inline float temperatureRead() { return 25.0f; }

// ---- String class (minimal) ----
class String : public std::string {
public:
    String() = default;
    String(const char* s) : std::string(s ? s : "") {}
    String(const std::string& s) : std::string(s) {}
    String(int v)         : std::string(std::to_string(v)) {}
    String(unsigned int v): std::string(std::to_string(v)) {}
    String(long v)        : std::string(std::to_string(v)) {}
    String(unsigned long v): std::string(std::to_string(v)) {}
    String(float v, int decimals = 2) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.*f", decimals, (double)v);
        assign(buf);
    }
    String(double v, int decimals = 2) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.*f", decimals, v);
        assign(buf);
    }
    String(char c) : std::string(1, c) {}

    const char* c_str() const { return std::string::c_str(); }
    int   indexOf(char c, int from=0) const {
        size_t p = find(c, from);
        return p == std::string::npos ? -1 : (int)p;
    }
    int   indexOf(const char* s, int from=0) const {
        size_t p = find(s, from);
        return p == std::string::npos ? -1 : (int)p;
    }
    String substring(int start, int end) const {
        return String(substr(start, end-start).c_str());
    }
    String substring(int start) const {
        return String(substr(start).c_str());
    }
    int    toInt() const   { return atoi(c_str()); }
    float  toFloat() const { return (float)atof(c_str()); }
    bool   isEmpty() const { return empty(); }
    bool   startsWith(const char* s) const { return find(s) == 0; }
    int    lastIndexOf(char c, int from = -1) const {
        size_t start = (from < 0 || (size_t)from >= size()) ? size() : (size_t)(from + 1);
        size_t p = rfind(c, start - 1);
        return p == std::string::npos ? -1 : (int)p;
    }
    int    lastIndexOf(const char* s, int from = -1) const {
        size_t start = (from < 0 || (size_t)from >= size()) ? size() : (size_t)(from + 1);
        size_t p = rfind(s, start - 1);
        return p == std::string::npos ? -1 : (int)p;
    }
    int    lastIndexOf(const String& s, int from = -1) const {
        return lastIndexOf(s.c_str(), from);
    }
    bool   endsWith(const char* s) const {
        size_t sl = strlen(s);
        return size() >= sl && substr(size()-sl) == s;
    }
    String toLowerCase() const {
        std::string r = *this;
        for (auto& c : r) c = tolower(c);
        return String(r.c_str());
    }
    String toUpperCase() const {
        std::string r = *this;
        for (auto& c : r) c = toupper(c);
        return String(r.c_str());
    }
    void   trim() {
        auto b = find_first_not_of(" \t\r\n");
        if (b == std::string::npos) { clear(); return; }
        auto e = find_last_not_of(" \t\r\n");
        *this = substr(b, e - b + 1);
    }
    int    length() const { return (int)size(); }
    char   charAt(int i) const { return (*this)[i]; }
    void   remove(int idx, int count=1) { erase(idx, count); }
    void   concat(const char* s) { append(s); }
    void   concat(const String& s) { append(s); }
    String& operator+=(const char* s)  { append(s); return *this; }
    String& operator+=(const String& s){ append(s); return *this; }
    String& operator+=(char c)         { push_back(c); return *this; }
    String& operator+=(int v)          { append(std::to_string(v)); return *this; }
    String  operator+(const String& o) const { return String((std::string(*this) + std::string(o)).c_str()); }
    String  operator+(const char* o)   const { return String((std::string(*this) + o).c_str()); }
    bool    operator==(const char* s)  const { return compare(s) == 0; }
    bool    operator!=(const char* s)  const { return compare(s) != 0; }
    bool    operator==(const String& s) const { return compare(s) == 0; }
};

// ---- Serial ----
class HardwareSerial {
public:
    void begin(long) {}
    void print(const char* s)   { fputs(s, stdout); }
    void print(const String& s) { fputs(s.c_str(), stdout); }
    void print(int v)           { printf("%d", v); }
    void print(unsigned int v)  { printf("%u", v); }
    void print(long v)          { printf("%ld", v); }
    void print(unsigned long v) { printf("%lu", v); }
    void print(float v)         { printf("%g", v); }
    void print(double v)        { printf("%g", v); }
    void print(char c)          { putchar(c); }
    void println(const char* s) { printf("%s\n", s); }
    void println(const String& s){ printf("%s\n", s.c_str()); }
    void println(int v)         { printf("%d\n", v); }
    void println(unsigned int v){ printf("%u\n", v); }
    void println(long v)        { printf("%ld\n", v); }
    void println(unsigned long v){printf("%lu\n", v); }
    void println(float v)       { printf("%g\n", v); }
    void println(double v)      { printf("%g\n", v); }
    void println(char c)        { printf("%c\n", c); }
    void println()              { putchar('\n'); }
    void printf(const char* fmt, ...) {
        va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    }
    void flush() { fflush(stdout); }
    operator bool() const { return true; }
};
extern HardwareSerial Serial;

// ---- ESP class stub ----
class EspClass {
public:
    uint32_t getCpuFreqMHz() const { return 240; }
    uint32_t getPsramSize()  const { return 8 * 1024 * 1024; }
    uint32_t getFreeHeap()   const { return 4 * 1024 * 1024; }
    const char* getChipModel() const { return "ESP32-S3 (SIM)"; }
};
extern EspClass ESP;

// ---- heap_caps stub ----
#define MALLOC_CAP_SPIRAM   (1 << 3)
#define MALLOC_CAP_INTERNAL (1 << 0)
#define MALLOC_CAP_8BIT     (1 << 2)
static inline size_t heap_caps_get_free_size(uint32_t) { return 512u * 1024u * 1024u; }
static inline size_t heap_caps_get_largest_free_block(uint32_t) { return 512u * 1024u * 1024u; }
static inline void*  heap_caps_malloc(size_t sz, uint32_t) { return malloc(sz); }
static inline void   heap_caps_free(void* p) { free(p); }

// ---- esp_get_free_heap stubs ----
static inline uint32_t esp_get_free_heap_size()         { return 4 * 1024 * 1024; }
static inline uint32_t esp_get_minimum_free_heap_size() { return 2 * 1024 * 1024; }

// ---- Math ----
static inline float degrees(float r) { return r * RAD_TO_DEG; }
static inline float radians(float d) { return d * DEG_TO_RAD; }
static inline float sq2(float x) { return x * x; }
static inline long random(long lo, long hi) { return lo + rand() % (hi - lo); }
static inline long random(long hi) { return rand() % hi; }

// ---- PSRAM ----
// ps_malloc = PSRAM malloc → just use system malloc on Linux
static inline void* ps_malloc(size_t sz) { return malloc(sz); }
static inline size_t heap_caps_get_total_size(uint32_t) { return 8 * 1024 * 1024; }
