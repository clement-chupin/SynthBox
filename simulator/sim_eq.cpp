// sim_eq.cpp — real-time EQ-curve window (desktop simulator only)
//
// Companion to sim_spectrogram.cpp's scrolling waterfall: instead of a history view,
// this window shows a single live curve of the current frequency content, plotted
// left (bass) to right (treble) — the classic real-time EQ/analyzer look, and more
// direct than the waterfall for "does turning this knob visibly move the curve"
// tuning-by-eye.
//
// Taps the same post-FX output_ring as sim_spectrogram.cpp (see that file's header
// comment for why that tap point and the per-sample circular buffer are the right
// approach) but keeps its own independent copy of the ring/FFT plumbing rather than
// sharing state with it — either window can be added, removed, or changed without
// touching the other.

#ifndef __ANDROID__

#include <SDL2/SDL.h>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cmath>

extern "C" {
    extern int16_t  output_ring[];
    extern uint16_t ring_write_ptr;
}
#define TAP_RING_LEN      2048
#define AUDIO_SAMPLE_RATE 44100.0f

#define FFT_N    1024
#define FFT_BINS (FFT_N / 2)

static float s_fftRe[FFT_N];
static float s_fftIm[FFT_N];
static float s_window[FFT_N];   // Hann window, precomputed once

#define MY_RING_LEN (FFT_N * 4)
static float    s_myRing[MY_RING_LEN];
static uint32_t s_myWritePos     = 0;
static uint32_t s_myTotalWritten = 0;
static uint16_t s_lastReadPos    = 0;

static void fftInitWindow() {
    for (int i = 0; i < FFT_N; i++)
        s_window[i] = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * i / (FFT_N - 1));
}

// In-place iterative radix-2 Cooley-Tukey FFT. n must be a power of 2.
static void fft(float* re, float* im, int n) {
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        float ang = -2.0f * (float)M_PI / (float)len;
        float wRe = cosf(ang), wIm = sinf(ang);
        for (int i = 0; i < n; i += len) {
            float curRe = 1.0f, curIm = 0.0f;
            for (int k = 0; k < len / 2; k++) {
                float uRe = re[i + k],           uIm = im[i + k];
                float vRe = re[i + k + len/2] * curRe - im[i + k + len/2] * curIm;
                float vIm = re[i + k + len/2] * curIm + im[i + k + len/2] * curRe;
                re[i + k]         = uRe + vRe; im[i + k]         = uIm + vIm;
                re[i + k + len/2] = uRe - vRe; im[i + k + len/2] = uIm - vIm;
                float nextRe = curRe * wRe - curIm * wIm;
                float nextIm = curRe * wIm + curIm * wRe;
                curRe = nextRe; curIm = nextIm;
            }
        }
    }
}

// ---- Window / rendering ----
#define EQ_W 480
#define EQ_H 220
#define EQ_MARGIN_L 6
#define EQ_MARGIN_R 6
#define EQ_MARGIN_T 10
#define EQ_MARGIN_B 10
#define EQ_PLOT_W (EQ_W - EQ_MARGIN_L - EQ_MARGIN_R)
#define EQ_PLOT_H (EQ_H - EQ_MARGIN_T - EQ_MARGIN_B)

// Log-scale frequency range shown left-to-right — musically useful range, and log
// spacing gives the bass end (where most FX shaping matters) real width instead of
// being crushed into a handful of pixels the way a linear axis would.
#define FREQ_MIN 30.0f
#define FREQ_MAX 16000.0f

static SDL_Window*   s_win  = nullptr;
static SDL_Renderer* s_rend = nullptr;

// One smoothed dB level per plot column (left=bass..right=treble), persisted across
// frames for the attack/decay smoothing in simEqRender().
static float s_curveDb[EQ_PLOT_W];

