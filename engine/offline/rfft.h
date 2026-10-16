#pragma once
// A float FFT of a real signal of kRfftN = 4096 points, in place, for the offline sample processing (process.h: the Tonal stretch and
// paulstretch). Not the audio thread's FFT (dsp/fft.h: 512 points, q15, SIMD): this one trades speed for range and resolution, and its
// working memory is the caller's (the card job borrows it from PSRAM while it runs).
//
//   table    kRfftN / 4 + 1 floats: sin(2 pi j / N), j = 0 .. N/4 (rfft_table fills it)
//   data     kRfftN + 2 floats. Time: x[0 .. N). Spectrum: bins 0 .. N/2 as (re, im) pairs, X[k] at data[2k], data[2k + 1],
//            X[k] = sum x[n] e^{-j 2 pi n k / N}
//   inverse  gives back N/2 times the signal (the caller folds 1 / (N/2) into its own scaling)
#include <cstdint>

namespace sc {

constexpr int kRfftN = 4096;

void rfft_table(float *table);
void rfft_forward(float *data, const float *table);
void rfft_inverse(float *data, const float *table);

// cos and sin of 2 pi j / N for any j in [0, N), from the quarter table.
inline void rfft_cs(const float *t, int j, float &c, float &s) {
    const int q = kRfftN / 4;
    if (j < q)          { c = t[q - j];      s = t[j]; }
    else if (j < 2 * q) { c = -t[j - q];     s = t[2 * q - j]; }
    else if (j < 3 * q) { c = -t[3 * q - j]; s = -t[j - 2 * q]; }
    else                { c = t[j - 3 * q];  s = -t[4 * q - j]; }
}

}  // namespace sc
