#include "buffer_pool.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <thread>

namespace {

uint64_t now_ns() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + ts.tv_nsec;
}

// Keeps the optimizer from deleting the acquire/release pair.
std::atomic<uint64_t> g_sink{0};

void bench_single_thread() {
  constexpr size_t kBufSize = 2048;
  constexpr size_t kBufCount = 1024;
  constexpr uint64_t kIters = 10'000'000ull;

  uxnet::BufferPool pool(kBufSize, kBufCount);

  const uint64_t start = now_ns();
  uint64_t acc = 0;
  for (uint64_t i = 0; i < kIters; ++i) {
    uxnet::BufferDescriptor* b = pool.acquire();
    b->len = static_cast<uint32_t>(i);
    acc += reinterpret_cast<uintptr_t>(b->data);
    pool.release(b);
  }
  const uint64_t elapsed = now_ns() - start;
  g_sink.fetch_add(acc, std::memory_order_relaxed);

  std::printf("[single-thread] %llu acquire+release cycles\n",
              (unsigned long long)kIters);
  std::printf("  total          : %.3f ms\n", elapsed / 1e6);
  std::printf("  per op         : %.2f ns\n", double(elapsed) / kIters);
}

// Minimal SPSC index channel: a power-of-two ring with acquire/release cursors.
class IndexChannel {
 public:
  bool try_push(uint32_t v) {
    const uint64_t head = head_.load(std::memory_order_relaxed);
    if (head - tail_.load(std::memory_order_acquire) >= kCap) return false;
    slots_[head & (kCap - 1)] = v;
    head_.store(head + 1, std::memory_order_release);
    return true;
  }
  bool try_pop(uint32_t& v) {
    const uint64_t tail = tail_.load(std::memory_order_relaxed);
    if (tail == head_.load(std::memory_order_acquire)) return false;
    v = slots_[tail & (kCap - 1)];
    tail_.store(tail + 1, std::memory_order_release);
    return true;
  }

 private:
  static constexpr uint64_t kCap = 1024;
  uint32_t slots_[kCap];
  std::atomic<uint64_t> head_{0};
  std::atomic<uint64_t> tail_{0};
};

void bench_producer_consumer() {
  constexpr size_t kBufSize = 2048;
  constexpr size_t kBufCount = 4096;
  constexpr uint64_t kIters = 5'000'000ull;

  uxnet::BufferPool pool(kBufSize, kBufCount);
  IndexChannel chan;

  const uint64_t start = now_ns();

  std::thread producer([&] {
    for (uint64_t i = 0; i < kIters; ++i) {
      uxnet::BufferDescriptor* b;
      while ((b = pool.acquire()) == nullptr) { /* pool drained, spin */ }
      while (!chan.try_push(b->pool_index)) { /* channel full, spin */ }
    }
  });

  uint64_t consumed = 0;
  uint32_t index;
  while (consumed < kIters) {
    if (chan.try_pop(index)) {
      pool.release(pool.descriptor_at(index));
      ++consumed;
    }
  }
  producer.join();

  const uint64_t elapsed = now_ns() - start;
  std::printf("[producer/consumer] %llu round trips over a %zu-buffer pool\n",
              (unsigned long long)kIters, kBufCount);
  std::printf("  total          : %.3f ms\n", elapsed / 1e6);
  std::printf("  per round trip : %.2f ns\n", double(elapsed) / kIters);
}

void bench_exhaustion() {
  constexpr size_t kBufSize = 2048;
  constexpr size_t kBufCount = 16;

  uxnet::BufferPool pool(kBufSize, kBufCount);

  uxnet::BufferDescriptor* held[kBufCount];
  for (size_t i = 0; i < kBufCount; ++i) held[i] = pool.acquire();

  const bool all_taken = [&] {
    for (size_t i = 0; i < kBufCount; ++i)
      if (held[i] == nullptr) return false;
    return true;
  }();
  const bool empty_returns_null = (pool.acquire() == nullptr);

  for (size_t i = 0; i < kBufCount; ++i) pool.release(held[i]);
  const size_t free_after_release = pool.free_count();
  const bool refilled = (free_after_release == kBufCount);

  const bool reacquire_ok = [&] {
    for (size_t i = 0; i < kBufCount; ++i)
      if (pool.acquire() == nullptr) return false;
    return true;
  }();

  std::printf("[exhaustion] pool of %zu buffers\n", kBufCount);
  std::printf("  all acquired         : %s\n", all_taken ? "yes" : "no");
  std::printf("  empty -> nullptr     : %s\n", empty_returns_null ? "yes" : "no");
  std::printf("  free after release   : %zu (expect %zu)\n", free_after_release,
              kBufCount);
  std::printf("  all available again  : %s\n", refilled ? "yes" : "no");
  std::printf("  reacquire all        : %s\n", reacquire_ok ? "yes" : "no");
  std::printf("  total_exhausted      : %llu\n",
              (unsigned long long)pool.total_exhausted());

  const bool pass =
      all_taken && empty_returns_null && refilled && reacquire_ok;
  std::printf("  RESULT               : %s\n", pass ? "PASS" : "FAIL");
}

}  // namespace

int main() {
  bench_single_thread();
  std::printf("\n");
  bench_producer_consumer();
  std::printf("\n");
  bench_exhaustion();
  return 0;
}
