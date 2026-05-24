#ifndef UXNET_PACKET_PARSER_H
#define UXNET_PACKET_PARSER_H

#include <array>
#include <cstddef>
#include <cstdint>

namespace uxnet {

inline uint16_t rd16(const uint8_t* p) {
  return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

inline uint32_t rd32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) |
         (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

class EthernetView {
 public:
  EthernetView() = default;
  EthernetView(const uint8_t* buf, size_t buf_len) : buf_(buf), len_(buf_len) {}

  bool valid() const { return len_ >= 14; }

  std::array<uint8_t, 6> dst_mac() const {
    return {buf_[0], buf_[1], buf_[2], buf_[3], buf_[4], buf_[5]};
  }
  std::array<uint8_t, 6> src_mac() const {
    return {buf_[6], buf_[7], buf_[8], buf_[9], buf_[10], buf_[11]};
  }
  uint16_t ethertype() const { return rd16(buf_ + 12); }

  const uint8_t* payload() const { return buf_ + 14; }
  size_t payload_len(size_t total_len) const { return total_len - 14; }

 private:
  const uint8_t* buf_ = nullptr;
  size_t len_ = 0;
};

class IPv4View {
 public:
  IPv4View() = default;
  IPv4View(const uint8_t* buf, size_t len) : buf_(buf), len_(len) {}

  bool valid() const {
    return (buf_[0] >> 4) == 4 && len_ >= 20 && header_len() >= 20;
  }

  size_t header_len() const { return (buf_[0] & 0x0F) * 4; }
  uint8_t protocol() const { return buf_[9]; }
  uint32_t src_ip() const { return rd32(buf_ + 12); }
  uint32_t dst_ip() const { return rd32(buf_ + 16); }
  uint16_t total_len() const { return rd16(buf_ + 2); }

  const uint8_t* payload() const { return buf_ + header_len(); }
  size_t payload_len() const { return total_len() - header_len(); }

  bool checksum_valid() const {
    const size_t hl = header_len();
    uint32_t sum = 0;
    for (size_t i = 0; i + 1 < hl; i += 2) sum += rd16(buf_ + i);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return sum == 0xFFFF;
  }

 private:
  const uint8_t* buf_ = nullptr;
  size_t len_ = 0;
};

class UDPView {
 public:
  UDPView() = default;
  UDPView(const uint8_t* buf, size_t len) : buf_(buf), len_(len) {}

  bool valid() const { return len_ >= 8; }

  uint16_t src_port() const { return rd16(buf_ + 0); }
  uint16_t dst_port() const { return rd16(buf_ + 2); }
  uint16_t length() const { return rd16(buf_ + 4); }

  const uint8_t* payload() const { return buf_ + 8; }
  size_t payload_len() const { return length() - 8; }

 private:
  const uint8_t* buf_ = nullptr;
  size_t len_ = 0;
};

struct ParsedPacket {
  bool valid = false;
  EthernetView eth;
  IPv4View ip;
  UDPView udp;
  bool has_ip = false;
  bool has_udp = false;
  const uint8_t* raw = nullptr;
  size_t raw_len = 0;
};

class PacketParser {
 public:
  static ParsedPacket parse(const uint8_t* buf, size_t len) {
    ParsedPacket p;
    p.raw = buf;
    p.raw_len = len;

    p.eth = EthernetView(buf, len);
    const bool eth_ok = p.eth.valid();

    p.ip = IPv4View(p.eth.payload(), len - 14);
    const bool is_ipv4 =
        eth_ok & (p.eth.ethertype() == 0x0800) & p.ip.valid();
    p.has_ip = is_ipv4;

    p.udp = UDPView(p.ip.payload(), p.ip.payload_len());
    const bool is_udp = is_ipv4 & (p.ip.protocol() == 17) & p.udp.valid();
    p.has_udp = is_udp;

    // Drop IPv4 packets whose header checksum does not verify.
    const bool csum_ok = (!is_ipv4) | p.ip.checksum_valid();
    p.valid = eth_ok & csum_ok;
    return p;
  }
};

}  // namespace uxnet

#endif  // UXNET_PACKET_PARSER_H
