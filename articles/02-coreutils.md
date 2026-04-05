# The Reverse Rewrite: Transpiling Rust Back to C++

## Part 2: The Full Circle — uutils/coreutils

*By Su Qingyue*

---

### The Setup

In Part 1, we transpiled hexyl from Rust to C++ and found the translation mostly mechanical. But hexyl is a single-author project with a clean architecture. The next question is quieter, but more revealing: what happens when we apply the same process to code that is itself a *rewrite* of older C software?

[uutils/coreutils](https://github.com/uutils/coreutils) is a Rust reimplementation of the GNU coreutils — the fundamental Unix command-line utilities. This gives us a unique opportunity: a three-way comparison. For each utility, we have:

1. **The original GNU C** — decades of battle-tested code
2. **The Rust rewrite** — modern reimplementation by the uutils project
3. **The C++ transpilation** — our reverse translation from Rust

If expressiveness were the main story, this is where we would expect the translation to resist us. If the more important difference lies elsewhere, the three-way comparison should help make that visible.

### The Subjects

We selected three utilities spanning a complexity spectrum:

| Utility | What it does | Complexity | Key challenge |
|---------|-------------|-----------|---------------|
| **echo** | Print arguments | Simple | Custom flag parsing, POSIXLY_CORRECT mode |
| **cat** | Concatenate files | Medium | Line numbering, nonprint notation, buffered I/O |
| **tr** | Translate/delete characters | Complex | Set notation parser, POSIX classes, operation chaining |

### echo: The Warm-Up

echo is deceptively simple. The GNU C version is 286 lines. The Rust version is 256. Our C++ version is 236.

The interesting part isn't the translation itself — it's what echo reveals about the Rust ecosystem's dependency structure. The Rust version imports:

- `clap` for argument parsing (which echo doesn't actually use — it has custom flag parsing)
- `uucore::format::parse_escape_only` for backslash escape handling
- `uucore::os_str_as_bytes` for OsString-to-bytes conversion

In C++, all of this reduces to ~40 lines of inline escape processing and direct `const char*` from `argv`. The Rust version's `OsString` abstraction — designed to handle non-UTF-8 filenames on Windows — adds complexity that disappears on Unix where `argv` is already raw bytes.

The POSIXLY_CORRECT handling is identical across all three versions: check the environment variable, change flag parsing behavior, force escape interpretation on. The algorithm is language-independent.

### cat: The Middle Ground

cat is where the comparison gets interesting. Three different approaches to the same problem:

**GNU C (829 lines)**: A single `main()` function with `simple_cat()` for plain copying and a complex `cat()` function for formatting options. Uses manual buffer management with pointer arithmetic. The line number is stored as ASCII digits in a global char array, incremented by propagating carries through the digits — a clever optimization that avoids `sprintf` on every line.

**Rust (863 lines)**: Clean module structure with `write_fast()` for the simple path and `write_lines()` for formatting. Uses `BufWriter` for output buffering, `memchr2` crate for efficient newline scanning, and `thiserror` for structured error types. The line number uses the same ASCII-digit-increment technique, extracted into uucore's `fast_inc_one` function.

**C++ (600 lines)**: Follows the Rust structure (separate fast/lines paths) with C++ idioms. `OutputBuffer` wraps a `std::vector<char>` with flush-to-fd semantics. The `LineNumber` struct is nearly identical to the Rust version. Uses `getopt_long` for argument parsing.

The 30% reduction from Rust to C++ comes from three sources:

1. **No trait hierarchy.** Rust's `FdReadable` trait exists to unify `Stdin` and `File` behind `Read + AsFd`. In C++, both are already file descriptors — the trait simply vanishes.

2. **No error type hierarchy.** Rust's `CatError` enum with `thiserror` derive creates a structured error type with automatic `Display` implementations. In C++, error messages are string literals passed to `fprintf(stderr, ...)`.

3. **No inline tests.** Rust's `#[cfg(test)]` modules live in the source file. C++ tests are separate (not counted).

The core algorithm — the `write_lines` state machine that handles squeeze-blank, line numbering, show-ends, show-nonprint, and the \r\n detection for `^M$` — is structurally identical across all three versions. The same sentinel newline technique, the same M-^X notation for high bytes, the same carry-propagation for line numbers.

### tr: The Stress Test

tr is the most complex utility and provides the clearest picture of what changes — and what doesn't — across languages.

**GNU C (1,906 lines)**: A monolithic implementation with the set parser, operations, and I/O all interleaved. Uses a hand-rolled parser with `goto` for flow control. Character classes are expanded inline. The translation table is a 256-element `unsigned char` array.

**Rust (1,098 lines)**: Cleanly separated into modules: `operation.rs` (parser + operations, 772 lines), `simd.rs` (optimized I/O, 109 lines), `tr.rs` (CLI + dispatch, 197 lines). Uses `nom` parser combinators for set parsing, trait-based polymorphism for operations, and `bytecount` for SIMD-optimized single-byte operations.

**C++ (694 lines)**: Single file with hand-rolled recursive descent parser, direct dispatch for the 5 operation modes, and `std::array<bool, 256>` / `std::array<uint8_t, 256>` lookup tables.

The 37% reduction from Rust to C++ is almost entirely explained by one thing: **nom**. The Rust version uses ~200 lines of parser combinator code to define tr's set grammar. The C++ version uses ~100 lines of hand-rolled parsing for the same grammar. Parser combinators provide composability, automatic error reporting, and testability — genuine benefits for complex grammars. But tr's set notation is simple enough that a recursive descent parser handles it with less code and comparable clarity.

The trait hierarchy (`SymbolTranslator`, `ChunkProcessor`, `ChainedSymbolTranslator`) is another source. It's an elegant abstraction — but there are only 5 operation combinations in tr. The C++ version just has 5 processing functions, each ~15 lines, dispatched by a switch in `main()`. No inheritance, no virtual dispatch, no templates. Same behavior, less machinery.

### The Numbers

| Metric | GNU C | Rust | C++ |
|--------|-------|------|-----|
| Total lines | 3,021 | 2,217 | 1,530 |
| External dependencies | autoconf/gnulib | 8 crates | none |
| Build system | autotools | Cargo | CMake |
| Tests passing | n/a | n/a | 66/66 |
| Behavioral differences | — | — | None vs system utils |

A caveat on the GNU C line counts: they include extensive comments (the GNU coding standards require detailed prose), gnulib helper functions pulled into each utility, and autoconf-related boilerplate. The Rust and C++ counts are more directly comparable to each other than either is to the GNU C figures.

The C++ version is 50% shorter than GNU C and 31% shorter than Rust. This isn't because C++ is inherently more concise — it's because:

1. **No shared library overhead.** The Rust versions depend on `uucore` (a shared utility crate) and several external crates (`clap`, `nom`, `memchr`, `thiserror`, `bytecount`). Each dependency adds abstraction layers. The C++ versions are self-contained.

2. **No inline tests.** The Rust line counts include `#[cfg(test)]` modules.

3. **No platform abstraction.** The Rust versions handle Windows (binary mode, broken pipe exit codes). The C++ versions target POSIX only.

4. **Simpler error handling.** `Result<T, E>` with `?` propagation is elegant but verbose. `fprintf(stderr, ...)` is terse.

### The Translation Table (Expanded)

Building on Part 1's table, here are the patterns specific to coreutils:

| Rust (coreutils) | C++ | Notes |
|-------------------|-----|-------|
| `uucore::fast_inc_one` | Inline digit carry-propagation | Same technique as GNU C's `next_line_num` |
| `nom` combinators (`alt`, `delimited`, ...) | Hand-rolled recursive descent | Simpler for small grammars |
| `thiserror` derive | `fprintf(stderr, ...)` | No error type hierarchy |
| `memchr2` crate | 4-line scan loop | Or `std::memchr` for single-byte |
| `SymbolTranslator` trait | Direct function dispatch | 5 combinations = 5 functions |
| `BufWriter<StdoutLock>` | `OutputBuffer` (vector + flush to fd) | Same semantics |
| `#[cfg(unix)] FdReadable` | Raw file descriptor (int) | Unix trait unification unnecessary |
| `OsString` / `os_str_as_bytes` | `const char*` / `argv` | Unix argv is already bytes |
| `clap` arg definition | `getopt_long` | Standard POSIX API |

### What the Rust Rewrite Actually Accomplished

Here's the core finding: the algorithms are identical across all three versions. The carry-propagation line counter, the M-^X nonprint notation, the 256-element byte lookup tables — these are language-independent techniques. The Rust rewrite didn't discover better algorithms.

What it did do:

**1. Eliminated memory safety bugs.** The GNU C versions use pointer arithmetic with manual bounds checking. A single off-by-one error could cause a buffer overflow. The Rust versions make this class of bug impossible at compile time.

**2. Structured the error handling.** GNU C's `error()` calls are scattered through the code with ad-hoc error messages. The Rust versions define error types (`CatError`, `BadSequence`) that make error paths explicit and testable.

**3. Separated platform-specific code.** GNU C uses `#ifdef` for platform differences. Rust uses `#[cfg()]` attributes with proper type-level separation (the `FdReadable` trait, the `splice` module).

**4. Added comprehensive tests.** The Rust versions include unit tests in the source files and integration tests that verify behavior against the GNU versions.

None of these are *expressiveness* differences. They're *safety* and *engineering practice* differences. The Rust compiler enforced standards that the C compiler left to programmer discipline.

### What the Comparison Suggests

The three-way comparison reveals a layered picture:

- **GNU C → Rust**: The rewrite added safety guarantees and modernized code structure. The algorithms stayed the same.
- **Rust → C++**: The transpilation removed safety guarantees and simplified abstractions. The algorithms stayed the same.
- **GNU C → C++**: The transpilation is what the original C code *would have looked like* if written in modern C++ — structured, typed, but without the decades of accumulated complexity.

The C++ versions sit between the GNU C and Rust versions: they have much of the structural clarity of the Rust code but without the compiler-enforced safety. In practice, they often read like what you might get if a C++ programmer reimplemented the utilities from scratch while borrowing the Rust version's architecture.

For these utilities, the pattern from Part 1 continued: **the main visible difference was not what the programs could express, but what the toolchain checked or left to programmer discipline.** The same programs, the same algorithms, and no observed expressiveness gap in these cases.

### Next Up

In Part 3, we move to the area most likely to show a real limit: async and concurrency. We'll transpile Tokio's mini-redis teaching project to C++, where the safety guarantees — Send, Sync, ownership of shared state — are genuinely harder to replace.

---

*The complete source code for all three transpilations is available in [`02-coreutils`](../02-coreutils/). 66 behavioral tests verify the C++ versions against system utilities.*
