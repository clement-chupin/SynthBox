#include "engine/offline/rfft.h"
#include <cmath>

namespace sc {

namespace {
constexpr int kN = kRfftN, kM = kRfftN / 2;

// In-place complex FFT of kM points, (re, im) pairs; sign -1 = forward, +1 = inverse (not normalised). Radix 2, the twiddles from
// the quarter table (the angle 2 pi k / len is the table's 2 pi (k N / len) / N, always under half a turn).
void cfft(float *z, const float *t, float sign) {
    for (int i = 1, j = 0; i < kM; i++) {                                     // bit reversal
        int bit = kM >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            const float r = z[2 * i], m = z[2 * i + 1];
            z[2 * i] = z[2 * j]; z[2 * i + 1] = z[2 * j + 1];
            z[2 * j] = r; z[2 * j + 1] = m;
        }
    }
    for (int len = 2; len <= kM; len <<= 1) {
        const int half = len / 2, step = kN / len;
        for (int k = 0; k < half; k++) {
            float wr, wi;
            rfft_cs(t, k * step, wr, wi);
            wi *= sign;
            for (int i = k; i < kM; i += len) {
                float *a = z + 2 * i, *b = z + 2 * (i + half);
                const float tr = b[0] * wr - b[1] * wi, ti = b[0] * wi + b[1] * wr;
                b[0] = a[0] - tr; b[1] = a[1] - ti;
                a[0] += tr; a[1] += ti;
            }
        }
    }
}
}  // namespace

void rfft_table(float *table) {
    for (int j = 0; j <= kN / 4; j++) table[j] = static_cast<float>(std::sin(6.283185307179586 * j / kN));
}

// The real signal as kM complex points z[n] = x[2n] + j x[2n + 1] (the pairs as they lie), one complex FFT, then the even and odd
// halves are told apart: with E / O the spectra of the even / odd samples, Z = E + jO, E[k] = (Z[k] + conj Z[M - k]) / 2,
// O[k] = -j (Z[k] - conj Z[M - k]) / 2, and X[k] = E[k] + w^k O[k], X[M - k] = conj(E[k] - w^k O[k]), w = e^{-j 2 pi / N}.
void rfft_forward(float *x, const float *t) {
    cfft(x, t, -1.0f);
    const float e0 = x[0], o0 = x[1];
    for (int k = 1; k <= kM / 2; k++) {
        const int m = kM - k;
        const float a = x[2 * k], b = x[2 * k + 1], c = x[2 * m], d = x[2 * m + 1];
        const float er = 0.5f * (a + c), ei = 0.5f * (b - d), orr = 0.5f * (b + d), oi = 0.5f * (c - a);
        float wr, wi;
        rfft_cs(t, k, wr, wi);                                                // w^k = (wr, -wi)
        const float tr = orr * wr + oi * wi, ti = oi * wr - orr * wi;
        x[2 * k] = er + tr; x[2 * k + 1] = ei + ti;
        x[2 * m] = er - tr; x[2 * m + 1] = -(ei - ti);
    }
    x[0] = e0 + o0; x[1] = 0.0f;
    x[kN] = e0 - o0; x[kN + 1] = 0.0f;
}

// The other way: E[k] = (X[k] + conj X[M - k]) / 2, O[k] = w^-k (X[k] - conj X[M - k]) / 2, Z[k] = E[k] + jO[k],
// Z[M - k] = conj E[k] + j conj O[k]; the inverse complex FFT then leaves x[2n], x[2n + 1] in the pairs, times kM.
void rfft_inverse(float *x, const float *t) {
    const float x0 = x[0], xm = x[kN];
    for (int k = 1; k <= kM / 2; k++) {
        const int m = kM - k;
        const float a = x[2 * k], b = x[2 * k + 1], c = x[2 * m], d = x[2 * m + 1];
        const float er = 0.5f * (a + c), ei = 0.5f * (b - d), dr = 0.5f * (a - c), di = 0.5f * (b + d);
        float wr, wi;
        rfft_cs(t, k, wr, wi);                                                // w^-k = (wr, +wi)
        const float orr = dr * wr - di * wi, oi = dr * wi + di * wr;
        x[2 * k] = er - oi; x[2 * k + 1] = ei + orr;
        x[2 * m] = er + oi; x[2 * m + 1] = orr - ei;
    }
    x[0] = 0.5f * (x0 + xm); x[1] = 0.5f * (x0 - xm);
    cfft(x, t, 1.0f);
}

}  // namespace sc
