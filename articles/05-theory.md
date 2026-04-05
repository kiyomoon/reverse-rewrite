# The Reverse Rewrite: Transpiling Rust Back to C++ — Part 5: Notes on Expressiveness, Rejection, and a Possible Third Path

*By Su Qingyue*

---

## The Evidence

Over four experiments, we transpiled approximately 9,500 lines of Rust from four well-known projects (eight distinct components) back into idiomatic C++:

| Article | Project | Rust lines | C++ lines | Reduction | Tests |
|---------|---------|----------:|----------:|----------:|------:|
| 1 | hexyl (hex viewer) | 2,392 | 2,111 | −12% | 26/26 |
| 2 | coreutils (echo, cat, tr) | 2,217 | 1,530 | −31% | 66/66 |
| 3 | mini-redis (async server) | 3,393 | 2,266 | −33% | 10/10 |
| 4 | fish shell (3 modules†) | 1,331 | 1,087 | −18% | n/a |
| **Total** | | **9,333** | **6,994** | **−25%** | **102/102** |

†fish shell's 3 modules represent ~1,300 of ~75,000 total Rust lines (<2% of the codebase); conclusions from that experiment should be extrapolated with caution.

Every behavioral test passed. Every algorithm was preserved. The C++ transpilations were consistently shorter — not because shorter is inherently better, but because Rust's type-level safety machinery adds code that exists for the compiler, not the programmer. That extra code buys compile-time verification — a real benefit. But when the compiler is removed from the equation, the machinery collapses, and what remains is the behavioral core.

Article 4 added a counter-direction data point: fish shell's human-written Rust rewrite is 16–21% *larger* than its original C++ (the range depends on whether blank lines and comments are included in the count). This confirms that the line count differences across Articles 1–3 were not an intrinsic property of the languages, but a property of the translation direction.

This article steps back from code and asks what these results seem to suggest.

## Two Properties, Often Conflated

Discussions around Rust and C++ often bundle together one or both of the following claims:

1. **Rust is more expressive** — it lets you write programs that are harder or impossible to write in C++.
2. **Rust is safer** — its compiler rejects programs that C++ would accept, catching bugs at compile time.

These are different claims. They can be tested independently, but not always with the same level of confidence. Our experiments speak to them in a bounded way.

### Expressiveness

In the strict formal sense, expressiveness is about what a language can compute — and since both Rust and C++ are Turing-complete, they are trivially equivalent. The more useful notion (following Felleisen, 1991) is *practical expressiveness*: whether a construct in L₁ can be translated to L₂ by local, mechanical transformation, or whether it requires global restructuring. A language is more practically expressive when it provides constructs that require non-local redesign to replicate elsewhere.

The reverse-transpilation experiment tests this directly. If a Rust program can be translated to C++ by local pattern substitution — same algorithms, same behavior, same tests passing — then C++ appears practically as expressive as Rust for that domain. The Rust is not obviously "saying" anything the C++ cannot say with comparable effort.

Across 9,333 lines of Rust spanning hex viewers, Unix utilities, async servers, and shell internals, we found one clear case where the translation required *architectural redesign*: `tokio::select!` with dynamic fan-in in mini-redis. A handful of other cases — building a custom broadcast channel, diagnosing a blocking-in-async bug — required domain expertise beyond pattern matching, but did not require restructuring the program's architecture. In most of the material we examined, the translation was much closer to pattern substitution than to reinvention.

Even `select!` is not an expressiveness gap in the formal sense. The C++ version expresses the same concurrent behavior — it just assembles it from lower-level parts (timer cancellation, socket readability callbacks) rather than a single macro primitive. The C++ program exists; it works; it passes the same tests. The gap is in *composability*, not *expressiveness*.

### Rejection

A language L₁ *rejects more* than L₂ if programs that compile in L₂ fail to compile in L₁. The rejected programs include both incorrect programs (bugs) and correct programs that the compiler cannot prove correct (false positives).

This is Rust's actual advantage over C++. The borrow checker rejects programs with dangling references. The type system rejects programs that send non-`Send` types across threads. The lifetime annotations reject programs where references outlive their referents.

None of these properties survived any of our translations. Consider:

