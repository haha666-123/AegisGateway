#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "common/Status.h"
#include "common/Util.h"

namespace aegis::ai {

struct UpstreamConfig {
    std::string name;
    std::string base_url;  // http://host:port[/prefix]
    std::string api_key;
    std::vector<std::string> models;
    int weight{1};
};

struct UpstreamTarget {
    std::string name;
    std::string base_url;
    std::string api_key;
};

// Weighted round robin over the upstreams that serve a given model, with
// per-upstream failure tracking:
//
//   * every upstream advertises a weight; the rotation is expanded into `weight`
//     slots so a 3:1 split really sends three requests to the primary for one to
//     the fallback;
//   * after `failure_threshold` consecutive failures an upstream is skipped for
//     `cooldown` (a simple circuit breaker) and becomes eligible again afterwards;
//   * `attempt` selects the n-th candidate, which is how the gateway implements
//     failover: the first attempt uses the rotation, later attempts walk the same
//     ordered list.
class Router {
public:
    struct Health {
        std::string name;
        bool healthy{true};
        int consecutive_failures{0};
        std::uint64_t picks{0};
        std::uint64_t failures{0};
    };

    Router(std::vector<UpstreamConfig> upstreams, int failure_threshold = 3,
           std::chrono::seconds cooldown = std::chrono::seconds(30));

    [[nodiscard]] Result<UpstreamTarget> pick(std::string_view model, int attempt);

    void report_success(std::string_view name);
    void report_failure(std::string_view name);

    [[nodiscard]] bool has_model(std::string_view model) const;
    [[nodiscard]] std::vector<std::string> models() const;
    [[nodiscard]] std::vector<Health> health() const;
    [[nodiscard]] std::size_t upstream_count() const;

private:
    struct State {
        UpstreamConfig config;
        int consecutive_failures{0};
        TimePoint unhealthy_until{};
        std::uint64_t picks{0};
        std::uint64_t failures{0};
    };

    [[nodiscard]] bool eligible(const State& state, TimePoint now) const noexcept;

    mutable std::mutex mutex_;
    std::vector<State> upstreams_;
    int failure_threshold_;
    std::chrono::seconds cooldown_;
    std::uint64_t rotation_{0};
};

}  // namespace aegis::ai
