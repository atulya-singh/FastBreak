// huge_alloc.h — Layer 2: 2 MB huge-page backing store for the buffer pool.
#ifndef UXNET_HUGE_ALLOC_H
#define UXNET_HUGE_ALLOC_H

#include <cstddef>
#include <mutex>
#include <unordered_map>

namespace uxnet {

class HugePageAllocator {
 public:
  static constexpr size_t HUGE_PAGE_SIZE = 2UL * 1024 * 1024;
  static constexpr size_t REGULAR_PAGE_SIZE = 4096UL;

  // Rounds up to a whole number of huge pages, tries MAP_HUGETLB, and returns a
  // HUGE_PAGE_SIZE-aligned pointer. Falls back to regular pages with a stderr
  // warning unless force_huge, in which case failure throws std::runtime_error.
  static void* allocate(size_t bytes, bool force_huge = false);

  static void deallocate(void* ptr);
  static bool is_huge(void* ptr);
  static size_t available_huge_pages();  // /proc/meminfo HugePages_Free
  static size_t total_huge_pages();      // /proc/meminfo HugePages_Total
  static void print_stats();

 private:
  struct AllocationInfo {
    size_t bytes;
    bool is_huge;
  };

  static std::unordered_map<void*, AllocationInfo>& registry();
  static std::mutex& registry_mutex();
};

}  // namespace uxnet

#endif  // UXNET_HUGE_ALLOC_H
