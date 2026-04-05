/// Server entry point.
/// Translates: bin/server.rs
///
/// Rust: #[tokio::main] → asio::io_context + asio::co_spawn + ctx.run()
/// Rust: signal::ctrl_c() → asio::signal_set

#include "server.hpp"

#include <asio.hpp>
#include <iostream>
#include <string>

static constexpr uint16_t DEFAULT_PORT = 6379;

int main(int argc, char* argv[]) {
    uint16_t port = DEFAULT_PORT;

    // Simple argument parsing (replaces clap)
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc) {
            port = static_cast<uint16_t>(std::stoi(argv[++i]));
        }
    }

    try {
        asio::io_context ctx;

        // Set up signal handling for graceful shutdown (replaces tokio::signal::ctrl_c())
        auto shutdown_signal = std::make_shared<mini_redis::ShutdownSignal>(
            ctx.get_executor());

        asio::signal_set signals(ctx, SIGINT, SIGTERM);
        signals.async_wait([shutdown_signal](const asio::error_code&, int) {
            std::cerr << "shutting down" << std::endl;
            shutdown_signal->trigger();
        });

        // Bind TCP listener
        asio::ip::tcp::acceptor acceptor(
            ctx,
            asio::ip::tcp::endpoint(
                asio::ip::make_address("127.0.0.1"), port));

        // Start the server coroutine
        asio::co_spawn(ctx,
            mini_redis::server_run(std::move(acceptor), shutdown_signal),
            [](std::exception_ptr ep) {
                if (ep) {
                    try { std::rethrow_exception(ep); }
                    catch (const std::exception& e) {
                        std::cerr << "server error: " << e.what() << std::endl;
                    }
                }
            });

        // Run the event loop (replaces #[tokio::main])
        ctx.run();

    } catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
