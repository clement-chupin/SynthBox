#pragma once
// FastLED stub for GrvEP simulator — LED colors stored in CRGB array
// FastLED.show() is intercepted by sim_leds to render to SDL

#include <stdint.h>
#include <string.h>

// HSV color (hue 0-255, sat 0-255, val 0-255)
struct CHSV {
    uint8_t h, s, v;
    CHSV() : h(0), s(0), v(0) {}
    CHSV(uint8_t h, uint8_t s, uint8_t v) : h(h), s(s), v(v) {}
};

struct CRGB {
    uint8_t r, g, b;
    CRGB() : r(0), g(0), b(0) {}
    CRGB(uint8_t r, uint8_t g, uint8_t b) : r(r), g(g), b(b) {}
    CRGB(uint32_t colorcode)
        : r((colorcode >> 16) & 0xFF)
        , g((colorcode >>  8) & 0xFF)
        , b( colorcode        & 0xFF) {}
    CRGB& operator=(uint32_t colorcode) {
        r = (colorcode >> 16) & 0xFF;
        g = (colorcode >>  8) & 0xFF;
        b =  colorcode        & 0xFF;
        return *this;
    }
    // Construct from CHSV
    CRGB(const CHSV& hsv) {
        uint8_t h = hsv.h, s = hsv.s, v = hsv.v;
        if (s == 0) { r = g = b = v; return; }
        uint8_t region = h / 43;
        uint8_t rem    = (h - region * 43) * 6;
        uint8_t p = (v * (255 - s)) >> 8;
        uint8_t q = (v * (255 - ((s * rem) >> 8))) >> 8;
        uint8_t t = (v * (255 - ((s * (255 - rem)) >> 8))) >> 8;
        switch (region) {
            case 0: r=v; g=t; b=p; break;
            case 1: r=q; g=v; b=p; break;
            case 2: r=p; g=v; b=t; break;
            case 3: r=p; g=q; b=v; break;
            case 4: r=t; g=p; b=v; break;
            default:r=v; g=p; b=q; break;
        }
    }
    CRGB& operator=(const CHSV& hsv) { *this = CRGB(hsv); return *this; }

    bool operator==(const CRGB& o) const { return r==o.r && g==o.g && b==o.b; }
    bool operator!=(const CRGB& o) const { return !(*this == o); }

    // Scale brightness
    CRGB& nscale8(uint8_t scale) {
        r = (r * scale) >> 8;
        g = (g * scale) >> 8;
        b = (b * scale) >> 8;
        return *this;
    }

    static const CRGB Black;
    static const CRGB Red;
    static const CRGB Green;
    static const CRGB Blue;
    static const CRGB White;
};

// forward declaration — implementation in sim_leds.cpp
void simLedsShow(const CRGB* leds, int count);

// FastLED controller stub
#define SK6812 0
#define BGR    0

class CFastLED {
    const CRGB* _leds = nullptr;
    int _count = 0;
public:
    template<int TYPE, int PIN, int ORDER>
    void addLeds(CRGB* leds, int count) {
        _leds = leds;
        _count = count;
    }

    void setBrightness(uint8_t) {}
    void setMaxPowerInVoltsAndMilliamps(uint8_t, uint16_t) {}
    void show() {
        if (_leds && _count > 0)
            simLedsShow(_leds, _count);
    }
    void clear(bool write = false) {
        if (_leds) for (int i=0;i<_count;i++) const_cast<CRGB*>(_leds)[i] = CRGB(0,0,0);
        if (write) show();
    }
};

extern CFastLED FastLED;
