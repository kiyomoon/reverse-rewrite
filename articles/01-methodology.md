# The Reverse Rewrite: Transpiling Rust Back to C++

## Part 1: Methodology — hexyl

*By Su Qingyue*

---

### The Premise

In recent years, "Rewrite it in Rust" has become a familiar refrain in systems programming. Sometimes the argument is about safety; sometimes, more loosely, about expressiveness. This series looks at a narrower question: if we translate well-known Rust programs back into idiomatic modern C++, what survives, and what does not?

If Rust's practical advantage were chiefly expressiveness, the reverse translation should quickly run into constructs with no natural C++ counterpart. If, instead, the translation stays mostly mechanical, then the more important difference may lie elsewhere: not only in what the languages can express, but in what their compilers can reject.

That distinction matters. Expressiveness and rejection are related, but not identical. When they are collapsed into one vague claim, the discussion around Rust and C++ becomes less precise than it could be.

### The Subject: hexyl

[hexyl](https://github.com/sharkdp/hexyl) is a command-line hex viewer by David Peter (sharkdp). At ~2,400 lines of Rust, it's small enough to transpile completely but large enough to exercise real Rust idioms:

- **14 enums**, some with data variants (`IncludeMode::File(String)`)
- **Trait implementations** (`Read`, `Seek` for a custom `Input` type)
- **Generics** (`Printer<'a, Writer: Write>`)
- **Builder pattern** (consuming `self`)
- **`const fn`** for compile-time gradient generation
- **`LazyLock`** for deferred initialization
- **`anyhow`/`thiserror`** error handling
- **`clap` derive** for CLI argument parsing
- **Pattern matching** with guards and exhaustiveness

This covers a representative slice of everyday Rust.

### The Method

The transpilation follows a simple rule: **produce idiomatic C++23, not "Rust in C++ syntax."** Every construct should look like something a C++ programmer would write from scratch. Where Rust and C++ have different idioms for the same concept, use the C++ idiom.

The steps:

1. **Analyze** the Rust source: module structure, dependency graph, features used
2. **Transpile** module-by-module, starting from leaf dependencies (no internal deps)
3. **Verify** by comparing output byte-for-byte against the Rust version
4. **Document** every translation decision

### The Translation Table

Here's how Rust constructs map to C++ in this project:

| Rust | C++ | Notes |
|------|-----|-------|
| Simple `enum` | `enum class` | Direct mapping |
| `enum` with data | `std::variant` or tagged struct | `IncludeMode::File(String)` → `struct IncludeModeFile` in a `variant` |
| `match` (exhaustive) | `switch` + `-Wswitch-enum` | Compiler warns on missing cases |
| `Option<T>` | `std::optional<T>` | Identical semantics |
| `Result<T, E>` | `std::expected<T, E>` (C++23) | Direct mapping |
| `trait` impl | Class method / interface | `Read` → `std::istream`, `Seek` → custom `seek()` method |
| Generics (`<Writer: Write>`) | `std::ostream&` | C++ already has a universal write interface |
| Builder (consuming `self`) | Builder returning `&` | C++ builders return references by convention |
| `LazyLock<String>` | Function-local `static` | Meyer's singleton — thread-safe by C++11 standard |
| `const fn` | `constexpr` | C++20 supports constexpr float math |
| `anyhow::Result` | Exceptions | Idiomatic for CLI applications |
| `thiserror` derive | Manual error messages | Small enough to not need a framework |
| `clap` derive | CLI11 library | Closest C++ equivalent to clap's declarative style |
| `&'a mut Writer` | `std::ostream&` | Lifetime annotation disappears; reference validity is conventional |
| `Box<dyn Read>` | `std::unique_ptr<std::istream>` | Trait object → virtual interface; C++ has built-in stream hierarchy |
| `Vec<T>` | `std::vector<T>` | Direct mapping |
| `String` / `&str` | `std::string` / `std::string_view` | Direct mapping |

### What Disappears

Some Rust features have no C++ equivalent — not because C++ can't express the concept, but because C++ doesn't *enforce* it:

**Lifetime annotations.** The Rust `Printer<'a, Writer: Write>` ties the printer's lifetime to its writer. In C++, `Printer` holds an `std::ostream&` — the same relationship exists, but nothing prevents you from using the printer after the stream is destroyed. The *structure* is identical; the *guarantee* is gone.

**Borrow checker constraints.** The Rust code's `&mut self` methods guarantee exclusive access. The C++ methods take `this` — same semantics, no compiler verification.

**`#[non_exhaustive]`** on enums. Tells downstream crates they can't exhaustively match. No C++ equivalent, but also rarely needed within a single project.

**`pub(crate)` visibility.** Rust's module visibility becomes C++ header inclusion conventions.

None of these affect *what the program does*. They affect whether the compiler can prove the program is correct.

### What Needs Explicit Handling

A few things require care rather than direct translation:

**Byte classification.** Rust's `u8::is_ascii_whitespace()` has a specific definition: `\t`, `\n`, `\x0C`, `\r`, and space. Notably, it *excludes* `\x0B` (vertical tab). C's `isspace()` includes it. Getting this wrong produces visible output differences. The solution: enumerate the exact bytes rather than using C library functions.

**UTF-8 output.** Rust's `char` is a Unicode scalar value; printing it automatically produces valid UTF-8. C++'s `char` is a byte. For box-drawing characters (┌─┬┐), braille patterns, and special symbols (⋄•×), we need explicit UTF-8 encoding. This adds ~30 lines of utility code.

**Integer overflow.** Rust panics on overflow in debug mode. C++ signed overflow is undefined behavior. In practice, hexyl's arithmetic doesn't overflow, but it's worth noting as a general translation concern.

### The Result

| Metric | Rust | C++ |
|--------|------|-----|
| Source lines (library + CLI) | 2,392 | 2,111 |
| External dependencies | 8 crates | 1 library (CLI11) |
| Build system | Cargo.toml | CMakeLists.txt |
| C++ standard | — | C++23 |
| Behavioral differences | — | None (byte-identical output) |

The C++ version is actually *shorter* — 12% fewer lines. This isn't because C++ is more concise in general; it's because:

1. **`std::ostream` eliminates a generic parameter.** Rust's `Printer<'a, Writer: Write>` becomes just `Printer`. The Writer generic exists because Rust's standard library doesn't have a single universal write trait that both files and buffers implement out of the box — `Write` is the unifying abstraction. In C++, `std::ostream` *is* that abstraction, so the generic is unnecessary.

2. **CLI11 has more compact syntax than clap derive.** Not a language difference — just library API design.

3. **No test code in source files.** Rust's `#[cfg(test)]` modules live in the source files; C++ tests are separate files (not counted in the 2,111).

### What This Proves (And Doesn't)

**For a project like hexyl, it supports a narrower claim:** every Rust construct we encountered had a natural C++ counterpart, and the translation was mostly mechanical rather than creative.

**It does not show:** That the C++ version is equally *safe*. The Rust compiler verified properties (no use-after-free, no data races, exhaustive matching) that the C++ compiler simply trusts the programmer to uphold. The safety guarantees did not survive translation; the code structure did, the proof obligations did not.

**The practical takeaway:** in this case study, Rust's value seems to lie less in what it lets the programmer say, and more in what its compiler insists on checking. That is a property of the verification pipeline, not just of the surface language.

### Next Up

In Part 2, we'll tackle the full circle: transpiling Rust reimplementations of GNU coreutils back to C++, and comparing the result with the original C code. Three languages, one program, and a more concrete question: what changed across the rewrites, and what stayed the same?

---

*The complete source code for this transpilation is available in [`01-hexyl`](../01-hexyl/). The C++ version passes 26 behavioral tests comparing its output byte-for-byte against the original Rust binary.*

**About the author.** Su Qingyue designed the experimental methodology and shaped the framing of the series. The transpilation experiments and article drafts were produced through AI-assisted workflows and then revised into their final form.
