#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace hexyl {

// --- Compile-time ANSI RGB escape sequence generation ---

struct GradientStop {
    uint8_t r, g, b;
    double position;
};

constexpr std::array<uint8_t, 3> as_dec(uint8_t byte) {
    return {{
        static_cast<uint8_t>('0' + (byte / 100)),
        static_cast<uint8_t>('0' + ((byte % 100) / 10)),
        static_cast<uint8_t>('0' + (byte % 10)),
    }};
}

constexpr std::array<uint8_t, 19> rgb_bytes(uint8_t r, uint8_t g, uint8_t b) {
    std::array<uint8_t, 19> buf = {{
        '\x1b', '[', '3', '8', ';', '2', ';',
        0, 0, 0, ';', 0, 0, 0, ';', 0, 0, 0, 'm'
    }};
    auto rd = as_dec(r);
    buf[7] = rd[0]; buf[8] = rd[1]; buf[9] = rd[2];
    auto gd = as_dec(g);
    buf[11] = gd[0]; buf[12] = gd[1]; buf[13] = gd[2];
    auto bd = as_dec(b);
    buf[15] = bd[0]; buf[16] = bd[1]; buf[17] = bd[2];
    return buf;
}

template <std::size_t N, std::size_t S>
constexpr std::array<std::array<uint8_t, 19>, N>
generate_color_gradient(const std::array<GradientStop, S>& stops) {
    static_assert(S >= 2, "need at least two stops for the gradient");
    std::array<std::array<uint8_t, 19>, N> out{};
    for (std::size_t i = 0; i < N; ++i) {
        out[i] = rgb_bytes(0, 0, 0);
    }
    for (std::size_t byte = 0; byte < N; ++byte) {
        double relative_byte = static_cast<double>(byte) / static_cast<double>(N);
        std::size_t i = 1;
        while (i < S && stops[i].position < relative_byte) {
            ++i;
        }
        if (i >= S) {
            i = S - 1;
        }
        auto prev_stop = stops[i - 1];
        auto stop = stops[i];
        double diff = stop.position - prev_stop.position;
        double t = (relative_byte - prev_stop.position) / diff;
        auto r = static_cast<uint8_t>(prev_stop.r + t * (static_cast<double>(stop.r) - prev_stop.r));
        auto g = static_cast<uint8_t>(prev_stop.g + t * (static_cast<double>(stop.g) - prev_stop.g));
        auto b = static_cast<uint8_t>(prev_stop.b + t * (static_cast<double>(stop.b) - prev_stop.b));
        out[byte] = rgb_bytes(r, g, b);
    }
    return out;
}

// --- Compile-time color tables ---

inline constexpr auto COLOR_NULL_RGB = rgb_bytes(100, 100, 100);
inline constexpr auto COLOR_DEL = rgb_bytes(64, 128, 0);

inline constexpr auto COLOR_GRADIENT_NONASCII =
    generate_color_gradient<128>(std::array<GradientStop, 3>{{
        {255, 0, 0, 0.0}, {255, 255, 0, 0.66}, {255, 255, 255, 1.0}
    }});

inline constexpr auto COLOR_GRADIENT_ASCII_NONPRINTABLE =
    generate_color_gradient<31>(std::array<GradientStop, 2>{{
        {255, 0, 255, 0.0}, {128, 0, 255, 1.0}
    }});

inline constexpr auto COLOR_GRADIENT_ASCII_PRINTABLE =
    generate_color_gradient<95>(std::array<GradientStop, 2>{{
        {0, 128, 255, 0.0}, {0, 255, 128, 1.0}
    }});

// --- Runtime-initialized color strings (from env vars) ---

const std::string& color_null();
const std::string& color_offset();
const std::string& color_ascii_printable();
const std::string& color_ascii_whitespace();
const std::string& color_ascii_other();
const std::string& color_nonascii();

// ANSI reset: default foreground
inline constexpr std::string_view COLOR_RESET = "\x1b[39m";

// --- Code page tables ---

