#include "net/HttpClient.h"

#include <array>
#include <utility>
#include <vector>

#include "common/Log.h"
#include "common/Metrics.h"

namespace aegis::net {
namespace {

constexpr std::size_t kReadChunk = 32U * 1024U;

}  // namespace

class HttpClient::Session : public std::enable_shared_from_this<Session> {
public:
    Session(HttpClient& owner, std::uint64_t id, Request request, Callbacks callbacks)
        : owner_(owner),
          id_(id),
          request_(std::move(request)),
          callbacks_(std::move(callbacks)),
          loop_(&owner.loop()) {}

    ~Session() {
        if (handle_ != kInvalidHandle) {
            close_socket(handle_);
            handle_ = kInvalidHandle;
        }
    }

    void start() {
        auto self = shared_from_this();
        arm_timer(owner_.connect_timeout_);
        owner_.resolver_.resolve_async(
            *loop_, request_.host, request_.port,
            [self](Result<std::vector<Endpoint>> endpoints) {
                self->on_resolved(std::move(endpoints));
            });
    }

    void cancel() {
        if (done_) return;
        finish(Status::error(errc::kClosed, "request cancelled"));
    }

private:
    enum class Phase { Connecting, Active };

    void on_resolved(Result<std::vector<Endpoint>> endpoints) {
        if (done_) return;
        if (!endpoints || endpoints->empty()) {
            finish(endpoints ? Status::error(errc::kIoError, "no endpoint resolved")
                             : endpoints.error());
            return;
        }
        endpoint_ = endpoints->front();

        auto handle = create_tcp_socket(endpoint_.family());
        if (!handle) {
            finish(handle.error());
            return;
        }
        handle_ = *handle;

        auto nonblocking = set_nonblocking(handle_);
        if (!nonblocking) {
            finish(nonblocking.error());
            return;
        }
        (void)set_nodelay(handle_);

        auto self = shared_from_this();
        const bool registered = loop_->add(handle_, kWritable, [self](std::uint32_t events) {
            if (self->phase_ == Phase::Connecting) {
                self->on_connecting(events);
                return;
            }
            if ((events & kWritable) != 0) self->on_writable();
            if (!self->done_ && (events & kReadable) != 0) self->on_readable();
        });
        if (!registered) {
            finish(Status::error(errc::kIoError, "failed to register upstream socket"));
            return;
        }
        interest_ = kWritable;

        const auto outcome = connect_tcp(handle_, endpoint_);
        if (outcome.status == IoStatus::Error) {
            finish(Status::error(errc::kIoError,
                                 "connect failed: " + error_text(outcome.error)));
            return;
        }
        if (outcome.status == IoStatus::Ok) {
            phase_ = Phase::Active;
            on_connected();
        }
    }

    void on_connecting(std::uint32_t events) {
        if (done_) return;
        if ((events & (kError | kHangup)) != 0 && (events & kWritable) == 0) {
            finish(Status::error(errc::kIoError, "connect failed"));
            return;
        }
        auto connected = finish_connect(handle_);
        if (!connected) {
            finish(connected.error());
            return;
        }
        phase_ = Phase::Active;
        on_connected();
    }

    void on_connected() {
        if (done_) return;
        arm_timer(owner_.read_timeout_);

        HttpRequest request;
        request.method = request_.method;
        request.target = request_.target;
        request.version = "HTTP/1.1";
        request.body = request_.body;
        request.headers.set("Host", request_.host + ':' + std::to_string(request_.port));
        request.headers.set("Connection", "close");
        request.headers.set("User-Agent", "aegis/1.0");
        for (const auto& [name, value] : request_.headers.items()) {
            request.headers.set(name, value);
        }
        output_ = serialize_request(request);

        Metrics::instance().inc(Counter::UpstreamRequestsTotal);

        if (interest_ != (kWritable | kReadable)) {
            interest_ = kWritable | kReadable;
            loop_->update(handle_, interest_);
        }
        on_writable();
    }

    void on_writable() {
        if (done_) return;
        std::size_t offset = 0;
        while (offset < output_.size()) {
            const auto outcome =
                write_some(handle_, output_.data() + offset, output_.size() - offset);
            if (outcome.ok()) {
                offset += outcome.bytes;
                continue;
            }
            if (outcome.status == IoStatus::WouldBlock) {
                output_.erase(0, offset);
                return;
            }
            finish(Status::error(errc::kIoError,
                                 "write failed: " + error_text(outcome.error)));
            return;
        }
        output_.clear();
        if ((interest_ & kWritable) != 0) {
            interest_ = kReadable;
            loop_->update(handle_, interest_);
        }
    }

