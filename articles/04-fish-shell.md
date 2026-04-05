# The Reverse Rewrite: Transpiling Rust Back to C++ — Part 4: The Round Trip — fish shell

*By Su Qingyue*

---

## The Premise

In 2023, the fish shell began its migration from C++ to Rust. The project — roughly 70,000 lines of mature, well-maintained C++ — was incrementally ported using autocxx for interop, with the final C++ release (3.7.1) shipping in January 2024 and the first Rust release (4.0.0) following in February 2025.

This rewrite has often been cited as one of the clearer real-world examples in the broader "Rewrite it in Rust" discussion: a project maintained for over a decade that chose to port its codebase from C++ to Rust. The stated motivations were safety, correctness, and developer experience.

This gives us a useful round-trip comparison. We take selected modules from the Rust fish shell, transpile them back to C++, and compare the result with the original pre-2023 C++ code. If the round-trip produces similar code, the rewrite looks more like a shift in enforcement than in expression. If it produces something very different, then the rewrite carried a deeper design change with it.

## The Modules

We selected three self-contained modules that seemed likely to be analytically useful:

**color** (518 Rust lines) — Terminal color handling: RGB parsing, 256-color palette matching, named color lookup. Rich type patterns: Rust enum with associated data, bitflags, sorted table binary search, nearest-color-by-squared-distance algorithm.

**wgetopt** (578 Rust lines) — Wide-character GNU getopt. This module has the deepest lineage: it was originally GNU C, ported to C++ with wide characters for fish, then rewritten in Rust for fish 4.0. Transpiling it back to C++ completes a four-language genealogy.

**timer** (235 Rust lines) — Execution time measurement using `getrusage` and `steady_clock`. The control case: nearly identical line counts across both fish versions, so we expect near-perfect convergence.

## The Numbers

| Module | Original C++ | Rust | Transpiled C++ |
|--------|------------:|-----:|---------------:|
| color  | 493         | 518  | 448            |
| wgetopt| 603         | 578  | 412            |
| timer  | 237         | 235  | 227            |
| **Total** | **1,333** | **1,331** | **1,087** |

The first thing worth noticing is that Rust and original C++ are the same size. In Articles 1–3, Rust was always larger than the C++ transpilation (by 12%, 31%, and 33%). Here, the source and target are essentially equal — the fish team's Rust rewrite did not add verbosity for two of three modules, and actually shortened the third.

The second thing worth noticing is that the transpiled C++ is 18% shorter than both. This continues the trend from previous articles, but here the baseline is different. We are not merely removing Rust's type machinery, because the original C++ never had much of that machinery to begin with — the Rust version is already lean. The reduction comes from using modern C++ idioms that the 2014-era original never adopted.

## Does the Round-Trip Converge?

### The Named Color Table: Perfect Convergence

The named color table in the color module is the most unambiguous test of convergence. It is a static, sorted array of 22 entries mapping color names to palette indices and RGB values.

The answer: the table is character-for-character identical across all three versions. Same names, same order, same indices, same RGB values, same hidden flags. The algorithm that searches this table — binary search with case-insensitive comparison — is also identical in all three.

This is not surprising. Data tables are language-independent. But it establishes the baseline: where there is nothing to translate, the round-trip is perfect.

### The Exchange Function: Divergence Through Improvement

The `exchange` function in wgetopt permutes argv elements to group options before non-options. It is the heart of GNU getopt's argument reordering.

The original C++ implements this with the classic GNU algorithm: a while loop with two inner for loops that repeatedly swap the shorter of two segments with the far end of the longer segment. It is 44 lines of pointer arithmetic, carefully commented.

The Rust rewrite replaced all of this with one line: `self.argv[left..right].rotate_left(middle - left)`.

The transpiled C++ follows the Rust's lead: `std::rotate(argv + left, argv + middle, argv + right)`.

