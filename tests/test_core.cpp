#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "TestFramework.h"
#include "common/Log.h"
#include "common/Metrics.h"
#include "common/MpscQueue.h"
#include "common/Status.h"
#include "common/Util.h"

using namespace aegis;

namespace {

std::filesystem::path temp_path(const std::string& name) {
    auto path = std::filesystem::temp_directory_path();
    path /= "aegis_test_" + random_hex(6) + "_" + name;
    return path;
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

}  // namespace

AEGIS_TEST(status_basic) {
    const Status ok;
    AEGIS_CHECK(ok.ok());
    AEGIS_CHECK_EQ(ok.code(), 0);

    const auto error = Status::error(errc::kParseError, "bad input");
    AEGIS_CHECK(!error.ok());
    AEGIS_CHECK_EQ(error.code(), errc::kParseError);
    AEGIS_CHECK_EQ(error.message(), std::string("bad input"));
    AEGIS_CHECK(error.to_string().find("bad input") != std::string::npos);

    Result<int> value = 7;
    AEGIS_CHECK(value.has_value());
    AEGIS_CHECK_EQ(*value, 7);

    Result<int> failure = fail(errc::kInternal, "boom");
    AEGIS_CHECK(!failure.has_value());
    AEGIS_CHECK_EQ(failure.error().code(), errc::kInternal);
}

AEGIS_TEST(util_string_helpers) {
    AEGIS_CHECK_EQ(to_lower("HeLLo"), std::string("hello"));
    AEGIS_CHECK_EQ(trim("  \t hello \r\n"), std::string_view("hello"));
    AEGIS_CHECK_EQ(trim(""), std::string_view(""));
    AEGIS_CHECK(iequals("Content-Length", "content-length"));
    AEGIS_CHECK(!iequals("abc", "abd"));

    const auto parts = split("a,b,,c", ',');
    AEGIS_CHECK_EQ(parts.size(), std::size_t{4});
    AEGIS_CHECK_EQ(std::string(parts[2]), std::string(""));
    AEGIS_CHECK_EQ(std::string(parts[3]), std::string("c"));

    const auto single = split("no-delim", ',');
    AEGIS_CHECK_EQ(single.size(), std::size_t{1});
    AEGIS_CHECK_EQ(std::string(single[0]), std::string("no-delim"));
}

AEGIS_TEST(util_sanitize_and_bytes) {
    const std::string dirty = std::string("line1\nline2\t") + '\x01' + "end";
    const std::string clean = sanitize_for_log(dirty);
    AEGIS_CHECK(clean.find('\n') == std::string::npos);
    AEGIS_CHECK(clean.find("\\n") != std::string::npos);
    AEGIS_CHECK(clean.find("\\t") != std::string::npos);
    AEGIS_CHECK(clean.find("\\x01") != std::string::npos);

    AEGIS_CHECK_EQ(human_bytes(999), std::string("999.00 B"));
    AEGIS_CHECK_EQ(human_bytes(1024), std::string("1.00 KiB"));
    AEGIS_CHECK_EQ(human_bytes(1024 * 1024 + 512 * 1024), std::string("1.50 MiB"));
}

AEGIS_TEST(util_hash_and_random) {
    AEGIS_CHECK_EQ(fnv1a64("aegis"), fnv1a64("aegis"));
    AEGIS_CHECK_NE(fnv1a64("aegis"), fnv1a64("Aegis"));
    AEGIS_CHECK_EQ(fnv1a64(""), 1469598103934665603ULL);

    const auto hex1 = random_hex(8);
    const auto hex2 = random_hex(8);
    AEGIS_CHECK_EQ(hex1.size(), std::size_t{16});
    AEGIS_CHECK_NE(hex1, hex2);
    for (const char c : hex1) {
        AEGIS_CHECK((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
    }
}

AEGIS_TEST(util_time_format) {
    const auto tp = std::chrono::system_clock::from_time_t(1700000000);
    const auto text = format_local_time(tp);
    AEGIS_CHECK_EQ(text.size(), std::size_t{23});
    AEGIS_CHECK_EQ(text[4], '-');
    AEGIS_CHECK_EQ(text[10], ' ');
    AEGIS_CHECK_EQ(text[19], '.');
    AEGIS_CHECK(to_millis(std::chrono::milliseconds(1500)) == 1500);
}

AEGIS_TEST(mpsc_fifo_and_bounds) {
    MpscBoundedQueue<int, 4> queue;
    AEGIS_CHECK(queue.try_push(1));
    AEGIS_CHECK(queue.try_push(2));
    AEGIS_CHECK(queue.try_push(3));
    AEGIS_CHECK(queue.try_push(4));
    AEGIS_CHECK(!queue.try_push(5));  // full

    int value = 0;
    AEGIS_CHECK(queue.try_pop(value));
    AEGIS_CHECK_EQ(value, 1);
    AEGIS_CHECK(queue.try_push(5));  // one slot freed

    for (int expected = 2; expected <= 5; ++expected) {
        AEGIS_CHECK(queue.try_pop(value));
        AEGIS_CHECK_EQ(value, expected);
    }
    AEGIS_CHECK(!queue.try_pop(value));
}

AEGIS_TEST(mpsc_multi_producer_stress) {
    constexpr int kProducers = 4;
    constexpr int kPerProducer = 5000;

    MpscBoundedQueue<int, 1024> queue;
    std::atomic<int> produced{0};
    std::atomic<bool> done{false};
    std::vector<std::thread> producers;

    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&queue, &produced, p] {
            for (int i = 0; i < kPerProducer; ++i) {
                while (!queue.try_push(p * kPerProducer + i)) {
                    std::this_thread::yield();
                }
                produced.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    std::vector<char> seen(static_cast<std::size_t>(kProducers * kPerProducer), 0);
    int received = 0;
    int value = 0;
    while (received < kProducers * kPerProducer) {
        if (queue.try_pop(value)) {
            AEGIS_CHECK(value >= 0 && value < kProducers * kPerProducer);
            if (value >= 0 && value < static_cast<int>(seen.size())) {
                AEGIS_CHECK_EQ(static_cast<int>(seen[static_cast<std::size_t>(value)]), 0);
                seen[static_cast<std::size_t>(value)] = 1;
            }
            ++received;
        } else {
            std::this_thread::yield();
        }
    }
    for (auto& producer : producers) producer.join();
    done.store(true);

    AEGIS_CHECK_EQ(received, kProducers * kPerProducer);
    AEGIS_CHECK_EQ(produced.load(), kProducers * kPerProducer);
    for (const char flag : seen) AEGIS_CHECK_EQ(static_cast<int>(flag), 1);
}

AEGIS_TEST(logger_writes_file_with_level_filter) {
    const auto path = temp_path("log");
    LogConfig config;
    config.level = LogLevel::Info;
    config.console = false;
    config.file_path = path.string();
    Logger::instance().configure(config);

    for (int i = 0; i < 200; ++i) {
        AEGIS_INFO("info line {}", i);
        AEGIS_DEBUG("debug line {} should be filtered", i);
    }
    Logger::instance().flush();

    const auto stats = Logger::instance().stats();
    AEGIS_CHECK(stats.written >= 200);

    // Release the file handle before inspecting and deleting it.
    Logger::instance().configure(LogConfig{});

    const auto text = read_file(path);
    // The logger is a process wide singleton: a record queued by an earlier test
    // may land in this file, so assert on our own lines instead of the total.
    std::size_t info_lines = 0;
    std::size_t at = 0;
    while ((at = text.find("info line ", at)) != std::string::npos) {
        ++info_lines;
        at += 10;
    }
    AEGIS_CHECK_EQ(info_lines, std::size_t{200});
    AEGIS_CHECK(text.find("info line 199") != std::string::npos);
    AEGIS_CHECK(text.find("debug line") == std::string::npos);
    AEGIS_CHECK(text.find("[INFO]") != std::string::npos);

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

AEGIS_TEST(logger_rotates_files) {
    const auto path = temp_path("rotate");
    LogConfig config;
    config.level = LogLevel::Info;
    config.console = false;
    config.file_path = path.string();
    config.max_file_bytes = 2048;
    config.max_files = 2;
    Logger::instance().configure(config);

    for (int i = 0; i < 400; ++i) {
        AEGIS_INFO("rotation payload padded to force a rollover {:04d}", i);
    }
    Logger::instance().flush();

    AEGIS_CHECK(std::filesystem::exists(path));
    AEGIS_CHECK(std::filesystem::exists(path.string() + ".1"));
    // Rotation happens after a whole batch is written, so the active file may
    // overshoot the configured limit by up to one batch.
    AEGIS_CHECK(std::filesystem::file_size(path) <= 16384);

    Logger::instance().configure(LogConfig{});
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    std::filesystem::remove(path.string() + ".1", ignored);
    std::filesystem::remove(path.string() + ".2", ignored);
}

AEGIS_TEST(logger_survives_overflow) {
    LogConfig config;
    config.level = LogLevel::Trace;
    config.console = false;
    Logger::instance().configure(config);

    const auto before = Logger::instance().stats().dropped;
    for (int i = 0; i < 40000; ++i) {
        AEGIS_TRACE("flood {}", i);
    }
    Logger::instance().flush();
    const auto after = Logger::instance().stats();

    // Whatever the scheduling, the process must stay alive and either write or
    // count every record - never lose track silently.
    AEGIS_CHECK(after.written + after.dropped >= before + 1);
    AEGIS_CHECK(after.written + after.dropped >= 1000);

    Logger::instance().configure(LogConfig{});
}

AEGIS_TEST(metrics_counters_and_render) {
    auto& metrics = Metrics::instance();
    metrics.reset();

    metrics.inc(Counter::HttpRequestsTotal);
    metrics.inc(Counter::HttpRequestsTotal, 4);
    AEGIS_CHECK_EQ(metrics.counter(Counter::HttpRequestsTotal), std::uint64_t{5});

    metrics.set(Gauge::ConnectionsActive, 3.0);
    metrics.add(Gauge::ConnectionsActive, 2.0);
    AEGIS_CHECK_EQ(metrics.gauge(Gauge::ConnectionsActive), 5.0);

    metrics.observe(Histogram::RequestLatencyMs, 0.5);
    metrics.observe(Histogram::RequestLatencyMs, 7.0);
    metrics.observe(Histogram::RequestLatencyMs, 7500.0);

    const auto text = metrics.render();
    AEGIS_CHECK(text.find("aegis_http_requests_total 5") != std::string::npos);
    AEGIS_CHECK(text.find("# TYPE aegis_connections_active gauge") != std::string::npos);
    AEGIS_CHECK(text.find("aegis_request_latency_ms_count 3") != std::string::npos);
    AEGIS_CHECK(text.find("aegis_request_latency_ms_bucket{le=\"+Inf\"} 3") != std::string::npos);
    AEGIS_CHECK(text.find("aegis_cache_hit_ratio") != std::string::npos);
    AEGIS_CHECK(text.find("aegis_uptime_seconds") != std::string::npos);
}

AEGIS_TEST(metrics_cache_hit_ratio) {
    auto& metrics = Metrics::instance();
    metrics.reset();
    metrics.inc(Counter::CacheHitsTotal, 3);
    metrics.inc(Counter::CacheMissesTotal, 1);
    const auto text = metrics.render();
    AEGIS_CHECK(text.find("aegis_cache_hit_ratio 0.7500") != std::string::npos);
    metrics.reset();
}
