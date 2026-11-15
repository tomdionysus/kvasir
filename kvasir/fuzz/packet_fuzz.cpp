//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The packet reader over arbitrary bytes, fed in arbitrary pieces, and the
// handshake response decoder over every payload it yields: each returns a
// value or refuses with a ProtocolError, never anything else, and the reader
// holds no more than it was given.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>

#include "kvasir/protocol/messages.h"
#include "kvasir/protocol/packet.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size < 5) return 0;
  kvasir::protocol::PacketLimits limits;
  limits.max_payload = 8192;
  limits.restricted_payload = 512;
  kvasir::protocol::PacketReader reader(limits);
  reader.restrict((data[0] & 1) != 0);
  // The first byte also sets the piece size, so pieces split every boundary.
  const size_t piece = static_cast<size_t>(data[0] >> 1) + 1;
  // The next four bytes are the capabilities the server is taken to offer.
  const uint32_t capabilities =
      static_cast<uint32_t>(data[1]) | (static_cast<uint32_t>(data[2]) << 8) | (static_cast<uint32_t>(data[3]) << 16) | (static_cast<uint32_t>(data[4]) << 24);
  const std::span<const uint8_t> bytes(data + 5, size - 5);
  size_t fed = 0;
  try {
    while (fed < bytes.size()) {
      const auto part = bytes.subspan(fed, std::min(piece, bytes.size() - fed));
      reader.feed(part);
      fed += part.size();
      while (auto packet = reader.next()) {
        if (packet->payload.size() > limits.max_payload) std::abort();
        try {
          const auto response = kvasir::protocol::decode_handshake_response(packet->payload, capabilities);
          if (response.user.size() > kvasir::protocol::max_user_bytes) std::abort();
          if (response.auth_response.size() > kvasir::protocol::max_auth_response_bytes) std::abort();
        } catch (const kvasir::protocol::ProtocolError&) {
        }
      }
      if (reader.buffered() > fed) std::abort();
    }
  } catch (const kvasir::protocol::ProtocolError&) {
  }
  return 0;
}
