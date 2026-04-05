/// Async RESP connection implementation.
/// Translates: connection.rs

#include "connection.hpp"
#include <cstring>
#include <stdexcept>

namespace mini_redis {

Connection::Connection(asio::ip::tcp::socket socket)
    : socket_(std::move(socket))
    , read_buf_(4096)  // 4KB initial buffer, same as Rust
    , read_len_(0)
{
    write_buf_.reserve(4096);
}

asio::awaitable<std::optional<Frame>> Connection::read_frame() {
    for (;;) {
        // Try to parse a frame from buffered data
        if (read_len_ > 0) {
            size_t pos = 0;
            try {
                frame_check(read_buf_.data(), read_len_, pos);
                // Success — we have a complete frame. Parse it.
                size_t parse_pos = 0;
                Frame frame = frame_parse(read_buf_.data(), read_len_, parse_pos);

                // Advance the buffer (remove consumed bytes)
                size_t consumed = pos;
                if (consumed < read_len_) {
                    std::memmove(read_buf_.data(),
                                 read_buf_.data() + consumed,
                                 read_len_ - consumed);
                }
                read_len_ -= consumed;

                co_return frame;
            } catch (const FrameError& e) {
                if (!e.is_incomplete())
                    throw;
                // Incomplete — need more data, fall through to read
            }
        }

        // Ensure buffer has room
        if (read_len_ == read_buf_.size())
            read_buf_.resize(read_buf_.size() * 2);

        // Read more data from socket
        size_t n = co_await socket_.async_read_some(
            asio::buffer(read_buf_.data() + read_len_,
                         read_buf_.size() - read_len_),
            asio::use_awaitable);

        if (n == 0) {
            // Peer closed
            if (read_len_ == 0) {
                co_return std::nullopt;  // Clean close
            } else {
                throw std::runtime_error("connection reset by peer");
            }
        }
        read_len_ += n;
    }
}

asio::awaitable<void> Connection::write_frame(const Frame& frame) {
    write_buf_.clear();

    if (frame.is_array()) {
        const auto& arr = frame.as_array();
        // Array prefix: *<len>\r\n
        write_buf_.push_back('*');
        auto len_str = std::to_string(arr.size());
        write_buf_.insert(write_buf_.end(), len_str.begin(), len_str.end());
        write_buf_.push_back('\r');
        write_buf_.push_back('\n');
        // Encode each element
        for (const auto& entry : arr) {
            write_value_sync(entry);
        }
    } else {
        write_value_sync(frame);
    }

    // Flush the entire buffer to the socket
    co_await asio::async_write(socket_,
        asio::buffer(write_buf_.data(), write_buf_.size()),
        asio::use_awaitable);
}

// Synchronous write into buffer (we flush at the end of write_frame)
void Connection::write_value_sync(const Frame& frame) {
    if (frame.is_simple()) {
        write_buf_.push_back('+');
        const auto& s = frame.as_simple();
        write_buf_.insert(write_buf_.end(), s.begin(), s.end());
        write_buf_.push_back('\r');
        write_buf_.push_back('\n');
    } else if (frame.is_error()) {
        write_buf_.push_back('-');
        const auto& s = frame.as_error();
        write_buf_.insert(write_buf_.end(), s.begin(), s.end());
        write_buf_.push_back('\r');
        write_buf_.push_back('\n');
    } else if (frame.is_integer()) {
        write_buf_.push_back(':');
        auto s = std::to_string(frame.as_integer());
        write_buf_.insert(write_buf_.end(), s.begin(), s.end());
        write_buf_.push_back('\r');
        write_buf_.push_back('\n');
    } else if (frame.is_null()) {
        const char* null_str = "$-1\r\n";
        write_buf_.insert(write_buf_.end(), null_str, null_str + 5);
    } else if (frame.is_bulk()) {
        const auto& data = frame.as_bulk();
        write_buf_.push_back('$');
        auto len_str = std::to_string(data.size());
        write_buf_.insert(write_buf_.end(), len_str.begin(), len_str.end());
        write_buf_.push_back('\r');
        write_buf_.push_back('\n');
        write_buf_.insert(write_buf_.end(), data.begin(), data.end());
        write_buf_.push_back('\r');
        write_buf_.push_back('\n');
    }
    // Array within a value: unreachable (same as Rust)
}

std::optional<Frame> Connection::try_parse_buffered() {
    if (read_len_ == 0) return std::nullopt;
    size_t pos = 0;
    try {
        frame_check(read_buf_.data(), read_len_, pos);
        // Complete frame in buffer — parse it
        size_t parse_pos = 0;
        Frame frame = frame_parse(read_buf_.data(), read_len_, parse_pos);
        // Advance the buffer
        size_t consumed = pos;
        if (consumed < read_len_) {
            std::memmove(read_buf_.data(),
                         read_buf_.data() + consumed,
                         read_len_ - consumed);
        }
        read_len_ -= consumed;
        return frame;
    } catch (const FrameError& e) {
        if (!e.is_incomplete()) throw;
        return std::nullopt;  // Incomplete frame — need more data
    }
}

bool Connection::read_available() {
    // Ensure buffer has room
    if (read_len_ == read_buf_.size())
        read_buf_.resize(read_buf_.size() * 2);

    // Temporarily set socket to user-non-blocking mode.
    // ASIO always keeps the underlying fd non-blocking for reactor use;
    // this flag only affects synchronous read_some() behavior.
    socket_.non_blocking(true);
    asio::error_code ec;
    size_t n = socket_.read_some(
        asio::buffer(read_buf_.data() + read_len_,
                     read_buf_.size() - read_len_),
        ec);
    socket_.non_blocking(false);

    if (ec == asio::error::would_block || ec == asio::error::try_again)
        return true;   // No data available right now, still connected
    if (ec == asio::error::eof || (!ec && n == 0))
        return false;  // Peer closed
    if (ec)
        throw asio::system_error(ec);

    read_len_ += n;
    return true;
}

} // namespace mini_redis
