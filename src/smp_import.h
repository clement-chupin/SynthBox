#pragma once
// On-board WAV/MP3 -> .smp conversion.
//
// SevenistSynth_RitualMachine's sampler (engine/sampler/) only streams its own ".smp" container (mono 16-bit,
// 4 KB blocks, see smp_format.h). Its ESP32 app expects them cooked on a PC (tools/wav2smp.py); its simulator
// converts a .wav/.mp3 the first time it is used. GrvEP-fusion does the latter on every target, so the SD card
// keeps taking plain WAV/MP3: the first use of a file converts it once into SMP_CACHE_DIR, later uses (and
// reboots) reuse the cached file as long as the source keeps the same size.
//
// Incremental: step() produces about one 4 KB output block, so the loader task that runs it (audio_engine.cpp,
// smpLoaderTask) keeps the playing streams fed between steps. Written to "<cache>.tmp" and renamed when complete:
// an interrupted conversion never leaves a truncated file that looks valid.
#include <cstdint>
#include <cstddef>

#define SMP_CACHE_DIR  "/grvsmp/cache"
#define SMP_WAVE_BINS  128              // the overview MODE_DJ draws (audioDJComputeWaveform)

// malloc() that prefers PSRAM on the board (internal RAM is nearly full, see CLAUDE.md); free() releases it.
void* smpBigAlloc(size_t n);

// The cache file for a source path: SMP_CACHE_DIR "/<fnv1a32 of the path>.smp".
void smpCachePath(const char* src, char* out, size_t n);

// True when `cache` is a finished conversion of `src` by this converter version and the source still has the
// size it had then. Fills (when non-null) the SMP_WAVE_BINS-bin overview, the length and the sample rate.
bool smpCacheValid(const char* src, const char* cache, uint8_t* wave, uint32_t* frames, uint32_t* rate);

class SmpImporter {
public:
    enum Result { RUNNING, DONE, FAILED };

    // normalize: scale the result to about -1 dBFS (sample keys, matching GrvEP's normalizeBuffer(); not the DJ
    // deck, a whole track keeps its own mastering). Only done for files up to kNormalizeMaxSec long.
    bool begin(const char* src, const char* cache, bool normalize);
    Result step();
    void abort();                       // stops and deletes the partial output
    bool active() const { return phase_ != IDLE; }
    uint32_t frames() const { return frames_; }   // output frames written so far
    uint32_t rate() const { return rate_; }
    uint8_t error() const { return err_; }        // KEY_ERR_* (audio_engine.h) after FAILED

    static constexpr uint32_t kNormalizeMaxSec = 120;

private:
    enum Phase { IDLE, DECODE, NORMALIZE, FINISH };
    bool openWav();
    bool openMp3();
    bool fillWav();                     // appends source frames to out_; false at the end of the data
    bool fillMp3();
    bool decodeMp3Frame();              // one MP3 frame into pend_; false when the stream is exhausted
    bool flushBlock();                  // writes out_ as one audio block
    bool finish();
    void fail(uint8_t err);
    void closeFiles();

    Phase phase_ = IDLE;
    bool mp3_ = false, normalize_ = false;
    uint8_t err_ = 0;
    char tmp_[64] = {}, dst_[64] = {};
    uint32_t srcSize_ = 0, rate_ = 0, frames_ = 0, blocks_ = 0, normBlk_ = 0;
    int32_t peak_ = 0, gainQ15_ = 32767;
    // output
    int16_t out_[2048];
    uint32_t outN_ = 0;
    uint8_t* blockPeak_ = nullptr;      // max |sample| >> 7 per output block (overview + header peaks)
    uint32_t blockPeakCap_ = 0;
    // WAV
    uint16_t channels_ = 1, bits_ = 16;
    bool float_ = false;
    uint32_t dataLeft_ = 0;
    // MP3
    void* dec_ = nullptr;
    uint8_t* in_ = nullptr;
    uint8_t* inPtr_ = nullptr;
    int inLeft_ = 0;
    bool eof_ = false;
    int consErr_ = 0;
    int16_t* pend_ = nullptr;           // the decoded frame not yet copied to out_ (mono)
    uint32_t pendN_ = 0, pendPos_ = 0;
    // files (opaque: Arduino File objects live in the .cpp)
    void* srcFile_ = nullptr;
    void* dstFile_ = nullptr;
};