// A pure per-octave log scale (freq = FREQ_MIN * ratio^t) gives every octave the same
// pixel width — technically even, but most instruments concentrate their energy in
// the bottom handful of octaves, so that flat allocation still reads as "the bass
// hump dominates the picture" (30-400Hz alone is ~2 of the 9 octaves shown, yet
// visually the busiest region). BASS_BIAS warps t before the log lookup so the bass
// end covers its octaves in fewer x-pixels (compacted) and the treble end spreads its
// octaves over more (expanded) — i.e. more than log, matching the practical goal of
// this display (seeing filter/FX shaping detail) better than a flat per-octave split.
//
// t^(1/BASS_BIAS) (BASS_BIAS>1) rises steeply for small t: e.g. at t=0.25 it's already
// partway to 1, so a small move in x reaches a much higher exponent — i.e. sails
// through many low octaves — while it flattens out near t=1, so the same size move
// near the treble end covers far fewer octaves (expanded/more detail). Getting the
// exponent's placement backwards (t^BASS_BIAS instead of t^(1/BASS_BIAS)) does the
// opposite — expands bass further, compacts treble — which is what shipped earlier.
// tToFreq/freqToT must stay exact inverses of each other (freqToT feeds gridlines).
#define BASS_BIAS 2.0f
static float tToFreq(float t) {
    return FREQ_MIN * powf(FREQ_MAX / FREQ_MIN, powf(t, 1.0f / BASS_BIAS));
}
static float freqToT(float hz) {
    float lin = logf(hz / FREQ_MIN) / logf(FREQ_MAX / FREQ_MIN);
    if (lin < 0.0f) lin = 0.0f;
    return powf(lin, BASS_BIAS);
}
static float freqForCol(int x) {
    float t = (float)x / (float)(EQ_PLOT_W - 1);
    return tToFreq(t);
}

// Linearly-interpolated magnitude (dB) at an arbitrary frequency, from the FFT's
// linearly-spaced bins (AUDIO_SAMPLE_RATE/FFT_N Hz apart) — lets the curve be sampled
// at log-spaced x positions without a separate bucketing pass.
// A Hann-windowed FFT's raw bin magnitude for a full-scale sinusoid peaks at roughly
// amplitude * FFT_N/4 (the window's 0.5 coherent-gain factor applied to the usual
// N/2 single-bin DFT gain) — i.e. ~48dB above the "0dB = full scale" the display
// wants, for FFT_N=1024. Without correcting for that, ordinary playing reads as
// pinned to the ceiling across most of the spectrum ("sature trop facilement").
// Dividing by that same factor calibrates 0dB back to an actual full-scale signal.
#define FFT_MAG_NORM (FFT_N * 0.25f)

static float dbAtFreq(float hz) {
    // FREQ_MIN (30Hz) sits below the first non-DC bin (~43Hz at FFT_N=1024/44100Hz),
    // so binF can land under 1.0 here. Clamping binF itself (not just b0) before
    // splitting into b0/frac keeps frac in [0,1) — otherwise a naive b0=max(b0,1)
    // leaves frac negative, and the interpolation below extrapolates backward past
    // bin1 toward bin0 (DC) instead of interpolating, which blew up into a spurious
    // pinned-high reading at the display's leftmost (bass) column.
    float binF = hz / (AUDIO_SAMPLE_RATE / (float)FFT_N);
    if (binF < 1.0f) binF = 1.0f;
    int b0 = (int)binF;
    if (b0 >= FFT_BINS - 1) b0 = FFT_BINS - 2;
    float frac = binF - (float)b0;
    float m0 = sqrtf(s_fftRe[b0]*s_fftRe[b0]         + s_fftIm[b0]*s_fftIm[b0]);
    float m1 = sqrtf(s_fftRe[b0+1]*s_fftRe[b0+1]     + s_fftIm[b0+1]*s_fftIm[b0+1]);
    float mag = (m0 + (m1 - m0) * frac) / FFT_MAG_NORM;
    return 20.0f * log10f(mag + 1e-6f);
}

static int rowForDb(float db) {
    float norm = (db + 70.0f) / 70.0f; // -70dB..0dB -> 0..1
    if (norm < 0.0f) norm = 0.0f;
    if (norm > 1.0f) norm = 1.0f;
    return EQ_MARGIN_T + (int)((1.0f - norm) * (EQ_PLOT_H - 1));
}

bool simEqInit() {
    // Fixed position (not SDL_WINDOWPOS_UNDEFINED): to the right of the spectrogram
    // window, both along the screen's top edge — some window managers resolve
    // UNDEFINED to the same spot for every window, stacking them all on top of each
    // other (spectrogram is 400px wide at x=20, so 440 clears it with a 20px gap).
    s_win = SDL_CreateWindow("GrvEP - EQ",
        440, 20,
        EQ_W, EQ_H, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!s_win) return false;
    s_rend = SDL_CreateRenderer(s_win, -1, SDL_RENDERER_ACCELERATED);
    if (!s_rend) { SDL_DestroyWindow(s_win); s_win = nullptr; return false; }
    SDL_RenderSetLogicalSize(s_rend, EQ_W, EQ_H);
    SDL_SetRenderDrawBlendMode(s_rend, SDL_BLENDMODE_BLEND);
    fftInitWindow();
    s_lastReadPos = ring_write_ptr;
    for (int i = 0; i < EQ_PLOT_W; i++) s_curveDb[i] = -70.0f;
    return true;
}

