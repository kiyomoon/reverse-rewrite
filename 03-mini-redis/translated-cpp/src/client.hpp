#pragma once

/// Async Redis client.
/// Translates: clients/client.rs (core client + subscriber)
///
/// The blocking_client and buffered_client are omitted — they are
/// pedagogical extras, not core functionality.

#include "connection.hpp"
#include "frame.hpp"

#include <asio.hpp>
#include <asio/awaitable.hpp>
#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace mini_redis {

/// Message received on a subscribed channel.
struct Message {
    std::string channel;
    std::string content;
};

class Subscriber;

/// Async Redis client.
class Client {
public:
    /// Connect to a Redis server at the given address.
    static asio::awaitable<Client> connect(
        asio::any_io_executor executor,
        const std::string& addr, uint16_t port);

    /// PING command.
    asio::awaitable<std::string> ping(std::optional<std::string> msg = std::nullopt);

    /// GET command.
    asio::awaitable<std::optional<std::string>> get(const std::string& key);

    /// SET command.
    asio::awaitable<void> set(const std::string& key, std::string value);

    /// SET with expiration.
    asio::awaitable<void> set_expires(const std::string& key, std::string value,
                                      std::chrono::milliseconds ttl);

    /// PUBLISH command.
    asio::awaitable<uint64_t> publish(const std::string& channel, std::string message);

    /// SUBSCRIBE — consumes this client and returns a Subscriber.
    asio::awaitable<Subscriber> subscribe(std::vector<std::string> channels);

private:
    explicit Client(Connection conn) : connection_(std::move(conn)) {}
    asio::awaitable<Frame> read_response();
    asio::awaitable<void> subscribe_cmd(const std::vector<std::string>& channels);

    Connection connection_;
    friend class Subscriber;
};

/// A client in pub/sub mode.
class Subscriber {
public:
    /// Receive the next message.
    asio::awaitable<std::optional<Message>> next_message();

    /// Get currently subscribed channels.
    const std::vector<std::string>& get_subscribed() const { return channels_; }

    /// Subscribe to additional channels.
    asio::awaitable<void> subscribe(const std::vector<std::string>& channels);

    /// Unsubscribe from channels (empty = all).
    asio::awaitable<void> unsubscribe(const std::vector<std::string>& channels);

private:
    friend class Client;
    Subscriber(Client client, std::vector<std::string> channels)
        : client_(std::move(client)), channels_(std::move(channels)) {}

    Client client_;
    std::vector<std::string> channels_;
};

} // namespace mini_redis
