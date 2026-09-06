// sim_spectrogram.cpp — real-time scrolling spectrogram window (desktop simulator only)
//
// Added specifically to test/tune FX by ear-and-eye together, especially with the
// noise-based synth shapes (SHAPE_NOISE_WHITE/PINK/BROWN): full-spectrum noise makes
// a filter/EQ/effect's frequency response immediately visible as a shape in the
// waterfall, rather than having to guess from a sine/pluck's mostly-empty spectrum.
//
// Taps the audio engine's own post-FX output ring buffer (output_ring / ring_write_ptr,
// declared in AMY's libminiaudio-audio.c backend and written continuously by the
// miniaudio audio callback) — no changes needed to the audio pipeline itself, this
// just reads what's already there, downstream of every FX in the chain. Read here
// from the main/render thread while the audio thread keeps writing, unsynchronized —
// same casual cross-thread convention already used throughout this simulator for
// shared state (g_simSlider etc.); an occasional stale/torn sample is imperceptible
// in a spectrogram display.

#ifndef __ANDROID__

#include <SDL2/SDL.h>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cmath>

// ---- Tap point ----
// AMY_BLOCK_SIZE=128 and AMY_NCHANS=2 are fixed throughout this codebase (both
// visible at boot as "AMY_BLOCK_SIZE=128" and confirmed via i2s.c's stereo config) →
// libminiaudio-audio.c's OUTPUT_RING_FRAMES = AMY_BLOCK_SIZE*8 = 1024,
// OUTPUT_RING_LENGTH (interleaved stereo int16 samples) = 1024*2 = 2048. Not pulling
// these from amy.h here: this simulator target's own HAL header resolves amy.h to a
// sibling repo's copy (other_projects/amy) while actually linking the OTHER AMY
// copy's compiled sources (lib/AMY Synthesizer) — harmless for the handful of plain
// numeric constants this file needs, but not worth adding an include-path dependency
// on either copy for.
extern "C" {
    extern int16_t  output_ring[];
    extern uint16_t ring_write_ptr;
}
#define TAP_RING_LEN     2048
#define AUDIO_SAMPLE_RATE 44100.0f

// ---- FFT ----
#define FFT_N    1024
#define FFT_BINS (FFT_N / 2)

static float s_fftRe[FFT_N];
static float s_fftIm[FFT_N];
static float s_window[FFT_N];   // Hann window, precomputed once

// Our own circular buffer of downmixed mono audio, decoupled from output_ring's own
// (small, fixed) capacity. Every new sample is written one at a time at s_myWritePos
// (wrapping via modulo) — the FFT window is then just "the most recent FFT_N samples",
// read out via modulo indexing ending at s_myWritePos-1. This replaced an earlier
// version that instead shifted the old analysis window left in place and appended a
// variable-size new chunk each tick — that approach had a real bug: it assumed the
// preserved "old" tail and the freshly-appended "new" head were chronologically
// adjacent, which broke down whenever a tick's read crossed output_ring's own wrap
// point, splicing together samples that weren't actually consecutive in time and
// showing up as a broadband click in the spectrogram on every held note (not an
// artifact of the audio itself — confirmed by dumping the raw samples around the
// splice point and finding the jump sitting exactly at the shift/append boundary).
// A plain circular buffer with per-sample writes has no such seam to get wrong.
#define MY_RING_LEN (FFT_N * 4)
static float    s_myRing[MY_RING_LEN];
static uint32_t s_myWritePos     = 0;  // next write index, mod MY_RING_LEN
static uint32_t s_myTotalWritten = 0;  // total samples ever written (caps at needing >= FFT_N)
static uint16_t s_lastReadPos = 0;     // last-consumed position in output_ring

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
#define SPEC_W 400
#define SPEC_H 256
// Only the lower ~70% of bins are shown (~0-15.4kHz of the 22050Hz Nyquist range at
// 44100Hz/1024) — that's where nearly all musically-useful FX shaping happens, and
// showing the near-silent top octaves linearly would waste half the window height.
#define USABLE_BINS ((int)(FFT_BINS * 0.7f))

static SDL_Window*   s_win    = nullptr;
static SDL_Renderer* s_rend   = nullptr;
static SDL_Texture*  s_tex    = nullptr;
static uint32_t*     s_pixels = nullptr; // SPEC_W*SPEC_H, row-major, scrolls left

