#include "net/Socket.h"

#include <algorithm>
#include <cstring>

#if defined(_WIN32)
#include <mstcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace aegis::net {
namespace {

#if defined(_WIN32)
void winsock_startup() {
    static const bool ok = [] {
        WSADATA data{};
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    (void)ok;
}
#else
void winsock_startup() {}
#endif

bool same_family(const sockaddr_storage& storage, int family) {
    return storage.ss_family == family;
}

}  // namespace

void ensure_socket_layer() { winsock_startup(); }

int last_socket_error() {
#if defined(_WIN32)
    return WSAGetLastError();
#else
    return errno;
#endif
}

bool is_would_block(int error) {
#if defined(_WIN32)
    return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS;
#else
    return error == EAGAIN || error == EWOULDBLOCK || error == EINPROGRESS;
#endif
}

std::string error_text(int error) {
#if defined(_WIN32)
    char* buffer = nullptr;
    const DWORD length =
        ::FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                             FORMAT_MESSAGE_IGNORE_INSERTS,
                         nullptr, static_cast<DWORD>(error), 0,
                         reinterpret_cast<LPSTR>(&buffer), 0, nullptr);
    std::string text = (length > 0 && buffer != nullptr) ? std::string(buffer, length)
                                                         : "socket error " + std::to_string(error);
    if (buffer != nullptr) ::LocalFree(buffer);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == '.')) {
        text.pop_back();
    }
    return text;
#else
    return std::string(std::strerror(error));
#endif
}

std::string Endpoint::to_string() const {
    char host_buffer[NI_MAXHOST]{};
    char service_buffer[NI_MAXSERV]{};
    const int rc = ::getnameinfo(reinterpret_cast<const sockaddr*>(&storage), length, host_buffer,
                                 sizeof(host_buffer), service_buffer, sizeof(service_buffer),
                                 NI_NUMERICHOST | NI_NUMERICSERV);
    if (rc != 0) return "<invalid>";
    if (family() == AF_INET6) {
        return "[" + std::string(host_buffer) + "]:" + service_buffer;
    }
    return std::string(host_buffer) + ":" + service_buffer;
}

std::string Endpoint::host() const {
    char host_buffer[NI_MAXHOST]{};
    const int rc = ::getnameinfo(reinterpret_cast<const sockaddr*>(&storage), length, host_buffer,
                                 sizeof(host_buffer), nullptr, 0, NI_NUMERICHOST);
    return rc == 0 ? std::string(host_buffer) : std::string{};
}

std::uint16_t Endpoint::port() const {
    if (same_family(storage, AF_INET)) {
        return ntohs(reinterpret_cast<const sockaddr_in*>(&storage)->sin_port);
    }
    if (same_family(storage, AF_INET6)) {
        return ntohs(reinterpret_cast<const sockaddr_in6*>(&storage)->sin6_port);
    }
    return 0;
}

Result<std::vector<Endpoint>> resolve(std::string_view host, std::uint16_t port, int socket_type) {
    ensure_socket_layer();

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = socket_type;
    hints.ai_flags = AI_NUMERICSERV;

    const std::string host_text(host);
    const std::string port_text = std::to_string(port);

    addrinfo* result = nullptr;
    const int rc = ::getaddrinfo(host.empty() ? nullptr : host_text.c_str(), port_text.c_str(),
                                 &hints, &result);
    if (rc != 0) {
        return fail(errc::kIoError,
                    "getaddrinfo(" + host_text + ") failed: " + ::gai_strerror(rc));
    }

    std::vector<Endpoint> endpoints;
    for (const addrinfo* it = result; it != nullptr; it = it->ai_next) {
        if (it->ai_addr == nullptr || it->ai_addrlen == 0) continue;
        if (static_cast<std::size_t>(it->ai_addrlen) > sizeof(sockaddr_storage)) continue;
        Endpoint endpoint;
        std::memcpy(&endpoint.storage, it->ai_addr, it->ai_addrlen);
        endpoint.length = static_cast<socklen_t>(it->ai_addrlen);
        endpoints.push_back(endpoint);
    }
    ::freeaddrinfo(result);

    if (endpoints.empty()) {
        return fail(errc::kIoError, "no address resolved for " + host_text);
    }
    return endpoints;
}

