#include "buffer_pool.h"
#include "packet_parser.h"
#include "packet_ring.h"
#include "rss_distributor.h"
#include "worker_pool.h"

#include <atomic>
#include <array>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <stdexcept>
#include <vector>

namespace {

constexpr size_t kQueues = 4;

volatile sig_atomic_t g_stop = 0;
void on_sigint(int) { g_stop = 1; }

uint64_t now_ns() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + ts.tv_nsec;
}

std::array<std::atomic<uint64_t>, kQueues> g_queue_counts;

uint32_t queue_for(uxnet::RSSDistributor& rss, const uxnet::ParsedPacket& p) {
  if (p.has_udp) {
    return rss.hash(p.ip.src_ip(), p.ip.dst_ip(), p.udp.src_port(),
                    p.udp.dst_port());
  }
  if (p.has_ip) return rss.hash(p.ip.src_ip(), p.ip.dst_ip(), 0, 0);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <iface> [duration_secs]\n", argv[0]);
    return 1;
  }
  const int duration = argc >= 3 ? std::atoi(argv[2]) : 10;
  std::signal(SIGINT, on_sigint);

  try {
    uxnet::BufferPool pool(2048, 8192);
    uxnet::PacketRing ring(argv[1], 128);
    ring.set_busy_poll(50);
    uxnet::RSSDistributor rss(kQueues);

    std::vector<uxnet::WorkerConfig> configs;
    const int cores[kQueues] = {0, 2, 4, 6};
    for (size_t i = 0; i < kQueues; ++i) {
      configs.push_back({cores[i], -1, i, "worker" + std::to_string(i)});
    }

    uxnet::WorkerPool workers(
        rss, pool, configs,
        [](size_t queue_id, uxnet::BufferDescriptor*,
           const uxnet::ParsedPacket&) {
          g_queue_counts[queue_id].fetch_add(1, std::memory_order_relaxed);
        });

    const uint64_t start = now_ns();
    uint64_t last_tick = start;
    uxnet::RxFrame frame;

    while (!g_stop) {
      const uint64_t now = now_ns();
      if (now - start >= static_cast<uint64_t>(duration) * 1000000000ull) break;

      if (ring.receive(frame)) {
        uxnet::BufferDescriptor* buf = pool.acquire();
        if (buf != nullptr) {
          const uint32_t n = frame.captured_len < buf->capacity
                                 ? frame.captured_len
                                 : buf->capacity;
          std::memcpy(buf->data, frame.data, n);  // mmap ring -> pool buffer
          buf->len = n;
          const auto parsed = uxnet::PacketParser::parse(buf->data, buf->len);
          const uint32_t q = queue_for(rss, parsed);
          if (!rss.get_queue(q).push(buf)) {
            workers.record_drop(q);
            pool.release(buf);
          }
        }
        ring.release();
      }

      if (now - last_tick >= 1000000000ull) {
        std::printf("q0=%llu q1=%llu q2=%llu q3=%llu\n",
                    (unsigned long long)g_queue_counts[0].load(),
                    (unsigned long long)g_queue_counts[1].load(),
                    (unsigned long long)g_queue_counts[2].load(),
                    (unsigned long long)g_queue_counts[3].load());
        last_tick = now;
      }
    }

    workers.stop();
    std::printf("\n--- summary ---\n");
    workers.print_stats();
  } catch (const std::exception& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
  return 0;
}
