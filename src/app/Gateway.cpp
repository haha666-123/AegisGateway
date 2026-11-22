#include "app/Gateway.h"

#include <algorithm>
#include <chrono>
#include <utility>

#include "common/Log.h"
#include "common/Metrics.h"
#include "common/Util.h"

namespace aegis::app {
namespace {

constexpr std::size_t kBackpressureHighWatermark = 256U * 1024U;
constexpr auto kBackpressureRetry = std::chrono::milliseconds(20);

int status_for_parse_error(const Status& status) {
    switch (status.code()) {
        case errc::kTooLarge: return 413;
        case errc::kParseError: return 400;
        default: return 400;
    }
}

std::string last_user_message(const ai::OpenAiClient::Request& request) {
    for (auto it = request.messages.rbegin(); it != request.messages.rend(); ++it) {
        if (it->role == "user") return it->content;
    }
    return request.messages.empty() ? std::string{} : request.messages.back().content;
}

}  // namespace

Gateway::Gateway(Config config) : config_(std::move(config)) {}

Gateway::~Gateway() { stop(); }

std::uint16_t Gateway::port() const noexcept { return server_ ? server_->port() : 0; }

std::size_t Gateway::active_connections() const {
    return server_ ? server_->active_connections() : 0;
}

const char* Gateway::io_backend() const {
    return server_ ? server_->io_backend() : "unstarted";
}

Gateway::WorkerState*& Gateway::tls_worker() noexcept {
    static thread_local WorkerState* worker = nullptr;
    return worker;
}

Gateway::WorkerState& Gateway::worker() { return *tls_worker(); }

Result<void> Gateway::start() {
    pool_ = std::make_unique<ThreadPool>(std::max(2, config_.server.io_threads), "aegis-pool");
    resolver_ = std::make_unique<net::Resolver>(*pool_);
    router_ = std::make_unique<ai::Router>(config_.upstreams);
    if (config_.rate_limit.enabled) {
        limiter_ = std::make_unique<RateLimiter>(config_.rate_limit.requests_per_second,
                                                 config_.rate_limit.burst);
    }
    if (config_.cache.enabled) {
        cache_ = std::make_unique<ai::LruCache>(config_.cache.capacity, config_.cache.ttl);
        if (config_.cache.semantic) {
            semantic_cache_ = std::make_unique<ai::SemanticCache>(
                embedder_, config_.cache.capacity, config_.cache.ttl, config_.cache.threshold);
        }
    }

    net::TcpServer::Options options;
    options.host = config_.server.host;
    options.port = config_.server.port;
    options.io_threads = config_.server.io_threads;
    options.max_connections = config_.server.max_connections;
    options.idle_timeout = config_.server.idle_timeout;

    net::TcpServer::Callbacks callbacks;
    callbacks.on_open = [this](net::Connection::Ptr connection) { on_open(std::move(connection)); };
    callbacks.on_data = [this](net::Connection::Ptr connection, std::string_view data) {
        on_data(std::move(connection), data);
    };
    callbacks.on_close = [this](net::Connection::Ptr connection) {
        on_close(std::move(connection));
    };

    server_ = std::make_unique<net::TcpServer>(options, std::move(callbacks));
    auto started = server_->start();
    if (!started) return started;

    AEGIS_INFO("gateway ready: {} upstream(s), {} io thread(s), cache {}",
               router_->upstream_count(), options.io_threads,
               config_.cache.enabled ? "enabled" : "disabled");
    return {};
}

void Gateway::stop() {
    if (semantic_cache_) semantic_cache_->clear();
    if (cache_) cache_->clear();
    if (server_) {
        server_->stop();
        server_.reset();
    }
    workers_.clear();
    if (pool_) {
        pool_->shutdown();
        pool_.reset();
    }
}

Gateway::WorkerState& Gateway::worker_for(net::EventLoop& loop) {
    std::lock_guard<std::mutex> lock(workers_mutex_);
    auto& state = workers_[&loop];
    if (state.loop == nullptr) {
        state.loop = &loop;
        state.http = std::make_unique<net::HttpClient>(loop, *resolver_, config_.timeouts.connect,
                                                       config_.timeouts.upstream);
        Metrics::instance().set(Gauge::ThreadsWorker, static_cast<double>(pool_->size()));
    }
    return state;
}

void Gateway::on_open(net::Connection::Ptr connection) {
    auto& state = worker_for(connection->loop());
    tls_worker() = &state;
    auto& connection_state = state.connections[connection->id()];
    connection_state.parser.set_limits(
        net::ParseLimits{config_.limits.max_header_bytes, config_.limits.max_body_bytes});
}

void Gateway::on_close(net::Connection::Ptr connection) {
    auto* state = tls_worker();
    if (state == nullptr) return;

    const auto it = state->connections.find(connection->id());
    if (it == state->connections.end()) return;
    if (it->second.upstream_request_id != 0 && state->http) {
        state->http->cancel(it->second.upstream_request_id);
    }
    state->connections.erase(it);
}

void Gateway::on_data(net::Connection::Ptr connection, std::string_view data) {
    auto* worker = tls_worker();
    if (worker == nullptr) return;

    const std::uint64_t connection_id = connection->id();
    auto it = worker->connections.find(connection_id);
    if (it == worker->connections.end()) return;

    const auto result = it->second.parser.feed(data);
    if (result == net::HttpRequestParser::State::Failed) {
        // Reset/resolve everything before responding: the response may close the
        // connection, which destroys this connection state.
        const int status = status_for_parse_error(it->second.parser.error());
        const std::string message = it->second.parser.error().message();
        it->second.parser.reset();
        respond_error(*connection, status, message, false);
        return;
    }

    for (;;) {
        it = worker->connections.find(connection_id);
        if (it == worker->connections.end() || !connection->alive()) return;
        if (it->second.in_flight || !it->second.parser.complete()) return;

        dispatch(connection, it->second, it->second.parser.request());
        if (!connection->alive()) return;

        it = worker->connections.find(connection_id);
        if (it == worker->connections.end()) return;
        it->second.parser.reset();
        if (it->second.in_flight) return;  // wait for the upstream response
    }
}

bool Gateway::authorized(const net::HttpRequest& request, std::string& key) const {
    key.clear();

    const std::string* authorization = request.headers.find("Authorization");
    if (authorization != nullptr) {
        std::string_view value(*authorization);
        if (value.starts_with("Bearer ") || value.starts_with("bearer ")) {
            value.remove_prefix(7);
        }
        key.assign(trim(value));
    }
    if (key.empty()) {
        const std::string* api_key = request.headers.find("x-api-key");
        if (api_key != nullptr) key.assign(trim(*api_key));
    }

    if (config_.gateway_keys.empty()) return true;
    for (const auto& allowed : config_.gateway_keys) {
        if (allowed == key) return true;
    }
    return false;
}

int Gateway::max_attempts() const {
    const std::size_t upstreams = router_ ? router_->upstream_count() : 1;
    return static_cast<int>(std::min<std::size_t>(3, std::max<std::size_t>(1, upstreams)));
}

void Gateway::dispatch(net::Connection::Ptr connection, ConnectionState& state,
                       const net::HttpRequest& request) {
    Metrics::instance().inc(Counter::HttpRequestsTotal);

    const std::string_view path = request.path();
    // Liveness and metrics stay open so orchestrators and Prometheus can scrape
    // them without a credential.
    if (path == "/healthz") {
        handle_healthz(connection, request);
        return;
    }
    if (path == "/metrics") {
        handle_metrics(connection, request);
        return;
    }

    std::string key;
    if (!authorized(request, key)) {
        respond_error(*connection, 401, "invalid or missing API key", request.keep_alive());
        return;
    }

    if (limiter_) {
        const auto decision = limiter_->acquire(key.empty() ? connection->peer() : key);
        if (!decision.allowed) {
            Metrics::instance().inc(Counter::RateLimitedTotal);
            net::HttpResponse response = net::HttpResponse::json(
                429, R"({"error":{"message":"rate limit exceeded","type":"rate_limit_error"}})");
            response.reason = net::reason_phrase(429);
            char retry_after[32];
            std::snprintf(retry_after, sizeof(retry_after), "%.3f",
                          decision.retry_after_seconds);
            response.headers.set("Retry-After", retry_after);
            respond(*connection, 429, "application/json; charset=utf-8",
                    std::move(response.body), request.keep_alive(), &response.headers);
            return;
        }
    }

    if (path == "/v1/chat/completions") {
        if (request.method != net::Method::Post) {
            respond_error(*connection, 405, "only POST is allowed for this endpoint",
                          request.keep_alive());
            return;
        }
        handle_chat(connection, state, request);
        return;
    }
    if (path == "/v1/models") {
        handle_models(connection, request);
        return;
    }
    if (path == "/admin/stats") {
        handle_stats(connection, request);
        return;
    }

    respond_error(*connection, 404, "unknown route: " + std::string(path), request.keep_alive());
}

void Gateway::handle_chat(net::Connection::Ptr connection, ConnectionState& state,
                          const net::HttpRequest& request) {
    const auto document = parse_json(request.body);
    if (!document) {
        respond_error(*connection, 400, "request body is not valid JSON: " + document.error().message(),
                      request.keep_alive());
        return;
    }
    if (!document->is_object()) {
        respond_error(*connection, 400, "request body must be a JSON object", request.keep_alive());
        return;
    }

    const Json& body = *document;
    ai::OpenAiClient::Request pending;
    pending.model = body["model"].as_string("");
    if (pending.model.empty()) {
        respond_error(*connection, 400, "field 'model' is required", request.keep_alive());
        return;
    }
    const Json& messages = body["messages"];
    if (!messages.is_array() || messages.size() == 0) {
        respond_error(*connection, 400, "field 'messages' must be a non empty array",
                      request.keep_alive());
        return;
    }
    for (std::size_t i = 0; i < messages.size(); ++i) {
        const Json& item = messages[i];
        if (!item.is_object()) continue;
        ai::OpenAiClient::Message message;
        message.role = item["role"].as_string("user");
        message.content = item["content"].as_string("");
        if (message.content.empty()) continue;
        pending.messages.push_back(std::move(message));
    }
    if (pending.messages.empty()) {
        respond_error(*connection, 400, "no message with non empty 'content'",
                      request.keep_alive());
        return;
    }
    pending.stream = body["stream"].as_bool(false);
    pending.temperature = body["temperature"].is_number() ? body["temperature"].as_double() : -1.0;
    pending.max_tokens = body["max_tokens"].as_int(0);

    if (!router_->has_model(pending.model)) {
        respond_error(*connection, 404, "model is not configured: " + pending.model,
                      request.keep_alive());
        return;
    }

    state.pending = std::move(pending);
    state.cache_key.clear();
    state.attempt = 0;
    state.stream = state.pending.stream;
    state.head_sent = false;

    if (!state.stream && state.pending.temperature <= 0.0) {
        std::string material = state.pending.model;
        for (const auto& message : state.pending.messages) {
            material += '|';
            material += message.role;
            material += ':';
            material += message.content;
        }
        state.cache_key = std::to_string(fnv1a64(material));

        if (cache_) {
            if (auto hit = cache_->get(state.cache_key)) {
                Metrics::instance().inc(Counter::CacheHitsTotal);
                respond(*connection, 200, "application/json; charset=utf-8", std::move(*hit),
                        request.keep_alive());
                return;
            }
            Metrics::instance().inc(Counter::CacheMissesTotal);
        }
    }

    if (semantic_cache_) {
        const std::string prompt = last_user_message(state.pending);
        if (!prompt.empty()) {
            if (auto hit = semantic_cache_->get(prompt)) {
                Metrics::instance().inc(Counter::CacheHitsTotal);
                respond(*connection, 200, "application/json; charset=utf-8", std::move(*hit),
                        request.keep_alive());
                return;
            }
        }
    }

    state.in_flight = true;
    try_attempt(connection, connection->id(), 0);
}

void Gateway::try_attempt(net::Connection::Ptr connection, std::uint64_t connection_id,
                          int attempt) {
    auto* state = tls_worker();
    if (state == nullptr || state->http == nullptr) return;

    const auto it = state->connections.find(connection_id);
    if (it == state->connections.end() || !connection->alive()) return;
    auto& connection_state = it->second;

    auto target = router_->pick(connection_state.pending.model, attempt);
    if (!target) {
        connection_state.in_flight = false;
        respond_error(*connection, 404, target.error().message(), connection->alive());
        return;
    }

    ai::OpenAiClient client(*state->http, target->base_url, target->api_key);
    if (!client.configuration_error().ok()) {
        connection_state.in_flight = false;
        respond_error(*connection, 502, client.configuration_error().message(), true);
        return;
    }

    connection_state.attempt = attempt;
    connection_state.stream = connection_state.pending.stream;

    const std::string upstream_name = target->name;
    const std::uint64_t cid = connection_id;

    client.complete(
        connection_state.pending,
        [](std::string_view) {},
        [this, connection, cid, upstream_name](Result<ai::OpenAiClient::Outcome> result) {
            on_upstream_done(connection, cid, upstream_name, std::move(result));
        },
        [this, connection, cid](std::string_view chunk) {
            on_upstream_chunk(connection, cid, chunk);
        },
        [this, cid](std::uint64_t request_id) {
            auto* worker = tls_worker();
            if (worker == nullptr) return;
            const auto entry = worker->connections.find(cid);
            if (entry != worker->connections.end()) {
                entry->second.upstream_request_id = request_id;
            }
        });
}

void Gateway::on_upstream_chunk(net::Connection::Ptr connection, std::uint64_t connection_id,
                                std::string_view chunk) {
    auto* state = tls_worker();
    if (state == nullptr) return;

    const auto it = state->connections.find(connection_id);
    if (it == state->connections.end()) return;
    if (!connection->alive() || !it->second.stream) return;
    auto& connection_state = it->second;

    if (!connection_state.head_sent) {
        connection_state.head_sent = true;
        net::HttpResponse head;
        head.status = 200;
        head.reason = "OK";
        head.headers.set("Content-Type", "text/event-stream; charset=utf-8");
        head.headers.set("Cache-Control", "no-cache");
        head.headers.set("X-Accel-Buffering", "no");
        head.headers.set("Transfer-Encoding", "chunked");
        head.headers.set("Connection", "close");
        Metrics::instance().inc(Counter::HttpResponsesTotal);
        connection->send(net::serialize_response_head(head));
    }

    connection->send(net::chunk_encode(chunk));
    apply_backpressure(connection, connection_id);
}

void Gateway::apply_backpressure(net::Connection::Ptr connection, std::uint64_t connection_id) {
    auto* state = tls_worker();
    if (state == nullptr || state->http == nullptr || state->loop == nullptr) return;

    const auto it = state->connections.find(connection_id);
    if (it == state->connections.end()) return;
    const std::uint64_t request_id = it->second.upstream_request_id;
    if (request_id == 0) return;
    if (connection->pending_output() < kBackpressureHighWatermark) return;

    // The client cannot keep up: stop reading from the upstream for a moment so
    // memory stays bounded instead of buffering the whole completion.
    state->http->pause(request_id);
    state->loop->run_after(kBackpressureRetry, [this, connection, request_id] {
        auto* worker = tls_worker();
        if (worker == nullptr || worker->http == nullptr) return;
        if (!connection->alive()) return;
        worker->http->resume(request_id);
    });
}

void Gateway::on_upstream_done(net::Connection::Ptr connection, std::uint64_t connection_id,
                               std::string upstream_name,
                               Result<ai::OpenAiClient::Outcome> result) {
    auto* state = tls_worker();
    if (state == nullptr) return;

    const auto it = state->connections.find(connection_id);
    if (it == state->connections.end()) return;
    auto& connection_state = it->second;

    if (!result) {
        router_->report_failure(upstream_name);
        Metrics::instance().inc(Counter::HttpErrorsTotal);
        AEGIS_WARN("upstream '{}' failed: {}", upstream_name, result.error().message());

        if (!connection_state.head_sent && connection_state.attempt + 1 < max_attempts() &&
            connection->alive()) {
            try_attempt(connection, connection_id, connection_state.attempt + 1);
            return;
        }

        connection_state.in_flight = false;
        if (connection_state.head_sent) {
            finish_stream(connection, result.error());
            return;
        }
        const int status = result.error().code() == errc::kTimeout ? 504 : 502;
        respond_error(*connection, status, result.error().message(), true);
        return;
    }

    router_->report_success(upstream_name);

    if (connection_state.stream) {
        connection_state.in_flight = false;
        finish_stream(connection, Status::success());
        return;
    }

    std::string body = result->raw_body;
    if (body.empty()) {
        Json document = Json::make_object();
        Json message = Json::make_object();
        message.set("role", "assistant");
        message.set("content", result->content);
        Json choice = Json::make_object();
        choice.set("index", 0);
        choice.set("message", std::move(message));
        choice.set("finish_reason", "stop");
        Json choices = Json::make_array();
        choices.push_back(std::move(choice));
        document.set("id", result->id.empty() ? random_hex(12) : result->id);
        document.set("object", "chat.completion");
        document.set("model", result->model.empty() ? connection_state.pending.model : result->model);
        document.set("choices", std::move(choices));
        Json usage = Json::make_object();
        usage.set("prompt_tokens", result->usage.prompt_tokens);
        usage.set("completion_tokens", result->usage.completion_tokens);
        usage.set("total_tokens", result->usage.prompt_tokens + result->usage.completion_tokens);
        document.set("usage", std::move(usage));
        body = document.dump();
    }

    if (!connection_state.cache_key.empty() && cache_) {
        cache_->put(connection_state.cache_key, body);
    }
    if (semantic_cache_) {
        const std::string prompt = last_user_message(connection_state.pending);
        if (!prompt.empty()) semantic_cache_->put(prompt, body);
    }

    connection_state.in_flight = false;
    respond(*connection, 200, "application/json; charset=utf-8", std::move(body), true);

    // Serve anything that was pipelined behind this request.
    for (;;) {
        auto next = state->connections.find(connection_id);
        if (next == state->connections.end() || !connection->alive()) return;
        if (next->second.in_flight || !next->second.parser.complete()) return;

        dispatch(connection, next->second, next->second.parser.request());
        if (!connection->alive()) return;

        next = state->connections.find(connection_id);
        if (next == state->connections.end()) return;
        next->second.parser.reset();
    }
}

void Gateway::finish_stream(net::Connection::Ptr connection, const Status& status) {
    if (!connection->alive()) return;

    if (!status.ok()) {
        Json error = Json::make_object();
        error.set("message", status.message());
        error.set("type", "upstream_error");
        Json document = Json::make_object();
        document.set("error", std::move(error));
        const std::string frame = "data: " + document.dump() + "\n\n";
        connection->send(net::chunk_encode(frame));
    }
    connection->send(net::chunk_terminator());
    connection->close();
}

void Gateway::handle_models(net::Connection::Ptr connection, const net::HttpRequest& request) {
    Json document = Json::make_object();
    document.set("object", "list");
    Json data = Json::make_array();
    for (const auto& model : router_->models()) {
        Json item = Json::make_object();
        item.set("id", model);
        item.set("object", "model");
        item.set("owned_by", "aegis");
        data.push_back(std::move(item));
    }
    document.set("data", std::move(data));
    respond_json(*connection, 200, document, request.keep_alive());
}

void Gateway::handle_healthz(net::Connection::Ptr connection, const net::HttpRequest& request) {
    Json document = Json::make_object();
    document.set("status", "ok");
    document.set("version", "1.0.0");
    document.set("backend", io_backend());
    respond_json(*connection, 200, document, request.keep_alive());
}

void Gateway::handle_metrics(net::Connection::Ptr connection, const net::HttpRequest& request) {
    respond(*connection, 200, "text/plain; version=0.0.4; charset=utf-8",
            Metrics::instance().render(), request.keep_alive());
}

void Gateway::handle_stats(net::Connection::Ptr connection, const net::HttpRequest& request) {
    Json document = Json::make_object();
    document.set("connections_active", static_cast<int64_t>(active_connections()));

    Json upstreams = Json::make_array();
    for (const auto& entry : router_->health()) {
        Json item = Json::make_object();
        item.set("name", entry.name);
        item.set("healthy", entry.healthy);
        item.set("consecutive_failures", entry.consecutive_failures);
        item.set("picks", static_cast<int64_t>(entry.picks));
        item.set("failures", static_cast<int64_t>(entry.failures));
        upstreams.push_back(std::move(item));
    }
    document.set("upstreams", std::move(upstreams));

    if (cache_) {
        Json cache = Json::make_object();
        const auto stats = cache_->stats();
        cache.set("entries", static_cast<int64_t>(cache_->size()));
        cache.set("hits", static_cast<int64_t>(stats.hits));
        cache.set("misses", static_cast<int64_t>(stats.misses));
        cache.set("evictions", static_cast<int64_t>(stats.evictions));
        cache.set("expirations", static_cast<int64_t>(stats.expirations));
        document.set("cache", std::move(cache));
    }
    if (semantic_cache_) {
        Json cache = Json::make_object();
        const auto stats = semantic_cache_->stats();
        cache.set("entries", static_cast<int64_t>(semantic_cache_->size()));
        cache.set("hits", static_cast<int64_t>(stats.hits));
        cache.set("misses", static_cast<int64_t>(stats.misses));
        document.set("semantic_cache", std::move(cache));
    }

    const auto log_stats = Logger::instance().stats();
    Json logging = Json::make_object();
    logging.set("dropped", static_cast<int64_t>(log_stats.dropped));
    logging.set("written", static_cast<int64_t>(log_stats.written));
    document.set("logging", std::move(logging));

    respond_json(*connection, 200, document, request.keep_alive());
}

void Gateway::respond(net::Connection& connection, int status, std::string_view content_type,
                      std::string body, bool keep_alive, const net::HeaderMap* extra_headers) {
    net::HttpResponse response;
    response.status = status;
    response.reason = net::reason_phrase(status);
    if (extra_headers != nullptr) {
        for (const auto& [name, value] : extra_headers->items()) {
            response.headers.set(name, value);
        }
    }
    response.headers.set("Content-Type", content_type);
    response.headers.set("Server", "aegis/1.0");
    response.headers.set("Connection", keep_alive ? "keep-alive" : "close");
    response.body = std::move(body);

    Metrics::instance().inc(Counter::HttpResponsesTotal);
    if (status >= 400) Metrics::instance().inc(Counter::HttpErrorsTotal);

    connection.send(net::serialize_response(response));
    if (!keep_alive) connection.close();
}

void Gateway::respond_json(net::Connection& connection, int status, const Json& body,
                           bool keep_alive) {
    respond(connection, status, "application/json; charset=utf-8", body.dump(), keep_alive);
}

void Gateway::respond_error(net::Connection& connection, int status, std::string message,
                            bool keep_alive) {
    Json error = Json::make_object();
    error.set("message", std::move(message));
    error.set("type", status >= 500 ? "upstream_error" : "invalid_request_error");
    Json document = Json::make_object();
    document.set("error", std::move(error));
    respond_json(connection, status, document, keep_alive);
}

}  // namespace aegis::app
