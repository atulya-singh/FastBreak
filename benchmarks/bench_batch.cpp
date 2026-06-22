#include "batch_processor.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <random>
#include <vector>

using namespace uxnet;

namespace {

constexpr size_t kNumBufs = 8192;   // working set >> L1/L2 when payload is large
constexpr size_t kPayload = 1024;   // bytes of UDP payload per packet
constexpr uint16_t kDstPort = 1234;

uint64_t now_ns() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + ts.tv_nsec;
}

// Lay out eth + IPv4 + UDP + payload into p; return total frame length.
uint32_t craft(uint8_t* p, size_t payload, uint16_t dst_port) {
  std::memset(p, 0, 14 + 20 + 8 + payload);
  p[12] = 0x08;  p[13] = 0x00;                 // ethertype IPv4
  uint8_t* ip = p + 14;
  ip[0] = 0x45;                                // v4, ihl=5
  const uint16_t ip_total = 20 + 8 + payload;
  ip[2] = ip_total >> 8;  ip[3] = ip_total & 0xFF;
  ip[9] = 17;                                  // UDP
  ip[12] = 10; ip[13] = 0; ip[14] = 0; ip[15] = 1;   // src ip
  ip[16] = 10; ip[17] = 0; ip[18] = 0; ip[19] = 2;   // dst ip
  uint8_t* udp = ip + 20;
  udp[2] = dst_port >> 8;  udp[3] = dst_port & 0xFF;
  const uint16_t udp_len = 8 + payload;
  udp[4] = udp_len >> 8;  udp[5] = udp_len & 0xFF;
  for (size_t i = 0; i < payload; ++i) udp[8 + i] = static_cast<uint8_t>(i);
  return 14 + 20 + 8 + payload;
}

struct NullHandler : BatchHandler {
  uint64_t calls = 0;
  void handle(const ParsedPacket&, BufferDescriptor*) override { ++calls; }
  void flush() override {}
};

struct ChecksumHandler : BatchHandler {
  uint64_t sum = 0;
  void handle(const ParsedPacket& pkt, BufferDescriptor*) override {
    const uint8_t* pl = pkt.udp.payload();
    const size_t n = pkt.udp.payload_len();
    uint64_t s = 0;
    for (size_t i = 0; i < n; ++i) s += pl[i];
    sum += s;
  }
  void flush() override {}
};

