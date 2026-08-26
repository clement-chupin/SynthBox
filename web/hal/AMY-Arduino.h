#pragma once
// AMY-Arduino wrapper for GrvEP simulator

// Pull in FreeRTOS stubs before amy.h (audio_engine.cpp uses xQueueCreate etc.)
#include "freertos/FreeRTOS.h"

// Wrap amy.h in extern "C" so that audio_engine.cpp's "extern "C" pcm_load" doesn't conflict
#ifdef __cplusplus
extern "C" {
#endif
#include "/home/cchupin/projects/other_projects/amy/src/amy.h"
#ifdef __cplusplus
}
#endif
