//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

#include "dannan/net/codec.h"

// The value encodings of the MySQL client/server protocol: integers
// little-endian in one, two, three, four or eight bytes; a length-encoded
// integer; a string with a length-encoded length, one ended by a NUL, or one
// running to the end of its packet. Dannan's own codec is big-endian with u32
// lengths, a different wire, so this one is Kvasir's; it keeps that one's
// shape and its Bytes. A reader is a parser of untrusted input: every length
// is checked against what is left and against the caller's maximum before
// anything is copied.
namespace kvasir::protocol {

using Bytes = dannan::net::Bytes;

// MySQL's error numbers for bytes that are not the protocol.
inline constexpr uint16_t er_handshake_error = 1043;
inline constexpr uint16_t er_net_packet_too_large = 1153;
inline constexpr uint16_t er_net_packets_out_of_order = 1156;
inline constexpr uint16_t er_malformed_packet = 1835;

// Bytes that are not the protocol. code() is the MySQL error number the
// client is told before the connection is closed, what() its message.
class ProtocolError : public std::runtime_error {
 public:
  ProtocolError(uint16_t code, const std::string& message) : std::runtime_error(message), _code(code) {}
  uint16_t code() const noexcept { return _code; }

 private:
  uint16_t _code;
};

class Writer {
 public:
  void u8(uint8_t value) { _data.push_back(value); }
  void u16(uint16_t value) { little(value, 2); }
  void u24(uint32_t value) { little(value, 3); }
  void u32(uint32_t value) { little(value, 4); }
  void u64(uint64_t value) { little(value, 8); }
  // A length-encoded integer: one byte below 251, else 0xfc, 0xfd or 0xfe and
  // two, three or eight bytes.
  void lenenc(uint64_t value);
  void raw(std::span<const uint8_t> bytes) { _data.insert(_data.end(), bytes.begin(), bytes.end()); }
  void raw(std::string_view bytes) { _data.insert(_data.end(), bytes.begin(), bytes.end()); }
  void zeros(size_t count) { _data.insert(_data.end(), count, 0); }
  void lenenc_string(std::string_view value);
  // Throws std::invalid_argument for a value holding a NUL, which would end it
  // early at the reader.
  void nul_string(std::string_view value);

  const Bytes& data() const noexcept { return _data; }
  Bytes take() noexcept { return std::move(_data); }

 private:
  void little(uint64_t value, int bytes);

  Bytes _data;
};

// Reads from a buffer the caller keeps alive. Throws ProtocolError
// (er_malformed_packet) for anything that cannot be read as asked.
class Reader {
 public:
  explicit Reader(std::span<const uint8_t> data) noexcept : _data(data) {}
  // A temporary buffer would be gone before the first read.
  explicit Reader(Bytes&&) = delete;

  uint8_t u8() { return static_cast<uint8_t>(little(1)); }
  uint16_t u16() { return static_cast<uint16_t>(little(2)); }
  uint32_t u24() { return static_cast<uint32_t>(little(3)); }
  uint32_t u32() { return static_cast<uint32_t>(little(4)); }
  uint64_t u64() { return little(8); }
  // Refuses 0xfb (NULL, which is not an integer) and 0xff.
  uint64_t lenenc();
  // Zero-copy: the span borrows the reader's buffer.
  std::span<const uint8_t> view(size_t count);
  void skip(size_t count) { view(count); }
  // Each refuses a string longer than `maximum`.
  std::string lenenc_string(size_t maximum);
  std::string nul_string(size_t maximum);
  std::string rest_string(size_t maximum);

  size_t remaining() const noexcept { return _data.size() - _position; }

 private:
  uint64_t little(int bytes);
  void need(size_t count) const;

  std::span<const uint8_t> _data;
  size_t _position = 0;
};

}  // namespace kvasir::protocol
