#pragma once
// web_tasks.h — cooperative "FreeRTOS task" scheduler for the single-threaded
// Emscripten build.
//
// The real ESP32 build and the SDL2 desktop simulator both run each FreeRTOS
// "task" (audioHandlerTask, ledUpdateTask, displayTask, bgServiceTask,
// djWorkerTask, ssWorkerTask — see src/main.cpp/src/audio_engine.cpp) as its own
// real thread (FreeRTOS task on hardware, a detached pthread in the simulator —
// see simulator/hal/freertos/task.h). Every one of those functions is written as
// an infinite `for(;;) { blocking-wait; do work; }` loop, which is exactly the
// wrong shape for a single JS thread with no real OS threads and no -pthread.
//
// Emscripten Fibers (emscripten/fiber.h) give each of these "tasks" its own real
// stack and a cooperative (not preemptive) context switch: a task runs until it
// hits one of our FreeRTOS-shaped blocking primitives (xQueueReceive/
// xSemaphoreTake/ulTaskNotifyTake/vTaskDelay, all reimplemented in
// hal/freertos/{queue,semphr,task}.h to call simTaskYield() instead of a real
// pthread condvar wait), at which point it swaps back out to whichever fiber
// called it in (web_main.cpp's per-frame simTasksPump(), itself called once per
// browser animation frame from the emscripten_set_main_loop callback). Each task
// therefore gets one "turn" per frame — close enough to the ESP32's real
// concurrency for a UI/audio-control app like this one, and exactly the pattern
// Emscripten's own fiber API exists for. Fibers require -sASYNCIFY (already
// mandated by structure/WEB_SIMULATOR.md for miniaudio's web-audio callback), so
// no extra build flag is needed for this.
//
// src/main.cpp and src/audio_engine.cpp are never modified for this — this file
// (plus hal/freertos/*) is the only place that knows tasks are fibers rather than
// real threads.

#include <stdint.h>

typedef void (*WebTaskFn)(void*);

// Registers a new cooperative task fiber. Returns a stable "handle" (actually a
// pointer to our internal task record) for use with xTaskNotifyGive/vTaskDelete,
// or nullptr if the task table is full (MAX_WEB_TASKS in web_tasks.cpp).
void* webTaskCreate(WebTaskFn fn, void* param, uint32_t stackSize);

// Called once per browser animation frame (from web_main.cpp's main loop tick,
// before or after the real loop()) — gives every registered task fiber exactly
// one turn: swap in, run until it yields (via simTaskYield(), called from inside
// our FreeRTOS blocking primitives) or the task function returns (never happens
// in practice — these are all `for(;;)` loops), then swap back out.
void webTasksPump();

// Called FROM INSIDE a running task fiber (by hal/freertos/{queue,semphr,task}.h)
// to hand control back to webTasksPump() for this frame. A no-op if called from
// outside any task fiber (e.g. accidentally from the main loop/loop() itself).
void simTaskYield();

// true if the calling code is currently running inside a task fiber (vs. the
// main/scheduler context) — used by ulTaskNotifyTake to find "its own" task record.
void* webCurrentTask();

// Sets/clears/reads the one-shot notification flag used by xTaskNotifyGive/
// ulTaskNotifyTake. `task` is the handle returned by webTaskCreate().
void webTaskNotifyGive(void* task);
bool webTaskNotifyTake(void* task, bool clearOnExit);
