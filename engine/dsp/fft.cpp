#include "engine/dsp/fft.h"
#if SC_FFT_PIE && defined(HWV1_BENCH)
#include <esp_cpu.h>
#endif

namespace sc {
namespace {

constexpr int N = kFftN;
constexpr int32_t kSafe = 13500;     // max |component| for which one butterfly stage cannot overflow int16: (1 + sqrt 2) * 13500 < 32768

inline int32_t iabs(int32_t v) { return v < 0 ? -v : v; }
inline int32_t imax(int32_t a, int32_t b) { return a > b ? a : b; }

// One radix-2 stage; returns the largest |component| it wrote, which decides the next stage's shift.
// max(|a + t|, |a - t|) = |a| + |t|, so the scan costs two abs-adds per butterfly instead of a pass over the block.
// Butterflies of a stage are independent: the twiddle loop is outside, each twiddle is read once.
template <bool kShift>
int32_t stage(int16_t *re, int16_t *im, int len, int sh) {
    const int32_t rnd = kShift ? (1 << (sh - 1)) : 0;
    const int half = len >> 1, step = N / len;
    int32_t mx = 0;
    for (int k = 0; k < half; k++) {
        const int32_t wr = kFftCos[k * step], wi = kFftSin[k * step];
        for (int a = k; a < N; a += len) {
            const int b = a + half;
            const int32_t ar = kShift ? (re[a] + rnd) >> sh : re[a], ai = kShift ? (im[a] + rnd) >> sh : im[a];
            const int32_t br = kShift ? (re[b] + rnd) >> sh : re[b], bi = kShift ? (im[b] + rnd) >> sh : im[b];
            const int32_t tr = (br * wr - bi * wi + (1 << 14)) >> 15;
            const int32_t ti = (br * wi + bi * wr + (1 << 14)) >> 15;
            re[a] = static_cast<int16_t>(ar + tr);
            im[a] = static_cast<int16_t>(ai + ti);
            re[b] = static_cast<int16_t>(ar - tr);
            im[b] = static_cast<int16_t>(ai - ti);
            mx = imax(mx, imax(iabs(ar) + iabs(tr), iabs(ai) + iabs(ti)));
        }
    }
    return mx;
}

int fft_core(int16_t *re, int16_t *im) {
    int32_t mx = 0;
    for (int i = 0; i < N; i++) {
        int j = kFftRev[i];
        if (j > i) {
            int16_t t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
        mx = imax(mx, imax(iabs(re[i]), iabs(im[i])));         // the swap only permutes: the max of the slots seen so far is enough
    }
    int exp = 0;
    for (int len = 2; len <= N; len <<= 1) {
        int sh = 0;
        while ((mx >> sh) > kSafe) sh++;
        exp += sh;
        mx = sh ? stage<true>(re, im, len, sh) : stage<false>(re, im, len, 0);
    }
    return exp;
}

#if SC_FFT_PIE
}  // namespace
uint32_t g_fft_pie_prof[11];
uint32_t g_stft_prof[7];
extern "C" {
void fft_s3_stage(int16_t *data, const int16_t *w, int n2, int groups, int sar, int16_t *mm);
void fft_s3_stage2(int16_t *data, const int16_t *w, int groups, int sar, int16_t *mm);
void fft_s3_stage1(int16_t *data, const int16_t *w, int n, int sar, int16_t *mm);
void fft_s3_minmax(const int16_t *re, const int16_t *im, int n, int16_t *mm);
void fft_s3_pack(const int16_t *re, const int16_t *im, int16_t *out, int n, int scale);
}
namespace {

alignas(16) int16_t g_buf[2 * N];    // interleaved work buffer: one transform at a time (the audio task)
alignas(16) int16_t g_tw[N];         // N/2 twiddles, q14, interleaved, bit-reversed order (the kernels' layout)
alignas(16) int16_t g_mm[16];        // a stage's per-lane max (0..7) and min (8..15)
int g_tw_sign = 0;                   // 0 = not built yet
constexpr int kPieSign = -1;         // ee.cmul's convention (measured on the board: +1 mirrors the spectrum)
#ifdef HWV1_BENCH
#define PIE_T(i) do { const uint32_t t_ = esp_cpu_get_cycle_count(); g_fft_pie_prof[i] += t_ - t_last; t_last = t_; } while (0)
#else
#define PIE_T(i) do { (void)(i); } while (0)
#endif

void pie_twiddles(int sign) {
    for (int j = 0; j < N / 2; j++) {
        const int k = kFftRev[j] >> 1;                          // 8-bit reversal of j
        g_tw[2 * j] = static_cast<int16_t>((kFftCos[k] + 1) >> 1);
        g_tw[2 * j + 1] = static_cast<int16_t>(sign * ((kFftSin[k] + 1) >> 1));
    }
    g_tw_sign = sign;
}

int pie_shift(int32_t mx) { int sh = 0; while ((mx >> sh) > kSafe) sh++; return sh; }
int32_t pie_max() {
    int32_t mx = 0;
    for (int i = 0; i < 8; i++) mx = imax(mx, imax(g_mm[i], -static_cast<int32_t>(g_mm[8 + i])));
    return mx;
}

int fft_pie(int16_t *re, int16_t *im) {
    if (!g_tw_sign) pie_twiddles(kPieSign);
#ifdef HWV1_BENCH
    uint32_t t_last = esp_cpu_get_cycle_count();
#endif
    uint32_t *d = reinterpret_cast<uint32_t *>(g_buf);
    const bool aligned = ((reinterpret_cast<uintptr_t>(re) | reinterpret_cast<uintptr_t>(im)) & 15) == 0;
    int32_t mx = 0;
    if (aligned) { fft_s3_minmax(re, im, N, g_mm); mx = pie_max(); }
    else for (int i = 0; i < N; i++) mx = imax(mx, imax(iabs(re[i]), iabs(im[i])));
    // The PIE shifts truncate (the C version rounds): a quiet frame that never shifts would keep a truncation error at its own
    // LSB in every stage. Scaling it up to the safe level first gives every frame the precision of a loud one.
    int up = 0;
    while (mx && (mx << (up + 1)) <= kSafe) up++;
    if (aligned) fft_s3_pack(re, im, g_buf, N, 1 << up);
    else for (int i = 0; i < N; i++)
        d[i] = static_cast<uint16_t>(re[i] << up) | (static_cast<uint32_t>(static_cast<uint16_t>(im[i] << up)) << 16);
    mx <<= up;
    int exp = -up, groups = 1, sh, st = 1;
    PIE_T(0);
    for (int n2 = N / 2; n2 >= 4; n2 >>= 1, groups <<= 1) {
        sh = pie_shift(mx); exp += sh;
        fft_s3_stage(g_buf, g_tw, n2, groups, 14 + sh, g_mm);
        mx = pie_max();
        PIE_T(st++);
    }
    sh = pie_shift(mx); exp += sh;
    fft_s3_stage2(g_buf, g_tw, groups, 14 + sh, g_mm);
    mx = pie_max();
    PIE_T(8);
    sh = pie_shift(mx); exp += sh;
    fft_s3_stage1(g_buf, g_tw, N, 14 + sh, g_mm);
    PIE_T(9);
    for (int i = 0; i < N; i++) {                               // the kernels leave the bins in bit-reversed order
        const uint32_t v = d[i];
        const int k = kFftRev[i];
        re[k] = static_cast<int16_t>(v);
        im[k] = static_cast<int16_t>(v >> 16);
    }
    PIE_T(10);
    return exp;
}
#endif

}  // namespace

int fft_q15_c(int16_t *re, int16_t *im) { return fft_core(re, im); }

// swapping re and im turns the forward FFT into the inverse
#if SC_FFT_PIE
int fft_q15(int16_t *re, int16_t *im) { return fft_pie(re, im); }
int ifft_q15(int16_t *re, int16_t *im) { return fft_pie(im, re); }
#else
int fft_q15(int16_t *re, int16_t *im) { return fft_core(re, im); }
int ifft_q15(int16_t *re, int16_t *im) { return fft_core(im, re); }
#endif

}  // namespace sc
