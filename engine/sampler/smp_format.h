#pragma once
// ".smp" cooked sample container (ADR-021). Block 0 is a 4 KB header, the audio follows in 4 KB blocks of mono
// 16-bit little-endian PCM, so every read the player issues is aligned to the card's sector size and the player
// never parses chunks at run time. Created by tools/wav2smp.py (or smp_build() for tests).
//
//   offset  size  field
//   0       4     magic "SMP1"
//   4       4     version (1)
//   8       4     sample rate (Hz)
//   12      4     frames
//   16      2     channels (1: stereo is mixed down by the tool)
//   18      2     bits (16)
//   20      2     root note (MIDI)
//   22      2     tune (cents, signed)
//   24      4     loop start (frame)
//   28      4     loop end (frame, exclusive); loop_end <= loop_start means "no loop"
//   32      1     default loop mode (0 off, 1 forward, 2 ping-pong)
//   33      1     slice count (0..16)
//   34      2     reserved
//   36      64    slice start frames (16 x u32)
//   100     64    amplitude overview for the sample list (64 buckets, max |sample| >> 7); all zero = not stored (files cooked
//                 before it existed), the reader then computes it from the audio
//   164..4095     zero
// Audio block b (0-based) is at file offset (b + 1) * 4096 and holds frames [b * 2048, (b + 1) * 2048).
#include <cstdint>
#include <cstring>
#include <vector>

namespace sc {

constexpr uint32_t kSmpBlockBytes = 4096;
constexpr uint32_t kSmpBlockFrames = kSmpBlockBytes / 2;
constexpr int kSmpMaxSlices = 16;
constexpr int kSmpPeaks = 64;
constexpr uint32_t kSmpPeaksOffset = 100;
enum SmpLoop : uint8_t { SMP_LOOP_OFF = 0, SMP_LOOP_FWD = 1, SMP_LOOP_PINGPONG = 2 };

struct SmpHeader {
    uint32_t sample_rate = 48000;
    uint32_t frames = 0;
    uint16_t root_note = 60;
    int16_t tune_cents = 0;
    uint32_t loop_start = 0, loop_end = 0;
    uint8_t loop_mode = SMP_LOOP_OFF;
    uint8_t slice_count = 0;
    uint32_t slice[kSmpMaxSlices] = {};
    uint8_t peaks[kSmpPeaks] = {};

    bool has_peaks() const { for (uint8_t p : peaks) if (p) return true; return false; }
    bool has_loop() const { return loop_end > loop_start + 1; }
    uint32_t blocks() const { return (frames + kSmpBlockFrames - 1) / kSmpBlockFrames; }
    uint32_t file_bytes() const { return (blocks() + 1) * kSmpBlockBytes; }
};

inline void put16(uint8_t *p, uint32_t v) { p[0] = static_cast<uint8_t>(v); p[1] = static_cast<uint8_t>(v >> 8); }
inline void put32(uint8_t *p, uint32_t v) { put16(p, v & 0xFFFFu); put16(p + 2, v >> 16); }
inline uint32_t get16(const uint8_t *p) { return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8); }
inline uint32_t get32(const uint8_t *p) { return get16(p) | (get16(p + 2) << 16); }

// Serialises the header into a 4 KB block.
inline void smp_write_header(uint8_t *block, const SmpHeader &h) {
    std::memset(block, 0, kSmpBlockBytes);
    std::memcpy(block, "SMP1", 4);
    put32(block + 4, 1);
    put32(block + 8, h.sample_rate);
    put32(block + 12, h.frames);
    put16(block + 16, 1);
    put16(block + 18, 16);
    put16(block + 20, h.root_note);
    put16(block + 22, static_cast<uint16_t>(h.tune_cents));
    put32(block + 24, h.loop_start);
    put32(block + 28, h.loop_end);
    block[32] = h.loop_mode;
    block[33] = h.slice_count;
    for (int i = 0; i < kSmpMaxSlices; i++) put32(block + 36 + 4 * i, h.slice[i]);
    std::memcpy(block + kSmpPeaksOffset, h.peaks, kSmpPeaks);
}

// Validates and parses the first block of a file.
inline bool smp_parse_header(const uint8_t *block, uint32_t bytes, SmpHeader &h) {
    if (bytes < 100 || std::memcmp(block, "SMP1", 4) != 0 || get32(block + 4) != 1) return false;
    if (get16(block + 16) != 1 || get16(block + 18) != 16) return false;
    h.sample_rate = get32(block + 8);
    h.frames = get32(block + 12);
    h.root_note = static_cast<uint16_t>(get16(block + 20));
    h.tune_cents = static_cast<int16_t>(get16(block + 22));
    h.loop_start = get32(block + 24);
    h.loop_end = get32(block + 28);
    h.loop_mode = block[32];
    h.slice_count = block[33] > kSmpMaxSlices ? static_cast<uint8_t>(kSmpMaxSlices) : block[33];
    for (int i = 0; i < kSmpMaxSlices; i++) h.slice[i] = get32(block + 36 + 4 * i);
    if (bytes >= kSmpPeaksOffset + kSmpPeaks) std::memcpy(h.peaks, block + kSmpPeaksOffset, kSmpPeaks);
    return h.sample_rate > 0 && h.frames > 0;
}

// The amplitude overview of a whole sample (what the sample list draws): per bucket the largest |sample|, scaled to 0..255.
inline void smp_compute_peaks(const int16_t *pcm, uint32_t frames, uint8_t out[kSmpPeaks]) {
    for (int b = 0; b < kSmpPeaks; b++) {
        const uint64_t f0 = static_cast<uint64_t>(frames) * static_cast<uint64_t>(b) / kSmpPeaks;
        const uint64_t f1 = static_cast<uint64_t>(frames) * static_cast<uint64_t>(b + 1) / kSmpPeaks;
        const uint64_t hi = f1 > f0 + 1 ? f1 : f0 + 1;
        const uint64_t stride = (hi - f0) / 512 > 1 ? (hi - f0) / 512 : 1;
        int peak = 0;
        for (uint64_t f = f0; f < hi && f < frames; f += stride) { const int v = pcm[f] < 0 ? -pcm[f] : pcm[f]; if (v > peak) peak = v; }
        out[b] = static_cast<uint8_t>(peak >> 7 > 255 ? 255 : peak >> 7);
    }
}

// Builds a complete file image in memory (tests, tools).
inline std::vector<uint8_t> smp_build(const SmpHeader &h, const int16_t *pcm) {
    std::vector<uint8_t> f(h.file_bytes(), 0);
    SmpHeader hh = h;
    if (!hh.has_peaks()) smp_compute_peaks(pcm, h.frames, hh.peaks);
    smp_write_header(f.data(), hh);
    for (uint32_t i = 0; i < h.frames; i++) put16(&f[kSmpBlockBytes + 2 * i], static_cast<uint16_t>(pcm[i]));
    return f;
}

}  // namespace sc
