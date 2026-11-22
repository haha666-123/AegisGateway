#include "common/ThreadPool.h"

#include <algorithm>

#include "common/Log.h"

namespace aegis {

ThreadPool::ThreadPool(std::size_t workers, std::string name) : name_(std::move(name)) {
    if (workers == 0) {
        const auto hardware = std::thread::hardware_concurrency();
        workers = hardware == 0 ? 4 : std::min<std::size_t>(hardware, 8);
    }
    workers_.reserve(workers);
    for (std::size_t i = 0; i < workers; ++i) {
        workers_.emplace_back([this, i] {
            std::string thread_name = name_ + "-" + std::to_string(i);
            (void)thread_name;
            worker_loop();
        });
    }
}

ThreadPool::~ThreadPool() { shutdown(); }

void ThreadPool::submit(std::function<void()> task) {
    if (!task) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) return;  // dropping late work is better than blocking an I/O thread
        tasks_.push_back(std::move(task));
    }
    wakeup_.notify_one();
}

void ThreadPool::shutdown() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_ && workers_.empty()) return;
        stopping_ = true;
    }
    wakeup_.notify_all();
    for (auto& worker : workers_) {
        if (worker.joinable()) worker.join();
    }
    workers_.clear();

    std::deque<std::function<void()>> leftover;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        leftover.swap(tasks_);
    }
    if (!leftover.empty()) {
        AEGIS_WARN("thread pool '{}' shut down with {} unfinished task(s)", name_,
                   leftover.size());
    }
}

std::size_t ThreadPool::pending() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return tasks_.size();
}

void ThreadPool::worker_loop() {
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wakeup_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
            if (tasks_.empty()) {
                if (stopping_) return;
                continue;
            }
            task = std::move(tasks_.front());
            tasks_.pop_front();
        }
        try {
            task();
        } catch (const std::exception& ex) {
            AEGIS_ERROR("thread pool task threw: {}", ex.what());
        } catch (...) {
            AEGIS_ERROR("thread pool task threw an unknown exception");
        }
    }
}

}  // namespace aegis
