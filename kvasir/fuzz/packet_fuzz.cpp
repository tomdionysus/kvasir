//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The packet reader over arbitrary bytes, fed in arbitrary pieces, and the
// handshake response decoder over every payload it yields: each returns a
// value or refuses with a ProtocolError, never anything else, and the reader
// holds no more than it was given. The first byte sets the piece size and
// whether the reader is restricted, the next four the capabilities offered.
// Registered with Dannan's fuzz harness.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

#include "dannan/fuzz/fuzz.h"
#include "kvasir/protocol/messages.h"
#include "kvasir/protocol/packet.h"

namespace {

void run(std::span<const uint8_t> data) {
  if (data.size() < 5) return;
  kvasir::protocol::PacketLimits limits;
  limits.max_payload = 8192;
  limits.restricted_payload = 512;
  kvasir::protocol::PacketReader reader(limits);
  reader.restrict((data[0] & 1) != 0);
  const size_t piece = static_cast<size_t>(data[0] >> 1) + 1;
  const uint32_t capabilities =
      static_cast<uint32_t>(data[1]) | (static_cast<uint32_t>(data[2]) << 8) | (static_cast<uint32_t>(data[3]) << 16) | (static_cast<uint32_t>(data[4]) << 24);
  const auto bytes = data.subspan(5);
  size_t fed = 0;
  try {
    while (fed < bytes.size()) {
      const auto part = bytes.subspan(fed, std::min(piece, bytes.size() - fed));
      reader.feed(part);
      fed += part.size();
      while (auto packet = reader.next()) {
        if (packet->payload.size() > limits.max_payload) throw std::logic_error("a payload past the limit");
        try {
          const auto response = kvasir::protocol::decode_handshake_response(packet->payload, capabilities);
          if (response.user.size() > kvasir::protocol::max_user_bytes) throw std::logic_error("a user past its bound");
          if (response.auth_response.size() > kvasir::protocol::max_auth_response_bytes) throw std::logic_error("an auth response past its bound");
        } catch (const kvasir::protocol::ProtocolError&) {
        }
      }
      if (reader.buffered() > fed) throw std::logic_error("the reader holds more than it was given");
    }
  } catch (const kvasir::protocol::ProtocolError&) {
  }
}

// A handshake response in one packet: protocol 4.1 and secure connection
// offered, a user "u" and an empty auth response.
const dannan::fuzz::Registration registration("packet", run, [] {
  const dannan::net::Bytes payload{0x00, 0x82, 0x08, 0x00, 0x00, 0x00, 0x00, 0x01, 0xff, 0, 0, 0, 0, 0, 0,   0, 0,
                                   0,    0,    0,    0,    0,    0,    0,    0,    0,    0, 0, 0, 0, 0, 'u', 0, 0};
  dannan::net::Bytes one{0x20, 0x00, 0x82, 0x08, 0x00, static_cast<uint8_t>(payload.size()), 0, 0, 1};
  one.insert(one.end(), payload.begin(), payload.end());
  auto split = one;
  split[0] = 0x05;  // fed three bytes at a time
  return std::vector<dannan::net::Bytes>{one, split, dannan::net::Bytes{0x01, 0, 0, 0, 0, 0xff, 0xff, 0xff, 0}};
});

}  // namespace
