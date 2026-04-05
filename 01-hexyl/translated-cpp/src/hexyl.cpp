#include "hexyl.hpp"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <format>
#include <iomanip>
#include <sstream>

namespace hexyl {

// --- Byte classification and color ---

ByteCategory byte_category(uint8_t b) {
    if (b == 0x00) return ByteCategory::Null;
    if (b > 0x20 && b < 0x7F) return ByteCategory::AsciiPrintable; // is_ascii_graphic
    // Rust's is_ascii_whitespace: \t(09), \n(0A), \x0C(0C), \r(0D), space(20)
    // Notably excludes \x0B (vertical tab)
    if (b == 0x09 || b == 0x0A || b == 0x0C || b == 0x0D || b == 0x20)
        return ByteCategory::AsciiWhitespace;
    if (b < 0x80) return ByteCategory::AsciiOther;
    return ByteCategory::NonAscii;
}

// For the Default color scheme, returns a string_view into the lazily-initialized color strings.
std::string_view byte_color(uint8_t b, ColorScheme scheme) {
    auto cat = byte_category(b);
    if (scheme == ColorScheme::Default) {
        switch (cat) {
            case ByteCategory::Null: return color_null();
            case ByteCategory::AsciiPrintable: return color_ascii_printable();
            case ByteCategory::AsciiWhitespace: return color_ascii_whitespace();
            case ByteCategory::AsciiOther: return color_ascii_other();
            case ByteCategory::NonAscii: return color_nonascii();
        }
    }
    // Gradient scheme returns empty — handled separately with RGB
    return {};
}

const uint8_t* byte_color_rgb(uint8_t b) {
    auto cat = byte_category(b);
    switch (cat) {
        case ByteCategory::Null:
            return COLOR_NULL_RGB.data();
        case ByteCategory::AsciiWhitespace:
            if (b == ' ') return COLOR_GRADIENT_ASCII_PRINTABLE[0].data();
            // fall through
        case ByteCategory::AsciiOther:
            if (b == 0x7f) return COLOR_DEL.data();
            return COLOR_GRADIENT_ASCII_NONPRINTABLE[b - 1].data();
        case ByteCategory::AsciiPrintable:
            return COLOR_GRADIENT_ASCII_PRINTABLE[b - ' '].data();
        case ByteCategory::NonAscii:
            return COLOR_GRADIENT_NONASCII[b - 128].data();
    }
    return COLOR_NULL_RGB.data(); // unreachable
}

static uint8_t to_braille_bits(uint8_t byte) {
    static constexpr int mapping[8] = {0, 3, 1, 4, 2, 5, 6, 7};
    uint8_t out = 0;
    for (int from = 0; from < 8; ++from) {
        out |= static_cast<uint8_t>(((byte >> from) & 1) << mapping[from]);
    }
    return out;
}

char32_t byte_as_char(uint8_t b, CharacterTable table) {
    auto cat = byte_category(b);
    switch (table) {
        case CharacterTable::Default:
            switch (cat) {
                case ByteCategory::Null: return U'⋄';
                case ByteCategory::AsciiPrintable: return static_cast<char32_t>(b);
                case ByteCategory::AsciiWhitespace:
                    return (b == 0x20) ? U' ' : U'_';
                case ByteCategory::AsciiOther: return U'•';
                case ByteCategory::NonAscii: return U'×';
            }
            break;
        case CharacterTable::Ascii:
            switch (cat) {
                case ByteCategory::Null: return U'.';
                case ByteCategory::AsciiPrintable: return static_cast<char32_t>(b);
                case ByteCategory::AsciiWhitespace:
                    return (b == 0x20) ? U' ' : U'.';
                case ByteCategory::AsciiOther: return U'.';
                case ByteCategory::NonAscii: return U'.';
            }
            break;
        case CharacterTable::CP1047:
            return static_cast<char32_t>(CP1047[b]);
        case CharacterTable::CP437:
            return CP437[b];
        case CharacterTable::Braille:
            switch (cat) {
                case ByteCategory::Null: return U'⋄';
                case ByteCategory::AsciiPrintable: return static_cast<char32_t>(b);
                case ByteCategory::AsciiWhitespace:
                    if (b == ' ') return U' ';
                    if (b == '\t') return U'→';
                    if (b == '\n') return U'↵';
                    if (b == '\r') return U'←';
                    // fall through to braille
                    return static_cast<char32_t>(0x2800 + to_braille_bits(b));
                case ByteCategory::AsciiOther:
                case ByteCategory::NonAscii:
                    return static_cast<char32_t>(0x2800 + to_braille_bits(b));
            }
            break;
    }
    return U'?'; // unreachable
}

// --- Printer implementation ---

Printer::Printer(std::ostream& writer, bool show_color, bool show_char_panel,
                 bool show_position_panel, BorderStyle border_style,
                 bool use_squeeze, uint64_t panels, uint8_t group_size,
                 Base base, Endianness endianness, CharacterTable character_table,
                 IncludeMode include_mode, ColorScheme color_scheme)
    : line_buf_(8 * static_cast<size_t>(panels), 0)
    , writer_(writer)
    , show_char_panel_(show_char_panel)
    , show_position_panel_(show_position_panel)
    , show_color_(show_color)
    , color_scheme_(color_scheme)
    , border_style_(border_style)
    , squeezer_(use_squeeze ? Squeezer::Ignore : Squeezer::Disabled)
    , panels_(panels)
    , group_size_(group_size)
    , endianness_(endianness)
    , include_mode_(std::move(include_mode))
{
    // Pre-compute byte display strings for hex panel
    byte_hex_panel_.resize(256);
    for (int i = 0; i < 256; ++i) {
        switch (base) {
            case Base::Binary:
                byte_hex_panel_[i] = std::format("{:08b}", i);
                break;
            case Base::Octal:
                byte_hex_panel_[i] = std::format("{:03o}", i);
                break;
            case Base::Decimal:
                byte_hex_panel_[i] = std::format("{:03d}", i);
                break;
            case Base::Hexadecimal:
                byte_hex_panel_[i] = std::format("{:02x}", i);
                break;
        }
    }

    // Pre-compute byte display strings for character panel
    byte_char_panel_.resize(256);
    for (int i = 0; i < 256; ++i) {
        byte_char_panel_[i] = to_utf8(byte_as_char(static_cast<uint8_t>(i), character_table));
    }

    // Gray hex panel (for position display)
    byte_hex_panel_g_.resize(256);
    for (int i = 0; i < 256; ++i) {
        byte_hex_panel_g_[i] = std::format("{:02x}", i);
    }

    base_digits_ = [&] {
        switch (base) {
            case Base::Binary: return uint8_t(8);
            case Base::Octal: return uint8_t(3);
            case Base::Decimal: return uint8_t(3);
            case Base::Hexadecimal: return uint8_t(2);
        }
        return uint8_t(2);
    }();
}

void Printer::write_bytes(const uint8_t* data, size_t len) {
    writer_.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(len));
}

