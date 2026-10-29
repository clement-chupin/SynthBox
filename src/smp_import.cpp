// On-board WAV/MP3 -> .smp conversion — see smp_import.h.
#include "smp_import.h"
#include "audio_engine.h"   // KEY_ERR_*

#include <Arduino.h>
#include <SD.h>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <new>

#include "mp3dec.h"
#include "engine/sampler/smp_format.h"

#if defined(ESP_PLATFORM) && !defined(SIMULATOR)
#include <esp_heap_caps.h>
#endif

using namespace sc;

namespace {

// GrvEP-fusion's own tag in the header block, past smp_format.h's last field (the peaks end at byte 164); the
// engine's parser never reads it. "GRV1", converter version, source size, then the SMP_WAVE_BINS overview.
constexpr uint32_t kGrvTagOffset = 256;
constexpr uint32_t kConverterVersion = 1;
constexpr int kInBytes = 8192;                                     // WAV read / MP3 feed buffer
constexpr int kMp3FrameSamples = MAX_NGRAN * MAX_NCHAN * MAX_NSAMP;
constexpr int32_t kTargetPeak = 29205;                             // about -1 dBFS
constexpr uint32_t kFadeInFrames = 32;                             // GrvEP's applyBufferFades(): no click at the start

File& asFile(void* p) { return *static_cast<File*>(p); }

uint32_t fnv1a(const char* s) {
    uint32_t h = 2166136261u;
    while (*s) { h ^= static_cast<uint8_t>(*s++); h *= 16777619u; }
    return h;
}

bool endsWithNoCase(const char* s, const char* ext) {
    size_t n = strlen(s), m = strlen(ext);
    if (n < m) return false;
    for (size_t i = 0; i < m; i++)
        if (tolower(static_cast<unsigned char>(s[n - m + i])) != tolower(static_cast<unsigned char>(ext[i]))) return false;
    return true;
}

uint32_t sourceSize(const char* src) {
    File f = SD.open(src, FILE_READ);
    if (!f) return 0;
    uint32_t s = f.size();
    f.close();
    return s;
}

// The max of `in` over `bins` equal ranges (at least one entry each).
void reducePeaks(const uint8_t* in, uint32_t n, uint8_t* out, int bins) {
    for (int b = 0; b < bins; b++) {
        uint32_t lo = static_cast<uint32_t>(static_cast<uint64_t>(n) * b / bins);
        uint32_t hi = static_cast<uint32_t>(static_cast<uint64_t>(n) * (b + 1) / bins);
        if (hi <= lo) hi = lo + 1;
        uint8_t m = 0;
        for (uint32_t i = lo; i < hi && i < n; i++) if (in[i] > m) m = in[i];
        out[b] = m;
    }
}

}  // namespace

void* smpBigAlloc(size_t n) {
#if defined(ESP_PLATFORM) && !defined(SIMULATOR)
    if (void* p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM)) return p;
#endif
    return malloc(n);
}

void smpCachePath(const char* src, char* out, size_t n) {
    snprintf(out, n, SMP_CACHE_DIR "/%08lx.smp", static_cast<unsigned long>(fnv1a(src)));
}

bool smpCacheValid(const char* src, const char* cache, uint8_t* wave, uint32_t* frames, uint32_t* rate) {
    File f = SD.open(cache, FILE_READ);
    if (!f) return false;
    uint8_t* hdr = static_cast<uint8_t*>(smpBigAlloc(kSmpBlockBytes));
    if (!hdr) { f.close(); return false; }
    const bool read = f.read(hdr, kSmpBlockBytes) == static_cast<int>(kSmpBlockBytes);
    const uint32_t size = f.size();
    f.close();
    SmpHeader h;
    const uint8_t* t = hdr + kGrvTagOffset;
    bool ok = read && smp_parse_header(hdr, kSmpBlockBytes, h) && size >= h.file_bytes()
              && memcmp(t, "GRV1", 4) == 0 && get32(t + 4) == kConverterVersion;
    if (ok) {
        const uint32_t srcSize = sourceSize(src);
        ok = srcSize != 0 && get32(t + 8) == srcSize;
    }
    if (ok) {
        if (wave) memcpy(wave, t + 12, SMP_WAVE_BINS);
        if (frames) *frames = h.frames;
        if (rate) *rate = h.sample_rate;
    }
    free(hdr);
    return ok;
}

