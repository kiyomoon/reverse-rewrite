#include "byte_offset.hpp"
#include "colors.hpp"
#include "hexyl.hpp"
#include "input.hpp"

#include <CLI/CLI.hpp>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

#ifdef _WIN32
#include <io.h>
#define isatty _isatty
#define fileno _fileno
#else
#include <sys/ioctl.h>
#include <unistd.h>
#endif

using namespace hexyl;

static int get_terminal_width() {
#ifdef _WIN32
    return 80;
#else
    struct winsize w {};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_col > 0) {
        return w.ws_col;
    }
    return 80;
#endif
}

static bool stdout_supports_color() {
#ifdef _WIN32
    return false;
#else
    return isatty(fileno(stdout));
#endif
}

static void print_color_table() {
    std::cout << "hexyl color reference:\n\n";

    std::cout << color_null();
    std::cout << "\xe2\x8b\x84 NULL bytes (0x00)\n"; // ⋄
    std::cout << COLOR_RESET;

    std::cout << color_ascii_printable();
    std::cout << "a ASCII printable characters (0x20 - 0x7E)\n";
    std::cout << COLOR_RESET;

    std::cout << color_ascii_whitespace();
    std::cout << "_ ASCII whitespace (0x09 - 0x0D, 0x20)\n";
    std::cout << COLOR_RESET;

    std::cout << color_ascii_other();
    std::cout << "\xe2\x80\xa2 ASCII control characters (except NULL and whitespace)\n"; // •
    std::cout << COLOR_RESET;

    std::cout << color_nonascii();
    std::cout << "\xc3\x97 Non-ASCII bytes (0x80 - 0xFF)\n"; // ×
    std::cout << COLOR_RESET;
}