void Printer::write_bytes(std::string_view sv) {
    writer_.write(sv.data(), static_cast<std::streamsize>(sv.size()));
}

void Printer::write_str(const std::string& s) {
    writer_.write(s.data(), static_cast<std::streamsize>(s.size()));
}

void Printer::write_char(char c) {
    writer_.put(c);
}

void Printer::write_utf8(char32_t cp) {
    std::string s;
    encode_utf8(cp, s);
    write_str(s);
}

char32_t Printer::outer_sep() const {
    switch (border_style_) {
        case BorderStyle::Unicode: return U'│';
        case BorderStyle::Ascii: return U'|';
        case BorderStyle::None: return U' ';
    }
    return U'│';
}

char32_t Printer::inner_sep() const {
    switch (border_style_) {
        case BorderStyle::Unicode: return U'┊';
        case BorderStyle::Ascii: return U'|';
        case BorderStyle::None: return U' ';
    }
    return U'┊';
}

size_t Printer::panel_sz() const {
    size_t group_sz = static_cast<size_t>(base_digits_) * group_size_ + 1;
    size_t group_per_panel = 8 / group_size_;
    return 1 + group_sz * group_per_panel;
}

void Printer::write_border(char32_t left_corner, char32_t horiz,
                           char32_t col_sep, char32_t right_corner) {
    std::string h_str = to_utf8(horiz);
    std::string h8;
    for (int i = 0; i < 8; ++i) h8 += h_str;
    std::string h_repeat;
    for (size_t i = 0; i < panel_sz(); ++i) h_repeat += h_str;

    if (show_position_panel_) {
        write_utf8(left_corner);
        write_str(h8);
        write_utf8(col_sep);
    } else {
        write_utf8(left_corner);
    }

    for (uint64_t i = 0; i < panels_ - 1; ++i) {
        write_str(h_repeat);
        write_utf8(col_sep);
    }
    if (show_char_panel_) {
        write_str(h_repeat);
        write_utf8(col_sep);
    } else {
        write_str(h_repeat);
    }

    if (show_char_panel_) {
        for (uint64_t i = 0; i < panels_ - 1; ++i) {
            write_str(h8);
            write_utf8(col_sep);
        }
        write_str(h8);
        write_utf8(right_corner);
    } else {
        write_utf8(right_corner);
    }

    write_char('\n');
}

