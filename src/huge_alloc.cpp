// huge_alloc.cpp — Layer 2 implementation. See include/huge_alloc.h.
#include "huge_alloc.h"

#include <sys/mman.h>  // mmap, munmap, MAP_HUGETLB, MAP_POPULATE

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>

namespace uxnet {
namespace {

size_t round_up(size_t bytes, size_t multiple) {
  if (bytes == 0) return multiple;
  return ((bytes + multiple - 1) / multiple) * multiple;
}

// Reads a "Key:  N kB" line from /proc/meminfo and returns N, or 0 if absent.
size_t read_meminfo_field(const char* key) {
  std::ifstream meminfo("/proc/meminfo");
  std::string name;
  size_t value = 0;
  std::string unit;
  while (meminfo >> name >> value >> unit) {
    if (name == key) return value;
    // Lines without a trailing unit (rare) still consumed a token above; the
    // stream self-corrects on the next iteration.
  }
  return 0;
}

}  // namespace

std::unordered_map<void*, HugePageAllocator::AllocationInfo>&
HugePageAllocator::registry() {
  static std::unordered_map<void*, AllocationInfo> map;
  return map;
}

std::mutex& HugePageAllocator::registry_mutex() {
  static std::mutex m;
  return m;
}

void* HugePageAllocator::allocate(size_t bytes, bool force_huge) {
  const size_t len = round_up(bytes, HUGE_PAGE_SIZE);

  // MAP_POPULATE prefaults the whole region so the fault cost is paid here, not
  // on the hot path. MAP_HUGETLB requests 2 MB pages.
  void* p = ::mmap(nullptr, len, PROT_READ | PROT_WRITE,
                   MAP_HUGETLB | MAP_ANONYMOUS | MAP_PRIVATE | MAP_POPULATE,
                   -1, 0);
  bool is_huge = true;

  if (p == MAP_FAILED) {
    if (force_huge) {
      throw std::runtime_error(std::string("HugePageAllocator: huge-page mmap "
                                           "failed: ") +
                               std::strerror(errno));
    }
    std::fprintf(stderr,
                 "HugePageAllocator: huge-page mmap of %zu bytes failed (%s); "
                 "falling back to regular pages\n",
                 len, std::strerror(errno));
    p = ::mmap(nullptr, len, PROT_READ | PROT_WRITE,
               MAP_ANONYMOUS | MAP_PRIVATE | MAP_POPULATE, -1, 0);
    if (p == MAP_FAILED) {
      throw std::runtime_error(std::string("HugePageAllocator: fallback mmap "
                                           "failed: ") +
                               std::strerror(errno));
    }
    is_huge = false;
  }

  {
    std::lock_guard<std::mutex> lock(registry_mutex());
    registry()[p] = AllocationInfo{len, is_huge};
  }
  return p;
}

void HugePageAllocator::deallocate(void* ptr) {
  if (ptr == nullptr) return;

  size_t len = 0;
  {
    std::lock_guard<std::mutex> lock(registry_mutex());
    auto it = registry().find(ptr);
    if (it == registry().end()) {
      throw std::runtime_error("HugePageAllocator: deallocate of untracked "
                               "pointer");
    }
    len = it->second.bytes;
    registry().erase(it);
  }

  if (::munmap(ptr, len) < 0) {
    throw std::runtime_error(std::string("HugePageAllocator: munmap failed: ") +
                             std::strerror(errno));
  }
}

bool HugePageAllocator::is_huge(void* ptr) {
  std::lock_guard<std::mutex> lock(registry_mutex());
  auto it = registry().find(ptr);
  return it != registry().end() && it->second.is_huge;
}

size_t HugePageAllocator::available_huge_pages() {
  return read_meminfo_field("HugePages_Free:");
}

size_t HugePageAllocator::total_huge_pages() {
  return read_meminfo_field("HugePages_Total:");
}

void HugePageAllocator::print_stats() {
  std::lock_guard<std::mutex> lock(registry_mutex());
  size_t huge_count = 0, huge_bytes = 0, reg_count = 0, reg_bytes = 0;
  for (const auto& [ptr, info] : registry()) {
    if (info.is_huge) {
      ++huge_count;
      huge_bytes += info.bytes;
    } else {
      ++reg_count;
      reg_bytes += info.bytes;
    }
  }
  std::fprintf(stderr,
               "HugePageAllocator: %zu live allocations — "
               "huge: %zu (%zu bytes), regular: %zu (%zu bytes)\n",
               huge_count + reg_count, huge_count, huge_bytes, reg_count,
               reg_bytes);
}

}  // namespace uxnet
