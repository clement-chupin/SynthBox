#pragma once
// FreeRTOS -> cooperative-fiber bridge for the GrvEP web (Emscripten) build.
// See web/web_tasks.h for why this is fibers rather than the simulator's pthreads.

#include <stdint.h>

#define configTICK_RATE_HZ 1000
#define portMAX_DELAY      0xFFFFFFFFUL
#define pdMS_TO_TICKS(ms)  ((uint32_t)(ms))
#define portTICK_PERIOD_MS 1
#define pdTRUE  1
#define pdFALSE 0
#define pdPASS  1
#define pdFAIL  0

typedef uint32_t TickType_t;
typedef int      BaseType_t;
typedef uint32_t UBaseType_t;

// Include the remaining FreeRTOS sub-headers
#include "task.h"
#include "queue.h"
#include "semphr.h"
