#pragma once
// FreeRTOS queue → mutex + condvar + circular buffer

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

struct SimQueue {
    uint8_t*   buf;
    size_t     itemSize;
    size_t     capacity;
    size_t     head;
    size_t     tail;
    size_t     count;
    pthread_mutex_t mu;
    pthread_cond_t  cond;
};

typedef SimQueue* QueueHandle_t;
#define portMAX_DELAY 0xFFFFFFFFUL

static inline QueueHandle_t xQueueCreate(size_t length, size_t itemSize) {
    SimQueue* q = new SimQueue;
    q->buf = (uint8_t*)malloc(length * itemSize);
    q->itemSize = itemSize;
    q->capacity = length;
    q->head = q->tail = q->count = 0;
    pthread_mutex_init(&q->mu, nullptr);
    pthread_cond_init(&q->cond, nullptr);
    return q;
}

static inline int xQueueSend(QueueHandle_t q, const void* item, uint32_t wait) {
    (void)wait;
    pthread_mutex_lock(&q->mu);
    bool ok = false;
    if (q->count < q->capacity) {
        memcpy(q->buf + q->tail * q->itemSize, item, q->itemSize);
        q->tail = (q->tail + 1) % q->capacity;
        q->count++;
        ok = true;
        pthread_cond_signal(&q->cond);
    }
    pthread_mutex_unlock(&q->mu);
    return ok ? 1 : 0;
}

static inline int xQueueSendToFront(QueueHandle_t q, const void* item, uint32_t wait) {
    (void)wait;
    pthread_mutex_lock(&q->mu);
    bool ok = false;
    if (q->count < q->capacity) {
        q->head = (q->head + q->capacity - 1) % q->capacity;
        memcpy(q->buf + q->head * q->itemSize, item, q->itemSize);
        q->count++;
        ok = true;
        pthread_cond_signal(&q->cond);
    }
    pthread_mutex_unlock(&q->mu);
    return ok ? 1 : 0;
}

static inline int xQueueReceive(QueueHandle_t q, void* item, uint32_t wait) {
    pthread_mutex_lock(&q->mu);
    if (wait == portMAX_DELAY) {
        while (q->count == 0)
            pthread_cond_wait(&q->cond, &q->mu);
    } else if (q->count == 0) {
        if (wait > 0) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += (long)wait * 1000000;
            ts.tv_sec  += ts.tv_nsec / 1000000000;
            ts.tv_nsec %= 1000000000;
            pthread_cond_timedwait(&q->cond, &q->mu, &ts);
        }
        if (q->count == 0) {
            pthread_mutex_unlock(&q->mu);
            return 0;
        }
    }
    memcpy(item, q->buf + q->head * q->itemSize, q->itemSize);
    q->head = (q->head + 1) % q->capacity;
    q->count--;
    pthread_mutex_unlock(&q->mu);
    return 1;
}

static inline int xQueueReceiveFromISR(QueueHandle_t q, void* item, int* wake) {
    if (wake) *wake = 0;
    return xQueueReceive(q, item, 0);
}

static inline size_t uxQueueMessagesWaiting(QueueHandle_t q) {
    pthread_mutex_lock(&q->mu);
    size_t n = q->count;
    pthread_mutex_unlock(&q->mu);
    return n;
}

static inline size_t uxQueueSpacesAvailable(QueueHandle_t q) {
    pthread_mutex_lock(&q->mu);
    size_t n = q->capacity - q->count;
    pthread_mutex_unlock(&q->mu);
    return n;
}

static inline void vQueueDelete(QueueHandle_t q) {
    if (!q) return;
    free(q->buf);
    pthread_mutex_destroy(&q->mu);
    pthread_cond_destroy(&q->cond);
    delete q;
}

static inline int xQueueSendToBack(QueueHandle_t q, const void* item, uint32_t wait) {
    return xQueueSend(q, item, wait);
}

static inline int xQueueReset(QueueHandle_t q) {
    pthread_mutex_lock(&q->mu);
    q->head = q->tail = q->count = 0;
    pthread_mutex_unlock(&q->mu);
    return 1;
}