int main(int argc, char* argv[]) {
    CLI::App app{"A command-line hex viewer", "hexyl"};
    app.set_version_flag("--version", "hexyl 0.17.0 (C++ port)");

    std::string file_arg;
    app.add_option("FILE", file_arg, "The file to display. If no FILE argument is given, read from STDIN.");

    std::string length_arg;
    app.add_option("-n,--length,-c,--bytes", length_arg,
        "Only read N bytes from the input. Supports units (kB, MiB, etc.) and hex (0xff).")
        ->option_text("N");

    // -l as short alias for length (same option group)
    app.add_option("-l", length_arg)->group("")->option_text("N");

    std::string skip_arg;
    app.add_option("-s,--skip", skip_arg,
        "Skip the first N bytes of the input. Supports units and hex. Negative seeks from end.")
        ->option_text("N");

    std::string block_size_arg = "512";
    app.add_option("--block-size", block_size_arg,
        "Sets the size of the `block` unit to SIZE.")
        ->option_text("SIZE");

    bool no_squeezing = false;
    app.add_flag("-v,--no-squeezing", no_squeezing,
        "Displays all input data (disable squeezing).");

    std::string color_arg = "always";
    app.add_option("--color", color_arg,
        "When to use colors: always, auto, never, force.")
        ->option_text("WHEN")
        ->check(CLI::IsMember({"always", "auto", "never", "force"}));

    std::string border_arg = "unicode";
    app.add_option("--border", border_arg,
        "Border style: unicode, ascii, none.")
        ->option_text("STYLE")
        ->check(CLI::IsMember({"unicode", "ascii", "none"}));

    bool plain = false;
    app.add_flag("-p,--plain", plain,
        "Display output with --no-characters, --no-position, --border=none, and --color=never.");

    bool no_characters = false;
    app.add_flag("--no-characters", no_characters, "Do not show the character panel.");

    bool characters_flag = false;
    app.add_flag("-C,--characters", characters_flag, "Show the character panel (overrides --no-characters).");

    std::string char_table_arg = "default";
    app.add_option("--character-table", char_table_arg,
        "How bytes are mapped to characters: default, ascii, codepage-1047, codepage-437, braille.")
        ->option_text("FORMAT")
        ->check(CLI::IsMember({"default", "ascii", "codepage-1047", "codepage-437", "braille"}));

    std::string color_scheme_arg = "default";
    app.add_option("--color-scheme", color_scheme_arg,
        "Color scheme: default, gradient.")
        ->option_text("FORMAT")
        ->check(CLI::IsMember({"default", "gradient"}));

    bool no_position = false;
    app.add_flag("-P,--no-position", no_position, "Do not show the position panel.");

    std::string display_offset_arg = "0";
    app.add_option("-o,--display-offset", display_offset_arg,
        "Add N bytes to the displayed file position.")
        ->option_text("N");

    std::string panels_arg;
    app.add_option("--panels", panels_arg,
        "Number of hex data panels. Use 'auto' for terminal width.")
        ->option_text("N");

    std::string group_size_arg = "1";
    app.add_option("-g,--group-size,--groupsize", group_size_arg,
        "Bytes per group: 1, 2, 4, or 8.")
        ->option_text("N")
        ->check(CLI::IsMember({"1", "2", "4", "8"}));

    std::string endianness_arg = "big";
    app.add_option("--endianness", endianness_arg,
        "Group byte order: little, big.")
        ->option_text("FORMAT")
        ->check(CLI::IsMember({"little", "big"}));

    bool little_endian_flag = false;
    app.add_flag("-e", little_endian_flag, "Alias for --endianness=little.");

    std::string base_arg = "hexadecimal";
    app.add_option("-b,--base", base_arg,
        "Base for bytes: binary/b/2, octal/o/8, decimal/d/10, hexadecimal/x/16.")
        ->option_text("B");

    uint64_t terminal_width_opt = 0;
    app.add_option("--terminal-width", terminal_width_opt,
        "Sets the number of terminal columns.")
        ->option_text("N");

    bool print_color_table_flag = false;
    app.add_flag("--print-color-table", print_color_table_flag,
        "Print a table showing how different types of bytes are colored.");

    bool include_mode_flag = false;
    app.add_flag("-i,--include", include_mode_flag,
        "Output in C include file style.");

    CLI11_PARSE(app, argc, argv);

    // --- Handle special modes ---
    if (print_color_table_flag) {
        print_color_table();
        return 0;
    }

    // --- Apply --plain overrides ---
    if (plain) {
        color_arg = "never";
        border_arg = "none";
        no_characters = true;
        no_position = true;
    }

    // -C overrides --no-characters
    if (characters_flag) {
        no_characters = false;
    }

    // --- Open input ---
    std::cin.sync_with_stdio(false);
    Input reader = file_arg.empty()
        ? Input::from_stdin()
        : (file_arg == "-" ? Input::from_stdin() : [&] {
            namespace fs = std::filesystem;
            if (fs::is_directory(file_arg)) {
                std::cerr << "Error: '" << file_arg << "' is a directory.\n";
                std::exit(1);
            }
            return Input::from_file(file_arg);
        }());

    // --- Parse block size ---
    auto parse_bs = [&]() -> PositiveI64 {
        if (auto hex = try_parse_as_hex_number(block_size_arg)) {
            if (!*hex) {
                std::cerr << "Error: invalid block size\n";
                std::exit(1);
            }
            auto p = PositiveI64::create(**hex);
            if (!p) {
                std::cerr << "Error: block size argument must be positive\n";
                std::exit(1);
            }
            return *p;
        }
        auto nu = extract_num_and_unit_from(block_size_arg);
        if (!nu) {
            std::cerr << "Error: " << nu.error().message() << "\n";
            std::exit(1);
        }
        auto [num, unit] = *nu;
        if (unit.kind == UnitKind::Block) {
            std::cerr << "Error: can not use 'block(s)' as a unit to specify block size\n";
            std::exit(1);
        }
        int64_t val = num * unit.get_multiplier();
        auto p = PositiveI64::create(val);
        if (!p) {
            std::cerr << "Error: block size argument must be positive\n";
            std::exit(1);
        }
        return *p;
    };
    auto block_size = parse_bs();

    // --- Parse skip ---
    uint64_t skip_offset = 0;
    if (!skip_arg.empty()) {
        auto offset = parse_byte_offset(skip_arg, block_size);
        if (!offset) {
            std::cerr << "Error: failed to parse `--skip` arg \"" << skip_arg
                      << "\" as byte count: " << offset.error().message() << "\n";
            std::exit(1);
        }
        try {
            switch (offset->kind) {
                case ByteOffsetKind::ForwardFromBeginning:
                case ByteOffsetKind::ForwardFromLastOffset:
                    skip_offset = reader.seek(0, offset->value.value);
                    break;
                case ByteOffsetKind::BackwardFromEnd:
                    skip_offset = reader.seek(2, -offset->value.value);
                    break;
            }
        } catch (const std::exception& e) {
            std::cerr << "Error: Failed to jump to the desired input position. "
                      << "This could be caused by a negative offset that is too large or by "
                      << "an input that is not seek-able (e.g. if the input comes from a pipe).\n";
            std::exit(1);
        }
    }

    // --- Parse length and create reader ---
    auto parse_byte_count = [&](const std::string& s) -> uint64_t {
        auto offset = parse_byte_offset(s, block_size);
        if (!offset) {
            std::cerr << "Error: " << offset.error().message() << "\n";
            std::exit(1);
        }
        auto fwd = offset->assume_forward_offset_from_start();
        if (!fwd) {
            std::cerr << "Error: " << fwd.error() << "\n";
            std::exit(1);
        }
        return static_cast<uint64_t>(*fwd);
    };

    std::unique_ptr<std::istream> final_reader;
    if (!length_arg.empty()) {
        uint64_t length = parse_byte_count(length_arg);
        auto inner = reader.into_inner();
        final_reader = std::make_unique<LimitedReader>(std::move(inner), length);
    } else {
        final_reader = reader.into_inner();
    }

    // --- Color ---
    bool no_color_env = std::getenv("NO_COLOR") != nullptr;
    bool show_color;
    if (color_arg == "never") {
        show_color = false;
    } else if (color_arg == "always") {
        show_color = !no_color_env;
    } else if (color_arg == "force") {
        show_color = true;
    } else { // auto
        show_color = !no_color_env && stdout_supports_color();
    }

    // --- Border style ---
    BorderStyle border_style = BorderStyle::Unicode;
    if (border_arg == "ascii") border_style = BorderStyle::Ascii;
    else if (border_arg == "none") border_style = BorderStyle::None;

    bool squeeze = !no_squeezing;
    bool show_char_panel = !no_characters;
    bool show_position_panel = !no_position;

    // --- Display offset ---
    uint64_t display_offset = parse_byte_count(display_offset_arg);

    // --- Base ---
    Base base;
    if (base_arg == "b" || base_arg == "bin" || base_arg == "binary" || base_arg == "2") {
        base = Base::Binary;
    } else if (base_arg == "o" || base_arg == "oct" || base_arg == "octal" || base_arg == "8") {
        base = Base::Octal;
    } else if (base_arg == "d" || base_arg == "dec" || base_arg == "decimal" || base_arg == "10") {
        base = Base::Decimal;
    } else if (base_arg == "x" || base_arg == "hex" || base_arg == "hexadecimal" || base_arg == "16") {
        base = Base::Hexadecimal;
    } else {
        std::cerr << "Error: invalid base \"" << base_arg << "\"\n";
        return 1;
    }

    uint8_t base_digits;
    switch (base) {
        case Base::Binary: base_digits = 8; break;
        case Base::Octal: base_digits = 3; break;
        case Base::Decimal: base_digits = 3; break;
        case Base::Hexadecimal: base_digits = 2; break;
    }

    // --- Group size ---
    uint8_t group_size = static_cast<uint8_t>(std::stoi(group_size_arg));

    // --- Panels ---
    uint64_t terminal_width = static_cast<uint64_t>(get_terminal_width());

    auto max_panels_fn = [&](uint64_t tw, uint64_t bd, uint64_t gs) -> uint64_t {
        uint64_t offset = show_position_panel ? 10 : 1;
        uint64_t col_width = show_char_panel
            ? ((8 / gs) * (bd * gs + 1)) + 2 + 8
            : ((8 / gs) * (bd * gs + 1)) + 2;
        if (tw <= offset || (tw - offset) / col_width < 1) return 1;
        return (tw - offset) / col_width;
    };

    uint64_t panels;
    if (panels_arg == "auto") {
        panels = max_panels_fn(terminal_width, base_digits, group_size);
    } else if (!panels_arg.empty()) {
        panels = std::stoull(panels_arg);
        if (panels == 0) {
            std::cerr << "Error: --panels must be a positive integer\n";
            return 1;
        }
    } else if (terminal_width_opt > 0) {
        panels = max_panels_fn(terminal_width_opt, base_digits, group_size);
    } else {
        panels = std::min<uint64_t>(2, max_panels_fn(terminal_width, base_digits, group_size));
    }

    // --- Endianness ---
    Endianness endianness = little_endian_flag ? Endianness::Little
        : (endianness_arg == "little" ? Endianness::Little : Endianness::Big);

    // --- Character table ---
    CharacterTable character_table = CharacterTable::Default;
    if (char_table_arg == "ascii") character_table = CharacterTable::Ascii;
    else if (char_table_arg == "codepage-1047") character_table = CharacterTable::CP1047;
    else if (char_table_arg == "codepage-437") character_table = CharacterTable::CP437;
    else if (char_table_arg == "braille") character_table = CharacterTable::Braille;

    // --- Color scheme ---
    ColorScheme color_scheme = (color_scheme_arg == "gradient")
        ? ColorScheme::Gradient : ColorScheme::Default;

    // --- Include mode ---
    IncludeMode include_mode = IncludeModeOff{};
    if (include_mode_flag) {
        if (!file_arg.empty()) {
            if (file_arg == "-") {
                include_mode = IncludeModeFile{"stdin"};
            } else {
                namespace fs = std::filesystem;
                auto fname = fs::path(file_arg).filename().string();
                include_mode = IncludeModeFile{fname.empty() ? "file" : fname};
            }
        } else {
            include_mode = IncludeModeStdin{};
        }
    }

    // --- Build and run printer ---
    std::ostream& out = std::cout;
    auto printer = PrinterBuilder(out)
        .show_color(show_color)
        .show_char_panel(show_char_panel)
        .show_position_panel(show_position_panel)
        .with_border_style(border_style)
        .enable_squeezing(squeeze)
        .num_panels(panels)
        .group_size(group_size)
        .with_base(base)
        .endianness(endianness)
        .character_table(character_table)
        .include_mode(std::move(include_mode))
        .color_scheme(color_scheme)
        .build();

    printer.display_offset(skip_offset + display_offset);

    try {
        printer.print_all(*final_reader);
    } catch (const std::exception& e) {
        // Check for broken pipe
        if (std::cout.fail()) {
            return 0;
        }
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
