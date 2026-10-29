#pragma once
// Offline processing of a sample (ADR-041 decisions 36-38: the pads' sample tools). Mono q15 in, mono q15 out, block by block: the card
// holds the file, nothing the size of the sample is ever in memory (the prototype has ~60 KB of PSRAM to spare). The caller supplies the
// reader (any frame range of the source) and the writer (the destination, frames in order); Processor runs in steps of a few blocks so
// the card task can keep the sample streams fed in between (platform/engine/render_job.cpp). Not the audio thread: floats are fine here.
// The time, pitch and texture operations need working memory (work_bytes: 6 to 43 KB by operation); the caller lends it for the job.
#include <cstddef>
#include <cstdint>
#include "engine/dsp/q.h"
#include "engine/sampler/smp_format.h"

namespace sc {

// ENGINE_PROFILE on the ESP32: CPU cycles spent in the stages of the spectral frames since the last begin() (0 load: the card read and
// the window, 1 forward FFT, 2 the spectrum's work, 3 inverse FFT, 4 overlap-add), and the frames counted. Zeros elsewhere.
extern uint32_t g_proc_prof[5], g_proc_frames;

struct FrameReader {
    virtual ~FrameReader() = default;
    virtual bool read(uint32_t frame, uint32_t n, q15 *dst) = 0;      // frames [frame, frame + n) of the source (inside it)
};
struct FrameWriter {
    virtual ~FrameWriter() = default;
    virtual bool write(const q15 *src, uint32_t n) = 0;              // the next n frames of the result
};

// Level & cleanup (decision 38: the first group). amount:
//   Normalize   the peak's target in 0.1 dBFS (-3 = -0.3 dB)          Gain       0.1 dB (-240..+240), saturating
//   FadeIn/Out  ms from the file's start / to its end (linear)         TrimSilence the threshold in dBFS (-90..-6): what is under it at
//                                                                      both ends goes (5 ms kept before the first sound)
//   Reverse     -                                                      RemoveDc   - (a 10 Hz one-pole high-pass)
//   PadTo       the length in frames: silence appended up to it (a Grid chop that fills whole grid units, decision 43); a shorter
//               length keeps the sample as it is
//
// Time & pitch (the second group), all in the time domain:
//   Stretch     the new length in ppm of the old one (250000..4000000 = x0.25..x4), the pitch kept: WSOLA (overlap-add of ~43 ms
//               windows, each taken where it continues the previous one best; the first window keeps the attack untouched)
//   Resample    the new length in ppm of the old one: the speed changes and the pitch with it (a tape's speed); a 32-tap windowed
//               sinc, its cutoff lowered when the result is shorter
//   Pitch       cents (-2400..+2400), the length kept: Stretch by the pitch ratio, then Resample back, in one pass
// Loop and slices scale with the length.
//
//   Crop        frames [amount, amount2) only (amount2 0 = to the end); loop and slices move with it, what falls outside goes
//
// Color / lo-fi (the fourth group), the length kept:
//   Decimate    the rate in Hz the sample is held at (no filter before it: the folding is the point)
//   Bits        the word length (1..16): rounded to that many bits
//   Drive       0.1 dB of gain into a soft clipper (0..360); full scale stays full scale
//   LowPass / HighPass   the cutoff in Hz, 12 dB per octave (Butterworth)
//
// The spectral ones (a 4096-point FFT, engine/offline/rfft.h: windows of 85 ms at 48 kHz):
//   StretchTonal  as Stretch, by a phase vocoder (75 % overlap, the phases of each spectral peak carried on and its neighbours locked
//                 to it): smooth on sustained and polyphonic sounds, soft on attacks (WSOLA is the opposite)
//   PitchTonal    as Pitch, StretchTonal feeding the resampler
//   Paul          paulstretch: the new length in ppm (x1 .. x64); every window keeps its spectrum's magnitudes and gets random
//                 phases, so the sound becomes a slow, smooth wash. The result is 6 dB under the source's level (the peaks of the
//                 random phases need the room)
enum class ProcOp : uint8_t { Normalize, Gain, FadeIn, FadeOut, TrimSilence, Reverse, RemoveDc, PadTo, Stretch, Resample, Pitch,
                              Crop, Decimate, Bits, Drive, LowPass, HighPass, StretchTonal, PitchTonal, Paul, Count };

class Processor {
public:
    static constexpr uint32_t kChunk = kSmpBlockFrames;                   // frames per step unit
    static constexpr int kWsHop = 1024, kWsSearch = kWsHop / 2;           // WSOLA at 48 kHz: the hop (half a window), the search each way
    static constexpr int kRsTaps = 32, kRsPhases = 32, kRsBuf = 1024;     // the resampler: taps, table phases, its input window
    // The working memory `op` needs (0 for most), and the source's header and the operation with that memory (aligned for a float,
    // the caller's until done). False when there is nothing to do (an empty sample) or the memory is missing.
    // The spectral operations work on one frame and its sine table over and over (fast_bytes: 20.5 KB): given as `fast`, memory that
    // is quick to reach at random (the prototype's internal RAM; its PSRAM costs milliseconds per pass over such a block), they run
    // about twice as fast, and `work` is that much smaller (work_bytes(op, true)). Without it everything lies in `work`.
    static size_t fast_bytes(ProcOp op);
    static size_t work_bytes(ProcOp op, bool with_fast = false);
    bool begin(const SmpHeader &in, ProcOp op, int32_t amount, int32_t amount2 = 0, void *work = nullptr, void *fast = nullptr);
    // One block (kChunk frames = a .smp block) the caller may use before begin and after done (the card job's header block).
    q15 *scratch() { return buf_; }
    // Up to `chunks` chunks of work (analysis first when the operation needs one, then the output). False on a read / write error.
    bool step(FrameReader &r, FrameWriter &w, int chunks);
    bool done() const { return phase_ == Phase::Done; }
    float progress() const;                                               // 0..1 over the analysis and the output
    // The result's header (frames, rate, loop and slices carried over: shifted by a trim, mirrored by a reverse; the overview of the
    // output): valid once done().
    const SmpHeader &out_header() const { return out_; }

private:
    enum class Phase : uint8_t { Analyze, Write, Done };
    uint32_t source_of(uint32_t o) const;                                 // the first source frame of output frame o's chunk (Reverse: see write)
    static bool uses_ws(ProcOp op) { return op == ProcOp::Stretch || op == ProcOp::Pitch; }
    static bool uses_rs(ProcOp op) { return op == ProcOp::Resample || op == ProcOp::Pitch || op == ProcOp::PitchTonal; }
    static bool uses_pv(ProcOp op) { return op == ProcOp::StretchTonal || op == ProcOp::PitchTonal; }
    static bool uses_fft(ProcOp op) { return uses_pv(op) || op == ProcOp::Paul; }
    // The spectral frames keep their source window between frames (8 KB more) and read only what is new. PitchTonal only when its
    // frame is in `fast` memory: with everything in `work` it is at the limit of what the prototype can lend, and reads every frame whole.
    static bool uses_win(ProcOp op, bool with_fast) { return op == ProcOp::StretchTonal || op == ProcOp::Paul || (op == ProcOp::PitchTonal && with_fast); }
    // The working memory's parts (p: set its pointers). with_fast: the frame and its table are in `fast`, not in `base`.
    static size_t layout(ProcOp op, Processor *p, uint8_t *base, bool with_fast, uint8_t *fast, size_t *fast_size);
    bool timed() const { return uses_ws(op_) || uses_rs(op_) || uses_fft(op_); }
    bool fetch(FrameReader &r, int64_t start, uint32_t n, q15 *dst);      // source frames, zeros outside the file
    bool slide(FrameReader &r, q15 *dst, int64_t start, uint32_t n);      // dst = the source's [start, start + n), reading only what it does not hold yet
    int  ws_search() const;                                               // the offset in seg_ that continues the previous window best
    bool ws_hop(FrameReader &r);                                          // the next hs_ stretched frames into ready_
    bool load_frame(FrameReader &r, int64_t centre);                      // fx_ = a Hann-windowed frame of the source, as its spectrum
    bool pv_frame(FrameReader &r);                                        // one phase vocoder frame overlap-added into ola_
    bool paul_frame(FrameReader &r);                                      // one paulstretch frame overlap-added into ola_
    bool st_hop(FrameReader &r);                                          // the next hop of the stretched signal (any method) into ready_
    bool st_pull(FrameReader &r, q15 *dst, uint32_t n);                   // the next n frames of the stretched signal
    bool stage1(FrameReader &r, q15 *dst, uint32_t n);                    // the next n frames of what the resampler reads
    bool rs_fill(FrameReader &r, q15 *dst, uint32_t n);                   // the next n resampled frames (from output frame pos_)
    SmpHeader in_, out_;
    ProcOp op_ = ProcOp::Gain;
    int32_t amount_ = 0;
    Phase phase_ = Phase::Done;
    uint32_t pos_ = 0;                                                    // frames done in the current phase
    uint32_t first_ = 0, last_ = 0;                                       // the trim: the kept region [first_, last_)
    int32_t peak_ = 0;                                                    // the analysis: the largest |sample|
    float gain_ = 1.0f;
    float dc_x_ = 0.0f, dc_y_ = 0.0f, dc_r_ = 0.999f;                     // the DC blocker's state
    uint32_t dec_rate_ = 0, dec_acc_ = 0;                                 // Decimate: the hold's rate and its phase
    float held_ = 0.0f;
    float k1_ = 1.0f, k2_ = 1.0f;                                         // Bits: the step; Drive: gain in, gain out
    float f_a1_ = 0.0f, f_a2_ = 0.0f, f_a3_ = 0.0f, f_s1_ = 0.0f, f_s2_ = 0.0f;   // the filter (a state-variable one) and its state
    uint32_t bucket_peak_[kSmpPeaks] = {};
    q15 buf_[kChunk];
    // WSOLA (Stretch, and the first stage of Pitch)
    int hs_ = kWsHop, srch_ = kWsSearch;                                  // hop and search at the file's rate
    uint32_t ws_len_ = 0, ws_k_ = 0;                                      // the stretched length, the hop's number
    int64_t ws_prev_ = 0;                                                 // where the previous window was taken
    uint32_t ready_n_ = 0, ready_at_ = 0;
    const q15 *ready_ = nullptr;                                          // the hop just made (ws_out_, or the head of ola_)
    // (the arrays below live in the caller's working memory: layout())
    q15 *seg_ = nullptr;                                                  // 2 kWsHop + 2 kWsSearch: the source around the hop's ideal position
    q15 *tail_ = nullptr;                                                 // kWsHop: the previous window's falling half
    q15 *ws_out_ = nullptr;                                               // kWsHop
    int16_t *tgt_ = nullptr;                                              // kWsHop / 4: what the next window should continue (every 4th frame, 12 bits)
    // the spectral methods (StretchTonal, PitchTonal, Paul): N = kRfftN
    float *fx_ = nullptr;                                                 // N + 2: the frame, its spectrum, the frame again
    float *sin_ = nullptr;                                                // N / 4 + 1: the FFT's table (also the windows and the rotations)
    q15 *ola_ = nullptr;                                                  // N: the overlap-add; its head is the finished hop
    q15 *win_ = nullptr;                                                  // N: the source under the frame (uses_win), kept from frame to frame
    int64_t win_at_ = 0;                                                  // the sliding window (seg_ or win_): the source frame of its first entry ...
    uint32_t win_n_ = 0;                                                  // ... and how many it holds (0: nothing yet)
    uint16_t *ph_ = nullptr, *th_ = nullptr;                              // N / 2 + 1 each: every bin's last phase, and how far it was turned (65536 = a turn)
    uint32_t rng_ = 0;                                                    // paulstretch's phases
    // the resampler (Resample, and the second stage of Pitch)
    uint32_t s1_len_ = 0, s1_pos_ = 0;                                    // its input: length, frames taken
    uint64_t rs_step_ = 0;                                                // input frames per output frame, Q32
    int64_t rs_base_ = 0;                                                 // the input frame of rs_buf_[0]
    uint32_t rs_n_ = 0;
    q15 *rs_buf_ = nullptr;                                               // kRsBuf
    float *rs_tab_ = nullptr;                                             // (kRsPhases + 1) * kRsTaps: the kernel at kRsPhases + 1 fractions
};

}  // namespace sc
