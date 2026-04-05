// echo.cpp — Transpiled from uutils/coreutils echo.rs to idiomatic C++23
//
// Three-way comparison: GNU C (echo.c) → Rust (echo.rs) → C++ (this file)
//
// Key translation decisions:
//   Rust Options struct          → C++ Options struct (identical)
//   Rust is_flag() / filter_flags() → C++ is_flag() / filter_flags() (same logic)
//   Rust parse_escape_only()     → C++ write_escaped() inline (no uucore dependency)
//   Rust OsString                → C++ const char* (argv is already bytes on Unix)
//   Rust FormatChar iterator     → C++ bool return (false = \c encountered)
//   Rust uumain()                → C++ main()

#include <cstdio>
#include <cstdlib>
#include <cstring>

// ---------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------

struct Options {
    bool trailing_newline = true;
    bool escape = false;
};

// ---------------------------------------------------------------------------
// Hex conversion
// ---------------------------------------------------------------------------

static bool is_hex_digit(unsigned char c) {
    return (c >= '0' && c <= '9')
        || (c >= 'a' && c <= 'f')
        || (c >= 'A' && c <= 'F');
}

static int hex_to_int(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return c - 'A' + 10;  // c >= 'A' && c <= 'F'
}

// ---------------------------------------------------------------------------
// Escape sequence processing
// ---------------------------------------------------------------------------

// Write a string with backslash-escape interpretation.
// Returns false if \c was encountered (caller must stop all output).
//
// Matches uucore::format::parse_escape_only with OctalParsing::ThreeDigits:
//   \a \b \c \e \f \n \r \t \v \\
//   \0NNN  (octal, 0-3 digits after the leading '0')
//   \xHH   (hex, 1-2 digits)
//   Unknown escapes: emit backslash + character literally
static bool write_escaped(const char* s, std::size_t len) {
    std::size_t i = 0;
    while (i < len) {
        auto c = static_cast<unsigned char>(s[i++]);

        if (c != '\\' || i >= len) {
            std::putchar(c);
            continue;
        }

        // We have a backslash and at least one more character
        c = static_cast<unsigned char>(s[i++]);
        switch (c) {
            case 'a':  std::putchar('\a'); break;
            case 'b':  std::putchar('\b'); break;
            case 'c':  return false;        // stop all output
            case 'e':  std::putchar('\x1B'); break;
            case 'f':  std::putchar('\f'); break;
            case 'n':  std::putchar('\n'); break;
            case 'r':  std::putchar('\r'); break;
            case 't':  std::putchar('\t'); break;
            case 'v':  std::putchar('\v'); break;
            case '\\': std::putchar('\\'); break;

            case '0': {
                // Octal: \0 followed by 0-3 octal digits
                unsigned char val = 0;
                for (int j = 0; j < 3 && i < len
                         && s[i] >= '0' && s[i] <= '7'; ++j) {
                    val = static_cast<unsigned char>(val * 8
                        + (s[i++] - '0'));
                }
                std::putchar(val);
                break;
            }

            case 'x': {
                // Hex: \x followed by 1-2 hex digits
                if (i < len && is_hex_digit(static_cast<unsigned char>(s[i]))) {
                    unsigned char val = static_cast<unsigned char>(
                        hex_to_int(static_cast<unsigned char>(s[i++])));
                    if (i < len && is_hex_digit(
                            static_cast<unsigned char>(s[i]))) {
                        val = static_cast<unsigned char>(val * 16
                            + hex_to_int(static_cast<unsigned char>(s[i++])));
                    }
                    std::putchar(val);
                } else {
                    // Not a valid hex escape — print literally
                    std::putchar('\\');
                    std::putchar('x');
                }
                break;
            }

            default:
                // Unknown escape — print backslash + character
                std::putchar('\\');
                std::putchar(c);
                break;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Flag parsing — mirrors Rust's is_flag() and filter_flags()
// ---------------------------------------------------------------------------

// Check if `arg` is a valid echo flag (combination of -n, -e, -E).
// If valid, update `options` and return true. Otherwise leave options
// unchanged and return false.
//
// Matches Rust behavior exactly:
//   "-eeEnEe" → valid flag
//   "-eeBne" → not a flag (invalid char 'B')
//   "-"      → not a flag
static bool is_flag(const char* arg, Options& options) {
    if (arg[0] != '-' || arg[1] == '\0') {
        return false;
    }

    // Tentative copy — only applied if the whole argument is valid
    Options temp = options;

    for (const char* p = arg + 1; *p != '\0'; ++p) {
        switch (*p) {
            case 'e': temp.escape = true;  break;
            case 'E': temp.escape = false; break;
            case 'n': temp.trailing_newline = false; break;
            default:  return false;  // invalid flag character
        }
    }

    options = temp;
    return true;
}

// Process arguments, consuming leading flags and returning the index
// of the first non-flag argument.
static int filter_flags(int argc, char** argv, Options& options) {
    int i = 0;
    while (i < argc && is_flag(argv[i], options)) {
        ++i;
    }
    return i;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    // Skip program name
    --argc;
    ++argv;

    bool is_posixly_correct = std::getenv("POSIXLY_CORRECT") != nullptr;

    Options options{};
    int arg_start = 0;

    if (is_posixly_correct) {
        // POSIXLY_CORRECT: escape sequences are always enabled.
        // Only "-n" (exact match) as the first argument activates
        // flag processing; all other args are literal.
        options.escape = true;

        if (argc > 0 && std::strcmp(argv[0], "-n") == 0) {
            // The Rust version calls filter_flags() but discards the
            // returned options — only trailing_newline=false is kept.
            Options discard = options;
            arg_start = filter_flags(argc, argv, discard);
            options.trailing_newline = false;
        }
    } else {
        // Normal mode: check for --help / --version (only as sole argument)
        if (argc == 1) {
            if (std::strcmp(argv[0], "--help") == 0) {
                std::puts(
                    "Usage: echo [SHORT-OPTION]... [STRING]...\n"
                    "  or:  echo LONG-OPTION\n"
                    "Echo the STRING(s) to standard output.\n"
                    "\n"
                    "  -n     do not output the trailing newline\n"
                    "  -e     enable interpretation of backslash escapes\n"
                    "  -E     disable interpretation of backslash escapes (default)\n"
                    "      --help     display this help and exit\n"
                    "      --version  output version information and exit");
                return EXIT_SUCCESS;
            }
            if (std::strcmp(argv[0], "--version") == 0) {
                std::puts("echo (C++ transpilation) 0.1");
                return EXIT_SUCCESS;
            }
        }

        // Process flags normally
        arg_start = filter_flags(argc, argv, options);
    }

    // Output arguments separated by spaces
    for (int i = arg_start; i < argc; ++i) {
        if (i > arg_start) {
            std::putchar(' ');
        }

        if (options.escape) {
            if (!write_escaped(argv[i], std::strlen(argv[i]))) {
                // \c encountered — stop immediately, no trailing newline
                return EXIT_SUCCESS;
            }
        } else {
            std::fputs(argv[i], stdout);
        }
    }

    if (options.trailing_newline) {
        std::putchar('\n');
    }

    return EXIT_SUCCESS;
}
