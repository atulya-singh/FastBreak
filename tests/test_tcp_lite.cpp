#include "tcp_lite.h"

#include <arpa/inet.h>
#include <gtest/gtest.h>

#include <cstring>
#include <random>
#include <vector>

#include "buffer_pool.h"
#include "rdtsc.h"

using namespace uxnet;

namespace {

constexpr uint64_t kFnvOffset = 1469598103934665603ull;
uint64_t fnv(uint64_t h, const uint8_t* d, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    h ^= d[i];
    h *= 1099511628211ull;
  }
  return h;
}

sockaddr_in addr(uint16_t port) {
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  return a;
}

std::vector<uint8_t> random_bytes(size_t n, uint32_t seed) {
  std::vector<uint8_t> v(n);
  std::mt19937 rng(seed);
  for (size_t i = 0; i < n; ++i) v[i] = static_cast<uint8_t>(rng());
  return v;
}

// Copies a wire packet into a fresh buffer from `pool` and feeds it to `peer`.
void deliver(TcpLiteSession& peer, BufferPool& pool, const uint8_t* p,
             size_t n) {
  BufferDescriptor* b = pool.acquire();
  ASSERT_NE(b, nullptr) << "delivery pool exhausted";
  std::memcpy(b->data, p, n);
  b->len = static_cast<uint32_t>(n);
  peer.on_packet_received(b);
}

}  // namespace

TEST(TcpLite, HeaderChecksumRoundTrip) {
  uint8_t payload[64];
  for (int i = 0; i < 64; ++i) payload[i] = static_cast<uint8_t>(i * 7);
  TcpLiteHeader h = TcpLiteHeader::make_data(42, 7, 64);
  h.compute_checksum(payload, 64);
  EXPECT_TRUE(h.verify_checksum(payload, 64));
  payload[10] ^= 0xFF;  // corrupt one byte
  EXPECT_FALSE(h.verify_checksum(payload, 64));
}

TEST(TcpLite, LoopbackReliability100MB) {
  BufferPool poolA(2048, 1024), poolB(2048, 1024);
  TcpLiteSession A(poolA, addr(40010), addr(40011));
  TcpLiteSession B(poolB, addr(40011), addr(40010));

  A.set_transmit([&](const uint8_t* p, size_t n) { deliver(B, poolB, p, n); });
  B.set_transmit([&](const uint8_t* p, size_t n) { deliver(A, poolA, p, n); });

  uint64_t rhash = kFnvOffset;
  B.set_data_callback(
      [&](const uint8_t* d, size_t l) { rhash = fnv(rhash, d, l); });

  const size_t kTotal = 100u * 1024 * 1024;
  std::vector<uint8_t> data = random_bytes(kTotal, 1);
  const uint64_t shash = fnv(kFnvOffset, data.data(), kTotal);

  A.send(data.data(), kTotal);

  uint64_t guard = 0;
  while (B.bytes_received() < kTotal) {
    A.tick();
    B.tick();
    ASSERT_LT(++guard, 10'000'000u) << "no progress";
  }
  EXPECT_EQ(B.bytes_received(), kTotal);
  EXPECT_EQ(rhash, shash);
  EXPECT_EQ(A.bytes_sent(), kTotal);
}

TEST(TcpLite, OnePercentPacketLoss100MB) {
  BufferPool poolA(2048, 1024), poolB(2048, 1024);
  TcpLiteSession A(poolA, addr(40020), addr(40021));
  TcpLiteSession B(poolB, addr(40021), addr(40020));
  A.set_rto_ns(200'000);  // 200us — retransmit briskly

  std::mt19937 drop_rng(999);
  A.set_transmit([&](const uint8_t* p, size_t n) {
    if (drop_rng() % 100 == 0) return;  // drop ~1%
    deliver(B, poolB, p, n);
  });
  B.set_transmit([&](const uint8_t* p, size_t n) { deliver(A, poolA, p, n); });

  uint64_t rhash = kFnvOffset;
  B.set_data_callback(
      [&](const uint8_t* d, size_t l) { rhash = fnv(rhash, d, l); });

  const size_t kTotal = 100u * 1024 * 1024;
  std::vector<uint8_t> data = random_bytes(kTotal, 2);
  const uint64_t shash = fnv(kFnvOffset, data.data(), kTotal);

  A.send(data.data(), kTotal);

  uint64_t guard = 0;
  while (B.bytes_received() < kTotal) {
    A.tick();
    B.tick();
    ASSERT_LT(++guard, 200'000'000u) << "stalled under loss";
  }
  EXPECT_EQ(B.bytes_received(), kTotal);
  EXPECT_EQ(rhash, shash);
  EXPECT_GT(A.retransmits(), 0u);
}

TEST(TcpLite, OutOfOrderDeliveredInOrder) {
  BufferPool poolA(2048, 512), poolB(2048, 512);
  TcpLiteSession A(poolA, addr(40030), addr(40031));
  TcpLiteSession B(poolB, addr(40031), addr(40030));

  std::vector<std::vector<uint8_t>> captured;
  A.set_transmit([&](const uint8_t* p, size_t n) {
    captured.emplace_back(p, p + n);  // hold, deliver later out of order
  });
  B.set_transmit([&](const uint8_t* p, size_t n) { deliver(A, poolA, p, n); });

  uint64_t rhash = kFnvOffset;
  B.set_data_callback(
      [&](const uint8_t* d, size_t l) { rhash = fnv(rhash, d, l); });

  const size_t kPackets = 200;  // < 256-packet window, so no eviction
  const size_t kTotal = kPackets * TCP_LITE_MAX_PAYLOAD;
  std::vector<uint8_t> data = random_bytes(kTotal, 3);
  const uint64_t shash = fnv(kFnvOffset, data.data(), kTotal);

  A.send(data.data(), kTotal);
  ASSERT_EQ(captured.size(), kPackets);

  for (size_t i = captured.size(); i-- > 0;)  // reverse order
    deliver(B, poolB, captured[i].data(), captured[i].size());
  B.tick();

  EXPECT_EQ(B.bytes_received(), kTotal);
  EXPECT_EQ(rhash, shash);
}

TEST(TcpLite, RetransmitAfterOutage) {
  (void)tsc_frequency_ghz();  // warm calibration before timing the outage

  BufferPool poolA(2048, 512), poolB(2048, 512);
  TcpLiteSession A(poolA, addr(40040), addr(40041));
  TcpLiteSession B(poolB, addr(40041), addr(40040));
  A.set_rto_ns(1'000'000);  // 1ms

  const uint64_t start = mono_ns();
  A.set_transmit([&](const uint8_t* p, size_t n) {
    if (mono_ns() - start < 5'000'000) return;  // black-hole first 5ms
    deliver(B, poolB, p, n);
  });
  B.set_transmit([&](const uint8_t* p, size_t n) { deliver(A, poolA, p, n); });

  uint64_t rhash = kFnvOffset;
  B.set_data_callback(
      [&](const uint8_t* d, size_t l) { rhash = fnv(rhash, d, l); });

  const size_t kTotal = 50u * 1024;
  std::vector<uint8_t> data = random_bytes(kTotal, 4);
  const uint64_t shash = fnv(kFnvOffset, data.data(), kTotal);

  A.send(data.data(), kTotal);

  while (B.bytes_received() < kTotal) {
    A.tick();
    B.tick();
    ASSERT_LT(mono_ns() - start, 2'000'000'000ull) << "never recovered";
  }
  EXPECT_EQ(B.bytes_received(), kTotal);
  EXPECT_EQ(rhash, shash);
  EXPECT_GT(A.retransmits(), 0u);
}
