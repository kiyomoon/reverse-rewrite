/// Async Redis client implementation.
/// Translates: clients/client.rs

#include "client.hpp"
#include "command.hpp"

#include <stdexcept>

namespace mini_redis {

// ================================================================
// Client
// ================================================================

asio::awaitable<Client> Client::connect(
    asio::any_io_executor executor,
    const std::string& addr, uint16_t port)
{
    asio::ip::tcp::resolver resolver(executor);
    auto endpoints = co_await resolver.async_resolve(
        addr, std::to_string(port), asio::use_awaitable);

    asio::ip::tcp::socket socket(executor);
    co_await asio::async_connect(socket, endpoints, asio::use_awaitable);

    co_return Client(Connection(std::move(socket)));
}

asio::awaitable<Frame> Client::read_response() {
    auto response = co_await connection_.read_frame();
    if (!response) {
        throw std::runtime_error("connection reset by server");
    }
    if (response->is_error()) {
        throw std::runtime_error(response->as_error());
    }
    co_return std::move(*response);
}

asio::awaitable<std::string> Client::ping(std::optional<std::string> msg) {
    CmdPing cmd{std::move(msg)};
    co_await connection_.write_frame(cmd.into_frame());

    Frame response = co_await read_response();
    if (response.is_simple()) co_return response.as_simple();
    if (response.is_bulk())   co_return response.as_bulk();
    throw std::runtime_error(response.to_error_msg());
}

asio::awaitable<std::optional<std::string>> Client::get(const std::string& key) {
    CmdGet cmd{key};
    co_await connection_.write_frame(cmd.into_frame());

    Frame response = co_await read_response();
    if (response.is_simple()) co_return response.as_simple();
    if (response.is_bulk())   co_return response.as_bulk();
    if (response.is_null())   co_return std::nullopt;
    throw std::runtime_error(response.to_error_msg());
}

asio::awaitable<void> Client::set(const std::string& key, std::string value) {
    CmdSet cmd{key, std::move(value), std::nullopt};
    co_await connection_.write_frame(cmd.into_frame());

    Frame response = co_await read_response();
    if (response.is_simple() && response.as_simple() == "OK") co_return;
    throw std::runtime_error(response.to_error_msg());
}

asio::awaitable<void> Client::set_expires(const std::string& key, std::string value,
                                           std::chrono::milliseconds ttl) {
    CmdSet cmd{key, std::move(value), ttl};
    co_await connection_.write_frame(cmd.into_frame());

    Frame response = co_await read_response();
    if (response.is_simple() && response.as_simple() == "OK") co_return;
    throw std::runtime_error(response.to_error_msg());
}

asio::awaitable<uint64_t> Client::publish(const std::string& channel, std::string message) {
    CmdPublish cmd{channel, std::move(message)};
    co_await connection_.write_frame(cmd.into_frame());

    Frame response = co_await read_response();
    if (response.is_integer()) co_return response.as_integer();
    throw std::runtime_error(response.to_error_msg());
}

asio::awaitable<void> Client::subscribe_cmd(const std::vector<std::string>& channels) {
    CmdSubscribe cmd{channels};
    co_await connection_.write_frame(cmd.into_frame());

    // Expect a confirmation for each channel
    for (const auto& channel : channels) {
        Frame response = co_await read_response();
        if (!response.is_array()) throw std::runtime_error(response.to_error_msg());
        const auto& arr = response.as_array();
        if (arr.size() < 2 || !(arr[0] == "subscribe") || !(arr[1] == channel)) {
            throw std::runtime_error(response.to_error_msg());
        }
    }
}

asio::awaitable<Subscriber> Client::subscribe(std::vector<std::string> channels) {
    co_await subscribe_cmd(channels);
    co_return Subscriber(std::move(*this), std::move(channels));
}

// ================================================================
// Subscriber
// ================================================================

asio::awaitable<std::optional<Message>> Subscriber::next_message() {
    auto maybe_frame = co_await client_.connection_.read_frame();
    if (!maybe_frame) co_return std::nullopt;

    Frame& frame = *maybe_frame;
    if (!frame.is_array()) throw std::runtime_error(frame.to_error_msg());

    const auto& arr = frame.as_array();
    if (arr.size() == 3 && arr[0] == "message") {
        co_return Message{arr[1].to_string(), arr[2].to_string()};
    }
    throw std::runtime_error(frame.to_error_msg());
}

asio::awaitable<void> Subscriber::subscribe(const std::vector<std::string>& channels) {
    co_await client_.subscribe_cmd(channels);
    for (const auto& ch : channels)
        channels_.push_back(ch);
}

asio::awaitable<void> Subscriber::unsubscribe(const std::vector<std::string>& channels) {
    CmdUnsubscribe cmd{channels};
    co_await client_.connection_.write_frame(cmd.into_frame());

    size_t num = channels.empty() ? channels_.size() : channels.size();
    for (size_t i = 0; i < num; ++i) {
        Frame response = co_await client_.read_response();
        if (!response.is_array()) throw std::runtime_error(response.to_error_msg());

        const auto& arr = response.as_array();
        if (arr.size() < 2 || !(arr[0] == "unsubscribe"))
            throw std::runtime_error(response.to_error_msg());

        std::string ch = arr[1].to_string();
        auto it = std::find(channels_.begin(), channels_.end(), ch);
        if (it != channels_.end()) channels_.erase(it);
    }
}

} // namespace mini_redis
