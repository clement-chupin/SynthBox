// sim_mp3dec.cpp — Helix MP3 API implementation using libmpg123 (via dlopen)
//
// audio_engine.cpp uses the Helix streaming API (MP3InitDecoder / MP3Decode / etc.).
// On the real device this comes from chmorgan/esp-libhelix-mp3.
// In the simulator we bridge to libmpg123.so.0 which is available on most Linux systems.
//
// API contract with audio_engine.cpp:
//   MP3Decode(dec, &ptr, &bytesLeft, frameBuf, 0):
//     - Feeds ALL remaining bytes to mpg123 (sets bytesLeft=0 so caller refills next time)
//     - Returns ERR_MP3_NONE(0) with decoded PCM in frameBuf + outputSamps set
//     - Returns ERR_MP3_MAINDATA_UNDERFLOW(-2) when more input needed (loop continues,
//       caller's bytesLeft<MAINBUF_SIZE triggers refill, then we feed again)
//   MP3GetLastFrameInfo: returns sample-rate/channels/outputSamps from last decode

#include "hal/mp3dec.h"
#ifndef _WIN32
#include <dlfcn.h>
#endif
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef _WIN32
// Windows build: no dlopen()/libmpg123.so equivalent to reach for here — unlike
// Linux, there's no system library a plain end-user's machine can be expected to
// already have installed, and statically bundling a full MP3 decoder is a separate
// undertaking. MP3 sample playback is therefore unavailable in the Windows build for
// now (WAV samples, and everything else, are unaffected); all the Helix API entry
// points below just report "no decoder" instead of decoding.
extern "C" HMP3Decoder MP3InitDecoder(void) { return nullptr; }
extern "C" void MP3FreeDecoder(HMP3Decoder) {}
extern "C" int MP3FindSyncWord(unsigned char* buf, int nBytes) {
    for (int i = 0; i < nBytes - 1; i++)
        if (buf[i] == 0xFF && (buf[i+1] & 0xE0) == 0xE0) return i;
    return -1;
}
extern "C" int MP3Decode(HMP3Decoder, unsigned char**, int*, short*, int) { return ERR_MP3_NULL_POINTER; }
extern "C" void MP3GetLastFrameInfo(HMP3Decoder, MP3FrameInfo* info) { if (info) *info = {}; }
extern "C" int MP3GetNextFrameInfo(HMP3Decoder, MP3FrameInfo* info, unsigned char*) {
    if (info) *info = {};
    return ERR_MP3_INDATA_UNDERFLOW;
}
#else

// ---- mpg123 types & constants (from mpg123.h — declared manually to avoid dep) ----
typedef struct mpg123_handle_struct mpg123_handle_t;
#define MPG123_OK           0
#define MPG123_NEED_MORE   10
#define MPG123_NEW_FORMAT  11

typedef mpg123_handle_t* (*pfn_new)      (const char*, int*);
typedef int              (*pfn_open_feed)(mpg123_handle_t*);
typedef int              (*pfn_feed)     (mpg123_handle_t*, const unsigned char*, size_t);
typedef int              (*pfn_read)     (mpg123_handle_t*, unsigned char*, size_t, size_t*);
typedef int              (*pfn_getformat)(mpg123_handle_t*, long*, int*, int*);
typedef void             (*pfn_delete)   (mpg123_handle_t*);
typedef int              (*pfn_init)     (void);

static pfn_new       g_new;
static pfn_open_feed g_open_feed;
static pfn_feed      g_feed;
static pfn_read      g_read;
static pfn_getformat g_getformat;
static pfn_delete    g_delete;
static bool          g_loaded;

static bool ensureMpg123() {
    if (g_loaded) return g_new != nullptr;
    g_loaded = true;
    void* lib = dlopen("libmpg123.so.0", RTLD_LAZY | RTLD_GLOBAL);
    if (!lib) { fprintf(stderr, "[sim] MP3: dlopen libmpg123.so.0 failed: %s\n", dlerror()); return false; }
    g_new       = (pfn_new)      dlsym(lib, "mpg123_new");
    g_open_feed = (pfn_open_feed)dlsym(lib, "mpg123_open_feed");
    g_feed      = (pfn_feed)     dlsym(lib, "mpg123_feed");
    g_read      = (pfn_read)     dlsym(lib, "mpg123_read");
    g_getformat = (pfn_getformat)dlsym(lib, "mpg123_getformat");
    g_delete    = (pfn_delete)   dlsym(lib, "mpg123_delete");
    // mpg123_init is deprecated in libmpg123 >= 1.27 but harmless to call
    auto init_fn = (pfn_init)dlsym(lib, "mpg123_init");
    if (init_fn) init_fn();
    return g_new != nullptr;
}

