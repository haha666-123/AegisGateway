#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "app/Config.h"
#include "app/Gateway.h"
#include "common/Log.h"

namespace {

std::atomic<bool> g_stop{false};

void handle_signal(int) { g_stop.store(true, std::memory_order_relaxed); }

void print_usage() {
    std::printf(
        "aegis - OpenAI compatible gateway\n"
        "\n"
        "usage: aegis [options]\n"
        "  --config <path>   configuration file (default: config/gateway.json)\n"
        "  --port <port>     override server.port\n"
        "  --threads <n>     override server.io_threads\n"
        "  --log-level <lvl> trace|debug|info|warn|error\n"
        "  --version         print version and exit\n"
        "  --help            print this help\n");
}

}  // namespace

int main(int argc, char** argv) {
    std::string config_path = "config/gateway.json";
    int port_override = 0;
    int threads_override = 0;
    std::string level_override;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const auto next = [&argc, &argv, &i]() -> std::string {
            if (i + 1 >= argc) return {};
            return argv[++i];
        };
        if (arg == "--config") {
            config_path = next();
        } else if (arg == "--port") {
            port_override = std::atoi(next().c_str());
        } else if (arg == "--threads") {
            threads_override = std::atoi(next().c_str());
        } else if (arg == "--log-level") {
            level_override = next();
        } else if (arg == "--version") {
            std::printf("aegis 1.0.0\n");
            return 0;
        } else if (arg == "--help" || arg == "-h") {
            print_usage();
            return 0;
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", argv[i]);
            print_usage();
            return 2;
        }
    }

    auto config = aegis::app::Config::load(config_path);
    if (!config) {
        std::fprintf(stderr, "failed to load config: %s\n", config.error().message().c_str());
        return 1;
    }
    if (port_override > 0) config->server.port = static_cast<std::uint16_t>(port_override);
    if (threads_override > 0) config->server.io_threads = threads_override;
    if (!level_override.empty()) {
        const std::string level = aegis::to_lower(level_override);
        if (level == "trace") config->logging.level = aegis::LogLevel::Trace;
        else if (level == "debug") config->logging.level = aegis::LogLevel::Debug;
        else if (level == "info") config->logging.level = aegis::LogLevel::Info;
        else if (level == "warn") config->logging.level = aegis::LogLevel::Warn;
        else if (level == "error") config->logging.level = aegis::LogLevel::Error;
    }

    aegis::Logger::instance().configure(config->logging);
    AEGIS_INFO("aegis 1.0.0 starting");
    AEGIS_INFO("config:\n{}", config->describe());

    aegis::app::Gateway gateway(*config);
    auto started = gateway.start();
    if (!started) {
        AEGIS_ERROR("startup failed: {}", started.error().message());
        aegis::Logger::instance().shutdown();
        return 1;
    }

    AEGIS_INFO("listening on {}:{}", config->server.host, gateway.port());
    AEGIS_INFO("try: curl -s http://127.0.0.1:{}/healthz", gateway.port());

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    while (!g_stop.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    AEGIS_INFO("shutdown requested, draining connections");
    gateway.stop();
    AEGIS_INFO("bye");
    aegis::Logger::instance().shutdown();
    return 0;
}
