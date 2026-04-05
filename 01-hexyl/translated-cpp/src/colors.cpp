#include "colors.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <optional>
#include <string>

namespace hexyl {

// Parse an ANSI color name or hex code string into an ANSI foreground escape sequence.
// Supports: "black", "red", "green", "yellow", "blue", "magenta", "cyan", "white"
// and their "bright" variants, plus "#RRGGBB" hex codes.
static std::optional<std::string> parse_color_spec(const std::string& spec) {
    std::string lower = spec;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    // Standard ANSI colors
    struct ColorEntry { const char* name; int code; };
    static constexpr ColorEntry entries[] = {
        {"black", 30}, {"red", 31}, {"green", 32}, {"yellow", 33},
        {"blue", 34}, {"magenta", 35}, {"cyan", 36}, {"white", 37},
        {"bright black", 90}, {"bright red", 91}, {"bright green", 92},
        {"bright yellow", 93}, {"bright blue", 94}, {"bright magenta", 95},
        {"bright cyan", 96}, {"bright white", 97},
    };

    for (const auto& e : entries) {
        if (lower == e.name) {
            return "\x1b[" + std::to_string(e.code) + "m";
        }
    }

    // #RRGGBB hex color
    if (spec.size() == 7 && spec[0] == '#') {
        auto parse_hex = [](const std::string& s, int start) -> std::optional<uint8_t> {
            try {
                return static_cast<uint8_t>(std::stoi(s.substr(start, 2), nullptr, 16));
            } catch (...) {
                return std::nullopt;
            }
        };
        auto r = parse_hex(spec, 1);
        auto g = parse_hex(spec, 3);
        auto b = parse_hex(spec, 5);
        if (r && g && b) {
            return "\x1b[38;2;" + std::to_string(*r) + ";" +
                   std::to_string(*g) + ";" + std::to_string(*b) + "m";
        }
    }

    return std::nullopt;
}

// Initialize a color from an environment variable, falling back to a default ANSI code.
static std::string init_color(const char* name, int default_ansi_code) {
    std::string env_var = std::string("HEXYL_COLOR_") + name;
    const char* val = std::getenv(env_var.c_str());
    if (val) {
        if (auto parsed = parse_color_spec(val)) {
            return *parsed;
        }
    }
    return "\x1b[" + std::to_string(default_ansi_code) + "m";
}

// Meyer's singleton pattern — thread-safe by C++11 §6.7/4

const std::string& color_null() {
    static const std::string s = init_color("NULL", 90); // BrightBlack
    return s;
}

const std::string& color_offset() {
    static const std::string s = init_color("OFFSET", 90); // BrightBlack
    return s;
}

const std::string& color_ascii_printable() {
    static const std::string s = init_color("ASCII_PRINTABLE", 36); // Cyan
    return s;
}

const std::string& color_ascii_whitespace() {
    static const std::string s = init_color("ASCII_WHITESPACE", 32); // Green
    return s;
}

const std::string& color_ascii_other() {
    static const std::string s = init_color("ASCII_OTHER", 32); // Green
    return s;
}

const std::string& color_nonascii() {
    static const std::string s = init_color("NONASCII", 33); // Yellow
    return s;
}

void encode_utf8(char32_t cp, std::string& out) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0x10FFFF) {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

std::string to_utf8(char32_t cp) {
    std::string s;
    encode_utf8(cp, s);
    return s;
}

} // namespace hexyl
