#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "net/Socket.h"

namespace aegis::net {

enum IoEvent : std::uint32_t {
    kReadable = 1U << 0,
    kWritable = 1U << 1,
    kError = 1U << 2,
    kHangup = 1U << 3,
};

struct ReadyEvent {
    Handle handle{kInvalidHandle};
    std::uint32_t events{0};
};

// Thin abstraction over the native readiness notification mechanism:
//   Linux   -> epoll
//   Windows -> WSAPoll
//   other   -> poll(2)
//
// Level triggered on purpose: every connection reads until EAGAIN, so edge
// triggered bookkeeping would only add complexity without a measurable win at
// this scale (and LT is what the unit tests can reason about deterministically).
class Poller {
public:
    virtual ~Poller() = default;

    virtual bool add(Handle handle, std::uint32_t events) = 0;
    virtual bool update(Handle handle, std::uint32_t events) = 0;
    virtual bool remove(Handle handle) = 0;
    [[nodiscard]] virtual int wait(std::vector<ReadyEvent>& ready, int timeout_ms) = 0;
    [[nodiscard]] virtual const char* backend() const noexcept = 0;

    [[nodiscard]] static std::unique_ptr<Poller> create();
};

}  // namespace aegis::net
