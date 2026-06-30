// tcp_lite.h — Layer 5: minimal reliable datagram protocol over UDP.
#ifndef UXNET_TCP_LITE_H
#define UXNET_TCP_LITE_H

#include <netinet/in.h>

#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "buffer_pool.h"

namespace uxnet {

// ===========================================================================
// TCP-Lite protocol
//
// A reliable, ordered datagram protocol layered on UDP, stripped down for
// low-latency trading traffic. Wire header is 16 bytes, packed, host byte
// order (single-datacenter / loopback use):
//
//   uint32_t seq_num      sender's per-packet sequence number
//   uint32_t ack_num      cumulative ACK — the next seq the receiver expects
//   uint16_t flags        SYN=0x01  ACK=0x02  FIN=0x04  RST=0x08
//   uint16_t payload_len  bytes of data following the header
//   uint32_t checksum     CRC32 over header+payload with this field zeroed
//
// Deliberately omitted, because they are the wrong trade-off for HFT:
//   - no flow control          - no congestion control / slow start
//   - no Nagle coalescing      - no delayed-ACK batching beyond one tick
//
// Reliability = fixed-timeout retransmit (default 1 ms, configurable) over a
// 256-packet bitmap receive window for out-of-order tracking. Sequence
// numbers count packets (not bytes), so the send queue and receive window
// both index by (seq & 255).
// ===========================================================================

enum TcpFlags : uint16_t {
  TCP_SYN = 0x01,
  TCP_ACK = 0x02,
  TCP_FIN = 0x04,
  TCP_RST = 0x08,
};

struct __attribute__((packed)) TcpLiteHeader {
  uint32_t seq_num;
  uint32_t ack_num;
  uint16_t flags;
  uint16_t payload_len;
  uint32_t checksum;

  static TcpLiteHeader make_syn(uint32_t seq);
  static TcpLiteHeader make_ack(uint32_t seq, uint32_t ack);
  static TcpLiteHeader make_data(uint32_t seq, uint32_t ack, uint16_t len);

  // checksum field is treated as 0 during CRC32 computation.
  void compute_checksum(const uint8_t* payload, uint16_t len);
  bool verify_checksum(const uint8_t* payload, uint16_t len) const;
};
static_assert(sizeof(TcpLiteHeader) == 16, "TcpLiteHeader must be 16 bytes");

constexpr uint32_t TCP_LITE_WINDOW = 256;
constexpr size_t TCP_LITE_MAX_PAYLOAD = 1024;
constexpr size_t TCP_LITE_MTU = sizeof(TcpLiteHeader) + TCP_LITE_MAX_PAYLOAD;

// Circular buffer of unacknowledged sent packets, indexed by seq & 255.
class UnackedQueue {
 public:
  explicit UnackedQueue(BufferPool& pool) : pool_(pool) {}
  ~UnackedQueue() { release_all(); }

  void mark_sent(uint32_t seq, BufferDescriptor* buf, uint64_t sent_tsc);
  // Release all packets with seq < up_to_seq; returns the sent_tsc of the
  // newest acked packet (for an RTT sample), or 0 if nothing was acked.
  uint64_t mark_acked(uint32_t up_to_seq);
  std::vector<uint32_t> check_timeouts(uint64_t now_tsc, uint64_t timeout_tsc);

  BufferDescriptor* buffer_for(uint32_t seq);
  void touch(uint32_t seq, uint64_t now_tsc);
  bool occupied(uint32_t seq) const;
  void release_all();

 private:
  struct Slot {
    BufferDescriptor* buf = nullptr;
    uint64_t sent_tsc = 0;
    uint32_t seq = 0;
    bool occupied = false;
  };
  BufferPool& pool_;
  std::array<Slot, TCP_LITE_WINDOW> slots_{};
};

// Tracks which sequence numbers have arrived within the 256-packet window.
class RecvBitmap {
 public:
  bool mark_received(uint32_t seq);  // false if duplicate or outside window
  uint32_t cumulative_ack() const;   // next expected seq (one past contiguous)
  void advance(uint32_t new_base);
  bool has(uint32_t seq) const;
  uint32_t base_seq() const { return base_seq_; }

 private:
  std::bitset<TCP_LITE_WINDOW> received_;
  uint32_t base_seq_ = 0;
};

class TcpLiteSession {
 public:
  using DataCallback = std::function<void(const uint8_t* data, size_t len)>;
  using TransmitFn = std::function<void(const uint8_t* pkt, size_t len)>;

  TcpLiteSession(BufferPool& pool, const sockaddr_in& local,
                 const sockaddr_in& remote);
  ~TcpLiteSession();

  TcpLiteSession(const TcpLiteSession&) = delete;
  TcpLiteSession& operator=(const TcpLiteSession&) = delete;

  void send(const uint8_t* data, size_t len);
  void on_packet_received(BufferDescriptor* buf);
  void tick();

  void set_data_callback(DataCallback cb) { data_cb_ = std::move(cb); }
  void set_transmit(TransmitFn fn) { transmit_ = std::move(fn); }
  void set_rto_ns(uint64_t ns) { rto_ns_ = ns; }

  int fd() const { return fd_; }
  uint64_t bytes_sent() const { return bytes_sent_; }
  uint64_t bytes_received() const { return bytes_received_; }
  uint64_t retransmits() const { return retransmits_; }
  uint64_t round_trip_time_ns() const { return rtt_ns_; }

 private:
  void pump_tx();
  void deliver_in_order();
  void udp_transmit(const uint8_t* pkt, size_t len);

  BufferPool& pool_;
  sockaddr_in local_{};
  sockaddr_in remote_{};
  int fd_ = -1;

  DataCallback data_cb_;
  TransmitFn transmit_;
  uint64_t rto_ns_ = 1'000'000;  // 1 ms

  UnackedQueue unacked_;
  std::vector<uint8_t> pending_tx_;
  size_t tx_off_ = 0;
  uint32_t next_seq_ = 0;
  uint32_t snd_una_ = 0;

  RecvBitmap recv_bitmap_;
  std::array<std::vector<uint8_t>, TCP_LITE_WINDOW> reasm_;
  bool pending_ack_ = false;

  uint64_t bytes_sent_ = 0;
  uint64_t bytes_received_ = 0;
  uint64_t retransmits_ = 0;
  uint64_t rtt_ns_ = 0;
};

}  // namespace uxnet

#endif  // UXNET_TCP_LITE_H
