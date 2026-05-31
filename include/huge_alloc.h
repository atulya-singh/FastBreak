// huge_alloc.h — Layer 2: 2 MB huge-page backing store for the buffer pool.
//
// HugePageAllocator hands out mmap'd regions backed by 2 MB huge pages when the
// kernel can supply them, falling back to regular 4 KB pages otherwise. Every
// mapping is MAP_POPULATE'd so the page faults are paid at allocate() time, not
// on the packet hot path. Returned pointers are aligned to HUGE_PAGE_SIZE and
// their sizes rounded up to a whole number of huge pages.
//
// All methods are static; there is no instance state. A single process-wide map
// records each live allocation so deallocate() knows the munmap length. That map
// is guarded by a mutex and is only ever touched by allocate()/deallocate() —
// never from a hot path.
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

  // Rounds `bytes` up to a whole number of huge pages and maps that region.
  // Tries MAP_HUGETLB first. If huge pages are unavailable and force_huge is
  // false, falls back to a regular-page mapping and warns on stderr. If
  // force_huge is true and huge pages fail, throws std::runtime_error. The
  // returned pointer is aligned to HUGE_PAGE_SIZE.
  static void* allocate(size_t bytes, bool force_huge = false);

  // Unmaps a region previously returned by allocate(). No-op on nullptr;
  // throws std::runtime_error on an unknown pointer.
  static void deallocate(void* ptr);

  // True if `ptr` came from allocate() and is backed by huge pages.
  static bool is_huge(void* ptr);

  // /proc/meminfo HugePages_Free — 2 MB pages the kernel can still hand out.
  static size_t available_huge_pages();

  // /proc/meminfo HugePages_Total — size of the configured huge-page pool.
  static size_t total_huge_pages();

  // Dumps live allocations (count, bytes, huge vs regular split) to stderr.
  static void print_stats();

 private:
  struct AllocationInfo {
    size_t bytes;   // rounded-up mapping length passed to munmap
    bool is_huge;   // whether the mapping is huge-page backed
  };

  // Process-wide registry of live allocations. Guarded by registry_mutex();
  // touched only by allocate()/deallocate()/is_huge()/print_stats().
  static std::unordered_map<void*, AllocationInfo>& registry();
  static std::mutex& registry_mutex();
};

}  // namespace uxnet

#endif  // UXNET_HUGE_ALLOC_H
