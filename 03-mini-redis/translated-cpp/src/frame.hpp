#pragma once

/// RESP (Redis Serialization Protocol) frame types and parsing.
/// Translates: frame.rs

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace mini_redis {

/// A frame in the Redis protocol.
/// Rust enum with data → std::variant
struct Frame {
    struct Simple { std::string value; };
    struct Error  { std::string value; };
    struct Integer { uint64_t value; };
    struct Bulk   { std::string data; };  // Bytes → std::string (byte buffer)
    struct Null   {};
    struct Array  { std::vector<Frame> elements; };

    using Value = std::variant<Simple, Error, Integer, Bulk, Null, Array>;
    Value value;

    // Constructors
    static Frame simple(std::string s)   { return {Simple{std::move(s)}}; }
    static Frame error(std::string s)    { return {Error{std::move(s)}}; }
    static Frame integer(uint64_t v)     { return {Integer{v}}; }
    static Frame bulk(std::string d)     { return {Bulk{std::move(d)}}; }
    static Frame null()                  { return {Null{}}; }
    static Frame array()                 { return {Array{{}}}; }
    static Frame array(std::vector<Frame> elems) { return {Array{std::move(elems)}}; }

    // Array helpers — panics if not an array (matches Rust behavior)
    void push_bulk(std::string bytes);
    void push_int(uint64_t v);

    // Type queries
    bool is_simple() const  { return std::holds_alternative<Simple>(value); }
    bool is_error() const   { return std::holds_alternative<Error>(value); }
    bool is_integer() const { return std::holds_alternative<Integer>(value); }
    bool is_bulk() const    { return std::holds_alternative<Bulk>(value); }
    bool is_null() const    { return std::holds_alternative<Null>(value); }
    bool is_array() const   { return std::holds_alternative<Array>(value); }

    // Accessors
    const std::string& as_simple() const  { return std::get<Simple>(value).value; }
    const std::string& as_error() const   { return std::get<Error>(value).value; }
    uint64_t as_integer() const           { return std::get<Integer>(value).value; }
    const std::string& as_bulk() const    { return std::get<Bulk>(value).data; }
    const std::vector<Frame>& as_array() const { return std::get<Array>(value).elements; }
    std::vector<Frame>& as_array_mut()    { return std::get<Array>(value).elements; }

    /// Comparison with string (matches Rust's PartialEq<&str>)
    bool operator==(std::string_view s) const;

    /// Display formatting
    std::string to_string() const;

    /// Create an "unexpected frame" error message
    std::string to_error_msg() const;
};

/// Frame parsing error
struct FrameError : std::runtime_error {
    enum Kind { Incomplete, Other };
    Kind kind;
    FrameError(Kind k, std::string msg = "")
        : std::runtime_error(k == Incomplete ? "stream ended early" : msg), kind(k) {}
    bool is_incomplete() const { return kind == Incomplete; }
};

/// Check if a complete frame can be parsed from the buffer.
/// Advances cursor position past the frame if complete.
/// Throws FrameError::Incomplete if more data needed.
void frame_check(const uint8_t* data, size_t len, size_t& pos);

/// Parse a frame from the buffer, starting at pos.
/// The frame must have been validated with frame_check first.
Frame frame_parse(const uint8_t* data, size_t len, size_t& pos);

} // namespace mini_redis
