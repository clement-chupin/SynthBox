// web_amy_stubs.c — Stubs for Emscripten threading primitives used by AMY.
// AMY calls emscripten_lock_* for its internal synchronisation. In a
// single-threaded (no-pthread) Emscripten build these symbols are absent
// from the standard libraries, so we provide trivial no-op implementations.
//
// emscripten_lock_t is volatile uint32_t (emscripten/threading.h).
// 0 = unlocked, 1 = locked.

#ifdef __EMSCRIPTEN__
#include <stdint.h>

// AMY expects this to return int (0 = acquired)
int emscripten_lock_busyspin_wait_acquire(volatile uint32_t *lock, double timeout_ms) {
    (void)timeout_ms;
    *lock = 1;
    return 0;
}

// AMY calls this as (i32)->void but wasm-ld warns on mismatch; match with void
void emscripten_lock_release(volatile uint32_t *lock) {
    *lock = 0;
}

// Also stub out any other threading helpers AMY may reference
void emscripten_lock_init(volatile uint32_t *lock) {
    *lock = 0;
}

int emscripten_lock_try_acquire(volatile uint32_t *lock) {
    *lock = 1;
    return 1; // always succeeds in single-threaded build
}
#endif
