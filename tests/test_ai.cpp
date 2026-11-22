#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "MockUpstream.h"
#include "TestFramework.h"
#include "ai/Cache.h"
#include "ai/Embedding.h"
#include "ai/OpenAiClient.h"
#include "ai/Prompt.h"
#include "ai/Router.h"
#include "ai/Sse.h"
#include "ai/Tokenizer.h"
#include "common/RateLimiter.h"
#include "common/ThreadPool.h"
#include "net/EventLoop.h"
#include "net/HttpClient.h"
#include "net/Resolver.h"

using namespace aegis;
using namespace aegis::ai;
using namespace std::chrono_literals;

namespace {

template <class Predicate>
bool wait_for(Predicate predicate, std::chrono::milliseconds timeout = 5000ms) {
    const auto deadline = steady_now() + timeout;
    while (steady_now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(2ms);
    }
    return predicate();
}

}  // namespace

AEGIS_TEST(sse_parse_basic_and_split) {
    const std::string raw =
        "data: {\"a\":1}\n\n"
        "data: second\n\n"
        ": comment line\n"
        "data: third\n\n";

    SseParser parser;
    std::vector<std::string> events;
    parser.feed(raw, [&events](const SseEvent& event) { events.push_back(event.data); });
    AEGIS_CHECK_EQ(events.size(), std::size_t{3});
    AEGIS_CHECK_EQ(events[0], std::string("{\"a\":1}"));
    AEGIS_CHECK_EQ(events[2], std::string("third"));
    AEGIS_CHECK_EQ(parser.events_dispatched(), std::size_t{3});

    // Every possible split point must yield identical events.
    for (std::size_t split = 1; split < raw.size(); ++split) {
        SseParser split_parser;
        std::vector<std::string> split_events;
        split_parser.feed(std::string_view(raw).substr(0, split),
                          [&split_events](const SseEvent& event) {
                              split_events.push_back(event.data);
                          });
        split_parser.feed(std::string_view(raw).substr(split),
                          [&split_events](const SseEvent& event) {
                              split_events.push_back(event.data);
                          });
        AEGIS_CHECK_EQ(split_events.size(), std::size_t{3});
        AEGIS_CHECK_EQ(split_events[0], std::string("{\"a\":1}"));
    }
}

AEGIS_TEST(sse_multiline_data_and_fields) {
    SseParser parser;
    std::vector<SseEvent> events;
    parser.feed("event: update\nid: 42\nretry: 1500\ndata: line1\ndata: line2\n\n",
                [&events](const SseEvent& event) { events.push_back(event); });
    AEGIS_CHECK_EQ(events.size(), std::size_t{1});
    AEGIS_CHECK_EQ(events[0].event, std::string("update"));
    AEGIS_CHECK_EQ(events[0].id, std::string("42"));
    AEGIS_CHECK(events[0].has_retry);
    AEGIS_CHECK_EQ(events[0].retry, 1500);
    AEGIS_CHECK_EQ(events[0].data, std::string("line1\nline2"));
}

AEGIS_TEST(tokenizer_estimates) {
    AEGIS_CHECK_EQ(estimate_tokens(""), 0);
    AEGIS_CHECK_EQ(estimate_tokens("hello"), 2);        // 5 chars / 4, rounded up
    AEGIS_CHECK_EQ(estimate_tokens("hello world"), 4);  // two 5 char runs
    // UTF-8 bytes for four CJK code points: one token each.
    AEGIS_CHECK_EQ(estimate_tokens("\xE4\xBD\xA0\xE5\xA5\xBD\xE4\xB8\x96\xE7\x95\x8C"), 4);
    AEGIS_CHECK(estimate_tokens("a, b!") >= 2);
    AEGIS_CHECK_EQ(estimate_message_tokens("user", "hi"), 4 + 1 + 1);
    AEGIS_CHECK(estimate_chat_tokens({{"user", "hi"}}) > 3);
}

AEGIS_TEST(prompt_render) {
    PromptTemplate prompt("You are {{role}}. Answer in {{language}}.");
    const auto variables = prompt.variables();
    AEGIS_CHECK_EQ(variables.size(), std::size_t{2});
    AEGIS_CHECK_EQ(variables[0], std::string("role"));

    std::unordered_map<std::string, std::string> values{{"role", "a reviewer"},
                                                        {"language", "Chinese"}};
    const auto rendered = prompt.render(values);
    AEGIS_CHECK(rendered.has_value());
    AEGIS_CHECK_EQ(*rendered, std::string("You are a reviewer. Answer in Chinese."));

    values.erase("language");
    const auto missing = prompt.render(values);
    AEGIS_CHECK(!missing.has_value());
    AEGIS_CHECK_EQ(missing.error().code(), errc::kInvalidArgument);

    PromptTemplate broken("hello {{name");
    AEGIS_CHECK(!broken.render({}).has_value());
}

