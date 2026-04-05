// cat.cpp — Transpiled from uutils/coreutils cat.rs to idiomatic C++23
//
// Three-way comparison: GNU C (cat.c) → Rust (cat.rs) → C++ (this file)
//
// Key translation decisions:
//   Rust LineNumber + fast_inc_one    → C++ LineNumber (same buffer-based digit increment)
//   Rust CatError enum (thiserror)    → C++ fprintf(stderr, ...) + return codes
//   Rust InputHandle<R: FdReadable>   → C++ InputHandle { int fd; bool is_interactive; }
//   Rust BufWriter<StdoutLock>        → C++ OutputBuffer (manual vector<char> buffer)
//   Rust memchr2 crate               → C++ memchr2() inline helper
//   Rust clap arg parsing             → C++ getopt_long
//   Rust #[cfg(linux)] splice         → Omitted (portability; GNU C uses copy_file_range)
//   Rust trait FdReadable             → Not needed (using raw fd)
//   Rust OutputOptions methods        → C++ member functions (identical)
//   Rust write_nonprint_to_end        → C++ same ^/M- notation logic

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <getopt.h>
#include <sys/stat.h>
#include <unistd.h>

// =====================================================================
// Constants
// =====================================================================

static constexpr int LINE_NUMBER_BUF_SIZE = 32;
static constexpr std::size_t INPUT_BUF_SIZE = 1024 * 31;
static constexpr std::size_t FAST_BUF_SIZE  = 1024 * 64;

// =====================================================================
// LineNumber — buffer-based line numbering
// =====================================================================
// Stores the line number as ASCII digits in a fixed-size char buffer.
// Incrementing operates directly on the ASCII digits (carry-propagation),
// which is significantly faster than sprintf/to_string on every line.
//
// Rust equivalent: LineNumber struct + uucore::fast_inc_one.
// GNU C equivalent: line_buf[] + next_line_num().

struct LineNumber {
    char buf[LINE_NUMBER_BUF_SIZE];
    int print_start;
    int num_start;
    int num_end;  // exclusive — points to the '\t' separator

    LineNumber() {
        // Fill with '0' so there's room to grow leftward
        std::memset(buf, '0', LINE_NUMBER_BUF_SIZE);
        // Initialize the visible portion: "     1\t"
        static constexpr char init[] = "     1\t";
        static constexpr int init_len = sizeof(init) - 1;  // 7
        print_start = LINE_NUMBER_BUF_SIZE - init_len;  // 25
        std::memcpy(buf + print_start, init, init_len);
        num_start = LINE_NUMBER_BUF_SIZE - 2;  // 30: position of '1'
        num_end   = LINE_NUMBER_BUF_SIZE - 1;  // 31: position of '\t'
    }

    void increment() {
        int pos = num_end - 1;
        while (pos >= num_start) {
            if (buf[pos] < '9') {
                ++buf[pos];
                return;
            }
            buf[pos] = '0';
            --pos;
        }
        // All digits overflowed — extend leftward
        --num_start;
        buf[num_start] = '1';
        if (num_start < print_start) {
            print_start = num_start;
        }
    }

    const char* data() const { return buf + print_start; }
    int size() const { return LINE_NUMBER_BUF_SIZE - print_start; }
};

// =====================================================================
// Enums and structs
// =====================================================================

enum class NumberingMode { None, NonEmpty, All };

struct OutputOptions {
    NumberingMode number = NumberingMode::None;
    bool squeeze_blank  = false;
    bool show_tabs      = false;
    bool show_ends      = false;
    bool show_nonprint  = false;

    const char* tab()         const { return show_tabs ? "^I" : "\t"; }
    int         tab_len()     const { return show_tabs ? 2 : 1; }
    const char* end_of_line() const { return show_ends ? "$\n" : "\n"; }
    int         eol_len()     const { return show_ends ? 2 : 1; }

