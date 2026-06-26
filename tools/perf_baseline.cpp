// perf_baseline.cpp — quantify the huge-page TLB benefit for the buffer pool's
// acquire/release hot path. Runs an identical per-buffer touch workload over a
// 4K-page-backed region and a 2MB-huge-page-backed region and compares the
// hardware counters (this is the source of the "reduced TLB misses by X%" metric).
#include <sys/mman.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#include "perf_counters.h"
#include "rdtsc.h"

using namespace uxnet;

namespace {

constexpr size_t kBufSize = 2048;
constexpr size_t kBufCount = 65536;          // 128 MB working set
constexpr uint64_t kPasses = 64;             // touch each buffer many times

struct Region {
  uint8_t* base = nullptr;
  size_t bytes = 0;
  bool huge = false;
};

Region alloc_4k(size_t bytes) {
  void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
  if (p == MAP_FAILED) { std::perror("mmap 4k"); return {}; }
  return {static_cast<uint8_t*>(p), bytes, false};
}

Region alloc_huge(size_t bytes) {
  const size_t kHuge = 2UL * 1024 * 1024;
  const size_t rounded = (bytes + kHuge - 1) / kHuge * kHuge;
  void* p = mmap(nullptr, rounded, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | MAP_POPULATE, -1, 0);
  if (p == MAP_FAILED) {
    std::fprintf(stderr,
                 "mmap MAP_HUGETLB failed (%s) — falling back to 4K pages; "
                 "huge-page column will NOT reflect a real huge-page mapping\n",
                 std::strerror(errno));
    return alloc_4k(bytes);
  }
  return {static_cast<uint8_t*>(p), rounded, true};
}

// Acquire buffer -> touch first cache line (write+read) -> release, in a
// shuffled order so page translations churn. Mirrors the pool's hot path with
// page size as the only variable. Returns a checksum to defeat elision.
uint64_t run_workload(const Region& r, const std::vector<uint32_t>& order) {
  uint64_t sum = 0;
  for (uint64_t p = 0; p < kPasses; ++p) {
    for (uint32_t idx : order) {
      volatile uint64_t* slot =
          reinterpret_cast<volatile uint64_t*>(r.base + idx * kBufSize);
      *slot = idx + p;
      sum += *slot;
    }
  }
  return sum;
}

struct Metrics {
  uint64_t l1_miss, llc_miss, dtlb_load_miss, dtlb_store_miss, ins, cyc;
  double ipc() const { return cyc ? static_cast<double>(ins) / cyc : 0.0; }
};

Metrics measure(const Region& r, const std::vector<uint32_t>& order) {
  PerfScope scope({PerfEvent::L1_CACHE_MISSES, PerfEvent::LLC_CACHE_MISSES,
                   PerfEvent::TLB_LOAD_MISSES, PerfEvent::TLB_STORE_MISSES,
                   PerfEvent::INSTRUCTIONS, PerfEvent::CYCLES});
  volatile uint64_t sink = run_workload(r, order);
  (void)sink;
  return {scope.count(PerfEvent::L1_CACHE_MISSES),
          scope.count(PerfEvent::LLC_CACHE_MISSES),
          scope.count(PerfEvent::TLB_LOAD_MISSES),
          scope.count(PerfEvent::TLB_STORE_MISSES),
          scope.count(PerfEvent::INSTRUCTIONS),
          scope.count(PerfEvent::CYCLES)};
}

double reduction_pct(uint64_t base, uint64_t improved) {
  if (base == 0) return 0.0;
  return (static_cast<double>(base) - improved) / base * 100.0;
}

void row(const char* name, uint64_t a, uint64_t b) {
  std::printf("  %-18s %16llu %16llu %13.1f%%\n", name, (unsigned long long)a,
              (unsigned long long)b, reduction_pct(a, b));
}

}  // namespace

int main() {
  const size_t bytes = kBufSize * kBufCount;

  std::vector<uint32_t> order(kBufCount);
  for (size_t i = 0; i < kBufCount; ++i) order[i] = static_cast<uint32_t>(i);
  std::mt19937 rng(0xC0FFEE);
  std::shuffle(order.begin(), order.end(), rng);

  Region r4k = alloc_4k(bytes);
  Region rhuge = alloc_huge(bytes);
  if (r4k.base == nullptr || rhuge.base == nullptr) {
    std::fprintf(stderr, "allocation failed; aborting\n");
    return 1;
  }

  std::printf("perf_baseline: %zu buffers x %zu B = %.0f MB working set, "
              "%llu passes\n",
              kBufCount, kBufSize, bytes / 1e6, (unsigned long long)kPasses);
  std::printf("huge region backed by: %s\n\n",
              rhuge.huge ? "2MB huge pages" : "4K pages (HUGETLB unavailable)");

  // Warm up + verify counters are actually available.
  {
    PerfCounter probe(PerfEvent::TLB_LOAD_MISSES);
    if (!probe.valid()) {
      std::fprintf(stderr,
                   "perf counters unavailable (need perf_event_paranoid <= 1 or "
                   "CAP_PERFMON). Table will show zeros.\n\n");
    }
  }

  const Metrics m4k = measure(r4k, order);
  const Metrics mhuge = measure(rhuge, order);

  std::printf("  %-18s %16s %16s %14s\n", "counter", "4K pages", "huge pages",
              "reduction");
  std::printf("  ------------------------------------------------------------------------\n");
  row("L1_CACHE_MISSES", m4k.l1_miss, mhuge.l1_miss);
  row("LLC_CACHE_MISSES", m4k.llc_miss, mhuge.llc_miss);
  row("TLB_LOAD_MISSES", m4k.dtlb_load_miss, mhuge.dtlb_load_miss);
  row("TLB_STORE_MISSES", m4k.dtlb_store_miss, mhuge.dtlb_store_miss);
  row("INSTRUCTIONS", m4k.ins, mhuge.ins);
  row("CYCLES", m4k.cyc, mhuge.cyc);
  std::printf("  %-18s %16.3f %16.3f\n", "IPC", m4k.ipc(), mhuge.ipc());

  const uint64_t tlb_4k = m4k.dtlb_load_miss + m4k.dtlb_store_miss;
  const uint64_t tlb_huge = mhuge.dtlb_load_miss + mhuge.dtlb_store_miss;
  std::printf("\n  => total dTLB misses reduced by %.1f%% with huge pages\n",
              reduction_pct(tlb_4k, tlb_huge));

  munmap(r4k.base, r4k.bytes);
  munmap(rhuge.base, rhuge.bytes);
  return 0;
}
