#include "common/RateLimiter.h"

#include <algorithm>

namespace aegis {

namespace {
constexpr std::size_t kSweepThreshold = 4096;
constexpr auto kSweepInterval = std::chrono::seconds(60);
}  // namespace

RateLimiter::RateLimiter(double rate_per_second, double burst)
    : rate_(rate_per_second > 0.0 ? rate_per_second : 1.0), burst_(burst > 0.0 ? burst : 1.0) {
    last_sweep_ = steady_now();
}

RateLimiter::Decision RateLimiter::acquire(std::string_view key) {
    std::lock_guard<std::mutex> lock(mutex_);

    const auto now = steady_now();
    const std::string owned(key);
    auto it = buckets_.find(owned);
    if (it == buckets_.end()) {
        Bucket bucket;
        bucket.tokens = burst_;
        bucket.last_refill = now;
        it = buckets_.emplace(owned, bucket).first;
    }

    Bucket& bucket = it->second;
    const double elapsed = std::chrono::duration<double>(now - bucket.last_refill).count();
    bucket.tokens = std::min(burst_, bucket.tokens + elapsed * rate_);
    bucket.last_refill = now;

    Decision decision;
    if (bucket.tokens >= 1.0) {
        bucket.tokens -= 1.0;
        decision.allowed = true;
        decision.remaining = bucket.tokens;
    } else {
        decision.allowed = false;
        decision.remaining = bucket.tokens;
        decision.retry_after_seconds = (1.0 - bucket.tokens) / rate_;
    }

    sweep_if_needed();
    return decision;
}

void RateLimiter::sweep_if_needed() {
    if (buckets_.size() < kSweepThreshold) return;
    const auto now = steady_now();
    if (now - last_sweep_ < kSweepInterval) return;
    last_sweep_ = now;

    // Drop buckets that have refilled completely and have been idle for a while:
    // they carry no state that a fresh bucket would not reproduce.
    for (auto it = buckets_.begin(); it != buckets_.end();) {
        const bool full = it->second.tokens >= burst_;
        const bool idle = std::chrono::duration_cast<std::chrono::seconds>(now - it->second.last_refill)
                              .count() > 300;
        if (full && idle) {
            it = buckets_.erase(it);
        } else {
            ++it;
        }
    }
}

void RateLimiter::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    buckets_.clear();
    last_sweep_ = steady_now();
}

std::size_t RateLimiter::bucket_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return buckets_.size();
}

}  // namespace aegis