    void on_readable() {
        if (done_) return;
        std::array<char, kReadChunk> scratch{};
        for (;;) {
            const auto outcome = read_some(handle_, scratch.data(), scratch.size());
            if (outcome.ok()) {
                consume(std::string_view(scratch.data(), outcome.bytes));
                if (done_) return;
                continue;
            }
            if (outcome.status == IoStatus::Closed) {
                on_eof();
                return;
            }
            if (outcome.status == IoStatus::WouldBlock) return;
            finish(Status::error(errc::kIoError,
                                 "read failed: " + error_text(outcome.error)));
            return;
        }
    }

    void consume(std::string_view data) {
        const auto state = parser_.feed(data, [this](std::string_view chunk) {
            if (callbacks_.on_body) callbacks_.on_body(chunk);
        });

        if (!headers_delivered_ && parser_.headers_complete()) {
            headers_delivered_ = true;
            if (callbacks_.on_headers) callbacks_.on_headers(parser_.status(), parser_.headers());
            if (done_) return;
        }

        if (state == HttpResponseParser::State::Failed) {
            finish(parser_.error());
            return;
        }
        if (state == HttpResponseParser::State::Complete) {
            finish(Status::success());
        }
    }

    void on_eof() {
        if (done_) return;
        if (!parser_.headers_complete()) {
            finish(Status::error(errc::kClosed, "connection closed before response headers"));
            return;
        }
        if (!headers_delivered_) {
            headers_delivered_ = true;
            if (callbacks_.on_headers) callbacks_.on_headers(parser_.status(), parser_.headers());
            if (done_) return;
        }
        const auto state = parser_.finish_eof();
        finish(state == HttpResponseParser::State::Complete ? Status::success()
                                                            : parser_.error());
    }

    void arm_timer(std::chrono::milliseconds timeout) {
        auto self = shared_from_this();
        if (timer_ != 0) loop_->cancel_timer(timer_);
        timer_ = loop_->run_after(timeout, [self] {
            self->finish(Status::error(errc::kTimeout, "upstream request timed out"));
        });
    }

    void finish(const Status& status) {
        if (done_) return;
        done_ = true;

        if (timer_ != 0) {
            loop_->cancel_timer(timer_);
            timer_ = 0;
        }
        if (handle_ != kInvalidHandle) {
            loop_->remove(handle_);
            close_socket(handle_);
            handle_ = kInvalidHandle;
        }
        if (!status.ok()) Metrics::instance().inc(Counter::UpstreamFailuresTotal);

        // Keep the session alive until the callback returns: the owner releases
        // its reference below, which may be the last one.
        auto self = shared_from_this();
        auto callback = callbacks_.on_complete;
        owner_.release(id_);
        if (callback) callback(status);
    }

    HttpClient& owner_;
    std::uint64_t id_{0};
    Request request_;
    Callbacks callbacks_;
    EventLoop* loop_{nullptr};

    Handle handle_{kInvalidHandle};
    Endpoint endpoint_{};
    HttpResponseParser parser_;
    std::string output_;
    Phase phase_{Phase::Connecting};
    bool headers_delivered_{false};
    bool done_{false};
    std::uint32_t interest_{0};
    EventLoop::TimerId timer_{0};
};

HttpClient::HttpClient(EventLoop& loop, Resolver& resolver,
                       std::chrono::milliseconds connect_timeout,
                       std::chrono::milliseconds read_timeout)
    : loop_(loop),
      resolver_(resolver),
      connect_timeout_(connect_timeout),
      read_timeout_(read_timeout) {}

HttpClient::~HttpClient() { cancel_all(); }

void HttpClient::send(Request request, Callbacks callbacks) {
    const std::uint64_t id = next_id_++;
    auto session = std::make_shared<Session>(*this, id, std::move(request), std::move(callbacks));
    sessions_[id] = session;
    active_.fetch_add(1, std::memory_order_relaxed);
    session->start();
}

void HttpClient::release(std::uint64_t id) {
    // Deferred erase: the session is still on the stack of its own callback.
    loop_.post([this, id] {
        if (sessions_.erase(id) > 0) active_.fetch_sub(1, std::memory_order_relaxed);
    });
}

void HttpClient::cancel_all() {
    std::vector<std::shared_ptr<Session>> sessions;
    sessions.reserve(sessions_.size());
    for (auto& [id, session] : sessions_) sessions.push_back(session);

    for (auto& session : sessions) session->cancel();
    sessions_.clear();
    active_.store(0, std::memory_order_relaxed);
}

}  // namespace aegis::net
