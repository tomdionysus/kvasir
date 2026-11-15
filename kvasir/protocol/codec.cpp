//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "kvasir/protocol/codec.h"

#include <algorithm>

namespace kvasir::protocol {

namespace {

[[noreturn]] void malformed() { throw ProtocolError(er_malformed_packet, "Malformed communication packet."); }

}  // namespace

void Writer::little(uint64_t value, int bytes) {
  for (int i = 0; i < bytes; ++i) _data.push_back(static_cast<uint8_t>(value >> (8 * i)));
}

void Writer::lenenc(uint64_t value) {
  if (value < 251) {
    u8(static_cast<uint8_t>(value));
  } else if (value < (1u << 16)) {
    u8(0xfc);
    u16(static_cast<uint16_t>(value));
  } else if (value < (1u << 24)) {
    u8(0xfd);
    u24(static_cast<uint32_t>(value));
  } else {
    u8(0xfe);
    u64(value);
  }
}

void Writer::lenenc_string(std::string_view value) {
  lenenc(value.size());
  raw(value);
}

void Writer::nul_string(std::string_view value) {
  if (value.find('\0') != std::string_view::npos) throw std::invalid_argument("a NUL-terminated string cannot hold a NUL");
  raw(value);
  u8(0);
}

void Reader::need(size_t count) const {
  if (count > remaining()) malformed();
}

uint64_t Reader::little(int bytes) {
  need(static_cast<size_t>(bytes));
  uint64_t value = 0;
  for (int i = 0; i < bytes; ++i) value |= static_cast<uint64_t>(_data[_position + static_cast<size_t>(i)]) << (8 * i);
  _position += static_cast<size_t>(bytes);
  return value;
}

uint64_t Reader::lenenc() {
  const uint8_t first = u8();
  if (first < 251) return first;
  if (first == 0xfc) return u16();
  if (first == 0xfd) return u24();
  if (first == 0xfe) return u64();
  malformed();
}

std::span<const uint8_t> Reader::view(size_t count) {
  need(count);
  const auto out = _data.subspan(_position, count);
  _position += count;
  return out;
}

std::string Reader::lenenc_string(size_t maximum) {
  const uint64_t length = lenenc();
  if (length > maximum) malformed();
  const auto bytes = view(static_cast<size_t>(length));
  return std::string(bytes.begin(), bytes.end());
}

std::string Reader::nul_string(size_t maximum) {
  const auto left = _data.subspan(_position);
  const auto end = std::find(left.begin(), left.end(), uint8_t{0});
  if (end == left.end()) malformed();
  const auto length = static_cast<size_t>(end - left.begin());
  if (length > maximum) malformed();
  _position += length + 1;
  return std::string(left.begin(), end);
}

std::string Reader::rest_string(size_t maximum) {
  if (remaining() > maximum) malformed();
  const auto bytes = view(remaining());
  return std::string(bytes.begin(), bytes.end());
}

}  // namespace kvasir::protocol
