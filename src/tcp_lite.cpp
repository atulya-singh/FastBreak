// tcp_lite.cpp — see include/tcp_lite.h.
#include "tcp_lite.h"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "rdtsc.h"

namespace uxnet {

namespace {

const uint32_t* crc_table() {
  static uint32_t t[256];
  static const bool init = [] {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      t[i] = c;
    }
    return true;
  }();
  (void)init;
  return t;
}

uint32_t crc32_seg(const uint8_t* h, size_t hn, const uint8_t* p, size_t pn) {
  const uint32_t* t = crc_table();
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < hn; ++i) crc = t[(crc ^ h[i]) & 0xFF] ^ (crc >> 8);
  for (size_t i = 0; i < pn; ++i) crc = t[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
  return crc ^ 0xFFFFFFFFu;
}

}  // namespace

// --- TcpLiteHeader ----------------------------------------------------------

TcpLiteHeader TcpLiteHeader::make_syn(uint32_t seq) {
  TcpLiteHeader h{};
  h.seq_num = seq;
  h.flags = TCP_SYN;
  h.compute_checksum(nullptr, 0);
  return h;
}

TcpLiteHeader TcpLiteHeader::make_ack(uint32_t seq, uint32_t ack) {
  TcpLiteHeader h{};
  h.seq_num = seq;
  h.ack_num = ack;
  h.flags = TCP_ACK;
  h.compute_checksum(nullptr, 0);
  return h;
}

TcpLiteHeader TcpLiteHeader::make_data(uint32_t seq, uint32_t ack, uint16_t len) {
  TcpLiteHeader h{};
  h.seq_num = seq;
  h.ack_num = ack;
  h.flags = TCP_ACK;  // data packets piggyback a cumulative ACK
  h.payload_len = len;
  // checksum computed by the caller once the payload is in place.
  return h;
}

void TcpLiteHeader::compute_checksum(const uint8_t* payload, uint16_t len) {
  checksum = 0;
  checksum = crc32_seg(reinterpret_cast<const uint8_t*>(this), sizeof(*this),
                       payload, len);
}

bool TcpLiteHeader::verify_checksum(const uint8_t* payload, uint16_t len) const {
  TcpLiteHeader tmp = *this;
  tmp.checksum = 0;
  return crc32_seg(reinterpret_cast<const uint8_t*>(&tmp), sizeof(tmp), payload,
                   len) == checksum;
}

// --- UnackedQueue -----------------------------------------------------------

void UnackedQueue::mark_sent(uint32_t seq, BufferDescriptor* buf,
                             uint64_t sent_tsc) {
  Slot& s = slots_[seq & (TCP_LITE_WINDOW - 1)];
  if (s.occupied && s.buf) pool_.release(s.buf);  // shouldn't happen in-window
  s.buf = buf;
  s.sent_tsc = sent_tsc;
  s.seq = seq;
  s.occupied = true;
}

uint64_t UnackedQueue::mark_acked(uint32_t up_to_seq) {
  uint64_t newest_tsc = 0;
  uint32_t newest_seq = 0;
  bool any = false;
  for (Slot& s : slots_) {
    if (!s.occupied) continue;
    if (static_cast<int32_t>(s.seq - up_to_seq) < 0) {  // s.seq < up_to_seq
      if (!any || static_cast<int32_t>(s.seq - newest_seq) > 0) {
        newest_tsc = s.sent_tsc;
        newest_seq = s.seq;
        any = true;
      }
      pool_.release(s.buf);
      s.buf = nullptr;
      s.occupied = false;
    }
  }
  return any ? newest_tsc : 0;
}

std::vector<uint32_t> UnackedQueue::check_timeouts(uint64_t now_tsc,
                                                   uint64_t timeout_tsc) {
  std::vector<uint32_t> out;
  for (Slot& s : slots_) {
    if (s.occupied && now_tsc - s.sent_tsc >= timeout_tsc) out.push_back(s.seq);
  }
  return out;
}

BufferDescriptor* UnackedQueue::buffer_for(uint32_t seq) {
  Slot& s = slots_[seq & (TCP_LITE_WINDOW - 1)];
  return (s.occupied && s.seq == seq) ? s.buf : nullptr;
}

void UnackedQueue::touch(uint32_t seq, uint64_t now_tsc) {
  Slot& s = slots_[seq & (TCP_LITE_WINDOW - 1)];
  if (s.occupied && s.seq == seq) s.sent_tsc = now_tsc;
}

bool UnackedQueue::occupied(uint32_t seq) const {
  const Slot& s = slots_[seq & (TCP_LITE_WINDOW - 1)];
  return s.occupied && s.seq == seq;
}

void UnackedQueue::release_all() {
  for (Slot& s : slots_) {
    if (s.occupied && s.buf) pool_.release(s.buf);
    s.buf = nullptr;
    s.occupied = false;
  }
}

// --- RecvBitmap -------------------------------------------------------------

bool RecvBitmap::mark_received(uint32_t seq) {
  const int32_t off = static_cast<int32_t>(seq - base_seq_);
  if (off < 0) return false;                                // already delivered
  if (off >= static_cast<int32_t>(TCP_LITE_WINDOW)) return false;  // out of window
  if (received_[off]) return false;                         // duplicate
  received_[off] = true;
  return true;
}

uint32_t RecvBitmap::cumulative_ack() const {
  uint32_t n = 0;
  while (n < TCP_LITE_WINDOW && received_[n]) ++n;
  return base_seq_ + n;
}

void RecvBitmap::advance(uint32_t new_base) {
  const uint32_t shift = new_base - base_seq_;
  if (shift == 0) return;
  if (shift >= TCP_LITE_WINDOW)
    received_.reset();
  else
    received_ >>= shift;
  base_seq_ = new_base;
}

