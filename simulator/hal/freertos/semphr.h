#pragma once
// FreeRTOS semaphore/mutex → pthreads

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

struct SimSemaphore {
    pthread_mutex_t mu;
    pthread_cond_t  cond;
    int count;
    int max;
};

typedef SimSemaphore* SemaphoreHandle_t;

static inline SemaphoreHandle_t xSemaphoreCreateMutex() {
    SimSemaphore* s = new SimSemaphore;
    pthread_mutex_init(&s->mu, nullptr);
    pthread_cond_init(&s->cond, nullptr);
    s->count = 1; s->max = 1;
    return s;
}

static inline SemaphoreHandle_t xSemaphoreCreateBinary() {
    SimSemaphore* s = new SimSemaphore;
    pthread_mutex_init(&s->mu, nullptr);
    pthread_cond_init(&s->cond, nullptr);
    s->count = 0; s->max = 1;
    return s;
}

static inline SemaphoreHandle_t xSemaphoreCreateCounting(int maxCount, int initCount) {
    SimSemaphore* s = new SimSemaphore;
    pthread_mutex_init(&s->mu, nullptr);
    pthread_cond_init(&s->cond, nullptr);
    s->count = initCount; s->max = maxCount;
    return s;
}

static inline int xSemaphoreTake(SemaphoreHandle_t s, uint32_t wait) {
    pthread_mutex_lock(&s->mu);
    if (wait == 0xFFFFFFFFUL) {
        while (s->count <= 0) pthread_cond_wait(&s->cond, &s->mu);
    } else if (s->count <= 0) {
        if (wait > 0) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += (long)wait * 1000000;
            ts.tv_sec  += ts.tv_nsec / 1000000000;
            ts.tv_nsec %= 1000000000;
            pthread_cond_timedwait(&s->cond, &s->mu, &ts);
        }
        if (s->count <= 0) { pthread_mutex_unlock(&s->mu); return 0; }
    }
    s->count--;
    pthread_mutex_unlock(&s->mu);
    return 1;
}

static inline int xSemaphoreGive(SemaphoreHandle_t s) {
    pthread_mutex_lock(&s->mu);
    if (s->count < s->max) {
        s->count++;
        pthread_cond_signal(&s->cond);
    }
    pthread_mutex_unlock(&s->mu);
    return 1;
}

static inline int xSemaphoreGiveFromISR(SemaphoreHandle_t s, int*) {
    return xSemaphoreGive(s);
}
static inline int xSemaphoreTakeFromISR(SemaphoreHandle_t s, int*) {
    return xSemaphoreTake(s, 0);
}

static inline void vSemaphoreDelete(SemaphoreHandle_t s) {
    if (!s) return;
    pthread_mutex_destroy(&s->mu);
    pthread_cond_destroy(&s->cond);
    delete s;
}
