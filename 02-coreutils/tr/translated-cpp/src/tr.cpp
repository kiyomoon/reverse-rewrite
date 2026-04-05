// tr.cpp — Transpiled from uutils/coreutils tr to idiomatic C++23
//
// Three-way comparison: GNU C (tr.c) → Rust (tr.rs + operation.rs) → C++ (this file)
//
// Key translation decisions:
//   Rust nom parser combinators     → C++ hand-rolled recursive descent parser
//   Rust Sequence enum              → C++ tagged struct with flatten()
//   Rust Class enum                 → C++ enum class + expand function
//   Rust SymbolTranslator trait     → C++ std::optional<uint8_t> transform functions
//   Rust ChunkProcessor trait       → C++ direct loop (skip SIMD optimization)
//   Rust ChainedSymbolTranslator    → C++ inline chaining in dispatch code
//   Rust bytecount crate (SIMD)     → Omitted (standard loop; compiler auto-vectorizes)
//   Rust [bool; 256] / [u8; 256]   → std::array<bool, 256> / std::array<uint8_t, 256>
//   Rust clap arg parsing           → C++ getopt_long
//   Rust BadSequence error enum     → C++ error string + early exit

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <getopt.h>
#include <unistd.h>

// =====================================================================
// Constants
// =====================================================================

static constexpr std::size_t BUF_SIZE = 1024 * 32;
static const char* program_name = "tr";

// =====================================================================
// POSIX character classes
// =====================================================================

enum class CharClass {
    Alnum, Alpha, Blank, Control, Digit, Graph,
    Lower, Print, Punct, Space, Upper, Xdigit
};

static bool parse_class_name(std::string_view name, CharClass& out) {
    struct Entry { std::string_view name; CharClass cls; };
    static constexpr Entry entries[] = {
        {"alnum", CharClass::Alnum},   {"alpha", CharClass::Alpha},
        {"blank", CharClass::Blank},   {"cntrl", CharClass::Control},
        {"digit", CharClass::Digit},   {"graph", CharClass::Graph},
        {"lower", CharClass::Lower},   {"print", CharClass::Print},
        {"punct", CharClass::Punct},   {"space", CharClass::Space},
        {"upper", CharClass::Upper},   {"xdigit", CharClass::Xdigit},
    };
    for (auto& e : entries) {
        if (e.name == name) { out = e.cls; return true; }
    }
    return false;
}

/// Expand a POSIX character class into its constituent bytes.
/// Order follows the C locale, matching Rust's Class::iter().
static void expand_class(CharClass cls, std::vector<uint8_t>& out) {
    auto push_range = [&](uint8_t lo, uint8_t hi) {
        for (int c = lo; c <= hi; ++c) out.push_back(static_cast<uint8_t>(c));
    };
    switch (cls) {
        case CharClass::Alnum:
            push_range('0', '9'); push_range('A', 'Z'); push_range('a', 'z'); break;
        case CharClass::Alpha:
            push_range('A', 'Z'); push_range('a', 'z'); break;
        case CharClass::Blank:
            out.push_back('\t'); out.push_back(' '); break;
        case CharClass::Control:
            push_range(0, 31); out.push_back(127); break;
        case CharClass::Digit:
            push_range('0', '9'); break;
        case CharClass::Graph:
            push_range(0x21, 0x7E); break;
        case CharClass::Lower:
            push_range('a', 'z'); break;
        case CharClass::Print:
            push_range(0x20, 0x7E); break;
        case CharClass::Punct:
            push_range(0x21, 0x2F); push_range(0x3A, 0x40);
            push_range(0x5B, 0x60); push_range(0x7B, 0x7E); break;
        case CharClass::Space:
            out.push_back('\t'); out.push_back('\n'); out.push_back('\x0B');
            out.push_back('\f'); out.push_back('\r'); out.push_back(' '); break;
        case CharClass::Upper:
            push_range('A', 'Z'); break;
        case CharClass::Xdigit:
            push_range('0', '9'); push_range('A', 'F'); push_range('a', 'f'); break;
    }
}

// =====================================================================
// Sequence — parsed element of a set specification
// =====================================================================

enum class SeqKind { Char, Range, Star, Repeat, Class };

struct Sequence {
    SeqKind kind;
    uint8_t ch;         // Char, Star, Repeat, Range-start
    uint8_t ch2;        // Range-end
    std::size_t count;  // Repeat count
    CharClass cls;      // Class

