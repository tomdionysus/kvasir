//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "kvasir/protocol/codec.h"

// MySQL's packet framing: a three-byte little-endian payload length, a
// one-byte sequence id, then the payload. A payload of 0xffffff bytes or more
// is sent as fragments of 0xffffff bytes, each with the next sequence id, and
// ends with a shorter fragment, an empty one when the payload is an exact
// multiple. Bytes arrive from outside the process, so the reader is a parser
// of untrusted input: a payload's size is bounded as each fragment's header is
// read, before its bytes are kept.
namespace kvasir::protocol {

inline constexpr size_t max_fragment = 0xffffff;

struct PacketLimits {
  // One payload: MySQL 8.4's default max_allowed_packet.
  size_t max_payload = 64u * 1024 * 1024;
  // For a connection not yet authenticated: nothing a client sends before it
  // has signed in is large.
  size_t restricted_payload = 64 * 1024;
};

// One payload, whole, with the sequence ids of its first and last fragments.
struct Packet {
  uint8_t first_sequence = 0;
  uint8_t last_sequence = 0;
  Bytes payload;
};

// One connection's packets, taken from its bytes in order. Not thread-safe:
// one connection's strand owns it.
class PacketReader {
 public:
  explicit PacketReader(PacketLimits limits = {});

  // Appends bytes received.
  void feed(std::span<const uint8_t> bytes);
  // The next whole payload; none until one has arrived whole. Throws
  // ProtocolError for a payload over the limit (er_net_packet_too_large) or a
  // fragment out of sequence (er_net_packets_out_of_order); the reader is then
  // spent and throws the same error again.
  std::optional<Packet> next();

  // While true, the restricted limit applies (an unauthenticated connection).
  void restrict(bool on) noexcept { _restricted = on; }
  // Bytes held: received and not yet part of a payload returned.
  size_t buffered() const noexcept { return (_buffer.size() - _position) + _payload.size(); }

 private:
  [[noreturn]] void fail(uint16_t code, const char* message);

  PacketLimits _limits;
  bool _restricted = false;
  Bytes _buffer;
  size_t _position = 0;
  std::optional<ProtocolError> _failed;

  // A payload being assembled from fragments.
  bool _assembling = false;
  uint8_t _first_sequence = 0;
  uint8_t _expected_sequence = 0;
  Bytes _payload;
};

// Appends `payload` to `out` as one packet or as fragments, numbering from
// `sequence` and leaving it at the id the next packet takes.
void frame(std::span<const uint8_t> payload, uint8_t& sequence, Bytes& out);

}  // namespace kvasir::protocol
