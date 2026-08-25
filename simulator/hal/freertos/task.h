#pragma once
// FreeRTOS task API → pthreads

#include <pthread.h>
#include <unistd.h>
#include <stdint.h>

typedef void* TaskHandle_t;
typedef void (*TaskFunction_t)(void*);

// xTaskCreatePinnedToCore — ignore core affinity, use a pthread
static inline int xTaskCreatePinnedToCore(
    TaskFunction_t fn, const char* name, uint32_t stackSize,
    void* param, int priority, TaskHandle_t* handle, int core)
{
    (void)name; (void)stackSize; (void)priority; (void)core;
    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    // Each task argument is heap-allocated so the lambda captures correctly
    struct Args { TaskFunction_t fn; void* p; };
    Args* a = new Args{fn, param};
    int r = pthread_create(&tid, &attr, [](void* v) -> void* {
        Args* a = (Args*)v;
        a->fn(a->p);
        delete a;
        return nullptr;
    }, a);
    pthread_attr_destroy(&attr);
    if (handle) *handle = (void*)(uintptr_t)tid;
    return r == 0 ? 1 : 0;
}

static inline int xTaskCreate(
    TaskFunction_t fn, const char* name, uint32_t stack,
    void* param, int prio, TaskHandle_t* handle)
{
    return xTaskCreatePinnedToCore(fn, name, stack, param, prio, handle, 0);
}

static inline void vTaskDelay(uint32_t ticks) { usleep(ticks * 1000); }
static inline void vTaskDelayUntil(uint32_t* prev, uint32_t inc) {
    *prev += inc;
    usleep(inc * 1000);
}
static inline void vTaskDelete(TaskHandle_t) {}
static inline uint32_t xTaskGetTickCount() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}
static inline void taskYIELD() { sched_yield(); }
static inline void taskENTER_CRITICAL() {}
static inline void taskEXIT_CRITICAL() {}
