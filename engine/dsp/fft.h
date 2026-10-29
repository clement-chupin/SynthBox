#pragma once
// In-place radix-2 decimation-in-time complex FFT of size kFftN (512), q15 data with block floating point
// (ADR-016). Before each stage the whole block is scanned and shifted right just enough to keep the worst case
// butterfly growth (1 + sqrt 2) inside int16, so precision follows the signal level. The result is
//   X_true = stored * 2^exp      (exp = the returned number of right shifts)
// The forward transform is X[k] = sum x[n] e^{-j 2 pi n k / N}; the inverse is the unnormalised
// x[n] = sum X[k] e^{+j 2 pi n k / N}, so a forward + inverse round trip multiplies by N (callers fold 1/N into
// the exponent).
// On the ESP32-S3 (SC_FFT_PIE) the stages also exist as PIE SIMD kernels (fft_s3.S) with the same block floating point.
#include <cstdint>
#include "engine/dsp/tables.h"
#if defined(ESP_PLATFORM)
#include "sdkconfig.h"
#if CONFIG_IDF_TARGET_ESP32S3
#define SC_FFT_PIE 1
#endif
#endif

namespace sc {

int fft_q15(int16_t *re, int16_t *im);      // forward, returns exp
int ifft_q15(int16_t *re, int16_t *im);     // inverse (unnormalised), returns exp

int fft_q15_c(int16_t *re, int16_t *im);    // the portable C version (what fft_q15 is everywhere but the S3)

#if SC_FFT_PIE
extern uint32_t g_fft_pie_prof[11];         // HWV1_BENCH builds: cycles summed per piece (pack, stages 1..9, unpack)
extern uint32_t g_stft_prof[7];             // HWV1_BENCH builds: Stft frame pieces (window, fft, callback, mirror, ifft, overlap-add, fifo)
#endif
}  // namespace sc

#if SC_FFT_PIE
extern "C" void pie_vmul_s16(const int16_t *a, const int16_t *b, int16_t *out, int n, int sar);   // fft_s3.S
extern "C" void pie_ola_shl(const int16_t *re, const int16_t *win, int32_t *acc, int n, int s);  // the Stft overlap-add, 0 <= s <= 16
extern "C" void pie_ola_shr(const int16_t *re, const int16_t *win, int32_t *acc, int n, int r);  // and with a right shift, 1 <= r <= 30
extern "C" void pie_conv2_s16(const int16_t *xl, const int16_t *xr, const int16_t *t, int nvec, int32_t *acc);   // the Convolver's dot products
#endif

namespace sc {

}  // namespace sc