    static Sequence make_char(uint8_t c) { return {SeqKind::Char, c, 0, 0, {}}; }
    static Sequence make_range(uint8_t lo, uint8_t hi) { return {SeqKind::Range, lo, hi, 0, {}}; }
    static Sequence make_star(uint8_t c) { return {SeqKind::Star, c, 0, 0, {}}; }
    static Sequence make_repeat(uint8_t c, std::size_t n) { return {SeqKind::Repeat, c, 0, n, {}}; }
    static Sequence make_class(CharClass cl) { return {SeqKind::Class, 0, 0, 0, cl}; }

    /// Expand this sequence into individual bytes.
    void flatten(std::vector<uint8_t>& out) const {
        switch (kind) {
            case SeqKind::Char:
                out.push_back(ch); break;
            case SeqKind::Range:
                for (int c = ch; c <= ch2; ++c) out.push_back(static_cast<uint8_t>(c)); break;
            case SeqKind::Star:
                out.push_back(ch); break;  // Star expansion is handled separately
            case SeqKind::Repeat:
                for (std::size_t i = 0; i < count; ++i) out.push_back(ch); break;
            case SeqKind::Class:
                expand_class(cls, out); break;
        }
    }
};

// =====================================================================
// Set specification parser
// =====================================================================
// Replaces Rust's nom-based parser with hand-rolled recursive descent.
// Handles: literals, escapes, ranges (a-z), classes ([:alpha:]),
// equivalences ([=c=]), repeats ([c*n], [c*]).

/// Parse a single character, handling backslash escape sequences.
/// Matches Rust's parse_backslash_or_char.
static uint8_t parse_one_char(const char* s, std::size_t& pos, std::size_t len) {
    if (s[pos] != '\\' || pos + 1 >= len) {
        return static_cast<uint8_t>(s[pos++]);
    }
    ++pos;  // skip backslash
    char c = s[pos++];
    switch (c) {
        case 'a':  return '\a';
        case 'b':  return '\b';
        case 'f':  return '\f';
        case 'n':  return '\n';
        case 'r':  return '\r';
        case 't':  return '\t';
        case 'v':  return '\v';
        case '\\': return '\\';
        case '0': case '1': case '2': case '3':
        case '4': case '5': case '6': case '7': {
            // Octal: 1-3 digits (first digit already consumed)
            unsigned val = static_cast<unsigned>(c - '0');
            for (int i = 0; i < 2 && pos < len && s[pos] >= '0' && s[pos] <= '7'; ++i) {
                val = val * 8 + static_cast<unsigned>(s[pos++] - '0');
            }
            return static_cast<uint8_t>(val & 0xFF);
        }
        case 'x': {
            // Hex: 1-2 digits
            auto hex_val = [](char ch) -> int {
                if (ch >= '0' && ch <= '9') return ch - '0';
                if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
                if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
                return -1;
            };
            if (pos < len) {
                int h = hex_val(s[pos]);
                if (h >= 0) {
                    unsigned val = static_cast<unsigned>(h);
                    ++pos;
                    if (pos < len) {
                        h = hex_val(s[pos]);
                        if (h >= 0) { val = val * 16 + static_cast<unsigned>(h); ++pos; }
                    }
                    return static_cast<uint8_t>(val);
                }
            }
            // Invalid hex — treat as literal 'x' (backslash already consumed)
            return 'x';
        }
        default:
            // Unknown escape — return the character after backslash
            return static_cast<uint8_t>(c);
    }
}

