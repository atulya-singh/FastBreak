// buffer_pool.h — Layer 2: fixed-size buffer pool over a huge-page backing store.
//
// A BufferPool carves one contiguous huge-page region into `buffer_count`
// equal-sized buffers and hands them out through a lock-free free list. There is
// zero allocation on the hot path: acquire()/release() are a single CAS each.
//
// The free list is a Treiber stack whose 64-bit head packs a 32-bit ABA
// generation counter above a 32-bit slot index. Bumping the generation on every
// update is the Michael-Scott fix for ABA: a slot that is popped, reused, and
// pushed back cannot masquerade as the head we last observed, because the
// generation half will have moved on.
#ifndef UXNET_BUFFER_POOL_H
#define UXNET_BUFFER_POOL_H

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace uxnet {

// One buffer's metadata, cache-line sized so descriptors never share a line.
// While a buffer is free its slot index chain lives in the first 4 padding
// bytes (see FreeStack); the field is only meaningful off the free list.
struct BufferDescriptor {
  uint8_t* data;         // into the pool backing store
  uint32_t len;          // bytes currently used
  uint32_t capacity;     // == pool buffer_size, constant for the buffer's life
  uint32_t pool_index;   // this buffer's slot, for O(1) return
  uint8_t _pad[44];
};
static_assert(sizeof(BufferDescriptor) == 64,
              "BufferDescriptor must be exactly one cache line");

// Lock-free intrusive stack of free slot indices. Not copyable/movable. The
// backing descriptor array is attached once, before first use, and the `next`
// link for each free slot is overlaid on that descriptor's padding.
class FreeStack {
 public:
  static constexpr uint32_t kEmpty = 0xFFFFFFFFu;

  FreeStack() : top_(pack(0, kEmpty)) {}
  FreeStack(const FreeStack&) = delete;
  FreeStack& operator=(const FreeStack&) = delete;

  void attach(BufferDescriptor* descriptors) { descriptors_ = descriptors; }

  void push(uint32_t index);
  uint32_t pop();  // kEmpty when the stack is empty

  // Approximate: read without locking, may lag concurrent push/pop.
  uint32_t approx_size() const { return size_.load(std::memory_order_relaxed); }

 private:
  static uint64_t pack(uint32_t gen, uint32_t index) {
    return (static_cast<uint64_t>(gen) << 32) | index;
  }
  static uint32_t gen_of(uint64_t v) { return static_cast<uint32_t>(v >> 32); }
  static uint32_t index_of(uint64_t v) { return static_cast<uint32_t>(v); }

  // The overlaid `next` link for a free slot.
  static uint32_t& next_of(BufferDescriptor& d) {
    return *reinterpret_cast<uint32_t*>(d._pad);
  }

  std::atomic<uint64_t> top_;
  std::atomic<uint32_t> size_{0};
  BufferDescriptor* descriptors_ = nullptr;
};

class BufferPool {
 public:
  // Allocates two huge-page regions: buffer_count*buffer_size of backing store
  // and buffer_count descriptors. numa_node >= 0 requests a best-effort bind of
  // both regions to that node; -1 leaves placement to the kernel. Throws
  // std::runtime_error on allocation failure.
  BufferPool(size_t buffer_size, size_t buffer_count, int numa_node = -1);
  ~BufferPool();

  BufferPool(const BufferPool&) = delete;
  BufferPool& operator=(const BufferPool&) = delete;
  BufferPool(BufferPool&&) = delete;
  BufferPool& operator=(BufferPool&&) = delete;

  // Returns a free buffer, or nullptr when the pool is exhausted.
  BufferDescriptor* acquire();

  // Returns a buffer to the pool. buf must have come from this pool.
  void release(BufferDescriptor* buf);

  // Descriptor for a raw slot index, e.g. when only the index crossed a queue.
  BufferDescriptor* descriptor_at(uint32_t index) { return &descriptors_[index]; }

  size_t free_count() const { return free_stack_.approx_size(); }
  size_t capacity() const { return buffer_count_; }
  size_t buffer_size() const { return buffer_size_; }

  uint64_t total_acquires() const {
    return total_acquires_.load(std::memory_order_relaxed);
  }
  uint64_t total_releases() const {
    return total_releases_.load(std::memory_order_relaxed);
  }
  uint64_t total_exhausted() const {
    return total_exhausted_.load(std::memory_order_relaxed);
  }

 private:
  size_t buffer_size_;
  size_t buffer_count_;
  int numa_node_;

  uint8_t* backing_ = nullptr;
  BufferDescriptor* descriptors_ = nullptr;
  FreeStack free_stack_;

  std::atomic<uint64_t> total_acquires_{0};
  std::atomic<uint64_t> total_releases_{0};
  std::atomic<uint64_t> total_exhausted_{0};
};

}  // namespace uxnet

#endif  // UXNET_BUFFER_POOL_H