    // True if we can bypass all formatting and just copy bytes.
    bool can_write_fast() const {
        return !(show_tabs || show_nonprint || show_ends
                 || squeeze_blank || number != NumberingMode::None);
    }
};

struct OutputState {
    LineNumber line_number{};
    bool at_line_start          = true;
    bool skipped_carriage_return = false;
    bool one_blank_kept          = false;
};

struct InputHandle {
    int  fd;
    bool is_interactive;
};

// =====================================================================
// Helpers
// =====================================================================

static const char* program_name = "cat";

/// Find the first occurrence of c1 or c2 in [s, s+n).
/// Equivalent to Rust's memchr2 crate.
static const char* memchr2(const char* s, char c1, char c2, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        if (s[i] == c1 || s[i] == c2) return s + i;
    }
    return nullptr;
}

/// Write exactly n bytes, retrying on EINTR.
static bool full_write(int fd, const void* buf, std::size_t n) {
    auto p = static_cast<const char*>(buf);
    while (n > 0) {
        ssize_t w = ::write(fd, p, n);
        if (w <= 0) {
            if (w < 0 && errno == EINTR) continue;
            return false;
        }
        p += w;
        n -= static_cast<std::size_t>(w);
    }
    return true;
}

// =====================================================================
// OutputBuffer — buffered writes to STDOUT_FILENO
// =====================================================================
// Matches Rust's BufWriter<StdoutLock> with manual flush semantics.

class OutputBuffer {
    std::vector<char> buf_;
    bool error_ = false;

public:
    OutputBuffer() {
        buf_.reserve(INPUT_BUF_SIZE * 4 + LINE_NUMBER_BUF_SIZE + 256);
    }

    void put(char c) { buf_.push_back(c); }

    void write(const char* data, std::size_t n) {
        buf_.insert(buf_.end(), data, data + n);
    }

    void write(const char* s) { write(s, std::strlen(s)); }

    void flush() {
        if (buf_.empty() || error_) return;
        if (!full_write(STDOUT_FILENO, buf_.data(), buf_.size())) {
            error_ = true;
        }
        buf_.clear();
    }

    bool has_error() const { return error_; }
};

// =====================================================================
// write_*_to_end: output bytes until a line-ending is found
// =====================================================================
// Each function returns the number of INPUT bytes consumed
// (i.e., the offset within the buffer where \n or \r was found,
// or the buffer length if no line-ending was encountered).

/// Plain copy: stop at \n or \r.
static std::size_t write_to_end(OutputBuffer& out,
                                const char* buf, std::size_t len) {
    auto p = memchr2(buf, '\n', '\r', len);
    if (p) {
        auto n = static_cast<std::size_t>(p - buf);
        out.write(buf, n);
        return n;
    }
    out.write(buf, len);
    return len;
}

/// Show tabs as ^I: stop at \n or \r.
static std::size_t write_tab_to_end(OutputBuffer& out,
                                     const char* buf, std::size_t len) {
    std::size_t count = 0;
    while (count < len) {
        auto c = static_cast<unsigned char>(buf[count]);
        if (c == '\n' || c == '\r') return count;
        if (c == '\t') {
            out.write("^I", 2);
        } else {
            out.put(static_cast<char>(c));
        }
        ++count;
    }
    return count;
}

