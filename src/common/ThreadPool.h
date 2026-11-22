#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace aegis {

// Fixed size worker pool for blocking work that must not stall an I/O thread:
// name resolution (getaddrinfo), embedding computation for the semantic cache,
// and config reloads.
//
// The queue is a mutex protected deque guarded by a condition variable. That is
// deliberate: these tasks are coarse grained (milliseconds), so the queue is not
// hot, while a lock free structure here would only add subtle failure modes.
// The hot path (logging) uses MpscBoundedQueue instead.
class ThreadPool {
public:
    explicit ThreadPool(std::size_t workers = 0, std::string name = "aegis-worker");
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    void submit(std::function<void()> task);

    // Stops accepting work, drains the queue, then joins the workers.
    void shutdown();

    [[nodiscard]] std::size_t size() const noexcept { return workers_.size(); }
    [[nodiscard]] std::size_t pending() const;

private:
    void worker_loop();

    std::vector<std::thread> workers_;
    std::deque<std::function<void()>> tasks_;
    mutable std::mutex mutex_;
    std::condition_variable wakeup_;
    bool stopping_{false};
    std::string name_;
};

}  // namespace aegis
