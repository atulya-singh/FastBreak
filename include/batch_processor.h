// batch_processor.h — Layer 4: batched drain + dispatch-table processing.
#ifndef UXNET_BATCH_PROCESSOR_H
#define UXNET_BATCH_PROCESSOR_H

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "buffer_pool.h"
#include "packet_parser.h"
#include "rss_distributor.h"

namespace uxnet {

struct ProcessingBatch {
  static constexpr size_t BATCH_SIZE = 32;
  std::array<BufferDescriptor*, BATCH_SIZE> bufs;
  std::array<ParsedPacket, BATCH_SIZE> packets;
  size_t count = 0;
};

class BatchHandler {
 public:
  virtual ~BatchHandler() = default;
  virtual void handle(const ParsedPacket& pkt, BufferDescriptor* buf) = 0;
  virtual void flush() = 0;
};

struct alignas(64) BatchStats {
  uint64_t batches_processed = 0;
  uint64_t packets_processed = 0;
  uint64_t packets_dispatched = 0;
};

class BatchProcessor {
 public:
  BatchProcessor(BufferPool& pool, RSSDistributor& rss, size_t queue_index);

  void run_once();
  void dispatch_batch(ProcessingBatch& batch);
  void register_udp_handler(uint16_t port, BatchHandler* handler);
  void set_default_handler(BatchHandler* handler) { default_handler_ = handler; }

  uint64_t batches_processed() const { return stats_.batches_processed; }
  uint64_t packets_processed() const { return stats_.packets_processed; }
  uint64_t packets_dispatched() const { return stats_.packets_dispatched; }
  double avg_batch_size() const {
    return stats_.batches_processed
               ? static_cast<double>(stats_.packets_processed) /
                     stats_.batches_processed
               : 0.0;
  }

 private:
  BufferPool& pool_;
  RSSDistributor& rss_;
  size_t queue_index_;
  ProcessingBatch batch_;
  BatchStats stats_;
  BatchHandler* default_handler_ = nullptr;
  std::unique_ptr<BatchHandler*[]> udp_handlers_;  // 65536 slots, port-indexed
};

}  // namespace uxnet

#endif  // UXNET_BATCH_PROCESSOR_H
