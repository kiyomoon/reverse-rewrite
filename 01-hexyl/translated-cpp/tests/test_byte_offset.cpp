#include <catch2/catch_test_macros.hpp>

#include "byte_offset.hpp"

using namespace hexyl;

TEST_CASE("unit multipliers", "[byte_offset]") {
    REQUIRE(Unit{UnitKind::Kilobyte, {}}.get_multiplier() == 1000 * Unit{UnitKind::Byte, {}}.get_multiplier());
    REQUIRE(Unit{UnitKind::Megabyte, {}}.get_multiplier() == 1000 * Unit{UnitKind::Kilobyte, {}}.get_multiplier());
    REQUIRE(Unit{UnitKind::Gigabyte, {}}.get_multiplier() == 1000 * Unit{UnitKind::Megabyte, {}}.get_multiplier());
    REQUIRE(Unit{UnitKind::Terabyte, {}}.get_multiplier() == 1000 * Unit{UnitKind::Gigabyte, {}}.get_multiplier());

    REQUIRE(Unit{UnitKind::Kibibyte, {}}.get_multiplier() == 1024 * Unit{UnitKind::Byte, {}}.get_multiplier());
    REQUIRE(Unit{UnitKind::Mebibyte, {}}.get_multiplier() == 1024 * Unit{UnitKind::Kibibyte, {}}.get_multiplier());
    REQUIRE(Unit{UnitKind::Gibibyte, {}}.get_multiplier() == 1024 * Unit{UnitKind::Mebibyte, {}}.get_multiplier());
    REQUIRE(Unit{UnitKind::Tebibyte, {}}.get_multiplier() == 1024 * Unit{UnitKind::Gibibyte, {}}.get_multiplier());
}

TEST_CASE("process sign", "[byte_offset]") {
    {
        auto r = process_sign_of("123");
        REQUIRE(r.has_value());
        REQUIRE(r->first == "123");
        REQUIRE(r->second == ByteOffsetKind::ForwardFromBeginning);
    }
    {
        auto r = process_sign_of("+123");
        REQUIRE(r.has_value());
        REQUIRE(r->first == "123");
        REQUIRE(r->second == ByteOffsetKind::ForwardFromLastOffset);
    }
    {
        auto r = process_sign_of("-123");
        REQUIRE(r.has_value());
        REQUIRE(r->first == "123");
        REQUIRE(r->second == ByteOffsetKind::BackwardFromEnd);
    }
    {
        auto r = process_sign_of("-");
        REQUIRE(!r.has_value());
        REQUIRE(r.error().kind == ByteOffsetParseError::EmptyAfterSign);
    }
    {
        auto r = process_sign_of("+");
        REQUIRE(!r.has_value());
        REQUIRE(r.error().kind == ByteOffsetParseError::EmptyAfterSign);
    }
    {
        auto r = process_sign_of("");
        REQUIRE(!r.has_value());
        REQUIRE(r.error().kind == ByteOffsetParseError::Empty);
    }
}

TEST_CASE("parse as hex", "[byte_offset]") {
    REQUIRE(!try_parse_as_hex_number("73").has_value());
    {
        auto r = try_parse_as_hex_number("0x1337");
        REQUIRE(r.has_value());
        REQUIRE(r->has_value());
        REQUIRE(**r == 0x1337);
    }
    {
        auto r = try_parse_as_hex_number("0xnope");
        REQUIRE(r.has_value());
        REQUIRE(!r->has_value());
    }
    {
        auto r = try_parse_as_hex_number("0x-1");
        REQUIRE(r.has_value());
        REQUIRE(!r->has_value());
    }
}

TEST_CASE("extract num and unit", "[byte_offset]") {
    {
        auto r = extract_num_and_unit_from("4");
        REQUIRE(r.has_value());
        REQUIRE(r->first == 4);
        REQUIRE(r->second.kind == UnitKind::Byte);
    }
    {
        auto r = extract_num_and_unit_from("2blocks");
        REQUIRE(r.has_value());
        REQUIRE(r->first == 2);
        REQUIRE(r->second.kind == UnitKind::Block);
    }
    {
        auto r = extract_num_and_unit_from("1024kb");
        REQUIRE(r.has_value());
        REQUIRE(r->first == 1024);
        REQUIRE(r->second.kind == UnitKind::Kilobyte);
    }
    {
        auto r = extract_num_and_unit_from("gib");
        REQUIRE(!r.has_value());
        REQUIRE(r.error().kind == ByteOffsetParseError::EmptyWithUnit);
    }
    {
        auto r = extract_num_and_unit_from("");
        REQUIRE(!r.has_value());
        REQUIRE(r.error().kind == ByteOffsetParseError::Empty);
    }
    {
        auto r = extract_num_and_unit_from("25litres");
        REQUIRE(!r.has_value());
        REQUIRE(r.error().kind == ByteOffsetParseError::InvalidUnit);
    }
}