bool SmpImporter::begin(const char* src, const char* cache, bool normalize) {
    abort();
    err_ = KEY_ERR_IO;
    frames_ = blocks_ = normBlk_ = srcSize_ = rate_ = 0;
    outN_ = pendN_ = pendPos_ = 0;
    peak_ = 0;
    gainQ15_ = 32767;
    normalize_ = normalize;
    eof_ = false;
    consErr_ = 0;
    inLeft_ = 0;
    snprintf(dst_, sizeof dst_, "%s", cache);
    snprintf(tmp_, sizeof tmp_, "%s.tmp", cache);

    in_ = static_cast<uint8_t*>(smpBigAlloc(kInBytes));
    blockPeakCap_ = 256;
    blockPeak_ = static_cast<uint8_t*>(smpBigAlloc(blockPeakCap_));
    if (!in_ || !blockPeak_) { fail(KEY_ERR_ALLOC); return false; }
    inPtr_ = in_;

    srcFile_ = new (std::nothrow) File(SD.open(src, FILE_READ));
    if (!srcFile_ || !asFile(srcFile_)) { fail(KEY_ERR_IO); return false; }
    srcSize_ = asFile(srcFile_).size();
    mp3_ = endsWithNoCase(src, ".mp3");
    if (!(mp3_ ? openMp3() : openWav())) { fail(err_); return false; }

    SD.mkdir("/grvsmp");
    SD.mkdir(SMP_CACHE_DIR);
    SD.remove(tmp_);
    dstFile_ = new (std::nothrow) File(SD.open(tmp_, "w+b"));
    if (!dstFile_ || !asFile(dstFile_)) { fail(KEY_ERR_IO); return false; }
    memset(out_, 0, sizeof out_);                                   // header placeholder, rewritten by finish()
    if (asFile(dstFile_).write(reinterpret_cast<const uint8_t*>(out_), kSmpBlockBytes) != kSmpBlockBytes) { fail(KEY_ERR_IO); return false; }
    phase_ = DECODE;
    return true;
}

bool SmpImporter::openWav() {
    File& f = asFile(srcFile_);
    uint8_t h[12];
    if (f.read(h, 12) != 12 || memcmp(h, "RIFF", 4) != 0 || memcmp(h + 8, "WAVE", 4) != 0) { err_ = KEY_ERR_FORMAT; return false; }
    bool haveFmt = false;
    for (;;) {
        uint8_t c[8];
        if (f.read(c, 8) != 8) { err_ = KEY_ERR_FORMAT; return false; }
        const uint32_t len = get32(c + 4);
        if (memcmp(c, "fmt ", 4) == 0) {
            uint8_t fm[40] = {};
            const uint32_t rd = len < sizeof fm ? len : static_cast<uint32_t>(sizeof fm);
            if (rd < 16 || f.read(fm, rd) != static_cast<int>(rd)) { err_ = KEY_ERR_FORMAT; return false; }
            uint16_t fmt = static_cast<uint16_t>(get16(fm));
            if (fmt == 0xFFFE && rd >= 26) fmt = static_cast<uint16_t>(get16(fm + 24));   // WAVE_FORMAT_EXTENSIBLE: the sub-format GUID
            channels_ = static_cast<uint16_t>(get16(fm + 2));
            rate_ = get32(fm + 4);
            bits_ = static_cast<uint16_t>(get16(fm + 14));
            float_ = fmt == 3;
            const bool bitsOk = float_ ? bits_ == 32 : (bits_ == 8 || bits_ == 16 || bits_ == 24 || bits_ == 32);
            if ((fmt != 1 && fmt != 3) || channels_ == 0 || channels_ > 8 || rate_ == 0 || !bitsOk) { err_ = KEY_ERR_FORMAT; return false; }
            haveFmt = true;
            const uint32_t skip = (len - rd) + (len & 1);
            if (skip && !f.seek(f.position() + skip)) { err_ = KEY_ERR_FORMAT; return false; }
        } else if (memcmp(c, "data", 4) == 0) {
            if (!haveFmt) { err_ = KEY_ERR_FORMAT; return false; }
            const uint32_t rem = f.size() - f.position();
            dataLeft_ = (len == 0 || len > rem) ? rem : len;             // streamed writers leave 0 / 0xFFFFFFFF here
            return true;
        } else if (!f.seek(f.position() + len + (len & 1))) {
            err_ = KEY_ERR_FORMAT;
            return false;
        }
    }
}

