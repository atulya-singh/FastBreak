#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "spsc_ring.h"

#include <pthread.h>
#include <sched.h>
#include <x86intrin.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <thread>
#include <vector>

namespace {

constexpr int kProducerCore = 0;
constexpr int kConsumerCore = 2;

uint64_t now_ns() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + ts.tv_nsec;
}

inline uint64_t rdtscp() {
  unsigned aux;
  return __rdtscp(&aux);
}

bool pin_thread(int core) {
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(core, &set);
  return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
}

double calibrate_cycles_per_ns() {
  const uint64_t c0 = __rdtsc();
  const uint64_t n0 = now_ns();
  while (now_ns() - n0 < 200000000ull) { /* ~200 ms */ }
  const uint64_t c1 = __rdtsc();
  const uint64_t n1 = now_ns();
  return static_cast<double>(c1 - c0) / static_cast<double>(n1 - n0);
}

template <size_t N>
double throughput_mops(uint64_t items, bool* checksum_ok) {
  auto ring = std::make_unique<uxnet::SPSCRing<uint64_t, N>>();
  std::atomic<uint64_t> consumer_sum{0};

  const uint64_t start = now_ns();
  std::thread producer([&] {
    pin_thread(kProducerCore);
    for (uint64_t i = 0; i < items; ++i) {
      while (!ring->push(i)) { /* spin on full */ }
    }
  });
  std::thread consumer([&] {
    pin_thread(kConsumerCore);
    uint64_t sum = 0, v = 0;
    for (uint64_t n = 0; n < items; ++n) {
      while (!ring->pop(v)) { /* spin on empty */ }
      sum += v;
    }
    consumer_sum.store(sum);
  });
  producer.join();
  consumer.join();
  const double secs = (now_ns() - start) / 1e9;

  if (checksum_ok != nullptr) {
    const uint64_t expected = items * (items - 1) / 2;  // sum 0..items-1
    *checksum_ok = (consumer_sum.load() == expected);
  }
  return items / secs / 1e6;
}

void bench_throughput(uint64_t items) {
  bool ok = false;
  const double mops = throughput_mops<1024>(items, &ok);
  std::printf("[throughput] %llu items, ring=1024, cores %d->%d\n",
              (unsigned long long)items, kProducerCore, kConsumerCore);
  std::printf("  rate            : %.1f Mops/sec\n", mops);
  std::printf("  checksum        : %s\n", ok ? "OK (no items lost)" : "MISMATCH");
}

void bench_latency(uint64_t iters, double cyc_per_ns) {
  auto req = std::make_unique<uxnet::SPSCRing<uint64_t, 64>>();
  auto resp = std::make_unique<uxnet::SPSCRing<uint64_t, 64>>();
  std::vector<uint64_t> samples(iters);

  std::thread consumer([&] {
    pin_thread(kConsumerCore);
    uint64_t v = 0;
    for (uint64_t i = 0; i < iters; ++i) {
      while (!req->pop(v)) { /* spin */ }
      while (!resp->push(v)) { /* spin */ }
    }
  });

  pin_thread(kProducerCore);
  for (uint64_t i = 0; i < iters; ++i) {
    const uint64_t t0 = rdtscp();
    while (!req->push(i)) { /* spin */ }
    uint64_t v;
    while (!resp->pop(v)) { /* spin */ }
    const uint64_t t1 = rdtscp();
    samples[i] = t1 - t0;
  }
  consumer.join();

  std::sort(samples.begin(), samples.end());
  auto ns_at = [&](double q) {
    const size_t idx = static_cast<size_t>(q * (iters - 1));
    return samples[idx] / cyc_per_ns;
  };
  std::printf("[latency] %llu round trips, cores %d<->%d\n",
              (unsigned long long)iters, kProducerCore, kConsumerCore);
  std::printf("  p50             : %.1f ns\n", ns_at(0.50));
  std::printf("  p99             : %.1f ns\n", ns_at(0.99));
  std::printf("  p99.9           : %.1f ns\n", ns_at(0.999));
}

void bench_size_sweep(uint64_t items) {
  std::printf("[size sweep] %llu items per run, cores %d->%d\n",
              (unsigned long long)items, kProducerCore, kConsumerCore);
  std::printf("  %8s  %12s\n", "ring", "Mops/sec");
  bool ok;
  std::printf("  %8d  %12.1f\n", 64, throughput_mops<64>(items, &ok));
  std::printf("  %8d  %12.1f\n", 256, throughput_mops<256>(items, &ok));
  std::printf("  %8d  %12.1f\n", 1024, throughput_mops<1024>(items, &ok));
  std::printf("  %8d  %12.1f\n", 4096, throughput_mops<4096>(items, &ok));
  std::printf("  %8d  %12.1f\n", 16384, throughput_mops<16384>(items, &ok));
  std::printf("  %8d  %12.1f\n", 65536, throughput_mops<65536>(items, &ok));
}

}  // namespace

int main(int argc, char** argv) {
  const bool quick = (argc > 1 && std::strcmp(argv[1], "quick") == 0);

  if (!pin_thread(kProducerCore)) {
    std::fprintf(stderr,
                 "warning: CPU pinning failed; results reflect unpinned "
                 "threads\n");
  }
  const double cyc_per_ns = calibrate_cycles_per_ns();
  std::printf("TSC: %.3f cycles/ns\n\n", cyc_per_ns);

  bench_throughput(quick ? 20'000'000ull : 500'000'000ull);
  std::printf("\n");
  bench_latency(quick ? 100'000ull : 1'000'000ull, cyc_per_ns);
  std::printf("\n");
  bench_size_sweep(quick ? 5'000'000ull : 100'000'000ull);
  return 0;
}