/// Try to parse a bracket expression starting at s[pos] == '['.
/// Returns true if a bracket expression was successfully parsed.
static bool try_parse_bracket(const char* s, std::size_t& pos, std::size_t len,
                               std::vector<Sequence>& out, std::string& /*error*/) {
    if (pos >= len || s[pos] != '[') return false;

    // --- Try [:classname:] ---
    if (pos + 2 < len && s[pos + 1] == ':') {
        // Find closing ":]"
        auto close = std::string_view(s + pos + 2, len - pos - 2).find(":]");
        if (close != std::string_view::npos) {
            auto name = std::string_view(s + pos + 2, close);
            CharClass cls;
            if (parse_class_name(name, cls)) {
                out.push_back(Sequence::make_class(cls));
                pos += 2 + close + 2;  // skip [: + name + :]
                return true;
            }
        }
    }

    // --- Try [=c=] ---
    if (pos + 4 <= len && s[pos + 1] == '=') {
        // Equivalence class: [=X=]
        if (pos + 4 < len && s[pos + 3] == '=' && s[pos + 4] == ']') {
            out.push_back(Sequence::make_char(static_cast<uint8_t>(s[pos + 2])));
            pos += 5;
            return true;
        }
    }

    // --- Try [c*] or [c*n] ---
    if (pos + 3 <= len) {
        std::size_t saved = pos + 1;  // after '['
        if (saved < len) {
            uint8_t c = parse_one_char(s, saved, len);
            if (saved < len && s[saved] == '*') {
                ++saved;  // skip '*'
                if (saved < len && s[saved] == ']') {
                    // [c*] — star (expand to fill)
                    out.push_back(Sequence::make_star(c));
                    pos = saved + 1;
                    return true;
                }
                // [c*n] — repeat with count
                std::size_t num_start = saved;
                if (saved < len && s[saved] == '0') {
                    // Octal count
                    ++saved;
                    std::size_t oct_start = saved;
                    while (saved < len && s[saved] >= '0' && s[saved] <= '7') ++saved;
                    if (saved < len && s[saved] == ']') {
                        std::size_t count = 0;
                        for (std::size_t i = oct_start; i < saved; ++i) {
                            count = count * 8 + static_cast<std::size_t>(s[i] - '0');
                        }
                        out.push_back(Sequence::make_repeat(c, count));
                        pos = saved + 1;
                        return true;
                    }
                } else {
                    // Decimal count
                    while (saved < len && s[saved] >= '0' && s[saved] <= '9') ++saved;
                    if (saved > num_start && saved < len && s[saved] == ']') {
                        std::size_t count = 0;
                        for (std::size_t i = num_start; i < saved; ++i) {
                            count = count * 10 + static_cast<std::size_t>(s[i] - '0');
                        }
                        out.push_back(Sequence::make_repeat(c, count));
                        pos = saved + 1;
                        return true;
                    }
                }
            }
        }
    }

    // No bracket expression matched — '[' will be treated as literal
    return false;
}

/// Parse a complete set specification string into Sequence elements.
static bool parse_set(const char* s, std::vector<Sequence>& out, std::string& error) {
    std::size_t len = std::strlen(s);
    std::size_t pos = 0;

    while (pos < len) {
        // Try bracket expressions first
        if (s[pos] == '[' && try_parse_bracket(s, pos, len, out, error)) {
            if (!error.empty()) return false;
            continue;
        }

        // Parse a single character (possibly escaped)
        uint8_t c = parse_one_char(s, pos, len);

        // Check for range: c-d
        if (pos < len && s[pos] == '-' && pos + 1 < len) {
            ++pos;  // skip '-'
            uint8_t d = parse_one_char(s, pos, len);
            if (d >= c) {
                out.push_back(Sequence::make_range(c, d));
            } else {
                error = "range-endpoints of '" + std::string(1, static_cast<char>(c))
                      + "-" + std::string(1, static_cast<char>(d))
                      + "' are in reverse collating sequence order";
                return false;
            }
        } else {
            out.push_back(Sequence::make_char(c));
        }
    }
    return true;
}

// =====================================================================
// Set resolution — flatten, complement, pad/truncate
// =====================================================================

/// Flatten a parsed set into a vector of individual bytes.
static std::vector<uint8_t> flatten_set(const std::vector<Sequence>& seqs) {
    std::vector<uint8_t> result;
    for (auto& seq : seqs) {
        seq.flatten(result);
    }
    return result;
}

/// Compute the complement of a byte set (all 256 bytes not in the set).
static std::vector<uint8_t> complement_set(const std::vector<uint8_t>& set) {
    std::array<bool, 256> present{};
    for (uint8_t b : set) present[b] = true;
    std::vector<uint8_t> result;
    for (int i = 0; i < 256; ++i) {
        if (!present[static_cast<std::size_t>(i)])
            result.push_back(static_cast<uint8_t>(i));
    }
    return result;
}