void Printer::print_header() {
    switch (border_style_) {
        case BorderStyle::Unicode:
            write_border(U'┌', U'─', U'┬', U'┐');
            break;
        case BorderStyle::Ascii:
            write_border(U'+', U'-', U'+', U'+');
            break;
        case BorderStyle::None:
            break;
    }
}

void Printer::print_footer() {
    switch (border_style_) {
        case BorderStyle::Unicode:
            write_border(U'└', U'─', U'┴', U'┘');
            break;
        case BorderStyle::Ascii:
            write_border(U'+', U'-', U'+', U'+');
            break;
        case BorderStyle::None:
            break;
    }
}

void Printer::print_position_panel() {
    write_utf8(outer_sep());
    if (show_color_) {
        write_str(color_offset());
    }
    if (show_position_panel_) {
        if (squeezer_ == Squeezer::Print) {
            write_char('*');
            if (show_color_) {
                write_bytes(COLOR_RESET);
            }
            write_bytes(reinterpret_cast<const uint8_t*>("       "), 7);
        } else {
            uint64_t addr = idx_ + display_offset_;
            uint8_t byte_index[8];
            for (int i = 7; i >= 0; --i) {
                byte_index[i] = static_cast<uint8_t>(addr & 0xFF);
                addr >>= 8;
            }
            // Skip leading zero bytes (but always show at least 4 bytes)
            int skip = 0;
            while (byte_index[skip] == 0x00 && skip < 4) {
                ++skip;
            }
            for (int i = skip; i < 8; ++i) {
                write_str(byte_hex_panel_g_[byte_index[i]]);
            }
            if (show_color_) {
                write_bytes(COLOR_RESET);
            }
        }
        write_utf8(outer_sep());
    }
}

void Printer::print_char(uint64_t i) {
    if (squeezer_ == Squeezer::Print || squeezer_ == Squeezer::Delete) {
        write_char(' ');
    } else {
        if (i < line_buf_.size()) {
            uint8_t b = line_buf_[i];
            if (show_color_) {
                std::string_view new_color;
                if (color_scheme_ == ColorScheme::Default) {
                    new_color = byte_color(b, color_scheme_);
                }
                bool need_set = false;
                if (color_scheme_ == ColorScheme::Gradient) {
                    // Always write RGB for gradient
                    write_bytes(byte_color_rgb(b), 19);
                    has_curr_color_ = true;
                    curr_color_ = {}; // force re-set on next non-gradient
                } else if (!has_curr_color_ || curr_color_ != new_color) {
                    write_bytes(new_color);
                    curr_color_ = new_color;
                    has_curr_color_ = true;
                }
                (void)need_set;
            }
            write_str(byte_char_panel_[b]);
        } else {
            squeezer_ = Squeezer::Print;
        }
    }

    if (i == 8 * panels_ - 1) {
        if (show_color_) {
            write_bytes(COLOR_RESET);
            has_curr_color_ = false;
        }
        write_utf8(outer_sep());
    } else if (i % 8 == 7) {
        if (show_color_) {
            write_bytes(COLOR_RESET);
            has_curr_color_ = false;
        }
        write_utf8(inner_sep());
    }
}

void Printer::print_char_panel() {
    for (size_t i = 0; i < line_buf_.size(); ++i) {
        print_char(static_cast<uint64_t>(i));
    }
}

