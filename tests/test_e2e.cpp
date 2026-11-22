#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "MockUpstream.h"
#include "TestFramework.h"
#include "app/Config.h"
#include "app/Gateway.h"
#include "common/Log.h"
#include "net/Http.h"
#include "net/Socket.h"

using namespace aegis;
using namespace std::chrono_literals;

namespace {

struct ResponseData {
    int status{0};
    net::HeaderMap headers;
    std::string body;
};

class RawClient {
public:
    static std::unique_ptr<RawClient> connect_to(std::uint16_t port) {
        auto handle = net::create_tcp_socket();
        if (!handle) return nullptr;
        auto endpoints = net::resolve("127.0.0.1", port);
        if (!endpoints) {
            net::close_socket(*handle);
            return nullptr;
        }
        const auto outcome = net::connect_tcp(*handle, endpoints->front());
        if (outcome.status != net::IoStatus::Ok) {
            net::close_socket(*handle);
            return nullptr;
        }
        (void)net::set_recv_timeout(*handle, 200ms);
        return std::unique_ptr<RawClient>(new RawClient(*handle));
    }

    ~RawClient() { close(); }

    bool send(std::string_view data) {
        return net::write_some(handle_, data.data(), data.size()).ok();
    }

    bool read_response(ResponseData& response, std::chrono::milliseconds timeout = 5000ms) {
        net::HttpResponseParser parser;
        const auto deadline = steady_now() + timeout;
        char buffer[8192];

        response.body.clear();
        while (steady_now() < deadline) {
            const auto outcome = net::read_some(handle_, buffer, sizeof(buffer));
            if (outcome.ok()) {
                const auto state = parser.feed(
                    std::string_view(buffer, outcome.bytes),
                    [&response](std::string_view chunk) { response.body.append(chunk); });
                if (state == net::HttpResponseParser::State::Failed) return false;
                if (state == net::HttpResponseParser::State::Complete) {
                    response.status = parser.status();
                    response.headers = parser.headers();
                    return true;
                }
                continue;
            }
            if (outcome.status == net::IoStatus::Closed) {
                if (parser.finish_eof() == net::HttpResponseParser::State::Complete) {
                    response.status = parser.status();
                    response.headers = parser.headers();
                    return true;
                }
                return false;
            }
        }
        return false;
    }

    void close() {
        if (handle_ != net::kInvalidHandle) {
            net::close_socket(handle_);
            handle_ = net::kInvalidHandle;
        }
    }

private:
    explicit RawClient(net::Handle handle) : handle_(handle) {}
    net::Handle handle_{net::kInvalidHandle};
};

ResponseData http_call(std::uint16_t port, const std::string& raw_request) {
    ResponseData response;
    auto client = RawClient::connect_to(port);
    if (!client) return response;
    if (!client->send(raw_request)) return response;
    (void)client->read_response(response);
    return response;
}

std::string chat_body(std::string_view content, bool stream, std::string_view model = "mock-1") {
    std::string body = "{\"model\":\"" + std::string(model) + "\",\"messages\":[{\"role\":\"user\","
                       "\"content\":\"" + std::string(content) + "\"}]";
    if (stream) body += ",\"stream\":true";
    body += "}";
    return body;
}

std::string chat_request(std::string_view body, std::string_view key = "sk-test",
                         std::string_view host = "127.0.0.1") {
    std::string request = "POST /v1/chat/completions HTTP/1.1\r\nHost: ";
    request += std::string(host) + "\r\nContent-Type: application/json\r\n";
    if (!key.empty()) request += "Authorization: Bearer " + std::string(key) + "\r\n";
    request += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
    request += std::string(body);
    return request;
}

std::string get_request(std::string_view path, std::string_view key = "sk-test") {
    std::string request = "GET ";
    request += std::string(path) + " HTTP/1.1\r\nHost: 127.0.0.1\r\n";
    if (!key.empty()) request += "Authorization: Bearer " + std::string(key) + "\r\n";
    request += "\r\n";
    return request;
}

app::Config make_config(std::uint16_t upstream_port, bool cache_enabled = true,
                        double rps = 200.0, double burst = 200.0) {
    app::Config config;
    config.server.host = "127.0.0.1";
    config.server.port = 0;
    config.server.io_threads = 1;
    config.server.idle_timeout = 5000ms;
    config.logging.level = LogLevel::Warn;
    config.logging.console = false;
    config.upstreams.push_back(
        {"mock", "http://127.0.0.1:" + std::to_string(upstream_port), "upstream-key", {"mock-1"}, 1});
    config.gateway_keys = {"sk-test"};
    config.cache.enabled = cache_enabled;
    config.cache.capacity = 64;
    config.cache.ttl = 60s;
    config.rate_limit.enabled = true;
    config.rate_limit.requests_per_second = rps;
    config.rate_limit.burst = burst;
    config.timeouts.connect = 2000ms;
    config.timeouts.upstream = 10000ms;
    return config;
}

}  // namespace

