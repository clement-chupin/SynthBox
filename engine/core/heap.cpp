#include "engine/core/heap.h"

namespace sc {

static constexpr uint32_t kMagic = 0x5EA9C0DEu;
static constexpr size_t kAlign = 16;

Heap::Hdr *Heap::next(Hdr *h) const {
    uint8_t *n = reinterpret_cast<uint8_t *>(h) + sizeof(Hdr) + h->size;
    return n >= base_ + cap_ ? nullptr : reinterpret_cast<Hdr *>(n);
}

void Heap::init(void *mem, size_t size) {
    uintptr_t a = (reinterpret_cast<uintptr_t>(mem) + (kAlign - 1)) & ~(kAlign - 1);
    size_t lost = a - reinterpret_cast<uintptr_t>(mem);
    base_ = reinterpret_cast<uint8_t *>(a);
    cap_ = size > lost ? (size - lost) & ~(kAlign - 1) : 0;
    used_ = high_ = 0;
    if (cap_ > sizeof(Hdr)) {
        Hdr *h = reinterpret_cast<Hdr *>(base_);
        h->size = cap_ - sizeof(Hdr);
        h->is_free = 1;
        h->magic = kMagic;
    } else {
        cap_ = 0;
    }
}

void *Heap::alloc(size_t bytes) {
    if (!cap_) return spill_ ? spill_->alloc(bytes) : nullptr;
    size_t need = (bytes + (kAlign - 1)) & ~(kAlign - 1);
    if (need == 0) need = kAlign;
    for (Hdr *h = reinterpret_cast<Hdr *>(base_); h; h = next(h)) {
        if (!h->is_free || h->size < need) continue;
        if (h->size >= need + sizeof(Hdr) + kAlign) {            // split
            Hdr *r = reinterpret_cast<Hdr *>(reinterpret_cast<uint8_t *>(h) + sizeof(Hdr) + need);
            r->size = h->size - need - sizeof(Hdr);
            r->is_free = 1;
            r->magic = kMagic;
            h->size = need;
        }
        h->is_free = 0;
        used_ += h->size;
        if (used_ > high_) high_ = used_;
        uint8_t *p = reinterpret_cast<uint8_t *>(h) + sizeof(Hdr);
        for (size_t i = 0; i < h->size; i++) p[i] = 0;
        return p;
    }
    if (!spill_) return nullptr;
    void *q = spill_->alloc(bytes);
    if (q) spilled_ += bytes;
    return q;
}

void Heap::free(void *p) {
    if (!p) return;
    if (!owns(p)) { if (spill_) { spill_->free(p); } return; }       // a block that spilled into the other heap
    Hdr *h = reinterpret_cast<Hdr *>(static_cast<uint8_t *>(p) - sizeof(Hdr));
    if (h->magic != kMagic || h->is_free) return;               // bad / double free: ignore
    h->is_free = 1;
    used_ -= h->size;
    for (Hdr *c = reinterpret_cast<Hdr *>(base_); c;) {         // coalesce adjacent free blocks
        Hdr *n = next(c);
        if (c->is_free && n && n->is_free) c->size += sizeof(Hdr) + n->size;
        else c = n;
    }
}

size_t Heap::largest_free() const {
    size_t best = 0;
    for (const Hdr *h = reinterpret_cast<const Hdr *>(base_); h && cap_; h = next(const_cast<Hdr *>(h)))
        if (h->is_free && h->size > best) best = h->size;
    return best;
}

bool Heap::check() const {
    size_t sum = 0, payload = 0;
    for (const Hdr *h = reinterpret_cast<const Hdr *>(base_); h && cap_; h = next(const_cast<Hdr *>(h))) {
        if (h->magic != kMagic) return false;
        sum += sizeof(Hdr) + h->size;
        if (!h->is_free) payload += h->size;
    }
    return !cap_ || (sum == cap_ && payload == used_);
}

}  // namespace sc
