#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "ai/Router.h"
#include "common/Log.h"
#include "common/Status.h"
#include "common/Json.h"

namespace aegis::app {

struct ServerConfig {
    std::string host{"0.0.0.0"};
    std::uint16_t port{8080};
    int io_threads{4};
    std::size_t max_connections{20000};
    std::chrono::milliseconds idle_timeout{65000};
};

struct LimitsConfig {
    std::size_t max_header_bytes{16U * 1024U};
    std::size_t max_body_bytes{4U * 1024U * 1024U};
};

struct RateLimitConfig {
    bool enabled{true};
    double requests_per_second{20.0};
    double burst{40.0};
};

struct CacheConfig {
    bool enabled{true};
    std::size_t capacity{4096};
    std::chrono::seconds ttl{300};
    bool semantic{false};
    float threshold{0.92F};
};

struct TimeoutsConfig {
    std::chrono::milliseconds connect{3000};
    std::chrono::milliseconds upstream{30000};
};

struct Config {
    ServerConfig server;
    LogConfig logging;
    LimitsConfig limits;
    std::vector<ai::UpstreamConfig> upstreams;
    RateLimitConfig rate_limit;
    CacheConfig cache;
    TimeoutsConfig timeouts;
    std::vector<std::string> gateway_keys;

    static Result<Config> load(const std::string& path);
    static Result<Config> from_json(const Json& document);

    [[nodiscard]] std::string describe() const;
};

}  // namespace aegis::app
