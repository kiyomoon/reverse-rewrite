#pragma once

/// Redis command types and dispatch.
/// Translates: cmd/mod.rs, cmd/get.rs, cmd/set.rs, cmd/ping.rs,
///             cmd/publish.rs, cmd/subscribe.rs, cmd/unknown.rs
///
/// Rust's Command enum → std::variant<Get, Set, Ping, Publish, Subscribe, Unsubscribe, Unknown>
/// Each command's apply() is an async method taking Db + Connection.

#include "connection.hpp"
#include "db.hpp"
#include "frame.hpp"
#include "parse.hpp"

#include <asio/awaitable.hpp>
#include <chrono>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace mini_redis {

// Forward declare Shutdown
class Shutdown;

// ---------- Command structs ----------

struct CmdGet {
    std::string key;
    static CmdGet parse_frames(Parse& parse);
    asio::awaitable<void> apply(Db& db, Connection& dst);
    Frame into_frame() const;
};

struct CmdSet {
    std::string key;
    std::string value;
    std::optional<Duration> expire;
    static CmdSet parse_frames(Parse& parse);
    asio::awaitable<void> apply(Db& db, Connection& dst);
    Frame into_frame() const;
};

struct CmdPing {
    std::optional<std::string> msg;
    static CmdPing parse_frames(Parse& parse);
    asio::awaitable<void> apply(Connection& dst);
    Frame into_frame() const;
};

struct CmdPublish {
    std::string channel;
    std::string message;
    static CmdPublish parse_frames(Parse& parse);
    asio::awaitable<void> apply(Db& db, Connection& dst);
    Frame into_frame() const;
};

struct CmdSubscribe {
    std::vector<std::string> channels;
    static CmdSubscribe parse_frames(Parse& parse);
    asio::awaitable<void> apply(Db& db, Connection& dst, Shutdown& shutdown);
    Frame into_frame() const;
};

struct CmdUnsubscribe {
    std::vector<std::string> channels;
    static CmdUnsubscribe parse_frames(Parse& parse);
    Frame into_frame() const;
};

struct CmdUnknown {
    std::string command_name;
    asio::awaitable<void> apply(Connection& dst);
};

// ---------- Command variant ----------

using Command = std::variant<CmdGet, CmdSet, CmdPing, CmdPublish,
                             CmdSubscribe, CmdUnsubscribe, CmdUnknown>;

/// Parse a Frame into a Command.
Command command_from_frame(Frame frame);

/// Get the Redis command name.
std::string command_get_name(const Command& cmd);

/// Apply a command (dispatches to the variant's apply method).
asio::awaitable<void> command_apply(Command cmd, Db& db, Connection& dst,
                                    Shutdown& shutdown);

} // namespace mini_redis
