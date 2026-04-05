#pragma once

#include "colors.hpp"

#include <cstdint>
#include <istream>
#include <memory>
#include <ostream>
#include <string>
#include <variant>
#include <vector>

namespace hexyl {

// --- Enums (translating Rust's simple enums to enum class) ---

enum class Base {
    Binary,
    Octal,
    Decimal,
    Hexadecimal,
};

enum class ByteCategory {
    Null,
    AsciiPrintable,
    AsciiWhitespace,
    AsciiOther,
    NonAscii,
};

// IncludeMode: has a data-carrying variant File(String)
// Rust: enum IncludeMode { File(String), Stdin, Slice, Off }
struct IncludeModeFile { std::string filename; };
struct IncludeModeStdin {};
struct IncludeModeSlice {};
struct IncludeModeOff {};
using IncludeMode = std::variant<IncludeModeFile, IncludeModeStdin, IncludeModeSlice, IncludeModeOff>;

enum class CharacterTable {
    Default,
    Ascii,
    CP1047,
    CP437,
    Braille,
};

enum class ColorScheme {
    Default,
    Gradient,
};

enum class Endianness {
    Little,
    Big,
};

enum class BorderStyle {
    Unicode,
    Ascii,
    None,
};

// --- Internal types ---

ByteCategory byte_category(uint8_t b);
std::string_view byte_color(uint8_t b, ColorScheme scheme);
// Returns a pointer to a 19-byte RGB escape sequence (for Gradient scheme)
const uint8_t* byte_color_rgb(uint8_t b);
char32_t byte_as_char(uint8_t b, CharacterTable table);

// --- Printer ---

class Printer {
public:
    // Use PrinterBuilder to construct
    Printer(std::ostream& writer, bool show_color, bool show_char_panel,
            bool show_position_panel, BorderStyle border_style,
            bool use_squeeze, uint64_t panels, uint8_t group_size,
            Base base, Endianness endianness, CharacterTable character_table,
            IncludeMode include_mode, ColorScheme color_scheme);

    void display_offset(uint64_t offset) { display_offset_ = offset; }
    void print_all(std::istream& reader);

private:
    void print_header();
    void print_footer();
    void print_position_panel();
    void print_char(uint64_t i);
    void print_char_panel();
    void print_byte(size_t i, uint8_t b);
    void print_bytes();
    void write_border(char32_t left_corner, char32_t horizontal_line,
                      char32_t column_separator, char32_t right_corner);
    size_t panel_sz() const;
    void reorder_buffer_to_little_endian(std::vector<uint8_t>& buf) const;
    size_t print_bytes_in_include_style(std::istream& reader);

    char32_t outer_sep() const;
    char32_t inner_sep() const;

    // Write helpers
    void write_bytes(const uint8_t* data, size_t len);
    void write_bytes(std::string_view sv);
    void write_str(const std::string& s);
    void write_char(char c);
    void write_utf8(char32_t cp);

    uint64_t idx_ = 0;
    std::vector<uint8_t> line_buf_;
    std::ostream& writer_;
    bool show_char_panel_;
    bool show_position_panel_;
    bool show_color_;
    std::string_view curr_color_;
    bool has_curr_color_ = false;
    ColorScheme color_scheme_;
    BorderStyle border_style_;
    std::vector<std::string> byte_hex_panel_;
    std::vector<std::string> byte_char_panel_;
    std::vector<std::string> byte_hex_panel_g_;

    enum class Squeezer { Print, Delete, Ignore, Disabled };
    Squeezer squeezer_;

    uint64_t display_offset_ = 0;
    uint64_t panels_;
    size_t squeeze_byte_ = 0;
    uint8_t group_size_;
    uint8_t base_digits_;
    Endianness endianness_;
    IncludeMode include_mode_;
};

// --- Builder ---

class PrinterBuilder {
public:
    explicit PrinterBuilder(std::ostream& writer) : writer_(writer) {}

    PrinterBuilder& show_color(bool v) { show_color_ = v; return *this; }
    PrinterBuilder& show_char_panel(bool v) { show_char_panel_ = v; return *this; }
    PrinterBuilder& show_position_panel(bool v) { show_position_panel_ = v; return *this; }
    PrinterBuilder& with_border_style(BorderStyle v) { border_style_ = v; return *this; }
    PrinterBuilder& enable_squeezing(bool v) { use_squeeze_ = v; return *this; }
    PrinterBuilder& num_panels(uint64_t v) { panels_ = v; return *this; }
    PrinterBuilder& group_size(uint8_t v) { group_size_ = v; return *this; }
    PrinterBuilder& with_base(Base v) { base_ = v; return *this; }
    PrinterBuilder& endianness(Endianness v) { endianness_ = v; return *this; }
    PrinterBuilder& character_table(CharacterTable v) { character_table_ = v; return *this; }
    PrinterBuilder& include_mode(IncludeMode v) { include_mode_ = std::move(v); return *this; }
    PrinterBuilder& color_scheme(ColorScheme v) { color_scheme_ = v; return *this; }

    Printer build();

private:
    std::ostream& writer_;
    bool show_color_ = true;
    bool show_char_panel_ = true;
    bool show_position_panel_ = true;
    BorderStyle border_style_ = BorderStyle::Unicode;
    bool use_squeeze_ = true;
    uint64_t panels_ = 2;
    uint8_t group_size_ = 1;
    Base base_ = Base::Hexadecimal;
    Endianness endianness_ = Endianness::Big;
    CharacterTable character_table_ = CharacterTable::Default;
    IncludeMode include_mode_ = IncludeModeOff{};
    ColorScheme color_scheme_ = ColorScheme::Default;
};

} // namespace hexyl
