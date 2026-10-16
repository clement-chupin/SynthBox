#pragma once
// Short-time Fourier transform engine for spectral effects (ADR-016): sine analysis window, FFT, a callback that
// edits the spectrum, inverse FFT, sine synthesis window, overlap-add with 75 % overlap (hop = N/4).
// sin^2 windows at hop N/4 sum to 2, folded into the output scaling, so an untouched spectrum is a perfect
// reconstruction (measured in the tests). Latency is reported by latency().
//
// The callback gets the spectrum as Frame{re, im, exp}: true value = stored * 2^exp (block floating point).
// It may edit bins and change `exp` (e.g. when it restores held, older data). Mono mode (hermitian = true): the
// input is real, the callback edits bins 0..N/2 only and the engine mirrors them. Dual mode: a second input is
// packed into the imaginary part (X = A + jB); the callback owns the whole spectrum and must leave the Hermitian
// spectrum of the (real) result in re/im.
#include <cstdint>
#include "engine/core/heap.h"
#include "engine/dsp/fft.h"
#include "engine/dsp/q.h"
#include "engine/dsp/tables.h"
#include <cstring>
#if SC_FFT_PIE && defined(HWV1_BENCH)
#include <esp_cpu.h>
#define STFT_T(i) do { const uint32_t t_ = esp_cpu_get_cycle_count(); g_stft_prof[i] += t_ - t_last; t_last = t_; } while (0)
#else
#define STFT_T(i) do {} while (0)
#endif

namespace sc {

struct StftFrame {
    int16_t *re;
    int16_t *im;
    int exp;
};

class Stft {
public:
    static constexpr int N = kFftN;
    static constexpr int H = N / 4;
    static constexpr int kFifo = 2 * H;

    Stft() = default;
    Stft(const Stft &) = delete;
    Stft &operator=(const Stft &) = delete;
    ~Stft() { release(); }

    bool init(Heap &h, bool dual) {
        release();
        heap_ = &h;
        mem_ = static_cast<uint8_t *>(h.alloc(bytes(dual)));
        if (!mem_) { heap_ = nullptr; return false; }
        uint8_t *p = mem_;
        in_ = reinterpret_cast<q15 *>(p); p += sizeof(q15) * N;
        if (dual) { in2_ = reinterpret_cast<q15 *>(p); p += sizeof(q15) * N; }
        re_ = reinterpret_cast<int16_t *>(p); p += sizeof(int16_t) * N;
        im_ = reinterpret_cast<int16_t *>(p); p += sizeof(int16_t) * N;
        fifo_ = reinterpret_cast<q15 *>(p); p += sizeof(q15) * kFifo;
        acc_ = reinterpret_cast<int32_t *>(p);
        reset();
        return true;
    }
    void release() { if (heap_) heap_->free(mem_); mem_ = nullptr; heap_ = nullptr; }
    bool ready() const { return mem_ != nullptr; }

    void reset() {
        for (int i = 0; i < N; i++) { in_[i] = 0; if (in2_) in2_[i] = 0; acc_[i] = 0; }
        for (int i = 0; i < kFifo; i++) fifo_[i] = 0;
        ipos_ = opos_ = hop_ = 0;
        rd_ = 0; wr_ = H; count_ = H;               // prefill: the first output block never underflows
    }

    // Input-to-output delay in samples (measured by the tests): window fill + hop alignment.
    static constexpr int latency() { return N; }

    // Processes n samples; in2 may be null when the engine was initialised mono.
    template <typename Fn>
    void process(const q15 *in, const q15 *in2, q15 *out, int n, Fn &&fn) {
        for (int i = 0; i < n; i++) {
            in_[ipos_] = in[i];
            if (in2_) in2_[ipos_] = in2 ? in2[i] : 0;
            ipos_ = (ipos_ + 1) & (N - 1);
            if (++hop_ == H) { hop_ = 0; frame(fn); }
            out[i] = fifo_[rd_];
            rd_ = (rd_ + 1) & (kFifo - 1);
            count_--;
        }
    }

private:
    static size_t bytes(bool dual) {
        return sizeof(q15) * N * (dual ? 2 : 1) + sizeof(int16_t) * N * 2 + sizeof(q15) * kFifo + sizeof(int32_t) * N + 16;
    }

