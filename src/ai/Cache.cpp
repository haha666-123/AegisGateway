#include "ai/Cache.h"

#include <algorithm>
#include <utility>

namespace aegis::ai {

LruCache::LruCache(std::size_t capacity, std::chrono::seconds ttl)
    : capacity_(std::max<std::size_t>(capacity, 1)), ttl_(ttl) {}

void LruCache::put(std::string key, std::string value) {
    std::lock_guard<std::mutex> lock(mutex_);

    const auto existing = index_.find(key);
    if (existing != index_.end()) {
        existing->second->value = std::move(value);
        existing->second->expires_at = std::chrono::steady_clock::now() + ttl_;
        items_.splice(items_.begin(), items_, existing->second);
        return;
    }

    while (items_.size() >= capacity_) {
        index_.erase(items_.back().key);
        items_.pop_back();
        ++stats_.evictions;
    }

    items_.push_front(Entry{std::move(key), std::move(value),
                            std::chrono::steady_clock::now() + ttl_});
    index_[items_.front().key] = items_.begin();
}

std::optional<std::string> LruCache::get(std::string_view key) {
    std::lock_guard<std::mutex> lock(mutex_);

    const auto it = index_.find(std::string(key));
    if (it == index_.end()) {
        ++stats_.misses;
        return std::nullopt;
    }
    if (it->second->expires_at <= std::chrono::steady_clock::now()) {
        items_.erase(it->second);
        index_.erase(it);
        ++stats_.expirations;
        ++stats_.misses;
        return std::nullopt;
    }

    items_.splice(items_.begin(), items_, it->second);
    ++stats_.hits;
    return it->second->value;
}

void LruCache::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    items_.clear();
    index_.clear();
}

std::size_t LruCache::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return items_.size();
}

LruCache::Stats LruCache::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

SemanticCache::SemanticCache(const Embedder& embedder, std::size_t capacity,
                             std::chrono::seconds ttl, float threshold)
    : embedder_(embedder),
      capacity_(std::max<std::size_t>(capacity, 1)),
      ttl_(ttl),
      threshold_(std::clamp(threshold, 0.0F, 1.0F)) {}

void SemanticCache::put(std::string_view prompt, std::string value) {
    auto embedding = embedder_.embed(prompt);

    std::lock_guard<std::mutex> lock(mutex_);
    if (entries_.size() >= capacity_) {
        entries_.erase(entries_.begin());  // FIFO eviction
        ++stats_.evictions;
    }
    entries_.push_back(Entry{std::move(embedding), std::move(value),
                             std::chrono::steady_clock::now() + ttl_});
    ++stats_.inserts;
}

std::optional<std::string> SemanticCache::get(std::string_view prompt) {
    const auto query = embedder_.embed(prompt);

    std::lock_guard<std::mutex> lock(mutex_);
    const auto now = std::chrono::steady_clock::now();

    float best_score = -1.0F;
    std::size_t best_index = entries_.size();
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        if (entries_[i].expires_at <= now) continue;
        const float score = cosine_similarity(query, entries_[i].embedding);
        if (score > best_score) {
            best_score = score;
            best_index = i;
        }
    }

    if (best_index == entries_.size() || best_score < threshold_) {
        ++stats_.misses;
        return std::nullopt;
    }

    ++stats_.hits;
    // Move the hit to the back so FIFO eviction behaves like a simple LRU.
    if (best_index + 1 != entries_.size()) {
        Entry hit = std::move(entries_[best_index]);
        entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(best_index));
        entries_.push_back(std::move(hit));
    }
    return entries_.back().value;
}

void SemanticCache::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();
}

std::size_t SemanticCache::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

SemanticCache::Stats SemanticCache::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

}  // namespace aegis::ai
