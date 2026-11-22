#include "app/Config.h"

#include <cstdlib>
#include <fstream>
#include <sstream>

#include "common/Util.h"

namespace aegis::app {
namespace {

std::string get_env(const std::string& name) {
    if (name.empty()) return {};
    const char* value = std::getenv(name.c_str());
    return value == nullptr ? std::string{} : std::string(value);
}

std::chrono::milliseconds read_millis(const Json& object, std::string_view key,
                                      std::chrono::milliseconds fallback) {
    const Json& value = object[key];
    if (!value.is_number()) return fallback;
    return std::chrono::milliseconds(value.as_int64(fallback.count()));
}

}  // namespace

Result<Config> Config::from_json(const Json& document) {
    if (!document.is_object()) {
        return fail(errc::kConfigError, "config root must be a JSON object");
    }

    Config config;

    const Json& server = document["server"];
    if (server.is_object()) {
        config.server.host = server["host"].is_string() ? std::string(server["host"].as_string())
                                                        : config.server.host;
        config.server.port = static_cast<std::uint16_t>(server["port"].as_int(config.server.port));
        config.server.io_threads = server["io_threads"].as_int(config.server.io_threads);
        config.server.max_connections = static_cast<std::size_t>(
            server["max_connections"].as_int64(static_cast<int64_t>(config.server.max_connections)));
        config.server.idle_timeout =
            read_millis(server, "idle_timeout_ms", config.server.idle_timeout);
    }

    const Json& logging = document["logging"];
    if (logging.is_object()) {
        const std::string level = to_lower(logging["level"].as_string("info"));
        if (level == "trace") {
            config.logging.level = LogLevel::Trace;
        } else if (level == "debug") {
            config.logging.level = LogLevel::Debug;
        } else if (level == "warn") {
            config.logging.level = LogLevel::Warn;
        } else if (level == "error") {
            config.logging.level = LogLevel::Error;
        } else {
            config.logging.level = LogLevel::Info;
        }
        config.logging.console = logging["console"].as_bool(true);
        config.logging.file_path = logging["file"].as_string("");
        config.logging.max_file_bytes = static_cast<std::size_t>(
            logging["max_file_bytes"].as_int64(32LL * 1024 * 1024));
        config.logging.max_files = logging["max_files"].as_int(4);
    }

    const Json& limits = document["limits"];
    if (limits.is_object()) {
        config.limits.max_header_bytes = static_cast<std::size_t>(
            limits["max_header_bytes"].as_int64(static_cast<int64_t>(config.limits.max_header_bytes)));
        config.limits.max_body_bytes = static_cast<std::size_t>(
            limits["max_body_bytes"].as_int64(static_cast<int64_t>(config.limits.max_body_bytes)));
    }

    const Json& upstreams = document["upstreams"];
    if (upstreams.is_array()) {
        for (std::size_t i = 0; i < upstreams.size(); ++i) {
            const Json& item = upstreams[i];
            if (!item.is_object()) continue;

            ai::UpstreamConfig upstream;
            upstream.name = item["name"].as_string("upstream-" + std::to_string(i));
            upstream.base_url = item["base_url"].as_string("http://127.0.0.1:9001");
            upstream.weight = item["weight"].as_int(1);

            if (item["api_key"].is_string()) {
                upstream.api_key = item["api_key"].as_string("");
            } else if (item["api_key_env"].is_string()) {
                upstream.api_key = get_env(std::string(item["api_key_env"].as_string("")));
            }

            const Json& models = item["models"];
            if (models.is_array()) {
                for (std::size_t m = 0; m < models.size(); ++m) {
                    if (models[m].is_string()) {
                        upstream.models.emplace_back(models[m].as_string());
                    }
                }
            }
            config.upstreams.push_back(std::move(upstream));
        }
    }
    if (config.upstreams.empty()) {
        return fail(errc::kConfigError, "config requires at least one upstream");
    }

    const Json& rate_limit = document["rate_limit"];
    if (rate_limit.is_object()) {
        config.rate_limit.enabled = rate_limit["enabled"].as_bool(true);
        config.rate_limit.requests_per_second =
            rate_limit["requests_per_second"].as_double(config.rate_limit.requests_per_second);
        config.rate_limit.burst = rate_limit["burst"].as_double(config.rate_limit.burst);
    }

    const Json& cache = document["cache"];
    if (cache.is_object()) {
        config.cache.enabled = cache["enabled"].as_bool(true);
        config.cache.capacity =
            static_cast<std::size_t>(cache["capacity"].as_int64(static_cast<int64_t>(config.cache.capacity)));
        config.cache.ttl = std::chrono::seconds(cache["ttl_seconds"].as_int64(300));
        const Json& semantic = cache["semantic"];
        if (semantic.is_object()) {
            config.cache.semantic = semantic["enabled"].as_bool(false);
            config.cache.threshold = static_cast<float>(semantic["threshold"].as_double(0.92));
        }
    }

    const Json& timeouts = document["timeouts"];
    if (timeouts.is_object()) {
        config.timeouts.connect = read_millis(timeouts, "connect_ms", config.timeouts.connect);
        config.timeouts.upstream = read_millis(timeouts, "upstream_ms", config.timeouts.upstream);
    }

    const Json& keys = document["gateway_keys"];
    if (keys.is_array()) {
        for (std::size_t i = 0; i < keys.size(); ++i) {
            if (keys[i].is_string()) config.gateway_keys.emplace_back(keys[i].as_string());
        }
    }

    if (config.server.io_threads <= 0) config.server.io_threads = 1;
    if (config.server.port == 0) config.server.port = 8080;
    return config;
}

Result<Config> Config::load(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return fail(errc::kConfigError, "cannot open config file: " + path);
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    const std::string text = buffer.str();

    auto document = parse_json(text);
    if (!document) {
        return fail(errc::kConfigError,
                    "invalid JSON in " + path + ": " + document.error().message());
    }
    return from_json(*document);
}

std::string Config::describe() const {
    std::ostringstream out;
    out << "listen=" << server.host << ':' << server.port << " io_threads=" << server.io_threads
        << " upstreams=" << upstreams.size() << " rate_limit=" << (rate_limit.enabled ? "on" : "off")
        << " cache=" << (cache.enabled ? "on" : "off")
        << " semantic_cache=" << (cache.semantic ? "on" : "off");
    for (const auto& upstream : upstreams) {
        out << "\n  - " << upstream.name << " -> " << upstream.base_url << " models=[";
        for (std::size_t i = 0; i < upstream.models.size(); ++i) {
            if (i > 0) out << ',';
            out << upstream.models[i];
        }
        out << "] weight=" << upstream.weight;
    }
    return out.str();
}

}  // namespace aegis::app
