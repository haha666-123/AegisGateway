#pragma once

#include <atomic>
#include <cstdint>
#include <format>
#include <memory>
#include <string>
#include <string_view>

namespace aegis {

enum class LogLevel : int { Trace = 0, Debug = 1, Info = 2, Warn = 3, Error = 4, Off = 5 };

[[nodiscard]] const char* to_string(LogLevel level) noexcept;

struct LogConfig {
    LogLevel level{LogLevel::Info};
    bool console{true};
    std::string file_path;  // empty -> file output disabled
    std::size_t max_file_bytes{32U * 1024U * 1024U};
    int max_files{4};
};

// Asynchronous logger.
//
// Producers (any thread) format the message and push it into a bounded MPSC
// ring; a dedicated consumer thread owns the output streams and drains the ring
// in batches. When the ring is full the record is dropped and counted instead
// of blocking the caller - dropping a debug line must never stall an I/O
// thread.
class Logger {
public:
    static Logger& instance();

    void configure(const LogConfig& config);
    [[nodiscard]] LogConfig config() const;

    [[nodiscard]] bool should_log(LogLevel level) const noexcept {
        return static_cast<int>(level) >= level_.load(std::memory_order_relaxed);
    }

    void log(LogLevel level, std::string_view message);

    // Blocks until every record submitted before the call has been written.
    void flush();

    // Stops the consumer thread after draining the ring.
    void shutdown();

    struct Stats {
        std::uint64_t dropped{0};
        std::uint64_t written{0};
        std::uint64_t queue_depth{0};
    };
    [[nodiscard]] Stats stats() const;

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

private:
    Logger();
    ~Logger();

    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::atomic<int> level_{static_cast<int>(LogLevel::Info)};
};

template <class... Args>
void log_format(LogLevel level, std::format_string<Args...> fmt, Args&&... args) {
    Logger& logger = Logger::instance();
    if (!logger.should_log(level)) return;
    logger.log(level, std::vformat(fmt.get(), std::make_format_args(args...)));
}

}  // namespace aegis

#define AEGIS_LOG(level, ...) \
    ::aegis::log_format((level), __VA_ARGS__)

#define AEGIS_TRACE(...) AEGIS_LOG(::aegis::LogLevel::Trace, __VA_ARGS__)
#define AEGIS_DEBUG(...) AEGIS_LOG(::aegis::LogLevel::Debug, __VA_ARGS__)
#define AEGIS_INFO(...) AEGIS_LOG(::aegis::LogLevel::Info, __VA_ARGS__)
#define AEGIS_WARN(...) AEGIS_LOG(::aegis::LogLevel::Warn, __VA_ARGS__)
#define AEGIS_ERROR(...) AEGIS_LOG(::aegis::LogLevel::Error, __VA_ARGS__)