AEGIS_TEST(embedding_similarity) {
    HashingEmbedder embedder(128);
    AEGIS_CHECK_EQ(embedder.dimension(), std::size_t{128});

    const auto a = embedder.embed("how do I reset my password");
    const auto b = embedder.embed("how do I reset my password");
    const auto c = embedder.embed("what is the weather in hangzhou");
    AEGIS_CHECK_EQ(a.size(), std::size_t{128});
    AEGIS_CHECK(cosine_similarity(a, b) > 0.999F);
    AEGIS_CHECK(cosine_similarity(a, c) < cosine_similarity(a, b));

    const auto empty = embedder.embed("");
    AEGIS_CHECK_EQ(cosine_similarity(empty, empty), 0.0F);
    AEGIS_CHECK_EQ(cosine_similarity(a, empty), 0.0F);
}

AEGIS_TEST(lru_cache_behaviour) {
    LruCache cache(2, std::chrono::seconds(60));
    cache.put("a", "1");
    cache.put("b", "2");
    AEGIS_CHECK_EQ(cache.size(), std::size_t{2});

    auto value = cache.get("a");
    AEGIS_CHECK(value.has_value());
    AEGIS_CHECK_EQ(*value, std::string("1"));

    // "b" is now the least recently used.
    cache.put("c", "3");
    AEGIS_CHECK_EQ(cache.size(), std::size_t{2});
    AEGIS_CHECK(!cache.get("b").has_value());
    AEGIS_CHECK(cache.get("a").has_value());
    AEGIS_CHECK(cache.get("c").has_value());
    AEGIS_CHECK_EQ(cache.stats().hits, std::uint64_t{3});
    AEGIS_CHECK_EQ(cache.stats().evictions, std::uint64_t{1});

    LruCache expiring(4, std::chrono::seconds(0));
    expiring.put("x", "1");
    std::this_thread::sleep_for(5ms);
    AEGIS_CHECK(!expiring.get("x").has_value());
    AEGIS_CHECK_EQ(expiring.stats().expirations, std::uint64_t{1});
}

AEGIS_TEST(semantic_cache_behaviour) {
    HashingEmbedder embedder(256);
    SemanticCache cache(embedder, 8, std::chrono::seconds(60), 0.8F);

    cache.put("how to reset password", "answer-1");
    const auto hit = cache.get("how to reset password");
    AEGIS_CHECK(hit.has_value());
    AEGIS_CHECK_EQ(*hit, std::string("answer-1"));

    const auto miss = cache.get("completely unrelated question about weather");
    AEGIS_CHECK(!miss.has_value());
    AEGIS_CHECK_EQ(cache.stats().hits, std::uint64_t{1});
    AEGIS_CHECK_EQ(cache.stats().misses, std::uint64_t{1});
}

AEGIS_TEST(rate_limiter_token_bucket) {
    RateLimiter limiter(1000.0, 2.0);
    AEGIS_CHECK(limiter.acquire("k").allowed);
    AEGIS_CHECK(limiter.acquire("k").allowed);

    const auto blocked = limiter.acquire("k");
    AEGIS_CHECK(!blocked.allowed);
    AEGIS_CHECK(blocked.retry_after_seconds > 0.0);

    // A different key has its own bucket.
    AEGIS_CHECK(limiter.acquire("other").allowed);

    std::this_thread::sleep_for(10ms);
    AEGIS_CHECK(limiter.acquire("k").allowed);
}

AEGIS_TEST(router_weighted_round_robin_and_failover) {
    std::vector<UpstreamConfig> configs{
        {"primary", "http://127.0.0.1:9001", "k1", {"mock-1"}, 3},
        {"backup", "http://127.0.0.1:9002", "k2", {"mock-1"}, 1},
    };
    Router router(configs);
    AEGIS_CHECK(router.has_model("mock-1"));
    AEGIS_CHECK(!router.has_model("gpt-4"));

    int primary = 0;
    int backup = 0;
    for (int i = 0; i < 40; ++i) {
        auto target = router.pick("mock-1", 0);
        AEGIS_CHECK(target.has_value());
        if (target->name == "primary") ++primary;
        if (target->name == "backup") ++backup;
    }
    AEGIS_CHECK_EQ(primary + backup, 40);
    AEGIS_CHECK(primary > backup);  // 3:1 weighting

    // Failover walks to a different upstream for attempt >= 1.
    auto first = router.pick("mock-1", 0);
    auto second = router.pick("mock-1", 1);
    AEGIS_CHECK(first.has_value() && second.has_value());
    AEGIS_CHECK_NE(first->name, second->name);
}

