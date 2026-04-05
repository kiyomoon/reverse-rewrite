/// Command parser implementation.
/// Translates: parse.rs

#include "parse.hpp"
#include <charconv>

namespace mini_redis {

Parse::Parse(Frame frame) {
    if (!frame.is_array())
        throw ParseError(ParseError::Other,
            "protocol error; expected array, got frame");
    parts_ = std::move(frame.as_array_mut());
}

Frame Parse::next() {
    if (pos_ >= parts_.size())
        throw ParseError(ParseError::EndOfStream);
    return std::move(parts_[pos_++]);
}

std::string Parse::next_string() {
    Frame f = next();
    if (f.is_simple()) return f.as_simple();
    if (f.is_bulk())   return f.as_bulk();
    throw ParseError(ParseError::Other,
        "protocol error; expected simple frame or bulk frame");
}

std::string Parse::next_bytes() {
    Frame f = next();
    if (f.is_simple()) return f.as_simple();
    if (f.is_bulk())   return f.as_bulk();
    throw ParseError(ParseError::Other,
        "protocol error; expected simple frame or bulk frame");
}

uint64_t Parse::next_int() {
    Frame f = next();
    if (f.is_integer()) return f.as_integer();
    // Try parsing from string representation
    std::string s;
    if (f.is_simple()) s = f.as_simple();
    else if (f.is_bulk()) s = f.as_bulk();
    else throw ParseError(ParseError::Other,
        "protocol error; expected int frame");

    uint64_t result = 0;
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), result);
    if (ec != std::errc{})
        throw ParseError(ParseError::Other, "protocol error; invalid number");
    return result;
}

void Parse::finish() {
    if (pos_ < parts_.size())
        throw ParseError(ParseError::Other,
            "protocol error; expected end of frame, but there was more");
}

} // namespace mini_redis
