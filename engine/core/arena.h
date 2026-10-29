#pragma once
// Bump allocator over a caller-provided memory block (ADR-001: no heap in the engine).
// [INIT] only: modules take their state and delay buffers from an arena when they are created or when
// a plan is compiled; nothing allocates on the audio path. mark()/rewind() lets a plan compiler build
// a candidate plan and throw it away.
#include <cstddef>
#include <cstdint>

namespace sc {

class Arena {
public:
    Arena() = default;
    Arena(void *mem, size_t size) { init(mem, size); }

    void init(void *mem, size_t size) {
        base_ = static_cast<uint8_t *>(mem);
        size_ = size;
        used_ = 0;
        high_ = 0;
        failed_ = false;
    }

    // Returns nullptr (and latches failed()) when the arena is exhausted. Memory is zero-filled.
    void *alloc(size_t bytes, size_t align = 8) {
        size_t start = (used_ + (align - 1)) & ~(align - 1);
        if (!base_ || start + bytes > size_) { failed_ = true; return nullptr; }
        used_ = start + bytes;
        if (used_ > high_) high_ = used_;
        uint8_t *p = base_ + start;
        for (size_t i = 0; i < bytes; i++) p[i] = 0;
        return p;
    }

    template <typename T>
    T *alloc_array(size_t n, size_t align = alignof(T)) { return static_cast<T *>(alloc(sizeof(T) * n, align < alignof(T) ? alignof(T) : align)); }

    size_t mark() const { return used_; }
    void rewind(size_t mark) { if (mark <= used_) used_ = mark; }
    void reset() { used_ = 0; failed_ = false; }

    size_t used() const { return used_; }
    size_t capacity() const { return size_; }
    size_t high_water() const { return high_; }
    bool failed() const { return failed_; }

private:
    uint8_t *base_ = nullptr;
    size_t size_ = 0, used_ = 0, high_ = 0;
    bool failed_ = false;
};

}  // namespace sc
