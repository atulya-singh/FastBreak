// test_huge_alloc.cpp — Layer 2 HugePageAllocator tests.
#include "huge_alloc.h"

#include <cstdint>
#include <fstream>
#include <string>

#include <gtest/gtest.h>

using uxnet::HugePageAllocator;

namespace {

// Resident set size of this process in kB, from /proc/self/status.
size_t vm_rss_kb() {
  std::ifstream status("/proc/self/status");
  std::string key;
  size_t value = 0;
  std::string unit;
  while (status >> key >> value >> unit) {
    if (key == "VmRSS:") return value;
  }
  return 0;
}

}  // namespace

// Test 1: a 64 MB region survives a full write/read pattern with no corruption.
TEST(HugePageAllocator, WriteReadPatternNoCorruption) {
  constexpr size_t kBytes = 64UL * 1024 * 1024;
  constexpr uint32_t kPattern = 0xDEADBEEF;
  const size_t count = kBytes / sizeof(uint32_t);

  auto* p = static_cast<uint32_t*>(HugePageAllocator::allocate(kBytes));
  ASSERT_NE(p, nullptr);

  for (size_t i = 0; i < count; ++i) p[i] = kPattern;
  for (size_t i = 0; i < count; ++i) ASSERT_EQ(p[i], kPattern) << "at index " << i;

  HugePageAllocator::deallocate(p);
}

// Test 2: allocate+free 100 times leaves RSS essentially where it started.
TEST(HugePageAllocator, NoLeakOverManyCycles) {
  constexpr size_t kBytes = 2UL * 1024 * 1024;

  // Warm up once so any first-touch/allocator growth is excluded from the delta.
  HugePageAllocator::deallocate(HugePageAllocator::allocate(kBytes));

  const size_t rss_before = vm_rss_kb();
  for (int i = 0; i < 100; ++i) {
    auto* p = static_cast<volatile uint8_t*>(HugePageAllocator::allocate(kBytes));
    p[0] = 1;
    p[kBytes - 1] = 1;
    HugePageAllocator::deallocate(const_cast<uint8_t*>(p));
  }
  const size_t rss_after = vm_rss_kb();

  // Allow a little slack for gtest/stdio bookkeeping; a real leak would be
  // ~100 * 2 MB and blow past this.
  const size_t slack_kb = 4UL * 1024;
  EXPECT_LE(rss_after, rss_before + slack_kb)
      << "rss_before=" << rss_before << " rss_after=" << rss_after;
}

// Test 3: the free-page count is a plausible, bounded number.
TEST(HugePageAllocator, AvailableHugePagesIsSensible) {
  const size_t free_pages = HugePageAllocator::available_huge_pages();
  const size_t total_pages = HugePageAllocator::total_huge_pages();
  EXPECT_LE(free_pages, total_pages);
}

// Test 4: when huge pages are configured, a huge allocation reports is_huge().
TEST(HugePageAllocator, IsHugeWhenHugePagesAvailable) {
  if (HugePageAllocator::available_huge_pages() == 0) {
    GTEST_SKIP() << "no free huge pages configured on this host";
  }
  void* p = HugePageAllocator::allocate(HugePageAllocator::HUGE_PAGE_SIZE, true);
  EXPECT_TRUE(HugePageAllocator::is_huge(p));
  HugePageAllocator::deallocate(p);
}

// Test 5: a 1-byte request rounds up to a full 2 MB huge page — the whole page
// is writable without faulting past the mapping.
TEST(HugePageAllocator, RoundsUpToHugePage) {
  auto* p = static_cast<uint8_t*>(HugePageAllocator::allocate(1));
  ASSERT_NE(p, nullptr);
  for (size_t i = 0; i < HugePageAllocator::HUGE_PAGE_SIZE; ++i) p[i] = 0xAB;
  EXPECT_EQ(p[0], 0xAB);
  EXPECT_EQ(p[HugePageAllocator::HUGE_PAGE_SIZE - 1], 0xAB);
  HugePageAllocator::deallocate(p);
}
