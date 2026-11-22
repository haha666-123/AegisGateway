#pragma once

#include <expected>
#include <string>
#include <utility>

namespace aegis {

// A light weight error carrier used across the whole code base.
// code == 0 means success, anything else is a failure described by `message`.
class Status {
public:
    constexpr Status() noexcept = default;
    Status(int code, std::string message) : code_(code), message_(std::move(message)) {}

    static Status success() noexcept { return Status{}; }
    static Status error(int code, std::string message) { return Status{code, std::move(message)}; }

    [[nodiscard]] bool ok() const noexcept { return code_ == 0; }
    [[nodiscard]] int code() const noexcept { return code_; }
    [[nodiscard]] const std::string& message() const noexcept { return message_; }

    explicit operator bool() const noexcept { return ok(); }

    std::string to_string() const {
        if (ok()) return "ok";
        return "error(" + std::to_string(code_) + "): " + message_;
    }

private:
    int code_{0};
    std::string message_;
};

// Stable error codes. They are also mapped to HTTP status codes by the gateway.
namespace errc {
inline constexpr int kOk = 0;
inline constexpr int kInvalidArgument = 1;
inline constexpr int kParseError = 2;
inline constexpr int kNotFound = 3;
inline constexpr int kTimeout = 4;
inline constexpr int kIoError = 5;
inline constexpr int kClosed = 6;
inline constexpr int kUpstreamError = 7;
inline constexpr int kRateLimited = 8;
inline constexpr int kConfigError = 9;
inline constexpr int kInternal = 10;
inline constexpr int kTooLarge = 11;
inline constexpr int kShuttingDown = 12;
}  // namespace errc

template <class T>
using Result = std::expected<T, Status>;

// Convenience for functions that only report a Status.
inline Result<void> ok_result() { return {}; }
inline std::unexpected<Status> fail(int code, std::string message) {
    return std::unexpected(Status::error(code, std::move(message)));
}

}  // namespace aegis