static uint32_t heatColor(float mag01) {
    if (mag01 < 0.0f) mag01 = 0.0f;
    if (mag01 > 1.0f) mag01 = 1.0f;
    uint8_t r, g, b;
    if (mag01 < 0.25f) {
        float t = mag01 / 0.25f;
        r = 0; g = 0; b = (uint8_t)(t * 255.0f);
    } else if (mag01 < 0.5f) {
        float t = (mag01 - 0.25f) / 0.25f;
        r = 0; g = (uint8_t)(t * 255.0f); b = 255;
    } else if (mag01 < 0.75f) {
        float t = (mag01 - 0.5f) / 0.25f;
        r = (uint8_t)(t * 255.0f); g = 255; b = (uint8_t)((1.0f - t) * 255.0f);
    } else {
        float t = (mag01 - 0.75f) / 0.25f;
        r = 255; g = (uint8_t)((1.0f - t) * 255.0f); b = 0;
    }
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

// Y row for a given frequency, matching the linear bin→row mapping used when filling
// each new column below (row 0 = top = USABLE_BINS-1, row SPEC_H-1 = bottom = bin 1).
static int rowForFreq(float hz) {
    float bin = hz / (AUDIO_SAMPLE_RATE / (float)FFT_N);
    float t = (bin - 1.0f) / (float)(USABLE_BINS - 1);
    int y = SPEC_H - 1 - (int)(t * (SPEC_H - 1));
    if (y < 0) y = 0; if (y > SPEC_H - 1) y = SPEC_H - 1;
    return y;
}

bool simSpectrogramInit() {
    // Fixed position (not SDL_WINDOWPOS_UNDEFINED): left of the EQ window, both along
    // the screen's top edge — some window managers resolve UNDEFINED to the same spot
    // for every window, stacking every simulator window exactly on top of each other.
    s_win = SDL_CreateWindow("GrvEP - Spectrogramme",
        20, 20,
        SPEC_W, SPEC_H, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!s_win) return false;
    s_rend = SDL_CreateRenderer(s_win, -1, SDL_RENDERER_ACCELERATED);
    if (!s_rend) { SDL_DestroyWindow(s_win); s_win = nullptr; return false; }
    SDL_RenderSetLogicalSize(s_rend, SPEC_W, SPEC_H);
    SDL_SetRenderDrawBlendMode(s_rend, SDL_BLENDMODE_BLEND);
    s_tex = SDL_CreateTexture(s_rend, SDL_PIXELFORMAT_ARGB8888,
                              SDL_TEXTUREACCESS_STREAMING, SPEC_W, SPEC_H);
    s_pixels = (uint32_t*)calloc((size_t)SPEC_W * SPEC_H, sizeof(uint32_t));
    fftInitWindow();
    s_lastReadPos = ring_write_ptr;
    return true;
}

void simSpectrogramRender() {
    if (!s_win) return;

    // Pull whatever's new since last tick (wrap-aware), downmix stereo→mono, and write
    // each frame one at a time into our own circular buffer. If we've fallen behind by
    // more than a full output_ring (audio thread lapped us), the oldest unread frames
    // are simply gone — nothing to do but skip them and resume from what's left. (An
    // earlier version capped this far more aggressively — at half the ring — to guard
    // against the writer lapping us mid-read; that threshold was lower than what a
    // single ~16ms render tick normally consumes, so it fired on essentially every
    // tick instead of just genuine overruns, discarding roughly half of every block
    // and reintroducing the exact same class of splicing artifact this file exists to
    // avoid. Full-ring is the right threshold: genuine overruns are rare enough that
    // measuring against the ring's actual capacity, not an arbitrary fraction of it,
    // is what actually distinguishes them from normal per-tick consumption.)
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

    // Extract the most recent FFT_N samples, ending at s_myWritePos-1, into the linear
    // (windowed) analysis buffer — plain modulo indexing, no shifting/splicing.
    for (int i = 0; i < FFT_N; i++) {
        uint32_t idx = (s_myWritePos + MY_RING_LEN - FFT_N + (uint32_t)i) % MY_RING_LEN;
        s_fftRe[i] = s_myRing[idx] * s_window[i];
        s_fftIm[i] = 0.0f;
    }
    fft(s_fftRe, s_fftIm, FFT_N);

    // Scroll the waterfall left by one column, then fill the new rightmost column.
    for (int y = 0; y < SPEC_H; y++)
        memmove(&s_pixels[y * SPEC_W], &s_pixels[y * SPEC_W + 1], (SPEC_W - 1) * sizeof(uint32_t));
    for (int y = 0; y < SPEC_H; y++) {
        int bin = 1 + (int)((float)(SPEC_H - 1 - y) / (float)(SPEC_H - 1) * (float)(USABLE_BINS - 1));
        float mag = sqrtf(s_fftRe[bin] * s_fftRe[bin] + s_fftIm[bin] * s_fftIm[bin]);
        float db = 20.0f * log10f(mag + 1e-6f);
        float norm = (db + 70.0f) / 70.0f; // -70dB..0dB -> 0..1
        s_pixels[y * SPEC_W + (SPEC_W - 1)] = heatColor(norm);
    }

    SDL_UpdateTexture(s_tex, nullptr, s_pixels, SPEC_W * (int)sizeof(uint32_t));
    SDL_RenderClear(s_rend);
    SDL_RenderCopy(s_rend, s_tex, nullptr, nullptr);

    // Frequency gridlines (1/5/10/15kHz) — thin, translucent, drawn over the data.
    SDL_SetRenderDrawColor(s_rend, 255, 255, 255, 60);
    static const float kGridHz[] = {1000.0f, 5000.0f, 10000.0f, 15000.0f};
    for (float hz : kGridHz) {
        int y = rowForFreq(hz);
        SDL_RenderDrawLine(s_rend, 0, y, SPEC_W - 1, y);
    }

    SDL_RenderPresent(s_rend);
}

void simSpectrogramDestroy() {
    if (s_tex)  SDL_DestroyTexture(s_tex);
    if (s_rend) SDL_DestroyRenderer(s_rend);
    if (s_win)  SDL_DestroyWindow(s_win);
    free(s_pixels);
    s_tex = nullptr; s_rend = nullptr; s_win = nullptr; s_pixels = nullptr;
}

#endif // __ANDROID__
