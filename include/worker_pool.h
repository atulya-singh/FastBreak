// worker_pool.h — Layer 3: CPU-pinned busy-polling workers draining RSS queues.
#ifndef UXNET_WORKER_POOL_H
#define UXNET_WORKER_POOL_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "buffer_pool.h"
#include "packet_parser.h"
#include "rss_distributor.h"

namespace uxnet {

struct WorkerConfig {
  int cpu_core;
  int numa_node;
  size_t queue_index;
  std::string name;
};

using PacketCallback =
    std::function<void(size_t queue_id, BufferDescriptor*, const ParsedPacket&)>;

struct alignas(64) WorkerStats {
  std::atomic<uint64_t> packets_processed{0};
  std::atomic<uint64_t> packets_dropped{0};
  std::atomic<uint64_t> idle_spins{0};
};

class WorkerPool {
 public:
  WorkerPool(RSSDistributor& rss, BufferPool& pool,
             const std::vector<WorkerConfig>& configs, PacketCallback callback);
  ~WorkerPool();

  WorkerPool(const WorkerPool&) = delete;
  WorkerPool& operator=(const WorkerPool&) = delete;

  // Increment the drop counter for whichever worker polls `queue_index`.
  // Producers call this when submit() finds the queue full.
  void record_drop(size_t queue_index);

  void stop();
  void print_stats() const;

 private:
  void worker_loop(size_t worker_id, WorkerConfig config);

  RSSDistributor& rss_;
  BufferPool& pool_;
  PacketCallback callback_;
  std::vector<WorkerConfig> configs_;
  std::unique_ptr<WorkerStats[]> stats_;
  std::vector<std::thread> threads_;
  std::atomic<bool> stop_{false};
};

}  // namespace uxnet

#endif  // UXNET_WORKER_POOL_H
