// Radix-2 FFT stages for the ESP32-S3's PIE (128-bit SIMD) extension, used by fft.cpp on that chip (ADR-016 addendum).
// Data: interleaved int16 complex (re, im), 16-byte aligned; natural order in, bit-reversed order out. Twiddles: q14,
// interleaved, in bit-reversed order (N/2 entries). Each stage computes a +- w * b with everything shifted right by
// `sar - 14` (the block floating point shift chosen by the caller, 0..2), and writes the per-lane max and min of
// its outputs to mm[0..7] / mm[8..15], from which the caller chooses the next stage's shift.
// The loop structure follows Espressif's ESP-DSP dsps_fft2r_sc16_aes3 (Apache-2.0, see THIRD_PARTY_NOTICES.md);
// the per-stage shift and the max / min tracking are ours (ESP-DSP halves every stage).
#include "sdkconfig.h"
#if CONFIG_IDF_TARGET_ESP32S3

    .text

// void fft_s3_stage(int16_t *data, const int16_t *w, int n2, int groups, int sar, int16_t *mm)
// General stage, half-size n2 >= 4 complex (one vector = 4 complex), `groups` groups of 2 * n2.
    .align 4
    .global fft_s3_stage
    .type fft_s3_stage, @function
fft_s3_stage:
    entry       a1, 64
    wsr.sar     a6
    movi.n      a8, 1
    slli        a8, a8, 14              // 1.0 in q14: a * 16384 >> sar = a >> (sar - 14)
    s16i        a8, a1, 0
    mov.n       a9, a1
    ee.vldbc.16.ip q6, a9, 0
    ee.zero.q   q4                      // running max
    ee.zero.q   q7                      // running min
    srli        a10, a4, 2              // vectors per half group
    slli        a15, a4, 2              // half group in bytes
    mov.n       a9, a2                  // a: first half of the group
    movi.n      a14, 0
.Lgroup:
    add.n       a8, a9, a15             // b: second half
    ee.vldbc.32.ip q5, a3, 4            // the group's twiddle, broadcast
    loopnez     a10, .Lgroup_end
    ee.vld.128.ip q0, a8, 0
    ee.vld.128.ip q2, a9, 0
    ee.cmul.s16 q1, q5, q0, 2
    ee.vmul.s16 q2, q2, q6
    ee.cmul.s16 q1, q5, q0, 3
    ee.vsubs.s16 q3, q2, q1
    ee.vmax.s16 q4, q4, q3
    ee.vmin.s16 q7, q7, q3
    ee.vadds.s16.st.incp q3, a8, q3, q2, q1     // stores a - wb at b, then q3 = a + wb
    ee.vmax.s16 q4, q4, q3
    ee.vmin.s16 q7, q7, q3
    ee.vst.128.ip q3, a9, 16
.Lgroup_end:
    addi.n      a14, a14, 1
    add.n       a9, a9, a15             // skip the second half
    bne         a14, a5, .Lgroup
    ee.vst.128.ip q4, a7, 16
    ee.vst.128.ip q7, a7, 0
    retw.n

// void fft_s3_stage2(int16_t *data, const int16_t *w, int groups, int sar, int16_t *mm)
// Half-size 2: two groups (8 complex) per iteration.
    .align 4
    .global fft_s3_stage2
    .type fft_s3_stage2, @function
fft_s3_stage2:
    entry       a1, 64
    wsr.sar     a5
    movi.n      a8, 1
    slli        a8, a8, 14
    s16i        a8, a1, 0
    mov.n       a9, a1
    ee.vldbc.16.ip q6, a9, 0
    ee.zero.q   q5
    ee.zero.q   q7
    srli        a10, a4, 1
    mov.n       a8, a2
    mov.n       a9, a3
    loopnez     a10, .Lst2_end
    ee.vld.l.64.ip q0, a8, 8            // a of group 1
    ee.vldbc.32.ip q2, a9, 4            // w1
    ee.vld.l.64.ip q1, a8, 8            // b of group 1
    ee.vldbc.32.ip q3, a9, 4            // w2
    ee.vld.h.64.ip q0, a8, 8            // a of group 2
    ee.vunzip.32 q2, q3                 // q2 = w1 w1 w2 w2
    ee.vld.h.64.ip q1, a8, -24          // b of group 2, back to the start
    ee.vmul.s16 q0, q0, q6
    ee.cmul.s16 q4, q2, q1, 2
    ee.cmul.s16 q4, q2, q1, 3
    ee.vadds.s16 q2, q0, q4
    ee.vsubs.s16 q3, q0, q4
    ee.vmax.s16 q5, q5, q2
    ee.vmin.s16 q7, q7, q2
    ee.vmax.s16 q5, q5, q3
    ee.vmin.s16 q7, q7, q3
    ee.vst.l.64.ip q2, a8, 8
    ee.vst.l.64.ip q3, a8, 8
    ee.vst.h.64.ip q2, a8, 8
    ee.vst.h.64.ip q3, a8, 8
