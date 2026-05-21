// packet_ring.cpp — Layer 1 implementation. See include/packet_ring.h.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE  // SO_BUSY_POLL
#endif

#include "packet_ring.h"

#include <arpa/inet.h>        // htons
#include <linux/if_ether.h>   // ETH_P_ALL
#include <linux/if_packet.h>  // sockaddr_ll, tpacket_req, tpacket2_hdr, TP_STATUS_*
#include <net/if.h>           // struct ifreq, IFNAMSIZ
#include <sys/ioctl.h>        // ioctl, SIOCGIFINDEX
#include <sys/mman.h>         // mmap, munmap
#include <sys/socket.h>       // socket, setsockopt, bind
#include <unistd.h>           // close

#include <cerrno>
#include <cstring>
#include <stdexcept>

namespace uxnet {
namespace {

// TPACKET_V2 ring geometry.
constexpr uint32_t kBlockSize = 1u << 22;  // 4 MB blocks
constexpr uint32_t kFrameSize = 2048;      // one frame per 2 KB slot
constexpr uint32_t kFramesPerBlock = kBlockSize / kFrameSize;

[[noreturn]] void throw_errno(const char* what) {
  throw std::runtime_error(std::string(what) + ": " + std::strerror(errno));
}

}  // namespace

PacketRing::PacketRing(const std::string& iface, int num_blocks) {
  if (num_blocks <= 0) {
    throw std::runtime_error("PacketRing: num_blocks must be positive");
  }
  if (iface.size() >= IFNAMSIZ) {
    throw std::runtime_error("PacketRing: interface name too long: " + iface);
  }

  try {
    // 1. Raw packet socket capturing every protocol.
    fd_ = ::socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (fd_ < 0) throw_errno("socket(AF_PACKET, SOCK_RAW)");

    // 2. Resolve the interface index for binding.
    struct ifreq ifr;
    std::memset(&ifr, 0, sizeof(ifr));
    std::strncpy(ifr.ifr_name, iface.c_str(), IFNAMSIZ - 1);
    if (::ioctl(fd_, SIOCGIFINDEX, &ifr) < 0) {
      throw_errno("ioctl(SIOCGIFINDEX)");
    }
    const int ifindex = ifr.ifr_ifindex;

    // 3. Select TPACKET_V2 before laying out the ring (layout is version-dependent).
    int version = TPACKET_V2;
    if (::setsockopt(fd_, SOL_PACKET, PACKET_VERSION, &version,
                     sizeof(version)) < 0) {
      throw_errno("setsockopt(PACKET_VERSION, TPACKET_V2)");
    }

    // 4. Describe and install the RX ring.
    total_frames_ = static_cast<size_t>(num_blocks) * kFramesPerBlock;
    struct tpacket_req req;
    std::memset(&req, 0, sizeof(req));
    req.tp_block_size = kBlockSize;
    req.tp_block_nr = static_cast<unsigned int>(num_blocks);
    req.tp_frame_size = kFrameSize;
    req.tp_frame_nr = static_cast<unsigned int>(total_frames_);
    if (::setsockopt(fd_, SOL_PACKET, PACKET_RX_RING, &req, sizeof(req)) < 0) {
      throw_errno("setsockopt(PACKET_RX_RING)");
    }

    // 5. Map the whole ring into our address space (one contiguous region).
    ring_bytes_ = static_cast<size_t>(num_blocks) * kBlockSize;
    void* base = ::mmap(nullptr, ring_bytes_, PROT_READ | PROT_WRITE,
                        MAP_SHARED, fd_, 0);
    if (base == MAP_FAILED) {
      ring_ = nullptr;
      throw_errno("mmap(PACKET_RX_RING)");
    }
    ring_ = static_cast<uint8_t*>(base);

    // 6. Precompute a pointer to every frame slot. Frames never span a block,
    //    and kBlockSize is an exact multiple of kFrameSize, so each block tiles
    //    cleanly into kFramesPerBlock slots.
    frames_.reserve(total_frames_);
    for (int b = 0; b < num_blocks; ++b) {
      uint8_t* block = ring_ + static_cast<size_t>(b) * kBlockSize;
      for (uint32_t f = 0; f < kFramesPerBlock; ++f) {
        frames_.push_back(block + f * kFrameSize);
      }
    }

    // 7. Bind the socket to the interface — packets start filling the ring now.
    struct sockaddr_ll addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sll_family = AF_PACKET;
    addr.sll_protocol = htons(ETH_P_ALL);
    addr.sll_ifindex = ifindex;
    if (::bind(fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
      throw_errno("bind(AF_PACKET)");
    }
  } catch (...) {
    cleanup();
    throw;
  }
}

PacketRing::~PacketRing() { cleanup(); }

void PacketRing::cleanup() noexcept {
  if (ring_ != nullptr) {
    ::munmap(ring_, ring_bytes_);
    ring_ = nullptr;
  }
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

bool PacketRing::receive(RxFrame& frame) {
  auto* hdr = reinterpret_cast<tpacket2_hdr*>(frames_[current_frame_]);

  // The kernel publishes a frame by setting TP_STATUS_USER after writing the
  // payload. Read the status word, and only trust the payload once we have
  // observed the USER bit — the full barrier below orders the payload loads
  // after the status load (acquire).
  const uint32_t status =
      *reinterpret_cast<volatile uint32_t*>(&hdr->tp_status);
  if ((status & TP_STATUS_USER) == 0) {
    return false;  // still owned by the kernel; never blocks
  }
  __sync_synchronize();

  if (status & TP_STATUS_LOSING) {
    ++drops_;  // ring overflowed at/around this slot
  }

  // tp_mac is the offset from the frame header to the start of the link-layer
  // (Ethernet) header, accounting for TPACKET alignment padding.
  frame.data = reinterpret_cast<const uint8_t*>(hdr) + hdr->tp_mac;
  frame.len = hdr->tp_len;
  frame.captured_len = hdr->tp_snaplen;
  return true;
}

void PacketRing::release() {
  auto* hdr = reinterpret_cast<tpacket2_hdr*>(frames_[current_frame_]);

  // Ensure our reads of the payload complete before we hand the slot back to
  // the kernel by clearing the status word to TP_STATUS_KERNEL (== 0).
  __sync_synchronize();
  *reinterpret_cast<volatile uint32_t*>(&hdr->tp_status) = TP_STATUS_KERNEL;

  current_frame_ = (current_frame_ + 1) % total_frames_;
}

void PacketRing::set_busy_poll(int micros) {
  if (::setsockopt(fd_, SOL_SOCKET, SO_BUSY_POLL, &micros, sizeof(micros)) < 0) {
    throw_errno("setsockopt(SO_BUSY_POLL)");
  }
}

}  // namespace uxnet