/// Show non-printing characters with ^/M- notation: stop at \n only.
/// \r is converted to ^M (not stopped at), unlike write_to_end.
static std::size_t write_nonprint_to_end(OutputBuffer& out,
                                          const char* buf, std::size_t len,
                                          const char* tab, int tab_len) {
    std::size_t count = 0;
    for (std::size_t i = 0; i < len; ++i) {
        auto byte = static_cast<unsigned char>(buf[i]);
        if (byte == '\n') break;

        if (byte == 9) {
            // TAB — use configured representation
            out.write(tab, static_cast<std::size_t>(tab_len));
        } else if (byte <= 8 || (byte >= 10 && byte <= 31)) {
            // Control characters → ^@..^_ (excluding TAB)
            out.put('^');
            out.put(static_cast<char>(byte + 64));
        } else if (byte <= 126) {
            // Printable ASCII (32..126)
            out.put(static_cast<char>(byte));
        } else if (byte == 127) {
            // DEL → ^?
            out.write("^?", 2);
        } else if (byte <= 159) {
            // High control (128..159) → M-^@..M-^_
            out.put('M');  out.put('-');  out.put('^');
            out.put(static_cast<char>(byte - 64));
        } else if (byte <= 254) {
            // High printable (160..254) → M-SPACE..M-~
            out.put('M');  out.put('-');
            out.put(static_cast<char>(byte - 128));
        } else {
            // 255 → M-^?
            out.write("M-^?", 4);
        }
        ++count;
    }
    return count;
}

/// Dispatch to the appropriate write function based on options.
static std::size_t write_end(OutputBuffer& out,
                              const char* buf, std::size_t len,
                              const OutputOptions& opt) {
    if (opt.show_nonprint) {
        return write_nonprint_to_end(out, buf, len, opt.tab(), opt.tab_len());
    } else if (opt.show_tabs) {
        return write_tab_to_end(out, buf, len);
    } else {
        return write_to_end(out, buf, len);
    }
}

// =====================================================================
// Line-ending helpers
// =====================================================================

/// Write end-of-line marker; flush if input is interactive (terminal).
static void write_end_of_line(OutputBuffer& out,
                               const char* eol, int eol_len,
                               bool is_interactive) {
    out.write(eol, static_cast<std::size_t>(eol_len));
    if (is_interactive) {
        out.flush();
    }
}

/// Handle a newline in the input stream.
/// Takes care of: pending \r, squeeze-blank, and line numbering for
/// blank lines (only in NumberingMode::All).
static void write_new_line(OutputBuffer& out,
                            const OutputOptions& opt,
                            OutputState& state,
                            bool is_interactive) {
    if (state.skipped_carriage_return) {
        // A \r was deferred — now we know it's followed by \n.
        if (opt.show_ends) {
            out.write("^M", 2);  // Show \r\n as ^M$\n
        } else {
            out.put('\r');        // Emit the deferred \r
        }
        state.skipped_carriage_return = false;
        write_end_of_line(out, opt.end_of_line(), opt.eol_len(),
                          is_interactive);
        return;
    }

    if (!state.at_line_start || !opt.squeeze_blank || !state.one_blank_kept) {
        state.one_blank_kept = true;
        if (state.at_line_start && opt.number == NumberingMode::All) {
            out.write(state.line_number.data(),
                      static_cast<std::size_t>(state.line_number.size()));
            state.line_number.increment();
        }
        write_end_of_line(out, opt.end_of_line(), opt.eol_len(),
                          is_interactive);
    }
    // If squeeze_blank && one_blank_kept && at_line_start:
    // suppress this blank line entirely.
}

// =====================================================================
// write_fast — simple byte copy (no formatting options active)
// =====================================================================

static bool write_fast(InputHandle& handle) {
    char buf[FAST_BUF_SIZE];
    for (;;) {
        ssize_t n = ::read(handle.fd, buf, FAST_BUF_SIZE);
        if (n == 0) return true;
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (!full_write(STDOUT_FILENO, buf, static_cast<std::size_t>(n))) {
            return false;
        }
    }
}

// =====================================================================
// write_lines — line-by-line processing with formatting
// =====================================================================

