// perf_counters.cpp — see include/perf_counters.h.
#include "perf_counters.h"

#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

#include "rdtsc.h"

namespace uxnet {

namespace {

long perf_event_open(struct perf_event_attr* attr, pid_t pid, int cpu,
                     int group_fd, unsigned long flags) {
  return syscall(SYS_perf_event_open, attr, pid, cpu, group_fd, flags);
}

// Cache event config: (id) | (op << 8) | (result << 16).
uint64_t cache_config(uint64_t id, uint64_t op, uint64_t result) {
  return id | (op << 8) | (result << 16);
}

void fill_attr(PerfEvent event, struct perf_event_attr* attr) {
  std::memset(attr, 0, sizeof(*attr));
  attr->size = sizeof(*attr);
  attr->exclude_kernel = 0;
  attr->exclude_hv = 1;
  attr->disabled = 1;
  attr->inherit = 1;

  switch (event) {
    case PerfEvent::LLC_CACHE_MISSES:
      attr->type = PERF_TYPE_HARDWARE;
      attr->config = PERF_COUNT_HW_CACHE_MISSES;
      break;
    case PerfEvent::BRANCH_MISSES:
      attr->type = PERF_TYPE_HARDWARE;
      attr->config = PERF_COUNT_HW_BRANCH_MISSES;
      break;
    case PerfEvent::INSTRUCTIONS:
      attr->type = PERF_TYPE_HARDWARE;
      attr->config = PERF_COUNT_HW_INSTRUCTIONS;
      break;
    case PerfEvent::CYCLES:
      attr->type = PERF_TYPE_HARDWARE;
      attr->config = PERF_COUNT_HW_CPU_CYCLES;
      break;
    case PerfEvent::L1_CACHE_MISSES:
      attr->type = PERF_TYPE_HW_CACHE;
      attr->config = cache_config(PERF_COUNT_HW_CACHE_L1D,
                                  PERF_COUNT_HW_CACHE_OP_READ,
                                  PERF_COUNT_HW_CACHE_RESULT_MISS);
      break;
    case PerfEvent::TLB_LOAD_MISSES:
      attr->type = PERF_TYPE_HW_CACHE;
      attr->config = cache_config(PERF_COUNT_HW_CACHE_DTLB,
                                  PERF_COUNT_HW_CACHE_OP_READ,
                                  PERF_COUNT_HW_CACHE_RESULT_MISS);
      break;
    case PerfEvent::TLB_STORE_MISSES:
      attr->type = PERF_TYPE_HW_CACHE;
      attr->config = cache_config(PERF_COUNT_HW_CACHE_DTLB,
                                  PERF_COUNT_HW_CACHE_OP_WRITE,
                                  PERF_COUNT_HW_CACHE_RESULT_MISS);
      break;
  }
}

}  // namespace

const char* perf_event_name(PerfEvent event) {
  switch (event) {
    case PerfEvent::L1_CACHE_MISSES: return "L1_CACHE_MISSES";
    case PerfEvent::LLC_CACHE_MISSES: return "LLC_CACHE_MISSES";
    case PerfEvent::BRANCH_MISSES: return "BRANCH_MISSES";
    case PerfEvent::INSTRUCTIONS: return "INSTRUCTIONS";
    case PerfEvent::CYCLES: return "CYCLES";
    case PerfEvent::TLB_LOAD_MISSES: return "TLB_LOAD_MISSES";
    case PerfEvent::TLB_STORE_MISSES: return "TLB_STORE_MISSES";
  }
  return "UNKNOWN";
}

PerfCounter::PerfCounter(PerfEvent event, int cpu, pid_t pid) : event_(event) {
  struct perf_event_attr attr;
  fill_attr(event, &attr);
  fd_ = static_cast<int>(perf_event_open(&attr, pid, cpu, -1, 0));
  if (fd_ < 0) {
    std::fprintf(stderr, "PerfCounter(%s): perf_event_open failed: %s\n",
                 perf_event_name(event), std::strerror(errno));
    return;
  }
  ioctl(fd_, PERF_EVENT_IOC_RESET, 0);
}

PerfCounter::~PerfCounter() {
  if (fd_ >= 0) close(fd_);
}

void PerfCounter::start() {
  if (fd_ >= 0) ioctl(fd_, PERF_EVENT_IOC_ENABLE, 0);
}

void PerfCounter::stop() {
  if (fd_ >= 0) ioctl(fd_, PERF_EVENT_IOC_DISABLE, 0);
}

uint64_t PerfCounter::read() {
  if (fd_ < 0) return 0;
  uint64_t value = 0;
  if (::read(fd_, &value, sizeof(value)) != static_cast<ssize_t>(sizeof(value)))
    return 0;
  return value;
}

PerfScope::PerfScope(std::initializer_list<PerfEvent> events, int cpu,
                     pid_t pid) {
  counters_.reserve(events.size());
  for (PerfEvent e : events)
    counters_.push_back(std::make_unique<PerfCounter>(e, cpu, pid));
  for (auto& c : counters_) c->start();
}

PerfScope::~PerfScope() {
  for (auto& c : counters_) c->stop();
}

uint64_t PerfScope::count(PerfEvent event) const {
  for (const auto& c : counters_)
    if (c->event() == event) return c->read();
  return 0;
}

void PerfScope::report() const {
  for (auto& c : counters_) c->stop();
  bool have_ins = false, have_cyc = false;
  uint64_t ins = 0, cyc = 0;
  for (const auto& c : counters_) {
    const uint64_t v = c->read();
    std::printf("  %-18s %16llu%s\n", perf_event_name(c->event()),
                (unsigned long long)v, c->valid() ? "" : "  (unavailable)");
    if (c->event() == PerfEvent::INSTRUCTIONS) { have_ins = true; ins = v; }
    if (c->event() == PerfEvent::CYCLES) { have_cyc = true; cyc = v; }
  }
  if (have_ins && have_cyc && cyc > 0)
    std::printf("  %-18s %16.3f\n", "IPC",
                static_cast<double>(ins) / static_cast<double>(cyc));
}

namespace {

// Bucketing: 1ns [0,1k), 10ns [1k,10k), 100ns [10k,100k), 1us [100k,1M), of.
constexpr size_t kR0 = 1000;   // indices [0,1000)
constexpr size_t kR1 = 1900;   // [1000,1900)
constexpr size_t kR2 = 2800;   // [1900,2800)
constexpr size_t kR3 = 3700;   // [2800,3700)
constexpr size_t kOverflow = 3700;
constexpr size_t kNumBuckets = 3701;

size_t ns_to_bucket(uint64_t ns) {
  if (ns < 1000) return ns;
  if (ns < 10000) return kR0 + (ns - 1000) / 10;
  if (ns < 100000) return kR1 + (ns - 10000) / 100;
  if (ns < 1000000) return kR2 + (ns - 100000) / 1000;
  return kOverflow;
}

uint64_t bucket_lo_ns(size_t i) {
  if (i < kR0) return i;
  if (i < kR1) return 1000 + (i - kR0) * 10;
  if (i < kR2) return 10000 + (i - kR1) * 100;
  if (i < kR3) return 100000 + (i - kR2) * 1000;
  return 1000000;
}

uint64_t bucket_step_ns(size_t i) {
  if (i < kR0) return 1;
  if (i < kR1) return 10;
  if (i < kR2) return 100;
  if (i < kR3) return 1000;
  return 0;
}

}  // namespace

LatencyHistogram::LatencyHistogram() : buckets_(kNumBuckets, 0) {}

void LatencyHistogram::record(uint64_t start_tsc, uint64_t end_tsc) {
  if (end_tsc <= start_tsc) return;
  const uint64_t ns = static_cast<uint64_t>(cycles_to_ns(end_tsc - start_tsc));
  buckets_[ns_to_bucket(ns)]++;
  count_++;
  sum_ns_ += ns;
  if (ns > max_ns_) max_ns_ = ns;
}

uint64_t LatencyHistogram::percentile_ns(double p) const {
  if (count_ == 0) return 0;
  const uint64_t target = static_cast<uint64_t>(p * count_);
  uint64_t cum = 0;
  for (size_t i = 0; i < buckets_.size(); ++i) {
    cum += buckets_[i];
    if (cum >= target + 1) return bucket_lo_ns(i) + bucket_step_ns(i) / 2;
  }
  return max_ns_;
}

void LatencyHistogram::print() const {
  if (count_ == 0) {
    std::printf("LatencyHistogram: no samples\n");
    return;
  }
  std::printf("LatencyHistogram (%llu samples)\n", (unsigned long long)count_);
  std::printf("  p50   %8llu ns\n", (unsigned long long)percentile_ns(0.50));
  std::printf("  p90   %8llu ns\n", (unsigned long long)percentile_ns(0.90));
  std::printf("  p99   %8llu ns\n", (unsigned long long)percentile_ns(0.99));
  std::printf("  p99.9 %8llu ns\n", (unsigned long long)percentile_ns(0.999));
  std::printf("  p99.99%8llu ns\n", (unsigned long long)percentile_ns(0.9999));
  std::printf("  max   %8llu ns\n", (unsigned long long)max_ns_);
  std::printf("  mean  %8llu ns\n", (unsigned long long)(sum_ns_ / count_));

  const uint64_t edges[] = {100,    250,    500,    1000,   2500,
                            5000,   10000,  25000,  50000,  100000,
                            250000, 500000, 1000000, UINT64_MAX};
  const char* labels[] = {"  <100ns", "100-250ns", "250-500ns", "0.5-1us",
                          "1-2.5us",  "2.5-5us",   "5-10us",     "10-25us",
                          "25-50us",  "50-100us",  "0.1-0.25ms", "0.25-0.5ms",
                          "0.5-1ms",  "  >1ms"};
  const size_t nrows = sizeof(edges) / sizeof(edges[0]);
  uint64_t rows[nrows];
  std::memset(rows, 0, sizeof(rows));
  for (size_t i = 0; i < buckets_.size(); ++i) {
    if (buckets_[i] == 0) continue;
    const uint64_t lo = bucket_lo_ns(i);
    for (size_t r = 0; r < nrows; ++r) {
      if (lo < edges[r]) { rows[r] += buckets_[i]; break; }
    }
  }
  uint64_t rmax = 1;
  for (size_t r = 0; r < nrows; ++r) rmax = rows[r] > rmax ? rows[r] : rmax;
  std::printf("  histogram:\n");
  for (size_t r = 0; r < nrows; ++r) {
    const int bars = static_cast<int>(rows[r] * 40 / rmax);
    std::printf("    %-12s %10llu |", labels[r], (unsigned long long)rows[r]);
    for (int b = 0; b < bars; ++b) std::putchar('#');
    std::putchar('\n');
  }
}

}  // namespace uxnet
