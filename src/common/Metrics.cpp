#include "common/Metrics.h"

#include <chrono>
#include <cstdio>
#include <string_view>

namespace aegis {
namespace {

using namespace std::chrono;
const system_clock::time_point g_start_time = system_clock::now();

constexpr std::array<const char*, static_cast<std::size_t>(Counter::Count)> kCounterNames{
    "aegis_http_requests_total",
    "aegis_http_responses_total",
    "aegis_http_errors_total",
    "aegis_connections_opened_total",
    "aegis_connections_closed_total",
    "aegis_upstream_requests_total",
    "aegis_upstream_failures_total",
    "aegis_cache_hits_total",
    "aegis_cache_misses_total",
    "aegis_rate_limited_total",
    "aegis_request_bytes_total",
    "aegis_response_bytes_total",
    "aegis_stream_events_total",
};

constexpr std::array<const char*, static_cast<std::size_t>(Counter::Count)> kCounterHelp{
    "HTTP requests accepted by the gateway",
    "HTTP responses written by the gateway",
    "HTTP responses with a 4xx/5xx status",
    "TCP connections accepted",
    "TCP connections closed",
    "Requests forwarded to an upstream model endpoint",
    "Upstream requests that failed or timed out",
    "Chat completions served from cache",
    "Chat completions that missed the cache",
    "Requests rejected by the token bucket",
    "Request body bytes read",
    "Response body bytes written",
    "SSE chunks forwarded to clients",
};

constexpr std::array<const char*, static_cast<std::size_t>(Gauge::Count)> kGaugeNames{
    "aegis_connections_active",
    "aegis_io_threads",
    "aegis_worker_threads",
    "aegis_uptime_seconds",
};

constexpr std::array<const char*, static_cast<std::size_t>(Gauge::Count)> kGaugeHelp{
    "HTTP connections currently open",
    "Event loop threads",
    "Thread pool workers",
    "Seconds since process start",
};

constexpr std::array<const char*, static_cast<std::size_t>(Histogram::Count)> kHistogramNames{
    "aegis_request_latency_ms",
    "aegis_upstream_latency_ms",
};

constexpr std::array<const char*, static_cast<std::size_t>(Histogram::Count)> kHistogramHelp{
    "End to end gateway latency in milliseconds",
    "Latency of upstream chat completion calls in milliseconds",
};

void append_metric_header(std::string& out, const char* name, const char* type, const char* help) {
    out += "# HELP ";
    out += name;
    out += ' ';
    out += help;
    out += "\n# TYPE ";
    out += name;
    out += ' ';
    out += type;
    out += '\n';
}

}  // namespace

const char* metric_name(Counter counter) noexcept {
    return kCounterNames[static_cast<std::size_t>(counter)];
}

const char* metric_name(Gauge gauge) noexcept { return kGaugeNames[static_cast<std::size_t>(gauge)]; }

const char* metric_name(Histogram histogram) noexcept {
    return kHistogramNames[static_cast<std::size_t>(histogram)];
}

Metrics& Metrics::instance() {
    static Metrics metrics;
    return metrics;
}

Metrics::Metrics() {
    for (auto& counter : counters_) counter.store(0, std::memory_order_relaxed);
    for (auto& gauge : gauges_) gauge.store(0.0, std::memory_order_relaxed);
    for (auto& histogram : buckets_) {
        for (auto& bucket : histogram) bucket.store(0, std::memory_order_relaxed);
    }
    for (auto& count : hist_counts_) count.store(0, std::memory_order_relaxed);
    for (auto& sum : hist_sums_) sum.store(0.0, std::memory_order_relaxed);
}

void Metrics::inc(Counter counter, std::uint64_t delta) noexcept {
    counters_[static_cast<std::size_t>(counter)].fetch_add(delta, std::memory_order_relaxed);
}

void Metrics::set(Gauge gauge, double value) noexcept {
    gauges_[static_cast<std::size_t>(gauge)].store(value, std::memory_order_relaxed);
}

void Metrics::add(Gauge gauge, double delta) noexcept {
    gauges_[static_cast<std::size_t>(gauge)].fetch_add(delta, std::memory_order_relaxed);
}

void Metrics::observe(Histogram histogram, double value_ms) noexcept {
    const auto index = static_cast<std::size_t>(histogram);
    std::size_t bucket = kBucketCount;  // +Inf overflow bucket
    for (std::size_t i = 0; i < kBucketCount; ++i) {
        if (value_ms <= kBuckets[i]) {
            bucket = i;
            break;
        }
    }
    buckets_[index][bucket].fetch_add(1, std::memory_order_relaxed);
    hist_counts_[index].fetch_add(1, std::memory_order_relaxed);
    hist_sums_[index].fetch_add(value_ms, std::memory_order_relaxed);
}

std::uint64_t Metrics::counter(Counter counter) const noexcept {
    return counters_[static_cast<std::size_t>(counter)].load(std::memory_order_relaxed);
}

double Metrics::gauge(Gauge gauge) const noexcept {
    return gauges_[static_cast<std::size_t>(gauge)].load(std::memory_order_relaxed);
}

std::string Metrics::render() const {
    std::string out;
    out.reserve(4096);

    for (std::size_t i = 0; i < kCounterCount; ++i) {
        append_metric_header(out, kCounterNames[i], "counter", kCounterHelp[i]);
        out += kCounterNames[i];
        out += ' ';
        out += std::to_string(counters_[i].load(std::memory_order_relaxed));
        out += '\n';
    }

    for (std::size_t i = 0; i < kGaugeCount; ++i) {
        append_metric_header(out, kGaugeNames[i], "gauge", kGaugeHelp[i]);
        out += kGaugeNames[i];
        out += ' ';
        double value = gauges_[i].load(std::memory_order_relaxed);
        if (static_cast<Gauge>(i) == Gauge::UptimeSeconds) {
            const auto uptime = duration_cast<milliseconds>(system_clock::now() - g_start_time);
            value = static_cast<double>(uptime.count()) / 1000.0;
        }
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.3f", value);
        out += buf;
        out += '\n';
    }

    for (std::size_t i = 0; i < kHistogramCount; ++i) {
        append_metric_header(out, kHistogramNames[i], "histogram", kHistogramHelp[i]);
        std::uint64_t cumulative = 0;
        for (std::size_t b = 0; b < kBucketCount; ++b) {
            cumulative += buckets_[i][b].load(std::memory_order_relaxed);
            char buf[128];
            std::snprintf(buf, sizeof(buf), "%s_bucket{le=\"%g\"} %llu\n", kHistogramNames[i],
                          kBuckets[b], static_cast<unsigned long long>(cumulative));
            out += buf;
        }
        cumulative += buckets_[i][kBucketCount].load(std::memory_order_relaxed);
        char buf[128];
        std::snprintf(buf, sizeof(buf), "%s_bucket{le=\"+Inf\"} %llu\n", kHistogramNames[i],
                      static_cast<unsigned long long>(cumulative));
        out += buf;

        std::snprintf(buf, sizeof(buf), "%s_sum %.3f\n%s_count %llu\n", kHistogramNames[i],
                      hist_sums_[i].load(std::memory_order_relaxed), kHistogramNames[i],
                      static_cast<unsigned long long>(
                          hist_counts_[i].load(std::memory_order_relaxed)));
        out += buf;
    }

    const auto hits = counter(Counter::CacheHitsTotal);
    const auto misses = counter(Counter::CacheMissesTotal);
    const double ratio = (hits + misses) == 0
                             ? 0.0
                             : static_cast<double>(hits) / static_cast<double>(hits + misses);

    append_metric_header(out, "aegis_cache_hit_ratio", "gauge",
                         "Cache hits divided by cache lookups");
    char buf[64];
    std::snprintf(buf, sizeof(buf), "aegis_cache_hit_ratio %.4f\n", ratio);
    out += buf;

    return out;
}

void Metrics::reset() {
    for (auto& counter : counters_) counter.store(0, std::memory_order_relaxed);
    for (auto& gauge : gauges_) gauge.store(0.0, std::memory_order_relaxed);
    for (auto& histogram : buckets_) {
        for (auto& bucket : histogram) bucket.store(0, std::memory_order_relaxed);
    }
    for (auto& count : hist_counts_) count.store(0, std::memory_order_relaxed);
    for (auto& sum : hist_sums_) sum.store(0.0, std::memory_order_relaxed);
}

}  // namespace aegis