**Lifetimes.** Rust's `WGetopter<'opts, 'args, 'argarray>` carries three lifetime parameters ensuring that option strings, argument strings, and the argv array all remain valid while the parser exists. The C++ `WGetopter` has zero lifetime parameters. The same correctness property must hold — but nothing enforces it.

**Send + Sync.** mini-redis's `Arc<Mutex<State>>` with `State: Send + Sync` guarantees at compile time that the shared database is safe to access from multiple tasks. The C++ `shared_ptr<Shared>` with `std::mutex` works correctly — but a programmer could accidentally share a non-thread-safe object and the compiler would not complain.

**Borrow exclusivity.** hexyl's `&mut self` methods guarantee exclusive access. C++ has no equivalent — the programmer simply knows not to call methods from multiple threads simultaneously.

In every case, the *structure* was preserved but the *guarantee* was lost. The transpiled C++ does the right thing, but nothing in its own type system proves it does.

## A Taxonomy of Translation

Our experiments reveal four categories of Rust constructs, distinguished by what happens when they are translated to C++:

### Category 1: Direct Equivalents

These translate one-to-one with no loss of meaning:

| Rust | C++ |
|------|-----|
| `Option<T>` | `std::optional<T>` |
| `Result<T, E>` | `std::expected<T, E>` |
| `Vec<T>` | `std::vector<T>` |
| `String` / `&str` | `std::string` / `std::string_view` |
| `enum class` (simple) | `enum class` |
| `match` (simple) | `switch` |
| `impl` block | Class methods |
| `const fn` | `constexpr` |
| `Box<dyn Trait>` | `std::unique_ptr<Interface>` |
| `Arc<Mutex<T>>` | `shared_ptr<mutex>` + lock |

These represent shared infrastructure between the languages. They are why the translation is mostly mechanical.

### Category 2: Collapsed Abstractions

These are Rust constructs that exist to satisfy the compiler's proof obligations. They have C++ counterparts that are simpler because the proof obligations are absent:

| Rust | C++ | What collapsed |
|------|-----|----------------|
| `Printer<'a, Writer: Write>` | `Printer` with `ostream&` | Lifetime annotation, Writer generic |
| `Pin<Box<dyn Stream>>` | eliminated | Pinning, heap allocation, trait object |
| `BufWriter<StdoutLock<'_>>` | `OutputBuffer` | Lock lifetime |
| `impl SymbolTranslator for X` (5 types) | 5 free functions | Trait hierarchy |
| `StreamMap<String, Messages>` | `vector<Subscription>` | Stream composition |
| `async_stream::stream!` | eliminated | Generator/stream adapter |
| `Send + Sync` bounds | implicit | Thread safety markers |
| `thiserror` derives | `fprintf(stderr, ...)` | Error type hierarchy |

This category helps explain why the C++ transpilations are often shorter in this project. These abstractions add lines to the Rust source that have little direct behavioral content — they exist to help the compiler verify safety properties. When verification is removed, many of those lines disappear.

### Category 3: Safety Annotations

These are type-level markers that carry proof information. They vanish completely in translation:

| Rust | C++ |
|------|-----|
| `&'a T` | `const T&` (no lifetime) |
| `&'a mut T` | `T&` (no exclusivity) |
| `WGetopter<'opts, 'args, 'argarray>` | `WGetopter` |
| `T: Send + Sync` | (nothing) |
| `pub(crate)` | (convention) |
| `#[non_exhaustive]` | (nothing) |

This is where Rust's real value lies — and where it is most fragile. The annotations carry information that exists *only* in the compiler's verification pass. They leave no trace in the binary, and they leave no trace in the transpiled source.

### Category 4: Composition Primitives

These represent genuine capability gaps where C++ requires manual assembly:

| Rust | C++ | Gap |
|------|-----|-----|
| `tokio::select!` | Shared timer + callbacks | Concurrent fan-in composition |
| `tokio::sync::broadcast` | Custom implementation | No stdlib equivalent |

This is the smallest category. Across 9,333 lines, we found exactly one composition primitive (`select!`) that required architectural redesign. Everything else was pattern substitution.

## Three Observations

The experiments suggest three empirical patterns:

### 1. Algorithms are language-independent

Every algorithm we examined — carry-propagation line numbering, M-^X nonprint notation, 256-element byte lookup tables, squared-distance color matching, binary search on sorted tables, GNU getopt option permutation, RESP protocol parsing, key-value expiration with priority queues, timer formatting with unit selection — was identical across all languages.

