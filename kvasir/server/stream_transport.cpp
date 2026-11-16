//
// Kvasir - The Distributed Relational Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "kvasir/server/stream_transport.h"

#include <boost/asio/buffer.hpp>

namespace kvasir::server {

StreamTransport::StreamTransport(std::shared_ptr<dannan::listen::Stream> stream) : _stream(std::move(stream)) {}

void StreamTransport::read_some(std::span<uint8_t> buffer, std::function<void(std::optional<size_t>)> done) {
  _stream->async_read_some(boost::asio::buffer(buffer.data(), buffer.size()), [done = std::move(done)](boost::system::error_code error, size_t bytes) {
    if (error || bytes == 0) {
      done(std::nullopt);
    } else {
      done(bytes);
    }
  });
}

void StreamTransport::send(std::string bytes) { _stream->send(std::move(bytes)); }

void StreamTransport::start_tls(std::function<void(std::string)> done) { _stream->start_tls_unanswered(std::move(done)); }

void StreamTransport::close() { _stream->close(); }

void StreamTransport::abort() { _stream->abort(); }

std::string StreamTransport::peer_host() const { return _stream->peer().address().to_string(); }

}  // namespace kvasir::server
