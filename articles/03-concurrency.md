# The Reverse Rewrite: Transpiling Rust Back to C++ — Part 3: The Hard Case — mini-redis

*By Su Qingyue*

---

## The Question

This was the point where the experiment was most likely to reveal a limit.

In Parts 1 and 2, we translated hexyl (a hex viewer) and three coreutils (echo, cat, tr) from Rust to C++. The results were consistent: the C++ transpilations were shorter, all behavioral tests passed, and the translations were largely mechanical. But one reasonable objection remained: those projects were sequential, single-threaded programs. If there were a place where C++ might stop following so easily, async and concurrency would be it.

So we chose the hardest teaching project in the Rust ecosystem: tokio-rs/mini-redis.

## The Subject

mini-redis is Tokio's official example for learning async Rust. It implements a Redis server with:

- Async TCP accept loop with concurrent connection handling
- Key-value storage with background expiration (purge task on a timer)
- Pub/sub broadcast channels with dynamic subscription management
- Graceful shutdown via signal handling and connection draining

The codebase exercises nearly every major async primitive: `tokio::spawn`, `tokio::select!`, `async fn`, `broadcast::channel`, `Notify`, `Semaphore`, `Pin<Box<dyn Stream>>`, `StreamMap`, and `async_stream::stream!`.

| Component | Rust lines | C++ lines | Notes |
|-----------|----------:|----------:|-------|
| Frame codec | 302 | 265 | RESP protocol, nearly identical logic |
| Command parser | 149 | 110 | Cursor over array frames |
| Connection | 236 | 240 | BufWriter → manual buffers |
| Database | 369 | 387 | Custom broadcast channel adds lines |
| Commands | 945 | 572 | Collapsed trait objects, stream wrappers |
| Server | 370 | 255 | Simplified shutdown coordination |
| Shutdown | 49 | 65 | atomic bool + timer vs. broadcast receiver |
| Client + CLI | 973 | 372 | Simplified client API |
| **Total** | **3,393** | **2,266** | **−33%** |

The C++ translation is 33% shorter. This continues the trend: hexyl was 12% shorter, coreutils was 31% shorter. But unlike the previous projects, this reduction comes with an asterisk.

## The Method

The async runtime maps cleanly: Tokio → standalone ASIO (header-only, no Boost dependency) with C++20 coroutines. `tokio::spawn` becomes `asio::co_spawn`. Rust's `async fn` returning `impl Future<Output=T>` becomes `asio::awaitable<T>`. `TcpListener::accept().await` becomes `co_await acceptor.async_accept()`.

For the database layer, Rust's `Arc<Mutex<State>>` becomes `shared_ptr<Shared>` with `std::mutex`. The `BTreeSet<(Instant, String)>` for tracking key expirations becomes `std::set<pair<time_point, string>>` — same ordered data structure, same purge algorithm.

For the broadcast system, there is no C++ standard library equivalent. Rust's `tokio::sync::broadcast` is a multi-consumer channel where each subscriber receives every message sent after subscribing. We built a custom `BroadcastSender<T>` / `BroadcastReceiver<T>` template using a vector of weak pointers to subscriber queues, with ASIO timer cancellation for async wake-up.

Through seven of the eight major components, the translation remained mechanical — the same pattern-matching approach from Parts 1 and 2. Then we reached the subscribe command.

## Where It Stopped Being Mechanical: `tokio::select!`

The subscribe command is 349 lines of Rust and the most architecturally significant code in the project. A subscribed client enters a long-lived event loop that must simultaneously:

1. Deliver broadcast messages from any subscribed channel
2. Read new commands from the client (subscribe/unsubscribe)
3. Listen for server shutdown

Rust handles this with `tokio::select!`, which races three awaitables and runs whichever completes first:

```
select! {
    Some((channel, msg)) = subscriptions.next() => { ... }
    res = dst.read_frame() => { ... }
    _ = shutdown.recv() => { return Ok(()) }
}
```

The first branch uses `StreamMap` to merge messages from all subscribed broadcast channels into a single async stream, where each channel's receiver is wrapped in a `Pin<Box<dyn Stream>>` created by `async_stream::stream!`.

C++20 coroutines have no equivalent. There is no `select!`. There is no `StreamMap`. There is no way to race arbitrary awaitables.

ASIO's `experimental::awaitable_operators` provides `operator||` for racing two awaitables, but it doesn't support dynamic fan-in — racing N broadcast receivers where N changes as the client subscribes and unsubscribes. This is not a limitation of the C++ implementation; it is a limitation of the programming model.

## The Solution: Shared Wake-up Timer

We solved this by creating a single `asio::steady_timer` set to `time_point::max()` — effectively a condition variable for the async world. Three sources cancel this timer to wake the main loop:

**Broadcast messages:** Each `BroadcastReceiver` holds a `weak_ptr` to the shared timer. When `BroadcastSender::send()` delivers a message to a subscriber's queue, it also cancels the shared timer via the weak pointer. This aggregates notifications from all channels into one signal — the C++ equivalent of `StreamMap::next()`.

**Socket readability:** Before entering the wait, we register a callback with `socket.async_wait(wait_read)`. When data arrives on the TCP socket, the callback cancels the shared timer. The main loop then performs a non-blocking read and parses any buffered frames.

**Server shutdown:** A spawned watcher coroutine waits on the shutdown signal and cancels the shared timer when triggered.

The main loop becomes: drain pending broadcasts → non-blocking socket read → parse buffered commands → reset timer → wait for cancellation → repeat.

This is correct, event-driven (no polling), and handles all three sources of wakeup. But it required understanding cooperative scheduling semantics, timer cancellation as a signaling mechanism, and the relationship between `async_wait(wait_read)` and non-blocking reads — none of which are obvious from reading the Rust source.