// ---- Per-decoder state ----
struct SimMP3State {
    mpg123_handle_t* mh;
    long sampleRate;
    int  channels;
    int  outputSamps;   // int16_t samples in last decoded chunk (may be stereo)
    bool gotFormat;
};

// ---- Helix API ----

extern "C" HMP3Decoder MP3InitDecoder(void) {
    if (!ensureMpg123()) return nullptr;
    SimMP3State* s = new SimMP3State{};
    s->mh = g_new(nullptr, nullptr);
    if (!s->mh) { delete s; return nullptr; }
    g_open_feed(s->mh);
    return (HMP3Decoder)s;
}

extern "C" void MP3FreeDecoder(HMP3Decoder hDec) {
    if (!hDec) return;
    SimMP3State* s = (SimMP3State*)hDec;
    if (s->mh && g_delete) g_delete(s->mh);
    delete s;
}

// Scan for MP3 sync word (0xFF followed by 0xE0..0xFF)
extern "C" int MP3FindSyncWord(unsigned char* buf, int nBytes) {
    for (int i = 0; i < nBytes - 1; i++)
        if (buf[i] == 0xFF && (buf[i+1] & 0xE0) == 0xE0) return i;
    return -1;
}

extern "C" int MP3Decode(HMP3Decoder hDec, unsigned char** inbuf, int* bytesLeft,
                         short* outbuf, int /*useSize*/) {
    if (!hDec || !inbuf || !bytesLeft || !outbuf) return ERR_MP3_NULL_POINTER;
    SimMP3State* s = (SimMP3State*)hDec;

    // Feed all available bytes into mpg123's internal buffer.
    // Setting bytesLeft=0 ensures the caller's refill() triggers next iteration.
    if (*bytesLeft > 0) {
        g_feed(s->mh, *inbuf, (size_t)*bytesLeft);
        *inbuf    += *bytesLeft;
        *bytesLeft = 0;
    }

    // Try to read one MPEG-frame worth of PCM.
    // MAX_NGRAN * MAX_NCHAN * MAX_NSAMP int16_t = 2*2*576*2 = 4608 bytes (one stereo frame max)
    const size_t readSz = (size_t)(MAX_NGRAN * MAX_NCHAN * MAX_NSAMP) * sizeof(short);
    size_t done = 0;
    int ret = g_read(s->mh, (unsigned char*)outbuf, readSz, &done);

    if (ret == MPG123_NEW_FORMAT) {
        int enc;
        g_getformat(s->mh, &s->sampleRate, &s->channels, &enc);
        s->gotFormat = true;
        // Retry read after format negotiation
        ret = g_read(s->mh, (unsigned char*)outbuf, readSz, &done);
    }

    if ((ret == MPG123_OK || ret == MPG123_NEW_FORMAT) && done > 0) {
        if (!s->gotFormat) {
            int enc;
            g_getformat(s->mh, &s->sampleRate, &s->channels, &enc);
            s->gotFormat = true;
        }
        s->outputSamps = (int)(done / sizeof(short));
        return ERR_MP3_NONE;
    }

    // Need more input — caller will see bytesLeft==0, trigger refill, feed again.
    return ERR_MP3_MAINDATA_UNDERFLOW;
}

extern "C" void MP3GetLastFrameInfo(HMP3Decoder hDec, MP3FrameInfo* info) {
    if (!hDec || !info) return;
    SimMP3State* s = (SimMP3State*)hDec;
    info->bitrate       = 128;
    info->nChans        = s->channels   ? s->channels       : 2;
    info->samprate      = s->sampleRate ? (int)s->sampleRate : 44100;
    info->bitsPerSample = 16;
    info->outputSamps   = s->outputSamps;
    info->layer         = 3;
    info->version       = 0;
}

extern "C" int MP3GetNextFrameInfo(HMP3Decoder /*hDec*/, MP3FrameInfo* info, unsigned char* /*buf*/) {
    if (info) *info = {};
    return ERR_MP3_INDATA_UNDERFLOW;
}
#endif // _WIN32
