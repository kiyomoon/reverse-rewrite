#pragma once

/// Async RESP connection — reads and writes Frame values over TCP.
/// Translates: connection.rs
///
/// Rust's BufWriter<TcpStream> + BytesMut → ASIO socket with manual buffers.

#include "frame.hpp"

#include <asio.hpp>
#include <asio/awaitable.hpp>
#include <optional>
#include <string>
#include <vector>

namespace mini_redis {

class Connection {
public:
    explicit Connection(asio::ip::tcp::socket socket);

    // Move-only
    Connection(Connection&&) = default;
    Connection& operator=(Connection&&) = default;
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    /// Read a single Frame from the stream.
    /// Returns std::nullopt if the peer closed cleanly.
    asio::awaitable<std::optional<Frame>> read_frame();

    /// Write a single Frame to the stream.
    asio::awaitable<void> write_frame(const Frame& frame);

    /// Try to parse a complete frame from already-buffered data (synchronous).
    /// Returns std::nullopt if buffer doesn't contain a complete frame.
    std::optional<Frame> try_parse_buffered();

    /// Read whatever data is immediately available from the socket (non-blocking).
    /// Returns false if peer closed (EOF), true otherwise.
    bool read_available();

    /// Access the underlying socket (for async_wait / cancel).
    asio::ip::tcp::socket& socket() { return socket_; }

private:
    /// Encode a single frame value into write_buf_ (synchronous).
    void write_value_sync(const Frame& frame);

    asio::ip::tcp::socket socket_;

    // Read buffer (replaces BytesMut)
    std::vector<uint8_t> read_buf_;
    size_t read_len_ = 0;  // valid bytes in read_buf_

    // Write buffer (replaces BufWriter)
    std::vector<uint8_t> write_buf_;
};

} // namespace mini_redis
