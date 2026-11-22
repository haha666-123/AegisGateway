#include "net/TcpServer.h"

#include <algorithm>
#include <array>
#include <utility>
#include <vector>

#include "common/Log.h"
#include "common/Metrics.h"

namespace aegis::net {
namespace {

constexpr std::size_t kReadChunk = 64U * 1024U;
constexpr int kAcceptBatch = 64;
constexpr auto kIdleTimerGranularity = std::chrono::milliseconds(1000);

}  // namespace

// ------------------------------------------------------------------ Connection

Connection::Connection(EventLoop& loop, Handle handle, std::string peer, std::uint64_t id)
    : loop_(&loop), handle_(handle), peer_(std::move(peer)), id_(id) {}

Connection::~Connection() {
    if (handle_ != kInvalidHandle) {
        close_socket(handle_);
        handle_ = kInvalidHandle;
    }
}

void Connection::start(std::chrono::milliseconds idle_timeout, std::size_t high_watermark,
                       std::function<void(Connection::Ptr, std::string_view)> on_data,
                       std::function<void(Connection::Ptr)> on_close) {
    idle_timeout_ = idle_timeout;
    high_watermark_ = high_watermark;
    on_data_ = std::move(on_data);
    on_close_ = std::move(on_close);

    std::weak_ptr<Connection> weak = weak_from_this();
    loop_->add(handle_, kReadable, [weak](std::uint32_t events) {
        const auto self = weak.lock();
        if (!self) return;
        if ((events & (kError | kHangup)) != 0 && (events & kReadable) == 0) {
            self->close_now();
            return;
        }
        if ((events & kReadable) != 0) self->on_readable();
        if (self->alive_ && (events & kWritable) != 0) self->flush();
    });
    interest_ = kReadable;
    schedule_idle_check();
}

void Connection::on_readable() {
    std::array<char, kReadChunk> scratch{};
    for (;;) {
        const auto outcome = read_some(handle_, scratch.data(), scratch.size());
        if (outcome.ok()) {
            bytes_in_ += outcome.bytes;
            Metrics::instance().inc(Counter::RequestBytesTotal, outcome.bytes);
            touch();
            if (on_data_) {
                on_data_(shared_from_this(),
                         std::string_view(scratch.data(), outcome.bytes));
            }
            if (!alive_) return;
            if (!reading_) return;  // the handler applied back pressure
            continue;
        }
        if (outcome.status == IoStatus::Closed) {
            close_now();
            return;
        }
        if (outcome.status == IoStatus::WouldBlock) return;
        close_now();
        return;
    }
}

void Connection::send(std::string_view data) {
    if (!alive_ || data.empty()) return;
    output_.append(data);
    bytes_out_ += data.size();
    Metrics::instance().inc(Counter::ResponseBytesTotal, data.size());
    flush();
}

void Connection::send(std::string_view first, std::string_view second) {
    if (!alive_) return;
    std::size_t total = 0;
    if (!first.empty()) {
        output_.append(first);
        total += first.size();
    }
    if (!second.empty()) {
        output_.append(second);
        total += second.size();
    }
    if (total == 0) return;
    bytes_out_ += total;
    Metrics::instance().inc(Counter::ResponseBytesTotal, total);
    flush();
}

void Connection::flush() {
    if (!alive_) return;

    while (output_.readable() > 0) {
        const auto outcome = write_some(handle_, output_.data(), output_.readable());
        if (outcome.ok()) {
            output_.consume(outcome.bytes);
            touch();
            continue;
        }
        if (outcome.status == IoStatus::WouldBlock) {
            apply_interest();
            return;
        }
        close_now();
        return;
    }

    if (want_close_) {
        finish_close();
        return;
    }
    apply_interest();
}

void Connection::apply_interest() {
    std::uint32_t desired = 0;
    if (reading_ && !want_close_) desired |= kReadable;
    if (output_.readable() > 0) desired |= kWritable;
    if (desired == interest_) return;
    interest_ = desired;
    loop_->update(handle_, desired);
}

void Connection::pause_reading() {
    if (!alive_ || !reading_) return;
    reading_ = false;
    apply_interest();
}

void Connection::resume_reading() {
    if (!alive_ || reading_) return;
    reading_ = true;
    apply_interest();
}

void Connection::close() {
    if (!alive_) return;
    want_close_ = true;
    if (output_.readable() == 0) {
        finish_close();
        return;
    }
    apply_interest();
}

void Connection::close_now() {
    if (!alive_) return;
    want_close_ = true;
    output_.clear();
    finish_close();
}

void Connection::touch() {
    if (idle_timeout_.count() <= 0) return;  // idle timeout disabled
    if (last_touch_.time_since_epoch().count() == 0 ||
        steady_now() - last_touch_ >= kIdleTimerGranularity) {
        last_touch_ = steady_now();
        schedule_idle_check();
    }
}

void Connection::schedule_idle_check() {
    if (idle_timeout_.count() <= 0) return;
    if (idle_timer_ != 0) loop_->cancel_timer(idle_timer_);

    std::weak_ptr<Connection> weak = weak_from_this();
    idle_timer_ = loop_->run_after(idle_timeout_, [weak] {
        const auto self = weak.lock();
        if (!self || !self->alive_) return;
        AEGIS_DEBUG("connection {} idle timeout after {}ms", self->id_,
                    self->idle_timeout_.count());
        self->close_now();
    });
}

void Connection::finish_close() {
    if (!alive_) return;
    alive_ = false;

    if (idle_timer_ != 0) {
        loop_->cancel_timer(idle_timer_);
        idle_timer_ = 0;
    }
    loop_->remove(handle_);
    close_socket(handle_);
    handle_ = kInvalidHandle;

    // Copy the callback: the server erases its last reference to us while this
    // runs, which may destroy `this` before the call returns.
    auto callback = on_close_;
    if (callback) callback(shared_from_this());
}

// ------------------------------------------------------------------ TcpServer

TcpServer::TcpServer(Options options, Callbacks callbacks)
    : options_(std::move(options)), callbacks_(std::move(callbacks)) {
    options_.io_threads = std::clamp(options_.io_threads, 1, 64);
    options_.high_watermark = std::max<std::size_t>(options_.high_watermark, 64U * 1024U);
}

TcpServer::~TcpServer() { stop(); }

Result<void> TcpServer::start() {
    if (running_.load()) return fail(errc::kInvalidArgument, "server already running");

    auto endpoints = resolve(options_.host, options_.port);
    if (!endpoints) return std::unexpected(endpoints.error());

    // Prefer IPv4 for the wildcard bind: it keeps logs readable and matches what
    // container port mappings expect.
    Endpoint endpoint = endpoints->front();
    for (const auto& candidate : *endpoints) {
        if (candidate.family() == AF_INET) {
            endpoint = candidate;
            break;
        }
    }

    auto listener = listen_tcp(endpoint, 1024);
    if (!listener) return std::unexpected(listener.error());
    listener_ = *listener;

    auto local = socket_local_endpoint(listener_);
    if (!local) {
        close_socket(listener_);
        listener_ = kInvalidHandle;
        return std::unexpected(local.error());
    }
    bound_port_ = local->port();

    running_.store(true);

    acceptor_loop_ = std::make_unique<EventLoop>();
    for (int i = 0; i < options_.io_threads; ++i) {
        auto worker = std::make_unique<IoWorker>();
        worker->loop = std::make_unique<EventLoop>();
        workers_.push_back(std::move(worker));
    }
    for (auto& worker : workers_) {
        IoWorker* raw = worker.get();
        worker->thread = std::thread([raw] { raw->loop->run(); });
    }

    acceptor_thread_ = std::thread([this] {
        acceptor_loop_->add(listener_, kReadable, [this](std::uint32_t) { handle_accept(); });
        acceptor_loop_->run();
        acceptor_loop_->remove(listener_);
    });

    Metrics::instance().set(Gauge::ThreadsIo, static_cast<double>(options_.io_threads));

    AEGIS_INFO("tcp server listening on port {} with {} io threads ({})", bound_port_,
               options_.io_threads, workers_.empty() ? "n/a" : workers_.front()->loop->backend());
    return {};
}

void TcpServer::handle_accept() {
    for (int i = 0; i < kAcceptBatch; ++i) {
        Endpoint peer;
        const auto outcome = accept_tcp(listener_, &peer);
        if (outcome.status == IoStatus::WouldBlock) return;
        if (outcome.status != IoStatus::Ok) {
            AEGIS_WARN("accept failed: {}", error_text(outcome.error));
            return;
        }

        if (active_connections_.load(std::memory_order_relaxed) >= options_.max_connections) {
            close_socket(outcome.handle);
            AEGIS_WARN("connection limit {} reached, rejecting {}", options_.max_connections,
                       peer.to_string());
            continue;
        }

        const std::size_t index =
            next_worker_.fetch_add(1, std::memory_order_relaxed) % workers_.size();
        auto worker_loop = workers_[index]->loop.get();
        worker_loop->post([this, index, handle = outcome.handle, peer_text = peer.to_string()] {
            attach_connection(index, handle, peer_text);
        });
    }
}

void TcpServer::attach_connection(std::size_t worker_index, Handle handle, std::string peer) {
    IoWorker& worker = *workers_[worker_index];
    const std::uint64_t id = next_connection_id_.fetch_add(1, std::memory_order_relaxed);
    auto connection = std::shared_ptr<Connection>(
        new Connection(*worker.loop, handle, std::move(peer), id));

    worker.connections[id] = connection;
    active_connections_.fetch_add(1, std::memory_order_relaxed);
    Metrics::instance().inc(Counter::ConnectionsOpenedTotal);
    Metrics::instance().set(Gauge::ConnectionsActive,
                            static_cast<double>(active_connections_.load()));

    auto this_index = worker_index;
    auto data_handler = callbacks_.on_data;
    auto close_handler = callbacks_.on_close;

    connection->start(options_.idle_timeout, options_.high_watermark, data_handler,
                      [this, this_index, close_handler](Connection::Ptr conn) {
                          if (close_handler) close_handler(conn);
                          Metrics::instance().inc(Counter::ConnectionsClosedTotal);
                          Metrics::instance().set(
                              Gauge::ConnectionsActive,
                              static_cast<double>(active_connections_.fetch_sub(
                                                      1, std::memory_order_relaxed) -
                                                  1));

                          // Defer the erase: the connection object may still be
                          // on the stack of its own dispatch handler.
                          const std::uint64_t conn_id = conn->id();
                          workers_[this_index]->loop->post([this, this_index, conn_id] {
                              workers_[this_index]->connections.erase(conn_id);
                          });
                      });

    if (callbacks_.on_open) callbacks_.on_open(connection);
}

void TcpServer::stop() {
    if (!running_.exchange(false)) return;

    if (acceptor_loop_) acceptor_loop_->stop();
    if (acceptor_thread_.joinable()) acceptor_thread_.join();
    if (listener_ != kInvalidHandle) {
        close_socket(listener_);
        listener_ = kInvalidHandle;
    }

    for (auto& worker : workers_) {
        if (worker->loop) worker->loop->stop();
    }
    for (auto& worker : workers_) {
        if (worker->thread.joinable()) worker->thread.join();
    }
    for (auto& worker : workers_) {
        worker->connections.clear();
    }
    workers_.clear();
    acceptor_loop_.reset();

    active_connections_.store(0, std::memory_order_relaxed);
    Metrics::instance().set(Gauge::ConnectionsActive, 0.0);
    AEGIS_INFO("tcp server stopped");
}

void TcpServer::close_connections() {
    for (auto& worker : workers_) {
        worker->connections.clear();
    }
}

const char* TcpServer::io_backend() const {
    if (workers_.empty() || !workers_.front()->loop) return "unstarted";
    return workers_.front()->loop->backend();
}

}  // namespace aegis::net
