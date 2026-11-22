#include "MockUpstream.h"

#include <unordered_map>
#include <utility>
#include <vector>

#include "common/Json.h"
#include "common/Log.h"
#include "net/EventLoop.h"
#include "net/Http.h"
#include "net/TcpServer.h"

namespace aegis::tools {
namespace {

std::string chunk_payload(const std::string& model, const std::string& piece, bool first) {
    Json document = Json::make_object();
    document.set("id", "chatcmpl-mock");
    document.set("object", "chat.completion.chunk");
    document.set("model", model);
    Json delta = Json::make_object();
    if (first) delta.set("role", "assistant");
    delta.set("content", piece);
    Json choice = Json::make_object();
    choice.set("index", 0);
    choice.set("delta", std::move(delta));
    choice.set("finish_reason", "null");
    Json choices = Json::make_array();
    choices.push_back(std::move(choice));
    document.set("choices", std::move(choices));
    return "data: " + document.dump() + "\n\n";
}

std::string final_payload(const std::string& model, int prompt_tokens, int completion_tokens) {
    Json document = Json::make_object();
    document.set("id", "chatcmpl-mock");
    document.set("object", "chat.completion.chunk");
    document.set("model", model);
    document.set("choices", Json::make_array());
    Json usage = Json::make_object();
    usage.set("prompt_tokens", prompt_tokens);
    usage.set("completion_tokens", completion_tokens);
    usage.set("total_tokens", prompt_tokens + completion_tokens);
    document.set("usage", std::move(usage));
    return "data: " + document.dump() + "\n\ndata: [DONE]\n\n";
}

std::string completion_payload(const std::string& model, const std::string& content,
                               int prompt_tokens, int completion_tokens) {
    Json document = Json::make_object();
    document.set("id", "chatcmpl-mock");
    document.set("object", "chat.completion");
    document.set("model", model);
    Json message = Json::make_object();
    message.set("role", "assistant");
    message.set("content", content);
    Json choice = Json::make_object();
    choice.set("index", 0);
    choice.set("message", std::move(message));
    choice.set("finish_reason", "stop");
    Json choices = Json::make_array();
    choices.push_back(std::move(choice));
    document.set("choices", std::move(choices));
    Json usage = Json::make_object();
    usage.set("prompt_tokens", prompt_tokens);
    usage.set("completion_tokens", completion_tokens);
    usage.set("total_tokens", prompt_tokens + completion_tokens);
    document.set("usage", std::move(usage));
    return document.dump();
}

}  // namespace

class MockUpstream::Impl {
public:
    explicit Impl(Options options) : options_(std::move(options)) {}

    ~Impl() { stop(); }

    Result<void> start() {
        net::TcpServer::Options options;
        options.host = options_.host;
        options.port = options_.port;
        options.io_threads = options_.io_threads;
        options.idle_timeout = std::chrono::milliseconds(0);

        net::TcpServer::Callbacks callbacks;
        callbacks.on_open = [this](net::Connection::Ptr connection) { on_open(connection); };
        callbacks.on_data = [this](net::Connection::Ptr connection, std::string_view data) {
            on_data(connection, data);
        };
        callbacks.on_close = [this](net::Connection::Ptr connection) {
            streams_.erase(connection->id());
            parsers_.erase(connection->id());
        };

        server_ = std::make_unique<net::TcpServer>(options, std::move(callbacks));
        auto started = server_->start();
        if (!started) return started;

        AEGIS_INFO("mock upstream listening on port {}", server_->port());
        return {};
    }

    void stop() {
        if (server_) {
            server_->stop();
            server_.reset();
        }
        streams_.clear();
        parsers_.clear();
    }

    [[nodiscard]] std::uint16_t port() const noexcept { return server_ ? server_->port() : 0; }
    [[nodiscard]] int requests_served() const noexcept {
        return requests_.load(std::memory_order_relaxed);
    }

    void on_open(net::Connection::Ptr connection) {
        parsers_[connection->id()] = net::HttpRequestParser{};
    }

