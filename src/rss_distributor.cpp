// rss_distributor.cpp — Layer 3 implementation. See include/rss_distributor.h.
#include "rss_distributor.h"

#include <cassert>

namespace uxnet {
namespace {

// Standard 40-byte Microsoft/Intel RSS key (the 82599 datasheet test key).
constexpr uint8_t kRssKey[40] = {
    0x6d, 0x5a, 0x56, 0xda, 0x25, 0x5b, 0x0e, 0xc2, 0x41, 0x67,
    0x25, 0x3d, 0x43, 0xa3, 0x8f, 0xb0, 0xd0, 0xca, 0x2b, 0xcb,
    0xae, 0x7b, 0x30, 0xb4, 0x77, 0xcb, 0x2d, 0xa3, 0x80, 0x30,
    0xf2, 0x0c, 0x6a, 0x42, 0xb7, 0x3b, 0xbe, 0xac, 0x01, 0xfa};

}  // namespace

RSSDistributor::RSSDistributor(size_t queue_count)
    : queue_count_(queue_count),
      mask_(static_cast<uint32_t>(queue_count - 1)) {
  assert(queue_count != 0 && (queue_count & (queue_count - 1)) == 0 &&
         "queue_count must be a power of two");
  queues_ = std::make_unique<Queue[]>(queue_count);
}

uint32_t RSSDistributor::toeplitz(uint32_t src_ip, uint32_t dst_ip,
                                  uint16_t src_port,
                                  uint16_t dst_port) noexcept {
  uint8_t input[12] = {
      static_cast<uint8_t>(src_ip >> 24), static_cast<uint8_t>(src_ip >> 16),
      static_cast<uint8_t>(src_ip >> 8),  static_cast<uint8_t>(src_ip),
      static_cast<uint8_t>(dst_ip >> 24), static_cast<uint8_t>(dst_ip >> 16),
      static_cast<uint8_t>(dst_ip >> 8),  static_cast<uint8_t>(dst_ip),
      static_cast<uint8_t>(src_port >> 8), static_cast<uint8_t>(src_port),
      static_cast<uint8_t>(dst_port >> 8), static_cast<uint8_t>(dst_port)};

  uint32_t result = 0;
  // 32-bit sliding window over the key bit-stream, primed with key bytes 0..3.
  uint32_t window = (static_cast<uint32_t>(kRssKey[0]) << 24) |
                    (static_cast<uint32_t>(kRssKey[1]) << 16) |
                    (static_cast<uint32_t>(kRssKey[2]) << 8) | kRssKey[3];
  uint32_t next_bit = 32;  // index of the next key bit to shift in

  for (size_t byte = 0; byte < sizeof(input); ++byte) {
    for (int bit = 7; bit >= 0; --bit) {
      if ((input[byte] >> bit) & 1) result ^= window;
      const uint8_t key_byte = kRssKey[next_bit >> 3];
      const uint32_t key_bit = (key_byte >> (7 - (next_bit & 7))) & 1;
      window = (window << 1) | key_bit;
      ++next_bit;
    }
  }
  return result;
}

bool RSSDistributor::submit(BufferDescriptor* buf, const ParsedPacket& pkt) {
  uint32_t index;
  if (pkt.has_udp) {
    index = hash(pkt.ip.src_ip(), pkt.ip.dst_ip(), pkt.udp.src_port(),
                 pkt.udp.dst_port());
  } else if (pkt.has_ip) {
    index = hash(pkt.ip.src_ip(), pkt.ip.dst_ip(), 0, 0);
  } else {
    index = 0;
  }
  return queues_[index].push(buf);
}

}  // namespace uxnet