bool SmpImporter::openMp3() {
    File& f = asFile(srcFile_);
    uint8_t id3[10];
    uint32_t start = 0;
    if (f.read(id3, 10) == 10 && memcmp(id3, "ID3", 3) == 0) {          // skip an ID3v2 tag (syncsafe size, optional footer)
        start = ((id3[6] & 0x7Fu) << 21) | ((id3[7] & 0x7Fu) << 14) | ((id3[8] & 0x7Fu) << 7) | (id3[9] & 0x7Fu);
        start += 10 + ((id3[5] & 0x10) ? 10 : 0);
    }
    if (!f.seek(start)) { err_ = KEY_ERR_FORMAT; return false; }
    // The Helix decoder makes a dozen allocations, each under the 4 KB threshold below which ESP-IDF's malloc() picks internal
    // RAM: ~8.6 KB of it, measured, out of ~11 KB left after boot — enough that any other file opened during a conversion
    // (newlib's per-FILE lock is internal) aborted. Let them go to PSRAM for the duration of this call.
#if defined(ESP_PLATFORM) && !defined(SIMULATOR)
    heap_caps_malloc_extmem_enable(32);
    dec_ = MP3InitDecoder();
    heap_caps_malloc_extmem_enable(CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL);
#else
    dec_ = MP3InitDecoder();
#endif
    pend_ = static_cast<int16_t*>(smpBigAlloc(kMp3FrameSamples * sizeof(int16_t)));
    if (!dec_ || !pend_) { err_ = KEY_ERR_ALLOC; return false; }
    // refill() inline: the first fill
    const int rd = f.read(in_, kInBytes);
    inLeft_ = rd > 0 ? rd : 0;
    if (inLeft_ < kInBytes) eof_ = true;
    const int sync = MP3FindSyncWord(inPtr_, inLeft_);
    if (sync < 0) { err_ = KEY_ERR_FORMAT; return false; }
    inPtr_ += sync;
    inLeft_ -= sync;
    return true;
}

// One source frame (any format) -> out_, with the fade-in and the peak.
#define SMP_PUSH(v)                                                                           \
    do {                                                                                      \
        int32_t s_ = (v);                                                                     \
        const uint32_t at_ = frames_ + outN_;                                                 \
        if (at_ < kFadeInFrames) s_ = s_ * static_cast<int32_t>(at_) / kFadeInFrames;        \
        out_[outN_++] = static_cast<int16_t>(s_ > 32767 ? 32767 : (s_ < -32768 ? -32768 : s_)); \
    } while (0)

bool SmpImporter::fillWav() {
    File& f = asFile(srcFile_);
    const uint32_t bps = bits_ / 8, bpf = bps * channels_;
    while (outN_ < 2048 && dataLeft_ >= bpf) {
        uint32_t want = (2048 - outN_) * bpf;
        const uint32_t cap = (kInBytes / bpf) * bpf;
        if (want > cap) want = cap;
        if (want > dataLeft_) want = (dataLeft_ / bpf) * bpf;
        int rd = f.read(in_, want);
        if (rd <= 0) { dataLeft_ = 0; break; }
        rd -= rd % static_cast<int>(bpf);
        dataLeft_ -= static_cast<uint32_t>(rd);
        for (const uint8_t* p = in_; p < in_ + rd; p += bpf) {
            int32_t sum = 0;
            for (uint32_t c = 0; c < channels_; c++) {
                const uint8_t* q = p + c * bps;
                int32_t v;
                if (float_) {
                    float x;
                    memcpy(&x, q, 4);
                    x = x > 1.0f ? 1.0f : (x < -1.0f ? -1.0f : x);
                    v = static_cast<int32_t>(x * 32767.0f);
                } else if (bits_ == 8) {
                    v = (static_cast<int32_t>(q[0]) - 128) << 8;
                } else if (bits_ == 16) {
                    v = static_cast<int16_t>(get16(q));
                } else if (bits_ == 24) {
                    v = static_cast<int32_t>((static_cast<uint32_t>(q[0]) << 8) | (static_cast<uint32_t>(q[1]) << 16) | (static_cast<uint32_t>(q[2]) << 24)) >> 16;
                } else {
                    v = static_cast<int32_t>(get32(q)) >> 16;
                }
                sum += v;
            }
            SMP_PUSH(sum / static_cast<int32_t>(channels_));
        }
    }
    return dataLeft_ >= bpf;
}

