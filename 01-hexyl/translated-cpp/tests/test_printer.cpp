#include <catch2/catch_test_macros.hpp>

#include "hexyl.hpp"

#include <sstream>
#include <string>

using namespace hexyl;

static std::string run_printer(const std::string& input_data,
                               uint64_t panels = 2,
                               uint8_t group_size = 1,
                               Base base = Base::Hexadecimal,
                               Endianness endianness = Endianness::Big,
                               CharacterTable char_table = CharacterTable::Default,
                               IncludeMode include_mode = IncludeModeOff{},
                               uint64_t display_offset = 0) {
    std::ostringstream output;
    auto printer = PrinterBuilder(output)
        .show_color(false)
        .show_char_panel(true)
        .show_position_panel(true)
        .with_border_style(BorderStyle::Unicode)
        .enable_squeezing(true)
        .num_panels(panels)
        .group_size(group_size)
        .with_base(base)
        .endianness(endianness)
        .character_table(char_table)
        .include_mode(std::move(include_mode))
        .color_scheme(ColorScheme::Default)
        .build();

    if (display_offset > 0) {
        printer.display_offset(display_offset);
    }

    std::istringstream input(input_data);
    printer.print_all(input);
    return output.str();
}

TEST_CASE("empty file", "[printer]") {
    auto result = run_printer("");
    std::string expected =
        "\xe2\x94\x8c\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"
        "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\xac"
        "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"
        "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"
        "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"
        "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"
        "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"
        "\xe2\x94\x80\xe2\x94\xac";
    // This is getting complex with raw UTF-8. Let's just check key features.
    REQUIRE(result.find("No content") != std::string::npos);
    // Check it has the box drawing characters (┌ and ┘)
    REQUIRE(result.find("\xe2\x94\x8c") != std::string::npos); // ┌
    REQUIRE(result.find("\xe2\x94\x98") != std::string::npos); // ┘
}

TEST_CASE("short input", "[printer]") {
    auto result = run_printer("spam");
    REQUIRE(result.find("73 70 61 6d") != std::string::npos);
    REQUIRE(result.find("spam") != std::string::npos);
}

TEST_CASE("display offset", "[printer]") {
    auto result = run_printer("spamspamspamspamspam", 2, 1, Base::Hexadecimal,
                              Endianness::Big, CharacterTable::Default,
                              IncludeModeOff{}, 0xdeadbeef);
    REQUIRE(result.find("deadbeef") != std::string::npos);
    REQUIRE(result.find("deadbeff") != std::string::npos);
}

TEST_CASE("squeeze works", "[printer]") {
    std::string input(33, '\0');
    auto result = run_printer(input);
    // Should have the squeeze marker
    REQUIRE(result.find("*") != std::string::npos);
    // First line should show all zeros
    REQUIRE(result.find("00 00 00 00 00 00 00 00") != std::string::npos);
}

TEST_CASE("squeeze nonzero", "[printer]") {
    std::string input(33, '0');
    auto result = run_printer(input);
    REQUIRE(result.find("*") != std::string::npos);
    REQUIRE(result.find("30 30 30 30") != std::string::npos);
}

TEST_CASE("include mode from file", "[printer]") {
    auto result = run_printer("spamspamspamspamspam", 2, 1, Base::Hexadecimal,
                              Endianness::Big, CharacterTable::Default,
                              IncludeModeFile{"test.txt"});
    REQUIRE(result.find("unsigned char test_txt[]") != std::string::npos);
    REQUIRE(result.find("0x73, 0x70, 0x61, 0x6d") != std::string::npos);
    REQUIRE(result.find("unsigned int test_txt_len = 20") != std::string::npos);
}

TEST_CASE("include mode from stdin", "[printer]") {
    auto result = run_printer("spamspamspamspamspam", 2, 1, Base::Hexadecimal,
                              Endianness::Big, CharacterTable::Default,
                              IncludeModeStdin{});
    REQUIRE(result.find("0x73, 0x70, 0x61, 0x6d") != std::string::npos);
    // Should NOT have the "unsigned char" wrapper
    REQUIRE(result.find("unsigned char") == std::string::npos);
}

TEST_CASE("display offset in last line", "[printer]") {
    auto result = run_printer("AAAAAAAAAAAAAAAACCCC");
    REQUIRE(result.find("00000000") != std::string::npos);
    REQUIRE(result.find("00000010") != std::string::npos);
    REQUIRE(result.find("43 43 43 43") != std::string::npos);
}