static bool write_lines(InputHandle& handle,
                         const OutputOptions& opt,
                         OutputState& state) {
    char in_buf[INPUT_BUF_SIZE];
    OutputBuffer out;

    for (;;) {
        ssize_t n_read = ::read(handle.fd, in_buf, INPUT_BUF_SIZE);
        if (n_read == 0) break;
        if (n_read < 0) {
            if (errno == EINTR) continue;
            return false;
        }

        auto n = static_cast<std::size_t>(n_read);
        std::size_t pos = 0;

        while (pos < n) {
            // ---- Handle newlines ----
            if (in_buf[pos] == '\n') {
                write_new_line(out, opt, state, handle.is_interactive);
                state.at_line_start = true;
                ++pos;
                continue;
            }

            // ---- Emit any deferred \r ----
            if (state.skipped_carriage_return) {
                out.put('\r');
                state.skipped_carriage_return = false;
                state.at_line_start = false;
            }

            state.one_blank_kept = false;

            // ---- Line number at start of non-blank line ----
            if (state.at_line_start && opt.number != NumberingMode::None) {
                out.write(state.line_number.data(),
                          static_cast<std::size_t>(state.line_number.size()));
                state.line_number.increment();
            }

            // ---- Write content to end of line or end of buffer ----
            std::size_t offset = write_end(out, in_buf + pos, n - pos, opt);

            // End of buffer?
            if (offset + pos == n) {
                state.at_line_start = false;
                break;
            }

            // What character stopped us?
            if (in_buf[pos + offset] == '\r') {
                state.skipped_carriage_return = true;
            } else {
                // Must be '\n'
                write_end_of_line(out, opt.end_of_line(), opt.eol_len(),
                                  handle.is_interactive);
                state.at_line_start = true;
            }
            pos += offset + 1;
        }

        // Flush after each read chunk (required for pipe tests:
        // data should appear before we block on the next read).
        out.flush();
        if (out.has_error()) return false;
    }

    return true;
}

// =====================================================================
// File handling
// =====================================================================

/// Check whether input and stdout are the same regular file.
/// Allows same-inode for pipes and sockets (they're distinct streams).
static bool is_same_file(int input_fd) {
    struct stat in_st{}, out_st{};
    if (fstat(input_fd, &in_st) < 0 || fstat(STDOUT_FILENO, &out_st) < 0) {
        return false;
    }
    if (S_ISFIFO(in_st.st_mode) || S_ISSOCK(in_st.st_mode)) {
        return false;
    }
    return in_st.st_dev == out_st.st_dev && in_st.st_ino == out_st.st_ino;
}

static bool cat_handle(InputHandle& handle,
                        const OutputOptions& opt,
                        OutputState& state) {
    return opt.can_write_fast() ? write_fast(handle)
                                : write_lines(handle, opt, state);
}

static bool cat_path(const char* path,
                      const OutputOptions& opt,
                      OutputState& state) {
    bool is_stdin = (std::strcmp(path, "-") == 0);
    InputHandle handle{};

    if (is_stdin) {
        handle.fd = STDIN_FILENO;
        handle.is_interactive = isatty(STDIN_FILENO);
    } else {
        // Check for directory before opening
        struct stat st{};
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
            std::fprintf(stderr, "%s: %s: Is a directory\n",
                         program_name, path);
            return false;
        }
        handle.fd = open(path, O_RDONLY);
        if (handle.fd < 0) {
            std::fprintf(stderr, "%s: %s: %s\n",
                         program_name, path, std::strerror(errno));
            return false;
        }
        handle.is_interactive = false;
    }

    // Detect writing a file to itself
    if (is_same_file(handle.fd)) {
        std::fprintf(stderr, "%s: %s: input file is output file\n",
                     program_name, path);
        if (!is_stdin) close(handle.fd);
        return false;
    }

    bool ok = cat_handle(handle, opt, state);
    if (!ok) {
        std::fprintf(stderr, "%s: %s: %s\n",
                     program_name, path, std::strerror(errno));
    }

    if (!is_stdin) close(handle.fd);
    return ok;
}

// =====================================================================
// Argument parsing and main
// =====================================================================

