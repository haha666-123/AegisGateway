#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "MockUpstream.h"
#include "common/Log.h"

namespace {
std::atomic<bool> g_stop{false};
void handle_signal(int) { g_stop.store(true, std::memory_order_relaxed); }
}  // namespace

int main(int argc, char** argv) {
    aegis::tools::MockUpstream::Options options;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const auto next = [&argc, &argv, &i]() -> std::string {
            if (i + 1 >= argc) return {};
            return argv[++i];
        };
        if (arg == "--port") {
            options.port = static_cast<std::uint16_t>(std::atoi(next().c_str()));
        } else if (arg == "--tokens") {
            options.tokens = std::atoi(next().c_str());
        } else if (arg == "--delay-ms") {
            options.token_delay = std::chrono::milliseconds(std::atoi(next().c_str()));
        } else if (arg == "--model") {
            options.model = next();
        } else if (arg == "--reply") {
            options.reply = next();
        } else if (arg == "--help" || arg == "-h") {
            std::printf(
                "mock_upstream - fake OpenAI compatible endpoint\n"
                "\n"
                "usage: mock_upstream [options]\n"
                "  --port <port>        listen port (default 9001)\n"
                "  --tokens <n>         tokens emitted for streamed replies (default 8)\n"
                "  --delay-ms <n>       delay between tokens (default 5)\n"
                "  --model <name>       model name reported in responses\n"
                "  --reply <text>       non streaming reply text\n");
            return 0;
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", argv[i]);
            return 2;
        }
    }

    aegis::LogConfig log_config;
    log_config.level = aegis::LogLevel::Info;
    log_config.console = true;
    aegis::Logger::instance().configure(log_config);

    aegis::tools::MockUpstream upstream(options);
    auto started = upstream.start();
    if (!started) {
        std::fprintf(stderr, "failed to start mock upstream: %s\n",
                     started.error().message().c_str());
        return 1;
    }

    std::printf("mock upstream on http://127.0.0.1:%u/v1 (model=%s, tokens=%d, delay=%lldms)\n",
                static_cast<unsigned>(upstream.port()), options.model.c_str(), options.tokens,
                static_cast<long long>(options.token_delay.count()));
    std::fflush(stdout);

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);
    while (!g_stop.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    upstream.stop();
    aegis::Logger::instance().shutdown();
    return 0;
}
