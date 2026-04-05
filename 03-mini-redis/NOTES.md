# Article 3: mini-redis — Translation Notes

## Project

**Source:** [tokio-rs/mini-redis](https://github.com/tokio-rs/mini-redis) (commit `ceb481a`)
**Description:** Tokio's teaching implementation of a Redis server — async TCP, key-value storage with expiration, pub/sub broadcast channels, concurrent connection handling.

## Line Counts

| Language | Lines | Change |
|----------|------:|-------:|
| Rust (src/) | 3,393 | — |
| C++ (src/) | 2,266 | **−33%** |

The C++ transpilation is 33% shorter, continuing the pattern from Articles 1 and 2 (12% and 31% respectively). The reduction comes primarily from collapsing Rust's type-level abstractions (trait objects, Pin, Box, Stream, async_stream) into direct code.

## Architecture

| Rust | C++ |
|------|-----|
| Tokio runtime | ASIO standalone (1.30) + C++20 coroutines |
| `tokio::spawn` | `asio::co_spawn` |
| `async fn` → `impl Future` | `asio::awaitable<T>` |
| `TcpListener` / `TcpStream` | `asio::ip::tcp::acceptor` / `socket` |
| `BufWriter<TcpStream>` + `BytesMut` | Manual `vector<uint8_t>` read/write buffers |
| `broadcast::Sender<Bytes>` | Custom `BroadcastSender<string>` (template) |
| `broadcast::Receiver` | `BroadcastReceiver<string>` with timer notification |
| `tokio::sync::Notify` | `asio::steady_timer` (cancel to wake) |
| `Arc<Mutex<State>>` | `shared_ptr<Shared>` with `std::mutex` |
| `BTreeSet<(Instant, String)>` | `std::set<pair<time_point, string>>` |
| Signal handling (`tokio::signal`) | `asio::signal_set` |

## Key Translation Decisions

### 1. The `select!` Problem — The Hardest Translation

Rust's `tokio::select!` is the centerpiece of the subscribe command. It races three awaitables:

```rust
select! {
    Some((channel, msg)) = subscriptions.next() => { /* broadcast */ }
    res = dst.read_frame() => { /* client command */ }
    _ = shutdown.recv() => { return Ok(()) }
}
```

C++20 coroutines have no built-in equivalent. ASIO's `experimental::awaitable_operators` provides `operator||` for racing two awaitables, but it doesn't support dynamic fan-in (racing N broadcast receivers where N changes at runtime).

**Solution: Shared wake-up timer**

We create a single `asio::steady_timer` set to `time_point::max()`. Three sources cancel it to wake the main loop:

1. **Broadcast messages** — `BroadcastSender::send()` cancels each subscriber's `external_notify` timer (a `weak_ptr` to the shared timer).
2. **Socket readability** — `socket.async_wait(wait_read, callback)` fires a callback that cancels the timer.
3. **Server shutdown** — A spawned watcher coroutine waits on the shutdown signal and cancels the timer.

The main loop: drain broadcasts → read available socket data (non-blocking) → parse buffered commands → wait on shared timer → repeat.

This is architecturally different from Rust's zero-cost select. It's event-driven (not polling), but requires manual plumbing that Rust provides as a language primitive. This is the one place where the translation is genuinely **not mechanical**.

### 2. Broadcast Channel

Rust provides `tokio::sync::broadcast` — a multi-producer, multi-consumer channel. No C++ equivalent exists.

Custom implementation: `BroadcastSender<T>` holds a `vector<weak_ptr<Subscriber>>`. Each subscriber has a mutex-protected pending message queue and an ASIO timer for async notification. `send()` pushes to each subscriber's queue and cancels their timer. Dead subscribers (expired `weak_ptr`) are cleaned up lazily.

Key addition for subscribe multiplexing: each subscriber has an `external_notify` (`weak_ptr<steady_timer>`) that is also cancelled on message arrival, enabling the shared wake-up pattern described above.

### 3. Connection — Non-blocking Read Methods

The original Connection only had `read_frame()` (fully async, blocks until a complete frame arrives). For the subscribe loop, we added:

- `try_parse_buffered()` — Synchronous: check if the read buffer already contains a complete RESP frame. No I/O.
- `read_available()` — Synchronous non-blocking: toggle `socket_.non_blocking(true)`, attempt `read_some()`, restore. Returns false on EOF.
- `socket()` accessor — Expose the socket for `async_wait(wait_read)` registration.

These enable the subscribe loop to check for commands without blocking.

### 4. Shutdown Mechanism

Rust uses `broadcast::Receiver<()>` — every connection gets a receiver that fires on shutdown.

C++ uses `ShutdownSignal` (shared `atomic<bool>` + `steady_timer`). Each connection's `Shutdown` wrapper checks the atomic and can `co_await` the timer. Triggering shutdown: set the atomic and cancel the timer.

### 5. Connection Limiting

Rust uses `Arc<Semaphore>` with `acquire()`. Initial C++ translation used `std::counting_semaphore`, but `acquire()` is a **blocking call** that froze the single-threaded ASIO event loop.

Fix: replaced with `std::atomic<int>` counter. The accept loop checks the counter and yields briefly (10ms timer) if at capacity.

### 6. Graceful Shutdown Sequence

Rust uses a combination of mechanisms:
- `broadcast::Sender` for signaling all connections
- `mpsc::Sender` (drop detection) to wait for all connections to finish

C++ simplifies this:
- A spawned coroutine waits for shutdown then closes the acceptor (breaking the accept loop)
- An `atomic<int>` counter tracks active connections
- A `steady_timer` waits for the counter to reach zero

### 7. Frame Parsing

Rust's `Frame` is an enum with six variants. C++ maps to `std::variant<Simple, Error, Integer, Bulk, Null, Array>` with factory methods (`Frame::simple()`, `Frame::bulk()`, etc.).

The RESP parser uses cursor-based position tracking (matching Rust's `Cursor<&[u8]>` pattern). `frame_check()` validates completeness; `frame_parse()` extracts the frame.

### 8. Command Dispatch

Rust's `Command` enum with per-variant `apply()` → C++ `std::variant` with `std::visit` dispatch. Each command is a struct with `parse_frames()`, `apply()`, and `into_frame()` methods.

### 9. Collapsed Abstractions

Several Rust abstractions that exist for type safety or trait compliance collapse entirely in C++:

| Rust | C++ | Why |
|------|-----|-----|
| `Pin<Box<dyn Stream<Item=Bytes>>>` | eliminated | Receivers polled directly with `try_recv()` |
| `StreamMap<String, Messages>` | `vector<Subscription>` | No need for trait-object wrapping |
| `async_stream::stream!` macro | eliminated | No generator needed for broadcast → stream |
| `Bytes` (ref-counted byte buffer) | `std::string` | Deep copy is fine for this use case |
| `Send + Sync` trait bounds | implicit | Single-threaded `io_context`, no cross-thread sharing |
| `DbDropGuard` (Drop impl) | `DbDropGuard` (destructor) | 1:1 mapping |
| `impl Future for ...` | `asio::awaitable<T>` | Direct mapping |

### 10. `Bytes` → `std::string`

Rust's `Bytes` is a reference-counted, cheaply-cloneable byte buffer. The C++ translation uses `std::string` (deep copy). This is less efficient for large payloads but correct, and avoids introducing a custom ref-counted buffer for a teaching project.

## What Worked Mechanically

Most of the translation was straightforward pattern-matching:

- **Key-value store**: `HashMap` → `unordered_map`, `BTreeSet` → `std::set`, `Mutex` → `std::mutex`
- **Command parsing**: `Parse` struct with cursor — nearly identical logic
- **Frame codec**: RESP parsing with cursor positions — line-by-line correspondence
- **Server accept loop**: `loop { accept() }` → `while (!shutdown) { co_await accept() }`
- **Expiration purge task**: Background coroutine with timer sleep — identical algorithm

## What Did NOT Work Mechanically

1. **`tokio::select!`** — Required fundamental architectural redesign. The shared wake-up timer pattern is correct but non-obvious. An AI assistant would need to understand cooperative scheduling, timer cancellation semantics, and the relationship between async_wait and non-blocking reads.

2. **Blocking semaphore in async context** — `std::counting_semaphore::acquire()` blocks the thread, freezing the event loop. This is a subtle bug that only manifests at runtime. Rust's `Semaphore::acquire().await` is async by design.

3. **Socket readability detection** — Rust's `select!` transparently handles "is there data on the socket?". In C++, we had to expose the socket, use `async_wait(wait_read)` with a callback, and add non-blocking read methods. This is plumbing that Rust hides.

## Bugs Found During Testing

1. **Test harness stdout pipe**: Server process inherited the test subshell's stdout pipe. The `$()` command substitution hung waiting for the pipe to close. Fix: redirect server stdout to `/dev/null`.

2. **Blocking semaphore deadlock**: `std::counting_semaphore::acquire()` blocked the ASIO event loop. No connections could be processed while waiting for a permit. Fix: atomic counter with yield.

3. **Acceptor not cancelled on shutdown**: The accept loop blocked on `co_await accept()` even after shutdown was signaled. Fix: spawned coroutine that closes the acceptor on shutdown.

## Assessment

mini-redis is Rust's strongest case: async I/O, concurrent connections, channels, and `select!`. The C++ translation is **correct** (10/10 behavioral tests pass) and **shorter** (33% fewer lines), but the `select!` translation required genuine architectural insight rather than mechanical pattern-matching.

The key takeaway: `tokio::select!` is not just syntactic sugar — it embodies a concurrent composition model that C++ coroutines lack. The C++ solution works but requires understanding the underlying event loop mechanics at a level that Rust's abstractions intentionally hide.

This is the first case in the series where the translation hit a genuine expressiveness gap, not just a safety gap.
