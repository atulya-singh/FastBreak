#include "numa_util.h"

#include <sys/syscall.h>
#include <unistd.h>

#include <cstdio>
#include <dirent.h>
#include <fstream>
#include <string>

namespace {

std::string read_line(const std::string& path) {
  std::ifstream f(path);
  std::string s;
  std::getline(f, s);
  return s;
}

void print_node_hugepages(int node) {
  char dir[128];
  std::snprintf(dir, sizeof(dir),
                "/sys/devices/system/node/node%d/hugepages", node);
  DIR* d = ::opendir(dir);
  if (d == nullptr) return;

  for (dirent* e = ::readdir(d); e != nullptr; e = ::readdir(d)) {
    if (e->d_name[0] == '.') continue;
    const std::string base = std::string(dir) + "/" + e->d_name;
    const std::string total = read_line(base + "/nr_hugepages");
    const std::string free = read_line(base + "/free_hugepages");
    std::printf("    %s: total=%s free=%s\n", e->d_name, total.c_str(),
                free.c_str());
  }
  ::closedir(d);
}

}  // namespace

int main() {
  const int nodes = uxnet::numa_node_count();
  std::printf("NUMA nodes           : %d\n", nodes);

  unsigned cpu = 0;
  ::syscall(SYS_getcpu, &cpu, nullptr, nullptr);
  std::printf("current CPU / node   : %u / %d\n", cpu,
              uxnet::current_numa_node());
  std::printf("\n");

  for (int n = 0; n < nodes; ++n) {
    char path[128];
    std::snprintf(path, sizeof(path),
                  "/sys/devices/system/node/node%d/cpulist", n);
    const std::string cpulist = read_line(path);
    std::printf("node %d\n", n);
    std::printf("  cpus               : %s\n",
                cpulist.empty() ? "(none)" : cpulist.c_str());
    std::printf("  hugepages\n");
    print_node_hugepages(n);
  }

  if (nodes > 1) {
    std::printf("\nWARNING: %d NUMA nodes present — cross-node memory access "
                "costs bandwidth and latency. Pin workers and pools per node.\n",
                nodes);
  }
  return 0;
}