void Printer::print_byte(size_t i, uint8_t b) {
    if (squeezer_ == Squeezer::Print) {
        if (!show_position_panel_ && i == 0) {
            if (show_color_) {
                write_str(color_offset());
            }
            write_str(byte_char_panel_['*']);
            if (show_color_) {
                write_bytes(COLOR_RESET);
            }
        } else if (i % group_size_ == 0) {
            write_char(' ');
        }
        for (int d = 0; d < base_digits_; ++d) {
            write_char(' ');
        }
    } else if (squeezer_ == Squeezer::Delete) {
        write_bytes(reinterpret_cast<const uint8_t*>("   "), 3);
    } else {
        // Ignore or Disabled
        if (i % group_size_ == 0) {
            write_char(' ');
        }
        if (show_color_) {
            if (color_scheme_ == ColorScheme::Gradient) {
                write_bytes(byte_color_rgb(b), 19);
                has_curr_color_ = true;
                curr_color_ = {};
            } else {
                auto new_color = byte_color(b, color_scheme_);
                if (!has_curr_color_ || curr_color_ != new_color) {
                    write_bytes(new_color);
                    curr_color_ = new_color;
                    has_curr_color_ = true;
                }
            }
        }
        write_str(byte_hex_panel_[b]);
    }

    // byte is last in panel
    if (i % 8 == 7) {
        if (show_color_) {
            has_curr_color_ = false;
            write_bytes(COLOR_RESET);
        }
        write_char(' ');
        // byte is last in last panel
        if (static_cast<uint64_t>(i) % (8 * panels_) == 8 * panels_ - 1) {
            write_utf8(outer_sep());
        } else {
            write_utf8(inner_sep());
        }
    }
}

void Printer::reorder_buffer_to_little_endian(std::vector<uint8_t>& buf) const {
    size_t n = buf.size();
    size_t gs = group_size_;
    for (size_t idx = 0; idx < n; idx += gs) {
        size_t remaining = n - idx;
        size_t total = std::min(remaining, gs);
        std::reverse(buf.begin() + idx, buf.begin() + idx + total);
    }
}

void Printer::print_bytes() {
    auto buf = line_buf_;
    if (endianness_ == Endianness::Little) {
        reorder_buffer_to_little_endian(buf);
    }
    for (size_t i = 0; i < buf.size(); ++i) {
        print_byte(i, buf[i]);
    }
}

size_t Printer::print_bytes_in_include_style(std::istream& reader) {
    char buffer[1024];
    size_t total_bytes = 0;
    bool is_first_chunk = true;
    size_t line_counter = 0;

    while (true) {
        reader.read(buffer, sizeof(buffer));
        auto bytes_read = static_cast<size_t>(reader.gcount());
        if (bytes_read == 0) break;
        total_bytes += bytes_read;

        for (size_t i = 0; i < bytes_read; ++i) {
            if (line_counter % 12 == 0) {
                if (!is_first_chunk || line_counter > 0) {
                    write_bytes(reinterpret_cast<const uint8_t*>(",\n"), 2);
                }
                write_bytes(reinterpret_cast<const uint8_t*>("  "), 2);
                is_first_chunk = false;
            } else {
                write_bytes(reinterpret_cast<const uint8_t*>(", "), 2);
            }
            auto s = std::format("0x{:02x}", static_cast<uint8_t>(buffer[i]));
            write_str(s);
            ++line_counter;
        }
    }
    write_char('\n');
    return total_bytes;
}

