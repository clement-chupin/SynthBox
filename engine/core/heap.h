#pragma once
// Control-time heap over a caller-provided block (ADR-008). First fit, splitting, full coalescing on free.
// 16-byte aligned, zero-filled allocations. Used by plan compilation and module creation ([INIT]/[CONTROL]);
// it is never called from the audio path, so it needs neither to be fast nor lock-free.
#include <cstddef>
#include <cstdint>
#include <new>
#include <utility>

namespace sc {

class Heap {
public:
    void init(void *mem, size_t size);
    void *alloc(size_t bytes);              // nullptr when out of memory
    void free(void *p);                     // nullptr is ignored

    template <typename T, typename... A>
    T *make(A &&...a) {
        void *p = alloc(sizeof(T));
        return p ? new (p) T(std::forward<A>(a)...) : nullptr;
    }
    template <typename T>
    T *alloc_array(size_t n) { return static_cast<T *>(alloc(sizeof(T) * n)); }

    // When this heap is full, alloc() takes the block from `h` instead (a slower memory), and free() hands such a block back to it.
    // Lets a small fast heap (internal RAM) degrade to the big one (PSRAM) instead of failing.
    void set_spill(Heap *h) { spill_ = h; }
    bool owns(const void *p) const { return p >= base_ && p < base_ + cap_; }

    size_t spilled() const { return spilled_; }           // bytes that were taken from the spill heap because this one was full (cumulative)
    size_t used() const { return used_; }               // payload bytes currently allocated
    size_t high_water() const { return high_; }
    size_t capacity() const { return cap_; }
    size_t largest_free() const;
    bool check() const;                                  // heap structure is consistent (tests)

private:
    struct alignas(16) Hdr { size_t size; uint32_t is_free; uint32_t magic; };
    Hdr *next(Hdr *h) const;
    uint8_t *base_ = nullptr;
    size_t cap_ = 0, used_ = 0, high_ = 0, spilled_ = 0;
    Heap *spill_ = nullptr;
};

}  // namespace sc
