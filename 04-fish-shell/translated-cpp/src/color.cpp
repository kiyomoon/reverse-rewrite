/// Color type implementation.
/// Transpiled from: fish-shell src/color.rs (tag 4.0.0)

#include "color.hpp"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdlib>
#include <cwctype>
#include <iterator>

// ---- Color24 ----

Color24 Color24::from_bits(uint32_t bits) {
    assert((bits >> 24) == 0 && "from_bits() called with non-zero high byte");
    return {
        static_cast<uint8_t>((bits >> 16) & 0xFF),
        static_cast<uint8_t>((bits >> 8) & 0xFF),
        static_cast<uint8_t>(bits & 0xFF),
    };
}

// ---- Case-insensitive wide string comparison ----

/// Compare wide strings with simple ASCII canonicalization.
/// Returns <0, 0, or >0 (like strcmp).
static int simple_icase_compare(std::wstring_view s1, std::wstring_view s2) {
    auto it1 = s1.begin(), it2 = s2.begin();
    for (; it1 != s1.end() && it2 != s2.end(); ++it1, ++it2) {
        wchar_t c1 = *it1, c2 = *it2;
        if (L'A' <= c1 && c1 <= L'Z') c1 = L'a' + (c1 - L'A');
        if (L'A' <= c2 && c2 <= L'Z') c2 = L'a' + (c2 - L'A');
        if (c1 != c2) return c1 < c2 ? -1 : 1;
    }
    if (it1 != s1.end()) return 1;
    if (it2 != s2.end()) return -1;
    return 0;
}

// ---- Named color table ----

struct NamedColor {
    const wchar_t* name;
    uint8_t idx;
    uint8_t rgb[3];
    bool hidden;
};

// Keep this sorted alphabetically by name.
static constexpr NamedColor named_colors[] = {
    {L"black",     0,  {0x00, 0x00, 0x00}, false},
    {L"blue",      4,  {0x00, 0x00, 0x80}, false},
    {L"brblack",   8,  {0x80, 0x80, 0x80}, false},
    {L"brblue",    12, {0x00, 0x00, 0xFF}, false},
    {L"brbrown",   11, {0xFF, 0xFF, 0x00}, true},
    {L"brcyan",    14, {0x00, 0xFF, 0xFF}, false},
    {L"brgreen",   10, {0x00, 0xFF, 0x00}, false},
    {L"brgrey",    8,  {0x55, 0x55, 0x55}, true},
    {L"brmagenta", 13, {0xFF, 0x00, 0xFF}, false},
    {L"brown",     3,  {0x72, 0x50, 0x00}, true},
    {L"brpurple",  13, {0xFF, 0x00, 0xFF}, true},
    {L"brred",     9,  {0xFF, 0x00, 0x00}, false},
    {L"brwhite",   15, {0xFF, 0xFF, 0xFF}, false},
    {L"bryellow",  11, {0xFF, 0xFF, 0x00}, false},
    {L"cyan",      6,  {0x00, 0x80, 0x80}, false},
    {L"green",     2,  {0x00, 0x80, 0x00}, false},
    {L"grey",      7,  {0xE5, 0xE5, 0xE5}, true},
    {L"magenta",   5,  {0x80, 0x00, 0x80}, false},
    {L"purple",    5,  {0x80, 0x00, 0x80}, true},
    {L"red",       1,  {0x80, 0x00, 0x00}, false},
    {L"white",     7,  {0xC0, 0xC0, 0xC0}, false},
    {L"yellow",    3,  {0x80, 0x80, 0x00}, false},
};

static constexpr size_t named_color_count = std::size(named_colors);

// ---- Color distance ----

static unsigned squared_difference(uint8_t a, uint8_t b) {
    unsigned diff = (a > b) ? (a - b) : (b - a);
    return diff * diff;
}

static size_t convert_color(Color24 color, const uint32_t* palette, size_t count) {
    unsigned best_distance = ~0u;
    size_t best_index = 0;

    for (size_t i = 0; i < count; ++i) {
        Color24 c = Color24::from_bits(palette[i]);
        unsigned distance = squared_difference(color.r, c.r)
                          + squared_difference(color.g, c.g)
                          + squared_difference(color.b, c.b);
        if (distance <= best_distance) {
            best_distance = distance;
            best_index = i;
        }
    }
    return best_index;
}

static uint8_t term16_color_for_rgb(Color24 color) {
    static constexpr uint32_t colors[] = {
        0x000000, // Black
        0x800000, // Red
        0x008000, // Green
        0x808000, // Yellow
        0x000080, // Blue
        0x800080, // Magenta
        0x008080, // Cyan
        0xc0c0c0, // White
        0x808080, // Bright Black
        0xFF0000, // Bright Red
        0x00FF00, // Bright Green
        0xFFFF00, // Bright Yellow
        0x0000FF, // Bright Blue
        0xFF00FF, // Bright Magenta
        0x00FFFF, // Bright Cyan
        0xFFFFFF, // Bright White
    };
    return static_cast<uint8_t>(convert_color(color, colors, std::size(colors)));
}

