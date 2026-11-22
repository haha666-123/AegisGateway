#include "common/Log.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <future>
#include <mutex>
#include <semaphore>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "common/MpscQueue.h"
#include "common/Util.h"

namespace aegis {
namespace {

constexpr std::size_t kQueueCapacity = 8192;
constexpr std::size_t kMaxBatch = 256;

struct Record {
    LogLevel level{LogLevel::Info};
    std::uint32_t tid_hash{0};
    std::string message;
    std::shared_ptr<std::promise<void>> flushed;
};

using Queue = MpscBoundedQueue<Record, kQueueCapacity>;

std::uint32_t current_thread_hash() {
    const auto hash = std::hash<std::thread::id>{}(std::this_thread::get_id());
    return static_cast<std::uint32_t>(hash & 0xFFFFFFFFU);
}

}  // namespace

const char* to_string(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info: return "INFO";
        case LogLevel::Warn: return "WARN";
        case LogLevel::Error: return "ERROR";
        case LogLevel::Off: return "OFF";
    }
    return "?";
}

struct Logger::Impl {
    Queue queue;
    std::atomic<bool> stop_requested{false};
    std::atomic<std::uint64_t> dropped{0};
    std::atomic<std::uint64_t> written{0};

    std::binary_semaphore wakeup{0};
    std::thread worker;

    mutable std::mutex config_mutex;
    LogConfig config;

    std::FILE* file{nullptr};
    std::size_t file_bytes{0};
    std::string buffer;

    Impl() { worker = std::thread([this] { run(); }); }

    ~Impl() { join(); }

    void join() {
        if (!worker.joinable()) return;
        stop_requested.store(true, std::memory_order_release);
        wakeup.release();
        worker.join();
    }

    void open_file_locked() {
        if (config.file_path.empty() || file != nullptr) return;
        file = std::fopen(config.file_path.c_str(), "ab");
        if (file == nullptr) return;
        std::fseek(file, 0, SEEK_END);
        const long size = std::ftell(file);
        file_bytes = size > 0 ? static_cast<std::size_t>(size) : 0;
    }

    void rotate_if_needed_locked() {
        if (file == nullptr || config.max_file_bytes == 0) return;
        if (file_bytes < config.max_file_bytes) return;

        std::fclose(file);
        file = nullptr;

        // gateway.log.(n-1) -> gateway.log.n, gateway.log -> gateway.log.1
        for (int index = config.max_files - 1; index >= 1; --index) {
            const std::string from = config.file_path + "." + std::to_string(index);
            const std::string to = config.file_path + "." + std::to_string(index + 1);
            std::rename(from.c_str(), to.c_str());
        }
        if (config.max_files >= 1) {
            const std::string first = config.file_path + ".1";
            std::rename(config.file_path.c_str(), first.c_str());
        }
        open_file_locked();
        file_bytes = 0;
    }

    void write_locked(const std::string& text) {
        if (config.console) std::fwrite(text.data(), 1, text.size(), stdout);
        if (!config.file_path.empty()) {
            open_file_locked();
            if (file != nullptr) {
                std::fwrite(text.data(), 1, text.size(), file);
                file_bytes += text.size();
            }
        }
    }

    void flush_locked() {
        if (config.console) std::fflush(stdout);
        if (file != nullptr) std::fflush(file);
    }

    void format_record(const Record& record) {
        char tag[16];
        std::snprintf(tag, sizeof(tag), "%08x", record.tid_hash);

        buffer += format_local_time(std::chrono::system_clock::now());
        buffer += " [";
        buffer += to_string(record.level);
        buffer += "] [";
        buffer += tag;
        buffer += "] ";
        buffer += record.message;
        buffer += '\n';
    }

    void run() {
        std::vector<std::shared_ptr<std::promise<void>>> flushes;
        Record record;
        while (true) {
            const bool stopping = stop_requested.load(std::memory_order_acquire);
            if (!queue.try_pop(record)) {
                if (stopping) break;
                // Wake on producer notification, but never sleep for long so a
                // pending stop request is always observed.
                wakeup.try_acquire_for(std::chrono::milliseconds(20));
                continue;
            }

            buffer.clear();
            std::size_t lines = 0;
            std::size_t batched = 0;
            do {
                if (record.flushed) {
                    flushes.push_back(std::move(record.flushed));
                } else {
                    format_record(record);
                    ++lines;
                }
                ++batched;
            } while (batched < kMaxBatch && queue.try_pop(record));

            {
                std::lock_guard<std::mutex> lock(config_mutex);
                if (!buffer.empty()) {
                    write_locked(buffer);
                    written.fetch_add(lines, std::memory_order_relaxed);
                    rotate_if_needed_locked();
                }
                if (!flushes.empty()) {
                    flush_locked();
                    for (auto& promise : flushes) promise->set_value();
                    flushes.clear();
                }
            }
        }

        // Drain whatever is left, then release any waiter.
        buffer.clear();
        {
            std::lock_guard<std::mutex> lock(config_mutex);
            while (queue.try_pop(record)) {
                if (record.flushed) {
                    record.flushed->set_value();
                    continue;
                }
                format_record(record);
            }
            write_locked(buffer);
            flush_locked();
            buffer.clear();
            if (file != nullptr) {
                std::fclose(file);
                file = nullptr;
            }
        }
    }
};

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

Logger::Logger() : impl_(std::make_unique<Impl>()) {}

Logger::~Logger() {
    if (!impl_) return;
    impl_->join();
}

void Logger::configure(const LogConfig& config) {
    std::lock_guard<std::mutex> lock(impl_->config_mutex);
    if (impl_->file != nullptr) {
        std::fflush(impl_->file);
        std::fclose(impl_->file);
        impl_->file = nullptr;
    }
    impl_->config = config;
    impl_->file_bytes = 0;
    level_.store(static_cast<int>(config.level), std::memory_order_relaxed);
    impl_->open_file_locked();
}

LogConfig Logger::config() const {
    std::lock_guard<std::mutex> lock(impl_->config_mutex);
    return impl_->config;
}

void Logger::log(LogLevel level, std::string_view message) {
    if (!should_log(level)) return;

    Record record;
    record.level = level;
    record.tid_hash = current_thread_hash();
    record.message.assign(message);

    if (!impl_->queue.try_push(std::move(record))) {
        impl_->dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    impl_->wakeup.release();
}

void Logger::flush() {
    auto promise = std::make_shared<std::promise<void>>();
    auto future = promise->get_future();

    Record record;
    record.flushed = promise;
    bool queued = false;
    for (int attempt = 0; attempt < 200 && !queued; ++attempt) {
        if (impl_->queue.try_push(std::move(record))) {
            queued = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        record.flushed = promise;  // moved-from, restore
    }
    impl_->wakeup.release();
    if (!queued) return;

    (void)future.wait_for(std::chrono::seconds(2));
}

void Logger::shutdown() {
    flush();
    impl_->join();
}

Logger::Stats Logger::stats() const {
    Stats stats;
    stats.dropped = impl_->dropped.load(std::memory_order_relaxed);
    stats.written = impl_->written.load(std::memory_order_relaxed);
    stats.queue_depth = impl_->queue.size_approx();
    return stats;
}

}  // namespace aegis