/// Resolve set1 and set2 from their string specifications.
/// Handles complement, star expansion, and set2 padding/truncation.
static bool resolve_sets(const char* set1_str, const char* set2_str,
                          bool complement_flag, bool truncate_flag,
                          bool translating,
                          std::vector<uint8_t>& set1_out,
                          std::vector<uint8_t>& set2_out,
                          std::string& error) {
    // Parse set1
    std::vector<Sequence> seqs1;
    if (!parse_set(set1_str, seqs1, error)) return false;

    // Parse set2 (if provided)
    std::vector<Sequence> seqs2;
    if (set2_str && !parse_set(set2_str, seqs2, error)) return false;

    // Flatten set1 and apply complement
    set1_out = flatten_set(seqs1);
    if (complement_flag) {
        set1_out = complement_set(set1_out);
    }

    // Handle star expansion in set2:
    // [c*] expands to fill remaining needed length
    auto star_it = std::find_if(seqs2.begin(), seqs2.end(),
        [](const Sequence& s) { return s.kind == SeqKind::Star; });

    if (star_it != seqs2.end()) {
        // Flatten parts before and after the star
        std::vector<uint8_t> before, after;
        for (auto it = seqs2.begin(); it != star_it; ++it) it->flatten(before);
        for (auto it = star_it + 1; it != seqs2.end(); ++it) it->flatten(after);

        uint8_t star_char = star_it->ch;
        std::size_t needed = 0;
        if (set1_out.size() > before.size() + after.size()) {
            needed = set1_out.size() - before.size() - after.size();
        }

        set2_out = std::move(before);
        set2_out.insert(set2_out.end(), needed, star_char);
        set2_out.insert(set2_out.end(), after.begin(), after.end());
    } else {
        set2_out = flatten_set(seqs2);
    }

    // Pad or truncate set2 for translate mode
    if (translating) {
        if (truncate_flag) {
            if (set2_out.size() > set1_out.size()) {
                set2_out.resize(set1_out.size());
            }
        } else if (!set2_out.empty() && set1_out.size() > set2_out.size()) {
            // Pad set2 with its last character
            set2_out.resize(set1_out.size(), set2_out.back());
        }
    }

    return true;
}

// =====================================================================
// Operations — 256-element lookup tables
// =====================================================================

/// Build a deletion table: table[c] == true means delete byte c.
static std::array<bool, 256> build_delete_table(const std::vector<uint8_t>& set) {
    std::array<bool, 256> table{};
    for (uint8_t b : set) table[b] = true;
    return table;
}

/// Build a translation table: table[c] gives the replacement for byte c.
static std::array<uint8_t, 256> build_translate_table(
    const std::vector<uint8_t>& set1, const std::vector<uint8_t>& set2) {
    // Identity mapping
    std::array<uint8_t, 256> table;
    for (int i = 0; i < 256; ++i) table[static_cast<std::size_t>(i)] = static_cast<uint8_t>(i);
    // Apply translations (later entries override earlier ones for same byte)
    std::size_t n = std::min(set1.size(), set2.size());
    for (std::size_t i = 0; i < n; ++i) {
        table[set1[i]] = set2[i];
    }
    return table;
}

/// Build a squeeze table: table[c] == true means squeeze consecutive c's.
static std::array<bool, 256> build_squeeze_table(const std::vector<uint8_t>& set) {
    std::array<bool, 256> table{};
    for (uint8_t b : set) table[b] = true;
    return table;
}

// =====================================================================
// I/O processing
// =====================================================================

/// Delete bytes matching the table, reading from stdin, writing to stdout.
static void process_delete(const std::array<bool, 256>& del_table) {
    uint8_t in_buf[BUF_SIZE];
    uint8_t out_buf[BUF_SIZE];
    for (;;) {
        auto n = std::fread(in_buf, 1, BUF_SIZE, stdin);
        if (n == 0) break;
        std::size_t out_pos = 0;
        for (std::size_t i = 0; i < n; ++i) {
            if (!del_table[in_buf[i]]) {
                out_buf[out_pos++] = in_buf[i];
            }
        }
        if (out_pos > 0) std::fwrite(out_buf, 1, out_pos, stdout);
    }
}

/// Translate bytes using the table, reading from stdin, writing to stdout.
static void process_translate(const std::array<uint8_t, 256>& trans_table) {
    uint8_t buf[BUF_SIZE];
    for (;;) {
        auto n = std::fread(buf, 1, BUF_SIZE, stdin);
        if (n == 0) break;
        for (std::size_t i = 0; i < n; ++i) {
            buf[i] = trans_table[buf[i]];
        }
        std::fwrite(buf, 1, n, stdout);
    }
}

