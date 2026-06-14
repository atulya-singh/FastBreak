#include "rss_distributor.h"

#include <cstdint>
#include <random>
#include <vector>

#include <gtest/gtest.h>

using uxnet::RSSDistributor;

// Canonical Microsoft/Intel 82599 RSS 4-tuple (IPv4+TCP) test vectors.
TEST(RSSDistributor, ToeplitzKnownVectors) {
  struct V {
    uint32_t src_ip, dst_ip;
    uint16_t src_port, dst_port;
    uint32_t expected;
  };
  const V vectors[] = {
      {0x420995BB, 0xA18E6450, 2794, 1766, 0x51ccc178},
      {0xC75C6F02, 0x41458C53, 14230, 4739, 0xc626b0ea},
      {0x1813C65F, 0x0C16CFB8, 12898, 38024, 0x5c2b394a},
      {0x261BCD1E, 0xD18EA306, 48228, 2217, 0xafc7327f},
  };
  for (const V& v : vectors) {
    EXPECT_EQ(RSSDistributor::toeplitz(v.src_ip, v.dst_ip, v.src_port,
                                       v.dst_port),
              v.expected);
  }
}

TEST(RSSDistributor, SameTupleSameQueue) {
  RSSDistributor rss(16);
  const uint32_t a = rss.hash(0x0A000001, 0x0A000002, 1234, 5678);
  const uint32_t b = rss.hash(0x0A000001, 0x0A000002, 1234, 5678);
  EXPECT_EQ(a, b);
  EXPECT_LT(a, rss.queue_count());
}

TEST(RSSDistributor, DistributionWithin15Percent) {
  constexpr size_t kQueues = 8;
  constexpr int kPackets = 1'000'000;
  RSSDistributor rss(kQueues);

  std::mt19937 rng(12345);
  std::uniform_int_distribution<uint32_t> ip;
  std::uniform_int_distribution<uint32_t> port(0, 65535);

  std::vector<int> counts(kQueues, 0);
  for (int i = 0; i < kPackets; ++i) {
    const uint32_t idx = rss.hash(ip(rng), ip(rng),
                                  static_cast<uint16_t>(port(rng)),
                                  static_cast<uint16_t>(port(rng)));
    ASSERT_LT(idx, kQueues);
    ++counts[idx];
  }

  const double expected = static_cast<double>(kPackets) / kQueues;
  for (size_t q = 0; q < kQueues; ++q) {
    const double ratio = counts[q] / expected;
    EXPECT_GT(ratio, 0.85) << "queue " << q << " got " << counts[q];
    EXPECT_LT(ratio, 1.15) << "queue " << q << " got " << counts[q];
  }
}
