#pragma once

/// Shared database state with key expiration and pub/sub.
/// Translates: db.rs
///
/// Rust patterns translated:
///   Arc<Shared> where Shared { Mutex<State>, Notify }
///   → std::shared_ptr<Shared> with std::mutex + asio::steady_timer (as Notify)
///
///   broadcast::Sender<Bytes> for pub/sub
///   → custom Broadcast<std::string> using mutex + vector of weak_ptr subscribers
///
///   BTreeSet<(Instant, String)> for expiration tracking
///   → std::set<std::pair<time_point, std::string>>

#include <asio.hpp>
#include <asio/awaitable.hpp>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace mini_redis {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;
using Duration = Clock::duration;

// ---------- Broadcast channel ----------
// Replaces tokio::sync::broadcast.
// A multi-producer, multi-consumer channel where each consumer
// receives every message sent after it subscribes.

template<typename T>
class BroadcastReceiver;

template<typename T>
class BroadcastSender {
public:
    BroadcastSender() : state_(std::make_shared<State>()) {}

    /// Subscribe — returns a receiver that will get future messages.
    BroadcastReceiver<T> subscribe(asio::any_io_executor exec);

    /// Send a message to all receivers. Returns number of active receivers.
    size_t send(T value);

private:
    struct Subscriber {
        std::mutex mutex;
        std::vector<T> pending;
        bool closed = false;
        // ASIO timer used as async notification: cancel to wake up waiter
        std::shared_ptr<asio::steady_timer> notify;
        // Optional external timer to also cancel on message arrival.
        // Used by CmdSubscribe to aggregate notifications from all channels
        // into a single wake-up signal — the C++ equivalent of tokio::select!
        // over a StreamMap.
        std::weak_ptr<asio::steady_timer> external_notify;
    };

    struct State {
        std::mutex mutex;
        std::vector<std::weak_ptr<Subscriber>> subscribers;
    };

    std::shared_ptr<State> state_;
    friend class BroadcastReceiver<T>;
};

template<typename T>
class BroadcastReceiver {
public:
    /// Non-blocking: get next message or nullopt.
    std::optional<T> try_recv();

    /// Async: wait for a message to arrive.
    asio::awaitable<std::optional<T>> async_recv();

    /// Check if there are pending messages.
    bool has_pending() const;

    /// Set an external notification timer.
    /// When a message arrives, this timer will also be cancelled.
    /// Used to aggregate wake-ups from multiple channels into one timer.
    void set_external_notify(std::shared_ptr<asio::steady_timer> timer) {
        if (!sub_) return;
        std::lock_guard lock(sub_->mutex);
        sub_->external_notify = timer;
    }

private:
    friend class BroadcastSender<T>;
    std::shared_ptr<typename BroadcastSender<T>::Subscriber> sub_;
    BroadcastReceiver(std::shared_ptr<typename BroadcastSender<T>::Subscriber> sub)
        : sub_(std::move(sub)) {}
};

// Template implementations
template<typename T>
BroadcastReceiver<T> BroadcastSender<T>::subscribe(asio::any_io_executor exec) {
    auto sub = std::make_shared<Subscriber>();
    sub->notify = std::make_shared<asio::steady_timer>(exec, TimePoint::max());
    std::lock_guard lock(state_->mutex);
    state_->subscribers.push_back(sub);
    return BroadcastReceiver<T>(std::move(sub));
}

template<typename T>
size_t BroadcastSender<T>::send(T value) {
    std::lock_guard lock(state_->mutex);
    size_t count = 0;
    auto it = state_->subscribers.begin();
    while (it != state_->subscribers.end()) {
        if (auto sub = it->lock()) {
            {
                std::lock_guard sub_lock(sub->mutex);
                sub->pending.push_back(value);
            }
            // Wake up any coroutine waiting on this subscriber
            sub->notify->cancel();
            // Also wake up the external listener (subscribe loop's shared timer)
            if (auto ext = sub->external_notify.lock()) {
                ext->cancel();
            }
            ++count;
            ++it;
        } else {
            it = state_->subscribers.erase(it);
        }
    }
    return count;
}

template<typename T>
std::optional<T> BroadcastReceiver<T>::try_recv() {
    if (!sub_) return std::nullopt;
    std::lock_guard lock(sub_->mutex);
    if (sub_->pending.empty()) return std::nullopt;
    T val = std::move(sub_->pending.front());
    sub_->pending.erase(sub_->pending.begin());
    return val;
}

template<typename T>
asio::awaitable<std::optional<T>> BroadcastReceiver<T>::async_recv() {
    // Check for pending messages first
    if (auto msg = try_recv()) co_return msg;

    // Wait for notification (timer cancel = message arrived)
    asio::error_code ec;
    co_await sub_->notify->async_wait(
        asio::redirect_error(asio::use_awaitable, ec));
    // Reset timer for next wait
    sub_->notify->expires_at(TimePoint::max());

    // Try again after wake-up
    co_return try_recv();
}

template<typename T>
bool BroadcastReceiver<T>::has_pending() const {
    if (!sub_) return false;
    std::lock_guard lock(sub_->mutex);
    return !sub_->pending.empty();
}

// ---------- Database ----------

class Db {
public:
    /// Create a new Db. Starts the background expiration task.
    explicit Db(asio::any_io_executor exec);

    /// Get the value associated with a key.
    std::optional<std::string> get(const std::string& key) const;

    /// Set a key-value pair with optional expiration.
    void set(const std::string& key, std::string value,
             std::optional<Duration> expire);

    /// Subscribe to a pub/sub channel.
    BroadcastReceiver<std::string> subscribe(const std::string& channel,
                                              asio::any_io_executor exec);

    /// Publish a message to a channel. Returns subscriber count.
    size_t publish(const std::string& channel, std::string value);

    /// Signal the purge task to shut down.
    void shutdown_purge_task();

private:
    struct Entry {
        std::string data;
        std::optional<TimePoint> expires_at;
    };

    struct State {
        std::unordered_map<std::string, Entry> entries;
        std::unordered_map<std::string, BroadcastSender<std::string>> pub_sub;
        std::set<std::pair<TimePoint, std::string>> expirations;
        bool shutdown = false;
    };

    struct Shared {
        mutable std::mutex mutex;
        State state;
        asio::steady_timer notify;  // Used as async Notify (cancel to wake)

        explicit Shared(asio::any_io_executor exec) : notify(exec, TimePoint::max()) {}
    };

    std::shared_ptr<Shared> shared_;

    /// Purge expired keys. Returns next expiration time if any.
    static std::optional<TimePoint> purge_expired_keys(Shared& shared);

    /// Background task that purges expired keys.
    static asio::awaitable<void> purge_task(std::shared_ptr<Shared> shared);
};

/// RAII guard that shuts down the Db's purge task on destruction.
/// Translates: DbDropGuard
class DbDropGuard {
public:
    explicit DbDropGuard(asio::any_io_executor exec) : db_(exec) {}
    Db& db() { return db_; }
    ~DbDropGuard() { db_.shutdown_purge_task(); }
private:
    Db db_;
};

} // namespace mini_redis
