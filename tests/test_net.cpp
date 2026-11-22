#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "TestFramework.h"
#include "common/ThreadPool.h"
#include "net/EventLoop.h"
#include "net/Http.h"
#include "net/Resolver.h"
#include "net/Socket.h"
#include "net/TcpServer.h"

using namespace aegis;
using namespace aegis::net;
using namespace std::chrono_literals;

namespace {

template <class Predicate>
bool wait_for(Predicate predicate, std::chrono::milliseconds timeout = 3000ms) {
    const auto deadline = steady_now() + timeout;
    while (steady_now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(2ms);
    }
    return predicate();
}

class TestClient {
public:
    static std::unique_ptr<TestClient> connect_to(std::uint16_t port) {
        auto handle = create_tcp_socket();
        if (!handle) return nullptr;

        auto endpoints = resolve("127.0.0.1", port);
        if (!endpoints) {
            close_socket(*handle);
            return nullptr;
        }
        const auto outcome = connect_tcp(*handle, endpoints->front());
        if (outcome.status != IoStatus::Ok) {
            close_socket(*handle);
            return nullptr;
        }
        (void)set_recv_timeout(*handle, 300ms);
        (void)set_send_timeout(*handle, 300ms);
        return std::unique_ptr<TestClient>(new TestClient(*handle));
    }

    ~TestClient() { close(); }

    bool send(std::string_view data) {
        const auto outcome = write_some(handle_, data.data(), data.size());
        return outcome.ok();
    }

    std::string read_until(std::string_view marker, std::chrono::milliseconds timeout = 3000ms) {
        const auto deadline = steady_now() + timeout;
        std::string accumulated;
        char buffer[8192];
        while (steady_now() < deadline) {
            const auto outcome = read_some(handle_, buffer, sizeof(buffer));
            if (outcome.ok()) {
                accumulated.append(buffer, outcome.bytes);
                if (accumulated.find(marker) != std::string::npos) return accumulated;
                continue;
            }
            if (outcome.status == IoStatus::Closed) return accumulated;
            // WouldBlock (SO_RCVTIMEO fired) or a transient error: retry until the
            // deadline so the test never depends on a single timing value.
        }
        return accumulated;
    }

    void close() {
        if (handle_ != kInvalidHandle) {
            close_socket(handle_);
            handle_ = kInvalidHandle;
        }
    }

private:
    explicit TestClient(Handle handle) : handle_(handle) {}
    Handle handle_{kInvalidHandle};
};

}  // namespace

AEGIS_TEST(event_loop_post_and_stop) {
    EventLoop loop;
    std::atomic<int> counter{0};
    std::thread runner([&loop] { loop.run(); });

    loop.post([&counter] { counter.fetch_add(1); });
    loop.post([&counter] {
        counter.fetch_add(1);
        counter.fetch_add(1);
    });
    AEGIS_CHECK(wait_for([&counter] { return counter.load() >= 3; }));

    loop.stop();
    runner.join();
    AEGIS_CHECK(!loop.running());
    AEGIS_CHECK_EQ(std::string(loop.backend()).size() > 0, true);
}

AEGIS_TEST(event_loop_timers) {
    EventLoop loop;
    std::atomic<int> fired{0};
    std::atomic<bool> canceled_fired{false};
    std::thread runner([&loop] { loop.run(); });

    loop.run_after(20ms, [&fired] { fired.fetch_add(1); });
    const auto canceled = loop.run_after(30ms, [&canceled_fired] { canceled_fired.store(true); });
    AEGIS_CHECK(loop.cancel_timer(canceled));
    AEGIS_CHECK(!loop.cancel_timer(canceled));

    AEGIS_CHECK(wait_for([&fired] { return fired.load() == 1; }));
    std::this_thread::sleep_for(60ms);
    AEGIS_CHECK_EQ(fired.load(), 1);
    AEGIS_CHECK(!canceled_fired.load());

    loop.stop();
    runner.join();
}

AEGIS_TEST(event_loop_repeating_timer) {
    EventLoop loop;
    std::atomic<int> ticks{0};
    std::atomic<EventLoop::TimerId> id{0};
    std::thread runner([&loop] { loop.run(); });

    id.store(loop.run_every(10ms, [&ticks] { ticks.fetch_add(1); }));
    AEGIS_CHECK(wait_for([&ticks] { return ticks.load() >= 3; }));
    loop.cancel_timer(id.load());
    const int snapshot = ticks.load();
    std::this_thread::sleep_for(50ms);
    AEGIS_CHECK(ticks.load() <= snapshot + 1);

    loop.stop();
    runner.join();
}

AEGIS_TEST(socket_pair_roundtrip) {
    Handle read_end = kInvalidHandle;
    Handle write_end = kInvalidHandle;
    AEGIS_CHECK(create_socket_pair(&read_end, &write_end).has_value());

    const std::string payload = "ping";
    AEGIS_CHECK(write_some(write_end, payload.data(), payload.size()).ok());

    std::string received;
    char buffer[16]{};
    const auto deadline = steady_now() + 1000ms;
    while (received.size() < payload.size() && steady_now() < deadline) {
        const auto outcome = read_some(read_end, buffer, sizeof(buffer));
        if (outcome.ok()) received.append(buffer, outcome.bytes);
        if (outcome.status == IoStatus::Error) break;
    }
    AEGIS_CHECK_EQ(received, payload);

    close_socket(read_end);
    close_socket(write_end);
}

