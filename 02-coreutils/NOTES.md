# Article 2: uutils/coreutils — Translation Notes

## Utilities Selected

| Utility | Complexity | Key Rust Features |
|---------|-----------|-------------------|
| echo    | Simple    | Custom flag parsing, escape sequences, POSIXLY_CORRECT |
| cat     | Medium    | Trait generics (FdReadable), LineNumber fast-increment, memchr2, thiserror |
| tr      | Complex   | nom parser combinators, trait polymorphism, SIMD optimization, 256-element lookup tables |

## Three-Way Line Counts

| Utility | GNU C  | Rust   | C++  | C++ vs Rust |
|---------|--------|--------|------|-------------|
| echo    | 286    | 256    | 236  | −8%         |
| cat     | 829    | 863    | 600  | −30%        |
| tr      | 1,906  | 1,098  | 694  | −37%        |
| **Total** | **3,021** | **2,217** | **1,530** | **−31%** |

## Translation Decisions by Utility

### echo

| Rust | C++ | Notes |
|------|-----|-------|
| `Options` struct | `Options` struct | Direct mapping |
| `is_flag()` / `filter_flags()` | Same functions | Logic is identical |
| `uucore::parse_escape_only` | `write_escaped()` inline | Reimplement ~40 lines of escape handling |
| `OsString` args | `const char*` | On Unix, argv is already bytes |
| `FormatChar` iterator | `bool` return | False = `\c` encountered (stop) |
| `clap` derive (uu_app) | Manual `--help`/`--version` | echo uses custom arg parsing, not getopt |
| `env::var_os("POSIXLY_CORRECT")` | `std::getenv("POSIXLY_CORRECT")` | Direct mapping |

**Key observation:** echo's custom flag parser (not using clap) translates almost verbatim.
The Rust version's most complex dependency is `uucore::format::parse_escape_only` — a shared
library function. In C++, we inline the escape logic directly (~40 lines), eliminating the
dependency entirely.

### cat

| Rust | C++ | Notes |
|------|-----|-------|
| `LineNumber` + `fast_inc_one` | `LineNumber` struct | Same buffer-based digit increment technique |
| `CatError` enum (thiserror) | `fprintf(stderr, ...)` | Error strings inline, no error type hierarchy |
| `InputHandle<R: FdReadable>` | `InputHandle { int fd; bool interactive; }` | Generic gone; raw fd sufficient |
| `BufWriter<StdoutLock>` | `OutputBuffer` (vector + flush) | Manual buffered output |
| `memchr2` crate | `memchr2()` helper function | 4-line scan loop |
| `clap` arg parsing | `getopt_long` | Standard POSIX option parsing |
| `splice()` on Linux | Omitted | Platform-specific optimization; `write_fast()` still works |
| `#[cfg(unix)] FdReadable` trait | Not needed | Using fd directly, not generic over Read trait |
| `write_nonprint_to_end` | Same ^/M- notation | Byte-for-byte identical output |

**Key observation:** The Rust version's `FdReadable` trait exists solely to unify `Stdin` and
`File` behind a common `Read + AsFd` interface. In C++, both are already file descriptors,
so the trait simply disappears. This is a case where Rust's abstraction *adds* complexity
that C++ doesn't need.

The `LineNumber` struct translates almost identically — the buffer-based digit increment is a
shared technique across all three versions (GNU C, Rust, and C++).

### tr

| Rust | C++ | Notes |
|------|-----|-------|
| `nom` parser combinators | Hand-rolled recursive descent | ~100 lines replaces ~200 lines of nom |
| `Sequence` enum (5 variants) | `Sequence` tagged struct | Same semantics, different representation |
| `Class` enum | `CharClass` enum class | Direct mapping |
| `SymbolTranslator` trait | Direct dispatch (5 inline loops) | No trait hierarchy needed |
| `ChunkProcessor` trait | Merged into processing loops | SIMD optimization omitted |
| `ChainedSymbolTranslator<A, B>` | Inline in dispatch code | Only 2 chain combinations exist |
| `[bool; 256]` / `[u8; 256]` | `std::array<bool, 256>` / `std::array<uint8_t, 256>` | Direct mapping |
| `bytecount` crate (SIMD) | Standard loop | Compiler auto-vectorizes |
| `Box<dyn Iterator<Item = u8>>` | `std::vector<uint8_t>` | Eager collection vs lazy iteration |

**Key observation:** The nom parser combinators are the single largest source of code in the
Rust version. Replacing them with a hand-rolled parser reduces tr's line count by ~37%.
This isn't because C++ is more concise — it's because parser combinators trade code size
for composability, and tr's grammar is simple enough that a direct parser is clearer.

The trait hierarchy (`SymbolTranslator` + `ChunkProcessor` + `ChainedSymbolTranslator`) is
elegant in Rust but unnecessary in C++: there are only 5 operation combinations, so direct
dispatch with inline loops is simpler and equally fast.

## What Disappeared in Translation

### From Rust → C++ (safety guarantees lost):

1. **Lifetime annotations**: `InputHandle<R: FdReadable>` tied the reader's lifetime to the
   handle. In C++, the fd is just an int — nothing prevents use-after-close.

2. **Borrow checker**: The Rust version's `&mut OutputState` ensures exclusive access to the
   output state. In C++, it's a plain reference — no compiler verification.

3. **Exhaustive matching**: Rust's `match` on `CatError`, `Sequence`, `Class` enums is
   verified exhaustive. C++ `switch` has `-Wswitch-enum` but it's a warning, not an error.

4. **`thiserror` derive**: Automatic `Display`/`Error` trait implementations. In C++, error
   messages are just string literals — no type-level error handling.

5. **Type-level IO safety**: `FdReadable` trait bounds ensure the reader supports both
   `Read` and `AsFd`. In C++, any int could be passed as an fd.

### From GNU C → Rust (what the Rust rewrite added):

1. **Structured error handling**: GNU C uses `error()` calls scattered through code.
   Rust uses `Result<T, CatError>` with `?` propagation.

2. **Builder pattern**: Not applicable to these simple utilities, but the Rust ecosystem
   encourages structured API design even for internal code.

3. **Platform abstraction**: Rust's `#[cfg(unix)]` and trait system cleanly separate
   platform-specific code. GNU C uses `#ifdef` which is more ad-hoc.

4. **Memory safety**: The Rust versions cannot have buffer overflows, use-after-free,
   or null pointer dereferences. The C versions rely on careful programming.

## What the Rust Rewrite Actually Accomplished

The uutils/coreutils project isn't just a "rewrite in Rust" — it's a comprehensive
reimplementation that:

1. **Eliminated entire classes of bugs**: Buffer overflows, off-by-one in pointer
   arithmetic, missing null checks — none of these are possible in the Rust version.

2. **Modernized the code structure**: The Rust versions use proper error types, clear
   module boundaries, and documented APIs. The GNU C versions are monolithic.

3. **Added test coverage**: The Rust versions come with inline tests (`#[cfg(test)]`)
   and integration tests that verify behavior against the GNU versions.

4. **Made platform-specific code explicit**: `#[cfg(unix)]` attributes clearly mark
   what code is platform-dependent, whereas GNU C uses autoconf macros.

But the core *algorithms* are identical. The line numbering technique, the nonprint
notation, the set-based byte operations — these are the same in all three languages.
The Rust rewrite's value was not in discovering better algorithms, but in wrapping
the same algorithms in a safer, more structured container.