bool RecvBitmap::has(uint32_t seq) const {
  const int32_t off = static_cast<int32_t>(seq - base_seq_);
  if (off < 0 || off >= static_cast<int32_t>(TCP_LITE_WINDOW)) return false;
  return received_[off];
}

// --- TcpLiteSession ---------------------------------------------------------

TcpLiteSession::TcpLiteSession(BufferPool& pool, const sockaddr_in& local,
                               const sockaddr_in& remote)
    : pool_(pool), local_(local), remote_(remote), unacked_(pool) {
  fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd_ >= 0) {
    int one = 1;
    ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (::bind(fd_, reinterpret_cast<sockaddr*>(&local_), sizeof(local_)) != 0) {
      std::fprintf(stderr, "TcpLiteSession: bind failed (continuing)\n");
    }
  }
  transmit_ = [this](const uint8_t* p, size_t n) { udp_transmit(p, n); };
}

TcpLiteSession::~TcpLiteSession() {
  unacked_.release_all();
  if (fd_ >= 0) ::close(fd_);
}

void TcpLiteSession::udp_transmit(const uint8_t* pkt, size_t len) {
  if (fd_ < 0) return;
  ::sendto(fd_, pkt, len, 0, reinterpret_cast<const sockaddr*>(&remote_),
           sizeof(remote_));
}

void TcpLiteSession::send(const uint8_t* data, size_t len) {
  pending_tx_.insert(pending_tx_.end(), data, data + len);
  pump_tx();
}

void TcpLiteSession::pump_tx() {
  while (tx_off_ < pending_tx_.size() &&
         static_cast<uint32_t>(next_seq_ - snd_una_) < TCP_LITE_WINDOW) {
    BufferDescriptor* buf = pool_.acquire();
    if (buf == nullptr) break;  // pool exhausted, retry next tick

    const size_t remaining = pending_tx_.size() - tx_off_;
    const uint16_t plen =
        static_cast<uint16_t>(std::min(remaining, TCP_LITE_MAX_PAYLOAD));
    TcpLiteHeader h =
        TcpLiteHeader::make_data(next_seq_, recv_bitmap_.cumulative_ack(), plen);
    std::memcpy(buf->data, &h, sizeof(h));
    std::memcpy(buf->data + sizeof(h), pending_tx_.data() + tx_off_, plen);
    reinterpret_cast<TcpLiteHeader*>(buf->data)
        ->compute_checksum(buf->data + sizeof(h), plen);
    buf->len = static_cast<uint32_t>(sizeof(h) + plen);

    unacked_.mark_sent(next_seq_, buf, rdtsc());
    transmit_(buf->data, buf->len);
    bytes_sent_ += plen;
    tx_off_ += plen;
    ++next_seq_;
  }
  if (tx_off_ > 0 && tx_off_ == pending_tx_.size()) {
    pending_tx_.clear();
    tx_off_ = 0;
  }
}

void TcpLiteSession::on_packet_received(BufferDescriptor* buf) {
  const TcpLiteHeader* h = reinterpret_cast<const TcpLiteHeader*>(buf->data);
  const uint16_t plen = h->payload_len;
  const uint8_t* payload = buf->data + sizeof(TcpLiteHeader);

  if (!h->verify_checksum(payload, plen)) {  // corrupt — drop, sender will retx
    pool_.release(buf);
    return;
  }

  if (h->flags & TCP_ACK) {
    if (static_cast<int32_t>(h->ack_num - snd_una_) > 0) {
      const uint64_t sent = unacked_.mark_acked(h->ack_num);
      snd_una_ = h->ack_num;
      if (sent != 0) {
        const uint64_t sample = static_cast<uint64_t>(cycles_to_ns(rdtsc() - sent));
        rtt_ns_ = rtt_ns_ ? (7 * rtt_ns_ + sample) / 8 : sample;
      }
      pump_tx();  // window opened
    }
  }

  if (plen > 0) {
    const uint32_t seq = h->seq_num;
    if (recv_bitmap_.mark_received(seq))
      reasm_[seq & (TCP_LITE_WINDOW - 1)].assign(payload, payload + plen);
    deliver_in_order();
    pending_ack_ = true;
  }

  pool_.release(buf);
}

void TcpLiteSession::deliver_in_order() {
  while (recv_bitmap_.has(recv_bitmap_.base_seq())) {
    const uint32_t seq = recv_bitmap_.base_seq();
    std::vector<uint8_t>& v = reasm_[seq & (TCP_LITE_WINDOW - 1)];
    if (data_cb_) data_cb_(v.data(), v.size());
    bytes_received_ += v.size();
    v.clear();
    recv_bitmap_.advance(seq + 1);
  }
}

void TcpLiteSession::tick() {
  const uint64_t now = rdtsc();
  const uint64_t timeout_cyc =
      static_cast<uint64_t>(rto_ns_ * tsc_frequency_ghz());
  for (uint32_t seq : unacked_.check_timeouts(now, timeout_cyc)) {
    BufferDescriptor* b = unacked_.buffer_for(seq);
    if (b != nullptr) {
      transmit_(b->data, b->len);
      unacked_.touch(seq, now);
      ++retransmits_;
    }
  }

  pump_tx();

  if (pending_ack_) {
    TcpLiteHeader h =
        TcpLiteHeader::make_ack(next_seq_, recv_bitmap_.cumulative_ack());
    uint8_t pkt[sizeof(TcpLiteHeader)];
    std::memcpy(pkt, &h, sizeof(h));
    transmit_(pkt, sizeof(pkt));
    pending_ack_ = false;
  }
}

}  // namespace uxnet
