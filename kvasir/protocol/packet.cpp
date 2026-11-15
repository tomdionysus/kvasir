//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "kvasir/protocol/packet.h"

#include <algorithm>
#include <utility>

namespace kvasir::protocol {

PacketReader::PacketReader(PacketLimits limits) : _limits(limits) {}

void PacketReader::fail(uint16_t code, const char* message) {
  _failed.emplace(code, message);
  _buffer.clear();
  _position = 0;
  _payload.clear();
  throw *_failed;
}

void PacketReader::feed(std::span<const uint8_t> bytes) {
  if (_failed) return;
  // Drop what has been consumed before growing.
  if (_position > 0 && _position >= _buffer.size() / 2) {
    _buffer.erase(_buffer.begin(), _buffer.begin() + static_cast<std::ptrdiff_t>(_position));
    _position = 0;
  }
  _buffer.insert(_buffer.end(), bytes.begin(), bytes.end());
}

std::optional<Packet> PacketReader::next() {
  if (_failed) throw *_failed;
  const size_t limit = _restricted ? _limits.restricted_payload : _limits.max_payload;
  for (;;) {
    const size_t available = _buffer.size() - _position;
    if (available < 4) return std::nullopt;
    const uint8_t* header = _buffer.data() + _position;
    const size_t length = static_cast<size_t>(header[0]) | (static_cast<size_t>(header[1]) << 8) | (static_cast<size_t>(header[2]) << 16);
    const uint8_t sequence = header[3];
    if (_assembling && sequence != _expected_sequence) fail(er_net_packets_out_of_order, "Got packets out of order");
    // Bounded on the header, before the fragment's bytes are kept.
    if (length > limit || _payload.size() > limit - length) fail(er_net_packet_too_large, "Got a packet bigger than 'max_allowed_packet' bytes");
    if (available - 4 < length) return std::nullopt;

    if (!_assembling) {
      _assembling = true;
      _first_sequence = sequence;
    }
    _payload.insert(_payload.end(), header + 4, header + 4 + length);
    _position += 4 + length;
    _expected_sequence = static_cast<uint8_t>(sequence + 1);
    if (length == max_fragment) continue;

    Packet packet{_first_sequence, sequence, std::move(_payload)};
    _payload = Bytes();
    _assembling = false;
    return packet;
  }
}

void frame(std::span<const uint8_t> payload, uint8_t& sequence, Bytes& out) {
  size_t offset = 0;
  for (;;) {
    const size_t length = std::min(payload.size() - offset, max_fragment);
    out.push_back(static_cast<uint8_t>(length));
    out.push_back(static_cast<uint8_t>(length >> 8));
    out.push_back(static_cast<uint8_t>(length >> 16));
    out.push_back(sequence++);
    out.insert(out.end(), payload.begin() + static_cast<std::ptrdiff_t>(offset), payload.begin() + static_cast<std::ptrdiff_t>(offset + length));
    offset += length;
    // A full fragment is always followed by another, empty if need be.
    if (length < max_fragment) return;
  }
}

}  // namespace kvasir::protocol
