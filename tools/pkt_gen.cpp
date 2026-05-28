#include <arpa/inet.h>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

volatile sig_atomic_t g_stop = 0;
void on_sigint(int) { g_stop = 1; }

inline uint64_t rdtsc() {
  unsigned lo, hi;
  __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
  return (static_cast<uint64_t>(hi) << 32) | lo;
}

inline void cpu_pause() { __asm__ __volatile__("pause"); }

uint64_t now_ns() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + ts.tv_nsec;
}

// Measure TSC frequency against CLOCK_MONOTONIC.
uint64_t calibrate_tsc() {
  const uint64_t t0 = now_ns();
  const uint64_t c0 = rdtsc();
  while (now_ns() - t0 < 100000000ull) cpu_pause();
  const uint64_t c1 = rdtsc();
  const uint64_t t1 = now_ns();
  return (c1 - c0) * 1000000000ull / (t1 - t0);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 6) {
    std::fprintf(stderr,
                 "usage: %s <iface> <dst_ip> <dst_port> <pkt_size> <pps>\n",
                 argv[0]);
    return 1;
  }
  const char* iface = argv[1];
  const char* dst_ip = argv[2];
  const uint16_t dst_port = static_cast<uint16_t>(std::atoi(argv[3]));
  const size_t pkt_size = static_cast<size_t>(std::atoi(argv[4]));
  const uint64_t pps = static_cast<uint64_t>(std::atoll(argv[5]));

  std::signal(SIGINT, on_sigint);

  int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    std::perror("socket");
    return 1;
  }
  if (::setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, iface, std::strlen(iface)) <
      0) {
    std::perror("SO_BINDTODEVICE (need root?)");
  }

  struct sockaddr_in dst;
  std::memset(&dst, 0, sizeof(dst));
  dst.sin_family = AF_INET;
  dst.sin_port = htons(dst_port);
  if (inet_pton(AF_INET, dst_ip, &dst.sin_addr) != 1) {
    std::fprintf(stderr, "bad dst_ip: %s\n", dst_ip);
    return 1;
  }

  char* buf = static_cast<char*>(std::calloc(1, pkt_size ? pkt_size : 1));

  const uint64_t tsc_hz = calibrate_tsc();
  const uint64_t interval = pps ? tsc_hz / pps : 0;

  uint64_t sent = 0, last_sent = 0;
  uint64_t next = rdtsc();
  uint64_t sec_mark = next;

  while (!g_stop) {
    next += interval;
    while (rdtsc() < next) cpu_pause();

    if (::sendto(fd, buf, pkt_size, 0, reinterpret_cast<sockaddr*>(&dst),
                 sizeof(dst)) >= 0) {
      ++sent;
    }

    if (rdtsc() - sec_mark >= tsc_hz) {
      std::printf("send rate: %llu pps (target %llu)\n",
                  (unsigned long long)(sent - last_sent),
                  (unsigned long long)pps);
      last_sent = sent;
      sec_mark += tsc_hz;
    }
  }

  std::free(buf);
  ::close(fd);
  std::printf("\ntotal sent: %llu\n", (unsigned long long)sent);
  return 0;
}
