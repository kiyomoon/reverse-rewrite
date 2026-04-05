/// Server implementation — accept loop and per-connection handler.
/// Translates: server.rs
///
/// Key translation decisions:
///   Listener struct → local state in server_run coroutine
///   Handler struct → handle_connection coroutine
///   tokio::spawn → asio::co_spawn
///   tokio::select! { accept, shutdown } → cancel acceptor on shutdown
///   Arc<Semaphore>(MAX_CONNECTIONS) → atomic counter (non-blocking)
///   mpsc::channel for completion → atomic counter + timer
///   Exponential backoff on accept failure → same logic

#include "server.hpp"
#include "command.hpp"
#include "connection.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>

namespace mini_redis {

static constexpr int MAX_CONNECTIONS = 250;

/// Per-connection handler coroutine.
/// Translates Handler::run() from server.rs.
static asio::awaitable<void> handle_connection(
    asio::ip::tcp::socket socket,
    Db db,
    Shutdown shutdown)
{
    Connection connection(std::move(socket));

    while (!shutdown.is_shutdown()) {
        std::optional<Frame> maybe_frame;
        try {
            maybe_frame = co_await connection.read_frame();
        } catch (const std::exception& e) {
            std::cerr << "connection error: " << e.what() << std::endl;
            co_return;
        }

        if (!maybe_frame) {
            co_return;
        }

        Command cmd;
        try {
            cmd = command_from_frame(std::move(*maybe_frame));
        } catch (const std::exception& e) {
            std::cerr << "protocol error: " << e.what() << std::endl;
            co_return;
        }

        try {
            co_await command_apply(std::move(cmd), db, connection, shutdown);
        } catch (const std::exception& e) {
            std::cerr << "command error: " << e.what() << std::endl;
            co_return;
        }
    }
}

asio::awaitable<void> server_run(
    asio::ip::tcp::acceptor acceptor,
    std::shared_ptr<ShutdownSignal> shutdown_signal)
{
    auto executor = co_await asio::this_coro::executor;

    // Database with RAII cleanup (DbDropGuard)
    DbDropGuard db_holder(executor);

    // Track active connections (replaces Arc<Semaphore> + mpsc)
    auto active = std::make_shared<std::atomic<int>>(0);
    auto done_timer = std::make_shared<asio::steady_timer>(
        executor, std::chrono::steady_clock::time_point::max());

    // Close acceptor on shutdown to break the accept loop
    auto acceptor_ptr = std::make_shared<asio::ip::tcp::acceptor>(std::move(acceptor));

    // Register shutdown callback: close the acceptor
    asio::co_spawn(executor,
        [shutdown_signal, acceptor_ptr]() -> asio::awaitable<void> {
            // Wait for shutdown
            asio::error_code ec;
            co_await shutdown_signal->timer.async_wait(
                asio::redirect_error(asio::use_awaitable, ec));
            // Close acceptor to break accept loop
            asio::error_code close_ec;
            acceptor_ptr->close(close_ec);
        }, asio::detached);

    std::cerr << "accepting inbound connections" << std::endl;

    // Main accept loop
    int backoff = 1;
    while (!shutdown_signal->triggered.load()) {
        // Check connection limit
        if (active->load() >= MAX_CONNECTIONS) {
            // Wait briefly and retry
            asio::steady_timer wait(executor);
            wait.expires_after(std::chrono::milliseconds(10));
            asio::error_code ec;
            co_await wait.async_wait(
                asio::redirect_error(asio::use_awaitable, ec));
            continue;
        }

        // Accept a connection
        asio::error_code ec;
        asio::ip::tcp::socket socket(executor);
        co_await acceptor_ptr->async_accept(socket,
            asio::redirect_error(asio::use_awaitable, ec));

        if (ec) {
            if (shutdown_signal->triggered.load()) break;
            if (ec == asio::error::operation_aborted) break;

            // Exponential backoff on accept failure
            if (backoff > 64) {
                std::cerr << "failed to accept: " << ec.message() << std::endl;
                break;
            }
            asio::steady_timer timer(executor);
            timer.expires_after(std::chrono::seconds(backoff));
            co_await timer.async_wait(asio::use_awaitable);
            backoff *= 2;
            continue;
        }

        backoff = 1;  // Reset backoff on success

        // Spawn a handler for this connection
        active->fetch_add(1);
        auto db = db_holder.db();
        Shutdown shutdown(shutdown_signal);

        asio::co_spawn(executor,
            [socket = std::move(socket), db = std::move(db),
             shutdown = std::move(shutdown),
             active, done_timer]() mutable -> asio::awaitable<void>
            {
                co_await handle_connection(std::move(socket),
                                           std::move(db),
                                           std::move(shutdown));
                if (active->fetch_sub(1) == 1) {
                    done_timer->cancel();
                }
            },
            asio::detached);
    }

    // Graceful shutdown: wait for all active connections to finish
    if (active->load() > 0) {
        asio::error_code ec;
        co_await done_timer->async_wait(
            asio::redirect_error(asio::use_awaitable, ec));
    }
}

} // namespace mini_redis