[[noreturn]] static void print_usage_and_exit() {
    std::printf(
        "Usage: %s [OPTION]... [FILE]...\n"
        "Concatenate FILE(s) to standard output.\n"
        "\n"
        "With no FILE, or when FILE is -, read standard input.\n"
        "\n"
        "  -A, --show-all           equivalent to -vET\n"
        "  -b, --number-nonblank    number nonempty output lines, overrides -n\n"
        "  -e                       equivalent to -vE\n"
        "  -E, --show-ends          display $ at end of each line\n"
        "  -n, --number             number all output lines\n"
        "  -s, --squeeze-blank      suppress repeated empty output lines\n"
        "  -t                       equivalent to -vT\n"
        "  -T, --show-tabs          display TAB characters as ^I\n"
        "  -u                       (ignored)\n"
        "  -v, --show-nonprinting   use ^ and M- notation, except for LFD and TAB\n"
        "      --help               display this help and exit\n"
        "      --version            output version information and exit\n",
        program_name);
    std::exit(EXIT_SUCCESS);
}

int main(int argc, char** argv) {
    program_name = argv[0];
    if (const char* p = std::strrchr(program_name, '/')) {
        program_name = p + 1;
    }

    bool number          = false;
    bool number_nonblank = false;
    bool squeeze_blank   = false;
    bool show_ends       = false;
    bool show_nonprint   = false;
    bool show_tabs       = false;

    static const struct option long_options[] = {
        {"show-all",         no_argument, nullptr, 'A'},
        {"number-nonblank",  no_argument, nullptr, 'b'},
        {"show-ends",        no_argument, nullptr, 'E'},
        {"number",           no_argument, nullptr, 'n'},
        {"squeeze-blank",    no_argument, nullptr, 's'},
        {"show-tabs",        no_argument, nullptr, 'T'},
        {"show-nonprinting", no_argument, nullptr, 'v'},
        {"help",             no_argument, nullptr, 'h'},
        {"version",          no_argument, nullptr, 'V'},
        {nullptr, 0, nullptr, 0}
    };

    int c;
    while ((c = getopt_long(argc, argv, "AbeEnstTuv", long_options, nullptr))
           != -1) {
        switch (c) {
            case 'A': show_nonprint = show_ends = show_tabs = true; break;
            case 'b': number = number_nonblank = true; break;
            case 'e': show_ends = show_nonprint = true; break;
            case 'E': show_ends = true;       break;
            case 'n': number = true;          break;
            case 's': squeeze_blank = true;   break;
            case 't': show_tabs = show_nonprint = true; break;
            case 'T': show_tabs = true;       break;
            case 'u': /* ignored */           break;
            case 'v': show_nonprint = true;   break;
            case 'h': print_usage_and_exit();
            case 'V':
                std::puts("cat (C++ transpilation) 0.1");
                return EXIT_SUCCESS;
            default:
                std::fprintf(stderr, "Try '%s --help' for more information.\n",
                             program_name);
                return EXIT_FAILURE;
        }
    }

    OutputOptions opt;
    opt.number       = number_nonblank ? NumberingMode::NonEmpty
                     : number          ? NumberingMode::All
                                       : NumberingMode::None;
    opt.squeeze_blank = squeeze_blank;
    opt.show_tabs     = show_tabs;
    opt.show_ends     = show_ends;
    opt.show_nonprint = show_nonprint;

    // Collect file arguments (default to stdin)
    std::vector<std::string> files;
    if (optind >= argc) {
        files.emplace_back("-");
    } else {
        for (int i = optind; i < argc; ++i) {
            files.emplace_back(argv[i]);
        }
    }

    OutputState state;
    bool ok = true;

    for (const auto& file : files) {
        if (!cat_path(file.c_str(), opt, state)) {
            ok = false;
        }
    }

    // Emit any pending \r that wasn't followed by \n
    if (state.skipped_carriage_return) {
        full_write(STDOUT_FILENO, "\r", 1);
    }

    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
