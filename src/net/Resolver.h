#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/ThreadPool.h"
#include "net/Socket.h"

namespace aegis::net {

class EventLoop;

// Host resolution off the I/O thread.
//
// getaddrinfo() is blocking and can take hundreds of milliseconds on a cold DNS
// cache, so it runs on the shared ThreadPool and the result is posted back to the
// caller's loop. Positive results are cached with a TTL to keep the steady state
// free of resolver traffic.
class Resolver {
public:
    using Callback = std::function<void(Result<std::vector<Endpoint>>)>;

    explicit Resolver(ThreadPool& pool, std::chrono::seconds ttl = std::chrono::seconds(60));

    // `loop` must outlive the resolver and the in-flight resolution.
    void resolve_async(EventLoop& loop, std::string host, std::uint16_t port, Callback callback);

    void clear_cache();
    [[nodiscard]] std::size_t cache_size() const;

private:
    struct Entry {
        std::chrono::steady_clock::time_point expires_at;
        std::vector<Endpoint> endpoints;
    };

    [[nodiscard]] bool lookup(const std::string& key, std::vector<Endpoint>& endpoints);
    void store(const std::string& key, std::vector<Endpoint> endpoints);

    ThreadPool& pool_;
    std::chrono::seconds ttl_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, Entry> cache_;
};

}  // namespace aegis::net
