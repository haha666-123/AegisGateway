#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "ai/Sse.h"
#include "ai/Tokenizer.h"
#include "app/Config.h"
#include "app/Gateway.h"
#include "common/Json.h"
#include "common/MpscQueue.h"
#include "common/RateLimiter.h"
#include "net/Http.h"
#include "net/Socket.h"
#include "MockUpstream.h"

namespace {

using Clock = std::chrono::steady_clock;

double seconds_since(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

void report(const char* name, double ops, double bytes_per_op) {
    std::printf("%-22s %10.0f ops/s", name, ops);
    if (bytes_per_op > 0.0) {
        std::printf("   %8.2f MB/s", ops * bytes_per_op / (1024.0 * 1024.0));
    }
    std::printf("\n");
}

std::string sample_chat_request() {
    std::string text =
        R"({"model":"gpt-4o-mini","temperature":0.2,"max_tokens":512,"stream":false,"messages":[)";
    for (int i = 0; i < 8; ++i) {
        if (i > 0) text += ',';
        text += R"({"role":"user","content":"Please summarise the following incident report number )";
        text += std::to_string(i);
        text += R"( and list the three most likely root causes with evidence."})";
    }
    text += "]}";
    return text;
}

std::string sample_http_request() {
    const std::string body = sample_chat_request();
    std::string request = "POST /v1/chat/completions HTTP/1.1\r\n";
    request += "Host: 127.0.0.1:8080\r\n";
    request += "Content-Type: application/json\r\n";
    request += "Authorization: Bearer sk-benchmark\r\n";
    request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    request += "\r\n";
    request += body;
    return request;
}

std::string sample_sse_stream() {
    std::string stream;
    for (int i = 0; i < 16; ++i) {
        stream += R"(data: {"id":"chatcmpl-x","choices":[{"index":0,"delta":{"content":"token )";
        stream += std::to_string(i);
        stream += R"( "}}]})";
        stream += "\n\n";
    }
    stream += "data: [DONE]\n\n";
    return stream;
}

std::string sample_text() {
    return "The gateway forwards streaming completions without buffering the whole answer, "
           "which keeps tail latency low and memory bounded under load. "
           "epoll reports readiness, the loop reads until EAGAIN, and the parser is incremental.";
}

void bench_json_parse() {
    const std::string text = sample_chat_request();
    const int iterations = 200000;
    std::size_t checksum = 0;
    const auto start = Clock::now();
    for (int i = 0; i < iterations; ++i) {
        auto parsed = aegis::parse_json(text);
        checksum += parsed ? parsed->size() : 0;
    }
    const double elapsed = seconds_since(start);
    report("json_parse", iterations / elapsed, static_cast<double>(text.size()));
    std::printf("  (checksum %zu)\n", checksum);
}

void bench_http_parse() {
    const std::string request = sample_http_request();
    const int iterations = 200000;
    std::size_t checksum = 0;
    aegis::net::HttpRequestParser parser;
    const auto start = Clock::now();
    for (int i = 0; i < iterations; ++i) {
        const auto state = parser.feed(request);
        if (state == aegis::net::HttpRequestParser::State::Complete) {
            checksum += parser.request().body.size();
        }
        parser.reset();
    }
    const double elapsed = seconds_since(start);
    report("http_request_parse", iterations / elapsed, static_cast<double>(request.size()));
    std::printf("  (checksum %zu)\n", checksum);
}

void bench_sse_parse() {
    const std::string stream = sample_sse_stream();
    const int iterations = 200000;
    std::size_t events = 0;
    aegis::ai::SseParser parser;
    const auto start = Clock::now();
    for (int i = 0; i < iterations; ++i) {
        parser.feed(stream, [&events](const aegis::ai::SseEvent&) { ++events; });
        parser.reset();
    }
    const double elapsed = seconds_since(start);
    report("sse_parse", iterations / elapsed, static_cast<double>(stream.size()));
    std::printf("  (events %zu)\n", events);
}

void bench_tokenizer() {
    const std::string text = sample_text();
    const int iterations = 1000000;
    long long total = 0;
    const auto start = Clock::now();
    for (int i = 0; i < iterations; ++i) total += aegis::ai::estimate_tokens(text);
    const double elapsed = seconds_since(start);
    report("tokenizer", iterations / elapsed, static_cast<double>(text.size()));
    std::printf("  (tokens %lld)\n", total);
}

void bench_mpsc_queue() {
    constexpr int kProducers = 4;
    constexpr int kPerProducer = 500000;
    constexpr std::size_t kCapacity = 8192;

    aegis::MpscBoundedQueue<int, kCapacity> queue;
    std::atomic<bool> start{false};
    std::vector<std::thread> producers;
    producers.reserve(kProducers);

    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&queue, &start, p] {
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            for (int i = 0; i < kPerProducer; ++i) {
                while (!queue.try_push(p * kPerProducer + i)) std::this_thread::yield();
            }
        });
    }

    start.store(true, std::memory_order_release);
    const auto clock_start = Clock::now();
    long long consumed = 0;
    int value = 0;
    const long long target = static_cast<long long>(kProducers) * kPerProducer;
    while (consumed < target) {
        if (queue.try_pop(value)) {
            ++consumed;
        } else {
            std::this_thread::yield();
        }
    }
    const double elapsed = seconds_since(clock_start);
    for (auto& producer : producers) producer.join();

    std::printf("%-22s %10.0f ops/s   (%lld items, %d producers)\n", "mpsc_queue",
                static_cast<double>(consumed) / elapsed, consumed, kProducers);
}

