#include "numa_util.h"

#include <dirent.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <linux/mempolicy.h>

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

namespace uxnet {
namespace {

int read_int_file(const std::string& path, int fallback) {
  std::ifstream f(path);
  int v;
  if (f >> v) return v;
  return fallback;
}

}  // namespace

int numa_node_count() {
  DIR* d = ::opendir("/sys/devices/system/node");
  if (d == nullptr) return 1;

  int count = 0;
  for (dirent* e = ::readdir(d); e != nullptr; e = ::readdir(d)) {
    if (std::strncmp(e->d_name, "node", 4) == 0 && std::isdigit(e->d_name[4])) {
      ++count;
    }
  }
  ::closedir(d);
  return count > 0 ? count : 1;
}

int cpu_to_node(int cpu) {
  char path[128];
  std::snprintf(path, sizeof(path),
                "/sys/devices/system/cpu/cpu%d/topology/physical_package_id",
                cpu);
  return read_int_file(path, 0);
}

int current_numa_node() {
  unsigned cpu = 0;
  if (::syscall(SYS_getcpu, &cpu, nullptr, nullptr) != 0) return 0;
  return cpu_to_node(static_cast<int>(cpu));
}

void* allocate_on_node(size_t bytes, int node) {
  const int base = MAP_ANONYMOUS | MAP_PRIVATE | MAP_POPULATE;

  void* p = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, base | MAP_HUGETLB,
                   -1, 0);
  if (p == MAP_FAILED) {
    p = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, base, -1, 0);
  }
  if (p == MAP_FAILED) return nullptr;

  if (node != -1) {
    unsigned long nodemask = 1UL << node;
    ::syscall(SYS_mbind, p, bytes, MPOL_BIND, &nodemask,
              sizeof(nodemask) * 8, MPOL_MF_MOVE);
  }
  return p;
}

bool verify_allocation_node(void* ptr, size_t bytes, int expected_node) {
  (void)bytes;
  const uintptr_t target = reinterpret_cast<uintptr_t>(ptr);

  std::ifstream maps("/proc/self/numa_maps");
  std::string line, best_line;
  uintptr_t best_start = 0;
  bool found = false;

  while (std::getline(maps, line)) {
    const uintptr_t start = std::strtoull(line.c_str(), nullptr, 16);
    if (start <= target && (!found || start > best_start)) {
      best_start = start;
      best_line = line;
      found = true;
    }
  }
  if (!found) return false;

  long best_pages = -1;
  int best_node = -1;
  size_t pos = 0;
  while ((pos = best_line.find('N', pos)) != std::string::npos) {
    const char* s = best_line.c_str() + pos + 1;
    if (std::isdigit(*s)) {
      char* eq = nullptr;
      const long node = std::strtol(s, &eq, 10);
      if (eq != nullptr && *eq == '=') {
        const long pages = std::strtol(eq + 1, nullptr, 10);
        if (pages > best_pages) {
          best_pages = pages;
          best_node = static_cast<int>(node);
        }
      }
    }
    ++pos;
  }

  return best_node == expected_node;
}

}  // namespace uxnet
