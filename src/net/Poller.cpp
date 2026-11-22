#include "net/Poller.h"

#include <algorithm>
#include <unordered_map>

#if defined(_WIN32)
#include <winsock2.h>
#else
#include <cerrno>
#include <poll.h>
#include <unistd.h>
#endif

#if defined(__linux__)
#include <sys/epoll.h>
#endif

namespace aegis::net {
namespace {

constexpr int kMaxEventsPerWait = 256;

#if defined(__linux__)
std::uint32_t to_native(std::uint32_t events) {
    std::uint32_t native = 0;
    if ((events & kReadable) != 0) native |= EPOLLIN;
    if ((events & kWritable) != 0) native |= EPOLLOUT;
    native |= EPOLLRDHUP;
    return native;
}

std::uint32_t from_native(std::uint32_t events) {
    std::uint32_t out = 0;
    if ((events & (EPOLLIN | EPOLLRDHUP | EPOLLHUP)) != 0) out |= kReadable;
    if ((events & EPOLLOUT) != 0) out |= kWritable;
    if ((events & EPOLLERR) != 0) out |= kError;
    if ((events & EPOLLHUP) != 0) out |= kHangup;
    return out;
}

class EpollPoller final : public Poller {
public:
    EpollPoller() : fd_(::epoll_create1(EPOLL_CLOEXEC)) {}
    ~EpollPoller() override {
        if (fd_ >= 0) ::close(fd_);
    }

    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }

    bool add(Handle handle, std::uint32_t events) override {
        epoll_event event{};
        event.events = to_native(events);
        event.data.fd = static_cast<int>(handle);
        return ::epoll_ctl(fd_, EPOLL_CTL_ADD, static_cast<int>(handle), &event) == 0;
    }

    bool update(Handle handle, std::uint32_t events) override {
        epoll_event event{};
        event.events = to_native(events);
        event.data.fd = static_cast<int>(handle);
        return ::epoll_ctl(fd_, EPOLL_CTL_MOD, static_cast<int>(handle), &event) == 0;
    }

    bool remove(Handle handle) override {
        return ::epoll_ctl(fd_, EPOLL_CTL_DEL, static_cast<int>(handle), nullptr) == 0;
    }

    int wait(std::vector<ReadyEvent>& ready, int timeout_ms) override {
        epoll_event events[kMaxEventsPerWait];
        const int count = ::epoll_wait(fd_, events, kMaxEventsPerWait, timeout_ms);
        if (count <= 0) {
            ready.clear();
            return count;
        }
        ready.clear();
        ready.reserve(static_cast<std::size_t>(count));
        for (int i = 0; i < count; ++i) {
            const std::uint32_t translated = from_native(events[i].events);
            ready.push_back(ReadyEvent{static_cast<Handle>(events[i].data.fd), translated});
        }
        return count;
    }

    [[nodiscard]] const char* backend() const noexcept override { return "epoll"; }

private:
    int fd_{-1};
};
#endif  // __linux__

#if !defined(_WIN32)
std::uint32_t to_native_poll(std::uint32_t events) {
    std::uint32_t native = 0;
    if ((events & kReadable) != 0) native |= POLLIN;
    if ((events & kWritable) != 0) native |= POLLOUT;
    return native;
}

std::uint32_t from_native_poll(short revents) {
    std::uint32_t out = 0;
    if ((revents & (POLLIN | POLLHUP)) != 0) out |= kReadable;
    if ((revents & POLLOUT) != 0) out |= kWritable;
    if ((revents & POLLERR) != 0) out |= kError;
    if ((revents & POLLHUP) != 0) out |= kHangup;
    if ((revents & POLLNVAL) != 0) out |= kError;
    return out;
}

// Portable fallback: used on macOS/BSD and as a safety net if epoll_create1
// fails on Linux.
class PollPoller final : public Poller {
public:
    bool add(Handle handle, std::uint32_t events) override {
        if (index_.contains(handle)) return false;
        pollfd entry{};
        entry.fd = static_cast<int>(handle);
        entry.events = static_cast<short>(to_native_poll(events));
        fds_.push_back(entry);
        index_[handle] = fds_.size() - 1;
        return true;
    }

