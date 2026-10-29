// web_tasks.cpp — see web_tasks.h for the full rationale.
#include "web_tasks.h"
#include <emscripten/fiber.h>
#include <stdlib.h>
#include <string.h>

#define MAX_WEB_TASKS 8
// ESP32 FreeRTOS stack sizes passed by main.cpp/audio_engine.cpp (2048-16384 bytes)
// are far too small for clang-compiled Wasm call frames (no register allocation
// across calls the way native code gets); give every task a generous, uniform
// native stack regardless of what was requested. The asyncify stack only needs to
// hold the unwind/rewind bookkeeping for however deep a call chain gets between
// fiber switches, not the real data — 64KB is comfortably more than this app's
// deepest observed call chain (note handling / OLED draw) needs.
#define WEB_TASK_C_STACK_SIZE        262144
#define WEB_TASK_ASYNCIFY_STACK_SIZE  65536

struct WebTask {
    emscripten_fiber_t fiber;
    char*    cStack;
    char*    asyncifyStack;
    WebTaskFn fn;
    void*    param;
    bool     started;
    bool     notified;
};

static WebTask  s_tasks[MAX_WEB_TASKS];
static int      s_taskCount = 0;
static WebTask* s_currentTask = nullptr;  // which task fiber is running right now (nullptr = scheduler/main)

// The "main" fiber represents the real call stack that's driving webTasksPump()
// (ultimately, the browser's requestAnimationFrame callback via
// emscripten_set_main_loop). We capture it once so task fibers can swap back into it.
static emscripten_fiber_t s_mainFiber;
static char s_mainAsyncifyStack[WEB_TASK_ASYNCIFY_STACK_SIZE];
static bool s_mainFiberReady = false;

static void webTaskTrampoline(void* arg) {
    WebTask* t = (WebTask*)arg;
    // fn() is one of main.cpp's/audio_engine.cpp's `for(;;) {...}` task bodies —
    // it never returns in practice. If it somehow did, park this fiber forever
    // rather than falling off the end of a fiber entry function (undefined).
    t->fn(t->param);
    for (;;) simTaskYield();
}

void* webTaskCreate(WebTaskFn fn, void* param, uint32_t /*stackSize*/) {
    if (s_taskCount >= MAX_WEB_TASKS) return nullptr;
    WebTask* t = &s_tasks[s_taskCount++];
    memset(t, 0, sizeof(*t));
    t->fn    = fn;
    t->param = param;
    t->cStack        = (char*)malloc(WEB_TASK_C_STACK_SIZE);
    t->asyncifyStack  = (char*)malloc(WEB_TASK_ASYNCIFY_STACK_SIZE);
    emscripten_fiber_init(&t->fiber, webTaskTrampoline, t,
                          t->cStack, WEB_TASK_C_STACK_SIZE,
                          t->asyncifyStack, WEB_TASK_ASYNCIFY_STACK_SIZE);
    t->started  = true;
    t->notified = false;
    return (void*)t;
}

void webTasksPump() {
    if (!s_mainFiberReady) {
        emscripten_fiber_init_from_current_context(&s_mainFiber,
            s_mainAsyncifyStack, sizeof(s_mainAsyncifyStack));
        s_mainFiberReady = true;
    }
    for (int i = 0; i < s_taskCount; i++) {
        WebTask* t = &s_tasks[i];
        WebTask* prev = s_currentTask;
        s_currentTask = t;
        emscripten_fiber_swap(&s_mainFiber, &t->fiber);
        s_currentTask = prev;
    }
}

void simTaskYield() {
    WebTask* t = s_currentTask;
    if (!t) return;  // called from outside any task fiber — nothing to yield from
    emscripten_fiber_swap(&t->fiber, &s_mainFiber);
}

void* webCurrentTask() {
    return (void*)s_currentTask;
}

void webTaskNotifyGive(void* task) {
    WebTask* t = (WebTask*)task;
    if (t) t->notified = true;
}

bool webTaskNotifyTake(void* task, bool clearOnExit) {
    WebTask* t = (WebTask*)task;
    if (!t) return false;
    bool was = t->notified;
    if (was && clearOnExit) t->notified = false;
    return was;
}
