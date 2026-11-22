#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace aegis {

// Low overhead process metrics, rendered in the Prometheus text exposition
// format on GET /metrics.
//
// The set of series is fixed at compile time (enums instead of string keys) so
// hot paths only touch one atomic each - no map lookups, no allocations, no
// locks. Adding a series means adding an enumerator plus its name mapping.
enum class Counter : std::size_t {
    HttpRequestsTotal = 0,
    HttpResponsesTotal,
    HttpErrorsTotal,
    ConnectionsOpenedTotal,
    ConnectionsClosedTotal,
    UpstreamRequestsTotal,
    UpstreamFailuresTotal,
    CacheHitsTotal,
    CacheMissesTotal,
    RateLimitedTotal,
    RequestBytesTotal,
    ResponseBytesTotal,
    StreamEventsTotal,
    Count
};

enum class Gauge : std::size_t {
    ConnectionsActive = 0,
    ThreadsIo,
    ThreadsWorker,
    UptimeSeconds,
    Count
};

enum class Histogram : std::size_t {
    RequestLatencyMs = 0,
    UpstreamLatencyMs,
    Count
};

class Metrics {
public:
    static Metrics& instance();

    void inc(Counter counter, std::uint64_t delta = 1) noexcept;
    void set(Gauge gauge, double value) noexcept;
    void add(Gauge gauge, double delta) noexcept;
    void observe(Histogram histogram, double value_ms) noexcept;

    [[nodiscard]] std::uint64_t counter(Counter counter) const noexcept;
    [[nodiscard]] double gauge(Gauge gauge) const noexcept;

    // Prometheus text format; also includes process uptime and cache hit rate.
    [[nodiscard]] std::string render() const;

    void reset();

    Metrics(const Metrics&) = delete;
    Metrics& operator=(const Metrics&) = delete;

private:
    Metrics();

    static constexpr std::size_t kCounterCount = static_cast<std::size_t>(Counter::Count);
    static constexpr std::size_t kGaugeCount = static_cast<std::size_t>(Gauge::Count);
    static constexpr std::size_t kHistogramCount = static_cast<std::size_t>(Histogram::Count);
    static constexpr std::size_t kBucketCount = 12;

    static constexpr std::array<double, kBucketCount> kBuckets{
        1.0, 2.0, 5.0, 10.0, 25.0, 50.0, 100.0, 250.0, 500.0, 1000.0, 2500.0, 5000.0};

    std::array<std::atomic<std::uint64_t>, kCounterCount> counters_{};
    std::array<std::atomic<double>, kGaugeCount> gauges_{};
    std::array<std::array<std::atomic<std::uint64_t>, kBucketCount + 1>, kHistogramCount> buckets_{};
    std::array<std::atomic<std::uint64_t>, kHistogramCount> hist_counts_{};
    std::array<std::atomic<double>, kHistogramCount> hist_sums_{};
};

[[nodiscard]] const char* metric_name(Counter counter) noexcept;
[[nodiscard]] const char* metric_name(Gauge gauge) noexcept;
[[nodiscard]] const char* metric_name(Histogram histogram) noexcept;

}  // namespace aegis