AEGIS_TEST(e2e_health_models_metrics) {
    tools::MockUpstream::Options options;
    options.port = 0;
    tools::MockUpstream upstream(options);
    AEGIS_CHECK(upstream.start().has_value());

    app::Gateway gateway(make_config(upstream.port()));
    AEGIS_CHECK(gateway.start().has_value());
    AEGIS_CHECK(gateway.port() != 0);

    const auto health = http_call(gateway.port(), get_request("/healthz", ""));
    AEGIS_CHECK_EQ(health.status, 200);
    AEGIS_CHECK(health.body.find("\"status\":\"ok\"") != std::string::npos);

    const auto models = http_call(gateway.port(), get_request("/v1/models"));
    AEGIS_CHECK_EQ(models.status, 200);
    AEGIS_CHECK(models.body.find("mock-1") != std::string::npos);

    const auto metrics = http_call(gateway.port(), get_request("/metrics"));
    AEGIS_CHECK_EQ(metrics.status, 200);
    AEGIS_CHECK(metrics.body.find("aegis_http_requests_total") != std::string::npos);

    const auto stats = http_call(gateway.port(), get_request("/admin/stats"));
    AEGIS_CHECK_EQ(stats.status, 200);
    AEGIS_CHECK(stats.body.find("upstreams") != std::string::npos);

    const auto missing = http_call(gateway.port(), get_request("/nope"));
    AEGIS_CHECK_EQ(missing.status, 404);

    gateway.stop();
    upstream.stop();
}

AEGIS_TEST(e2e_chat_non_stream_and_cache) {
    tools::MockUpstream::Options options;
    options.port = 0;
    options.reply = "cached answer";
    tools::MockUpstream upstream(options);
    AEGIS_CHECK(upstream.start().has_value());

    app::Gateway gateway(make_config(upstream.port(), true));
    AEGIS_CHECK(gateway.start().has_value());

    const std::string body = chat_body("what is aegis", false);
    const auto first = http_call(gateway.port(), chat_request(body));
    AEGIS_CHECK_EQ(first.status, 200);
    AEGIS_CHECK(first.body.find("cached answer") != std::string::npos);

    const int after_first = upstream.requests_served();
    AEGIS_CHECK_EQ(after_first, 1);

    const auto second = http_call(gateway.port(), chat_request(body));
    AEGIS_CHECK_EQ(second.status, 200);
    AEGIS_CHECK_EQ(second.body, first.body);
    AEGIS_CHECK_EQ(upstream.requests_served(), after_first);  // served from cache

    // A different prompt must miss the cache.
    const auto third = http_call(gateway.port(), chat_request(chat_body("another question", false)));
    AEGIS_CHECK_EQ(third.status, 200);
    AEGIS_CHECK_EQ(upstream.requests_served(), after_first + 1);

    gateway.stop();
    upstream.stop();
}

AEGIS_TEST(e2e_chat_stream) {
    tools::MockUpstream::Options options;
    options.port = 0;
    options.tokens = 5;
    options.token_delay = 2ms;
    tools::MockUpstream upstream(options);
    AEGIS_CHECK(upstream.start().has_value());

    app::Gateway gateway(make_config(upstream.port()));
    AEGIS_CHECK(gateway.start().has_value());

    const auto response = http_call(gateway.port(), chat_request(chat_body("stream please", true)));
    AEGIS_CHECK_EQ(response.status, 200);
    AEGIS_CHECK(response.body.find("data: ") != std::string::npos);
    AEGIS_CHECK(response.body.find("[DONE]") != std::string::npos);
    AEGIS_CHECK(response.body.find("chat.completion.chunk") != std::string::npos);
    AEGIS_CHECK(response.body.find("\"usage\"") != std::string::npos);

    // Streaming responses are not cached.
    AEGIS_CHECK(gateway.router().upstream_count() == 1);

    gateway.stop();
    upstream.stop();
}

