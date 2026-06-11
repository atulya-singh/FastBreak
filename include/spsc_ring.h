// spsc_ring.h — Layer 3: single-producer/single-consumer lock-free ring.
#ifndef UXNET_SPSC_RING_H
#define UXNET_SPSC_RING_H

#include <atomic>
#include <cstddef>

namespace uxnet {

template <typename T, size_t N>
class SPSCRing {
  static_assert(N != 0 && (N & (N - 1)) == 0, "N must be a power of two");

 public:
  bool push(const T& item) noexcept {
    const size_t tail = tail_.load(std::memory_order_relaxed);
    const size_t head = head_.load(std::memory_order_acquire);
    if (tail - head == N) return false;
    slots_[tail & (N - 1)] = item;
    tail_.store(tail + 1, std::memory_order_release);
    return true;
  }

  bool pop(T& item) noexcept {
    const size_t head = head_.load(std::memory_order_relaxed);
    const size_t tail = tail_.load(std::memory_order_acquire);
    if (head == tail) return false;
    item = slots_[head & (N - 1)];
    head_.store(head + 1, std::memory_order_release);
    return true;
  }

  bool empty() const noexcept {
    return head_.load(std::memory_order_acquire) ==
           tail_.load(std::memory_order_acquire);
  }

  size_t size() const noexcept {
    return tail_.load(std::memory_order_acquire) -
           head_.load(std::memory_order_acquire);
  }

  static constexpr size_t head_offset() { return offsetof(SPSCRing, head_); }
  static constexpr size_t tail_offset() { return offsetof(SPSCRing, tail_); }

 private:
  alignas(64) std::atomic<size_t> head_{0};
  char _pad0[64 - sizeof(std::atomic<size_t>)];
  alignas(64) std::atomic<size_t> tail_{0};
  char _pad1[64 - sizeof(std::atomic<size_t>)];
  alignas(64) T slots_[N];
};

static_assert(sizeof(SPSCRing<size_t, 2>) % 64 == 0,
              "SPSCRing must be a whole number of cache lines");

}  // namespace uxnet

#endif  // UXNET_SPSC_RING_H
