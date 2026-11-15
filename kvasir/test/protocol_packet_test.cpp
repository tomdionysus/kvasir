//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// MySQL 8.4 manual, "MySQL Packets": a packet is int<3> payload length,
// int<1> sequence id, then the payload. A payload of 0xffffff bytes or more
// is split: fragments of 0xffffff bytes, each with the next sequence id,
// until one shorter, which is empty when the payload is an exact multiple.
// The manual's example COM_QUIT is 01 00 00 00 01. A payload over
// max_allowed_packet is error 1153; a sequence id out of order is error 1156.

#include <gtest/gtest.h>

#include <cstdint>
#include <span>
#include <vector>

#include "kvasir/protocol/packet.h"

namespace kvasir::protocol {
namespace {

Bytes bytes(std::initializer_list<int> values) {
  Bytes out;
  for (int v : values) out.push_back(static_cast<uint8_t>(v));
  return out;
}

Bytes filled(size_t size) {
  Bytes out(size);
  for (size_t i = 0; i < size; ++i) out[i] = static_cast<uint8_t>(i * 31 + 7);
  return out;
}

TEST(ProtocolPacket, TheManualsQuitPacketIsOnePayloadOfOneByte) {
  PacketReader reader;
  reader.feed(bytes({0x01, 0x00, 0x00, 0x00, 0x01}));
  const auto packet = reader.next();
  ASSERT_TRUE(packet);
  EXPECT_EQ(packet->first_sequence, 0);
  EXPECT_EQ(packet->last_sequence, 0);
  EXPECT_EQ(packet->payload, bytes({0x01}));
  EXPECT_FALSE(reader.next());
  EXPECT_EQ(reader.buffered(), 0u);
}

TEST(ProtocolPacket, NothingIsReturnedUntilAPayloadIsWhole) {
  const Bytes wire = bytes({0x03, 0x00, 0x00, 0x07, 'a', 'b', 'c'});
  PacketReader reader;
  for (size_t i = 0; i + 1 < wire.size(); ++i) {
    reader.feed(std::span<const uint8_t>(&wire[i], 1));
    EXPECT_FALSE(reader.next()) << "after byte " << i;
  }
  reader.feed(std::span<const uint8_t>(&wire.back(), 1));
  const auto packet = reader.next();
  ASSERT_TRUE(packet);
  EXPECT_EQ(packet->first_sequence, 7);
  EXPECT_EQ(packet->payload, bytes({'a', 'b', 'c'}));
}

TEST(ProtocolPacket, SeveralPacketsInOneReadComeOutInOrder) {
  Bytes wire = bytes({0x01, 0x00, 0x00, 0x00, 0x0e});
  const Bytes second = bytes({0x00, 0x00, 0x00, 0x01});
  const Bytes third = bytes({0x02, 0x00, 0x00, 0x02, 'h', 'i'});
  wire.insert(wire.end(), second.begin(), second.end());
  wire.insert(wire.end(), third.begin(), third.end());
  PacketReader reader;
  reader.feed(wire);
  auto packet = reader.next();
  ASSERT_TRUE(packet);
  EXPECT_EQ(packet->payload, bytes({0x0e}));
  packet = reader.next();
  ASSERT_TRUE(packet);
  EXPECT_TRUE(packet->payload.empty());
  EXPECT_EQ(packet->first_sequence, 1);
  packet = reader.next();
  ASSERT_TRUE(packet);
  EXPECT_EQ(packet->payload, bytes({'h', 'i'}));
  EXPECT_FALSE(reader.next());
}

TEST(ProtocolPacket, APayloadOfExactlyTheFragmentSizeEndsWithAnEmptyFragment) {
  const Bytes payload = filled(max_fragment);
  uint8_t sequence = 3;
  Bytes wire;
  frame(payload, sequence, wire);
  EXPECT_EQ(sequence, 5);
  ASSERT_EQ(wire.size(), max_fragment + 8);
  EXPECT_EQ(Bytes(wire.begin(), wire.begin() + 4), bytes({0xff, 0xff, 0xff, 0x03}));
  EXPECT_EQ(Bytes(wire.end() - 4, wire.end()), bytes({0x00, 0x00, 0x00, 0x04}));

  PacketReader reader;
  reader.feed(wire);
  const auto packet = reader.next();
  ASSERT_TRUE(packet);
  EXPECT_EQ(packet->first_sequence, 3);
  EXPECT_EQ(packet->last_sequence, 4);
  EXPECT_TRUE(packet->payload == payload);
}

TEST(ProtocolPacket, ALargerPayloadIsSplitAndJoined) {
  const Bytes payload = filled(max_fragment + 5);
  uint8_t sequence = 0;
  Bytes wire;
  frame(payload, sequence, wire);
  EXPECT_EQ(sequence, 2);
  ASSERT_EQ(wire.size(), max_fragment + 5 + 8);
  EXPECT_EQ(Bytes(wire.begin() + 4 + static_cast<std::ptrdiff_t>(max_fragment), wire.begin() + 8 + static_cast<std::ptrdiff_t>(max_fragment)),
            bytes({0x05, 0x00, 0x00, 0x01}));

  PacketReader reader;
  // The join holds when the fragments arrive in pieces.
  const size_t piece = 1000003;
  for (size_t at = 0; at < wire.size(); at += piece) {
    reader.feed(std::span<const uint8_t>(wire.data() + at, std::min(piece, wire.size() - at)));
    if (at + piece < wire.size()) {
      EXPECT_FALSE(reader.next());
    }
  }
  const auto packet = reader.next();
  ASSERT_TRUE(packet);
  EXPECT_EQ(packet->last_sequence, 1);
  EXPECT_TRUE(packet->payload == payload);
}

TEST(ProtocolPacket, AnEmptyPayloadIsOneEmptyPacket) {
  uint8_t sequence = 9;
  Bytes wire;
  frame({}, sequence, wire);
  EXPECT_EQ(wire, bytes({0x00, 0x00, 0x00, 0x09}));
  EXPECT_EQ(sequence, 10);
}

TEST(ProtocolPacket, TheSequenceIdWrapsAfter255) {
  uint8_t sequence = 255;
  Bytes wire;
  frame(bytes({'x'}), sequence, wire);
  EXPECT_EQ(sequence, 0);
  EXPECT_EQ(wire, bytes({0x01, 0x00, 0x00, 0xff, 'x'}));
}

TEST(ProtocolPacket, APayloadOverTheLimitIsRefusedOnItsHeader) {
  PacketLimits limits;
  limits.max_payload = 1024;
  PacketReader reader(limits);
  // The header alone claims 1025 bytes; none of them has arrived.
  reader.feed(bytes({0x01, 0x04, 0x00, 0x00}));
  try {
    reader.next();
    FAIL() << "a payload over the limit was accepted";
  } catch (const ProtocolError& e) {
    EXPECT_EQ(e.code(), er_net_packet_too_large);
  }
  // Spent: the same refusal again, and nothing more is kept.
  reader.feed(bytes({0x01, 0x00, 0x00, 0x00, 0x01}));
  EXPECT_THROW(reader.next(), ProtocolError);
  EXPECT_EQ(reader.buffered(), 0u);
}

TEST(ProtocolPacket, APayloadAtTheLimitIsAccepted) {
  PacketLimits limits;
  limits.max_payload = 4;
  PacketReader reader(limits);
  reader.feed(bytes({0x04, 0x00, 0x00, 0x00, 1, 2, 3, 4}));
  const auto packet = reader.next();
  ASSERT_TRUE(packet);
  EXPECT_EQ(packet->payload.size(), 4u);
}

TEST(ProtocolPacket, TheRestrictedLimitAppliesWhileRestricted) {
  PacketLimits limits;
  limits.max_payload = 1024;
  limits.restricted_payload = 8;
  PacketReader reader(limits);
  reader.restrict(true);
  reader.feed(bytes({0x09, 0x00, 0x00, 0x00}));
  EXPECT_THROW(reader.next(), ProtocolError);

  PacketReader open(limits);
  open.feed(bytes({0x09, 0x00, 0x00, 0x00, 1, 2, 3, 4, 5, 6, 7, 8, 9}));
  EXPECT_TRUE(open.next());
}

TEST(ProtocolPacket, FragmentsOverTheLimitTogetherAreRefused) {
  PacketLimits limits;
  limits.max_payload = max_fragment + 4;
  PacketReader reader(limits);
  Bytes wire = bytes({0xff, 0xff, 0xff, 0x00});
  wire.resize(wire.size() + max_fragment, 0x61);
  const Bytes second = bytes({0x05, 0x00, 0x00, 0x01});
  wire.insert(wire.end(), second.begin(), second.end());
  reader.feed(wire);
  try {
    reader.next();
    FAIL() << "fragments over the limit were accepted";
  } catch (const ProtocolError& e) {
    EXPECT_EQ(e.code(), er_net_packet_too_large);
  }
}

TEST(ProtocolPacket, AFragmentOutOfSequenceIsRefused) {
  PacketReader reader;
  Bytes wire = bytes({0xff, 0xff, 0xff, 0x00});
  wire.resize(wire.size() + max_fragment, 0x61);
  // The second fragment should carry sequence id 1.
  const Bytes second = bytes({0x01, 0x00, 0x00, 0x02, 'x'});
  wire.insert(wire.end(), second.begin(), second.end());
  reader.feed(wire);
  try {
    reader.next();
    FAIL() << "a fragment out of sequence was accepted";
  } catch (const ProtocolError& e) {
    EXPECT_EQ(e.code(), er_net_packets_out_of_order);
  }
}

}  // namespace
}  // namespace kvasir::protocol
