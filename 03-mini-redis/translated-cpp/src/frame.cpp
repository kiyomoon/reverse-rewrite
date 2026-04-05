/// RESP frame implementation.
/// Translates: frame.rs

#include "frame.hpp"
#include <charconv>
#include <cstring>
#include <sstream>
#include <stdexcept>

namespace mini_redis {

// ---------- Frame helpers ----------

void Frame::push_bulk(std::string bytes) {
    auto& arr = std::get<Array>(value);  // throws if not Array
    arr.elements.push_back(Frame::bulk(std::move(bytes)));
}

void Frame::push_int(uint64_t v) {
    auto& arr = std::get<Array>(value);
    arr.elements.push_back(Frame::integer(v));
}

bool Frame::operator==(std::string_view s) const {
    if (auto* p = std::get_if<Simple>(&value))
        return p->value == s;
    if (auto* p = std::get_if<Bulk>(&value))
        return std::string_view(p->data) == s;
    return false;
}

std::string Frame::to_string() const {
    return std::visit([](auto&& v) -> std::string {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, Simple>)  return v.value;
        if constexpr (std::is_same_v<T, Error>)   return "error: " + v.value;
        if constexpr (std::is_same_v<T, Integer>)  return std::to_string(v.value);
        if constexpr (std::is_same_v<T, Bulk>)     return v.data;  // simplified
        if constexpr (std::is_same_v<T, Null>)     return "(nil)";
        if constexpr (std::is_same_v<T, Array>) {
            std::string result;
            for (size_t i = 0; i < v.elements.size(); ++i) {
                if (i > 0) result += ' ';
                result += v.elements[i].to_string();
            }
            return result;
        }
        return {};
    }, value);
}

std::string Frame::to_error_msg() const {
    return "unexpected frame: " + to_string();
}

// ---------- Low-level parsing helpers ----------

static uint8_t peek_u8(const uint8_t* data, size_t len, size_t pos) {
    if (pos >= len) throw FrameError(FrameError::Incomplete);
    return data[pos];
}

static uint8_t get_u8(const uint8_t* data, size_t len, size_t& pos) {
    if (pos >= len) throw FrameError(FrameError::Incomplete);
    return data[pos++];
}

static void skip(const uint8_t* /*data*/, size_t len, size_t& pos, size_t n) {
    if (pos + n > len) throw FrameError(FrameError::Incomplete);
    pos += n;
}

/// Find \r\n-terminated line starting at pos. Returns pointer to start, sets pos past \n.
static std::pair<const uint8_t*, size_t> get_line(const uint8_t* data, size_t len, size_t& pos) {
    size_t start = pos;
    if (len < 2) throw FrameError(FrameError::Incomplete);
    size_t end = len - 1;
    for (size_t i = start; i < end; ++i) {
        if (data[i] == '\r' && data[i + 1] == '\n') {
            pos = i + 2;
            return {data + start, i - start};
        }
    }
    throw FrameError(FrameError::Incomplete);
}

/// Parse a \r\n-terminated decimal number
static uint64_t get_decimal(const uint8_t* data, size_t len, size_t& pos) {
    auto [line, line_len] = get_line(data, len, pos);
    uint64_t result = 0;
    auto [ptr, ec] = std::from_chars(
        reinterpret_cast<const char*>(line),
        reinterpret_cast<const char*>(line + line_len),
        result
    );
    if (ec != std::errc{})
        throw FrameError(FrameError::Other, "protocol error; invalid frame format");
    return result;
}

// ---------- frame_check ----------

void frame_check(const uint8_t* data, size_t len, size_t& pos) {
    uint8_t tag = get_u8(data, len, pos);
    switch (tag) {
    case '+':  // Simple
    case '-':  // Error
        get_line(data, len, pos);
        break;
    case ':':  // Integer
        get_decimal(data, len, pos);
        break;
    case '$': { // Bulk
        if (peek_u8(data, len, pos) == '-') {
            skip(data, len, pos, 4);  // skip "-1\r\n"
        } else {
            uint64_t n = get_decimal(data, len, pos);
            skip(data, len, pos, static_cast<size_t>(n) + 2);  // data + \r\n
        }
        break;
    }
    case '*': { // Array
        uint64_t n = get_decimal(data, len, pos);
        for (uint64_t i = 0; i < n; ++i)
            frame_check(data, len, pos);
        break;
    }
    default:
        throw FrameError(FrameError::Other,
            "protocol error; invalid frame type byte `" + std::to_string(tag) + "`");
    }
}

// ---------- frame_parse ----------

Frame frame_parse(const uint8_t* data, size_t len, size_t& pos) {
    uint8_t tag = get_u8(data, len, pos);
    switch (tag) {
    case '+': { // Simple
        auto [line, line_len] = get_line(data, len, pos);
        return Frame::simple(std::string(reinterpret_cast<const char*>(line), line_len));
    }
    case '-': { // Error
        auto [line, line_len] = get_line(data, len, pos);
        return Frame::error(std::string(reinterpret_cast<const char*>(line), line_len));
    }
    case ':': { // Integer
        return Frame::integer(get_decimal(data, len, pos));
    }
    case '$': { // Bulk or Null
        if (peek_u8(data, len, pos) == '-') {
            auto [line, line_len] = get_line(data, len, pos);
            if (line_len != 2 || line[0] != '-' || line[1] != '1')
                throw FrameError(FrameError::Other, "protocol error; invalid frame format");
            return Frame::null();
        }
        uint64_t n = get_decimal(data, len, pos);
        size_t sn = static_cast<size_t>(n);
        if (pos + sn + 2 > len)
            throw FrameError(FrameError::Incomplete);
        std::string bulk_data(reinterpret_cast<const char*>(data + pos), sn);
        pos += sn + 2;  // skip data + \r\n
        return Frame::bulk(std::move(bulk_data));
    }
    case '*': { // Array
        uint64_t n = get_decimal(data, len, pos);
        std::vector<Frame> elements;
        elements.reserve(static_cast<size_t>(n));
        for (uint64_t i = 0; i < n; ++i)
            elements.push_back(frame_parse(data, len, pos));
        return Frame::array(std::move(elements));
    }
    default:
        throw FrameError(FrameError::Other, "protocol error; unimplemented frame type");
    }
}

} // namespace mini_redis
