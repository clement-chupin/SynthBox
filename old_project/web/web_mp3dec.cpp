// web_mp3dec.cpp — replaces src/mp3dec.cpp (the Helix MP3 API) in the web build.
// No MP3 decoding in the browser build (structure/WEB_SIMULATOR.md's documented
// limitation) — every call reports "no data"/"underflow" exactly as if the input
// buffer were always empty. src/audio_engine.cpp already treats
// ERR_MP3_INDATA_UNDERFLOW as "nothing decoded, don't send PCM" (see its own
// comment on the Helix streaming API contract), so this never crashes — it just
// means .mp3 files silently produce no sound while .wav keeps working via the
// direct PCM path. See hal/mp3dec.h for the API contract these implement.

#include "hal/mp3dec.h"

extern "C" HMP3Decoder MP3InitDecoder(void) {
    // Must be non-null (callers check for allocation failure) — never dereferenced.
    return (HMP3Decoder)1;
}

extern "C" void MP3FreeDecoder(HMP3Decoder) {}

extern "C" int MP3FindSyncWord(unsigned char*, int) {
    return -1;  // no sync word ever found
}

extern "C" int MP3Decode(HMP3Decoder hDec, unsigned char** inbuf, int* bytesLeft,
                          short* /*outbuf*/, int /*useSize*/) {
    if (!hDec || !inbuf || !bytesLeft) return ERR_MP3_NULL_POINTER;
    return ERR_MP3_INDATA_UNDERFLOW;
}

extern "C" void MP3GetLastFrameInfo(HMP3Decoder, MP3FrameInfo* info) {
    if (!info) return;
    info->bitrate       = 0;
    info->nChans        = 2;
    info->samprate      = 44100;
    info->bitsPerSample = 16;
    info->outputSamps   = 0;
    info->layer         = 3;
    info->version       = 0;
}

extern "C" int MP3GetNextFrameInfo(HMP3Decoder, MP3FrameInfo* info, unsigned char*) {
    if (info) MP3GetLastFrameInfo(nullptr, info);
    return ERR_MP3_INDATA_UNDERFLOW;
}