// CP437 character table (256 entries)
// Copyright (c) 2016, Delan Azabani - ISC License
inline constexpr char32_t CP437[256] = {
    U'⋄',U'☺',U'☻',U'♥',U'♦',U'♣',U'♠',U'•',U'◘',U'○',U'◙',U'♂',U'♀',U'♪',U'♫',U'☼',
    U'►',U'◄',U'↕',U'‼',U'¶',U'§',U'▬',U'↨',U'↑',U'↓',U'→',U'←',U'∟',U'↔',U'▲',U'▼',
    U' ',U'!',U'"',U'#',U'$',U'%',U'&',U'\'',U'(',U')',U'*',U'+',U',',U'-',U'.',U'/',
    U'0',U'1',U'2',U'3',U'4',U'5',U'6',U'7',U'8',U'9',U':',U';',U'<',U'=',U'>',U'?',
    U'@',U'A',U'B',U'C',U'D',U'E',U'F',U'G',U'H',U'I',U'J',U'K',U'L',U'M',U'N',U'O',
    U'P',U'Q',U'R',U'S',U'T',U'U',U'V',U'W',U'X',U'Y',U'Z',U'[',U'\\',U']',U'^',U'_',
    U'`',U'a',U'b',U'c',U'd',U'e',U'f',U'g',U'h',U'i',U'j',U'k',U'l',U'm',U'n',U'o',
    U'p',U'q',U'r',U's',U't',U'u',U'v',U'w',U'x',U'y',U'z',U'{',U'|',U'}',U'~',U'⌂',
    U'Ç',U'ü',U'é',U'â',U'ä',U'à',U'å',U'ç',U'ê',U'ë',U'è',U'ï',U'î',U'ì',U'Ä',U'Å',
    U'É',U'æ',U'Æ',U'ô',U'ö',U'ò',U'û',U'ù',U'ÿ',U'Ö',U'Ü',U'¢',U'£',U'¥',U'₧',U'ƒ',
    U'á',U'í',U'ó',U'ú',U'ñ',U'Ñ',U'ª',U'º',U'¿',U'⌐',U'¬',U'½',U'¼',U'¡',U'«',U'»',
    U'░',U'▒',U'▓',U'│',U'┤',U'╡',U'╢',U'╖',U'╕',U'╣',U'║',U'╗',U'╝',U'╜',U'╛',U'┐',
    U'└',U'┴',U'┬',U'├',U'─',U'┼',U'╞',U'╟',U'╚',U'╔',U'╩',U'╦',U'╠',U'═',U'╬',U'╧',
    U'╨',U'╤',U'╥',U'╙',U'╘',U'╒',U'╓',U'╫',U'╪',U'┘',U'┌',U'█',U'▄',U'▌',U'▐',U'▀',
    U'α',U'ß',U'Γ',U'π',U'Σ',U'σ',U'µ',U'τ',U'Φ',U'Θ',U'Ω',U'δ',U'∞',U'φ',U'ε',U'∩',
    U'≡',U'±',U'≥',U'≤',U'⌠',U'⌡',U'÷',U'≈',U'°',U'∙',U'·',U'√',U'ⁿ',U'²',U'■',U'ﬀ',
};

// CP1047 (EBCDIC) character table
inline constexpr char CP1047[256] = {
    '.','.','.','.','.','.','.','.','.','.','.','.','.','.','.','.',
    '.','.','.','.','.','.','.','.','.','.','.','.','.','.','.','.',
    '.','.','.','.','.','.','.','.','.','.','.','.','.','.','.','.',
    '.','.','.','.','.','.','.','.','.','.','.','.','.','.','.','.',
    ' ','.','.','.','.','.','.','.','.','.','$','.','<','(','+','|',
    '&','.','.','.','.','.','.','.','.','.','!','$','*',')',';','.',
    '-','/','.','.','.','.','.','.','.','.','.',',','%','_','>','?',
    '.','.','.','.','.','.','.','.','.','.',':','#','@','\'','=','.',
    '.','a','b','c','d','e','f','g','h','i','.','{','.','(','+','.',
    '.','j','k','l','m','n','o','p','q','r','.','}','.',')','.','.',
    '.','~','s','t','u','v','w','x','y','z','.','.','.','.','.','.',
    '.','.','.','.','.','.','.','.','.','.','[',']','.','.','.','-',
    '{','A','B','C','D','E','F','G','H','I','.','.','.','.','.','.',
    '}','J','K','L','M','N','O','P','Q','R','.','.','.','.','.','.',
    '.','.','S','T','U','V','W','X','Y','Z','.','.','.','.','.','.',
    '0','1','2','3','4','5','6','7','8','9','.','.','.','.','.','.',
};

// Encode a Unicode code point to UTF-8, appending to the given string
void encode_utf8(char32_t cp, std::string& out);

// Get the UTF-8 string for a single char32_t
std::string to_utf8(char32_t cp);

} // namespace hexyl