bool SmpImporter::decodeMp3Frame() {
    File& f = asFile(srcFile_);
    HMP3Decoder dec = static_cast<HMP3Decoder>(dec_);
    auto refill = [&]() {
        if (inPtr_ != in_ && inLeft_ > 0) memmove(in_, inPtr_, static_cast<size_t>(inLeft_));
        inPtr_ = in_;
        if (eof_) return;
        const int space = kInBytes - inLeft_;
        const int rd = f.read(in_ + inLeft_, static_cast<size_t>(space));
        inLeft_ += rd > 0 ? rd : 0;
        if (rd < space) eof_ = true;
    };
    // Same error handling as GrvEP's svcLoadMp3(), including its end-of-file rule: the desktop simulator's decoder
    // shim (libmpg123) takes its whole input at once, so it can still hold undecoded audio once the file's bytes run
    // out — keep calling until 32 calls in a row bring nothing (one or two on the board's Helix decoder).
    for (int guard = 0; guard < 2000; guard++) {
        if (inLeft_ < MAINBUF_SIZE && !eof_) refill();
        const int ret = MP3Decode(dec, &inPtr_, &inLeft_, pend_, 0);
        if (ret == ERR_MP3_INDATA_UNDERFLOW) {
            if (eof_) { if (++consErr_ > 32) return false; continue; }
            refill();
            consErr_ = 0;
            continue;
        }
        if (ret == ERR_MP3_MAINDATA_UNDERFLOW) {
            if (eof_ && ++consErr_ > 32) return false;
            continue;
        }
        if (ret < 0) {
            if (++consErr_ > 64) return false;
            const int sk = inLeft_ > 1 ? MP3FindSyncWord(inPtr_ + 1, inLeft_ - 1) : -1;
            if (sk < 0) { if (eof_) return false; inLeft_ = 0; refill(); continue; }
            inPtr_ += sk + 1;
            inLeft_ -= sk + 1;
            continue;
        }
        consErr_ = 0;
        MP3FrameInfo info;
        MP3GetLastFrameInfo(dec, &info);
        if (info.nChans < 1 || info.nChans > 2 || info.outputSamps < 1 || info.outputSamps > kMp3FrameSamples || info.samprate <= 0) continue;
        if (!rate_) rate_ = static_cast<uint32_t>(info.samprate);
        const uint32_t n = static_cast<uint32_t>(info.outputSamps / info.nChans);
        if (info.nChans == 2)
            for (uint32_t j = 0; j < n; j++) pend_[j] = static_cast<int16_t>((static_cast<int32_t>(pend_[2 * j]) + pend_[2 * j + 1]) >> 1);
        pendN_ = n;
        pendPos_ = 0;
        return true;
    }
    return false;
}

bool SmpImporter::fillMp3() {
    while (outN_ < 2048) {
        if (pendPos_ >= pendN_) { if (!decodeMp3Frame()) return false; continue; }
        SMP_PUSH(pend_[pendPos_++]);
    }
    return true;
}

bool SmpImporter::flushBlock() {
    int32_t pk = 0;
    for (uint32_t i = 0; i < outN_; i++) { const int32_t v = out_[i] < 0 ? -out_[i] : out_[i]; if (v > pk) pk = v; }
    for (uint32_t i = outN_; i < 2048; i++) out_[i] = 0;
    // A write occasionally fails on the board's SD card at a random point of a long conversion. FatFs then marks the open
    // file as failed for good (every later call on it fails too), so a retry has to reopen the file before rewriting the block.
    const uint32_t at = (blocks_ + 1) * kSmpBlockBytes;
    bool written = asFile(dstFile_).write(reinterpret_cast<const uint8_t*>(out_), kSmpBlockBytes) == kSmpBlockBytes;
    for (int attempt = 0; attempt < 3 && !written; attempt++) {
        asFile(dstFile_).close();
        delay(5 << attempt);
        asFile(dstFile_) = SD.open(tmp_, "r+b");
        File& d = asFile(dstFile_);
        written = d && d.seek(at) && d.write(reinterpret_cast<const uint8_t*>(out_), kSmpBlockBytes) == kSmpBlockBytes;
    }
    if (!written) return false;
    if (blocks_ >= blockPeakCap_) {
        uint8_t* grown = static_cast<uint8_t*>(smpBigAlloc(blockPeakCap_ * 2));
        if (!grown) return false;
        memcpy(grown, blockPeak_, blockPeakCap_);
        free(blockPeak_);
        blockPeak_ = grown;
        blockPeakCap_ *= 2;
    }
    blockPeak_[blocks_++] = static_cast<uint8_t>((pk >> 7) > 255 ? 255 : (pk >> 7));
    if (pk > peak_) peak_ = pk;
    frames_ += outN_;
    outN_ = 0;
    return true;
}

