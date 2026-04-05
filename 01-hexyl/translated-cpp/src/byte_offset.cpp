#include "byte_offset.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>

namespace hexyl {

// --- Unit ---

int64_t Unit::get_multiplier() const {
    switch (kind) {
        case UnitKind::Byte:     return 1;
        case UnitKind::Kilobyte: return 1000;
        case UnitKind::Megabyte: return 1'000'000;
        case UnitKind::Gigabyte: return 1'000'000'000;
        case UnitKind::Terabyte: return 1'000'000'000'000LL;
        case UnitKind::Kibibyte: return 1LL << 10;
        case UnitKind::Mebibyte: return 1LL << 20;
        case UnitKind::Gibibyte: return 1LL << 30;
        case UnitKind::Tebibyte: return 1LL << 40;
        case UnitKind::Block:
            return custom_block_size.value_or(DEFAULT_BLOCK_SIZE);
    }
    return 1;
}

// --- ByteOffset ---

std::expected<NonNegativeI64, std::string>
ByteOffset::assume_forward_offset_from_start() const {
    switch (kind) {
        case ByteOffsetKind::ForwardFromBeginning:
        case ByteOffsetKind::ForwardFromLastOffset:
            return value;
        case ByteOffsetKind::BackwardFromEnd:
            return std::unexpected(std::string(
                "negative offset specified, but only positive offsets (counts) "
                "are accepted in this context"));
    }
    return std::unexpected(std::string("unknown offset kind"));
}

// --- ParseError ---

std::string ParseError::message() const {
    switch (kind) {
        case ByteOffsetParseError::Empty:
            return "no character data found, did you forget to write it?";
        case ByteOffsetParseError::EmptyAfterSign:
            return "no digits found after sign, did you forget to write them?";
        case ByteOffsetParseError::SignFoundAfterHexPrefix:
            return "found '" + detail + "' sign after hex prefix (\"0x\"); signs should go before it";
        case ByteOffsetParseError::InvalidNumAndUnit:
            return "\"" + detail + "\" is not of the expected form <pos-integer>[<unit>]";
        case ByteOffsetParseError::EmptyWithUnit:
            return "\"" + detail + "\" is a valid unit, but an integer should come before it";
        case ByteOffsetParseError::InvalidUnit:
            return "invalid unit \"" + detail + "\"";
        case ByteOffsetParseError::ParseNum:
            return "failed to parse integer part";
        case ByteOffsetParseError::UnitMultiplicationOverflow:
            return "count multiplied by the unit overflowed a signed 64-bit integer; "
                   "are you sure it should be that big?";
    }
    return "unknown error";
}

// --- Parsing ---

std::expected<std::pair<std::string_view, ByteOffsetKind>, ParseError>
process_sign_of(std::string_view n) {
    if (n.empty()) {
        return std::unexpected(ParseError{ByteOffsetParseError::Empty, ""});
    }
    char first = n[0];
    if (first == '+') {
        auto rest = n.substr(1);
        if (rest.empty()) {
            return std::unexpected(ParseError{ByteOffsetParseError::EmptyAfterSign, ""});
        }
        return std::pair{rest, ByteOffsetKind::ForwardFromLastOffset};
    }
    if (first == '-') {
        auto rest = n.substr(1);
        if (rest.empty()) {
            return std::unexpected(ParseError{ByteOffsetParseError::EmptyAfterSign, ""});
        }
        return std::pair{rest, ByteOffsetKind::BackwardFromEnd};
    }
    return std::pair{n, ByteOffsetKind::ForwardFromBeginning};
}

std::optional<std::expected<int64_t, ParseError>>
try_parse_as_hex_number(std::string_view n) {
    if (n.size() < 2 || n[0] != '0' || n[1] != 'x') {
        return std::nullopt; // not a hex number
    }
    auto num = n.substr(2);
    if (!num.empty()) {
        char c = num[0];
        if (c == '+' || c == '-') {
            if (num.size() == 1) {
                return std::expected<int64_t, ParseError>(
                    std::unexpected(ParseError{ByteOffsetParseError::EmptyAfterSign, ""}));
            }
            return std::expected<int64_t, ParseError>(
                std::unexpected(ParseError{ByteOffsetParseError::SignFoundAfterHexPrefix,
                                           std::string(1, c)}));
        }
    }
    int64_t result = 0;
    auto [ptr, ec] = std::from_chars(num.data(), num.data() + num.size(), result, 16);
    if (ec != std::errc{} || ptr != num.data() + num.size()) {
        return std::expected<int64_t, ParseError>(
            std::unexpected(ParseError{ByteOffsetParseError::ParseNum, std::string(num)}));
    }
    return result;
}

std::expected<std::pair<int64_t, Unit>, ParseError>
extract_num_and_unit_from(std::string_view n) {
    if (n.empty()) {
        return std::unexpected(ParseError{ByteOffsetParseError::Empty, ""});
    }

    // Find where digits end
    size_t unit_begin = 0;
    while (unit_begin < n.size() && std::isdigit(static_cast<unsigned char>(n[unit_begin]))) {
        ++unit_begin;
    }

    if (unit_begin < n.size()) {
        // There's a unit part
        auto num_str = n.substr(0, unit_begin);
        auto raw_unit = n.substr(unit_begin);

        // Lowercase the unit
        std::string lower_unit(raw_unit);
        std::transform(lower_unit.begin(), lower_unit.end(), lower_unit.begin(),
                       [](unsigned char c) { return std::tolower(c); });

        Unit unit;
        if (lower_unit.empty()) {
            unit = {UnitKind::Byte, std::nullopt};
        } else if (lower_unit == "kb") {
            unit = {UnitKind::Kilobyte, std::nullopt};
        } else if (lower_unit == "mb") {
            unit = {UnitKind::Megabyte, std::nullopt};
        } else if (lower_unit == "gb") {
            unit = {UnitKind::Gigabyte, std::nullopt};
        } else if (lower_unit == "tb") {
            unit = {UnitKind::Terabyte, std::nullopt};
        } else if (lower_unit == "kib") {
            unit = {UnitKind::Kibibyte, std::nullopt};
        } else if (lower_unit == "mib") {
            unit = {UnitKind::Mebibyte, std::nullopt};
        } else if (lower_unit == "gib") {
            unit = {UnitKind::Gibibyte, std::nullopt};
        } else if (lower_unit == "tib") {
            unit = {UnitKind::Tebibyte, std::nullopt};
        } else if (lower_unit == "block" || lower_unit == "blocks") {
            unit = {UnitKind::Block, std::nullopt};
        } else {
            if (num_str.empty()) {
                return std::unexpected(ParseError{ByteOffsetParseError::InvalidNumAndUnit,
                                                   std::string(raw_unit)});
            }
            return std::unexpected(ParseError{ByteOffsetParseError::InvalidUnit,
                                               std::string(raw_unit)});
        }

        if (num_str.empty()) {
            return std::unexpected(ParseError{ByteOffsetParseError::EmptyWithUnit,
                                               std::string(raw_unit)});
        }

        int64_t num = 0;
        auto [ptr, ec] = std::from_chars(num_str.data(), num_str.data() + num_str.size(), num);
        if (ec != std::errc{} || ptr != num_str.data() + num_str.size()) {
            return std::unexpected(ParseError{ByteOffsetParseError::ParseNum,
                                               std::string(num_str)});
        }
        return std::pair{num, unit};
    }

    // No unit part — just a number
    int64_t num = 0;
    auto [ptr, ec] = std::from_chars(n.data(), n.data() + n.size(), num);
    if (ec != std::errc{} || ptr != n.data() + n.size()) {
        return std::unexpected(ParseError{ByteOffsetParseError::ParseNum, std::string(n)});
    }
    return std::pair{num, Unit{UnitKind::Byte, std::nullopt}};
}

std::expected<ByteOffset, ParseError>
parse_byte_offset(std::string_view n, PositiveI64 block_size) {
    auto sign_result = process_sign_of(n);
    if (!sign_result) return std::unexpected(sign_result.error());
    auto [remaining, kind] = *sign_result;

    auto make_offset = [&](int64_t value) -> std::expected<ByteOffset, ParseError> {
        auto nn = NonNegativeI64::create(value);
        if (!nn) {
            return std::unexpected(ParseError{ByteOffsetParseError::ParseNum, "negative value"});
        }
        return ByteOffset{*nn, kind};
    };

    // Try hex
    if (auto hex = try_parse_as_hex_number(remaining)) {
        if (!*hex) return std::unexpected(hex->error());
        return make_offset(**hex);
    }

    // Parse number + unit
    auto nu_result = extract_num_and_unit_from(remaining);
    if (!nu_result) return std::unexpected(nu_result.error());
    auto [num, unit] = *nu_result;

    // Fill in block size if needed
    if (unit.kind == UnitKind::Block && !unit.custom_block_size) {
        unit.custom_block_size = block_size.value;
    }

    // Checked multiplication
    int64_t multiplier = unit.get_multiplier();
    // Check for overflow
    if (multiplier != 0 && (num > INT64_MAX / multiplier || num < INT64_MIN / multiplier)) {
        return std::unexpected(ParseError{ByteOffsetParseError::UnitMultiplicationOverflow, ""});
    }
    int64_t result = num * multiplier;

    return make_offset(result);
}

} // namespace hexyl
