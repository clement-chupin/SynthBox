#pragma once
#include <Arduino.h>

// WiFi Access Point + WebSocket audio receiver + software DSP chain.
//
// Phone connects to "GrvEP" WiFi, opens 192.168.4.1 in browser,
// clicks Start: page captures mic at 16 kHz and streams Int16 PCM via WebSocket.
// ESP32-S3 buffers incoming audio, applies DSP effects, outputs via AMY PCM streaming.
//
// Latency: ~300 ms (jitter buffer) + WiFi RTT (~10 ms on local AP).
// Audio format: mono, 16-bit LE PCM, 16 000 Hz.

void wifiAudioInit();   // call once in setup()
void wifiAudioStart();  // called on MODE_WIFI entry
void wifiAudioStop();   // called on MODE_WIFI exit
void wifiAudioTick();   // call every loop iteration (handles WebSocket accept/read)

bool     wifiAudioClientConnected();
uint32_t wifiAudioRingUsed();          // samples buffered (max = 32768)
int      wifiAudioGetLevel();          // peak level 0-127 for VU display
int      wifiAudioGetStationNum();     // number of WiFi stations connected to AP

// Effect controls — map FX overlay pots (pots[3..6]) to these:
void wifiAudioSetVolume(float vol);                    // 0-2, default 1
void wifiAudioSetLPF(float cutoffNorm, float reso);    // cutoff 0=200Hz 1=8kHz, reso 0-4
void wifiAudioSetDrive(float drive);                   // 0=clean 1=hard clip
void wifiAudioSetDelay(float level, float timeFrac);   // level 0-1, timeFrac 0-1 (0-1s)
