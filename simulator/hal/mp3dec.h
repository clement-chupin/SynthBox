#pragma once
// Helix MP3 decoder API — implemented in sim_mp3dec.cpp using libmpg123
// On the real device this comes from chmorgan/esp-libhelix-mp3.

#ifdef __cplusplus
extern "C" {
#endif

#define MAINBUF_SIZE 1940
#define MAX_NGRAN    2
#define MAX_NCHAN    2
#define MAX_NSAMP    576

typedef enum { MPEG1=0, MPEG2=1, MPEG25=2 } MPEGVersion;
typedef void* HMP3Decoder;

enum {
    ERR_MP3_NONE                =    0,
    ERR_MP3_INDATA_UNDERFLOW    =   -1,
    ERR_MP3_MAINDATA_UNDERFLOW  =   -2,
    ERR_MP3_FREE_BITRATE_SYNC   =   -3,
    ERR_MP3_OUT_OF_MEMORY       =   -4,
    ERR_MP3_NULL_POINTER        =   -5,
    ERR_MP3_INVALID_FRAMEHEADER =   -6,
    ERR_MP3_INVALID_SIDEINFO    =   -7,
    ERR_MP3_INVALID_SCALEFACT   =   -8,
    ERR_MP3_INVALID_HUFFCODES   =   -9,
    ERR_MP3_INVALID_DEQUANTIZE  =  -10,
    ERR_MP3_INVALID_IMDCT       =  -11,
    ERR_MP3_INVALID_SUBBAND     =  -12,
    ERR_UNKNOWN                 = -9999
};

typedef struct {
    int bitrate;
    int nChans;
    int samprate;
    int bitsPerSample;
    int outputSamps;
    int layer;
    int version;
} MP3FrameInfo;

HMP3Decoder MP3InitDecoder(void);
void        MP3FreeDecoder(HMP3Decoder hDec);
int         MP3Decode(HMP3Decoder hDec, unsigned char** inbuf, int* bytesLeft,
                      short* outbuf, int useSize);
void        MP3GetLastFrameInfo(HMP3Decoder hDec, MP3FrameInfo* info);
int         MP3GetNextFrameInfo(HMP3Decoder hDec, MP3FrameInfo* info, unsigned char* buf);
int         MP3FindSyncWord(unsigned char* buf, int nBytes);

#ifdef __cplusplus
}
#endif
