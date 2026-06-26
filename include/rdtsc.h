// rdtsc.h — TSC read + calibration helpers for latency measurement.
#ifndef UXNET_RDTSC_H
#define UXNET_RDTSC_H

#include <x86intrin.h>

#include <cstdint>
#include <ctime>

namespace uxnet {

inline uint64_t rdtsc() {
  __asm__ __volatile__("" ::: "memory");
  const uint64_t t = __rdtsc();
  __asm__ __volatile__("" ::: "memory");
  return t;
}

inline uint64_t rdtscp() {
  unsigned aux;
  const uint64_t t = __rdtscp(&aux);  // serializing: waits for prior insns
  __asm__ __volatile__("" ::: "memory");
  return t;
}

inline uint64_t mono_ns() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + ts.tv_nsec;
}

// Cycles per nanosecond == GHz. Calibrated once by sleeping 100 ms and
// comparing the CLOCK_MONOTONIC delta to the TSC delta; cached thereafter.
inline double tsc_frequency_ghz() {
  static double cached = 0.0;
  if (cached != 0.0) return cached;
  struct timespec req {
    0, 100000000L
  };
  const uint64_t c0 = rdtsc();
  const uint64_t n0 = mono_ns();
  nanosleep(&req, nullptr);
  const uint64_t c1 = rdtsc();
  const uint64_t n1 = mono_ns();
  cached = static_cast<double>(c1 - c0) / static_cast<double>(n1 - n0);
  return cached;
}

inline double cycles_to_ns(uint64_t cycles) {
  return static_cast<double>(cycles) / tsc_frequency_ghz();
}

}  // namespace uxnet

#endif  // UXNET_RDTSC_H
