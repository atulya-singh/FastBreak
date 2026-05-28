#include "packet_parser.h"
#include "packet_ring.h"

#include <arpa/inet.h>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <stdexcept>

namespace {

volatile sig_atomic_t g_stop = 0;
void on_sigint(int) { g_stop = 1; }

uint64_t now_ns(clockid_t clk) {
  struct timespec ts;
  clock_gettime(clk, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + ts.tv_nsec;
}

const char* ip_str(uint32_t host_ip, char* out) {
  struct in_addr a;
  a.s_addr = htonl(host_ip);
  return inet_ntop(AF_INET, &a, out, INET_ADDRSTRLEN);
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
  uxnet::PacketRing ring(argv[1], 128);
  ring.set_busy_poll(50);

  const uint64_t start = now_ns(CLOCK_MONOTONIC);
  uint64_t last_tick = start;
  uint64_t total = 0, last_total = 0;

  uxnet::RxFrame frame;
  char s_ip[INET_ADDRSTRLEN], d_ip[INET_ADDRSTRLEN];

  while (!g_stop) {
    const uint64_t now = now_ns(CLOCK_MONOTONIC);
    if (now - start >= static_cast<uint64_t>(duration) * 1000000000ull) break;

    if (ring.receive(frame)) {
      ++total;
      auto p = uxnet::PacketParser::parse(frame.data, frame.captured_len);
      if (p.valid && p.has_udp) {
        ip_str(p.ip.src_ip(), s_ip);
        ip_str(p.ip.dst_ip(), d_ip);
        std::printf("%llu %s:%u -> %s:%u len=%zu\n",
                    static_cast<unsigned long long>(now_ns(CLOCK_REALTIME)),
                    s_ip, p.udp.src_port(), d_ip, p.udp.dst_port(),
                    p.udp.payload_len());
      }
      ring.release();
    }

    if (now - last_tick >= 1000000000ull) {
      std::printf("packets/sec: %llu | total: %llu | drops: %llu\n",
                  static_cast<unsigned long long>(total - last_total),
                  static_cast<unsigned long long>(total),
                  static_cast<unsigned long long>(ring.frames_dropped()));
      last_tick = now;
      last_total = total;
    }
  }

  std::printf("\n--- summary ---\ntotal packets: %llu\ndrops: %llu\n",
              static_cast<unsigned long long>(total),
              static_cast<unsigned long long>(ring.frames_dropped()));
  } catch (const std::exception& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
  return 0;
}