The round-trip does not converge. The transpiled C++ is dramatically simpler than the original — not because C++ cannot express the original algorithm, but because the Rust rewrite was an opportunity to adopt a stdlib primitive that the original never used. `std::rotate` has been available since C++98. The original fish C++ simply never adopted it, because the code was derived from GNU getopt and nobody refactored it.

One reading of the round-trip experiment is that **the Rust rewrite acted as a catalyst for modernization, not necessarily as a shift in expressiveness.** The improvement (`rotate_left` / `std::rotate`) was always available in C++. It took a rewrite — any rewrite — to prompt its adoption.

### The Color Layout: Divergence Through Design Philosophy

The original fish C++ aggressively bit-packs the color type: 3 bits for the type tag, 5 bits for flags, plus a 3-byte union for data. A `static_assert` enforces `sizeof(rgb_color_t) <= 4`.

The Rust rewrite abandoned bit-packing entirely. The type became an algebraic enum (`Type::Named { idx: u8 }`, `Type::Rgb(Color24)`), and the flags became a `bitflags!` struct. The struct is now 8+ bytes — twice the original.

The transpiled C++ lands in between: `enum class Type : uint8_t` (1 byte) + `uint8_t flags_` (1 byte) + `union Data` (3 bytes) = 6 bytes. It follows Rust's structural clarity but doesn't recover the original's bit-packing.

A human optimizer could easily recover the 4-byte layout by re-introducing bitfields. But the transpiler — whether human or AI — working from the Rust source has no reason to: the Rust code gives no hint that the original was packed, and the packed layout was an optimization, not an algorithm. In practice, the 2-byte difference (4 vs 6 bytes) is unlikely to matter — fish does not store millions of color structs in contiguous arrays. The original's packing was a principled design choice, not a performance-critical optimization.

### The Timer: Near-Perfect Convergence

The timer module confirms what convergence looks like when the code is purely algorithmic. Every function in the transpiled version follows the original's structure:

- `take()`: identical (`getrusage` + `steady_clock::now()`)
- `get_delta()`: identical algorithm, identical formatting, identical unit thresholds (900M, 999995, 1000 microseconds)
- `push_timer()`: identical RAII pattern (lambda/Drop/destructor)

The only real difference: the original returns `wcstring` (wide string); the Rust returns `String`; the transpiled returns `std::string`. This is a consequence of the Rust rewrite's broader decision to reduce wide-string usage, not a module-level change.

## What Survived the Round-Trip

| Pattern | Status |
|---------|--------|
| Named color table (22 entries, sorted) | Identical across all three |
| Binary search with case-insensitive compare | Identical |
| Squared-distance color matching | Identical |
| getrusage + steady_clock timing | Identical |
| Timer formatting with unit selection | Identical |
| GNU getopt option parsing algorithm | Identical |
| Long option prefix matching with exact/ambiguous detection | Identical |

Every algorithm survived. The data structures survived. The control flow survived. Within the scope of these experiments, algorithms increasingly look language-independent. What began as a hypothesis now reads more like a recurring empirical pattern within this sample.

## What Changed in the Round-Trip

