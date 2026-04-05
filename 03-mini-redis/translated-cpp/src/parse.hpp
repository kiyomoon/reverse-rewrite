#pragma once

/// Command parser — extracts typed fields from a Frame::Array.
/// Translates: parse.rs

#include "frame.hpp"
#include <stdexcept>
#include <string>
#include <vector>

namespace mini_redis {

/// Error encountered while parsing a frame into a command.
struct ParseError : std::runtime_error {
    enum Kind { EndOfStream, Other };
    Kind kind;
    ParseError(Kind k, std::string msg = "")
        : std::runtime_error(k == EndOfStream
            ? "protocol error; unexpected end of stream" : msg),
          kind(k) {}
    bool is_end_of_stream() const { return kind == EndOfStream; }
};

/// Cursor-like parser over a Frame::Array.
/// Translates Rust's Parse struct.
class Parse {
public:
    /// Create a Parse from a Frame. The frame must be an Array.
    explicit Parse(Frame frame);

    /// Return the next entry as a string.
    std::string next_string();

    /// Return the next entry as raw bytes (string).
    std::string next_bytes();

    /// Return the next entry as an integer.
    uint64_t next_int();

    /// Ensure there are no more entries.
    void finish();

private:
    Frame next();
    std::vector<Frame> parts_;
    size_t pos_ = 0;
};

} // namespace mini_redis
