#include "net/EventLoop.h"

#include <algorithm>
#include <cstddef>

#include "common/Log.h"

namespace aegis::net {
namespace {

constexpr int kMaxIdleWaitMs = 200;
constexpr std::size_t kReadBufferSize = 512;

}  // namespace

EventLoop::EventLoop() : poller_(Poller::create()) {
    ensure_socket_layer();
    auto pair = create_socket_pair(&wakeup_read_, &wakeup_write_);
    if (!pair) {
        AEGIS_ERROR("event loop: failed to create wakeup pair: {}", pair.error().message());
        return;
    }
    add(wakeup_read_, kReadable, [this](std::uint32_t) { drain_wakeup(); });
}

EventLoop::~EventLoop() {
    if (running()) {
        AEGIS_WARN("event loop destroyed while still running");
        stop();
    }
    if (wakeup_read_ != kInvalidHandle) {
        poller_->remove(wakeup_read_);
        close_socket(wakeup_read_);
        wakeup_read_ = kInvalidHandle;
    }
    if (wakeup_write_ != kInvalidHandle) {
        close_socket(wakeup_write_);
        wakeup_write_ = kInvalidHandle;
    }
}

void EventLoop::run() {
    thread_id_ = std::this_thread::get_id();
    current_ = this;
    running_.store(true, std::memory_order_release);

    std::vector<ReadyEvent> ready;
    while (!stop_requested_.load(std::memory_order_acquire)) {
        ready.clear();
        const int timeout = compute_timeout_ms();
        const int count = poller_->wait(ready, timeout);

        for (const auto& event : ready) {
            const auto it = handlers_.find(event.handle);
            if (it == handlers_.end()) continue;
            // Copy the handler: it may deregister itself (or close the socket)
            // while it runs.
            IoHandler handler = it->second;
            handler(event.events);
        }

        run_pending();
        run_expired_timers();

        if (count < 0) {
            // Interrupted (EINTR) or a transient poller error; avoid a busy
            // loop when the error repeats.
            const int error = last_socket_error();
            if (!is_would_block(error)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    }

    running_.store(false, std::memory_order_release);
    stop_requested_.store(false, std::memory_order_release);
    current_ = nullptr;
}

void EventLoop::stop() {
    stop_requested_.store(true, std::memory_order_release);
    wakeup();
}

void EventLoop::post(Task task) {
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_.push_back(std::move(task));
        pending_count_.store(pending_.size(), std::memory_order_relaxed);
    }
    wakeup();
}

void EventLoop::wakeup() {
    if (wakeup_write_ == kInvalidHandle) return;
    const char byte = 1;
    const auto outcome = write_some(wakeup_write_, &byte, 1);
    (void)outcome;  // EAGAIN means a wakeup is already pending
}

void EventLoop::drain_wakeup() {
    char scratch[kReadBufferSize];
    for (;;) {
        const auto outcome = read_some(wakeup_read_, scratch, sizeof(scratch));
        if (outcome.ok()) continue;
        break;  // WouldBlock or Closed
    }
}

void EventLoop::run_pending() {
    if (pending_count_.load(std::memory_order_relaxed) == 0) return;
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_swap_.swap(pending_);
        pending_count_.store(0, std::memory_order_relaxed);
    }
    for (auto& task : pending_swap_) {
        if (task) task();
    }
    pending_swap_.clear();
}

int EventLoop::compute_timeout_ms() {
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        if (!pending_.empty()) return 0;
    }

    std::lock_guard<std::mutex> lock(timer_mutex_);
    if (timers_.empty()) return kMaxIdleWaitMs;

    const auto now = steady_now();
    const auto deadline = timers_.begin()->first.when;
    if (deadline <= now) return 0;

    const auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
    return static_cast<int>(std::clamp<long long>(wait, 0, kMaxIdleWaitMs));
}

EventLoop::TimerId EventLoop::run_after(std::chrono::milliseconds delay, Task task) {
    std::lock_guard<std::mutex> lock(timer_mutex_);
    const TimerId id = next_timer_id_++;
    const TimePoint when = steady_now() + delay;
    timers_.emplace(TimerKey{when, id}, TimerEntry{id, delay, std::move(task), false});
    timer_index_[id] = TimerKey{when, id};
    wakeup();
    return id;
}

EventLoop::TimerId EventLoop::run_every(std::chrono::milliseconds interval, Task task) {
    if (interval.count() <= 0) interval = std::chrono::milliseconds(1);
    std::lock_guard<std::mutex> lock(timer_mutex_);
    const TimerId id = next_timer_id_++;
    const TimePoint when = steady_now() + interval;
    timers_.emplace(TimerKey{when, id}, TimerEntry{id, interval, std::move(task), true});
    timer_index_[id] = TimerKey{when, id};
    wakeup();
    return id;
}

bool EventLoop::cancel_timer(TimerId id) {
    std::lock_guard<std::mutex> lock(timer_mutex_);
    const auto it = timer_index_.find(id);
    if (it == timer_index_.end()) return false;
    timers_.erase(it->second);
    timer_index_.erase(it);
    return true;
}

void EventLoop::run_expired_timers() {
    std::vector<TimerEntry> due;
    {
        std::lock_guard<std::mutex> lock(timer_mutex_);
        const auto now = steady_now();
        while (!timers_.empty()) {
            auto begin = timers_.begin();
            if (begin->first.when > now) break;

            TimerEntry entry = std::move(begin->second);
            timer_index_.erase(begin->first.id);
            timers_.erase(begin);

            if (entry.repeating && !stop_requested_.load(std::memory_order_acquire)) {
                const TimePoint next = now + entry.interval;
                const TimerKey key{next, entry.id};
                TimerEntry copy{entry.id, entry.interval, entry.task, true};
                timers_.emplace(key, std::move(copy));
                timer_index_[entry.id] = key;
            }
            due.push_back(std::move(entry));
        }
    }

    for (auto& entry : due) {
        if (entry.task) entry.task();
    }
}

bool EventLoop::add(Handle handle, std::uint32_t events, IoHandler handler) {
    if (!poller_->add(handle, events)) return false;
    handlers_[handle] = std::move(handler);
    return true;
}

bool EventLoop::update(Handle handle, std::uint32_t events) {
    if (!handlers_.contains(handle)) return false;
    return poller_->update(handle, events);
}

bool EventLoop::remove(Handle handle) {
    handlers_.erase(handle);
    return poller_->remove(handle);
}

}  // namespace aegis::net
