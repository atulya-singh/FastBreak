#include "packet_ring.h"

#include <cstdint>
#include <cstdio>
#include <ctime>
#include <stdexcept>

namespace {
uint64_t now_ns() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + ts.tv_nsec;
}
}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <iface>\n", argv[0]);
    return 1;
  }

  try {
  uxnet::PacketRing ring(argv[1], 128);
  ring.set_busy_poll(50);

  constexpr uint64_t kRunNs = 10ull * 1000000000ull;
  const uint64_t start = now_ns();

  uint64_t packets = 0, bytes = 0;
  uint32_t min_size = UINT32_MAX, max_size = 0;

  uxnet::RxFrame frame;
  while (now_ns() - start < kRunNs) {
    if (ring.receive(frame)) {
      ++packets;
      bytes += frame.len;
      if (frame.len < min_size) min_size = frame.len;
      if (frame.len > max_size) max_size = frame.len;
      ring.release();
    }
  }

  const double secs = (now_ns() - start) / 1e9;
  const uint64_t drops = ring.frames_dropped();
  const double pps = packets / secs;
  const double mbps = (bytes * 8.0) / 1e6 / secs;
  const double drop_pct =
      (packets + drops) ? 100.0 * drops / (packets + drops) : 0.0;
  const double avg_size = packets ? static_cast<double>(bytes) / packets : 0.0;

  std::printf("packets received : %llu\n", (unsigned long long)packets);
  std::printf("packets/sec      : %.0f\n", pps);
  std::printf("bytes received   : %llu\n", (unsigned long long)bytes);
  std::printf("megabits/sec     : %.2f\n", mbps);
  std::printf("drops            : %llu (%.4f%%)\n", (unsigned long long)drops,
              drop_pct);
  std::printf("packet size min/max/avg : %u / %u / %.1f\n",
              packets ? min_size : 0, max_size, avg_size);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
  return 0;
}