    void on_data(net::Connection::Ptr connection, std::string_view data) {
        auto& parser = parsers_[connection->id()];
        const auto state = parser.feed(data);
        if (state == net::HttpRequestParser::State::Failed) {
            connection->send(net::serialize_response(
                net::HttpResponse::text(400, parser.error().message())));
            connection->close();
            return;
        }
        if (state != net::HttpRequestParser::State::Complete) return;

        const std::string path(parser.request().path());
        const std::string body = parser.request().body;
        const std::uint64_t id = connection->id();
        parser.reset();
        serve(connection, id, path, body);
    }

private:
    void serve(net::Connection::Ptr connection, std::uint64_t id, const std::string& path,
               const std::string& body) {
        ++requests_;

        if (path != "/v1/chat/completions" && path != "/chat/completions") {
            connection->send(net::serialize_response(net::HttpResponse::text(404, "no route")));
            connection->close();
            return;
        }

        bool stream = false;
        int prompt_tokens = 11;
        const auto parsed = parse_json(body);
        if (parsed && parsed->is_object()) {
            stream = (*parsed)["stream"].as_bool(false);
            const Json& messages = (*parsed)["messages"];
            if (messages.is_array()) prompt_tokens = static_cast<int>(messages.size()) * 6;
        }

        if (!stream) {
            const std::string payload = completion_payload(options_.model, options_.reply,
                                                          prompt_tokens, options_.tokens);
            net::HttpResponse response;
            response.status = 200;
            response.reason = "OK";
            response.headers.set("Content-Type", "application/json");
            response.body = payload;
            connection->send(net::serialize_response(response));
            connection->close();
            return;
        }

        net::HttpResponse head;
        head.status = 200;
        head.reason = "OK";
        head.headers.set("Content-Type", "text/event-stream");
        head.headers.set("Transfer-Encoding", "chunked");
        head.headers.set("Connection", "close");
        connection->send(net::serialize_response_head(head));

        Stream stream_state;
        stream_state.connection = connection;
        stream_state.prompt_tokens = prompt_tokens;
        stream_state.remaining = options_.tokens;
        streams_[id] = std::move(stream_state);
        schedule(id);
    }

    void schedule(std::uint64_t id) {
        const auto it = streams_.find(id);
        if (it == streams_.end()) return;

        net::EventLoop* loop = &it->second.connection->loop();
        const auto delay = options_.token_delay.count() > 0 ? options_.token_delay
                                                            : std::chrono::milliseconds(1);
        it->second.timer = loop->run_after(delay, [this, id] { tick(id); });
    }

    void tick(std::uint64_t id) {
        auto it = streams_.find(id);
        if (it == streams_.end()) return;
        if (!it->second.connection->alive()) {
            streams_.erase(it);
            return;
        }

        if (it->second.remaining > 0) {
            const std::string piece = "tok" + std::to_string(it->second.remaining) + " ";
            it->second.connection->send(net::chunk_encode(
                chunk_payload(options_.model, piece, it->second.first)));
            it->second.first = false;
            --it->second.remaining;
            schedule(id);
            return;
        }

        // Remove the entry before closing: close() synchronously runs the
        // server's on_close handler, which would otherwise erase it again.
        auto connection = it->second.connection;
        const int prompt_tokens = it->second.prompt_tokens;
        streams_.erase(it);

        connection->send(
            net::chunk_encode(final_payload(options_.model, prompt_tokens, options_.tokens)));
        connection->send(net::chunk_terminator());
        connection->close();
    }

    struct Stream {
        net::Connection::Ptr connection;
        int prompt_tokens{0};
        int remaining{0};
        bool first{true};
        std::uint64_t timer{0};
    };

    Options options_;
    std::unique_ptr<net::TcpServer> server_;
    std::atomic<int> requests_{0};
    std::unordered_map<std::uint64_t, net::HttpRequestParser> parsers_;
    std::unordered_map<std::uint64_t, Stream> streams_;
};

MockUpstream::MockUpstream(Options options) : impl_(std::make_unique<Impl>(std::move(options))) {}

MockUpstream::~MockUpstream() = default;

Result<void> MockUpstream::start() {
    auto started = impl_->start();
    if (!started) return started;
    return {};
}

void MockUpstream::stop() { impl_->stop(); }

std::uint16_t MockUpstream::port() const noexcept { return impl_->port(); }

int MockUpstream::requests_served() const noexcept { return impl_->requests_served(); }

}  // namespace aegis::tools
