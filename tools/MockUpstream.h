#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

#include "common/Status.h"

namespace aegis::tools {

// A tiny OpenAI compatible upstream used by the end to end tests and by the
// `mock_upstream` binary. It answers /chat/completions with either a single JSON
// body or a token by token SSE stream, which is exactly what the gateway needs
// to exercise its forwarding, usage accounting and cache paths without calling a
// real model.
class MockUpstream {
public:
    struct Options {
        std::string host{"127.0.0.1"};
        std::uint16_t port{9001};
        int io_threads{1};
        std::chrono::milliseconds token_delay{5};
        int tokens{8};
        std::string model{"mock-1"};
        std::string reply{"Hello from the mock upstream."};
        bool fail_first_n{false};
    };

    explicit MockUpstream(Options options);
    ~MockUpstream();

    MockUpstream(const MockUpstream&) = delete;
    MockUpstream& operator=(const MockUpstream&) = delete;

    Result<void> start();
    void stop();

    [[nodiscard]] std::uint16_t port() const noexcept;
    [[nodiscard]] int requests_served() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace aegis::tools
