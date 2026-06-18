// worker_pool.cpp — Layer 3 implementation. See include/worker_pool.h.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE  // pthread_setname_np, pthread_setaffinity_np
#endif

#include "worker_pool.h"

#include <pthread.h>
#include <sched.h>

#include <cstdio>

namespace uxnet {

WorkerPool::WorkerPool(RSSDistributor& rss, BufferPool& pool,
                       const std::vector<WorkerConfig>& configs,
                       PacketCallback callback)
    : rss_(rss),
      pool_(pool),
      callback_(std::move(callback)),
      configs_(configs),
      stats_(std::make_unique<WorkerStats[]>(configs.size())) {
  threads_.reserve(configs_.size());
  for (size_t i = 0; i < configs_.size(); ++i) {
    threads_.emplace_back([this, i] { worker_loop(i, configs_[i]); });
  }
}

WorkerPool::~WorkerPool() { stop(); }

void WorkerPool::worker_loop(size_t worker_id, WorkerConfig config) {
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(config.cpu_core, &set);
  if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) != 0) {
    std::fprintf(stderr, "worker %s: pin to core %d failed\n",
                 config.name.c_str(), config.cpu_core);
  }

  pthread_setname_np(pthread_self(), config.name.substr(0, 15).c_str());

  // SCHED_FIFO real-time priority; needs CAP_SYS_NICE (or root). Non-fatal.
  sched_param sp;
  sp.sched_priority = 50;
  if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0) {
    std::fprintf(stderr,
                 "worker %s: SCHED_FIFO denied (needs CAP_SYS_NICE/root); "
                 "running at default priority\n",
                 config.name.c_str());
  }

  auto& queue = rss_.get_queue(config.queue_index);
  WorkerStats& st = stats_[worker_id];

  BufferDescriptor* buf = nullptr;
  uint64_t iters = 0;
  while (true) {
    if (queue.pop(buf)) {
      const ParsedPacket parsed = PacketParser::parse(buf->data, buf->len);
      callback_(config.queue_index, buf, parsed);
      pool_.release(buf);
      st.packets_processed.fetch_add(1, std::memory_order_relaxed);
    } else {
      st.idle_spins.fetch_add(1, std::memory_order_relaxed);
      std::atomic_thread_fence(std::memory_order_seq_cst);
    }
    if ((++iters % 1000) == 0 && stop_.load(std::memory_order_acquire)) break;
  }
}

void WorkerPool::record_drop(size_t queue_index) {
  for (size_t i = 0; i < configs_.size(); ++i) {
    if (configs_[i].queue_index == queue_index) {
      stats_[i].packets_dropped.fetch_add(1, std::memory_order_relaxed);
      return;
    }
  }
}

void WorkerPool::stop() {
  stop_.store(true, std::memory_order_release);
  for (std::thread& t : threads_) {
    if (t.joinable()) t.join();
  }
}

void WorkerPool::print_stats() const {
  uint64_t agg_processed = 0, agg_dropped = 0, agg_idle = 0;
  std::printf("%-16s %14s %12s %14s\n", "worker", "processed", "dropped",
              "idle_spins");
  for (size_t i = 0; i < configs_.size(); ++i) {
    const uint64_t p = stats_[i].packets_processed.load(std::memory_order_relaxed);
    const uint64_t d = stats_[i].packets_dropped.load(std::memory_order_relaxed);
    const uint64_t s = stats_[i].idle_spins.load(std::memory_order_relaxed);
    std::printf("%-16s %14llu %12llu %14llu\n", configs_[i].name.c_str(),
                (unsigned long long)p, (unsigned long long)d,
                (unsigned long long)s);
    agg_processed += p;
    agg_dropped += d;
    agg_idle += s;
  }
  std::printf("%-16s %14llu %12llu %14llu\n", "AGGREGATE",
              (unsigned long long)agg_processed, (unsigned long long)agg_dropped,
              (unsigned long long)agg_idle);
}

}  // namespace uxnet