void simEqRender() {
    if (!s_win) return;

    // Same wrap-aware ring drain as sim_spectrogram.cpp — see its header comment for
    // why a full-ring (not half-ring) overrun threshold is the correct one.
    uint16_t wp = ring_write_ptr;
    int availSamples = (int)wp - (int)s_lastReadPos;
    if (availSamples < 0) availSamples += TAP_RING_LEN;
    if (availSamples > TAP_RING_LEN) availSamples = TAP_RING_LEN;
    int framesAvail = availSamples / 2;

    uint16_t rp = s_lastReadPos;
    for (int i = 0; i < framesAvail; i++) {
        int16_t l = output_ring[rp]; rp = (uint16_t)((rp + 1) % TAP_RING_LEN);
        int16_t r = output_ring[rp]; rp = (uint16_t)((rp + 1) % TAP_RING_LEN);
        s_myRing[s_myWritePos] = ((float)l + (float)r) * 0.5f / 32768.0f;
        s_myWritePos = (s_myWritePos + 1) % MY_RING_LEN;
        s_myTotalWritten++;
    }
    s_lastReadPos = wp;

    if (s_myTotalWritten < (uint32_t)FFT_N) return; // not enough history yet

    for (int i = 0; i < FFT_N; i++) {
        uint32_t idx = (s_myWritePos + MY_RING_LEN - FFT_N + (uint32_t)i) % MY_RING_LEN;
        s_fftRe[i] = s_myRing[idx] * s_window[i];
        s_fftIm[i] = 0.0f;
    }
    fft(s_fftRe, s_fftIm, FFT_N);

    // Fast attack / slow decay per column — the standard peak-follower shape used by
    // every real analyzer/EQ display, so the curve tracks transients immediately but
    // doesn't flicker to the noise floor between ticks on a held note.
    for (int x = 0; x < EQ_PLOT_W; x++) {
        float db = dbAtFreq(freqForCol(x));
        float prev = s_curveDb[x];
        s_curveDb[x] = (db > prev) ? (prev + (db - prev) * 0.6f)
                                    : (prev + (db - prev) * 0.15f);
    }

    SDL_SetRenderDrawColor(s_rend, 10, 12, 18, 255);
    SDL_RenderClear(s_rend);

    // Frequency gridlines (matching the bass-compacting x mapping) + dB gridlines.
    SDL_SetRenderDrawColor(s_rend, 255, 255, 255, 35);
    static const float kGridHz[] = {100.0f, 300.0f, 1000.0f, 3000.0f, 10000.0f};
    for (float hz : kGridHz) {
        float t = freqToT(hz);
        int x = EQ_MARGIN_L + (int)(t * (EQ_PLOT_W - 1));
        SDL_RenderDrawLine(s_rend, x, EQ_MARGIN_T, x, EQ_MARGIN_T + EQ_PLOT_H - 1);
    }
    for (float db = -60.0f; db < 0.0f; db += 20.0f) {
        int y = rowForDb(db);
        SDL_RenderDrawLine(s_rend, EQ_MARGIN_L, y, EQ_MARGIN_L + EQ_PLOT_W - 1, y);
    }

    // Translucent fill down to the plot floor, solid line traced on top — classic
    // spectrum-analyzer look, readable at a glance.
    SDL_SetRenderDrawColor(s_rend, 60, 200, 255, 70);
    for (int x = 0; x < EQ_PLOT_W; x++) {
        int y = rowForDb(s_curveDb[x]);
        SDL_RenderDrawLine(s_rend, EQ_MARGIN_L + x, y, EQ_MARGIN_L + x, EQ_MARGIN_T + EQ_PLOT_H - 1);
    }
    SDL_SetRenderDrawColor(s_rend, 120, 230, 255, 255);
    for (int x = 0; x < EQ_PLOT_W - 1; x++) {
        int y0 = rowForDb(s_curveDb[x]);
        int y1 = rowForDb(s_curveDb[x + 1]);
        SDL_RenderDrawLine(s_rend, EQ_MARGIN_L + x, y0, EQ_MARGIN_L + x + 1, y1);
    }

    SDL_RenderPresent(s_rend);
}

void simEqDestroy() {
    if (s_rend) SDL_DestroyRenderer(s_rend);
    if (s_win)  SDL_DestroyWindow(s_win);
    s_rend = nullptr; s_win = nullptr;
}

#endif
