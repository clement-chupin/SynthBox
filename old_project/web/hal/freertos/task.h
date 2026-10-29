#pragma once
// FreeRTOS task API -> cooperative fibers (see web/web_tasks.h) for the web build.

#include <stdint.h>
#include <time.h>
#include "../../web_tasks.h"

typedef void* TaskHandle_t;
typedef void (*TaskFunction_t)(void*);

static inline uint32_t xTaskGetTickCount() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

// xTaskCreatePinnedToCore — core affinity and priority don't mean anything in a
// single-threaded cooperative scheduler; register a fiber task instead of a thread.
static inline int xTaskCreatePinnedToCore(
    TaskFunction_t fn, const char* name, uint32_t stackSize,
    void* param, int priority, TaskHandle_t* handle, int core)
{
    (void)name; (void)priority; (void)core;
    void* t = webTaskCreate(fn, param, stackSize);
    if (handle) *handle = t;
    return t ? 1 : 0;
}

static inline int xTaskCreate(
    TaskFunction_t fn, const char* name, uint32_t stack,
    void* param, int prio, TaskHandle_t* handle)
{
    return xTaskCreatePinnedToCore(fn, name, stack, param, prio, handle, 0);
}

// Blocking delay -> cooperative busy-yield until the requested time has passed.
// Every iteration hands control back to webTasksPump() (one video frame), so other
// tasks and the browser's own event loop keep running while this "sleeps".
static inline void vTaskDelay(uint32_t ticks) {
    uint32_t start = xTaskGetTickCount();
    while ((uint32_t)(xTaskGetTickCount() - start) < ticks) simTaskYield();
}
static inline void vTaskDelayUntil(uint32_t* prev, uint32_t inc) {
    *prev += inc;
    uint32_t now = xTaskGetTickCount();
    while ((int32_t)(*prev - now) > 0) { simTaskYield(); now = xTaskGetTickCount(); }
}
static inline void vTaskDelete(TaskHandle_t) {}
static inline void taskYIELD() { simTaskYield(); }

// portMUX_TYPE/taskENTER_CRITICAL/taskEXIT_CRITICAL: real mutual exclusion isn't
// needed here — only one fiber ever actually executes at a time, and a fiber only
// yields at our own explicit blocking calls (never mid-critical-section), so plain
// no-ops are already correct (stronger than the simulator's real pthread mutex,
// which guards against genuine concurrent OS threads that don't exist here).
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
static inline void taskENTER_CRITICAL(portMUX_TYPE*) {}
static inline void taskEXIT_CRITICAL(portMUX_TYPE*) {}

// Task notifications, backed by web_tasks.cpp's per-task one-shot flag.
static inline void xTaskNotifyGive(TaskHandle_t h) { webTaskNotifyGive(h); }
static inline uint32_t ulTaskNotifyTake(int clearOnExit, uint32_t wait) {
    uint32_t start = xTaskGetTickCount();
    void* self = webCurrentTask();
    for (;;) {
        if (webTaskNotifyTake(self, clearOnExit != 0)) return 1;
        if (wait != 0xFFFFFFFFUL && (uint32_t)(xTaskGetTickCount() - start) >= wait) return 0;
        simTaskYield();
    }
}
