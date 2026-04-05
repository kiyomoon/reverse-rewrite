#pragma once

/// Shutdown signal listener.
/// Translates: shutdown.rs
///
/// Rust uses broadcast::Receiver<()> — we use a shared atomic bool
/// plus an asio::steady_timer for async notification.

#include <asio.hpp>
#include <asio/awaitable.hpp>
#include <atomic>
#include <memory>

namespace mini_redis {

/// Shared shutdown state — one per server.
struct ShutdownSignal {
    std::atomic<bool> triggered{false};
    asio::steady_timer timer;

    explicit ShutdownSignal(asio::any_io_executor exec)
        : timer(exec, std::chrono::steady_clock::time_point::max()) {}

    void trigger() {
        triggered.store(true);
        timer.cancel();
    }
};

/// Per-connection shutdown listener.
/// Translates Rust's Shutdown struct wrapping broadcast::Receiver<()>.
class Shutdown {
public:
    explicit Shutdown(std::shared_ptr<ShutdownSignal> signal)
        : signal_(std::move(signal)), is_shutdown_(false) {}

    bool is_shutdown() const {
        return is_shutdown_ || signal_->triggered.load();
    }

    /// Access the underlying signal (for spawning watchers).
    std::shared_ptr<ShutdownSignal> signal() const { return signal_; }

    /// Wait for shutdown signal.
    asio::awaitable<void> recv() {
        if (is_shutdown_) co_return;

        if (signal_->triggered.load()) {
            is_shutdown_ = true;
            co_return;
        }

        // Wait on the timer — it will be cancelled when shutdown is triggered
        asio::error_code ec;
        co_await signal_->timer.async_wait(
            asio::redirect_error(asio::use_awaitable, ec));
        is_shutdown_ = true;
    }

private:
    std::shared_ptr<ShutdownSignal> signal_;
    bool is_shutdown_;
};

} // namespace mini_redis
