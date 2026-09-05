// sim_mp3dec.cpp — Helix MP3 API implementation (libmpg123 via dlopen on Linux/
// Android, bundled minimp3 on Windows — see the per-platform comments below).
//
// audio_engine.cpp uses the Helix streaming API (MP3InitDecoder / MP3Decode / etc.).
// On the real device this comes from chmorgan/esp-libhelix-mp3.
//
// API contract with audio_engine.cpp:
//   MP3Decode(dec, &ptr, &bytesLeft, frameBuf, 0):
//     - Advances ptr/bytesLeft by however many input bytes this call consumed
//     - Returns ERR_MP3_NONE(0) with decoded PCM in frameBuf + outputSamps set
//     - Returns ERR_MP3_INDATA_UNDERFLOW(-1) when the caller should refill from disk
//       before retrying, ERR_MP3_MAINDATA_UNDERFLOW(-2) when it should just retry at
//       the new (already-advanced) position without refilling
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
// already have installed. Instead we bundle minimp3 (single-header, public domain/
// CC0, https://github.com/lieff/minimp3) directly into the .exe and adapt its
// per-frame API to the same Helix-style contract audio_engine.cpp already speaks —
// no changes needed on the caller side, same as the Linux dlopen bridge below.
#define MINIMP3_IMPLEMENTATION
#include "hal/third_party/minimp3.h"

struct SimMP3State {
    mp3dec_t dec;
    int channels;
    int samprate;
    int outputSamps;  // interleaved sample count from the last successful decode
};

extern "C" HMP3Decoder MP3InitDecoder(void) {
    SimMP3State* s = new SimMP3State{};
    mp3dec_init(&s->dec);
    return (HMP3Decoder)s;
}
extern "C" void MP3FreeDecoder(HMP3Decoder hDec) { delete (SimMP3State*)hDec; }

extern "C" int MP3FindSyncWord(unsigned char* buf, int nBytes) {
    for (int i = 0; i < nBytes - 1; i++)
        if (buf[i] == 0xFF && (buf[i+1] & 0xE0) == 0xE0) return i;
    return -1;
}

extern "C" int MP3Decode(HMP3Decoder hDec, unsigned char** inbuf, int* bytesLeft,
                         short* outbuf, int /*useSize*/) {
    if (!hDec || !inbuf || !bytesLeft || !outbuf) return ERR_MP3_NULL_POINTER;
    if (*bytesLeft <= 0) return ERR_MP3_INDATA_UNDERFLOW;
    SimMP3State* s = (SimMP3State*)hDec;

    mp3dec_frame_info_t info = {};
    int samples = mp3dec_decode_frame(&s->dec, *inbuf, *bytesLeft, outbuf, &info);
    // frame_bytes is how many input bytes this call consumed — always advance past
    // them, even on failure: 0 valid frame found (need more data appended before
    // retrying) still reports the junk prefix it scanned past as frame_bytes.
    *inbuf     += info.frame_bytes;
    *bytesLeft -= info.frame_bytes;

    if (samples <= 0) {
        // frame_bytes==0: no frame header found anywhere in the window given — truly
        // need more bytes refilled before another attempt. frame_bytes>0: skipped
        // padding/junk (e.g. an ID3 tag) — already advanced past it, so the caller
        // should retry immediately at the new position rather than block on a refill.
        return info.frame_bytes == 0 ? ERR_MP3_INDATA_UNDERFLOW : ERR_MP3_MAINDATA_UNDERFLOW;
    }
    s->channels    = info.channels;
    s->samprate    = info.hz;
    s->outputSamps = samples * info.channels;
    return ERR_MP3_NONE;
}

extern "C" void MP3GetLastFrameInfo(HMP3Decoder hDec, MP3FrameInfo* info) {
    if (!hDec || !info) return;
    SimMP3State* s = (SimMP3State*)hDec;
    info->bitrate       = 128;
    info->nChans        = s->channels ? s->channels : 2;
    info->samprate      = s->samprate ? s->samprate : 44100;
    info->bitsPerSample = 16;
    info->outputSamps   = s->outputSamps;
    info->layer         = 3;
    info->version       = 0;
}

extern "C" int MP3GetNextFrameInfo(HMP3Decoder /*hDec*/, MP3FrameInfo* info, unsigned char* /*buf*/) {
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
