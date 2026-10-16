#include "engine/sampler/capture.h"

namespace sc {

CaptureTap &capture_tap() {
    static CaptureTap tap;
    return tap;
}

void CaptureTap::arm(q15 threshold, bool at_bar, uint32_t limit) {
    if (!buf_ || state() != Idle) return;
    threshold_ = threshold;
    at_bar_ = at_bar;
    limit_ = limit;
    w_.store(0); r_.store(0);
    time_.store(0); dropped_.store(0);
    state_.store(Armed, std::memory_order_release);
}

void CaptureTap::stop() {
    int s = state_.load();
    while ((s == Armed || s == Recording) && !state_.compare_exchange_weak(s, Done)) {}
}

void CaptureTap::reset() {
    state_.store(Idle, std::memory_order_release);
    w_.store(0); r_.store(0);
}

void CaptureTap::feed(const q15 *l, const q15 *r, int n, const Transport *t) {
    int s = state_.load(std::memory_order_acquire);
    if (s != Armed && s != Recording) return;
    int from = 0;
    if (s == Armed) {
        if (at_bar_ && t && t->running) {                                     // the bar line at or after this block's start
            const uint64_t bar = 4ull << 32;
            from = t->sample_at((t->beat_q32 + bar - 1) / bar * bar, n);
            if (from >= n) return;
        } else if (threshold_ > 0) {
            int32_t peak = 0;
            for (int i = 0; i < n; i++) {
                const int32_t a = l[i] < 0 ? -l[i] : l[i], b = r[i] < 0 ? -r[i] : r[i];
                if (a > peak) peak = a;
                if (b > peak) peak = b;
            }
            if (peak < threshold_) return;
        }
        if (!state_.compare_exchange_strong(s, Recording)) return;           // (stopped meanwhile)
    }
    uint32_t k = static_cast<uint32_t>(n - from);
    const uint32_t done = time_.load(std::memory_order_relaxed);
    if (limit_ && done + k > limit_) k = limit_ - done;
    const uint32_t w = w_.load(std::memory_order_relaxed), room = mask_ + 1 - (w - r_.load(std::memory_order_acquire));
    const uint32_t fit = k < room ? k : room;
    for (uint32_t i = 0; i < fit; i++) buf_[(w + i) & mask_] = static_cast<q15>((static_cast<int32_t>(l[from + i]) + r[from + i]) >> 1);
    w_.store(w + fit, std::memory_order_release);
    if (fit < k) dropped_.fetch_add(k - fit, std::memory_order_relaxed);
    time_.store(done + k, std::memory_order_relaxed);
    if (limit_ && done + k >= limit_) { int rec = Recording; state_.compare_exchange_strong(rec, Done); }
}

uint32_t CaptureTap::take(q15 *dst, uint32_t max) {
    const uint32_t r = r_.load(std::memory_order_relaxed), have = w_.load(std::memory_order_acquire) - r;
    const uint32_t k = have < max ? have : max;
    for (uint32_t i = 0; i < k; i++) dst[i] = buf_[(r + i) & mask_];
    r_.store(r + k, std::memory_order_release);
    return k;
}

}  // namespace sc
