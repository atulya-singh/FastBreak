// perf_counters.h — perf_event_open wrappers + latency histogram.
#ifndef UXNET_PERF_COUNTERS_H
#define UXNET_PERF_COUNTERS_H

#include <sys/types.h>  // pid_t

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <vector>

namespace uxnet {

enum class PerfEvent {
  L1_CACHE_MISSES,
  LLC_CACHE_MISSES,
  BRANCH_MISSES,
  INSTRUCTIONS,
  CYCLES,
  TLB_LOAD_MISSES,
  TLB_STORE_MISSES,
};

const char* perf_event_name(PerfEvent event);

class PerfCounter {
 public:
  PerfCounter(PerfEvent event, int cpu = -1, pid_t pid = 0);
  ~PerfCounter();

  PerfCounter(const PerfCounter&) = delete;
  PerfCounter& operator=(const PerfCounter&) = delete;

  void start();
  void stop();
  uint64_t read();

  bool valid() const { return fd_ >= 0; }
  PerfEvent event() const { return event_; }

 private:
  PerfEvent event_;
  int fd_ = -1;
};

// RAII: enables a set of counters on construction, disables on destruction.
class PerfScope {
 public:
  explicit PerfScope(std::initializer_list<PerfEvent> events, int cpu = -1,
                     pid_t pid = 0);
  ~PerfScope();

  PerfScope(const PerfScope&) = delete;
  PerfScope& operator=(const PerfScope&) = delete;

  // Reads every counter (0 if a given event is unavailable), 0 if not present.
  uint64_t count(PerfEvent event) const;
  void report() const;

 private:
  std::vector<std::unique_ptr<PerfCounter>> counters_;
};

// Fixed-granularity latency histogram, TSC-cycle input converted to ns.
class LatencyHistogram {
 public:
  LatencyHistogram();

  void record(uint64_t start_tsc, uint64_t end_tsc);
  void print() const;

  uint64_t count() const { return count_; }

 private:
  uint64_t percentile_ns(double p) const;

  std::vector<uint64_t> buckets_;
  uint64_t count_ = 0;
  uint64_t sum_ns_ = 0;
  uint64_t max_ns_ = 0;
};

}  // namespace uxnet

#endif  // UXNET_PERF_COUNTERS_H