/// Squeeze consecutive duplicates of bytes in the squeeze table.
static void process_squeeze(const std::array<bool, 256>& sq_table) {
    uint8_t in_buf[BUF_SIZE];
    uint8_t out_buf[BUF_SIZE];
    std::optional<uint8_t> prev;
    for (;;) {
        auto n = std::fread(in_buf, 1, BUF_SIZE, stdin);
        if (n == 0) break;
        std::size_t out_pos = 0;
        for (std::size_t i = 0; i < n; ++i) {
            uint8_t c = in_buf[i];
            if (sq_table[c] && prev == c) continue;
            prev = c;
            out_buf[out_pos++] = c;
        }
        if (out_pos > 0) std::fwrite(out_buf, 1, out_pos, stdout);
    }
}

/// Delete bytes matching del_table, then squeeze using sq_table.
/// Requires byte-by-byte processing (stateful squeeze).
static void process_delete_squeeze(const std::array<bool, 256>& del_table,
                                    const std::array<bool, 256>& sq_table) {
    uint8_t in_buf[BUF_SIZE];
    uint8_t out_buf[BUF_SIZE];
    std::optional<uint8_t> prev;
    for (;;) {
        auto n = std::fread(in_buf, 1, BUF_SIZE, stdin);
        if (n == 0) break;
        std::size_t out_pos = 0;
        for (std::size_t i = 0; i < n; ++i) {
            uint8_t c = in_buf[i];
            if (del_table[c]) continue;          // Stage 1: delete
            if (sq_table[c] && prev == c) continue;  // Stage 2: squeeze
            prev = c;
            out_buf[out_pos++] = c;
        }
        if (out_pos > 0) std::fwrite(out_buf, 1, out_pos, stdout);
    }
}

/// Translate using trans_table, then squeeze using sq_table.
static void process_translate_squeeze(const std::array<uint8_t, 256>& trans_table,
                                       const std::array<bool, 256>& sq_table) {
    uint8_t in_buf[BUF_SIZE];
    uint8_t out_buf[BUF_SIZE];
    std::optional<uint8_t> prev;
    for (;;) {
        auto n = std::fread(in_buf, 1, BUF_SIZE, stdin);
        if (n == 0) break;
        std::size_t out_pos = 0;
        for (std::size_t i = 0; i < n; ++i) {
            uint8_t c = trans_table[in_buf[i]];   // Stage 1: translate
            if (sq_table[c] && prev == c) continue;  // Stage 2: squeeze
            prev = c;
            out_buf[out_pos++] = c;
        }
        if (out_pos > 0) std::fwrite(out_buf, 1, out_pos, stdout);
    }
}

// =====================================================================
// CLI parsing and main
// =====================================================================

[[noreturn]] static void die(const char* fmt, const char* arg = nullptr) {
    std::fprintf(stderr, "%s: ", program_name);
    if (arg) std::fprintf(stderr, fmt, arg);
    else     std::fputs(fmt, stderr);
    std::fputc('\n', stderr);
    std::exit(EXIT_FAILURE);
}

[[noreturn]] static void die_usage(const char* fmt, const char* arg = nullptr) {
    die(fmt, arg);
}

static void print_usage() {
    std::printf(
        "Usage: %s [OPTION]... SET1 [SET2]\n"
        "Translate, squeeze, and/or delete characters from standard input,\n"
        "writing to standard output.\n"
        "\n"
        "  -c, -C, --complement    use the complement of SET1\n"
        "  -d, --delete            delete characters in SET1, do not translate\n"
        "  -s, --squeeze-repeats   replace each sequence of a repeated character\n"
        "                          that is listed in the last specified SET,\n"
        "                          with a single occurrence of that character\n"
        "  -t, --truncate-set1     first truncate SET1 to length of SET2\n"
        "      --help              display this help and exit\n"
        "      --version           output version information and exit\n"
        "\n"
        "SETs are specified as strings of characters. Most represent themselves.\n"
        "Interpreted sequences include:\n"
        "  \\NNN       character with octal value NNN (1 to 3 octal digits)\n"
        "  \\\\         backslash\n"
        "  \\a         audible BEL\n"
        "  \\b         backspace\n"
        "  \\f         form feed\n"
        "  \\n         new line\n"
        "  \\r         return\n"
        "  \\t         horizontal tab\n"
        "  \\v         vertical tab\n"
        "  CHAR1-CHAR2  all characters from CHAR1 to CHAR2 in ascending order\n"
        "  [CHAR*]      in SET2, copies of CHAR until length of SET1\n"
        "  [CHAR*REPEAT] REPEAT copies of CHAR, REPEAT octal if starting with 0\n"
        "  [:alnum:]    all letters and digits\n"
        "  [:alpha:]    all letters\n"
        "  [:blank:]    all horizontal whitespace\n"
        "  [:cntrl:]    all control characters\n"
        "  [:digit:]    all digits\n"
        "  [:graph:]    all printable characters, not including space\n"
        "  [:lower:]    all lower case letters\n"
        "  [:print:]    all printable characters, including space\n"
        "  [:punct:]    all punctuation characters\n"
        "  [:space:]    all horizontal or vertical whitespace\n"
        "  [:upper:]    all upper case letters\n"
        "  [:xdigit:]   all hexadecimal digits\n"
        "  [=CHAR=]     all characters which are equivalent to CHAR\n",
        program_name);
}