AEGIS_TEST(e2e_auth_and_validation) {
    tools::MockUpstream::Options options;
    options.port = 0;
    tools::MockUpstream upstream(options);
    AEGIS_CHECK(upstream.start().has_value());

    app::Gateway gateway(make_config(upstream.port()));
    AEGIS_CHECK(gateway.start().has_value());

    const auto no_auth = http_call(gateway.port(), chat_request(chat_body("hi", false), ""));
    AEGIS_CHECK_EQ(no_auth.status, 401);

    const auto wrong_key = http_call(gateway.port(), chat_request(chat_body("hi", false), "sk-nope"));
    AEGIS_CHECK_EQ(wrong_key.status, 401);

    const auto bad_model =
        http_call(gateway.port(), chat_request(chat_body("hi", false, "unknown-model")));
    AEGIS_CHECK_EQ(bad_model.status, 404);

    const auto bad_json = http_call(gateway.port(), chat_request("{not json"));
    AEGIS_CHECK_EQ(bad_json.status, 400);

    const auto wrong_method = http_call(gateway.port(),
                                        "PUT /v1/chat/completions HTTP/1.1\r\nHost: x\r\n"
                                        "Authorization: Bearer sk-test\r\n\r\n");
    AEGIS_CHECK_EQ(wrong_method.status, 405);

    gateway.stop();
    upstream.stop();
}

AEGIS_TEST(e2e_rate_limit) {
    tools::MockUpstream::Options options;
    options.port = 0;
    tools::MockUpstream upstream(options);
    AEGIS_CHECK(upstream.start().has_value());

    // One request per second, burst of one: the second call must be rejected.
    app::Gateway gateway(make_config(upstream.port(), false, 0.5, 1.0));
    AEGIS_CHECK(gateway.start().has_value());

    const auto body = chat_body("rate limited question", false);
    const auto first = http_call(gateway.port(), chat_request(body));
    AEGIS_CHECK_EQ(first.status, 200);

    const auto second = http_call(gateway.port(), chat_request(body));
    AEGIS_CHECK_EQ(second.status, 429);
    AEGIS_CHECK(second.headers.contains("Retry-After"));
    AEGIS_CHECK(second.body.find("rate limit") != std::string::npos);

    gateway.stop();
    upstream.stop();
}

AEGIS_TEST(e2e_failover_to_healthy_upstream) {
    tools::MockUpstream::Options options;
    options.port = 0;
    tools::MockUpstream upstream(options);
    AEGIS_CHECK(upstream.start().has_value());

    auto config = make_config(upstream.port(), false);
    // A dead upstream is tried first, the healthy one is the fallback.
    config.upstreams.insert(config.upstreams.begin(),
                            ai::UpstreamConfig{"dead", "http://127.0.0.1:1", "k", {"mock-1"}, 1});

    app::Gateway gateway(config);
    AEGIS_CHECK(gateway.start().has_value());

    for (int i = 0; i < 4; ++i) {
        const auto response =
            http_call(gateway.port(), chat_request(chat_body("failover " + std::to_string(i), false)));
        AEGIS_CHECK_EQ(response.status, 200);
    }

    bool dead_failed = false;
    for (const auto& health : gateway.router().health()) {
        if (health.name == "dead" && health.failures > 0) dead_failed = true;
    }
    AEGIS_CHECK(dead_failed);

    gateway.stop();
    upstream.stop();
}

AEGIS_TEST(e2e_body_limit) {
    tools::MockUpstream::Options options;
    options.port = 0;
    tools::MockUpstream upstream(options);
    AEGIS_CHECK(upstream.start().has_value());

    auto config = make_config(upstream.port());
    config.limits.max_body_bytes = 256;

    app::Gateway gateway(config);
    AEGIS_CHECK(gateway.start().has_value());

    const std::string big_content(1024, 'x');
    const auto response = http_call(gateway.port(), chat_request(chat_body(big_content, false)));
    AEGIS_CHECK_EQ(response.status, 413);

    gateway.stop();
    upstream.stop();
}
