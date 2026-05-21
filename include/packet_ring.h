// packet_ring.h — Layer 1: zero-copy userspace packet RX via PACKET_MMAP.
//
// PacketRing wraps a Linux AF_PACKET socket configured with a TPACKET_V2
// RX ring. The kernel writes received frames directly into an mmap'd ring
// shared with userspace; we read them by pointer dereference with no
// per-packet syscall and no copy. receive() never blocks — it is meant to be
// busy-polled from a CPU-pinned thread.
//
// Ownership: one PacketRing owns one socket + one mmap region. It is
// non-copyable and non-movable; construct it in place on the thread that will
// poll it.
#ifndef UXNET_PACKET_RING_H
#define UXNET_PACKET_RING_H

#include <cstdint>
#include <string>
#include <vector>

namespace uxnet {

// A zero-copy view of a single received frame. `data` points into the mmap'd
// ring and is valid only until the matching release() call hands the slot back
// to the kernel — copy out anything you need to keep before releasing.
struct RxFrame {
  const uint8_t* data = nullptr;  // first byte of the Ethernet header
  uint32_t len = 0;               // full on-wire length of the frame
  uint32_t captured_len = 0;      // bytes actually captured into the ring
};

class PacketRing {
 public:
  // Opens an AF_PACKET/SOCK_RAW socket on `iface`, sets up a TPACKET_V2 RX
  // ring of `num_blocks` 4 MB blocks, and mmaps it. Throws std::runtime_error
  // (with strerror(errno)) on any syscall failure.
  explicit PacketRing(const std::string& iface, int num_blocks = 64);
  ~PacketRing();

  PacketRing(const PacketRing&) = delete;
  PacketRing& operator=(const PacketRing&) = delete;
  PacketRing(PacketRing&&) = delete;
  PacketRing& operator=(PacketRing&&) = delete;

  // Non-blocking. If the current ring slot is owned by userspace, populates
  // `frame` and returns true; otherwise returns false immediately. Does not
  // advance — call release() once you are done with `frame`.
  bool receive(RxFrame& frame);

  // Hands the current slot back to the kernel and advances to the next slot.
  // Only meaningful after a receive() that returned true.
  void release();

  // Enables kernel-side busy polling on the socket (SO_BUSY_POLL), spinning
  // for up to `micros` microseconds instead of sleeping on an interrupt.
  void set_busy_poll(int micros);

  // Raw socket fd, e.g. for an external poll()/epoll() fallback.
  int fd() const { return fd_; }

  // Number of frames observed with the kernel's TP_STATUS_LOSING flag set,
  // i.e. points at which the ring overflowed and packets were dropped.
  uint64_t frames_dropped() const { return drops_; }

 private:
  void cleanup() noexcept;  // idempotent: munmap + close, used on error too

  int fd_ = -1;
  uint8_t* ring_ = nullptr;             // mmap base
  size_t ring_bytes_ = 0;               // total mmap length
  std::vector<uint8_t*> frames_;        // pointer to each frame slot
  size_t total_frames_ = 0;
  size_t current_frame_ = 0;
  uint64_t drops_ = 0;
};

}  // namespace uxnet

#endif  // UXNET_PACKET_RING_H
