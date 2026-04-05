# Article 4: fish shell — Translation Notes

## Project

**Source:** [fish-shell/fish-shell](https://github.com/fish-shell/fish-shell)
**C++ version:** tag 3.7.1 (January 2024 — last C++ release)
**Rust version:** tag 4.0.0 (February 2025 — first Rust release)
**Direction:** fish went C++ → Rust in 2023-2024. We transpile selected Rust modules back to C++, then compare with the original pre-2023 C++ code.

## Module Selection

Three self-contained modules were selected for the round-trip analysis:

| Module | Description | Selection rationale |
|--------|-------------|---------------------|
| **color** | Terminal color parsing: RGB, 256-color, named colors | Rich patterns (enum with data, bitflags, binary search, color distance algorithm). Very self-contained. |
| **wgetopt** | Wide-character GNU getopt | Maximum narrative value: GNU C → C++ wide port → Rust rewrite → C++ transpilation. Array permutation algorithm. Maximally self-contained. |
| **timer** | Execution time measurement (getrusage + duration formatting) | Nearly identical line counts across versions. Extremely self-contained. Good baseline for "mechanical translation" comparison. |

## Line Counts (Three-Way Comparison)

| Module | Original C++ (3.7.1) | Rust (4.0.0) | Transpiled C++ | Rust vs Orig | Trans vs Orig | Trans vs Rust |
|--------|---------------------:|-------------:|---------------:|-------------:|--------------:|--------------:|
| color  | 493                  | 518          | 448            | +5%          | −9%           | −14%          |
| wgetopt| 603                  | 578          | 412            | −4%          | −32%          | −29%          |
| timer  | 237                  | 235          | 227            | −1%          | −4%           | −3%           |
| **Total** | **1,333**         | **1,331**    | **1,087**      | **~0%**      | **−18%**      | **−18%**      |

**Key observation:** Rust and original C++ are nearly identical in total size (1,333 vs 1,331 — effectively the same). This is the opposite of Articles 1-3, where the Rust source was consistently larger than the C++ transpilation. Here the Rust is actually slightly larger than the original C++ in two out of three modules.

The transpiled C++ is 18% shorter than both. This comes from three sources:
1. Stripped verbose GNU-style comment blocks (especially wgetopt)
2. Used modern C++ idioms (`std::rotate`, `std::optional`, `std::wstring_view`)
3. Followed Rust's simplified API surface (e.g., no `long_only` in wgetopt)

## Three-Way Comparison: Does the Round-Trip Converge?

### color — Convergent Structure, Divergent Layout

**Named color table:** IDENTICAL data across all three versions. Same 22 entries, same order, same color names, same index values, same RGB values, same hidden flags. The table is the algorithm's core data, and it survived both translations unchanged.

**simple_icase_compare:** IDENTICAL algorithm across all three. All three convert to lowercase using manual ASCII case folding (not locale-dependent wcscasecmp). All three compare character-by-character. The only difference is how the return type is expressed: original returns `int`, Rust returns `Ordering`, transpiled returns `int`.

**try_parse_rgb:** IDENTICAL hex parsing logic. All three:
  - Strip one leading `#`
  - Handle 3-digit (FA3 → FFAA33) and 6-digit (F3A035) formats
  - Validate each character as a hex digit
  - Convert to RGB

**try_parse_named:** IDENTICAL binary search on sorted table. All three use `lower_bound` / `binary_search_by` with case-insensitive comparison.

**convert_color (nearest-color distance):** IDENTICAL squared-distance algorithm. Sum of squared differences for R, G, B channels. Same palette data.

**KEY DIVERGENCE — Memory layout:**
- **Original C++:** Aggressive bit-packing: `uint8_t type : 3` + `uint8_t flags : 5` + union. Static assertion: `sizeof(rgb_color_t) <= 4`.
- **Rust:** No bit-packing. Full algebraic enum `Type { None, Named{idx}, Rgb(Color24), Normal, Reset }` + `bitflags! Flags: u8`. Much larger in memory (~8+ bytes).
- **Transpiled C++:** `enum class Type : uint8_t` + `uint8_t flags_` + union. 6 bytes. Follows Rust's structural clarity but doesn't recover the original's bit-packing.

The transpiled version is structurally between the two: it uses a tagged union (like the original) but doesn't bit-pack the type and flags (like the Rust). A human optimizer would either recover the 4-byte packing or accept the 6 bytes for clarity.

**KEY DIVERGENCE — API design:**
- **Original C++:** `rgb_color_t(const wcstring& str)` constructor sets type to none on failure. No indication of parse failure.
- **Rust:** `RgbColor::from_wstr(s) -> Option<Self>`. Explicit failure signaling.
- **Transpiled C++:** `std::optional<RgbColor> from_wstr(std::wstring_view s)`. Follows Rust's explicit design.

This is a real design improvement that survived the round-trip. The original C++ silently produced a "none" color on parse failure; the Rust redesign made failure explicit; the transpilation preserves that explicitness.

### wgetopt — Convergent Algorithm, Improved Abstraction

**exchange (array permutation):**
- **Original C++:** Hand-rolled GNU getopt algorithm. Binary partition swap: repeatedly identifies the shorter of two segments and swaps it with part of the longer segment. 44 lines including the while loop, two inner for loops, and temp pointer.
- **Rust:** `self.argv[left..right].rotate_left(middle - left)`. 1 line.
- **Transpiled C++:** `std::rotate(argv + left, argv + middle, argv + right)`. 1 line.

This is the most dramatic divergence in the round-trip. The transpiled C++ does NOT converge with the original. Instead, it follows the Rust's design decision to use a stdlib primitive. `std::rotate` was available since C++98 but the original fish C++ (derived from GNU getopt) never adopted it. The Rust rewrite was the catalyst for this simplification.

**Core option parsing (initialize, advance_to_next_argv, handle_short_opt, handle_long_opt, find_matching_long_opt):**
All three implementations follow the IDENTICAL algorithm:
1. Check for `-` and `--` prefixes
2. Handle PERMUTE/REQUIRE_ORDER/RETURN_IN_ORDER ordering
3. Binary search for long options with exact/ambiguous matching
4. Character-by-character scanning for short options
5. Optional/required argument handling with the same edge cases

The algorithm is a direct descendant of GNU getopt. It survived both translations completely intact.

**KEY DIVERGENCE — API simplification:**
- **Original C++:** Has `long_only` parameter in `_wgetopt_internal()` (allows `-foo` to match long option `--foo` if there's no `-f` short opt). Supports `wcslen(arg) >= 3` heuristic for distinguishing `-fu` from `-f -u`.
- **Rust:** Removed `long_only` entirely. Simplified the matching logic.
- **Transpiled C++:** Follows Rust's simplified design (no `long_only`).

This is a deliberate design change made during the Rust rewrite, not a translation artifact. The transpilation preserves the Rust team's design decisions.

**Data types:**
- **Original C++:** `const wchar_t** argv` (C-style), `const wchar_t* nextchar`, `int woptind/woptopt`
- **Rust:** `&mut [&wstr]` (mutable slice of string references), `&wstr remaining_text`, `usize wopt_index`, `char unrecognized_opt`
- **Transpiled C++:** `const wchar_t** argv`, `const wchar_t* nextchar`, `int woptind`, `wchar_t woptopt`

The transpiled C++ converges EXACTLY with the original's data representation. Rust's rich reference types (`&mut [&wstr]`) collapse back to C-style pointer arrays. Lifetimes disappear entirely.

### timer — Near-Perfect Convergence

**TimerSnapshot::take():** IDENTICAL across all three. Call `getrusage(RUSAGE_SELF)` and `getrusage(RUSAGE_CHILDREN)`, record `steady_clock::now()`.

**get_delta / print_delta:** IDENTICAL algorithm:
1. Compute per-component deltas (fish sys, fish usr, child sys, child usr)
2. Clamp to zero (getrusage results may be stale)
3. Select display units based on magnitude (micros→millis→seconds→minutes)
4. Format with %6.2f alignment

The verbose output format string is character-for-character identical across all three. Same column widths, same labels, same alignment logic.

**RAII pattern:**
- **Original C++:** `cleanup_t push_timer(bool enabled)` returns a lambda-based RAII object
- **Rust:** `push_timer() -> PrintElapsedOnDrop` with Drop trait implementation
- **Transpiled C++:** `TimerGuard push_timer()` with destructor

All three implement the same RAII pattern. The only difference: original accepts `bool enabled` (returns no-op lambda if false); Rust's version always creates a timer; transpiled follows Rust's unconditional design.

**Timeval conversion:**
- **Original C++:** `static int64_t micros(struct timeval t)` — templated with chrono overload
- **Rust:** `crate::nix::timeval_to_duration` — external utility
- **Transpiled C++:** `static int64_t micros(const struct timeval& t)` — local helper with chrono overload

Converges with original. The function signature and implementation are nearly identical.

## Patterns That Survived the Round-Trip Unchanged

| Pattern | Original C++ | → Rust → | Transpiled C++ |
|---------|-------------|----------|----------------|
| Named color table (22 entries) | constexpr array | const slice | constexpr array |
| Binary search on sorted table | std::lower_bound | binary_search_by | std::lower_bound |
| Case-insensitive compare | Manual ASCII fold | Manual ASCII fold | Manual ASCII fold |
| Squared-distance color matching | Manual loop | iter().min_by_key() | Manual loop |
| getrusage + steady_clock | Direct syscall | Direct syscall | Direct syscall |
| Timer unit selection thresholds | 900M/999995/1000 | Same values | Same values |
| RESP protocol permutation algorithm | GNU getopt logic | Same logic | Same logic |
| Long option prefix matching | wcsncmp + exact/ambig | starts_with + enum | wcsncmp + exact/ambig |

## Patterns That Changed in the Round-Trip

| Pattern | Original C++ | Rust changed to | Transpiled C++ |
|---------|-------------|-----------------|----------------|
| Array permutation (exchange) | Manual binary swap loop (44 lines) | `rotate_left` (1 line) | `std::rotate` (1 line) |
| Color parse failure | Constructor sets type_none | `Option<Self>` | `std::optional<RgbColor>` |
| Memory layout (color) | 4-byte bit-packed struct | ~8+ byte algebraic enum | 6-byte tagged union |
| Flag manipulation | Unnamed enum + manual ops | `bitflags!` type-safe methods | Constexpr namespace + manual ops |
| wgetopt long_only parameter | Present | Removed | Absent (follows Rust) |
| push_timer enabled param | `bool enabled` parameter | Always active | Always active (follows Rust) |
| String types | wcstring / wchar_t* | wstr / &wstr | wstring_view / wchar_t* |

## Assessment

### What the Round-Trip Reveals

The transpiled C++ does NOT converge with the original pre-2023 C++. It produces a **third, distinct version** that:

1. **Shares algorithms** with both (algorithms are language-independent — confirmed again)
2. **Follows Rust's API design** (std::optional for fallible parsing, no long_only parameter)
3. **Uses modern C++ idioms** that the original didn't (std::rotate, std::optional, std::wstring_view, enum class)
4. **Loses the original's micro-optimizations** (bit-packing in color, manual swap in exchange)

### What This Means for the Thesis

The fish shell rewrite (C++ → Rust) added value in three distinct ways:
1. **Safety enforcement** (lifetimes, ownership, Send+Sync) — disappears in transpilation
2. **API redesign** (Option<Self> instead of silent failure, removal of unnecessary parameters) — survives transpilation
3. **Adoption of stdlib primitives** (rotate_left instead of manual swap) — survives as std::rotate

The rewrite was an opportunity for **incremental modernization** under the guise of a language migration. The safety was real but temporary (it doesn't survive translation). The design improvements were permanent — they persist in any language the code is expressed in.

### The Expressiveness Question

These three modules contain no case where Rust's expressiveness exceeds C++. Every algorithm, every data structure, every control flow pattern has a direct C++ equivalent. The Rust-specific type machinery (`bitflags!`, algebraic enums with data, lifetime annotations on `WGetopter<'opts, 'args, 'argarray>`) adds compile-time verification but not runtime capability.

The one place Rust's type system adds genuine expressiveness is the `Type` enum with associated data (`Named { idx: u8 }`, `Rgb(Color24)`) — C++ cannot express this as cleanly without either a tagged union or `std::variant`. But the original C++ already used a tagged union, and the transpiled C++ returns to that same pattern. The round-trip is complete.
