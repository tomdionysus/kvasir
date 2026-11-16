//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <memory>

#include "dannan/listen/stream.h"
#include "kvasir/server/connection.h"

// A connection's transport in the node: Dannan's accepted Stream, on a
// listener started with certificates and start_tls, so TLS begins when the
// client's SSLRequest has been read. MySQL's upgrade sends no answer, so it is
// Stream::start_tls_unanswered (Dannan 0.8.4).
//
// Thread safety: as the Stream's: read_some and start_tls on the stream's
// executor (the connection's), send, close and abort from anywhere.
namespace kvasir::server {

class StreamTransport : public Transport {
 public:
  explicit StreamTransport(std::shared_ptr<dannan::listen::Stream> stream);

  void read_some(std::span<uint8_t> buffer, std::function<void(std::optional<size_t> bytes)> done) override;
  void send(std::string bytes) override;
  void start_tls(std::function<void(std::string)> done) override;
  void close() override;
  void abort() override;
  std::string peer_host() const override;
  // The node's listener always has certificates: TLS is required (the design).
  bool tls_available() const override { return true; }

 private:
  std::shared_ptr<dannan::listen::Stream> _stream;
};

}  // namespace kvasir::server