AEGIS_TEST(resolver_resolves_localhost_async) {
    ThreadPool pool(2, "resolver-test");
    Resolver resolver(pool);
    EventLoop loop;
    std::atomic<bool> done{false};
    std::atomic<std::size_t> count{0};

    std::thread runner([&loop] { loop.run(); });
    resolver.resolve_async(loop, "127.0.0.1", 80,
                           [&done, &count](Result<std::vector<Endpoint>> result) {
                               if (result) count.store(result->size());
                               done.store(true);
                           });

    AEGIS_CHECK(wait_for([&done] { return done.load(); }));
    AEGIS_CHECK(count.load() >= 1);

    // Second call must be served from the cache but still delivered on the loop.
    done.store(false);
    resolver.resolve_async(loop, "127.0.0.1", 80,
                           [&done](Result<std::vector<Endpoint>> result) {
                               done.store(result.has_value());
                           });
    AEGIS_CHECK(wait_for([&done] { return done.load(); }));
    AEGIS_CHECK_EQ(resolver.cache_size(), std::size_t{1});

    loop.stop();
    runner.join();
}

AEGIS_TEST(tcp_server_echo) {
    TcpServer::Options options;
    options.host = "127.0.0.1";
    options.port = 0;
    options.io_threads = 2;

    TcpServer::Callbacks callbacks;
    callbacks.on_data = [](Connection::Ptr connection, std::string_view data) {
        connection->send(data);
    };

    TcpServer server(options, callbacks);
    const auto started = server.start();
    AEGIS_CHECK(started.has_value());
    AEGIS_CHECK(server.port() != 0);

    auto client = TestClient::connect_to(server.port());
    AEGIS_CHECK(client != nullptr);
    if (client) {
        AEGIS_CHECK(client->send("hello"));
        AEGIS_CHECK_EQ(client->read_until("hello"), std::string("hello"));

        // The same connection must keep working (keep-alive semantics).
        AEGIS_CHECK(client->send("world"));
        const auto again = client->read_until("world");
        AEGIS_CHECK(again.find("world") != std::string::npos);
    }

    server.stop();
}

AEGIS_TEST(tcp_server_http_roundtrip) {
    TcpServer::Options options;
    options.host = "127.0.0.1";
    options.port = 0;
    options.io_threads = 1;

    // Single I/O thread: connection state needs no locking, exactly like the
    // gateway's per connection parser.
    std::unordered_map<std::uint64_t, HttpRequestParser> parsers;

    TcpServer::Callbacks callbacks;
    callbacks.on_data = [&parsers](Connection::Ptr connection, std::string_view data) {
        auto& parser = parsers[connection->id()];
        const auto state = parser.feed(data);
        if (state == HttpRequestParser::State::Complete) {
            const std::string path(parser.request().path());
            const std::string body = "{\"path\":\"" + path + "\"}";
            connection->send(serialize_response(HttpResponse::json(200, body)));
            parser.reset();
        } else if (state == HttpRequestParser::State::Failed) {
            connection->send(serialize_response(HttpResponse::text(400, "bad request")));
            connection->close();
        }
    };
    callbacks.on_close = [&parsers](Connection::Ptr connection) {
        parsers.erase(connection->id());
    };

    TcpServer server(options, callbacks);
    AEGIS_CHECK(server.start().has_value());

    auto client = TestClient::connect_to(server.port());
    AEGIS_CHECK(client != nullptr);
    if (client) {
        AEGIS_CHECK(client->send("GET /healthz HTTP/1.1\r\nHost: x\r\n\r\n"));
        const auto full = client->read_until("{\"path\":\"/healthz\"}");
        AEGIS_CHECK(full.starts_with("HTTP/1.1 200 OK\r\n"));
        AEGIS_CHECK(full.find("Content-Length: 19") != std::string::npos);
        AEGIS_CHECK(full.find("{\"path\":\"/healthz\"}") != std::string::npos);

        // Keep-alive: a second request on the same socket.
        AEGIS_CHECK(client->send("GET /metrics HTTP/1.1\r\nHost: x\r\n\r\n"));
        const auto second = client->read_until("{\"path\":\"/metrics\"}");
        AEGIS_CHECK(second.find("{\"path\":\"/metrics\"}") != std::string::npos);
    }
    server.stop();
}

AEGIS_TEST(tcp_server_tracks_active_connections) {
    TcpServer::Options options;
    options.host = "127.0.0.1";
    options.port = 0;
    options.io_threads = 1;

    TcpServer::Callbacks callbacks;
    callbacks.on_data = [](Connection::Ptr connection, std::string_view data) {
        connection->send(data);
    };

    TcpServer server(options, callbacks);
    AEGIS_CHECK(server.start().has_value());
    AEGIS_CHECK_EQ(server.active_connections(), std::size_t{0});

    {
        auto client = TestClient::connect_to(server.port());
        AEGIS_CHECK(client != nullptr);
        AEGIS_CHECK(client->send("x"));
        AEGIS_CHECK(!client->read_until("x").empty());
        AEGIS_CHECK(wait_for([&server] { return server.active_connections() == 1; }));
    }

    AEGIS_CHECK(wait_for([&server] { return server.active_connections() == 0; }));
    server.stop();
    AEGIS_CHECK_EQ(server.active_connections(), std::size_t{0});
}
