// buffer_pool.cpp — Layer 2 implementation. See include/buffer_pool.h.
#include "buffer_pool.h"

#include <sys/syscall.h>  // SYS_mbind
#include <unistd.h>       // syscall

#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

#include <linux/mempolicy.h>  // MPOL_BIND, MPOL_MF_MOVE

#include "huge_alloc.h"

namespace uxnet {

void FreeStack::push(uint32_t index) {
  uint64_t old = top_.load(std::memory_order_relaxed);
  uint64_t neu;
  do {
    next_of(descriptors_[index]) = index_of(old);
    neu = pack(gen_of(old) + 1, index);
  } while (!top_.compare_exchange_weak(old, neu, std::memory_order_release,
                                       std::memory_order_relaxed));
  size_.fetch_add(1, std::memory_order_relaxed);
}

uint32_t FreeStack::pop() {
  uint64_t old = top_.load(std::memory_order_acquire);
  uint64_t neu;
  uint32_t index;
  do {
    index = index_of(old);
    if (index == kEmpty) return kEmpty;
    // Safe to read next_ under a stale `old`: a successful competing pop would
    // have bumped the generation, so our CAS would fail before we act on it.
    const uint32_t next = next_of(descriptors_[index]);
    neu = pack(gen_of(old) + 1, next);
  } while (!top_.compare_exchange_weak(old, neu, std::memory_order_acquire,
                                       std::memory_order_acquire));
  size_.fetch_sub(1, std::memory_order_relaxed);
  return index;
}

namespace {

// Best-effort NUMA bind via the raw mbind syscall (no libnuma). MAP_POPULATE
// has already faulted the pages, so MPOL_MF_MOVE asks the kernel to migrate
// them. Non-fatal: a failure only costs cross-node access, not correctness.
void maybe_bind_numa(void* addr, size_t len, int node) {
  if (node < 0) return;
  unsigned long nodemask = 1UL << node;
  long rc = ::syscall(SYS_mbind, addr, len, MPOL_BIND, &nodemask,
                      sizeof(nodemask) * 8, MPOL_MF_MOVE);
  if (rc != 0) {
    std::fprintf(stderr, "BufferPool: mbind to NUMA node %d failed (%s)\n", node,
                 std::strerror(errno));
  }
}

}  // namespace

BufferPool::BufferPool(size_t buffer_size, size_t buffer_count, int numa_node)
    : buffer_size_(buffer_size),
      buffer_count_(buffer_count),
      numa_node_(numa_node) {
  if (buffer_size_ == 0 || buffer_count_ == 0) {
    throw std::runtime_error("BufferPool: buffer_size and buffer_count must be "
                             "non-zero");
  }

  const size_t backing_bytes = buffer_size_ * buffer_count_;
  const size_t descriptor_bytes = buffer_count_ * sizeof(BufferDescriptor);

  backing_ = static_cast<uint8_t*>(HugePageAllocator::allocate(backing_bytes));
  descriptors_ =
      static_cast<BufferDescriptor*>(HugePageAllocator::allocate(descriptor_bytes));

  maybe_bind_numa(backing_, backing_bytes, numa_node_);
  maybe_bind_numa(descriptors_, descriptor_bytes, numa_node_);

  free_stack_.attach(descriptors_);
  for (size_t i = 0; i < buffer_count_; ++i) {
    BufferDescriptor& d = descriptors_[i];
    d.data = backing_ + i * buffer_size_;
    d.len = 0;
    d.capacity = static_cast<uint32_t>(buffer_size_);
    d.pool_index = static_cast<uint32_t>(i);
    free_stack_.push(static_cast<uint32_t>(i));
  }
}

BufferPool::~BufferPool() {
  if (backing_ != nullptr) HugePageAllocator::deallocate(backing_);
  if (descriptors_ != nullptr) HugePageAllocator::deallocate(descriptors_);
}

BufferDescriptor* BufferPool::acquire() {
  const uint32_t index = free_stack_.pop();
  if (index == FreeStack::kEmpty) {
    total_exhausted_.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
  }
  total_acquires_.fetch_add(1, std::memory_order_relaxed);
  return &descriptors_[index];
}

void BufferPool::release(BufferDescriptor* buf) {
  assert(buf != nullptr && buf->pool_index < buffer_count_ &&
         "BufferPool::release: buffer not from this pool");
  buf->len = 0;  // reset length only; leaving stale data is fine and cheap
  free_stack_.push(buf->pool_index);
  total_releases_.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace uxnet
