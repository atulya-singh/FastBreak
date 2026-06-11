#include "spsc_ring.h"

#include <cstdint>
#include <thread>

#include <gtest/gtest.h>

using uxnet::SPSCRing;

TEST(SPSCRing, SingleThreadedFillDrain) {
  SPSCRing<uint64_t, 8> ring;
  for (uint64_t i = 0; i < 8; ++i) EXPECT_TRUE(ring.push(i));
  EXPECT_FALSE(ring.push(999));  // full

  uint64_t v = 0;
  for (uint64_t i = 0; i < 8; ++i) {
    ASSERT_TRUE(ring.pop(v));
    EXPECT_EQ(v, i);
  }
  EXPECT_FALSE(ring.pop(v));  // empty
}

TEST(SPSCRing, FullReturnsFalseEmptyReturnsFalse) {
  SPSCRing<int, 4> ring;
  EXPECT_TRUE(ring.empty());
  for (int i = 0; i < 4; ++i) EXPECT_TRUE(ring.push(i));
  EXPECT_FALSE(ring.push(4));
  EXPECT_EQ(ring.size(), 4u);

  int v = 0;
  for (int i = 0; i < 4; ++i) EXPECT_TRUE(ring.pop(v));
  EXPECT_FALSE(ring.pop(v));
  EXPECT_TRUE(ring.empty());
}

TEST(SPSCRing, TwoThreadStressOrderAndNoLoss) {
  constexpr uint64_t kItems = 10'000'000ull;
  SPSCRing<uint64_t, 1024> ring;

  std::thread producer([&] {
    for (uint64_t i = 0; i < kItems; ++i) {
      while (!ring.push(i)) { /* spin on full */ }
    }
  });

  uint64_t expected = 0;
  uint64_t v = 0;
  while (expected < kItems) {
    if (ring.pop(v)) {
      ASSERT_EQ(v, expected);  // order preserved, nothing skipped
      ++expected;
    }
  }
  producer.join();
  EXPECT_EQ(expected, kItems);
}

TEST(SPSCRing, HeadAndTailOnDifferentCacheLines) {
  using Ring = SPSCRing<uint64_t, 16>;
  const size_t h = Ring::head_offset();
  const size_t t = Ring::tail_offset();
  EXPECT_EQ(h % 64, 0u);
  EXPECT_EQ(t % 64, 0u);
  EXPECT_NE(h / 64, t / 64);
  EXPECT_EQ(sizeof(Ring) % 64, 0u);
}
