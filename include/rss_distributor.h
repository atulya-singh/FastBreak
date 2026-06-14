// rss_distributor.h — Layer 3: Toeplitz RSS hashing into per-queue SPSC rings.
#ifndef UXNET_RSS_DISTRIBUTOR_H
#define UXNET_RSS_DISTRIBUTOR_H

#include <cstddef>
#include <cstdint>
#include <memory>

#include "buffer_pool.h"
#include "packet_parser.h"
#include "spsc_ring.h"

namespace uxnet {

class RSSDistributor {
 public:
  using Queue = SPSCRing<BufferDescriptor*, 8192>;

  explicit RSSDistributor(size_t queue_count);

  // Raw 32-bit Toeplitz hash of the 4-tuple, matching NIC RSS.
  static uint32_t toeplitz(uint32_t src_ip, uint32_t dst_ip, uint16_t src_port,
                           uint16_t dst_port) noexcept;

  // Toeplitz hash masked to a queue index.
  uint32_t hash(uint32_t src_ip, uint32_t dst_ip, uint16_t src_port,
                uint16_t dst_port) const noexcept {
    return toeplitz(src_ip, dst_ip, src_port, dst_port) & mask_;
  }

  bool submit(BufferDescriptor* buf, const ParsedPacket& pkt);

  Queue& get_queue(size_t index) { return queues_[index]; }
  size_t queue_count() const { return queue_count_; }

 private:
  size_t queue_count_;
  uint32_t mask_;
  std::unique_ptr<Queue[]> queues_;
};

}  // namespace uxnet

#endif  // UXNET_RSS_DISTRIBUTOR_H
