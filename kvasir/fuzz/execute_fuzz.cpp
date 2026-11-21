//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// COM_STMT_EXECUTE's reader over arbitrary bodies, for a statement of up to
// 64 parameters, some with long data, and again with the types the first
// body bound: each returns a value or refuses with a ProtocolError, never
// anything else, with one value per parameter. The first byte is the
// parameter count, the second which have long data. Registered with Dannan's
// fuzz harness.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "dannan/fuzz/fuzz.h"
#include "kvasir/protocol/statements.h"

namespace {

void run(std::span<const uint8_t> data) {
  if (data.size() < 2) return;
  const size_t count = data[0] % 65;
  std::vector<std::optional<kvasir::protocol::ValueType>> bound(count);
  std::vector<std::optional<std::string>> long_data(count);
  for (size_t i = 0; i < count; ++i)
    if ((data[1] >> (i % 8)) & 1 && i % 3 == 0) long_data[i] = "long";
  const auto body = data.subspan(2);
  for (int round = 0; round < 2; ++round) {
    try {
      const auto e = kvasir::protocol::decode_execute(body, bound, long_data);
      if (e.params.size() != count) throw std::logic_error("not one value per parameter");
    } catch (const kvasir::protocol::ProtocolError&) {
    }
  }
}

// Bodies after the command byte: statement id, flags, iteration count, then
// for one parameter its null bitmap, new-types flag, type and value.
const dannan::fuzz::Registration registration("execute", run, [] {
  return std::vector<dannan::net::Bytes>{
      dannan::net::Bytes{0, 0, 1, 0, 0, 0, 0x00, 1, 0, 0, 0},
      dannan::net::Bytes{1, 0, 1, 0, 0, 0, 0x00, 1, 0, 0, 0, 0x00, 0x01, 0x08, 0x00, 7, 0, 0, 0, 0, 0, 0, 0},
      dannan::net::Bytes{2, 0, 1, 0, 0, 0, 0x00, 1, 0, 0, 0, 0x02, 0x01, 0xfd, 0x00, 0x08, 0x00, 2, 'a', 'b'},
  };
});

}  // namespace