Result<Endpoint> parse_endpoint(std::string_view text, std::uint16_t default_port) {
    std::string_view host = text;
    std::uint16_t port = default_port;

    if (!text.empty() && text.front() == '[') {
        const auto close = text.find(']');
        if (close == std::string_view::npos) {
            return fail(errc::kInvalidArgument, "unterminated IPv6 literal in endpoint");
        }
        host = text.substr(1, close - 1);
        const auto rest = text.substr(close + 1);
        if (rest.starts_with(":")) {
            port = static_cast<std::uint16_t>(std::stoi(std::string(rest.substr(1))));
        }
    } else {
        const auto colon = text.rfind(':');
        if (colon != std::string_view::npos && text.find(':') == colon) {
            port = static_cast<std::uint16_t>(std::stoi(std::string(text.substr(colon + 1))));
            host = text.substr(0, colon);
        }
    }

    auto endpoints = resolve(host, port);
    if (!endpoints) return std::unexpected(endpoints.error());
    return endpoints->front();
}

Result<void> set_nonblocking(Handle handle) {
#if defined(_WIN32)
    u_long mode = 1;
    if (::ioctlsocket(static_cast<SOCKET>(handle), FIONBIO, &mode) != 0) {
        return fail(errc::kIoError, "ioctlsocket(FIONBIO): " + error_text(last_socket_error()));
    }
#else
    const int flags = ::fcntl(static_cast<int>(handle), F_GETFL, 0);
    if (flags < 0) return fail(errc::kIoError, "fcntl(F_GETFL): " + error_text(last_socket_error()));
    if (::fcntl(static_cast<int>(handle), F_SETFL, flags | O_NONBLOCK) < 0) {
        return fail(errc::kIoError, "fcntl(F_SETFL): " + error_text(last_socket_error()));
    }
#endif
    return {};
}

Result<void> set_nodelay(Handle handle) {
    const int yes = 1;
#if defined(_WIN32)
    if (::setsockopt(static_cast<SOCKET>(handle), IPPROTO_TCP, TCP_NODELAY,
                     reinterpret_cast<const char*>(&yes), sizeof(yes)) != 0) {
#else
    if (::setsockopt(static_cast<int>(handle), IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes)) != 0) {
#endif
        return fail(errc::kIoError, "setsockopt(TCP_NODELAY): " + error_text(last_socket_error()));
    }
    return {};
}

