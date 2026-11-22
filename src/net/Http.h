#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/Status.h"

namespace aegis::net {

enum class Method { Get, Post, Put, Patch, Delete, Head, Options, Unknown };

[[nodiscard]] const char* to_string(Method method) noexcept;
[[nodiscard]] std::optional<Method> method_from_string(std::string_view text) noexcept;

struct ParseLimits {
    std::size_t max_header_bytes{16U * 1024U};
    std::size_t max_body_bytes{4U * 1024U * 1024U};
};

class HeaderMap {
public:
    void set(std::string_view name, std::string_view value);
    void add(std::string_view name, std::string_view value);
    [[nodiscard]] const std::string* find(std::string_view name) const;
    [[nodiscard]] std::string get(std::string_view name, std::string_view fallback = {}) const;
    [[nodiscard]] bool contains(std::string_view name) const;
    [[nodiscard]] std::size_t size() const noexcept { return items_.size(); }
    [[nodiscard]] bool empty() const noexcept { return items_.empty(); }
    [[nodiscard]] const std::vector<std::pair<std::string, std::string>>& items() const noexcept {
        return items_;
    }
    [[nodiscard]] std::size_t byte_size() const noexcept;
    void clear() { items_.clear(); }

private:
    std::vector<std::pair<std::string, std::string>> items_;
};

struct HttpRequest {
    Method method{Method::Get};
    std::string target{"/"};
    std::string version{"HTTP/1.1"};
    HeaderMap headers;
    std::string body;

    [[nodiscard]] std::string_view path() const;
    [[nodiscard]] std::string_view query() const;
    [[nodiscard]] std::string query_param(std::string_view name) const;
    [[nodiscard]] bool keep_alive() const;
};

struct HttpResponse {
    int status{200};
    std::string reason{"OK"};
    HeaderMap headers;
    std::string body;

    static HttpResponse json(int status, std::string body);
    static HttpResponse text(int status, std::string_view body);
};

[[nodiscard]] const char* reason_phrase(int status) noexcept;

// ---------------------------------------------------------------- request side
//
// Incremental HTTP/1.1 request parser. `feed()` may be called with arbitrary
// fragments (the tests split a request at every byte boundary); the parser keeps
// its own state and returns Complete as soon as one full request has arrived.
// Bytes belonging to a pipelined next request stay buffered and are parsed after
// `reset()`.
class HttpRequestParser {
public:
    enum class State { RequestLine, Headers, Body, ChunkSize, ChunkData, ChunkTrailer, Complete, Failed };

    explicit HttpRequestParser(ParseLimits limits = {});

    State feed(std::string_view data);

    [[nodiscard]] State state() const noexcept { return state_; }
    [[nodiscard]] bool complete() const noexcept { return state_ == State::Complete; }
    [[nodiscard]] bool failed() const noexcept { return state_ == State::Failed; }
    [[nodiscard]] const HttpRequest& request() const noexcept { return request_; }
    [[nodiscard]] const Status& error() const noexcept { return error_; }
    [[nodiscard]] std::size_t remaining_buffered() const noexcept {
        return buffer_.size() - cursor_;
    }

    void reset();
    void set_limits(ParseLimits limits) { limits_ = limits; }

private:
    enum class Framing { None, ContentLength, Chunked };
    State fail(int code, std::string message);
    State parse_request_line();
    State parse_headers();
    State parse_body();
    State parse_chunk_size();
    State parse_chunk_data();
    State parse_chunk_trailer();
    bool consume_line(std::string_view& line);
    [[nodiscard]] bool has_line() const;

    ParseLimits limits_;
    State state_{State::RequestLine};
    HttpRequest request_;
    Status error_;
    std::string buffer_;
    std::size_t cursor_{0};

    Framing framing_{Framing::None};
    std::size_t content_length_{0};
    std::size_t chunk_remaining_{0};
    bool headers_done_{false};
};

// ---------------------------------------------------------------- response side
//
// Response parser that streams the body instead of buffering it, which is what
// a gateway needs for SSE/streaming completions: each fragment is handed to the
// sink as soon as it is decoded, so tokens reach the client with minimal delay.
class HttpResponseParser {
public:
    enum class State { StatusLine, Headers, Body, ChunkSize, ChunkData, ChunkTrailer, Complete, Failed };

    using BodySink = std::function<void(std::string_view)>;

    explicit HttpResponseParser(ParseLimits limits = {});

    State feed(std::string_view data, const BodySink& on_body);

    // Call when the peer closed the connection: completes close delimited
    // bodies and fails truncated ones.
    State finish_eof();

    [[nodiscard]] State state() const noexcept { return state_; }
    [[nodiscard]] bool complete() const noexcept { return state_ == State::Complete; }
    [[nodiscard]] bool failed() const noexcept { return state_ == State::Failed; }
    [[nodiscard]] int status() const noexcept { return status_; }
    [[nodiscard]] const std::string& reason() const noexcept { return reason_; }
    [[nodiscard]] const HeaderMap& headers() const noexcept { return headers_; }
    [[nodiscard]] const Status& error() const noexcept { return error_; }
    [[nodiscard]] std::size_t body_bytes() const noexcept { return body_bytes_; }
    [[nodiscard]] bool headers_complete() const noexcept { return headers_done_; }

    void reset();

private:
    enum class Framing { None, ContentLength, Chunked, CloseDelimited };
    State fail(int code, std::string message);
    State parse_status_line();
    State parse_headers();
    State dispatch_body(const BodySink& on_body);

    ParseLimits limits_;
    State state_{State::StatusLine};
    std::string buffer_;
    std::size_t cursor_{0};
    HeaderMap headers_;
    Status error_;
    int status_{0};
    std::string reason_;
    Framing framing_{Framing::None};
    std::size_t content_length_{0};
    std::size_t body_bytes_{0};
    std::size_t chunk_remaining_{0};
    bool headers_done_{false};
    bool saw_any_bytes_{false};
};

// ---------------------------------------------------------------- serialisation
[[nodiscard]] std::string serialize_request(const HttpRequest& request);
[[nodiscard]] std::string serialize_response(const HttpResponse& response);
// Headers only, for responses whose body is streamed (chunked transfer encoding).
[[nodiscard]] std::string serialize_response_head(const HttpResponse& response);
[[nodiscard]] std::string chunk_encode(std::string_view data);
[[nodiscard]] std::string_view chunk_terminator() noexcept;

[[nodiscard]] std::string url_decode(std::string_view text);
[[nodiscard]] std::string url_encode(std::string_view text);

}  // namespace aegis::net
