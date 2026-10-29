#pragma once
// FreeRTOS queue -> plain circular buffer + cooperative-yield wait (web build).
// No real concurrency exists in this single-threaded build, so the buffer needs
// no locking — only a yield point for callers that pass a nonzero/portMAX_DELAY
// wait, so other task fibers and the browser keep running while this "blocks".

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "task.h"  // xTaskGetTickCount, simTaskYield (via web_tasks.h)

struct SimQueue {
    uint8_t* buf;
    size_t   itemSize;
    size_t   capacity;
    size_t   head;
    size_t   tail;
    size_t   count;
};

typedef SimQueue* QueueHandle_t;
#define portMAX_DELAY 0xFFFFFFFFUL

static inline QueueHandle_t xQueueCreate(size_t length, size_t itemSize) {
    SimQueue* q = new SimQueue;
    q->buf = (uint8_t*)malloc(length * itemSize);
    q->itemSize = itemSize;
    q->capacity = length;
    q->head = q->tail = q->count = 0;
    return q;
}

static inline int xQueueSend(QueueHandle_t q, const void* item, uint32_t /*wait*/) {
    if (q->count >= q->capacity) return 0;
    memcpy(q->buf + q->tail * q->itemSize, item, q->itemSize);
    q->tail = (q->tail + 1) % q->capacity;
    q->count++;
    return 1;
}

static inline int xQueueSendToFront(QueueHandle_t q, const void* item, uint32_t /*wait*/) {
    if (q->count >= q->capacity) return 0;
    q->head = (q->head + q->capacity - 1) % q->capacity;
    memcpy(q->buf + q->head * q->itemSize, item, q->itemSize);
    q->count++;
    return 1;
}

static inline int xQueueReceive(QueueHandle_t q, void* item, uint32_t wait) {
    uint32_t start = xTaskGetTickCount();
    for (;;) {
        if (q->count > 0) {
            memcpy(item, q->buf + q->head * q->itemSize, q->itemSize);
            q->head = (q->head + 1) % q->capacity;
            q->count--;
            return 1;
        }
        if (wait == 0) return 0;
        if (wait != portMAX_DELAY && (uint32_t)(xTaskGetTickCount() - start) >= wait) return 0;
        simTaskYield();
    }
}

static inline int xQueueReceiveFromISR(QueueHandle_t q, void* item, int* wake) {
    if (wake) *wake = 0;
    return xQueueReceive(q, item, 0);
}

static inline size_t uxQueueMessagesWaiting(QueueHandle_t q) { return q->count; }
static inline size_t uxQueueSpacesAvailable(QueueHandle_t q) { return q->capacity - q->count; }

static inline void vQueueDelete(QueueHandle_t q) {
    if (!q) return;
    free(q->buf);
    delete q;
}

static inline int xQueueSendToBack(QueueHandle_t q, const void* item, uint32_t wait) {
    return xQueueSend(q, item, wait);
}

static inline int xQueueReset(QueueHandle_t q) {
    q->head = q->tail = q->count = 0;
    return 1;
}