// Local mirror of BatchProcessor's parse→dispatch pipeline, parameterized on
// batch size and prefetch so we can sweep both. Processes buffers in the order
// given by `idx` (shuffled = cold, random access).
template <bool Prefetch>
void pipeline(const std::vector<BufferDescriptor>& bufs,
              const std::vector<uint32_t>& idx, size_t batch,
              BatchHandler* const* udp_table, BatchHandler* def) {
  ParsedPacket parsed[64];
  const size_t total = idx.size();
  for (size_t base = 0; base < total; base += batch) {
    const size_t n = std::min(batch, total - base);
    for (size_t i = 0; i < n; ++i) {
      if (Prefetch && i + 2 < n)
        __builtin_prefetch(bufs[idx[base + i + 2]].data, 0, 0);
      const BufferDescriptor& b = bufs[idx[base + i]];
      parsed[i] = PacketParser::parse(b.data, b.len);
    }
    for (size_t i = 0; i < n; ++i) {
      BatchHandler* h =
          parsed[i].has_udp ? udp_table[parsed[i].udp.dst_port()] : nullptr;
      if (h == nullptr) h = def;
      if (h != nullptr) h->handle(parsed[i], nullptr);
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  const bool quick = argc > 1 && std::strcmp(argv[1], "quick") == 0;

  // Backing store + descriptors for kNumBufs crafted packets.
  const size_t slot = 14 + 20 + 8 + kPayload;
  std::vector<uint8_t> backing(kNumBufs * slot);
  std::vector<BufferDescriptor> bufs(kNumBufs);
  for (size_t i = 0; i < kNumBufs; ++i) {
    uint8_t* p = backing.data() + i * slot;
    const uint32_t len = craft(p, kPayload, kDstPort);
    bufs[i].data = p;
    bufs[i].len = len;
    bufs[i].capacity = slot;
    bufs[i].pool_index = static_cast<uint32_t>(i);
  }

  // Shuffled access order so each parsed buffer is cold in cache.
  std::vector<uint32_t> idx(kNumBufs);
  for (size_t i = 0; i < kNumBufs; ++i) idx[i] = static_cast<uint32_t>(i);
  std::mt19937 rng(12345);
  std::shuffle(idx.begin(), idx.end(), rng);

  auto udp_table = std::make_unique<BatchHandler*[]>(65536);
  std::memset(udp_table.get(), 0, 65536 * sizeof(BatchHandler*));

  const double gib_payload = static_cast<double>(kPayload);

  std::printf("bench_batch: %zu buffers x %zu payload (%.1f MB working set)%s\n",
              kNumBufs, kPayload, kNumBufs * slot / 1e6, quick ? " [quick]" : "");
  std::printf("             each measurement runs shuffled (cold) access\n\n");

  // --- 1. Null handler: dispatch overhead alone ------------------------------
  {
    NullHandler nh;
    udp_table[kDstPort] = &nh;
    const uint64_t target = quick ? 5'000'000ull : 50'000'000ull;
    const uint64_t passes = target / kNumBufs;
    const uint64_t total_pkts = passes * kNumBufs;
    const uint64_t t0 = now_ns();
    for (uint64_t p = 0; p < passes; ++p)
      pipeline<true>(bufs, idx, 32, udp_table.get(), nullptr);
    const uint64_t dt = now_ns() - t0;
    std::printf("[1] null handler (batch=32, prefetch on)\n");
    std::printf("    %.2f ns/packet   %.1f Mpps   (dispatched %llu)\n\n",
                static_cast<double>(dt) / total_pkts,
                total_pkts / (dt / 1000.0), (unsigned long long)nh.calls);
    udp_table[kDstPort] = nullptr;
  }

  // --- 2. Checksum handler: read every payload byte --------------------------
  {
    ChecksumHandler ch;
    udp_table[kDstPort] = &ch;
    const uint64_t target = quick ? 2'000'000ull : 10'000'000ull;
    const uint64_t passes = target / kNumBufs;
    const uint64_t total_pkts = passes * kNumBufs;
    const uint64_t t0 = now_ns();
    for (uint64_t p = 0; p < passes; ++p)
      pipeline<true>(bufs, idx, 32, udp_table.get(), nullptr);
    const uint64_t dt = now_ns() - t0;
    const double gbps = total_pkts * gib_payload * 8.0 / dt;  // bytes*8/ns = Gb/s
    std::printf("[2] checksum handler (batch=32, prefetch on, reads all payload)\n");
    std::printf("    %.2f ns/packet   %.1f Mpps   %.2f Gbit/s   (sum=%llu)\n\n",
                static_cast<double>(dt) / total_pkts,
                total_pkts / (dt / 1000.0), gbps, (unsigned long long)ch.sum);
    udp_table[kDstPort] = nullptr;
  }

  // --- 3. Throughput vs batch size -------------------------------------------
  {
    ChecksumHandler ch;
    udp_table[kDstPort] = &ch;
    const size_t sizes[] = {1, 8, 16, 32, 64};
    const uint64_t target = quick ? 2'000'000ull : 10'000'000ull;
    const uint64_t passes = target / kNumBufs;
    const uint64_t total_pkts = passes * kNumBufs;
    std::printf("[3] throughput vs batch size (checksum handler, prefetch on)\n");
    std::printf("    %-8s %-14s %-10s\n", "batch", "ns/packet", "Mpps");
    for (size_t bs : sizes) {
      const uint64_t t0 = now_ns();
      for (uint64_t p = 0; p < passes; ++p)
        pipeline<true>(bufs, idx, bs, udp_table.get(), nullptr);
      const uint64_t dt = now_ns() - t0;
      std::printf("    %-8zu %-14.2f %-10.1f\n", bs,
                  static_cast<double>(dt) / total_pkts,
                  total_pkts / (dt / 1000.0));
    }
    std::printf("\n");
    udp_table[kDstPort] = nullptr;
  }

  // --- 4. Prefetch impact on cold payloads -----------------------------------
  {
    ChecksumHandler ch;
    udp_table[kDstPort] = &ch;
    const uint64_t target = quick ? 3'000'000ull : 20'000'000ull;
    const uint64_t passes = target / kNumBufs;
    const uint64_t total_pkts = passes * kNumBufs;

    const uint64_t t0 = now_ns();
    for (uint64_t p = 0; p < passes; ++p)
      pipeline<false>(bufs, idx, 32, udp_table.get(), nullptr);
    const double no_pf = static_cast<double>(now_ns() - t0) / total_pkts;

    const uint64_t t1 = now_ns();
    for (uint64_t p = 0; p < passes; ++p)
      pipeline<true>(bufs, idx, 32, udp_table.get(), nullptr);
    const double pf = static_cast<double>(now_ns() - t1) / total_pkts;

    std::printf("[4] prefetch impact (batch=32, cold shuffled payloads)\n");
    std::printf("    without __builtin_prefetch: %.2f ns/packet\n", no_pf);
    std::printf("    with    __builtin_prefetch: %.2f ns/packet\n", pf);
    std::printf("    delta: %.2f ns/packet (%.1f%%)\n\n", no_pf - pf,
                no_pf > 0 ? (no_pf - pf) / no_pf * 100.0 : 0.0);
    udp_table[kDstPort] = nullptr;
  }

  return 0;
}
