/// Color type for terminal color handling.
/// Transpiled from: fish-shell src/color.rs (tag 4.0.0)
///
/// Key translation decisions:
///   Rust enum Type { None, Named{idx}, Rgb(Color24), Normal, Reset }
///     -> C++ enum class + tagged union (same data layout as original fish C++)
///   bitflags! Flags -> uint8_t with constexpr flag constants
///   RgbColor::from_wstr -> std::optional<RgbColor> (Rust's Option<Self>)
///   &wstr -> std::wstring_view
///   assert_sorted_by_name! -> static_assert at compile time (not enforced here)
///   binary_search_by -> std::lower_bound
///   simple_icase_compare -> free function (case-insensitive wide string comparison)

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/// 24-bit RGB color.
struct Color24 {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;

    /// Unpack from a 24-bit integer (0x00RRGGBB).
    static Color24 from_bits(uint32_t bits);

    bool operator==(const Color24&) const = default;
};

/// Color type flags (bold, underline, etc.).
namespace ColorFlag {
    inline constexpr uint8_t None      = 0;
    inline constexpr uint8_t Bold      = 1 << 0;
    inline constexpr uint8_t Underline = 1 << 1;
    inline constexpr uint8_t Italics   = 1 << 2;
    inline constexpr uint8_t Dim       = 1 << 3;
    inline constexpr uint8_t Reverse   = 1 << 4;
}

/// Represents a terminal color: none, named (0-15), RGB, normal, or reset.
class RgbColor {
public:
    /// Color type tag.
    enum class Type : uint8_t { None, Named, Rgb, Normal, Reset };

    // ---- Construction ----

    /// Default: type none.
    RgbColor() = default;

    /// Parse a color from a wide string.
    /// Returns nullopt if the string is not a recognized color.
    static std::optional<RgbColor> from_wstr(std::wstring_view s);

    /// Create an RGB color.
    static RgbColor from_rgb(uint8_t r, uint8_t g, uint8_t b);

    // ---- Named constants ----
    static RgbColor white();
    static RgbColor black();
    static RgbColor reset();
    static RgbColor normal();
    static RgbColor none();

    // ---- Type queries ----
    bool is_normal() const { return type_ == Type::Normal; }
    bool is_reset()  const { return type_ == Type::Reset; }
    bool is_none()   const { return type_ == Type::None; }
    bool is_named()  const { return type_ == Type::Named; }
    bool is_rgb()    const { return type_ == Type::Rgb; }
    bool is_special() const { return !is_named() && !is_rgb(); }

    // ---- Flag queries and mutators ----
    bool is_bold()      const { return (flags_ & ColorFlag::Bold) != 0; }
    bool is_underline() const { return (flags_ & ColorFlag::Underline) != 0; }
    bool is_italics()   const { return (flags_ & ColorFlag::Italics) != 0; }
    bool is_dim()       const { return (flags_ & ColorFlag::Dim) != 0; }
    bool is_reverse()   const { return (flags_ & ColorFlag::Reverse) != 0; }

    void set_bold(bool v)      { set_flag(ColorFlag::Bold, v); }
    void set_underline(bool v) { set_flag(ColorFlag::Underline, v); }
    void set_italics(bool v)   { set_flag(ColorFlag::Italics, v); }
    void set_dim(bool v)       { set_flag(ColorFlag::Dim, v); }
    void set_reverse(bool v)   { set_flag(ColorFlag::Reverse, v); }

    // ---- Conversion ----

    /// Returns the name index (0-15). Requires named or RGB.
    uint8_t to_name_index() const;

    /// Returns the term256 palette index. Requires RGB.
    uint8_t to_term256_index() const;

    /// Returns the 24-bit color. Requires RGB.
    Color24 to_color24() const;

    // ---- Utility ----

    /// Returns the names of all non-hidden named colors, plus "normal".
    static std::vector<std::wstring_view> named_color_names();

    bool operator==(const RgbColor& o) const {
        return type_ == o.type_ && flags_ == o.flags_ &&
               data_.name_idx == o.data_.name_idx; // union covers both cases
    }
    bool operator!=(const RgbColor& o) const { return !(*this == o); }

private:
    Type type_ = Type::None;
    uint8_t flags_ = 0;
    union Data {
        uint8_t name_idx;
        Color24 color;
        Data() : name_idx(0) {}
    } data_;

    void set_flag(uint8_t flag, bool v) {
        if (v) flags_ |= flag; else flags_ &= ~flag;
    }

    static std::optional<RgbColor> try_parse_special(std::wstring_view s);
    static std::optional<RgbColor> try_parse_named(std::wstring_view s);
    static std::optional<RgbColor> try_parse_rgb(std::wstring_view s);
};