void Printer::print_all(std::istream& reader) {
    bool is_empty = true;

    // Handle include mode
    if (auto* f = std::get_if<IncludeModeFile>(&include_mode_)) {
        // Convert non-alphanumeric to '_'
        std::string var_name;
        for (char c : f->filename) {
            var_name += std::isalnum(static_cast<unsigned char>(c)) ? c : '_';
        }
        write_str("unsigned char " + var_name + "[] = {\n");
        auto total = print_bytes_in_include_style(reader);
        write_str("};\n");
        write_str("unsigned int " + var_name + "_len = " + std::to_string(total) + ";\n");
        return;
    }
    if (std::holds_alternative<IncludeModeStdin>(include_mode_) ||
        std::holds_alternative<IncludeModeSlice>(include_mode_)) {
        print_bytes_in_include_style(reader);
        return;
    }

    // Main read loop
    size_t line_size = 8 * static_cast<size_t>(panels_);
    bool has_leftover = false;
    size_t leftover_count = 0;

    while (true) {
        reader.read(reinterpret_cast<char*>(line_buf_.data()),
                     static_cast<std::streamsize>(line_size));
        auto n = static_cast<size_t>(reader.gcount());

        if (n > 0 && n < line_size) {
            if (is_empty) {
                print_header();
                is_empty = false;
            }
            // Try to fill the rest
            size_t leftover = n;
            bool done = false;
            while (!done) {
                reader.read(reinterpret_cast<char*>(line_buf_.data() + leftover),
                             static_cast<std::streamsize>(line_size - leftover));
                auto extra = static_cast<size_t>(reader.gcount());
                leftover += extra;
                if (extra == 0) {
                    line_buf_.resize(leftover);
                    has_leftover = true;
                    leftover_count = leftover;
                    done = true;
                } else if (leftover >= line_size) {
                    done = true;
                    // We filled the line, continue in the main loop
                    has_leftover = false;
                }
            }
            if (has_leftover) break;
        } else if (n == 0) {
            if (squeezer_ == Squeezer::Delete) {
                line_buf_.clear();
                has_leftover = true;
                leftover_count = 0;
            }
            break;
        }

        if (is_empty) {
            print_header();
        }

        // Squeeze check: if active, check if line is all the same byte
        if (squeezer_ == Squeezer::Print || squeezer_ == Squeezer::Delete) {
            // Check if line is all squeeze_byte
            bool all_same = true;
            // Compare using size_t chunks like Rust
            size_t repeat_val = squeeze_byte_;
            const size_t* chunks = reinterpret_cast<const size_t*>(line_buf_.data());
            size_t num_chunks = line_buf_.size() / sizeof(size_t);
            for (size_t c = 0; c < num_chunks; ++c) {
                if (chunks[c] != repeat_val) {
                    all_same = false;
                    break;
                }
            }
            if (all_same) {
                if (squeezer_ == Squeezer::Delete) {
                    idx_ += 8 * panels_;
                    continue;
                }
            } else {
                squeezer_ = Squeezer::Ignore;
            }
        }

        // Print the line
        print_position_panel();
        print_bytes();
        if (show_char_panel_) {
            print_char_panel();
        }
        write_char('\n');

        if (is_empty) {
            writer_.flush();
            is_empty = false;
        }

        idx_ += 8 * panels_;

        // Transition from Print to Delete
        if (squeezer_ == Squeezer::Print) {
            squeezer_ = Squeezer::Delete;
        }

        // Check if this line is all the same byte (to start squeeze)
        if (squeezer_ != Squeezer::Disabled && squeezer_ != Squeezer::Delete) {
            uint8_t first = line_buf_[0];
            // Create a size_t with the byte repeated
            size_t repeat_byte = 0;
            std::memset(&repeat_byte, first, sizeof(size_t));

            const size_t* chunks = reinterpret_cast<const size_t*>(line_buf_.data());
            size_t num_chunks = line_buf_.size() / sizeof(size_t);
            bool all_same = true;
            for (size_t c = 0; c < num_chunks; ++c) {
                if (chunks[c] != repeat_byte) {
                    all_same = false;
                    break;
                }
            }
            if (all_same) {
                squeezer_ = Squeezer::Print;
                squeeze_byte_ = repeat_byte;
            }
        }
    }

    // Special ending
    if (is_empty && !has_leftover) {
        // Completely empty input
        base_digits_ = 2;
        print_header();
        if (show_position_panel_) {
            // "│        │"
            write_utf8(U'│');
            write_str("        ");
        }
        write_utf8(U'│');
        write_str(" No content");
        // Pad to fill panel
        size_t pad = panel_sz() - 1 - 10; // 10 = len("No content")
        for (size_t i = 0; i < pad; ++i) write_char(' ');
        write_utf8(U'│');
        // Second panel
        for (size_t i = 0; i < panel_sz(); ++i) write_char(' ');
        write_utf8(U'│');

        if (show_char_panel_) {
            for (size_t i = 0; i < 8; ++i) write_char(' ');
            write_utf8(U'│');
            for (size_t i = 0; i < 8; ++i) write_char(' ');
            write_utf8(U'│');
        }
        write_char('\n');
    } else if (has_leftover) {
        size_t n = leftover_count;
        // Print the incomplete last line
        squeezer_ = Squeezer::Ignore;
        print_position_panel();
        print_bytes();
        squeezer_ = Squeezer::Print;
        for (size_t i = n; i < 8 * static_cast<size_t>(panels_); ++i) {
            print_byte(i, 0);
        }
        if (show_char_panel_) {
            squeezer_ = Squeezer::Ignore;
            print_char_panel();
            squeezer_ = Squeezer::Print;
            for (size_t i = n; i < 8 * static_cast<size_t>(panels_); ++i) {
                print_char(static_cast<uint64_t>(i));
            }
        }
        write_char('\n');
    }

    print_footer();
    writer_.flush();
}

// --- PrinterBuilder ---

Printer PrinterBuilder::build() {
    return Printer(writer_, show_color_, show_char_panel_, show_position_panel_,
                   border_style_, use_squeeze_, panels_, group_size_, base_,
                   endianness_, character_table_, std::move(include_mode_),
                   color_scheme_);
}

} // namespace hexyl