TEST_CASE("parse byte offset", "[byte_offset]") {
    auto bs = PositiveI64::create(DEFAULT_BLOCK_SIZE).value();

    // Basic numbers
    {
        auto r = parse_byte_offset("0", bs);
        REQUIRE(r.has_value());
        REQUIRE(r->value.value == 0);
        REQUIRE(r->kind == ByteOffsetKind::ForwardFromBeginning);
    }
    {
        auto r = parse_byte_offset("100", bs);
        REQUIRE(r.has_value());
        REQUIRE(r->value.value == 100);
        REQUIRE(r->kind == ByteOffsetKind::ForwardFromBeginning);
    }
    {
        auto r = parse_byte_offset("+100", bs);
        REQUIRE(r.has_value());
        REQUIRE(r->value.value == 100);
        REQUIRE(r->kind == ByteOffsetKind::ForwardFromLastOffset);
    }

    // Hex numbers
    {
        auto r = parse_byte_offset("0x0", bs);
        REQUIRE(r.has_value());
        REQUIRE(r->value.value == 0);
    }
    {
        auto r = parse_byte_offset("0xf", bs);
        REQUIRE(r.has_value());
        REQUIRE(r->value.value == 15);
    }
    {
        auto r = parse_byte_offset("0xdeadbeef", bs);
        REQUIRE(r.has_value());
        REQUIRE(r->value.value == 3'735'928'559LL);
    }

    // Units
    {
        auto r = parse_byte_offset("1KB", bs);
        REQUIRE(r.has_value());
        REQUIRE(r->value.value == 1000);
    }
    {
        auto r = parse_byte_offset("2MB", bs);
        REQUIRE(r.has_value());
        REQUIRE(r->value.value == 2'000'000);
    }
    {
        auto r = parse_byte_offset("1GiB", bs);
        REQUIRE(r.has_value());
        REQUIRE(r->value.value == 1'073'741'824);
    }

    // Blocks
    {
        auto r = parse_byte_offset("1block", PositiveI64::create(512).value());
        REQUIRE(r.has_value());
        REQUIRE(r->value.value == 512);
    }
    {
        auto r = parse_byte_offset("2block", PositiveI64::create(4).value());
        REQUIRE(r.has_value());
        REQUIRE(r->value.value == 8);
    }

    // Errors
    {
        auto r = parse_byte_offset("", bs);
        REQUIRE(!r.has_value());
        REQUIRE(r.error().kind == ByteOffsetParseError::Empty);
    }
    {
        auto r = parse_byte_offset("+", bs);
        REQUIRE(!r.has_value());
        REQUIRE(r.error().kind == ByteOffsetParseError::EmptyAfterSign);
    }
    {
        auto r = parse_byte_offset("-", bs);
        REQUIRE(!r.has_value());
        REQUIRE(r.error().kind == ByteOffsetParseError::EmptyAfterSign);
    }
    {
        auto r = parse_byte_offset("K", bs);
        REQUIRE(!r.has_value());
        REQUIRE(r.error().kind == ByteOffsetParseError::InvalidNumAndUnit);
    }
    {
        auto r = parse_byte_offset("block", bs);
        REQUIRE(!r.has_value());
        REQUIRE(r.error().kind == ByteOffsetParseError::EmptyWithUnit);
    }
    {
        auto r = parse_byte_offset("0x-12", bs);
        REQUIRE(!r.has_value());
        REQUIRE(r.error().kind == ByteOffsetParseError::SignFoundAfterHexPrefix);
    }
    {
        auto r = parse_byte_offset("0x+12", bs);
        REQUIRE(!r.has_value());
        REQUIRE(r.error().kind == ByteOffsetParseError::SignFoundAfterHexPrefix);
    }
    {
        auto r = parse_byte_offset("1234asdf", bs);
        REQUIRE(!r.has_value());
        REQUIRE(r.error().kind == ByteOffsetParseError::InvalidUnit);
    }
    {
        auto r = parse_byte_offset("20000000TiB", bs);
        REQUIRE(!r.has_value());
        REQUIRE(r.error().kind == ByteOffsetParseError::UnitMultiplicationOverflow);
    }
}
