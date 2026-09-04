#pragma once
// SD card simulator — maps SD paths to ~/Music/ using POSIX I/O

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <string>
#include "Arduino.h"
#include "SPI.h"
#ifdef __ANDROID__
#include <SDL2/SDL.h>
#endif

#define FILE_READ   "r"
#define FILE_WRITE  "w"
#define FILE_APPEND "a"
#define O_READ  1
#define O_WRITE 2
#define O_CREAT 4

static inline std::string simSdRoot() {
#ifdef __ANDROID__
    // HOME is never set for an Android app process, so the desktop fallback
    // below resolves to an undefined/likely-unwritable "./Music" here. Use the
    // app's own external files dir instead — always available, no permissions
    // needed. This is also where the SAF import (GrvActivity.java) copies
    // picked-folder files into, so imported files land exactly where the rest
    // of the app already expects its "SD card" to be.
    const char* ext = SDL_AndroidGetExternalStoragePath();
    if (ext && ext[0]) return std::string(ext);
#endif
    const char* home = getenv("HOME");
    return std::string(home ? home : ".") + "/Music";
}

static inline std::string sdAbsPath(const char* sdPath) {
    std::string root = simSdRoot();
    if (sdPath && sdPath[0] == '/') return root + sdPath;
    return root + "/" + (sdPath ? sdPath : "");
}

class File {
public:
    File() : _fp(nullptr), _dir(nullptr), _size(0), _isDir(false) {}

    // Directory entry
    File(const char* path, bool isDir, DIR* d)
        : _fp(nullptr), _dir(d), _size(0), _isDir(isDir), _path(path) {}

    // File entry
    File(const char* path, FILE* fp, size_t sz)
        : _fp(fp), _dir(nullptr), _size(sz), _isDir(false), _path(path) {}

    operator bool() const { return _isDir ? (_dir != nullptr) : (_fp != nullptr); }
    bool isDirectory() const { return _isDir; }
    const char* name() const { return _path.c_str(); }

    File openNextFile() {
        if (!_dir) return File();
        struct dirent* ent;
        while ((ent = readdir(_dir)) != nullptr) {
            if (ent->d_name[0] == '.') continue;
            std::string child = _path + "/" + ent->d_name;
            struct stat st;
            if (::stat(child.c_str(), &st) != 0) continue;
            if (S_ISDIR(st.st_mode)) {
                DIR* d = opendir(child.c_str());
                return File(child.c_str(), true, d);
            }
            FILE* fp = fopen(child.c_str(), "r");
            return File(child.c_str(), fp, (size_t)st.st_size);
        }
        return File();
    }

    int read() {
        if (!_fp) return -1;
        int c = fgetc(_fp);
        return c == EOF ? -1 : c;
    }
    size_t read(uint8_t* buf, size_t len) {
        if (!_fp) return 0;
        return fread(buf, 1, len, _fp);
    }
    size_t write(const uint8_t* buf, size_t len) {
        if (!_fp) return 0;
        return fwrite(buf, 1, len, _fp);
    }

    void close() {
        if (_fp)  { fclose(_fp);    _fp  = nullptr; }
        if (_dir) { closedir(_dir); _dir = nullptr; }
    }

    uint32_t size()     const { return (uint32_t)_size; }
    uint32_t position() const { return _fp ? (uint32_t)ftell(_fp) : 0; }
    bool seek(uint32_t pos)   { return _fp ? (fseek(_fp, pos, SEEK_SET) == 0) : false; }
    // Matches the real Arduino File::available() contract: remaining byte count (not a
    // boolean) — callers like the WAV/MP3 chunk parsers compare it against thresholds
    // (e.g. "> 8" to ensure a full chunk header is left to read).
    int available() {
        if (!_fp) return 0;
        long cur = ftell(_fp);
        long remain = (long)_size - cur;
        return remain > 0 ? (int)remain : 0;
    }

private:
    FILE*       _fp;
    DIR*        _dir;
    size_t      _size;
    bool        _isDir;
    std::string _path;
};

class SDClass {
public:
    bool begin(uint8_t, SPIClass&, uint32_t = 4000000) { return _init(); }
    bool begin(uint8_t) { return _init(); }

    File open(const char* path, const char* mode = FILE_READ) {
        std::string abs = sdAbsPath(path);
        struct stat st;
        if (::stat(abs.c_str(), &st) != 0) {
            // For write modes, try to create the file
            if (strcmp(mode, FILE_READ) != 0) {
                FILE* fp = fopen(abs.c_str(), mode);
                if (fp) return File(abs.c_str(), fp, 0);
            }
            return File();
        }
        if (S_ISDIR(st.st_mode)) {
            DIR* d = opendir(abs.c_str());
            if (!d) return File();
            return File(abs.c_str(), true, d);
        }
        FILE* fp = fopen(abs.c_str(), mode);
        if (!fp) return File();
        return File(abs.c_str(), fp, (size_t)st.st_size);
    }
    File open(const String& path, const char* mode = FILE_READ) {
        return open(path.c_str(), mode);
    }

    bool exists(const char* path) {
        struct stat st;
        return ::stat(sdAbsPath(path).c_str(), &st) == 0;
    }
    bool exists(const String& path) { return exists(path.c_str()); }

    bool mkdir(const char* path) {
#ifdef _WIN32
        return ::mkdir(sdAbsPath(path).c_str()) == 0;  // mingw's mkdir() takes no mode arg
#else
        return ::mkdir(sdAbsPath(path).c_str(), 0755) == 0;
#endif
    }
    bool remove(const char* path) {
        return ::remove(sdAbsPath(path).c_str()) == 0;
    }

    uint64_t totalBytes() const { return (uint64_t)16 * 1024 * 1024 * 1024; }
    uint64_t usedBytes()  const { return (uint64_t) 1 * 1024 * 1024 * 1024; }

private:
    bool _init() {
        std::string root = simSdRoot();
        struct stat st;
#ifdef _WIN32
        if (::stat(root.c_str(), &st) != 0) ::mkdir(root.c_str());
#else
        if (::stat(root.c_str(), &st) != 0) ::mkdir(root.c_str(), 0755);
#endif
        printf("[sim] SD card → %s\n", root.c_str());
        return true;
    }
};

extern SDClass SD;
