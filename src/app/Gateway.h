#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

#include "ai/Cache.h"
#include "ai/Embedding.h"
#include "ai/OpenAiClient.h"
#include "ai/Router.h"
#include "app/Config.h"
#include "common/Json.h"
#include "common/RateLimiter.h"
#include "common/Status.h"
#include "common/ThreadPool.h"
#include "net/EventLoop.h"
#include "net/Http.h"
#include "net/HttpClient.h"
#include "net/Resolver.h"
#include "net/TcpServer.h"

namespace aegis::app {

// The HTTP front end.
//
// Responsibilities: authentication, rate limiting, request validation, cache
// lookups, upstream selection with failover, SSE forwarding and metrics. All
// request handling happens on the I/O thread that owns the connection; the only
// shared state is the router, the rate limiter and the caches, each of which is
// internally synchronised.
class Gateway {
public:
    explicit Gateway(Config config);
    ~Gateway();

    Gateway(const Gateway&) = delete;
    Gateway& operator=(const Gateway&) = delete;

    Result<void> start();
    void stop();

    [[nodiscard]] std::uint16_t port() const noexcept;
    [[nodiscard]] const Config& config() const noexcept { return config_; }
    [[nodiscard]] ai::Router& router() noexcept { return *router_; }
    [[nodiscard]] std::size_t active_connections() const;
    [[nodiscard]] const char* io_backend() const;

private:
    struct ConnectionState {
        net::HttpRequestParser parser;
        ai::OpenAiClient::Request pending;
        std::string cache_key;
        std::uint64_t upstream_request_id{0};
        int attempt{0};
        bool in_flight{false};
        bool stream{false};
        bool head_sent{false};
    };

    struct WorkerState {
        net::EventLoop* loop{nullptr};
        std::unique_ptr<net::HttpClient> http;
        std::unordered_map<std::uint64_t, ConnectionState> connections;
    };

    [[nodiscard]] static WorkerState*& tls_worker() noexcept;
    WorkerState& worker();
    WorkerState& worker_for(net::EventLoop& loop);

    void on_open(net::Connection::Ptr connection);
    void on_data(net::Connection::Ptr connection, std::string_view data);
    void on_close(net::Connection::Ptr connection);

    void dispatch(net::Connection::Ptr connection, ConnectionState& state,
                  const net::HttpRequest& request);
    void handle_chat(net::Connection::Ptr connection, ConnectionState& state,
                     const net::HttpRequest& request);
    void handle_models(net::Connection::Ptr connection, const net::HttpRequest& request);
    void handle_healthz(net::Connection::Ptr connection, const net::HttpRequest& request);
    void handle_metrics(net::Connection::Ptr connection, const net::HttpRequest& request);
    void handle_stats(net::Connection::Ptr connection, const net::HttpRequest& request);

    void try_attempt(net::Connection::Ptr connection, std::uint64_t connection_id, int attempt);
    void on_upstream_chunk(net::Connection::Ptr connection, std::uint64_t connection_id,
                           std::string_view chunk);
    void on_upstream_done(net::Connection::Ptr connection, std::uint64_t connection_id,
                          std::string upstream_name, Result<ai::OpenAiClient::Outcome> result);
    void finish_stream(net::Connection::Ptr connection, const Status& status);
    void apply_backpressure(net::Connection::Ptr connection, std::uint64_t connection_id);

    void respond(net::Connection& connection, int status, std::string_view content_type,
                 std::string body, bool keep_alive,
                 const net::HeaderMap* extra_headers = nullptr);
    void respond_json(net::Connection& connection, int status, const Json& body, bool keep_alive);
    void respond_error(net::Connection& connection, int status, std::string message,
                       bool keep_alive);

    [[nodiscard]] bool authorized(const net::HttpRequest& request, std::string& key) const;
    [[nodiscard]] int max_attempts() const;

    Config config_;
    std::unique_ptr<ThreadPool> pool_;
    std::unique_ptr<net::Resolver> resolver_;
    std::unique_ptr<ai::Router> router_;
    std::unique_ptr<RateLimiter> limiter_;
    std::unique_ptr<ai::LruCache> cache_;
    std::unique_ptr<ai::SemanticCache> semantic_cache_;
    ai::HashingEmbedder embedder_;
    std::unique_ptr<net::TcpServer> server_;

    std::mutex workers_mutex_;
    std::unordered_map<net::EventLoop*, WorkerState> workers_;
};

}  // namespace aegis::app
