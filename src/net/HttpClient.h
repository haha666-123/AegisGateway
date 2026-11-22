#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

#include "common/Status.h"
#include "net/EventLoop.h"
#include "net/Http.h"
#include "net/Resolver.h"
#include "net/Socket.h"

namespace aegis::net {

// Asynchronous HTTP/1.1 client used to talk to upstream model endpoints.
//
// One session per request, driven entirely by the owning event loop:
//
//   resolve (worker pool) -> non blocking connect -> write request
//        -> stream response body through HttpResponseParser -> callbacks
//
// Response bodies are streamed, never buffered whole, which is what makes token
// by token SSE forwarding possible. Sessions are closed after every request
// (`Connection: close`); connection pooling is tracked as a roadmap item.
class HttpClient {
public:
    struct Request {
        std::string host;
        std::uint16_t port{80};
        Method method{Method::Post};
        std::string target{"/"};
        HeaderMap headers;
        std::string body;
    };

    struct Callbacks {
        std::function<void(int status, const HeaderMap& headers)> on_headers;
        std::function<void(std::string_view chunk)> on_body;
        std::function<void(const Status& status)> on_complete;
    };

    HttpClient(EventLoop& loop, Resolver& resolver,
               std::chrono::milliseconds connect_timeout = std::chrono::milliseconds(3000),
               std::chrono::milliseconds read_timeout = std::chrono::milliseconds(30000));
    ~HttpClient();

    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;

    // Must be called on the loop thread.
    void send(Request request, Callbacks callbacks);

    void cancel_all();

    [[nodiscard]] std::size_t active_requests() const noexcept {
        return active_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] EventLoop& loop() noexcept { return loop_; }

private:
    class Session;
    friend class Session;

    void release(std::uint64_t id);

    EventLoop& loop_;
    Resolver& resolver_;
    std::chrono::milliseconds connect_timeout_;
    std::chrono::milliseconds read_timeout_;

    std::unordered_map<std::uint64_t, std::shared_ptr<Session>> sessions_;
    std::uint64_t next_id_{1};
    std::atomic<std::size_t> active_{0};
};

}  // namespace aegis::net
