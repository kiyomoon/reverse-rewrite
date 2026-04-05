#pragma once

/// Mini-Redis server — accept loop, per-connection handlers, graceful shutdown.
/// Translates: server.rs
///
/// Rust patterns translated:
///   tokio::spawn(handler) → asio::co_spawn(handler)
///   tokio::select! { server.run(), shutdown } → racing two awaitables
///   Arc<Semaphore> for connection limiting → std::counting_semaphore
///   broadcast::Sender for shutdown → ShutdownSignal (atomic + timer)
///   mpsc::channel for completion tracking → atomic counter + timer

#include "db.hpp"
#include "shutdown.hpp"

#include <asio.hpp>
#include <asio/awaitable.hpp>

namespace mini_redis {

/// Run the mini-redis server.
/// Accepts connections until shutdown_signal is triggered.
asio::awaitable<void> server_run(
    asio::ip::tcp::acceptor acceptor,
    std::shared_ptr<ShutdownSignal> shutdown_signal);

} // namespace mini_redis