Result<void> set_reuseaddr(Handle handle) {
    const int yes = 1;
#if defined(_WIN32)
    if (::setsockopt(static_cast<SOCKET>(handle), SOL_SOCKET, SO_REUSEADDR,
                     reinterpret_cast<const char*>(&yes), sizeof(yes)) != 0) {
#else
    if (::setsockopt(static_cast<int>(handle), SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) != 0) {
#endif
        return fail(errc::kIoError, "setsockopt(SO_REUSEADDR): " + error_text(last_socket_error()));
    }
    return {};
}

Result<void> set_keepalive(Handle handle) {
    const int yes = 1;
#if defined(_WIN32)
    if (::setsockopt(static_cast<SOCKET>(handle), SOL_SOCKET, SO_KEEPALIVE,
                     reinterpret_cast<const char*>(&yes), sizeof(yes)) != 0) {
#else
    if (::setsockopt(static_cast<int>(handle), SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof(yes)) != 0) {
#endif
        return fail(errc::kIoError, "setsockopt(SO_KEEPALIVE): " + error_text(last_socket_error()));
    }
    return {};
}

Result<Handle> listen_tcp(const Endpoint& endpoint, int backlog) {
    ensure_socket_layer();

#if defined(_WIN32)
    const SOCKET raw = ::socket(endpoint.family(), SOCK_STREAM, IPPROTO_TCP);
    if (raw == INVALID_SOCKET) {
        return fail(errc::kIoError, "socket(): " + error_text(last_socket_error()));
    }
    const Handle handle = static_cast<Handle>(raw);
#else
    const int raw = ::socket(endpoint.family(), SOCK_STREAM, IPPROTO_TCP);
    if (raw < 0) return fail(errc::kIoError, "socket(): " + error_text(last_socket_error()));
    const Handle handle = raw;
#endif

    auto fail_and_close = [&](const std::string& what) -> Result<Handle> {
        const int error = last_socket_error();
        close_socket(handle);
        return fail(errc::kIoError, what + ": " + error_text(error));
    };

    if (!set_reuseaddr(handle)) return fail_and_close("SO_REUSEADDR");

#if defined(SO_REUSEPORT) && !defined(_WIN32)
    const int yes = 1;
    ::setsockopt(static_cast<int>(handle), SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes));
#endif

#if defined(_WIN32)
    if (::bind(raw, reinterpret_cast<const sockaddr*>(&endpoint.storage), endpoint.length) != 0) {
#else
    if (::bind(raw, reinterpret_cast<const sockaddr*>(&endpoint.storage), endpoint.length) != 0) {
#endif
        return fail_and_close("bind()");
    }
    if (::listen(raw, backlog) != 0) return fail_and_close("listen()");

    if (!set_nonblocking(handle)) return fail_and_close("set_nonblocking()");
    return handle;
}

Result<Handle> create_tcp_socket(int family) {
    ensure_socket_layer();
#if defined(_WIN32)
    const SOCKET raw = ::socket(family, SOCK_STREAM, IPPROTO_TCP);
    if (raw == INVALID_SOCKET) {
        return fail(errc::kIoError, "socket(): " + error_text(last_socket_error()));
    }
    return static_cast<Handle>(raw);
#else
    const int raw = ::socket(family, SOCK_STREAM, IPPROTO_TCP);
    if (raw < 0) return fail(errc::kIoError, "socket(): " + error_text(last_socket_error()));
    return raw;
#endif
}

Result<void> set_recv_timeout(Handle handle, std::chrono::milliseconds timeout) {
#if defined(_WIN32)
    const DWORD value = static_cast<DWORD>(timeout.count());
    if (::setsockopt(static_cast<SOCKET>(handle), SOL_SOCKET, SO_RCVTIMEO,
                     reinterpret_cast<const char*>(&value), sizeof(value)) != 0) {
        return fail(errc::kIoError, "setsockopt(SO_RCVTIMEO): " + error_text(last_socket_error()));
    }
#else
    timeval value{};
    value.tv_sec = static_cast<long>(timeout.count() / 1000);
    value.tv_usec = static_cast<long>((timeout.count() % 1000) * 1000);
    if (::setsockopt(static_cast<int>(handle), SOL_SOCKET, SO_RCVTIMEO, &value, sizeof(value)) != 0) {
        return fail(errc::kIoError, "setsockopt(SO_RCVTIMEO): " + error_text(last_socket_error()));
    }
#endif
    return {};
}

Result<void> set_send_timeout(Handle handle, std::chrono::milliseconds timeout) {
#if defined(_WIN32)
    const DWORD value = static_cast<DWORD>(timeout.count());
    if (::setsockopt(static_cast<SOCKET>(handle), SOL_SOCKET, SO_SNDTIMEO,
                     reinterpret_cast<const char*>(&value), sizeof(value)) != 0) {
        return fail(errc::kIoError, "setsockopt(SO_SNDTIMEO): " + error_text(last_socket_error()));
    }
#else
    timeval value{};
    value.tv_sec = static_cast<long>(timeout.count() / 1000);
    value.tv_usec = static_cast<long>((timeout.count() % 1000) * 1000);
    if (::setsockopt(static_cast<int>(handle), SOL_SOCKET, SO_SNDTIMEO, &value, sizeof(value)) != 0) {
        return fail(errc::kIoError, "setsockopt(SO_SNDTIMEO): " + error_text(last_socket_error()));
    }
#endif
    return {};
}

void close_socket(Handle handle) noexcept {
    if (handle == kInvalidHandle) return;
#if defined(_WIN32)
    ::closesocket(static_cast<SOCKET>(handle));
#else
    ::close(static_cast<int>(handle));
#endif
}

AcceptOutcome accept_tcp(Handle listener, Endpoint* peer) {
    Endpoint local;
    Endpoint& target = peer != nullptr ? *peer : local;
    target.length = sizeof(sockaddr_storage);

#if defined(_WIN32)
    const SOCKET client = ::accept(static_cast<SOCKET>(listener),
                                   reinterpret_cast<sockaddr*>(&target.storage), &target.length);
    if (client == INVALID_SOCKET) {
        const int error = last_socket_error();
        if (error == WSAEWOULDBLOCK) return {kInvalidHandle, IoStatus::WouldBlock, error};
        return {kInvalidHandle, IoStatus::Error, error};
    }
    const Handle handle = static_cast<Handle>(client);
#else
    const int client = ::accept(static_cast<int>(listener),
                                reinterpret_cast<sockaddr*>(&target.storage), &target.length);
    if (client < 0) {
        const int error = last_socket_error();
        if (error == EAGAIN || error == EWOULDBLOCK) {
            return {kInvalidHandle, IoStatus::WouldBlock, error};
        }
        return {kInvalidHandle, IoStatus::Error, error};
    }
    const Handle handle = client;
#endif

    if (!set_nonblocking(handle)) {
        const int error = last_socket_error();
        close_socket(handle);
        return {kInvalidHandle, IoStatus::Error, error};
    }
    (void)set_nodelay(handle);
    return {handle, IoStatus::Ok, 0};
}

ConnectOutcome connect_tcp(Handle handle, const Endpoint& endpoint) {
#if defined(_WIN32)
    const int rc = ::connect(static_cast<SOCKET>(handle),
                             reinterpret_cast<const sockaddr*>(&endpoint.storage), endpoint.length);
    const int error = rc == 0 ? 0 : last_socket_error();
    const bool in_progress = rc != 0 && (error == WSAEWOULDBLOCK || error == WSAEINPROGRESS);
#else
    const int rc = ::connect(static_cast<int>(handle),
                             reinterpret_cast<const sockaddr*>(&endpoint.storage), endpoint.length);
    const int error = rc == 0 ? 0 : last_socket_error();
    const bool in_progress = rc != 0 && (error == EINPROGRESS || error == EAGAIN);
#endif
    if (rc == 0) return {IoStatus::Ok, 0, false};
    if (in_progress) return {IoStatus::WouldBlock, error, true};
    return {IoStatus::Error, error, false};
}

Result<void> finish_connect(Handle handle) {
    int error = 0;
    socklen_t length = sizeof(error);
#if defined(_WIN32)
    if (::getsockopt(static_cast<SOCKET>(handle), SOL_SOCKET, SO_ERROR,
                     reinterpret_cast<char*>(&error), &length) != 0) {
        return fail(errc::kIoError, "getsockopt(SO_ERROR): " + error_text(last_socket_error()));
    }
#else
    if (::getsockopt(static_cast<int>(handle), SOL_SOCKET, SO_ERROR, &error, &length) != 0) {
        return fail(errc::kIoError, "getsockopt(SO_ERROR): " + error_text(last_socket_error()));
    }
#endif
    if (error != 0) {
        return fail(errc::kIoError, "connect failed: " + error_text(error));
    }
    return {};
}

IoOutcome read_some(Handle handle, void* buffer, std::size_t length) {
#if defined(_WIN32)
    const int received = ::recv(static_cast<SOCKET>(handle), static_cast<char*>(buffer),
                                static_cast<int>(length), 0);
#else
    const ssize_t received = ::recv(static_cast<int>(handle), buffer, length, 0);
#endif
    if (received > 0) {
        return {IoStatus::Ok, static_cast<std::size_t>(received), 0};
    }
    if (received == 0) return {IoStatus::Closed, 0, 0};

    const int error = last_socket_error();
    if (is_would_block(error)) return {IoStatus::WouldBlock, 0, error};
    return {IoStatus::Error, 0, error};
}

IoOutcome write_some(Handle handle, const void* buffer, std::size_t length) {
#if defined(_WIN32)
    const int sent = ::send(static_cast<SOCKET>(handle), static_cast<const char*>(buffer),
                            static_cast<int>(length), 0);
#else
    const ssize_t sent = ::send(static_cast<int>(handle), buffer, length, MSG_NOSIGNAL);
#endif
    if (sent > 0) return {IoStatus::Ok, static_cast<std::size_t>(sent), 0};

    const int error = last_socket_error();
    if (is_would_block(error)) return {IoStatus::WouldBlock, 0, error};
    return {IoStatus::Error, 0, error};
}

Result<void> create_socket_pair(Handle* read_handle, Handle* write_handle) {
    ensure_socket_layer();
    if (read_handle == nullptr || write_handle == nullptr) {
        return fail(errc::kInvalidArgument, "null out parameter");
    }

    // A connected TCP pair on the loopback interface works on every platform
    // we target and is easy to reason about: one end is registered with the
    // poller, the other is written by foreign threads.
    auto endpoints = resolve("127.0.0.1", 0);
    if (!endpoints) return std::unexpected(endpoints.error());

    auto listener = listen_tcp(endpoints->front(), 4);
    if (!listener) return std::unexpected(listener.error());

    Endpoint local{};
    socklen_t length = sizeof(local.storage);
#if defined(_WIN32)
    ::getsockname(static_cast<SOCKET>(*listener), reinterpret_cast<sockaddr*>(&local.storage),
                  &length);
#else
    ::getsockname(static_cast<int>(*listener), reinterpret_cast<sockaddr*>(&local.storage), &length);
#endif
    local.length = length;

#if defined(_WIN32)
    const SOCKET client = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (client == INVALID_SOCKET) {
        close_socket(*listener);
        return fail(errc::kIoError, "socket(): " + error_text(last_socket_error()));
    }
    const Handle client_handle = static_cast<Handle>(client);
#else
    const int client = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (client < 0) {
        close_socket(*listener);
        return fail(errc::kIoError, "socket(): " + error_text(last_socket_error()));
    }
    const Handle client_handle = client;
#endif

    (void)connect_tcp(client_handle, local);
    Endpoint peer{};
    auto accepted = accept_tcp(*listener, &peer);
    close_socket(*listener);

    if (accepted.status != IoStatus::Ok) {
        close_socket(client_handle);
        return fail(errc::kIoError, "socket pair accept failed");
    }

    if (!set_nonblocking(client_handle) || !set_nonblocking(accepted.handle)) {
        close_socket(client_handle);
        close_socket(accepted.handle);
        return fail(errc::kIoError, "socket pair nonblocking failed");
    }

    *read_handle = accepted.handle;
    *write_handle = client_handle;
    return {};
}

Result<Endpoint> socket_local_endpoint(Handle handle) {
    Endpoint endpoint{};
    socklen_t length = sizeof(endpoint.storage);
#if defined(_WIN32)
    if (::getsockname(static_cast<SOCKET>(handle), reinterpret_cast<sockaddr*>(&endpoint.storage),
                      &length) != 0) {
#else
    if (::getsockname(static_cast<int>(handle), reinterpret_cast<sockaddr*>(&endpoint.storage),
                      &length) != 0) {
#endif
        return fail(errc::kIoError, "getsockname: " + error_text(last_socket_error()));
    }
    endpoint.length = length;
    return endpoint;
}

std::string local_address(Handle handle) {
    Endpoint endpoint{};
    socklen_t length = sizeof(endpoint.storage);
#if defined(_WIN32)
    if (::getsockname(static_cast<SOCKET>(handle), reinterpret_cast<sockaddr*>(&endpoint.storage),
                      &length) != 0) {
#else
    if (::getsockname(static_cast<int>(handle), reinterpret_cast<sockaddr*>(&endpoint.storage),
                      &length) != 0) {
#endif
        return {};
    }
    endpoint.length = length;
    return endpoint.to_string();
}

std::string peer_address(Handle handle) {
    Endpoint endpoint{};
    socklen_t length = sizeof(endpoint.storage);
#if defined(_WIN32)
    if (::getpeername(static_cast<SOCKET>(handle), reinterpret_cast<sockaddr*>(&endpoint.storage),
                      &length) != 0) {
#else
    if (::getpeername(static_cast<int>(handle), reinterpret_cast<sockaddr*>(&endpoint.storage),
                      &length) != 0) {
#endif
        return {};
    }
    endpoint.length = length;
    return endpoint.to_string();
}

}  // namespace aegis::net