| Pattern | Original C++ | → Rust → Transpiled C++ |
|---------|-------------|-------------------------|
| Array permutation | Manual swap loop (44 lines) | `std::rotate` (1 line) |
| Parse failure signaling | Constructor returns type_none | `std::optional<RgbColor>` |
| Memory layout (color) | 4-byte bit-packed | 6-byte unpacked |
| Flag manipulation | Unnamed enum + bitfields | Constexpr namespace + uint8_t |
| wgetopt long_only param | Present | Removed (follows Rust's simplification) |
| String types | wcstring (wide) | std::string / std::wstring_view |

The changes fall into three categories:

**1. Refactoring improvements** (`std::rotate`, `std::optional`) — These are modernizations that were always available in C++ but required a rewrite to prompt. They survive the round-trip because they represent genuine design improvements.

**2. Design decisions** (removing `long_only`, unconditional `push_timer`) — These are deliberate simplifications made during the Rust rewrite. The transpiled C++ preserves them because we translate the Rust as-is, not the original C++.

**3. Type-system artifacts** (bit-packing loss, bitflags! → manual ops) — These are consequences of mechanical translation. The original's micro-optimizations were guided by C++'s value-type semantics; the Rust redesign prioritized structural clarity; the transpilation follows the Rust's structure without recovering the original's discipline.

## The Whole-Project Perspective

The three modules we analyzed total 1,331 Rust lines. The entire fish-shell Rust codebase is approximately 75,000 lines — we examined less than 2%. Our conclusions apply confidently to the code we examined but should be extrapolated with caution.

However, the fish-shell's overall line count tells its own story. The Rust version (4.0.0) is 16–21% larger than the C++ version (3.7.1) at the whole-project level, depending on whether blank lines and comments are included (16%) or only non-blank source lines are counted (21%). This is the opposite of Articles 1–3, where C++ transpilations were consistently shorter. The direction matters: when professional C++ developers write C++ and then professional Rust developers rewrite it in Rust, the Rust is larger. When an AI transpiles Rust to C++, the C++ is smaller.

This suggests that the line count differences in Articles 1–3 were not a property of the languages themselves, but a property of the translation direction. Idiomatic rewrites add code (for good reasons: better error handling, explicit types, more tests). Mechanical transpilations remove it (by collapsing type machinery that was there for the compiler, not the programmer).

## What the Round-Trip Suggests

The round-trip produces a third version: neither the original C++ nor the Rust, but something in between. It shares algorithms with both, follows the Rust's API design, adopts modern C++ idioms, and loses the original's micro-optimizations.

This tells us three things about the fish shell rewrite:

**First, within these modules, the rewrite does not look primarily like a story about expressiveness.** Every algorithm we examined — color distance, binary search, getopt parsing, timer formatting — is expressed identically in both languages. No Rust feature was necessary to express what the code does.

**Second, the rewrite was an opportunity to modernize.** The `exchange` function's transformation from 44 lines to 1 line happened not because Rust has `rotate_left` and C++ doesn't (C++ has `std::rotate`), but because rewriting code from scratch encourages the adoption of better abstractions. This benefit accrues to any rewrite, in any language.

**Third, the safety enforcement is real but impermanent.** The lifetimes on `WGetopter<'opts, 'args, 'argarray>` — three lifetime parameters tracking the relationships between option strings, argument strings, and the array that holds them — represent genuine compile-time safety that prevents use-after-free bugs. But they vanish in the transpiled C++, replaced by the programmer's understanding that `argv` must outlive the `WGetopter` instance. The safety existed in the Rust compiler's verification pass, not in the code's structure.

## Updated Translation Table

Patterns discovered in the fish shell round-trip:

| Rust | C++ |
|------|-----|
| `enum Type { Named { idx: u8 }, Rgb(Color24) }` | `enum class Type` + tagged union |
| `bitflags! { struct Flags: u8 { ... } }` | `namespace` + `inline constexpr uint8_t` |
| `slice.rotate_left(n)` | `std::rotate()` |
| `Option<Self>` (fallible parse) | `std::optional<T>` |
| `&wstr` / `wstr` (fish's wide string) | `std::wstring_view` / `std::wstring` |
| `WGetopter<'opts, 'args, 'argarray>` (3 lifetimes) | `WGetopter` (no lifetime tracking) |
| `L!("foo")` macro | `L"foo"` literal |
| `PrintElapsedOnDrop` (Drop trait) | `TimerGuard` (destructor) |
| `char_at(0)` / `is_empty()` | `*ptr` / `*ptr == '\0'` |

## Next Up

In Part 5, we step back from code and synthesize the findings from all four experiments. We revisit the distinction between expressiveness and rejection, examine what it means for a language to "carry safety proofs," and ask whether AI-assisted transpilation might be useful as one possible workflow alongside rewriting and direct maintenance.

---

*The complete source code and comparison materials for this round-trip experiment are available in [`04-fish-shell`](../04-fish-shell/). This case study compares three fish-shell modules across the original C++, the Rust rewrite, and the transpiled C++ version.*
