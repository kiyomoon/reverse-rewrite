/// Command implementations.
/// Translates: cmd/mod.rs, get.rs, set.rs, ping.rs, publish.rs,
///             subscribe.rs, unknown.rs
///
/// Key translation decisions:
///   Command enum → std::variant + std::visit
///   tokio::select! in Subscribe → manual polling loop with timer-based wake
///   StreamMap<String, Messages> → vector of (channel, BroadcastReceiver)
///   Pin<Box<dyn Stream>> → eliminated (receivers polled directly)
///   async_stream::stream! → eliminated (no generator needed)

#include "command.hpp"
#include "shutdown.hpp"

#include <algorithm>
#include <cctype>

namespace mini_redis {

// ---------- Helper: lowercase a string ----------
static std::string to_lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

static std::string to_upper(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// ---------- command_from_frame ----------

Command command_from_frame(Frame frame) {
    Parse parse(std::move(frame));
    std::string command_name = to_lower(parse.next_string());

    Command cmd;
    if (command_name == "get") {
        cmd = CmdGet::parse_frames(parse);
    } else if (command_name == "set") {
        cmd = CmdSet::parse_frames(parse);
    } else if (command_name == "ping") {
        cmd = CmdPing::parse_frames(parse);
    } else if (command_name == "publish") {
        cmd = CmdPublish::parse_frames(parse);
    } else if (command_name == "subscribe") {
        cmd = CmdSubscribe::parse_frames(parse);
    } else if (command_name == "unsubscribe") {
        cmd = CmdUnsubscribe::parse_frames(parse);
    } else {
        // Unknown command — don't call finish() (there may be extra args)
        return CmdUnknown{command_name};
    }

    // For known commands, verify no trailing arguments
    parse.finish();
    return cmd;
}

std::string command_get_name(const Command& cmd) {
    return std::visit([](auto&& c) -> std::string {
        using T = std::decay_t<decltype(c)>;
        if constexpr (std::is_same_v<T, CmdGet>)         return "get";
        if constexpr (std::is_same_v<T, CmdSet>)         return "set";
        if constexpr (std::is_same_v<T, CmdPing>)        return "ping";
        if constexpr (std::is_same_v<T, CmdPublish>)     return "pub";
        if constexpr (std::is_same_v<T, CmdSubscribe>)   return "subscribe";
        if constexpr (std::is_same_v<T, CmdUnsubscribe>) return "unsubscribe";
        if constexpr (std::is_same_v<T, CmdUnknown>)     return c.command_name;
        return "";
    }, cmd);
}

asio::awaitable<void> command_apply(Command cmd, Db& db, Connection& dst,
                                    Shutdown& shutdown) {
    co_await std::visit([&](auto&& c) -> asio::awaitable<void> {
        using T = std::decay_t<decltype(c)>;
        if constexpr (std::is_same_v<T, CmdGet>) {
            co_await c.apply(db, dst);
        } else if constexpr (std::is_same_v<T, CmdSet>) {
            co_await c.apply(db, dst);
        } else if constexpr (std::is_same_v<T, CmdPing>) {
            co_await c.apply(dst);
        } else if constexpr (std::is_same_v<T, CmdPublish>) {
            co_await c.apply(db, dst);
        } else if constexpr (std::is_same_v<T, CmdSubscribe>) {
            co_await c.apply(db, dst, shutdown);
        } else if constexpr (std::is_same_v<T, CmdUnsubscribe>) {
            // Cannot apply Unsubscribe outside of Subscribe context
            co_await dst.write_frame(Frame::error(
                "ERR unsubscribe is not valid in this context"));
        } else if constexpr (std::is_same_v<T, CmdUnknown>) {
            co_await c.apply(dst);
        }
    }, cmd);
}

// ================================================================
// CmdGet
// ================================================================

CmdGet CmdGet::parse_frames(Parse& parse) {
    return CmdGet{parse.next_string()};
}

asio::awaitable<void> CmdGet::apply(Db& db, Connection& dst) {
    auto value = db.get(key);
    Frame response = value
        ? Frame::bulk(std::move(*value))
        : Frame::null();
    co_await dst.write_frame(response);
}

Frame CmdGet::into_frame() const {
    auto f = Frame::array();
    f.push_bulk("get");
    f.push_bulk(key);
    return f;
}

// ================================================================
// CmdSet
// ================================================================

CmdSet CmdSet::parse_frames(Parse& parse) {
    CmdSet cmd;
    cmd.key = parse.next_string();
    cmd.value = parse.next_bytes();

    // Optional EX/PX
    try {
        std::string option = to_upper(parse.next_string());
        if (option == "EX") {
            uint64_t secs = parse.next_int();
            cmd.expire = std::chrono::seconds(secs);
        } else if (option == "PX") {
            uint64_t ms = parse.next_int();
            cmd.expire = std::chrono::milliseconds(ms);
        } else {
            throw std::runtime_error(
                "currently `SET` only supports the expiration option");
        }
    } catch (const ParseError& e) {
        if (!e.is_end_of_stream()) throw;
        // No options — that's fine
    }

    return cmd;
}

asio::awaitable<void> CmdSet::apply(Db& db, Connection& dst) {
    db.set(key, std::move(value), expire);
    co_await dst.write_frame(Frame::simple("OK"));
}

Frame CmdSet::into_frame() const {
    auto f = Frame::array();
    f.push_bulk("set");
    f.push_bulk(key);
    f.push_bulk(value);
    if (expire) {
        f.push_bulk("PX");
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(*expire).count();
        f.push_int(static_cast<uint64_t>(ms));
    }
    return f;
}

// ================================================================
// CmdPing
// ================================================================

CmdPing CmdPing::parse_frames(Parse& parse) {
    try {
        return CmdPing{parse.next_bytes()};
    } catch (const ParseError& e) {
        if (!e.is_end_of_stream()) throw;
        return CmdPing{std::nullopt};
    }
}

asio::awaitable<void> CmdPing::apply(Connection& dst) {
    Frame response = msg
        ? Frame::bulk(std::move(*msg))
        : Frame::simple("PONG");
    co_await dst.write_frame(response);
}

Frame CmdPing::into_frame() const {
    auto f = Frame::array();
    f.push_bulk("ping");
    if (msg) f.push_bulk(*msg);
    return f;
}

// ================================================================
// CmdPublish
// ================================================================

CmdPublish CmdPublish::parse_frames(Parse& parse) {
    return CmdPublish{parse.next_string(), parse.next_bytes()};
}

asio::awaitable<void> CmdPublish::apply(Db& db, Connection& dst) {
    size_t num_subscribers = db.publish(channel, std::move(message));
    co_await dst.write_frame(Frame::integer(static_cast<uint64_t>(num_subscribers)));
}

Frame CmdPublish::into_frame() const {
    auto f = Frame::array();
    f.push_bulk("publish");
    f.push_bulk(channel);
    f.push_bulk(message);
    return f;
}

// ================================================================
// CmdSubscribe
// ================================================================
//
// This is the most complex command — it enters a long-lived event loop
// that simultaneously:
//   1. Polls subscribed channels for new messages
//   2. Reads new commands from the client (subscribe/unsubscribe)
//   3. Listens for server shutdown
//
// Rust uses tokio::select! + StreamMap<String, Pin<Box<dyn Stream>>>
// C++ uses a polling loop with short timer sleeps to multiplex.
//
// This is the key area where the translation is NOT mechanical.
// Rust's select! provides composable, zero-cost concurrent waiting.
// C++ has no direct equivalent — we must build the multiplexing manually.

/// Helper: build a subscribe confirmation frame ["subscribe", channel, count]
static Frame make_subscribe_frame(const std::string& channel, size_t count) {
    auto f = Frame::array();
    f.push_bulk("subscribe");
    f.push_bulk(channel);
    f.push_int(static_cast<uint64_t>(count));
    return f;
}

/// Helper: build an unsubscribe confirmation frame ["unsubscribe", channel, count]
static Frame make_unsubscribe_frame(const std::string& channel, size_t count) {
    auto f = Frame::array();
    f.push_bulk("unsubscribe");
    f.push_bulk(channel);
    f.push_int(static_cast<uint64_t>(count));
    return f;
}

/// Helper: build a message frame ["message", channel, message]
static Frame make_message_frame(const std::string& channel, const std::string& msg) {
    auto f = Frame::array();
    f.push_bulk("message");
    f.push_bulk(channel);
    f.push_bulk(msg);
    return f;
}

CmdSubscribe CmdSubscribe::parse_frames(Parse& parse) {
    CmdSubscribe cmd;
    cmd.channels.push_back(parse.next_string());
    // Additional channels
    for (;;) {
        try {
            cmd.channels.push_back(parse.next_string());
        } catch (const ParseError& e) {
            if (e.is_end_of_stream()) break;
            throw;
        }
    }
    return cmd;
}

asio::awaitable<void> CmdSubscribe::apply(Db& db, Connection& dst,
                                           Shutdown& shutdown) {
    // Active subscriptions: channel_name → receiver
    struct Subscription {
        std::string channel;
        BroadcastReceiver<std::string> receiver;
    };
    std::vector<Subscription> subscriptions;

    auto executor = co_await asio::this_coro::executor;

    // ── Shared wake-up timer ──
    //
    // This is the core of the tokio::select! translation.
    //
    // In Rust, select! races three awaitables:
    //   1. subscriptions.next()  — broadcast message from any channel
    //   2. dst.read_frame()      — client sends subscribe/unsubscribe
    //   3. shutdown.recv()        — server shutting down
    //
    // C++ has no built-in select! for awaitables.  Instead we create a
    // single steady_timer that acts as a shared notification channel.
    // Three sources cancel it to wake the main loop:
    //   • BroadcastSender::send() — via each receiver's external_notify
    //   • Socket readability      — via async_wait(wait_read) callback
    //   • Shutdown                — via a spawned watcher coroutine
    //
    // When the timer is cancelled the main loop wakes, drains all pending
    // work (broadcasts + buffered commands), and re-enters the wait.

    auto wake_timer = std::make_shared<asio::steady_timer>(
        executor, TimePoint::max());

    // Spawn a watcher that cancels wake_timer on server shutdown.
    // Captures signal by shared_ptr value (safe) and timer by weak_ptr
    // (timer may be destroyed if apply() returns first).
    auto shutdown_sig = shutdown.signal();
    asio::co_spawn(executor,
        [shutdown_sig,
         wt = std::weak_ptr<asio::steady_timer>(wake_timer)]()
            -> asio::awaitable<void>
        {
            if (shutdown_sig->triggered.load()) {
                if (auto t = wt.lock()) t->cancel();
                co_return;
            }
            asio::error_code ec;
            co_await shutdown_sig->timer.async_wait(
                asio::redirect_error(asio::use_awaitable, ec));
            if (auto t = wt.lock()) t->cancel();
        }, asio::detached);

    // Subscribe to initial channels
    for (auto& ch : channels) {
        auto receiver = db.subscribe(ch, executor);
        receiver.set_external_notify(wake_timer);
        subscriptions.push_back({ch, std::move(receiver)});
        co_await dst.write_frame(
            make_subscribe_frame(ch, subscriptions.size()));
    }

    // ── Main event loop ──
    while (!shutdown.is_shutdown()) {
        // 1. Drain pending broadcast messages
        for (auto& sub : subscriptions) {
            while (auto msg = sub.receiver.try_recv()) {
                co_await dst.write_frame(
                    make_message_frame(sub.channel, *msg));
            }
        }

        // 2. Read any immediately available socket data (non-blocking)
        //    and process buffered commands
        if (!dst.read_available()) co_return;  // EOF — client disconnected

        while (auto frame = dst.try_parse_buffered()) {
            try {
                Command cmd = command_from_frame(std::move(*frame));

                if (auto* sub_cmd = std::get_if<CmdSubscribe>(&cmd)) {
                    for (auto& ch : sub_cmd->channels) {
                        auto receiver = db.subscribe(ch, executor);
                        receiver.set_external_notify(wake_timer);
                        subscriptions.push_back({ch, std::move(receiver)});
                        co_await dst.write_frame(
                            make_subscribe_frame(ch, subscriptions.size()));
                    }
                } else if (auto* unsub_cmd = std::get_if<CmdUnsubscribe>(&cmd)) {
                    auto& unsub_channels = unsub_cmd->channels;
                    if (unsub_channels.empty()) {
                        // Unsubscribe from all
                        std::vector<std::string> all_channels;
                        for (auto& s : subscriptions)
                            all_channels.push_back(s.channel);
                        for (auto& ch : all_channels) {
                            subscriptions.erase(
                                std::remove_if(subscriptions.begin(),
                                    subscriptions.end(),
                                    [&](const Subscription& s) {
                                        return s.channel == ch;
                                    }),
                                subscriptions.end());
                            co_await dst.write_frame(
                                make_unsubscribe_frame(ch,
                                    subscriptions.size()));
                        }
                    } else {
                        for (auto& ch : unsub_channels) {
                            subscriptions.erase(
                                std::remove_if(subscriptions.begin(),
                                    subscriptions.end(),
                                    [&](const Subscription& s) {
                                        return s.channel == ch;
                                    }),
                                subscriptions.end());
                            co_await dst.write_frame(
                                make_unsubscribe_frame(ch,
                                    subscriptions.size()));
                        }
                    }
                } else {
                    auto name = command_get_name(cmd);
                    co_await dst.write_frame(Frame::error(
                        "ERR unknown command '" + name + "'"));
                }
            } catch (const std::exception&) {
                // Parse error — skip
            }
        }

        if (shutdown.is_shutdown()) break;

        // 3. Set up combined wait.
        //    Three things can wake us:
        //    a) Socket becomes readable → async_wait callback cancels timer
        //    b) Broadcast message      → BroadcastSender cancels external_notify
        //    c) Server shutdown         → watcher coroutine cancels timer
        wake_timer->expires_at(TimePoint::max());

        // Register socket readability notification
        dst.socket().async_wait(
            asio::ip::tcp::socket::wait_read,
            [wt = std::weak_ptr<asio::steady_timer>(wake_timer)]
                (asio::error_code) {
                if (auto t = wt.lock()) t->cancel();
            });

        // Block until wake_timer is cancelled (by any of the three sources)
        asio::error_code ec;
        co_await wake_timer->async_wait(
            asio::redirect_error(asio::use_awaitable, ec));

        // Cancel any remaining socket readability wait
        // (safe even if it already fired or socket is closed)
        asio::error_code cancel_ec;
        dst.socket().cancel(cancel_ec);

        // Loop back to drain broadcasts + read socket data
    }
}

Frame CmdSubscribe::into_frame() const {
    auto f = Frame::array();
    f.push_bulk("subscribe");
    for (auto& ch : channels)
        f.push_bulk(ch);
    return f;
}

// ================================================================
// CmdUnsubscribe
// ================================================================

CmdUnsubscribe CmdUnsubscribe::parse_frames(Parse& parse) {
    CmdUnsubscribe cmd;
    for (;;) {
        try {
            cmd.channels.push_back(parse.next_string());
        } catch (const ParseError& e) {
            if (e.is_end_of_stream()) break;
            throw;
        }
    }
    return cmd;
}

Frame CmdUnsubscribe::into_frame() const {
    auto f = Frame::array();
    f.push_bulk("unsubscribe");
    for (auto& ch : channels)
        f.push_bulk(ch);
    return f;
}

// ================================================================
// CmdUnknown
// ================================================================

asio::awaitable<void> CmdUnknown::apply(Connection& dst) {
    co_await dst.write_frame(Frame::error(
        "ERR unknown command '" + command_name + "'"));
}

} // namespace mini_redis