.Lst2_end:
    ee.vst.128.ip q5, a6, 16
    ee.vst.128.ip q7, a6, 0
    retw.n

// void fft_s3_stage1(int16_t *data, const int16_t *w, int n, int sar, int16_t *mm)
// Half-size 1 (the last stage): 4 butterflies (8 complex) per iteration, 4 twiddles.
    .align 4
    .global fft_s3_stage1
    .type fft_s3_stage1, @function
fft_s3_stage1:
    entry       a1, 64
    wsr.sar     a5
    movi.n      a8, 1
    slli        a8, a8, 14
    s16i        a8, a1, 0
    mov.n       a9, a1
    ee.vldbc.16.ip q6, a9, 0
    ee.zero.q   q4
    ee.zero.q   q7
    srli        a10, a4, 3
    loopnez     a10, .Lst1_end
    ee.vld.128.ip q0, a2, 16
    ee.vld.128.ip q1, a2, -16
    ee.vld.128.ip q2, a3, 16
    ee.vunzip.32 q0, q1                 // q0 = the a's, q1 = the b's
    ee.cmul.s16 q3, q2, q1, 2
    ee.vmul.s16 q0, q0, q6
    ee.cmul.s16 q3, q2, q1, 3
    ee.vsubs.s16 q1, q0, q3
    ee.vadds.s16 q0, q0, q3
    ee.vmax.s16 q4, q4, q0
    ee.vmin.s16 q7, q7, q0
    ee.vmax.s16 q4, q4, q1
    ee.vmin.s16 q7, q7, q1
    ee.vzip.32  q0, q1
    ee.vst.128.ip q0, a2, 16
    ee.vst.128.ip q1, a2, 16
.Lst1_end:
    ee.vst.128.ip q4, a6, 16
    ee.vst.128.ip q7, a6, 0
    retw.n

// void fft_s3_minmax(const int16_t *re, const int16_t *im, int n, int16_t *mm)
// Per-lane max (mm[0..7]) and min (mm[8..15]) of two int16 arrays of n values (n multiple of 8, 16-byte aligned).
    .align 4
    .global fft_s3_minmax
    .type fft_s3_minmax, @function
fft_s3_minmax:
    entry       a1, 32
    ee.zero.q   q4
    ee.zero.q   q7
    srli        a10, a4, 3
    loopnez     a10, .Lmm_end
    ee.vld.128.ip q0, a2, 16
    ee.vld.128.ip q1, a3, 16
    ee.vmax.s16 q4, q4, q0
    ee.vmin.s16 q7, q7, q0
    ee.vmax.s16 q4, q4, q1
    ee.vmin.s16 q7, q7, q1
.Lmm_end:
    ee.vst.128.ip q4, a5, 16
    ee.vst.128.ip q7, a5, 0
    retw.n

// void fft_s3_pack(const int16_t *re, const int16_t *im, int16_t *out, int n, int scale)
// out = interleaved (re, im) * scale (scale = 2^up, exact: the caller keeps the result in range). 16-byte aligned.
// void pie_vmul_s16(const int16_t *a, const int16_t *b, int16_t *out, int n, int sar)
// out[i] = (a[i] * b[i]) >> sar, 8 lanes at a time (ee.vmul.s16; n multiple of 8, all 16-byte aligned). The STFT window.
    .align 4
    .global pie_vmul_s16
    .type pie_vmul_s16, @function
pie_vmul_s16:
    entry       a1, 32
    wsr.sar     a6
    srli        a10, a5, 3
    loopnez     a10, .Lvm_end
    ee.vld.128.ip q0, a2, 16
    ee.vld.128.ip q1, a3, 16
    ee.vmul.s16 q2, q0, q1
    ee.vst.128.ip q2, a4, 16
.Lvm_end:
    retw.n

// Overlap-add of the STFT, the same arithmetic as the C loop in stft.h (bit-identical):
//   v = (re * win + 2^14) >> 15;  v = v << s  (pie_ola_shl, 0 <= s <= 16)  or  (v + 2^(r-1)) >> r  (pie_ola_shr, 1 <= r <= 30);
//   acc = sat32(acc + v).
// ee.vmul.s16 truncates and wraps (measured): with SAR 0 it gives the product's low 16 bits, with SAR 16 its high 16 bits,
// and vzip.16 joins them into the exact 32-bit products. n multiple of 8, everything 16-byte aligned.
// void pie_ola_shl(const int16_t *re, const int16_t *win, int32_t *acc, int n, int s)
    .align 4
    .global pie_ola_shl
    .type pie_ola_shl, @function