This held across four projects (eight components), three languages (C, Rust, C++), and both translation directions. Not a single algorithm clearly changed in translation. We did not see a Rust feature forcing a fundamentally different algorithmic approach. (The fish shell's `exchange` function changed from a manual swap loop to `std::rotate` — but `std::rotate` implements the same mathematical operation. The improvement was adopting a stdlib primitive, not changing the algorithm.)

This is not surprising in retrospect. Algorithms operate on abstract data; languages provide concrete syntax. But it is worth stating explicitly, because the "Rewrite it in Rust" discourse sometimes implies that Rust's type system enables better algorithms. These experiments did not show that.

### 2. Safety is a property of the compilation pipeline, not the source code

Consider a Rust program that passes the borrow checker. Its safety is verified. Now transpile it to C++. The C++ program does the same thing — same operations, same data flow, same lifetimes in practice. But the C++ compiler has not verified any of this.

Where did the safety go? It did not disappear from the *program* — the memory access patterns are the same. It disappeared from the *compilation pipeline*. The Rust compiler performed a verification pass and accepted the program. The C++ compiler performed no such pass.

This means safety is not *expressed* in the source code. It is *enforced* by the compiler. The source code merely provides enough annotations (lifetimes, borrow markers, Send/Sync bounds) for the compiler to perform its analysis. Change the compiler pipeline, and those annotations no longer carry the same enforceable meaning.

This suggests a more modest implication: some safety reasoning may be transportable, provided the translation is faithful enough and the result is re-verified aggressively. That is much weaker than saying safety transfers automatically. The verification does not have to live only in the compiler, but if it moves elsewhere, the burden of trust moves with it.

The critical caveat: transportability depends on the fidelity of the translation. If the transpilation introduces a subtle bug — a use-after-free that the Rust version's ownership model prevented — the safety guarantee does not carry over. Our experiments used behavioral test suites (102 tests across all projects) as the fidelity check, which provides practical confidence but not formal proof. The gap between "passes all tests" and "provably semantics-preserving" remains open.

### 3. Design improvements are permanent; safety enforcement is temporary

In the fish shell round-trip, we found three kinds of changes:

- **Design improvements** (std::optional for fallible parsing, std::rotate for array permutation) survived the round-trip. They are better regardless of language.
- **Safety enforcement** (lifetimes, ownership tracking) did not survive. They depend on the specific compiler's verification pass.
- **Micro-optimizations** (4-byte bit-packed color struct) did not survive. They depend on the specific language's memory model.

This hierarchy — design > optimization > safety — reflects a hierarchy of where the improvement lives:

- Design improvements live in the *algorithm and API*. They transfer across any translation.
- Micro-optimizations live in the *language's memory model*. They are lost in translation unless the translator knows to recreate them.
- Safety enforcement lives in the *compiler's verification pass*. It is lost whenever the compiler changes.

The fish shell rewrite looks valuable not only because of safety, but because it also created an opportunity for modernization that would persist across languages. In that sense, the rewrite acted as a *catalyst for design improvement*, while the strongest safety guarantees remained tied to the Rust toolchain.

## A Possible Third Path

The "Rewrite it in Rust" movement is often framed as a binary choice: keep your C++ and accept its risks, or migrate to Rust and adopt its guarantees. These experiments suggest a possible third option worth thinking about, though certainly not yet proving.

### A Possible Workflow

Consider the following workflow:

1. A C++ program exists with potential safety issues.
2. An AI transpiles it to Rust.
3. The Rust compiler verifies the program (or identifies issues).
4. The AI fixes any issues and transpiles back to C++.
5. The result is a C++ program shaped by a Rust-verified version of the same design.

That result would not be "as safe as Rust" by decree. It would be C++ code that has been translated, checked, and regression-tested against a stronger verification environment. The promise of such a workflow depends entirely on the fidelity of the translation and the seriousness of the validation around it.

This remains speculative. Our experiments show that algorithms can survive the round-trip and that much of Rust's safety story lives in verification rather than syntax. They do not, by themselves, establish a semantics-preserving compiler pipeline.

### What This Requires

For the third path to work in practice, three things are needed:

**1. Reliable transpilation.** Our experiments suggest this is achievable for much of the code we tested (Categories 1 and 2) but challenging for concurrent composition (Category 4).

**2. Comprehensive test suites.** Without behavioral tests, there is no confidence that the transpilation preserves semantics. This is the same requirement as any refactoring.

**3. Periodic re-verification.** As the C++ code evolves, it may drift from the verified structure. Periodic re-transpilation to Rust, re-verification, and comparison can catch this.

### What This Does Not Replace

The third path does not eliminate the need for safety-aware programming. It does not make C++ as safe as Rust for *new* code. It does not provide the ongoing, incremental verification that Rust's compiler provides on every build.

What it does is suggest a pragmatic alternative for the vast existing codebase of C++ software that will never be rewritten in Rust wholesale. For this code, the practical choice is often not between ideal options, but between incremental improvement and none.

### The Obvious Counter-Argument

"If the code is already in Rust after step 3, why not just stay in Rust?"

This is a fair question, and for greenfield projects or small codebases, the answer may well be: stay in Rust. But for the target audience of the third path — large, established C++ codebases — there are concrete reasons to translate back:

- **Team expertise.** A 200-person C++ team cannot switch to Rust overnight. The organizational cost of language migration often exceeds the technical cost.
- **Ecosystem integration.** The C++ program exists within a larger build system, testing infrastructure, and deployment pipeline. Replacing it with Rust may require replacing the entire toolchain.
- **Incremental adoption.** The third path can be applied module-by-module, verifying safety of the most critical components without migrating the entire project.
- **Ongoing maintenance.** If the team will maintain the code in C++ regardless, the round-trip provides a safety audit of the *existing* language rather than a migration to a new one.

The third path is not "better than Rust." At best, it is a workflow that may still be better than doing nothing for codebases where a full Rust migration is not realistic.

## One Distinction That Kept Reappearing

One recurring pattern in the broader "Rewrite it in Rust" discussion is that two different properties are often bundled together:

**What Rust lets you write** (expressiveness) — looked much closer to C++ than the surrounding discourse often suggests. Across 9,333 lines spanning four projects, we were able to reproduce the same programs and behaviors in C++.

**What Rust prevents you from writing** (rejection) — remained the more meaningful difference in this project. The borrow checker, lifetime system, and Send/Sync markers catch real bugs at compile time. No standard C++ toolchain provides the same level of guarantee by default.

But rejection is a property of the *compilation pipeline*, not the *language*. It may also be partially approached through other means: static analysis tools, formal verification, AI-assisted review, or round-trip workflows through languages that provide stronger guarantees.

Rust's value is real. But in these experiments, it showed up less in surface syntax than in the verification pass that happens between `cargo build` and the binary. That does not make the language irrelevant; it does suggest that the toolchain is carrying more of the practical weight than a casual reading of the syntax might imply.

## Conclusion

We set out to test a simple question: can Rust programs be mechanically translated back to C++? For the material in this project, the answer was mostly yes, with one notable exception (concurrent composition via `select!`). The resulting translations were often shorter, passed all tests we built around them, and preserved the observed algorithms.

This tells us three things:

**First**, within this sample, Rust and C++ looked far closer in expressiveness than they are often presented as being. The strong claim that Rust is categorically "more expressive" was not supported by the evidence we gathered here.

**Second**, Rust's strongest practical advantage here — compiler-enforced memory safety — behaved more like a property of the verification pipeline than a property of the translated source itself. It did not survive translation, because it was enforced by the compiler.

**Third**, the experiments suggest a possible workflow for C++ codebases that cannot realistically be rewritten wholesale: combine stronger verification environments, AI-assisted translation, and aggressive testing, rather than treating language migration as the only serious option.

Rust is a remarkable language. Its compiler catches bugs that no other mainstream language catches. But its value is often described too loosely. In these experiments, what mattered most was not that Rust offered a radically different way to state the program, but that it imposed a stronger way to check it.

That does not make verification free, or automatically transferable. It does suggest that the boundary between language choice and verification strategy is more flexible than the usual debate admits.

---

*This article concludes the series. The complete source code for all four transpilation experiments is available in [the repository root](..). 102 behavioral tests verify the C++ transpilations across all projects.*

# Appendix: Can C++ Simulate Rust's `Send`?

Throughout this series, we cataloged Rust constructs that collapse or vanish in translation to C++. Most are Category 2 (collapsed abstractions) or Category 3 (safety annotations) — they disappear because C++ doesn't need them or can't enforce them.

But one construct sits at the boundary: Rust's `Send` trait. It is the linchpin of Rust's "fearless concurrency" story, and it works through a mechanism that no current C++ feature can replicate: **automatic recursive derivation over a type's fields.**

C++26's static reflection (P2996) changes this — partially.

## What `Send` Actually Does

In Rust, `Send` is a marker trait indicating that a value can be safely transferred to another thread. The compiler derives it automatically: if every field of your struct is `Send`, your struct is `Send`. If any field is not (e.g., `Rc<T>`, a non-atomic reference count), the whole struct is not, and any attempt to pass it to `thread::spawn` fails at compile time.

```rust
// This just works. The compiler checks every field recursively.
struct GameState {
    score: u64,              // Send
    name: String,            // Send
    data: Vec<u8>,           // Send
}
// GameState is automatically Send.

struct BadState {
    cache: Rc<HashMap<String, String>>,  // NOT Send (non-atomic refcount)
}
// BadState is NOT Send. thread::spawn won't accept it.
```

No annotation required. No opt-in. The compiler does it.

## The C++ Status Quo: Concepts Without Inspection

C++20 concepts can express the *constraint* — "this function only accepts `Sendable` types" — but cannot *derive* whether a type satisfies it. You'd have to manually specialize for every type:

```cpp
template <typename T>
concept Sendable = /* what goes here? */;

// You can't write: "Sendable if all members are Sendable."
// C++20 has no way to iterate over a struct's fields at compile time.
```

This is the gap. The concept exists as a constraint mechanism, but without the ability to inspect a type's members, every type must be manually tagged. That doesn't scale, and in practice nobody does it.

## C++26 Static Reflection Closes the Derivation Gap

P2996 gives C++ compile-time access to a type's structure. With it, we can write automatic `Send` derivation:

```cpp
#include <experimental/meta>
#include <type_traits>
#include <memory>
#include <string>
#include <vector>

// Base case: primitives and standard safe types are Sendable.
template <typename T>
constexpr bool is_sendable_v = std::is_arithmetic_v<T>;

template <>
constexpr bool is_sendable_v<std::string> = true;

template <typename T>
constexpr bool is_sendable_v<std::vector<T>> = is_sendable_v<T>;

template <typename T>
constexpr bool is_sendable_v<std::unique_ptr<T>> = is_sendable_v<T>;

// shared_ptr is Sendable (atomic refcount), unlike Rust's Rc.
template <typename T>
constexpr bool is_sendable_v<std::shared_ptr<T>> = is_sendable_v<T>;

// Recursive derivation: a struct is Sendable if all its fields are.
template <typename T>
    requires std::is_class_v<T>
consteval bool derive_sendable() {
    bool result = true;
    [:expand(std::meta::nonstatic_data_members_of(^T)):] >> [&]<auto member> {
        using FieldType = typename[:std::meta::type_of(member):];
        if (!is_sendable_v<FieldType>)
            result = false;
    };
    return result;
}

// Non-atomic reference count — NOT Sendable (analogous to Rust's Rc).
template <typename T>
struct LocalRc { /* non-atomic refcount */ };

template <typename T>
constexpr bool is_sendable_v<LocalRc<T>> = false;  // explicit opt-out

// The constraint.
template <typename T>
concept Sendable = is_sendable_v<T> || (std::is_class_v<T> && derive_sendable<T>());

// A spawn function that enforces the constraint.
template <Sendable T>
void safe_spawn(std::function<void(T)> fn, T value);
```

Now:

```cpp
struct GameState {
    uint64_t score;
    std::string name;
    std::vector<uint8_t> data;
};
// derive_sendable<GameState>() == true. All fields are Sendable.

struct BadState {
    LocalRc<std::unordered_map<std::string, std::string>> cache;
};
// derive_sendable<BadState>() == false. LocalRc is not Sendable.

safe_spawn(process, GameState{});   // ✓ compiles
safe_spawn(process, BadState{});    // ✗ compile error
```

This is structurally equivalent to what Rust's compiler does: recursive field inspection, automatic derivation, compile-time rejection.

## The Gap That Remains

The derivation works. The constraint works. But there is nothing forcing anyone to use `safe_spawn`. The standard `std::thread` constructor accepts any callable — no `Sendable` check, no compiler error:

```cpp
BadState bad;
std::thread([bad]{ /* data race waiting to happen */ }).detach();  // compiles fine
```

In Rust, there is no way to spawn a thread without going through the `Send` gate. `std::thread::spawn` requires `Send` in its signature, and there is no alternative in the standard library that skips the check. (You can bypass it with `unsafe`, but that makes the escape explicit and auditable.)

In C++, the gate is optional. You can build it, and your team can agree to use it. But the language does not force the agreement.

## What This Illustrates

This is the expressiveness/rejection distinction in miniature:

- **Expressiveness**: C++26 can *express* the concept of `Send` — automatic derivation, recursive field checking, compile-time constraint. The mechanism is there. ✓
- **Rejection**: C++ cannot *reject* code that bypasses the check. The standard threading API does not require `Sendable`, and C++ has no way to retroactively add constraints to `std::thread`. ✗

Rust's advantage is not that it *can* check `Send` — with reflection, C++ can too. The advantage is that it *always* checks. The check is not a library convention; it is a language invariant. You cannot opt out without writing `unsafe`.

This is, in a single example, a compact version of the pattern that kept reappearing in the series. The languages looked much closer in what they could express than in what they could reject. And that difference lived in the compilation pipeline — in whether the verification was mandatory or voluntary.