## What This Reveals

The `select!` translation is the first case in this series where the translation was **not** mechanical.

In hexyl, every Rust pattern had a direct C++ counterpart. In coreutils, the algorithms were identical across all three languages. But in mini-redis, `tokio::select!` embodies a concurrent composition model — "race these futures, run whichever wins, cancel the rest" — that C++20 coroutines simply do not provide as a primitive.

The C++ solution works. It passes all tests. But an AI assistant performing this translation would need to:

1. Recognize that `co_await read_frame()` blocks the event loop and cannot coexist with broadcast delivery
2. Understand that `steady_timer::cancel()` can be used as an async notification mechanism
3. Design the shared timer pattern to aggregate notifications from multiple dynamic sources
4. Add non-blocking read methods to Connection that the Rust code never needed
5. Correctly handle the interaction between `async_wait(wait_read)` callbacks and timer cancellation

This is architectural design, not pattern matching. It requires understanding the semantics of cooperative scheduling at a level that Rust's `select!` macro intentionally abstracts away.

## The Blocking Semaphore Bug

A smaller but instructive bug appeared during transpilation development in the server's connection limiter. Rust uses `Arc<Semaphore>` with `semaphore.acquire().await` — an async operation that suspends the coroutine until a permit is available. The initial C++ translation naively used `std::counting_semaphore` with `acquire()` — a blocking call.

In a multi-threaded runtime, this would merely block one thread. In ASIO's single-threaded `io_context`, it froze the entire event loop. No connections could be accepted, no commands processed, no timers could fire. The server appeared to hang silently.

The fix was simple (replace with an atomic counter and a yield loop), but the bug illustrates a class of errors that Rust prevents structurally: you cannot accidentally call a blocking function from an async context because the type system distinguishes `acquire()` (sync, returns `SemaphorePermit`) from `acquire().await` (async, returns `impl Future<Output=SemaphorePermit>`). In C++, both look like function calls.

## The Collapsed Abstractions

Where the translation does work mechanically, the Rust abstractions collapse dramatically:

| Rust | C++ | What happened |
|------|-----|---------------|
| `Pin<Box<dyn Stream<Item=Bytes>>>` | eliminated | Receivers polled with `try_recv()` |
| `StreamMap<String, Messages>` | `vector<Subscription>` | No trait-object wrapping needed |
| `async_stream::stream!` | eliminated | No generator needed |
| `Bytes` (ref-counted) | `std::string` | Deep copy is fine here |
| `Send + Sync` bounds | implicit | Single-threaded context |
| `broadcast::Receiver` → `Stream` | `BroadcastReceiver::try_recv()` | Direct polling replaces stream |

The subscribe command drops from 349 Rust lines to a leaner C++ equivalent not because C++ is more expressive, but because Rust's type machinery (Pin, Box, dyn Stream, lifetime annotations on async streams) exists to satisfy the compiler's safety requirements rather than the programmer's intent. When those requirements are removed, the ceremony goes with them.

## The Numbers

| Metric | Value |
|--------|-------|
| Rust source lines (src/) | 3,393 |
| C++ source lines (src/) | 2,266 |
| Reduction | 33% (1,127 lines) |
| Behavioral tests | 10/10 passing |
| Tests covering pub/sub | 4 (basic, multi-sub, manage sub, error handling) |
| Tests covering expiration | 1 |
| Architectural redesigns | 1 (subscribe `select!` fan-in) |

## The Updated Translation Table

New patterns discovered in this project:

| Rust | C++ |
|------|-----|
| `tokio::spawn` | `asio::co_spawn` |
| `tokio::select!` | Shared `steady_timer` cancelled by multiple sources |
| `async fn` / `impl Future` | `asio::awaitable<T>` |
| `tokio::sync::broadcast` | Custom `BroadcastSender/Receiver<T>` |
| `tokio::sync::Notify` | `asio::steady_timer` (cancel to wake) |
| `tokio::sync::Semaphore` | `std::atomic<int>` counter |
| `Pin<Box<dyn Stream>>` | eliminated (direct polling) |
| `StreamMap` | `vector` of receivers |
| `async_stream::stream!` | eliminated |
| `Bytes` | `std::string` |
| `signal::ctrl_c()` | `asio::signal_set` |

## What mini-redis Shows

mini-redis was the case most likely to pressure the framing developed in the earlier pieces. Async and concurrency are often where Rust's practical advantage feels hardest to set aside.

The translation succeeded: 10/10 tests pass, the code is 33% shorter, and most components translated mechanically. But for the first time, we hit a pattern — `tokio::select!` with dynamic fan-in — that required genuine architectural redesign rather than pattern substitution.

This does not overturn the broader pattern from the earlier experiments, but it does refine it. The gap is not in what the languages can *do* — both can handle concurrent I/O correctly. The gap is in how naturally they *compose* concurrent operations. Rust's `select!` makes concurrent composition a first-class primitive; C++ makes you assemble it from lower-level parts.

Whether that composition advantage is worth the cost of Rust's type-level machinery (Pin, Box, dyn Stream, Send + Sync bounds) is a judgment call, not a technical fact. For the subscribe command, probably yes. For much of the rest of the codebase, the machinery added ceremony without adding comparable clarity.

## Next Up

In Part 4, we look at the fish shell. fish was rewritten from C++ to Rust in 2023. We'll transpile selected modules back to C++ and compare the result with the original pre-2023 C++ code. If the round-trip produces similar code, that points one way; if it produces radically different code, it points another.

---

*The complete source code for this transpilation is available in [`03-mini-redis`](../03-mini-redis/). The C++ version passes 10 behavioral tests covering key-value operations, pub/sub, expiration, and subscribe management.*