AEGIS_TEST(router_circuit_breaker) {
    std::vector<UpstreamConfig> configs{
        {"primary", "http://127.0.0.1:9001", "k1", {"mock-1"}, 1},
        {"backup", "http://127.0.0.1:9002", "k2", {"mock-1"}, 1},
    };
    Router router(configs, 2, std::chrono::seconds(60));

    router.report_failure("primary");
    router.report_failure("primary");
    for (int i = 0; i < 6; ++i) {
        auto target = router.pick("mock-1", 0);
        AEGIS_CHECK(target.has_value());
        AEGIS_CHECK_EQ(target->name, std::string("backup"));
    }

    bool primary_unhealthy = false;
    for (const auto& health : router.health()) {
        if (health.name == "primary") primary_unhealthy = !health.healthy;
    }
    AEGIS_CHECK(primary_unhealthy);

    router.report_success("primary");
    bool recovered = false;
    for (int i = 0; i < 8; ++i) {
        const auto target = router.pick("mock-1", 0);
        if (target && target->name == "primary") recovered = true;
    }
    AEGIS_CHECK(recovered);
}

AEGIS_TEST(openai_client_non_stream) {
    tools::MockUpstream::Options options;
    options.port = 0;
    options.tokens = 5;
    options.reply = "non stream reply";
    tools::MockUpstream upstream(options);
    AEGIS_CHECK(upstream.start().has_value());

    ThreadPool pool(2, "ai-test");
    net::Resolver resolver(pool);
    net::EventLoop loop;
    std::thread runner([&loop] { loop.run(); });

    std::atomic<bool> done{false};
    Result<OpenAiClient::Outcome> captured = fail(errc::kInternal, "not set");

    net::HttpClient http(loop, resolver);
    loop.post([&] {
        OpenAiClient client(http, "http://127.0.0.1:" + std::to_string(upstream.port()), "sk-test");
        OpenAiClient::Request request;
        request.model = "mock-1";
        request.messages.push_back({"user", "hello"});
        request.stream = false;
        client.complete(
            request, [](std::string_view) {},
            [&](Result<OpenAiClient::Outcome> result) {
                captured = std::move(result);
                done.store(true);
            });
    });

    AEGIS_CHECK(wait_for([&done] { return done.load(); }));
    AEGIS_CHECK(captured.has_value());
    if (captured) {
        AEGIS_CHECK_EQ(captured->content, std::string("non stream reply"));
        AEGIS_CHECK_EQ(captured->usage.completion_tokens, 5);
        AEGIS_CHECK(captured->raw_body.find("non stream reply") != std::string::npos);
    }

    loop.stop();
    runner.join();
    upstream.stop();
}

AEGIS_TEST(openai_client_stream) {
    tools::MockUpstream::Options options;
    options.port = 0;
    options.tokens = 4;
    tools::MockUpstream upstream(options);
    AEGIS_CHECK(upstream.start().has_value());

    ThreadPool pool(2, "ai-test");
    net::Resolver resolver(pool);
    net::EventLoop loop;
    std::thread runner([&loop] { loop.run(); });

    std::atomic<bool> done{false};
    std::string raw;
    std::string content;
    Result<OpenAiClient::Outcome> captured = fail(errc::kInternal, "not set");

    net::HttpClient http(loop, resolver);
    loop.post([&] {
        OpenAiClient client(http, "http://127.0.0.1:" + std::to_string(upstream.port()), "sk-test");
        OpenAiClient::Request request;
        request.model = "mock-1";
        request.messages.push_back({"user", "stream please"});
        request.stream = true;
        client.complete(
            request,
            [&content](std::string_view delta) { content.append(delta); },
            [&](Result<OpenAiClient::Outcome> result) {
                captured = std::move(result);
                done.store(true);
            },
            [&raw](std::string_view chunk) { raw.append(chunk); });
    });

    AEGIS_CHECK(wait_for([&done] { return done.load(); }));
    AEGIS_CHECK(captured.has_value());
    if (captured) {
        AEGIS_CHECK(captured->content.find("tok") != std::string::npos);
        AEGIS_CHECK_EQ(captured->usage.completion_tokens, 4);
    }
    AEGIS_CHECK(raw.find("data: ") != std::string::npos);
    AEGIS_CHECK(raw.find("[DONE]") != std::string::npos);
    AEGIS_CHECK_EQ(content, captured->content);

    loop.stop();
    runner.join();
    upstream.stop();
}
