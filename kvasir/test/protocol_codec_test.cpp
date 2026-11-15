//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// MySQL 8.4 manual, "Basic Data Types" of the client/server protocol:
// fixed-length integers are little-endian; a length-encoded integer is one
// byte below 251, else 0xfc and two bytes, 0xfd and three, 0xfe and eight;
// 0xfb and 0xff are not integers; strings are length-encoded, NUL-terminated
// or run to the end of the packet. A reader refuses what does not fit.

#include <gtest/gtest.h>

#include <cstdint>
#include <stdexcept>
#include <string>

#include "kvasir/protocol/codec.h"

namespace kvasir::protocol {
namespace {

Bytes bytes(std::initializer_list<int> values) {
  Bytes out;
  for (int v : values) out.push_back(static_cast<uint8_t>(v));
  return out;
}

Bytes lenenc(uint64_t value) {
  Writer w;
  w.lenenc(value);
  return w.take();
}

TEST(ProtocolCodec, FixedIntegersAreLittleEndian) {
  Writer w;
  w.u8(0x01);
  w.u16(0x0302);
  w.u24(0x060504);
  w.u32(0x0a090807);
  w.u64(0x1211100f0e0d0c0bull);
  const Bytes expected = bytes({1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18});
  EXPECT_EQ(w.data(), expected);

  Reader r(expected);
  EXPECT_EQ(r.u8(), 0x01);
  EXPECT_EQ(r.u16(), 0x0302);
  EXPECT_EQ(r.u24(), 0x060504u);
  EXPECT_EQ(r.u32(), 0x0a090807u);
  EXPECT_EQ(r.u64(), 0x1211100f0e0d0c0bull);
  EXPECT_EQ(r.remaining(), 0u);
}

TEST(ProtocolCodec, ALengthEncodedIntegerTakesTheShortestFormTheManualGives) {
  EXPECT_EQ(lenenc(0), bytes({0x00}));
  EXPECT_EQ(lenenc(250), bytes({0xfa}));
  EXPECT_EQ(lenenc(251), bytes({0xfc, 0xfb, 0x00}));
  EXPECT_EQ(lenenc(65535), bytes({0xfc, 0xff, 0xff}));
  EXPECT_EQ(lenenc(65536), bytes({0xfd, 0x00, 0x00, 0x01}));
  EXPECT_EQ(lenenc(16777215), bytes({0xfd, 0xff, 0xff, 0xff}));
  EXPECT_EQ(lenenc(16777216), bytes({0xfe, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00}));
}

TEST(ProtocolCodec, ALengthEncodedIntegerReadsBackInEveryForm) {
  for (uint64_t value : {0ull, 250ull, 251ull, 65535ull, 65536ull, 16777215ull, 16777216ull, 0xffffffffffffffffull}) {
    const Bytes encoded = lenenc(value);
    Reader r(encoded);
    EXPECT_EQ(r.lenenc(), value);
    EXPECT_EQ(r.remaining(), 0u);
  }
}

TEST(ProtocolCodec, NullAndErrorMarkersAreNotIntegers) {
  for (int first : {0xfb, 0xff}) {
    const Bytes encoded = bytes({first, 0, 0, 0, 0, 0, 0, 0, 0});
    Reader r(encoded);
    EXPECT_THROW(r.lenenc(), ProtocolError);
  }
}

TEST(ProtocolCodec, ATruncatedValueIsRefusedAsMalformed) {
  const Bytes two = bytes({0x01, 0x02});
  {
    Reader r(two);
    EXPECT_THROW(r.u32(), ProtocolError);
  }
  const Bytes short_lenenc = bytes({0xfc, 0x01});
  {
    Reader r(short_lenenc);
    EXPECT_THROW(r.lenenc(), ProtocolError);
  }
  const Bytes short_string = bytes({0x05, 'a', 'b'});
  {
    Reader r(short_string);
    try {
      r.lenenc_string(100);
      FAIL() << "a string longer than its packet was read";
    } catch (const ProtocolError& e) {
      EXPECT_EQ(e.code(), er_malformed_packet);
    }
  }
}

TEST(ProtocolCodec, StringsReadInTheirThreeForms) {
  Writer w;
  w.lenenc_string("abc");
  w.nul_string("root");
  w.raw(std::string_view("to the end"));
  const Bytes encoded = w.take();
  EXPECT_EQ(encoded, bytes({3, 'a', 'b', 'c', 'r', 'o', 'o', 't', 0, 't', 'o', ' ', 't', 'h', 'e', ' ', 'e', 'n', 'd'}));

  Reader r(encoded);
  EXPECT_EQ(r.lenenc_string(16), "abc");
  EXPECT_EQ(r.nul_string(16), "root");
  EXPECT_EQ(r.rest_string(16), "to the end");
  EXPECT_EQ(r.remaining(), 0u);
}

TEST(ProtocolCodec, AStringMayHoldAnyByte) {
  const std::string binary("\x00\xff\x7f", 3);
  Writer w;
  w.lenenc_string(binary);
  const Bytes encoded = w.take();
  Reader r(encoded);
  EXPECT_EQ(r.lenenc_string(16), binary);
}

TEST(ProtocolCodec, AStringOverItsMaximumIsRefusedBeforeItIsCopied) {
  Writer w;
  w.lenenc_string("abcdef");
  w.nul_string("abcdef");
  w.raw(std::string_view("abcdef"));
  const Bytes encoded = w.take();
  {
    Reader r(encoded);
    EXPECT_THROW(r.lenenc_string(5), ProtocolError);
  }
  {
    Reader r(encoded);
    r.lenenc_string(6);
    EXPECT_THROW(r.nul_string(5), ProtocolError);
  }
  {
    Reader r(encoded);
    r.lenenc_string(6);
    r.nul_string(6);
    EXPECT_THROW(r.rest_string(5), ProtocolError);
  }
  // A length that claims more than any packet could hold allocates nothing.
  const Bytes huge = bytes({0xfe, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f});
  Reader r(huge);
  EXPECT_THROW(r.lenenc_string(1u << 30), ProtocolError);
}

TEST(ProtocolCodec, ANulTerminatedStringWithoutItsNulIsRefused) {
  const Bytes encoded = bytes({'r', 'o', 'o', 't'});
  Reader r(encoded);
  EXPECT_THROW(r.nul_string(16), ProtocolError);
}

TEST(ProtocolCodec, ANulCannotBeWrittenInsideANulTerminatedString) {
  Writer w;
  EXPECT_THROW(w.nul_string(std::string("a\0b", 3)), std::invalid_argument);
}

}  // namespace
}  // namespace kvasir::protocol
