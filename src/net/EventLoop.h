#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "common/Util.h"
#include "net/Poller.h"
#include "net/Socket.h"

namespace aegis::net {

// One event loop per I/O thread.
//
// * `run()` blocks the calling thread and dispatches readiness events, posted
//   tasks and expired timers. A loop is single threaded by construction: all
//   handlers and timers run on the loop thread, so they never need locks.
// * `post()` is the only thread safe entry point besides `stop()`; it hands a
//   closure over to the loop thread through a mutex protected swap buffer plus a
//   socket pair wakeup. A swap buffer keeps contention on foreign threads low
//   because the loop consumes a whole batch at once.
// * Timers live in an ordered map keyed by (deadline, id) - a std::multimap
//   with explicit ids is enough for the few thousand timers a gateway keeps
//   (keep-alive, upstream timeouts); a heap would save memory but loses O(log n)
//   cancellation, which we need.
class EventLoop {
public:
    using Task = std::function<void()>;
    using IoHandler = std::function<void(std::uint32_t events)>;
    using TimerId = std::uint64_t;

    EventLoop();
    ~EventLoop();

    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    void run();
    void stop();

    [[nodiscard]] bool running() const noexcept {
        return running_.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool in_loop_thread() const noexcept {
        return std::this_thread::get_id() == thread_id_;
    }
    [[nodiscard]] const char* backend() const noexcept { return poller_->backend(); }

    void post(Task task);

    TimerId run_after(std::chrono::milliseconds delay, Task task);
    TimerId run_every(std::chrono::milliseconds interval, Task task);
    bool cancel_timer(TimerId id);

    bool add(Handle handle, std::uint32_t events, IoHandler handler);
    bool update(Handle handle, std::uint32_t events);
    bool remove(Handle handle);

    [[nodiscard]] static EventLoop* current() noexcept { return current_; }

private:
    struct TimerKey {
        TimePoint when;
        TimerId id{0};

        [[nodiscard]] bool operator<(const TimerKey& other) const noexcept {
            if (when != other.when) return when < other.when;
            return id < other.id;
        }
    };

    struct TimerEntry {
        TimerId id{0};
        std::chrono::milliseconds interval{0};
        Task task;
        bool repeating{false};
    };

    void wakeup();
    void drain_wakeup();
    void run_pending();
    void run_expired_timers();
    [[nodiscard]] int compute_timeout_ms();

    std::unique_ptr<Poller> poller_;
    Handle wakeup_read_{kInvalidHandle};
    Handle wakeup_write_{kInvalidHandle};

    std::unordered_map<Handle, IoHandler> handlers_;

    std::mutex timer_mutex_;
    std::map<TimerKey, TimerEntry> timers_;
    std::unordered_map<TimerId, TimerKey> timer_index_;
    TimerId next_timer_id_{1};

    std::mutex pending_mutex_;
    std::vector<Task> pending_;
    std::vector<Task> pending_swap_;
    std::atomic<std::size_t> pending_count_{0};

    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    std::thread::id thread_id_{};

    static inline thread_local EventLoop* current_{nullptr};
};

}  // namespace aegis::net
