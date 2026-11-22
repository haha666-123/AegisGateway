#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "common/Status.h"
#include "net/Buffer.h"
#include "net/EventLoop.h"
#include "net/Socket.h"

namespace aegis::net {

class TcpServer;

// A single accepted TCP connection. All methods must be called on the owning
// event loop thread unless stated otherwise, which means handlers never need a
// lock around connection state.
class Connection : public std::enable_shared_from_this<Connection> {
public:
    using Ptr = std::shared_ptr<Connection>;

    ~Connection();

    [[nodiscard]] std::uint64_t id() const noexcept { return id_; }
    [[nodiscard]] const std::string& peer() const noexcept { return peer_; }
    [[nodiscard]] EventLoop& loop() noexcept { return *loop_; }
    [[nodiscard]] bool alive() const noexcept { return alive_; }
    [[nodiscard]] std::size_t pending_output() const noexcept { return output_.readable(); }
    [[nodiscard]] std::uint64_t bytes_in() const noexcept { return bytes_in_; }
    [[nodiscard]] std::uint64_t bytes_out() const noexcept { return bytes_out_; }

    // Queues data and tries to flush immediately. Never blocks: if the socket is
    // busy the remainder is written when the poller reports writability.
    void send(std::string_view data);
    void send(std::string_view first, std::string_view second);

    // Flushes pending output before closing (graceful shutdown of a response).
    void close();
    // Drops pending output and closes immediately.
    void close_now();

    // Flow control: handlers pause reading when the peer is slower than the
    // upstream producer, which propagates back pressure instead of buffering
    // without bound.
    void pause_reading();
    void resume_reading();
    [[nodiscard]] bool reading_paused() const noexcept { return !reading_; }

private:
    friend class TcpServer;

    Connection(EventLoop& loop, Handle handle, std::string peer, std::uint64_t id);

    void start(std::chrono::milliseconds idle_timeout, std::size_t high_watermark,
               std::function<void(Connection::Ptr, std::string_view)> on_data,
               std::function<void(Connection::Ptr)> on_close);
    void on_readable();
    void flush();
    void apply_interest();
    void touch();
    void schedule_idle_check();
    void finish_close();

    EventLoop* loop_{nullptr};
    Handle handle_{kInvalidHandle};
    std::string peer_;
    std::uint64_t id_{0};

    Buffer output_;

    bool alive_{true};
    bool reading_{true};
    bool want_close_{false};
    std::uint32_t interest_{0};
    std::uint64_t bytes_in_{0};
    std::uint64_t bytes_out_{0};
    TimePoint last_touch_{};

    std::chrono::milliseconds idle_timeout_{0};
    std::size_t high_watermark_{1024U * 1024U};
    EventLoop::TimerId idle_timer_{0};

    std::function<void(Connection::Ptr, std::string_view)> on_data_;
    std::function<void(Connection::Ptr)> on_close_;
};

// Multi threaded TCP server:
//
//   acceptor thread (own loop)  --accept-->  round robin --post-->  N I/O loops
//
// Each I/O loop owns its connections and poller, so a connection is only ever
// touched by one thread. The acceptor does nothing but accept and hand off,
// which keeps the accept path free of application logic.
class TcpServer {
public:
    struct Options {
        std::string host{"0.0.0.0"};
        std::uint16_t port{8080};
        int io_threads{4};
        std::size_t max_connections{20000};
        std::chrono::milliseconds idle_timeout{std::chrono::milliseconds(65'000)};
        std::size_t high_watermark{1024U * 1024U};
    };

    struct Callbacks {
        std::function<void(Connection::Ptr)> on_open;
        std::function<void(Connection::Ptr, std::string_view)> on_data;
        std::function<void(Connection::Ptr)> on_close;
    };

    TcpServer(Options options, Callbacks callbacks);
    ~TcpServer();

    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    Result<void> start();
    void stop();

    [[nodiscard]] std::uint16_t port() const noexcept { return bound_port_; }
    [[nodiscard]] std::size_t active_connections() const noexcept {
        return active_connections_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] const char* io_backend() const;
    [[nodiscard]] int io_thread_count() const noexcept { return options_.io_threads; }

private:
    struct IoWorker {
        std::unique_ptr<EventLoop> loop;
        std::thread thread;
        std::unordered_map<std::uint64_t, Connection::Ptr> connections;
    };

    void acceptor_loop();
    void handle_accept();
    void attach_connection(std::size_t worker_index, Handle handle, std::string peer);
    void close_connections();

    Options options_;
    Callbacks callbacks_;

    Handle listener_{kInvalidHandle};
    std::uint16_t bound_port_{0};
    std::unique_ptr<EventLoop> acceptor_loop_;
    std::thread acceptor_thread_;

    std::vector<std::unique_ptr<IoWorker>> workers_;
    std::atomic<std::size_t> next_worker_{0};
    std::atomic<std::size_t> active_connections_{0};
    std::atomic<std::uint64_t> next_connection_id_{1};
    std::atomic<bool> running_{false};
};

}  // namespace aegis::net
