#pragma once
// FreeRTOS semaphore/mutex -> plain counter + cooperative-yield wait (web build).
// See queue.h's header comment — no real concurrency, no locking needed, only a
// yield point so a blocking wait doesn't freeze the single JS thread forever.

#include <stdint.h>
#include "task.h"  // xTaskGetTickCount, simTaskYield

struct SimSemaphore {
    int count;
    int max;
};

typedef SimSemaphore* SemaphoreHandle_t;

static inline SemaphoreHandle_t xSemaphoreCreateMutex() {
    SimSemaphore* s = new SimSemaphore;
    s->count = 1; s->max = 1;
    return s;
}

static inline SemaphoreHandle_t xSemaphoreCreateBinary() {
    SimSemaphore* s = new SimSemaphore;
    s->count = 0; s->max = 1;
    return s;
}

static inline SemaphoreHandle_t xSemaphoreCreateCounting(int maxCount, int initCount) {
    SimSemaphore* s = new SimSemaphore;
    s->count = initCount; s->max = maxCount;
    return s;
}

static inline int xSemaphoreTake(SemaphoreHandle_t s, uint32_t wait) {
    uint32_t start = xTaskGetTickCount();
    for (;;) {
        if (s->count > 0) { s->count--; return 1; }
        if (wait == 0) return 0;
        if (wait != 0xFFFFFFFFUL && (uint32_t)(xTaskGetTickCount() - start) >= wait) return 0;
        simTaskYield();
    }
}

static inline int xSemaphoreGive(SemaphoreHandle_t s) {
    if (s->count < s->max) s->count++;
    return 1;
}

static inline int xSemaphoreGiveFromISR(SemaphoreHandle_t s, int*) { return xSemaphoreGive(s); }
static inline int xSemaphoreTakeFromISR(SemaphoreHandle_t s, int*) { return xSemaphoreTake(s, 0); }

static inline void vSemaphoreDelete(SemaphoreHandle_t s) { delete s; }
