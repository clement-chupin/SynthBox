// web_mp3dec.cpp — No-op Helix MP3 API stubs for WASM build.
// The WASM build has no filesystem access to MP3 files, so these return errors.
// WAV playback still works via audio_engine.cpp's direct PCM path.
#include "hal/mp3dec.h"
#include <stdlib.h>

extern "C" {

HMP3Decoder MP3InitDecoder(void) { return malloc(1); }

void MP3FreeDecoder(HMP3Decoder hDec) { if(hDec) free(hDec); }

int MP3FindSyncWord(unsigned char* buf, int nBytes) {
    for (int i = 0; i < nBytes - 1; i++)
        if (buf[i] == 0xFF && (buf[i+1] & 0xE0) == 0xE0) return i;
    return -1;
}

int MP3Decode(HMP3Decoder hDec, unsigned char** inbuf, int* bytesLeft,
              short* outbuf, int useSize) {
    (void)hDec; (void)inbuf; (void)outbuf; (void)useSize;
    if (bytesLeft) *bytesLeft = 0;
    return ERR_MP3_INDATA_UNDERFLOW;
}

void MP3GetLastFrameInfo(HMP3Decoder hDec, MP3FrameInfo* info) {
    (void)hDec;
    if (!info) return;
    info->bitrate = 128; info->nChans = 2; info->samprate = 44100;
    info->bitsPerSample = 16; info->outputSamps = 0; info->layer = 3; info->version = 0;
}

int MP3GetNextFrameInfo(HMP3Decoder hDec, MP3FrameInfo* info, unsigned char* buf) {
    (void)hDec; (void)buf;
    if (info) *info = {};
    return ERR_MP3_INDATA_UNDERFLOW;
}

} // extern "C"
