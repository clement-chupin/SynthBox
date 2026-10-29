// web_amy_stubs.c — stubs for AMY's emscripten_lock_* calls.
//
// lib/AMY Synthesizer/src/amy.c has a dedicated __EMSCRIPTEN__ branch for its
// cross-core render-queue mutex (amy_grab_lock/amy_release_lock/amy_init_lock)
// that's built on <emscripten/threading.h>'s emscripten_lock_t — a real spinlock
// meant for an actual multi-threaded (-pthread, SharedArrayBuffer) Emscripten
// build. This web build is intentionally single-threaded (no -pthread, so no
// COOP/COEP headers are needed to host it on GitHub Pages — see
// structure/WEB_SIMULATOR.md), so emscripten_lock_busyspin_wait_acquire()/
// emscripten_lock_release() have no real multi-threaded implementation linked in.
// Since only one thread of execution ever exists here, the lock can never
// actually be contended — these stubs are always-free, always-succeed no-ops.
//
// amy.c is never modified for this (the single vendored AMY copy, shared with
// the ESP32/simulator builds, per the project's own CLAUDE.md) — this is the
// one new, web-only file the project's design doc calls for instead.

#include <stdbool.h>
#include <emscripten/threading.h>

bool emscripten_lock_busyspin_wait_acquire(emscripten_lock_t* lock, double maxWaitMs) {
    (void)lock;
    (void)maxWaitMs;
    // Single-threaded: the lock is always free. Nothing to wait for, always "acquired".
    return true;
}

void emscripten_lock_release(emscripten_lock_t* lock) {
    (void)lock;
}