static uint8_t term256_color_for_rgb(Color24 color) {
    static constexpr uint32_t colors[] = {
        0x000000, 0x00005f, 0x000087, 0x0000af, 0x0000d7, 0x0000ff,
        0x005f00, 0x005f5f, 0x005f87, 0x005faf, 0x005fd7, 0x005fff,
        0x008700, 0x00875f, 0x008787, 0x0087af, 0x0087d7, 0x0087ff,
        0x00af00, 0x00af5f, 0x00af87, 0x00afaf, 0x00afd7, 0x00afff,
        0x00d700, 0x00d75f, 0x00d787, 0x00d7af, 0x00d7d7, 0x00d7ff,
        0x00ff00, 0x00ff5f, 0x00ff87, 0x00ffaf, 0x00ffd7, 0x00ffff,
        0x5f0000, 0x5f005f, 0x5f0087, 0x5f00af, 0x5f00d7, 0x5f00ff,
        0x5f5f00, 0x5f5f5f, 0x5f5f87, 0x5f5faf, 0x5f5fd7, 0x5f5fff,
        0x5f8700, 0x5f875f, 0x5f8787, 0x5f87af, 0x5f87d7, 0x5f87ff,
        0x5faf00, 0x5faf5f, 0x5faf87, 0x5fafaf, 0x5fafd7, 0x5fafff,
        0x5fd700, 0x5fd75f, 0x5fd787, 0x5fd7af, 0x5fd7d7, 0x5fd7ff,
        0x5fff00, 0x5fff5f, 0x5fff87, 0x5fffaf, 0x5fffd7, 0x5fffff,
        0x870000, 0x87005f, 0x870087, 0x8700af, 0x8700d7, 0x8700ff,
        0x875f00, 0x875f5f, 0x875f87, 0x875faf, 0x875fd7, 0x875fff,
        0x878700, 0x87875f, 0x878787, 0x8787af, 0x8787d7, 0x8787ff,
        0x87af00, 0x87af5f, 0x87af87, 0x87afaf, 0x87afd7, 0x87afff,
        0x87d700, 0x87d75f, 0x87d787, 0x87d7af, 0x87d7d7, 0x87d7ff,
        0x87ff00, 0x87ff5f, 0x87ff87, 0x87ffaf, 0x87ffd7, 0x87ffff,
        0xaf0000, 0xaf005f, 0xaf0087, 0xaf00af, 0xaf00d7, 0xaf00ff,
        0xaf5f00, 0xaf5f5f, 0xaf5f87, 0xaf5faf, 0xaf5fd7, 0xaf5fff,
        0xaf8700, 0xaf875f, 0xaf8787, 0xaf87af, 0xaf87d7, 0xaf87ff,
        0xafaf00, 0xafaf5f, 0xafaf87, 0xafafaf, 0xafafd7, 0xafafff,
        0xafd700, 0xafd75f, 0xafd787, 0xafd7af, 0xafd7d7, 0xafd7ff,
        0xafff00, 0xafff5f, 0xafff87, 0xafffaf, 0xafffd7, 0xafffff,
        0xd70000, 0xd7005f, 0xd70087, 0xd700af, 0xd700d7, 0xd700ff,
        0xd75f00, 0xd75f5f, 0xd75f87, 0xd75faf, 0xd75fd7, 0xd75fff,
        0xd78700, 0xd7875f, 0xd78787, 0xd787af, 0xd787d7, 0xd787ff,
        0xd7af00, 0xd7af5f, 0xd7af87, 0xd7afaf, 0xd7afd7, 0xd7afff,
        0xd7d700, 0xd7d75f, 0xd7d787, 0xd7d7af, 0xd7d7d7, 0xd7d7ff,
        0xd7ff00, 0xd7ff5f, 0xd7ff87, 0xd7ffaf, 0xd7ffd7, 0xd7ffff,
        0xff0000, 0xff005f, 0xff0087, 0xff00af, 0xff00d7, 0xff00ff,
        0xff5f00, 0xff5f5f, 0xff5f87, 0xff5faf, 0xff5fd7, 0xff5fff,
        0xff8700, 0xff875f, 0xff8787, 0xff87af, 0xff87d7, 0xff87ff,
        0xffaf00, 0xffaf5f, 0xffaf87, 0xffafaf, 0xffafd7, 0xffafff,
        0xffd700, 0xffd75f, 0xffd787, 0xffd7af, 0xffd7d7, 0xffd7ff,
        0xffff00, 0xffff5f, 0xffff87, 0xffffaf, 0xffffd7, 0xffffff,
        0x080808, 0x121212, 0x1c1c1c, 0x262626, 0x303030, 0x3a3a3a,
        0x444444, 0x4e4e4e, 0x585858, 0x626262, 0x6c6c6c, 0x767676,
        0x808080, 0x8a8a8a, 0x949494, 0x9e9e9e, 0xa8a8a8, 0xb2b2b2,
        0xbcbcbc, 0xc6c6c6, 0xd0d0d0, 0xdadada, 0xe4e4e4, 0xeeeeee,
    };
    return static_cast<uint8_t>(16 + convert_color(color, colors, std::size(colors)));
}

