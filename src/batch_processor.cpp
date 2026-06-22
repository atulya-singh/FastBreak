// batch_processor.cpp — Layer 4 implementation. See include/batch_processor.h.
#include "batch_processor.h"

namespace uxnet {

BatchProcessor::BatchProcessor(BufferPool& pool, RSSDistributor& rss,
                               size_t queue_index)
    : pool_(pool),
      rss_(rss),
      queue_index_(queue_index),
      udp_handlers_(std::make_unique<BatchHandler*[]>(65536)) {
  for (size_t i = 0; i < 65536; ++i) udp_handlers_[i] = nullptr;
}

void BatchProcessor::run_once() {
  auto& queue = rss_.get_queue(queue_index_);

  size_t n = 0;
  while (n < ProcessingBatch::BATCH_SIZE && queue.pop(batch_.bufs[n])) ++n;
  batch_.count = n;
  if (n == 0) return;

  for (size_t i = 0; i < n; ++i) {
    if (i + 2 < n) __builtin_prefetch(batch_.bufs[i + 2]->data, 0, 0);
    batch_.packets[i] =
        PacketParser::parse(batch_.bufs[i]->data, batch_.bufs[i]->len);
  }

  dispatch_batch(batch_);

  for (size_t i = 0; i < n; ++i) pool_.release(batch_.bufs[i]);

  stats_.batches_processed += 1;
  stats_.packets_processed += n;
}

void BatchProcessor::dispatch_batch(ProcessingBatch& batch) {
  std::array<BatchHandler*, ProcessingBatch::BATCH_SIZE + 1> touched;
  size_t touched_count = 0;
  auto mark = [&](BatchHandler* h) {
    for (size_t j = 0; j < touched_count; ++j)
      if (touched[j] == h) return;
    touched[touched_count++] = h;
  };

  for (size_t i = 0; i < batch.count; ++i) {
    const ParsedPacket& pkt = batch.packets[i];
    BatchHandler* h = pkt.has_udp ? udp_handlers_[pkt.udp.dst_port()] : nullptr;
    if (h == nullptr) h = default_handler_;
    if (h != nullptr) {
      h->handle(pkt, batch.bufs[i]);
      stats_.packets_dispatched += 1;
      mark(h);
    }
  }

  for (size_t j = 0; j < touched_count; ++j) touched[j]->flush();
}

void BatchProcessor::register_udp_handler(uint16_t port, BatchHandler* handler) {
  udp_handlers_[port] = handler;
}

}  // namespace uxnet
