# The Reverse Rewrite

This repository archives a completed set of reverse-transpilation experiments: four Rust codebases were translated back into idiomatic modern C++, then compared against the Rust originals through behavioral tests, code structure, and translation notes.

The project name is **The Reverse Rewrite**. "Rust to C++" is still the most direct short description of what the experiment does, but the repository is meant to read more as an archive of the series than as a conversion tool or a one-line thesis.

The motivating question was narrow:

> If a Rust program can be translated back into reasonable C++, what seems to survive the translation, and what does not?

The working conclusion of the series is also narrow:

- In these case studies, the main difference showed up less in raw expressiveness than in compiler-enforced rejection of unsafe programs.
- That distinction matters, but it should still be read as an empirical pattern from a bounded sample, not as a formal proof.

This repository is meant as a readable archive of the experiment, not as a final word on Rust, C++, or language design.

If you want the reading version rather than the repository view, publish this branch with GitHub Pages and use [`index.html`](index.html) as the entry point.

## Repository Status

Completed and archived as a self-contained project.

- The article series is finished.
- The code and notes are kept here as reference material.
- The repository may inform later work on C++ safety tooling and AI-assisted remediation, but that would be a separate project.

## Contents

- [`articles/`](articles/) contains the six published pieces:
  - [`01-methodology.md`](articles/01-methodology.md)
  - [`02-coreutils.md`](articles/02-coreutils.md)
  - [`03-concurrency.md`](articles/03-concurrency.md)
  - [`04-fish-shell.md`](articles/04-fish-shell.md)
  - [`05-theory.md`](articles/05-theory.md)
  - [`06-afterword.md`](articles/06-afterword.md)
- [`01-hexyl/`](01-hexyl/) contains the first full transpilation, notes, and C++ test harness.
- [`02-coreutils/`](02-coreutils/) contains three GNU/uutils/C++ comparisons: `echo`, `cat`, and `tr`.
- [`03-mini-redis/`](03-mini-redis/) contains the async/concurrency case study and C++ behavioral test script.
- [`04-fish-shell/`](04-fish-shell/) contains the round-trip comparison for three fish shell modules.

## Results

| Article | Project | Rust lines | C++ lines | Tests | Main finding |
| --- | --- | ---: | ---: | ---: | --- |
| 1 | hexyl | 2,392 | 2,111 | 26/26 | Mostly mechanical translation; safety annotations vanish |
| 2 | coreutils (`echo`, `cat`, `tr`) | 2,217 | 1,530 | 66/66 | Algorithms stay intact across GNU C, Rust, and C++ |
| 3 | mini-redis | 3,393 | 2,266 | 10/10 | `tokio::select!` required architectural redesign |
| 4 | fish shell (3 modules) | 1,331 | 1,087 | n/a | Round-trip produces a third version, not the original C++ |
| 5 | theory | — | — | — | The experiments suggest separating expressiveness from rejection |

Totals across the transpilation experiments:

- 9,333 lines of Rust translated to 6,994 lines of C++
- 102/102 reported behavioral tests passing
- One clear case of non-mechanical redesign: async fan-in around `tokio::select!`

## Reading Order

If you only want the shortest path through the project:

1. [`articles/01-methodology.md`](articles/01-methodology.md) for the basic method
2. [`articles/03-concurrency.md`](articles/03-concurrency.md) for the hardest case
3. [`articles/04-fish-shell.md`](articles/04-fish-shell.md) for the round-trip comparison
4. [`articles/05-theory.md`](articles/05-theory.md) for the synthesis and open questions

If you want the full project record, read the articles in numeric order and use the experiment directories as supporting material.

## Reproducing What Is Here

The repository is organized by case study rather than by a single top-level build.

### 01-hexyl

```bash
cd 01-hexyl/translated-cpp
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

### 02-coreutils

Each utility is separate:

```bash
cd 02-coreutils/echo/translated-cpp
cmake -S . -B build
cmake --build build
```

```bash
cd 02-coreutils/cat/translated-cpp
cmake -S . -B build
cmake --build build
```

```bash
cd 02-coreutils/tr/translated-cpp
cmake -S . -B build
cmake --build build
```

The full behavioral harness described in the article is summarized in the notes, but unlike `hexyl` and `mini-redis`, it is not packaged here as one integrated test runner.

### 03-mini-redis

```bash
cd 03-mini-redis/translated-cpp
cmake -S . -B build
cmake --build build
bash test.sh
```

### 04-fish-shell

This directory is primarily a comparative archive of source snapshots, translated modules, and notes. It is the least packaged of the four experiments.

## Main Limitations

This repository supports an empirical argument, not a proof.

- Behavioral equivalence is tested, not formally proven.
- The fish-shell case study covers three modules, not the whole codebase.
- Line-count comparisons depend on scope and counting rules.
- Translation fidelity is strongest where the original projects already had clear tests.
- The "third path" discussed in the theory article should be read as a possible workflow worth exploring, not as a finished industrial workflow.

## Why Keep This Repository

The project started as a language question and ended closer to a tooling question:

- How much of Rust's practical value comes from what the language expresses?
- How much comes from what its compiler verifies?
- If verification and production language can be partially separated, what does that mean for existing C++ codebases?

Those questions remain open. This repository is one concrete attempt to put some evidence on the table.

## Authors

Su Qingyue designed the methodology, shaped the framing of the project, and assembled the final archive. The transpilation experiments, article drafts, and repository materials were produced through AI-assisted workflows and then refined into their final form.