    template <typename Fn>
    void frame(Fn &fn) {
#if SC_FFT_PIE && defined(HWV1_BENCH)
        uint32_t t_last = esp_cpu_get_cycle_count();
#endif
#if SC_FFT_PIE
        // ipos_ is a multiple of H here: the ring's two runs are whole 16-byte vectors (the heap and kStftWin are aligned)
        const int run = N - ipos_;
        pie_vmul_s16(in_ + ipos_, kStftWin, re_, run, 15);
        if (ipos_) pie_vmul_s16(in_, kStftWin + run, re_ + run, ipos_, 15);
        if (in2_) {
            pie_vmul_s16(in2_ + ipos_, kStftWin, im_, run, 15);
            if (ipos_) pie_vmul_s16(in2_, kStftWin + run, im_ + run, ipos_, 15);
        } else {
            std::memset(im_, 0, sizeof(int16_t) * N);
        }
#else
        for (int n = 0; n < N; n++) {
            const int idx = (ipos_ + n) & (N - 1);
            re_[n] = mul15(kStftWin[n], in_[idx]);
            im_[n] = in2_ ? mul15(kStftWin[n], in2_[idx]) : 0;
        }
#endif
        STFT_T(0);
        StftFrame f{re_, im_, fft_q15(re_, im_)};
        STFT_T(1);
        fn(f);
        STFT_T(2);
        if (!in2_) {                                              // mono: enforce a Hermitian spectrum
            im_[0] = 0;
            im_[N / 2] = 0;
            for (int k = 1; k < N / 2; k++) { re_[N - k] = re_[k]; im_[N - k] = sat16(-static_cast<int32_t>(im_[k])); }
        }
        STFT_T(3);
        const int ei = ifft_q15(re_, im_);
        STFT_T(4);
        const int s = f.exp + ei - kFftLog2N - 1;                 // 1/N of the inverse, 1/2 of the window overlap
        // The windowed sample fits int32 (|re * win| < 2^30), and so does its shift for -30 <= s <= 16: the common frames run
        // without 64-bit multiplies or shifts (2x cheaper on the S3); only the accumulation stays 64-bit, for the saturation.
        auto add = [this](int n, int64_t v) { int32_t &a = acc_[(opos_ + n) & (N - 1)]; a = sat32(static_cast<int64_t>(a) + v); };
#if SC_FFT_PIE
        if (s >= -30 && s <= 16) {                                 // the same arithmetic in PIE, over the ring's two runs (opos_ is a multiple of H)
            const int run = N - opos_;
            if (s >= 0) {
                pie_ola_shl(re_, kStftWin, acc_ + opos_, run, s);
                if (opos_) pie_ola_shl(re_ + run, kStftWin + run, acc_, opos_, s);
            } else {
                pie_ola_shr(re_, kStftWin, acc_ + opos_, run, -s);
                if (opos_) pie_ola_shr(re_ + run, kStftWin + run, acc_, opos_, -s);
            }
        } else
#endif
        if (s >= 0 && s <= 16) {
            for (int n = 0; n < N; n++) add(n, ((static_cast<int32_t>(re_[n]) * kStftWin[n] + (1 << 14)) >> 15) << s);
        } else if (s < 0 && s >= -30) {
            const int r = -s;
            const int32_t rnd = 1 << (r - 1);
            for (int n = 0; n < N; n++) add(n, (((static_cast<int32_t>(re_[n]) * kStftWin[n] + (1 << 14)) >> 15) + rnd) >> r);
        } else {
            for (int n = 0; n < N; n++) {
                int64_t v = (static_cast<int64_t>(re_[n]) * kStftWin[n] + (1 << 14)) >> 15;
                v = s >= 0 ? (v << s) : ((v + (1LL << (-s - 1))) >> -s);
                add(n, v);
            }
        }
        STFT_T(5);
        for (int j = 0; j < H; j++) {                              // the oldest H samples are complete
            const int idx = (opos_ + j) & (N - 1);
            fifo_[wr_] = sat16(acc_[idx]);
            wr_ = (wr_ + 1) & (kFifo - 1);
            acc_[idx] = 0;
        }
        count_ += H;
        opos_ = (opos_ + H) & (N - 1);
        STFT_T(6);
    }

    Heap *heap_ = nullptr;
    uint8_t *mem_ = nullptr;
    q15 *in_ = nullptr, *in2_ = nullptr, *fifo_ = nullptr;
    int16_t *re_ = nullptr, *im_ = nullptr;
    int32_t *acc_ = nullptr;
    int ipos_ = 0, opos_ = 0, hop_ = 0, rd_ = 0, wr_ = 0, count_ = 0;
};

}  // namespace sc