void bench_rate_limiter() {
    aegis::RateLimiter limiter(1e9, 1e9);
    const int iterations = 2000000;
    int allowed = 0;
    const auto start = Clock::now();
    for (int i = 0; i < iterations; ++i) {
        if (limiter.acquire("benchmark-key").allowed) ++allowed;
    }
    const double elapsed = seconds_since(start);
    report("rate_limiter", iterations / elapsed, 0.0);
    std::printf("  (allowed %d)\n", allowed);
}

// One blocking request against the gateway, used by the end to end benchmark.
bool one_request(std::uint16_t port, const std::string& request, std::string* body) {
    auto handle = aegis::net::create_tcp_socket();
    if (!handle) return false;
    auto endpoints = aegis::net::resolve("127.0.0.1", port);
    if (!endpoints) {
        aegis::net::close_socket(*handle);
        return false;
    }
    if (aegis::net::connect_tcp(*handle, endpoints->front()).status != aegis::net::IoStatus::Ok) {
        aegis::net::close_socket(*handle);
        return false;
    }
    (void)aegis::net::set_recv_timeout(*handle, std::chrono::milliseconds(500));
    (void)aegis::net::write_some(*handle, request.data(), request.size());

    aegis::net::HttpResponseParser parser;
    char buffer[4096];
    bool ok = false;
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    while (Clock::now() < deadline) {
        const auto outcome = aegis::net::read_some(*handle, buffer, sizeof(buffer));
        if (outcome.ok()) {
            const auto state = parser.feed(
                std::string_view(buffer, outcome.bytes),
                [body](std::string_view chunk) { body->append(chunk); });
            if (state == aegis::net::HttpResponseParser::State::Complete) {
                ok = parser.status() == 200;
                break;
            }
            if (state == aegis::net::HttpResponseParser::State::Failed) break;
            continue;
        }
        if (outcome.status == aegis::net::IoStatus::Closed) {
            ok = parser.finish_eof() == aegis::net::HttpResponseParser::State::Complete;
            break;
        }
    }
    aegis::net::close_socket(*handle);
    return ok;
}

void bench_end_to_end() {
    aegis::tools::MockUpstream::Options upstream_options;
    upstream_options.port = 0;
    upstream_options.tokens = 4;
    upstream_options.token_delay = std::chrono::milliseconds(0);
    aegis::tools::MockUpstream upstream(upstream_options);
    if (!upstream.start()) {
        std::printf("%-22s skipped (mock upstream failed to start)\n", "end_to_end");
        return;
    }

    aegis::app::Config config;
    config.server.host = "127.0.0.1";
    config.server.port = 0;
    config.server.io_threads = 1;
    config.logging.level = aegis::LogLevel::Error;
    config.logging.console = false;
    config.upstreams.push_back({"mock", "http://127.0.0.1:" + std::to_string(upstream.port()),
                                "k", {"mock-1"}, 1});
    config.gateway_keys = {"sk-bench"};
    // Caching is disabled so every request really reaches the upstream.
    config.cache.enabled = false;
    config.rate_limit.enabled = false;

    aegis::app::Gateway gateway(config);
    if (!gateway.start()) {
        std::printf("%-22s skipped (gateway failed to start)\n", "end_to_end");
        upstream.stop();
        return;
    }

    const std::string body =
        R"({"model":"mock-1","messages":[{"role":"user","content":"benchmark"}]})";
    std::string request = "POST /v1/chat/completions HTTP/1.1\r\nHost: 127.0.0.1\r\n";
    request += "Authorization: Bearer sk-bench\r\nContent-Type: application/json\r\n";
    request += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;

    constexpr int kThreads = 4;
    constexpr int kPerThread = 100;
    std::atomic<int> completed{0};

    const auto start = Clock::now();
    std::vector<std::thread> clients;
    clients.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        clients.emplace_back([&] {
            std::string response_body;
            for (int i = 0; i < kPerThread; ++i) {
                response_body.clear();
                if (one_request(gateway.port(), request, &response_body)) {
                    completed.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& client : clients) client.join();
    const double elapsed = seconds_since(start);

    std::printf("%-22s %10.0f req/s   (%d requests, %d client threads, 1 io thread)\n",
                "end_to_end", static_cast<double>(completed.load()) / elapsed, completed.load(),
                kThreads);

    gateway.stop();
    upstream.stop();
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    bool run_all = true;
    std::string only;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg.starts_with("--only=")) {
            only = std::string(arg.substr(7));
            run_all = false;
        } else if (arg == "--all") {
            run_all = true;
        } else if (arg == "--help" || arg == "-h") {
            std::printf(
                "aegis_bench - micro benchmarks\n"
                "\n"
                "usage: aegis_bench [--all | --only=<name>]\n"
                "  names: json, http, sse, tokenizer, mpsc, ratelimit\n");
            return 0;
        }
    }

    const auto run = [&](const char* name, void (*fn)()) {
        if (!run_all && only != name) return;
        fn();
    };

#if defined(NDEBUG)
    const char* build = "release";
#else
    const char* build = "debug";
#endif
    std::printf("aegis_bench (%s build, gcc %s)\n", build, __VERSION__);

    run("json", bench_json_parse);
    run("http", bench_http_parse);
    run("sse", bench_sse_parse);
    run("tokenizer", bench_tokenizer);
    run("mpsc", bench_mpsc_queue);
    run("ratelimit", bench_rate_limiter);
    run("e2e", bench_end_to_end);
    return 0;
}
