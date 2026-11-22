#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <list>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "ai/Embedding.h"

namespace aegis::ai {

// Exact match cache: LRU eviction + per entry TTL, bounded capacity.
class LruCache {
public:
    struct Stats {
        std::uint64_t hits{0};
        std::uint64_t misses{0};
        std::uint64_t evictions{0};
        std::uint64_t expirations{0};
    };

    LruCache(std::size_t capacity, std::chrono::seconds ttl);

    void put(std::string key, std::string value);
    [[nodiscard]] std::optional<std::string> get(std::string_view key);

    void clear();
    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] Stats stats() const;

private:
    struct Entry {
        std::string key;
        std::string value;
        std::chrono::steady_clock::time_point expires_at;
    };
    using ItemList = std::list<Entry>;

    mutable std::mutex mutex_;
    std::size_t capacity_;
    std::chrono::seconds ttl_;
    ItemList items_;  // front == most recently used
    std::unordered_map<std::string, ItemList::iterator> index_;
    Stats stats_;
};

// Semantic cache: a flat index of prompt embeddings, queried with cosine
// similarity. Linear scan is fine for the configured sizes (a few thousand
// entries); an approximate index is listed as future work.
class SemanticCache {
public:
    struct Stats {
        std::uint64_t hits{0};
        std::uint64_t misses{0};
        std::uint64_t inserts{0};
        std::uint64_t evictions{0};
    };

    SemanticCache(const Embedder& embedder, std::size_t capacity, std::chrono::seconds ttl,
                  float threshold);

    void put(std::string_view prompt, std::string value);
    [[nodiscard]] std::optional<std::string> get(std::string_view prompt);

    void clear();
    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] Stats stats() const;
    [[nodiscard]] float threshold() const noexcept { return threshold_; }

private:
    struct Entry {
        std::vector<float> embedding;
        std::string value;
        std::chrono::steady_clock::time_point expires_at;
    };

    mutable std::mutex mutex_;
    const Embedder& embedder_;
    std::size_t capacity_;
    std::chrono::seconds ttl_;
    float threshold_;
    std::vector<Entry> entries_;
    Stats stats_;
};

}  // namespace aegis::ai
