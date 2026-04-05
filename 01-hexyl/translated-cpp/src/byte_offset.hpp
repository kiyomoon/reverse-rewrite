#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <variant>

namespace hexyl {

inline constexpr int64_t DEFAULT_BLOCK_SIZE = 512;

// --- Wrapper types ---

struct NonNegativeI64 {
    int64_t value;

    static std::optional<NonNegativeI64> create(int64_t x) {
        if (x < 0) return std::nullopt;
        return NonNegativeI64{x};
    }

    explicit operator uint64_t() const {
        return static_cast<uint64_t>(value);
    }
};

struct PositiveI64 {
    int64_t value;

    static std::optional<PositiveI64> create(int64_t x) {
        if (x < 1) return std::nullopt;
        return PositiveI64{x};
    }

    explicit operator uint64_t() const {
        return static_cast<uint64_t>(value);
    }
};

// --- Unit ---

enum class UnitKind {
    Byte,
    Kilobyte,
    Megabyte,
    Gigabyte,
    Terabyte,
    Kibibyte,
    Mebibyte,
    Gibibyte,
    Tebibyte,
    Block,
};

struct Unit {
    UnitKind kind;
    std::optional<int64_t> custom_block_size; // only for Block

    int64_t get_multiplier() const;
};

// --- ByteOffset ---

enum class ByteOffsetKind {
    ForwardFromBeginning,
    ForwardFromLastOffset,
    BackwardFromEnd,
};

struct ByteOffset {
    NonNegativeI64 value;
    ByteOffsetKind kind;

    std::expected<NonNegativeI64, std::string> assume_forward_offset_from_start() const;
};

// --- Error type ---

enum class ByteOffsetParseError {
    Empty,
    EmptyAfterSign,
    SignFoundAfterHexPrefix,
    InvalidNumAndUnit,
    EmptyWithUnit,
    InvalidUnit,
    ParseNum,
    UnitMultiplicationOverflow,
};

struct ParseError {
    ByteOffsetParseError kind;
    std::string detail;

    std::string message() const;
};

// --- Parsing functions ---

std::expected<std::pair<std::string_view, ByteOffsetKind>, ParseError>
process_sign_of(std::string_view n);

std::optional<std::expected<int64_t, ParseError>>
try_parse_as_hex_number(std::string_view n);

std::expected<std::pair<int64_t, Unit>, ParseError>
extract_num_and_unit_from(std::string_view n);

std::expected<ByteOffset, ParseError>
parse_byte_offset(std::string_view n, PositiveI64 block_size);

} // namespace hexyl