pie_ola_shl:
    entry       a1, 64
    movi.n      a8, 1
    slli        a8, a8, 14
    s32i        a8, a1, 0
    mov.n       a9, a1
    ee.vldbc.32.ip q7, a9, 0            // 2^14 in every lane
    movi.n      a11, 0
    movi.n      a12, 16
    movi.n      a13, 15
    srli        a10, a5, 3
    loopnez     a10, .Lshl_end
    ee.vld.128.ip q0, a2, 16            // re
    ee.vld.128.ip q1, a3, 16            // win
    wsr.sar     a11
    ee.vmul.s16 q2, q0, q1              // low halves of the products
    wsr.sar     a12
    ee.vmul.s16 q3, q0, q1              // high halves
    ee.vzip.16  q2, q3                  // q2 = p0..p3, q3 = p4..p7 (int32)
    ee.vadds.s32 q2, q2, q7
    ee.vadds.s32 q3, q3, q7
    wsr.sar     a13
    ee.vsr.32   q2, q2
    ee.vsr.32   q3, q3
    wsr.sar     a6
    ee.vsl.32   q2, q2
    ee.vsl.32   q3, q3
    ee.vld.128.ip q4, a4, 16
    ee.vld.128.ip q5, a4, -16
    ee.vadds.s32 q4, q4, q2
    ee.vadds.s32 q5, q5, q3
    ee.vst.128.ip q4, a4, 16
    ee.vst.128.ip q5, a4, 16
.Lshl_end:
    retw.n

// void pie_ola_shr(const int16_t *re, const int16_t *win, int32_t *acc, int n, int r)
    .align 4
    .global pie_ola_shr
    .type pie_ola_shr, @function
pie_ola_shr:
    entry       a1, 64
    movi.n      a8, 1
    slli        a8, a8, 14
    s32i        a8, a1, 0
    mov.n       a9, a1
    ee.vldbc.32.ip q7, a9, 0            // 2^14
    addi.n      a8, a6, -1
    movi.n      a9, 1
    ssl         a8
    sll         a9, a9                  // 2^(r-1)
    s32i        a9, a1, 4
    addi.n      a9, a1, 4
    ee.vldbc.32.ip q6, a9, 0
    movi.n      a11, 0
    movi.n      a12, 16
    movi.n      a13, 15
    srli        a10, a5, 3
    loopnez     a10, .Lshr_end
    ee.vld.128.ip q0, a2, 16
    ee.vld.128.ip q1, a3, 16
    wsr.sar     a11
    ee.vmul.s16 q2, q0, q1
    wsr.sar     a12
    ee.vmul.s16 q3, q0, q1
    ee.vzip.16  q2, q3
    ee.vadds.s32 q2, q2, q7
    ee.vadds.s32 q3, q3, q7
    wsr.sar     a13
    ee.vsr.32   q2, q2
    ee.vsr.32   q3, q3
    ee.vadds.s32 q2, q2, q6
    ee.vadds.s32 q3, q3, q6
    wsr.sar     a6
    ee.vsr.32   q2, q2
    ee.vsr.32   q3, q3
    ee.vld.128.ip q4, a4, 16
    ee.vld.128.ip q5, a4, -16
    ee.vadds.s32 q4, q4, q2
    ee.vadds.s32 q5, q5, q3
    ee.vst.128.ip q4, a4, 16
    ee.vst.128.ip q5, a4, 16
.Lshr_end:
    retw.n

// void pie_cordic(const PieCordicArgs *a)   { const int16_t *re, *im; int32_t *xo, *ao; const uint32_t *atan; int n, iters; }
// The vectoring CORDIC of cordic.h (cordic_polar) on 4 bins at a time, bit-identical: per bin xo = the final x (before the
// 1/K scaling) and ao = the accumulated angle (without the half-turn for x < 0, which the caller adds). n multiple of 4;
// re / im 8-byte aligned, xo / ao 16-byte aligned. "if (y >= 0) add else subtract" = add (v ^ s) - s with s = y >> 31.
    .align 4
    .global pie_cordic
    .type pie_cordic, @function
pie_cordic:
    entry       a1, 32
    l32i.n      a3, a2, 4               // im
    l32i.n      a4, a2, 8               // xo
    l32i.n      a5, a2, 12              // ao
    l32i.n      a7, a2, 16              // atan table
    l32i.n      a10, a2, 20             // n
    l32i.n      a12, a2, 24             // iterations
    l32i.n      a2, a2, 0               // re
    srli        a10, a10, 2             // groups of 4 bins
    movi.n      a13, 31
    movi.n      a14, 4
    beqz        a10, .Lcd_end