// ---- RgbColor construction ----

std::optional<RgbColor> RgbColor::from_wstr(std::wstring_view s) {
    if (auto c = try_parse_special(s)) return c;
    if (auto c = try_parse_named(s)) return c;
    if (auto c = try_parse_rgb(s)) return c;
    return std::nullopt;
}

RgbColor RgbColor::from_rgb(uint8_t r, uint8_t g, uint8_t b) {
    RgbColor c;
    c.type_ = Type::Rgb;
    c.data_.color = {r, g, b};
    return c;
}

// ---- Named constants ----

RgbColor RgbColor::white() {
    RgbColor c;
    c.type_ = Type::Named;
    c.data_.name_idx = 7;
    return c;
}

RgbColor RgbColor::black() {
    RgbColor c;
    c.type_ = Type::Named;
    c.data_.name_idx = 0;
    return c;
}

RgbColor RgbColor::reset() {
    RgbColor c;
    c.type_ = Type::Reset;
    return c;
}

RgbColor RgbColor::normal() {
    RgbColor c;
    c.type_ = Type::Normal;
    return c;
}

RgbColor RgbColor::none() {
    return {};
}

// ---- Conversion ----

uint8_t RgbColor::to_name_index() const {
    assert(type_ == Type::Named || type_ == Type::Rgb);
    if (type_ == Type::Named) return data_.name_idx;
    return term16_color_for_rgb(data_.color);
}

uint8_t RgbColor::to_term256_index() const {
    assert(type_ == Type::Rgb);
    return term256_color_for_rgb(data_.color);
}

Color24 RgbColor::to_color24() const {
    assert(type_ == Type::Rgb);
    return data_.color;
}

// ---- named_color_names ----

std::vector<std::wstring_view> RgbColor::named_color_names() {
    std::vector<std::wstring_view> result;
    result.reserve(named_color_count + 1);
    for (const auto& nc : named_colors) {
        if (!nc.hidden) result.emplace_back(nc.name);
    }
    // "normal" isn't really a color and does not have a color palette index or
    // RGB value. Therefore, it does not appear in the named_colors table.
    // However, it is a legitimate color name for the "set_color" command so
    // include it in the publicly known list of colors.
    result.emplace_back(L"normal");
    return result;
}

// ---- Parsing ----

std::optional<RgbColor> RgbColor::try_parse_special(std::wstring_view s) {
    RgbColor c;
    if (simple_icase_compare(s, L"normal") == 0) {
        c.type_ = Type::Normal;
    } else if (simple_icase_compare(s, L"reset") == 0) {
        c.type_ = Type::Reset;
    } else {
        return std::nullopt;
    }
    return c;
}

std::optional<RgbColor> RgbColor::try_parse_rgb(std::wstring_view s) {
    // Skip one leading '#'.
    if (s.empty()) return std::nullopt;
    if (s[0] == L'#') s.remove_prefix(1);

    auto hex_digit = [](wchar_t ch) -> int {
        if (ch >= L'0' && ch <= L'9') return ch - L'0';
        if (ch >= L'a' && ch <= L'f') return 10 + (ch - L'a');
        if (ch >= L'A' && ch <= L'F') return 10 + (ch - L'A');
        return -1;
    };

    uint8_t r, g, b;
    if (s.size() == 3) {
        // Format: FA3 -> FFAA33
        int d0 = hex_digit(s[0]), d1 = hex_digit(s[1]), d2 = hex_digit(s[2]);
        if (d0 < 0 || d1 < 0 || d2 < 0) return std::nullopt;
        r = static_cast<uint8_t>(d0 * 16 + d0);
        g = static_cast<uint8_t>(d1 * 16 + d1);
        b = static_cast<uint8_t>(d2 * 16 + d2);
    } else if (s.size() == 6) {
        // Format: F3A035
        int d0 = hex_digit(s[0]), d1 = hex_digit(s[1]);
        int d2 = hex_digit(s[2]), d3 = hex_digit(s[3]);
        int d4 = hex_digit(s[4]), d5 = hex_digit(s[5]);
        if (d0 < 0 || d1 < 0 || d2 < 0 || d3 < 0 || d4 < 0 || d5 < 0)
            return std::nullopt;
        r = static_cast<uint8_t>(d0 * 16 + d1);
        g = static_cast<uint8_t>(d2 * 16 + d3);
        b = static_cast<uint8_t>(d4 * 16 + d5);
    } else {
        return std::nullopt;
    }
    return from_rgb(r, g, b);
}

std::optional<RgbColor> RgbColor::try_parse_named(std::wstring_view name) {
    if (name.empty()) return std::nullopt;

    // Binary search with case-insensitive comparison.
    auto it = std::lower_bound(
        std::begin(named_colors), std::end(named_colors), name,
        [](const NamedColor& nc, std::wstring_view n) {
            return simple_icase_compare(nc.name, n) < 0;
        });

    if (it != std::end(named_colors) &&
        simple_icase_compare(it->name, name) == 0) {
        RgbColor c;
        c.type_ = Type::Named;
        c.data_.name_idx = it->idx;
        return c;
    }
    return std::nullopt;
}