SmpImporter::Result SmpImporter::step() {
    switch (phase_) {
    case DECODE: {
        const bool more = mp3_ ? fillMp3() : fillWav();
        if ((outN_ == 2048 || (!more && outN_ > 0)) && !flushBlock()) { fail(KEY_ERR_IO); return FAILED; }
        if (more) return RUNNING;
        if (frames_ == 0 || rate_ == 0) { fail(KEY_ERR_FORMAT); return FAILED; }
        if (normalize_ && peak_ > 0 && peak_ < kTargetPeak && frames_ <= kNormalizeMaxSec * rate_) {
            gainQ15_ = static_cast<int32_t>(static_cast<int64_t>(kTargetPeak) * 32767 / peak_);   // above 32767 = a boost
            normBlk_ = 0;
            phase_ = NORMALIZE;
        } else {
            phase_ = FINISH;
        }
        return RUNNING;
    }
    case NORMALIZE: {
        if (normBlk_ >= blocks_) { phase_ = FINISH; return RUNNING; }
        File& d = asFile(dstFile_);
        const uint32_t off = (normBlk_ + 1) * kSmpBlockBytes;
        uint8_t* raw = reinterpret_cast<uint8_t*>(out_);
        if (!d.seek(off) || d.read(raw, kSmpBlockBytes) != static_cast<int>(kSmpBlockBytes)) { fail(KEY_ERR_IO); return FAILED; }
        for (uint32_t i = 0; i < kSmpBlockFrames; i++) {                // little-endian on every target (ESP32, x86, wasm)
            const int32_t v = (static_cast<int32_t>(out_[i]) * gainQ15_) >> 15;
            out_[i] = static_cast<int16_t>(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
        }
        if (!d.seek(off) || d.write(raw, kSmpBlockBytes) != kSmpBlockBytes) { fail(KEY_ERR_IO); return FAILED; }
        const int32_t p = (static_cast<int32_t>(blockPeak_[normBlk_]) * gainQ15_) >> 15;
        blockPeak_[normBlk_] = static_cast<uint8_t>(p > 255 ? 255 : p);
        normBlk_++;
        return RUNNING;
    }
    case FINISH:
        if (finish()) return DONE;
        fail(KEY_ERR_IO);
        return FAILED;
    default:
        return FAILED;
    }
}

bool SmpImporter::finish() {
    SmpHeader h;
    h.sample_rate = rate_;
    h.frames = frames_;
    h.root_note = 60;
    reducePeaks(blockPeak_, blocks_, h.peaks, kSmpPeaks);
    uint8_t* hdr = reinterpret_cast<uint8_t*>(out_);                   // out_ is exactly one 4 KB block
    smp_write_header(hdr, h);
    uint8_t* t = hdr + kGrvTagOffset;
    memcpy(t, "GRV1", 4);
    put32(t + 4, kConverterVersion);
    put32(t + 8, srcSize_);
    reducePeaks(blockPeak_, blocks_, t + 12, SMP_WAVE_BINS);
    File& d = asFile(dstFile_);
    if (!d.seek(0) || d.write(hdr, kSmpBlockBytes) != kSmpBlockBytes) return false;
    closeFiles();
    SD.remove(dst_);
    if (!SD.rename(tmp_, dst_)) return false;
    free(blockPeak_);
    blockPeak_ = nullptr;
    free(in_);
    in_ = nullptr;
    phase_ = IDLE;
    return true;
}

void SmpImporter::fail(uint8_t err) {
    abort();
    err_ = err;
}

void SmpImporter::closeFiles() {
    if (srcFile_) { asFile(srcFile_).close(); delete static_cast<File*>(srcFile_); srcFile_ = nullptr; }
    if (dstFile_) { asFile(dstFile_).close(); delete static_cast<File*>(dstFile_); dstFile_ = nullptr; }
    if (dec_) { MP3FreeDecoder(static_cast<HMP3Decoder>(dec_)); dec_ = nullptr; }
    if (pend_) { free(pend_); pend_ = nullptr; }
}

void SmpImporter::abort() {
    const bool hadOutput = dstFile_ != nullptr || phase_ != IDLE;
    closeFiles();
    if (hadOutput && tmp_[0]) SD.remove(tmp_);
    if (blockPeak_) { free(blockPeak_); blockPeak_ = nullptr; }
    if (in_) { free(in_); in_ = nullptr; }
    phase_ = IDLE;
}
