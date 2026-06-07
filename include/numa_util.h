#ifndef UXNET_NUMA_UTIL_H
#define UXNET_NUMA_UTIL_H

#include <cstddef>

namespace uxnet {

int numa_node_count();
int current_numa_node();
int cpu_to_node(int cpu);

void* allocate_on_node(size_t bytes, int node);
bool verify_allocation_node(void* ptr, size_t bytes, int expected_node);

}  // namespace uxnet

#endif  // UXNET_NUMA_UTIL_H
