/// Database implementation with key expiration and pub/sub.
/// Translates: db.rs
///
/// Key translation decisions:
///   tokio::sync::Notify → asio::steady_timer (cancel to wake)
///   tokio::spawn(purge_task) → asio::co_spawn(purge_task)
///   BTreeSet<(Instant, String)> → std::set<std::pair<time_point, string>>
///   broadcast::channel → BroadcastSender/BroadcastReceiver (custom)

#include "db.hpp"

namespace mini_redis {

Db::Db(asio::any_io_executor exec)
    : shared_(std::make_shared<Shared>(exec))
{
    // Start background purge task (same as tokio::spawn in Rust)
    asio::co_spawn(exec, purge_task(shared_), asio::detached);
}

std::optional<std::string> Db::get(const std::string& key) const {
    std::lock_guard lock(shared_->mutex);
    auto it = shared_->state.entries.find(key);
    if (it == shared_->state.entries.end())
        return std::nullopt;
    // Bytes::clone() in Rust is shallow; std::string copy is deep but fine here
    return it->second.data;
}

void Db::set(const std::string& key, std::string value,
             std::optional<Duration> expire) {
    std::unique_lock lock(shared_->mutex);

    bool notify = false;

    std::optional<TimePoint> expires_at;
    if (expire) {
        TimePoint when = Clock::now() + *expire;
        expires_at = when;

        // Check if this new expiration is sooner than the current earliest
        auto& exps = shared_->state.expirations;
        if (exps.empty() || exps.begin()->first > when) {
            notify = true;
        }
    }

    // Remove previous expiration if it existed
    auto prev_it = shared_->state.entries.find(key);
    if (prev_it != shared_->state.entries.end()) {
        if (prev_it->second.expires_at) {
            shared_->state.expirations.erase(
                {*prev_it->second.expires_at, key});
        }
    }

    // Insert the entry
    shared_->state.entries[key] = Entry{std::move(value), expires_at};

    // Track expiration
    if (expires_at) {
        shared_->state.expirations.insert({*expires_at, key});
    }

    // Release lock before notifying (same as Rust: drop(state) then notify)
    lock.unlock();

    if (notify) {
        // Wake the background task by cancelling its timer
        shared_->notify.cancel();
    }
}

BroadcastReceiver<std::string> Db::subscribe(const std::string& channel,
                                              asio::any_io_executor exec) {
    std::lock_guard lock(shared_->mutex);
    auto& sender = shared_->state.pub_sub[channel];  // creates if absent
    return sender.subscribe(exec);
}

size_t Db::publish(const std::string& channel, std::string value) {
    std::lock_guard lock(shared_->mutex);
    auto it = shared_->state.pub_sub.find(channel);
    if (it == shared_->state.pub_sub.end())
        return 0;
    return it->second.send(std::move(value));
}

void Db::shutdown_purge_task() {
    {
        std::lock_guard lock(shared_->mutex);
        shared_->state.shutdown = true;
    }
    // Wake the background task
    shared_->notify.cancel();
}

std::optional<TimePoint> Db::purge_expired_keys(Shared& shared) {
    std::lock_guard lock(shared.mutex);

    if (shared.state.shutdown)
        return std::nullopt;

    auto now = Clock::now();
    auto& exps = shared.state.expirations;
    auto& entries = shared.state.entries;

    while (!exps.empty()) {
        auto it = exps.begin();
        if (it->first > now) {
            return it->first;
        }
        // Key expired — remove it
        entries.erase(it->second);
        exps.erase(it);
    }

    return std::nullopt;
}

asio::awaitable<void> Db::purge_task(std::shared_ptr<Shared> shared) {
    while (true) {
        // Check shutdown
        {
            std::lock_guard lock(shared->mutex);
            if (shared->state.shutdown) break;
        }

        auto next_expiry = purge_expired_keys(*shared);

        if (next_expiry) {
            // Wait until next expiry or notification
            shared->notify.expires_at(*next_expiry);
            asio::error_code ec;
            co_await shared->notify.async_wait(
                asio::redirect_error(asio::use_awaitable, ec));
            // ec == operation_aborted means we were woken up (notify.cancel())
            // Either way, loop back and re-check
        } else {
            // No expirations — wait for notification
            shared->notify.expires_at(TimePoint::max());
            asio::error_code ec;
            co_await shared->notify.async_wait(
                asio::redirect_error(asio::use_awaitable, ec));
        }
    }
}

} // namespace mini_redis
