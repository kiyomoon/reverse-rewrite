/// CLI entry point.
/// Translates: bin/cli.rs
///
/// Simplified: manual arg parsing instead of clap's derive macros.

#include "client.hpp"

#include <asio.hpp>
#include <iostream>
#include <string>
#include <vector>

static constexpr uint16_t DEFAULT_PORT = 6379;

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: mini-redis-cli <command> [args...]\n"
                  << "Commands: ping, get, set, publish, subscribe\n";
        return 1;
    }

    std::string host = "127.0.0.1";
    uint16_t port = DEFAULT_PORT;
    int cmd_start = 1;

    // Parse --host and --port
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--host" && i + 1 < argc) {
            host = argv[++i];
            cmd_start = i + 1;
        } else if (arg == "--port" && i + 1 < argc) {
            port = static_cast<uint16_t>(std::stoi(argv[++i]));
            cmd_start = i + 1;
        } else {
            break;
        }
    }

    if (cmd_start >= argc) {
        std::cerr << "No command specified\n";
        return 1;
    }

    std::string command = argv[cmd_start];

    try {
        asio::io_context ctx;

        asio::co_spawn(ctx,
            [&]() -> asio::awaitable<void> {
                auto executor = co_await asio::this_coro::executor;
                auto client = co_await mini_redis::Client::connect(executor, host, port);

                if (command == "ping") {
                    std::optional<std::string> msg;
                    if (cmd_start + 1 < argc) msg = argv[cmd_start + 1];
                    auto result = co_await client.ping(std::move(msg));
                    std::cout << "\"" << result << "\"" << std::endl;

                } else if (command == "get") {
                    if (cmd_start + 1 >= argc) {
                        std::cerr << "Usage: get <key>\n";
                        co_return;
                    }
                    auto value = co_await client.get(argv[cmd_start + 1]);
                    if (value) {
                        std::cout << "\"" << *value << "\"" << std::endl;
                    } else {
                        std::cout << "(nil)" << std::endl;
                    }

                } else if (command == "set") {
                    if (cmd_start + 2 >= argc) {
                        std::cerr << "Usage: set <key> <value> [expires_ms]\n";
                        co_return;
                    }
                    std::string key = argv[cmd_start + 1];
                    std::string val = argv[cmd_start + 2];
                    if (cmd_start + 3 < argc) {
                        auto ms = std::chrono::milliseconds(std::stoi(argv[cmd_start + 3]));
                        co_await client.set_expires(key, std::move(val), ms);
                    } else {
                        co_await client.set(key, std::move(val));
                    }
                    std::cout << "OK" << std::endl;

                } else if (command == "publish") {
                    if (cmd_start + 2 >= argc) {
                        std::cerr << "Usage: publish <channel> <message>\n";
                        co_return;
                    }
                    co_await client.publish(argv[cmd_start + 1], argv[cmd_start + 2]);
                    std::cout << "Publish OK" << std::endl;

                } else if (command == "subscribe") {
                    std::vector<std::string> channels;
                    for (int i = cmd_start + 1; i < argc; ++i)
                        channels.push_back(argv[i]);
                    if (channels.empty()) {
                        std::cerr << "Usage: subscribe <channel> [channel...]\n";
                        co_return;
                    }
                    auto subscriber = co_await client.subscribe(std::move(channels));
                    while (auto msg = co_await subscriber.next_message()) {
                        std::cout << "got message from the channel: " << msg->channel
                                  << "; message = \"" << msg->content << "\"" << std::endl;
                    }

                } else {
                    std::cerr << "Unknown command: " << command << std::endl;
                }
            },
            [](std::exception_ptr ep) {
                if (ep) {
                    try { std::rethrow_exception(ep); }
                    catch (const std::exception& e) {
                        std::cerr << "error: " << e.what() << std::endl;
                    }
                }
            });

        ctx.run();

    } catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
