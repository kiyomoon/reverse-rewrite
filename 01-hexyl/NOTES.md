# hexyl Transpilation Notes

## Source Analysis (v0.17.0)

**5 files, 2,392 lines:**

| File | Lines | Role |
|------|-------|------|
| `colors.rs` | 184 | Color definitions, CP437/CP1047 tables, ANSI escape generation |
| `input.rs` | 64 | Input abstraction (File vs Stdin), custom seek for pipes |
| `lib.rs` | 1,153 | Core hex printer: enums, builder pattern, output formatting |
| `main.rs` | 835 | CLI parsing (clap), byte offset parsing, entry point |
| `tests.rs` | 156 | Unit tests for byte offset parsing |

Plus `tests/integration_tests.rs` (965 lines) with comprehensive behavioral tests.

## Rust Features Catalog

### Enums
- **Simple (no data):** `Base`, `ByteCategory`, `Squeezer`, `BorderStyle`, `CharacterTable`, `ColorScheme`, `Endianness`, `ByteOffsetKind`, `ColorWhen`, `GroupSize`
- **With data:** `IncludeMode(File(String))`, `Input<'a>(File/StdinLock)`, `Unit(Block{custom_size})`, `ByteOffsetParseError` (multiple variants with data)

### Traits
- `Read` impl for `Input` (dispatches to File/Stdin)
- `Seek` impl for `Input` (with pipe fallback: forward-only via read+discard)
- `From` impls for type conversions

### Generics
- `Printer<'a, Writer: Write>` — generic over output writer
- `PrinterBuilder<'a, Writer: Write>` — builder pattern
- `print_all<Reader: Read>` — generic over input reader

### Pattern Matching
- Exhaustive `match` on all enums
- Nested match with guard patterns (`AsciiWhitespace if self.0 == b' '`)

### Error Handling
- `anyhow::Result` for CLI errors
- `thiserror` derive for `ByteOffsetParseError`
- `?` operator throughout

### Ownership/Lifetimes
- `&'a mut Writer` lifetime tying Printer to Writer
- `Input<'a>` with `StdinLock<'a>`
- `Box<dyn Read + 'a>` trait object

### Const/Compile-time
- `const fn` for RGB escape generation and gradient computation
- `LazyLock<String>` for deferred color initialization from env vars

### Builder Pattern
- `PrinterBuilder` with method chaining (consuming `self`)

### External Crates
- `clap` (CLI derive), `anyhow`/`thiserror` (errors), `owo-colors`/`supports-color` (terminal colors), `terminal_size`, `const_format`

## Translation Decisions

### Enums → C++

| Rust | C++ | Rationale |
|------|-----|-----------|
| Simple enums (`Base`, `ByteCategory`, etc.) | `enum class` | Direct mapping, same semantics |
| `IncludeMode` (with `File(String)`) | `std::variant<...>` or tagged struct | Variant captures the data |
| `Input<'a>` (File/StdinLock) | Class wrapping `std::unique_ptr<std::istream>` | Both inherit from `std::istream`; polymorphism is free |
| `ByteOffsetParseError` | `enum class` + error message function | No need for full variant types; the error is for display |

### Trait Impls → C++

| Rust | C++ | Rationale |
|------|-----|-----------|
| `impl Read for Input` | `Input` wraps `std::istream` | `std::istream` already provides `read()` |
| `impl Seek for Input` | `Input::seek()` method | Custom seek with pipe fallback |
| `From<X> for Y` | Constructor or conversion operator | Idiomatic C++ |

### Generics → C++

| Rust | C++ | Rationale |
|------|-----|-----------|
| `Printer<'a, Writer: Write>` | `Printer` using `std::ostream&` | No need for templates; `std::ostream` is the universal sink |
| `print_all<Reader: Read>` | `print_all(std::istream&)` | Same reasoning |
| `PrinterBuilder<'a, Writer>` | `PrinterBuilder` with `std::ostream&` | Simplified |

### Error Handling → C++

| Rust | C++ | Rationale |
|------|-----|-----------|
| `io::Result<()>` | Exceptions / `std::ostream` error state | C++ I/O idiom |
| `anyhow::Result` | `std::expected<T, std::string>` or exceptions | CLI context; exceptions are fine |
| `ByteOffsetParseError` | `std::expected<T, ByteOffsetParseError>` | C++23, mirrors Rust Result |
| `thiserror` derive | Manual `what()` messages | Simple enough |

### LazyLock → C++

Meyer's singleton (function-local static):
```cpp
const std::string& color_null() {
    static const std::string s = init_color("NULL", 90); // BrightBlack
    return s;
}
```
Thread-safe by C++11 standard (§6.7/4).

### const fn → constexpr

The gradient generation and RGB escape code construction translate directly to `constexpr` functions in C++20/23.

### CLI Parsing

`clap` → CLI11 (header-only C++ library via CMake FetchContent). Closest in spirit: declarative, supports subcommands, validators, default values.

### What Disappears

1. **Borrow checker annotations** — Lifetimes `'a` on `Printer` and `Input` vanish. The C++ code relies on conventional discipline (don't use dangling references).
2. **`#[derive]` macros** — Replaced by explicit constructors, or not needed at all.
3. **`#[non_exhaustive]`** — No C++ equivalent; switch warnings serve a similar purpose.
4. **Module visibility (`pub(crate)`)** — C++ uses header inclusion as the visibility mechanism.

### What Needs Explicit Handling

1. **Integer overflow** — Rust panics in debug mode; C++ signed overflow is UB. Use checked arithmetic where the Rust code relies on panic behavior.
2. **UTF-8 in character output** — `char` in C++ is not `char` in Rust. Use `char32_t` or encode to UTF-8 explicitly for multi-byte characters (box-drawing, braille).
3. **Exhaustive matching** — C++ `switch` doesn't enforce exhaustiveness. Compiler warnings (`-Wswitch-enum`) help but aren't mandatory.

## C++ Project Structure

```
translated-cpp/
├── CMakeLists.txt          # C++23, FetchContent for CLI11/Catch2
├── src/
│   ├── colors.hpp          # Color constants, tables, gradients
│   ├── colors.cpp          # Lazy color init from env vars
│   ├── input.hpp           # Input class wrapping istream
│   ├── input.cpp           # Input implementation
│   ├── hexyl.hpp           # Core enums, PrinterBuilder, Printer
│   ├── hexyl.cpp           # Printer implementation
│   ├── byte_offset.hpp     # Byte offset parsing
│   ├── byte_offset.cpp     # Byte offset implementation
│   └── main.cpp            # CLI setup and entry point
└── tests/
    ├── test_printer.cpp     # Port of lib.rs unit tests
    ├── test_byte_offset.cpp # Port of tests.rs unit tests
    └── examples/            # Test fixtures (copied from Rust)
```

## Dependencies

| Rust crate | C++ equivalent | Method |
|------------|---------------|--------|
| `clap` | CLI11 | FetchContent |
| `anyhow` / `thiserror` | `std::expected` + custom errors | Standard library |
| `owo-colors` | Direct ANSI escape codes | Inline |
| `supports-color` | `isatty()` | POSIX |
| `terminal_size` | `ioctl(TIOCGWINSZ)` | POSIX |
| `const_format` | `constexpr` + string literals | Standard library |
| `assert_cmd` / `predicates` | Catch2 + subprocess | FetchContent |
