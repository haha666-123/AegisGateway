#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

#include "common/Util.h"

namespace aegis {

// Per key token bucket.
//
// Each API key gets a bucket of `burst` tokens that refills at `rate` tokens per
// second. A request costs one token; when the bucket is empty the caller gets a
// retry-after hint computed from the deficit, which maps directly onto the HTTP
// 429 + Retry-After response.
class RateLimiter {
public:
    struct Decision {
        bool allowed{false};
        double retry_after_seconds{0.0};
        double remaining{0.0};
    };

    RateLimiter(double rate_per_second, double burst);

    [[nodiscard]] Decision acquire(std::string_view key);

    void reset();
    [[nodiscard]] std::size_t bucket_count() const;

private:
    struct Bucket {
        double tokens{0.0};
        TimePoint last_refill{};
    };

    void sweep_if_needed();

    mutable std::mutex mutex_;
    double rate_;
    double burst_;
    std::unordered_map<std::string, Bucket> buckets_;
    TimePoint last_sweep_{};
};

}  // namespace aegis