int main(int argc, char** argv) {
    program_name = argv[0];
    if (const char* p = std::strrchr(program_name, '/')) {
        program_name = p + 1;
    }

    bool complement_flag  = false;
    bool delete_flag      = false;
    bool squeeze_flag     = false;
    bool truncate_flag    = false;

    static const struct option long_options[] = {
        {"complement",     no_argument, nullptr, 'c'},
        {"delete",         no_argument, nullptr, 'd'},
        {"squeeze-repeats", no_argument, nullptr, 's'},
        {"truncate-set1",  no_argument, nullptr, 't'},
        {"help",           no_argument, nullptr, 'h'},
        {"version",        no_argument, nullptr, 'V'},
        {nullptr, 0, nullptr, 0}
    };

    int c;
    while ((c = getopt_long(argc, argv, "cCdst", long_options, nullptr)) != -1) {
        switch (c) {
            case 'c': case 'C': complement_flag = true; break;
            case 'd': delete_flag = true;    break;
            case 's': squeeze_flag = true;   break;
            case 't': truncate_flag = true;  break;
            case 'h': print_usage(); return EXIT_SUCCESS;
            case 'V':
                std::puts("tr (C++ transpilation) 0.1");
                return EXIT_SUCCESS;
            default:
                std::fprintf(stderr, "Try '%s --help' for more information.\n",
                             program_name);
                return EXIT_FAILURE;
        }
    }

    // Collect operands (SET1 and optionally SET2)
    int n_operands = argc - optind;
    if (n_operands == 0) {
        die_usage("missing operand");
    }

    const char* set1_str = argv[optind];
    const char* set2_str = (n_operands >= 2) ? argv[optind + 1] : nullptr;

    // Validate operand counts
    if (delete_flag && !squeeze_flag) {
        if (n_operands > 1) {
            die_usage("extra operand '%s'\n"
                     "Only one string may be given when deleting without squeezing repeats.",
                     set2_str);
        }
    } else if (!delete_flag && !squeeze_flag) {
        // Translate mode: need exactly 2 operands
        if (n_operands < 2) {
            die_usage("missing operand after '%s'", set1_str);
        }
    }

    // Determine operation mode
    bool translating = !delete_flag && set2_str != nullptr && !squeeze_flag;
    // Also translate when set2 is given and squeeze is on (translate+squeeze)
    if (!delete_flag && set2_str != nullptr && squeeze_flag) {
        translating = true;
    }

    // Resolve sets
    std::vector<uint8_t> set1, set2;
    std::string error;
    if (!resolve_sets(set1_str, set2_str, complement_flag, truncate_flag,
                      translating, set1, set2, error)) {
        die("%s", error.c_str());
    }

    // Set binary mode for stdin/stdout (important for correct byte handling)
    // On POSIX systems this is a no-op; on Windows it matters.

    // Dispatch operation
    if (delete_flag && squeeze_flag) {
        // Delete set1, then squeeze set2
        auto del_table = build_delete_table(set1);
        auto sq_table = build_squeeze_table(set2);
        process_delete_squeeze(del_table, sq_table);
    } else if (delete_flag) {
        // Delete set1
        auto del_table = build_delete_table(set1);
        process_delete(del_table);
    } else if (squeeze_flag && set2_str == nullptr) {
        // Squeeze set1 only (no translation)
        auto sq_table = build_squeeze_table(set1);
        process_squeeze(sq_table);
    } else if (squeeze_flag) {
        // Translate set1→set2, then squeeze set2
        auto trans_table = build_translate_table(set1, set2);
        auto sq_table = build_squeeze_table(set2);
        process_translate_squeeze(trans_table, sq_table);
    } else {
        // Translate set1→set2
        auto trans_table = build_translate_table(set1, set2);
        process_translate(trans_table);
    }

    std::fflush(stdout);
    return EXIT_SUCCESS;
}