.Lcd_group:
    ee.zero.q   q2
    ee.zero.q   q3
    ee.vld.l.64.ip q0, a2, 8            // re0..3
    ee.vld.l.64.ip q1, a3, 8            // im0..3
    ee.vzip.16  q2, q0                  // the 16-bit values in the high halves of 32-bit lanes
    ee.vzip.16  q3, q1
    wsr.sar     a14
    ee.vsr.32   q2, q2                  // x * 4096
    ee.vsr.32   q3, q3                  // y * 4096
    wsr.sar     a13
    ee.vsr.32   q5, q2                  // -1 where x < 0: turn by half a turn (x, y negated)
    ee.xorq     q2, q2, q5
    ee.vsubs.s32 q2, q2, q5
    ee.xorq     q3, q3, q5
    ee.vsubs.s32 q3, q3, q5
    ee.zero.q   q4                      // angle
    mov.n       a9, a7
    movi.n      a11, 0                  // i
    loopnez     a12, .Lcd_iter_end
    wsr.sar     a13
    ee.vsr.32   q5, q3                  // s = -1 where y < 0
    wsr.sar     a11
    ee.vsr.32   q6, q3                  // dx = y >> i
    ee.vsr.32   q7, q2                  // dy = x >> i
    ee.vldbc.32.ip q1, a9, 4            // atan(2^-i)
    ee.xorq     q6, q6, q5
    ee.vsubs.s32 q6, q6, q5
    ee.vadds.s32 q2, q2, q6             // x += s ? -dx : dx
    ee.xorq     q7, q7, q5
    ee.vsubs.s32 q7, q7, q5
    ee.vsubs.s32 q3, q3, q7             // y -= s ? -dy : dy
    ee.xorq     q1, q1, q5
    ee.vsubs.s32 q1, q1, q5
    ee.vadds.s32 q4, q4, q1             // angle += s ? -atan : atan
    addi.n      a11, a11, 1
.Lcd_iter_end:
    ee.vst.128.ip q2, a4, 16
    ee.vst.128.ip q4, a5, 16
    addi.n      a10, a10, -1
    bnez        a10, .Lcd_group
.Lcd_end:
    retw.n

    .align 4
    .global fft_s3_pack
    .type fft_s3_pack, @function
fft_s3_pack:
    entry       a1, 64
    movi.n      a8, 0
    wsr.sar     a8                      // vmul: (x * scale) >> 0
    s16i        a6, a1, 0
    mov.n       a9, a1
    ee.vldbc.16.ip q6, a9, 0
    srli        a10, a5, 3
    loopnez     a10, .Lpk_end
    ee.vld.128.ip q0, a2, 16
    ee.vld.128.ip q1, a3, 16
    ee.vmul.s16 q0, q0, q6
    ee.vmul.s16 q1, q1, q6
    ee.vzip.16  q0, q1                  // q0 = re0 im0 .. re3 im3, q1 = re4 im4 .. re7 im7
    ee.vst.128.ip q0, a4, 16
    ee.vst.128.ip q1, a4, 16
.Lpk_end:
    retw.n

// void pie_conv2_s16(const int16_t *xl, const int16_t *xr, const int16_t *t, int nvec, int32_t *acc)
// The Convolver's two dot products (fx2_modules.cpp): sum of t[j] * xl[j], then of t[j] * xr[j], j < 8 * nvec, 8 lanes per
// ee.vmulas.s16.accx into the 40-bit ACCX (exact: the module's taps are at most 32767 in size, 512 of them). acc[0] / acc[1] = the low
// 32 / high 8 bits of the left sum, acc[2] / acc[3] the right one. Everything 16-byte aligned, nvec >= 1; reads exactly 8 * nvec values.
    .align 4
    .global pie_conv2_s16
    .type pie_conv2_s16, @function
pie_conv2_s16:
    entry       a1, 32
    ee.zero.accx
    mov.n       a8, a4
    loopnez     a5, .Lcv_l
    ee.vld.128.ip q0, a2, 16
    ee.vld.128.ip q1, a8, 16
    ee.vmulas.s16.accx q0, q1
.Lcv_l:
    rur.accx_0  a10
    rur.accx_1  a11
    s32i.n      a10, a6, 0
    s32i.n      a11, a6, 4
    ee.zero.accx
    mov.n       a8, a4
    loopnez     a5, .Lcv_r
    ee.vld.128.ip q0, a3, 16
    ee.vld.128.ip q1, a8, 16
    ee.vmulas.s16.accx q0, q1
.Lcv_r:
    rur.accx_0  a10
    rur.accx_1  a11
    s32i.n      a10, a6, 8
    s32i.n      a11, a6, 12
    retw.n

#endif
