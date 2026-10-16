#pragma once
// SD card stub for the GrvEP web (Emscripten) build.
//
// Unlike the desktop/Android simulator (simulator/hal/SD.h), which maps SD paths onto
// a real POSIX directory (~/Music), a browser has no filesystem to speak of — Emscripten
// does provide an in-memory MEMFS mounted at "/", but mounting it here would only ever
// produce empty directories, which is functionally identical to "no card present" while
// adding real code (getenv/mkdir/stat) that has nothing to do. So SD.begin() always
// returns false here: sdReady (src/main.cpp) stays false, exactly like a real unit
// with no card inserted, which the rest of the app already handles gracefully —
// SAMPLE/GRANU/SS2's SD browser modes display but list zero files (see
// structure/WEB_SIMULATOR.md's own "Pas d'accès SD" limitation) instead of crashing.

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <string>
#include "Arduino.h"
#include "SPI.h"

#define FILE_READ   "rb"
#define FILE_WRITE  "wb"
#define FILE_APPEND "ab"
#define O_READ  1
#define O_WRITE 2
#define O_CREAT 4

class File {
public:
    File() {}
    operator bool() const { return false; }
    bool isDirectory() const { return false; }
    const char* name() const { return ""; }
    File openNextFile() { return File(); }
    int read() { return -1; }
    size_t read(uint8_t*, size_t) { return 0; }
    size_t write(const uint8_t*, size_t) { return 0; }
    int getWriteError() const { return 0; }
    void close() {}
    uint32_t size()     const { return 0; }
    uint32_t position() const { return 0; }
    bool seek(uint32_t) { return false; }
    int available() { return 0; }
};

class SDClass {
public:
    bool begin(uint8_t, SPIClass&, uint32_t = 4000000) { return _init(); }
    bool begin(uint8_t) { return _init(); }
    void end() {}

    File open(const char*, const char* = FILE_READ) { return File(); }
    File open(const String&, const char* = FILE_READ) { return File(); }

    bool exists(const char*) { return false; }
    bool exists(const String&) { return false; }

    bool mkdir(const char*) { return false; }
    bool remove(const char*) { return false; }
    bool rename(const char*, const char*) { return false; }
    bool rename(const String&, const String&) { return false; }

    uint64_t totalBytes() const { return 0; }
    uint64_t usedBytes()  const { return 0; }

private:
    bool _init() {
        // No card, ever — see file header comment.
        printf("[web] SD card -> none (browser build has no filesystem access)\n");
        return false;
    }
};

extern SDClass SD;
