#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "common/Status.h"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace aegis::net {

#if defined(_WIN32)
using Handle = std::uintptr_t;
inline constexpr Handle kInvalidHandle = static_cast<Handle>(~static_cast<std::uintptr_t>(0));
#else
using Handle = int;
inline constexpr Handle kInvalidHandle = -1;
#endif

enum class IoStatus { Ok, WouldBlock, Closed, Error };

struct IoOutcome {
    IoStatus status{IoStatus::Error};
    std::size_t bytes{0};
    int error{0};

    [[nodiscard]] bool ok() const noexcept { return status == IoStatus::Ok; }
    [[nodiscard]] bool would_block() const noexcept { return status == IoStatus::WouldBlock; }
    [[nodiscard]] bool eof() const noexcept { return status == IoStatus::Closed; }
};

struct Endpoint {
    sockaddr_storage storage{};
    socklen_t length{0};

    [[nodiscard]] int family() const noexcept { return storage.ss_family; }
    [[nodiscard]] std::string to_string() const;
    [[nodiscard]] std::string host() const;
    [[nodiscard]] std::uint16_t port() const;
};

// Initialises Winsock on Windows; a no-op elsewhere. Called lazily by the
// socket helpers so callers never have to care.
void ensure_socket_layer();

[[nodiscard]] int last_socket_error();
[[nodiscard]] std::string error_text(int error);
[[nodiscard]] bool is_would_block(int error);

[[nodiscard]] Result<std::vector<Endpoint>> resolve(std::string_view host, std::uint16_t port,
                                                   int socket_type = SOCK_STREAM);

// "host:port" / "[::1]:port" / "127.0.0.1".
[[nodiscard]] Result<Endpoint> parse_endpoint(std::string_view text, std::uint16_t default_port);

[[nodiscard]] Result<void> set_nonblocking(Handle handle);
[[nodiscard]] Result<void> set_nodelay(Handle handle);
[[nodiscard]] Result<void> set_reuseaddr(Handle handle);
[[nodiscard]] Result<void> set_keepalive(Handle handle);

[[nodiscard]] Result<Handle> listen_tcp(const Endpoint& endpoint, int backlog = 256);
[[nodiscard]] Result<Handle> create_tcp_socket(int family = AF_INET);
void close_socket(Handle handle) noexcept;

[[nodiscard]] Result<void> set_recv_timeout(Handle handle, std::chrono::milliseconds timeout);
[[nodiscard]] Result<void> set_send_timeout(Handle handle, std::chrono::milliseconds timeout);

struct AcceptOutcome {
    Handle handle{kInvalidHandle};
    IoStatus status{IoStatus::Error};
    int error{0};
};
[[nodiscard]] AcceptOutcome accept_tcp(Handle listener, Endpoint* peer = nullptr);

// Non-blocking connect: returns success when connected immediately, success with
// `in_progress == true` when the connection needs poller attention, or an error.
struct ConnectOutcome {
    IoStatus status{IoStatus::Error};
    int error{0};
    bool in_progress{false};
};
[[nodiscard]] ConnectOutcome connect_tcp(Handle handle, const Endpoint& endpoint);

// Verifies the result of a connect that previously returned in_progress.
[[nodiscard]] Result<void> finish_connect(Handle handle);

[[nodiscard]] IoOutcome read_some(Handle handle, void* buffer, std::size_t length);
[[nodiscard]] IoOutcome write_some(Handle handle, const void* buffer, std::size_t length);

// Best effort: wakes up a poller that is waiting on this socket pair.
[[nodiscard]] Result<void> create_socket_pair(Handle* read_handle, Handle* write_handle);

[[nodiscard]] std::string local_address(Handle handle);
[[nodiscard]] std::string peer_address(Handle handle);
[[nodiscard]] Result<Endpoint> socket_local_endpoint(Handle handle);

}  // namespace aegis::net
