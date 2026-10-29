#pragma once
// Single-producer single-consumer ring of fixed-size commands (ADR-024): the UI / control thread posts, the audio
// thread drains at the start of every block. No locks, no allocation; a full queue rejects the command (the
// caller sees `false`) instead of blocking.
#include <atomic>
#include <cstdint>
#include <cstring>

namespace sc {

enum class Cmd : uint8_t { None, NoteOn, NoteOff, AllNotesOff, SetParam, SetBlob, SetDepth, VoiceMode, Transport, SeqSetup, SeqSteps, PadSeqSetup, PadSeqHits };   // VoiceMode: node = mode, idx = legato, value = glide coefficient (q15, 0 = none); Transport: node = running, idx = click, value = bpm; SeqSetup / SeqSteps: blob = SeqSetup / SeqChunk (sequencer.h); PadSeqSetup / PadSeqHits: blob = SeqSetup / PadHitChunk, node = the Pads module's node id

constexpr int kCmdBlobMax = 256;

struct Command {
    Cmd type = Cmd::None;
    uint8_t node = 0;           // node id (SetParam / SetBlob); edge index (SetDepth)
    uint8_t idx = 0;            // parameter index
    uint8_t note = 0;           // NoteOn / NoteOff
    int32_t value = 0;          // parameter value / velocity
    uint16_t size = 0;          // SetBlob payload size
    uint8_t blob[kCmdBlobMax];
};

template <int N>
class CommandRing {
    static_assert((N & (N - 1)) == 0, "N must be a power of two");
public:
    bool push(const Command &c) {                                   // [CONTROL] producer
        const uint32_t h = head_.load(std::memory_order_relaxed);
        if (h - tail_.load(std::memory_order_acquire) >= static_cast<uint32_t>(N)) return false;
        slot_[h & (N - 1)] = c;
        head_.store(h + 1, std::memory_order_release);
        return true;
    }
    bool pop(Command &c) {                                          // [AUDIO] consumer
        const uint32_t t = tail_.load(std::memory_order_relaxed);
        if (t == head_.load(std::memory_order_acquire)) return false;
        c = slot_[t & (N - 1)];
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }
    uint32_t size() const { return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire); }

private:
    Command slot_[N];
    std::atomic<uint32_t> head_{0}, tail_{0};
};

}  // namespace sc
