// buffer_pool.cpp — Layer 2 implementation. See include/buffer_pool.h.
#include "buffer_pool.h"

#include <sys/mman.h>  // munmap

#include <cassert>
#include <stdexcept>

#include "huge_alloc.h"
#include "numa_util.h"

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
    // Safe under a stale `old`: a competing pop bumps the generation, failing
    // our CAS before we act on this next link.
    const uint32_t next = next_of(descriptors_[index]);
    neu = pack(gen_of(old) + 1, next);
  } while (!top_.compare_exchange_weak(old, neu, std::memory_order_acquire,
                                       std::memory_order_acquire));
  size_.fetch_sub(1, std::memory_order_relaxed);
  return index;
}

namespace {

void* alloc_region(size_t bytes, int node) {
  if (node != -1) {
    void* p = allocate_on_node(bytes, node);
    if (p == nullptr) throw std::runtime_error("BufferPool: allocate_on_node failed");
    return p;
  }
  return HugePageAllocator::allocate(bytes);
}

void free_region(void* ptr, size_t bytes, int node) {
  if (ptr == nullptr) return;
  if (node != -1) {
    ::munmap(ptr, bytes);
  } else {
    HugePageAllocator::deallocate(ptr);
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

  backing_bytes_ = buffer_size_ * buffer_count_;
  descriptor_bytes_ = buffer_count_ * sizeof(BufferDescriptor);

  backing_ = static_cast<uint8_t*>(alloc_region(backing_bytes_, numa_node_));
  descriptors_ =
      static_cast<BufferDescriptor*>(alloc_region(descriptor_bytes_, numa_node_));

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
  free_region(backing_, backing_bytes_, numa_node_);
  free_region(descriptors_, descriptor_bytes_, numa_node_);
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
  buf->len = 0;
  free_stack_.push(buf->pool_index);
  total_releases_.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace uxnet