    bool update(Handle handle, std::uint32_t events) override {
        const auto it = index_.find(handle);
        if (it == index_.end()) return false;
        fds_[it->second].events = static_cast<short>(to_native_poll(events));
        return true;
    }

    bool remove(Handle handle) override {
        const auto it = index_.find(handle);
        if (it == index_.end()) return false;
        const std::size_t position = it->second;
        if (position + 1 != fds_.size()) {
            fds_[position] = fds_.back();
            index_[fds_[position].fd] = position;
        }
        fds_.pop_back();
        index_.erase(it);
        return true;
    }

    int wait(std::vector<ReadyEvent>& ready, int timeout_ms) override {
        const int count = ::poll(fds_.data(), static_cast<nfds_t>(fds_.size()), timeout_ms);
        if (count <= 0) {
            ready.clear();
            return count;
        }
        ready.clear();
        for (const auto& entry : fds_) {
            if (entry.revents == 0) continue;
            ready.push_back(
                ReadyEvent{static_cast<Handle>(entry.fd), from_native_poll(entry.revents)});
        }
        return static_cast<int>(ready.size());
    }

    [[nodiscard]] const char* backend() const noexcept override { return "poll"; }

private:
    std::vector<pollfd> fds_;
    std::unordered_map<Handle, std::size_t> index_;
};
#endif  // !_WIN32

#if defined(_WIN32)
std::uint32_t to_native_wsapoll(std::uint32_t events) {
    std::uint32_t native = 0;
    if ((events & kReadable) != 0) native |= POLLRDNORM;
    if ((events & kWritable) != 0) native |= POLLWRNORM;
    return native;
}

std::uint32_t from_native_wsapoll(short revents) {
    std::uint32_t out = 0;
    if ((revents & (POLLRDNORM | POLLIN | POLLHUP)) != 0) out |= kReadable;
    if ((revents & (POLLWRNORM | POLLOUT)) != 0) out |= kWritable;
    if ((revents & POLLERR) != 0) out |= kError;
    if ((revents & POLLHUP) != 0) out |= kHangup;
    if ((revents & POLLNVAL) != 0) out |= kError;
    return out;
}

class WsapollPoller final : public Poller {
public:
    bool add(Handle handle, std::uint32_t events) override {
        if (index_.contains(handle)) return false;
        WSAPOLLFD entry{};
        entry.fd = static_cast<SOCKET>(handle);
        entry.events = static_cast<short>(to_native_wsapoll(events));
        fds_.push_back(entry);
        index_[handle] = fds_.size() - 1;
        return true;
    }

    bool update(Handle handle, std::uint32_t events) override {
        const auto it = index_.find(handle);
        if (it == index_.end()) return false;
        fds_[it->second].events = static_cast<short>(to_native_wsapoll(events));
        return true;
    }

    bool remove(Handle handle) override {
        const auto it = index_.find(handle);
        if (it == index_.end()) return false;
        const std::size_t position = it->second;
        if (position + 1 != fds_.size()) {
            fds_[position] = fds_.back();
            index_[fds_[position].fd] = position;
        }
        fds_.pop_back();
        index_.erase(it);
        return true;
    }

    int wait(std::vector<ReadyEvent>& ready, int timeout_ms) override {
        const int count =
            ::WSAPoll(fds_.data(), static_cast<ULONG>(fds_.size()), timeout_ms);
        if (count <= 0) {
            ready.clear();
            return count;
        }
        ready.clear();
        for (const auto& entry : fds_) {
            if (entry.revents == 0) continue;
            ready.push_back(ReadyEvent{static_cast<Handle>(entry.fd),
                                       from_native_wsapoll(entry.revents)});
        }
        return static_cast<int>(ready.size());
    }

    [[nodiscard]] const char* backend() const noexcept override { return "wsapoll"; }

private:
    std::vector<WSAPOLLFD> fds_;
    std::unordered_map<Handle, std::size_t> index_;
};
#endif  // _WIN32

}  // namespace

std::unique_ptr<Poller> Poller::create() {
#if defined(__linux__)
    auto epoll_poller = std::make_unique<EpollPoller>();
    if (epoll_poller->valid()) return epoll_poller;
#endif
#if defined(_WIN32)
    return std::make_unique<WsapollPoller>();
#else
    return std::make_unique<PollPoller>();
#endif
}

}  // namespace aegis::net
