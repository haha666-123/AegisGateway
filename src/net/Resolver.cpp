#include "net/Resolver.h"

#include <utility>

#include "net/EventLoop.h"

namespace aegis::net {

Resolver::Resolver(ThreadPool& pool, std::chrono::seconds ttl) : pool_(pool), ttl_(ttl) {}

bool Resolver::lookup(const std::string& key, std::vector<Endpoint>& endpoints) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = cache_.find(key);
    if (it == cache_.end()) return false;
    if (it->second.expires_at <= std::chrono::steady_clock::now()) {
        cache_.erase(it);
        return false;
    }
    endpoints = it->second.endpoints;
    return true;
}

void Resolver::store(const std::string& key, std::vector<Endpoint> endpoints) {
    std::lock_guard<std::mutex> lock(mutex_);
    cache_[key] = Entry{std::chrono::steady_clock::now() + ttl_, std::move(endpoints)};
}

void Resolver::clear_cache() {
    std::lock_guard<std::mutex> lock(mutex_);
    cache_.clear();
}

std::size_t Resolver::cache_size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cache_.size();
}

void Resolver::resolve_async(EventLoop& loop, std::string host, std::uint16_t port,
                             Callback callback) {
    const std::string key = host + ':' + std::to_string(port);

    std::vector<Endpoint> cached;
    if (lookup(key, cached)) {
        loop.post([callback = std::move(callback), endpoints = std::move(cached)]() mutable {
            callback(endpoints);
        });
        return;
    }

    EventLoop* loop_ptr = &loop;
    pool_.submit([this, loop_ptr, key, host = std::move(host), port,
                  callback = std::move(callback)]() mutable {
        auto result = resolve(host, port);
        if (result) store(key, *result);
        loop_ptr->post([callback = std::move(callback), result = std::move(result)]() mutable {
            callback(std::move(result));
        });
    });
}

}  // namespace aegis::net
