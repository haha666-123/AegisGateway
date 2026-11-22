#include "net/Http.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <cstring>

#include "common/Util.h"

namespace aegis::net {
namespace {

char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool equals_ci(std::string_view lhs, std::string_view rhs) {
    if (lhs.size() != rhs.size()) return false;
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        if (lower(lhs[i]) != lower(rhs[i])) return false;
    }
    return true;
}

bool parse_size(std::string_view text, std::size_t& out) {
    if (text.empty()) return false;
    std::size_t value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') return false;
        const std::size_t digit = static_cast<std::size_t>(c - '0');
        if (value > (static_cast<std::size_t>(-1) - digit) / 10) return false;
        value = value * 10 + digit;
    }
    out = value;
    return true;
}

bool parse_hex(std::string_view text, std::size_t& out) {
    if (text.empty()) return false;
    std::size_t value = 0;
    for (const char c : text) {
        int digit = 0;
        if (c >= '0' && c <= '9') {
            digit = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            digit = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            digit = c - 'A' + 10;
        } else {
            return false;
        }
        if (value > (static_cast<std::size_t>(-1) - static_cast<std::size_t>(digit)) / 16) {
            return false;
        }
        value = value * 16 + static_cast<std::size_t>(digit);
    }
    out = value;
    return true;
}

void append_common_headers(std::string& out, const HeaderMap& headers) {
    for (const auto& [name, value] : headers.items()) {
        out += name;
        out += ": ";
        out += value;
        out += "\r\n";
    }
}

}  // namespace

const char* to_string(Method method) noexcept {
    switch (method) {
        case Method::Get: return "GET";
        case Method::Post: return "POST";
        case Method::Put: return "PUT";
        case Method::Patch: return "PATCH";
        case Method::Delete: return "DELETE";
        case Method::Head: return "HEAD";
        case Method::Options: return "OPTIONS";
        case Method::Unknown: return "UNKNOWN";
    }
    return "UNKNOWN";
}

std::optional<Method> method_from_string(std::string_view text) noexcept {
    if (text == "GET") return Method::Get;
    if (text == "POST") return Method::Post;
    if (text == "PUT") return Method::Put;
    if (text == "PATCH") return Method::Patch;
    if (text == "DELETE") return Method::Delete;
    if (text == "HEAD") return Method::Head;
    if (text == "OPTIONS") return Method::Options;
    return std::nullopt;
}

const char* reason_phrase(int status) noexcept {
    switch (status) {
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 304: return "Not Modified";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 413: return "Payload Too Large";
        case 422: return "Unprocessable Entity";
        case 429: return "Too Many Requests";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        default: return "Unknown";
    }
}

void HeaderMap::set(std::string_view name, std::string_view value) {
    for (auto& [existing_name, existing_value] : items_) {
        if (equals_ci(existing_name, name)) {
            existing_value.assign(value);
            return;
        }
    }
    items_.emplace_back(std::string(name), std::string(value));
}

void HeaderMap::add(std::string_view name, std::string_view value) {
    items_.emplace_back(std::string(name), std::string(value));
}

const std::string* HeaderMap::find(std::string_view name) const {
    for (const auto& [existing_name, existing_value] : items_) {
        if (equals_ci(existing_name, name)) return &existing_value;
    }
    return nullptr;
}

std::string HeaderMap::get(std::string_view name, std::string_view fallback) const {
    const std::string* value = find(name);
    return value == nullptr ? std::string(fallback) : *value;
}

bool HeaderMap::contains(std::string_view name) const { return find(name) != nullptr; }

std::size_t HeaderMap::byte_size() const noexcept {
    std::size_t total = 0;
    for (const auto& [name, value] : items_) total += name.size() + value.size() + 4;
    return total;
}

std::string_view HttpRequest::path() const {
    const auto question = target.find('?');
    return question == std::string::npos ? std::string_view(target)
                                         : std::string_view(target).substr(0, question);
}

std::string_view HttpRequest::query() const {
    const auto question = target.find('?');
    return question == std::string::npos ? std::string_view{}
                                         : std::string_view(target).substr(question + 1);
}

std::string HttpRequest::query_param(std::string_view name) const {
    const auto raw = query();
    for (const auto part : split(raw, '&')) {
        const auto equals = part.find('=');
        const auto key = equals == std::string_view::npos ? part : part.substr(0, equals);
        if (url_decode(key) != name) continue;
        return equals == std::string_view::npos ? std::string{}
                                                : url_decode(part.substr(equals + 1));
    }
    return {};
}

bool HttpRequest::keep_alive() const {
    const std::string* connection = headers.find("Connection");
    if (connection != nullptr) {
        if (equals_ci(*connection, "close")) return false;
        if (equals_ci(*connection, "keep-alive")) return true;
    }
    return version == "HTTP/1.1";
}

HttpResponse HttpResponse::json(int status, std::string body) {
    HttpResponse response;
    response.status = status;
    response.reason = reason_phrase(status);
    response.headers.set("Content-Type", "application/json; charset=utf-8");
    response.body = std::move(body);
    return response;
}

HttpResponse HttpResponse::text(int status, std::string_view body) {
    HttpResponse response;
    response.status = status;
    response.reason = reason_phrase(status);
    response.headers.set("Content-Type", "text/plain; charset=utf-8");
    response.body.assign(body);
    return response;
}

// ------------------------------------------------------------------ request parser

HttpRequestParser::HttpRequestParser(ParseLimits limits) : limits_(limits) {}

HttpRequestParser::State HttpRequestParser::fail(int code, std::string message) {
    state_ = State::Failed;
    error_ = Status::error(code, std::move(message));
    return state_;
}

bool HttpRequestParser::consume_line(std::string_view& line) {
    const auto newline = buffer_.find('\n', cursor_);
    if (newline == std::string::npos) return false;

    std::string_view raw(buffer_.data() + cursor_, newline - cursor_);
    if (!raw.empty() && raw.back() == '\r') raw.remove_suffix(1);
    line = raw;
    cursor_ = newline + 1;
    return true;
}

HttpRequestParser::State HttpRequestParser::feed(std::string_view data) {
    if (state_ == State::Complete || state_ == State::Failed) return state_;
    buffer_.append(data.data(), data.size());

    for (;;) {
        const State before = state_;
        switch (state_) {
            case State::RequestLine: parse_request_line(); break;
            case State::Headers: parse_headers(); break;
            case State::Body: parse_body(); break;
            case State::ChunkSize: parse_chunk_size(); break;
            case State::ChunkData: parse_chunk_data(); break;
            case State::ChunkTrailer: parse_chunk_trailer(); break;
            case State::Complete:
            case State::Failed: return state_;
        }
        if (state_ == before) return state_;
        if (state_ == State::Complete || state_ == State::Failed) return state_;
    }
}

HttpRequestParser::State HttpRequestParser::parse_request_line() {
    std::string_view line;
    if (!consume_line(line)) {
        if (buffer_.size() - cursor_ > limits_.max_header_bytes) {
            return fail(errc::kTooLarge, "request line exceeds header limit");
        }
        return state_;
    }

    const auto first_space = line.find(' ');
    const auto second_space =
        first_space == std::string_view::npos ? std::string_view::npos : line.find(' ', first_space + 1);
    if (first_space == std::string_view::npos || second_space == std::string_view::npos) {
        return fail(errc::kParseError, "malformed request line");
    }

    const auto method_text = line.substr(0, first_space);
    const auto method = method_from_string(method_text);
    if (!method) return fail(errc::kParseError, "unknown HTTP method");

    request_.method = *method;
    request_.target = std::string(line.substr(first_space + 1, second_space - first_space - 1));
    request_.version = std::string(line.substr(second_space + 1));

    if (request_.version != "HTTP/1.1" && request_.version != "HTTP/1.0") {
        return fail(errc::kParseError, "unsupported HTTP version: " + request_.version);
    }
    if (request_.target.empty() || request_.target.front() != '/') {
        return fail(errc::kParseError, "request target must be an absolute path");
    }

    state_ = State::Headers;
    return state_;
}

HttpRequestParser::State HttpRequestParser::parse_headers() {
    for (;;) {
        if (cursor_ > limits_.max_header_bytes) {
            return fail(errc::kTooLarge, "header section exceeds header limit");
        }

        std::string_view line;
        if (!consume_line(line)) {
            if (buffer_.size() - cursor_ > limits_.max_header_bytes) {
                return fail(errc::kTooLarge, "header section exceeds header limit");
            }
            return state_;
        }

        if (line.empty()) {
            headers_done_ = true;

            const std::string* transfer_encoding = request_.headers.find("Transfer-Encoding");
            const std::string* content_length = request_.headers.find("Content-Length");

            if (transfer_encoding != nullptr && equals_ci(*transfer_encoding, "chunked")) {
                framing_ = Framing::Chunked;
                state_ = State::ChunkSize;
            } else if (transfer_encoding != nullptr) {
                return fail(errc::kParseError, "unsupported transfer encoding");
            } else if (content_length != nullptr) {
                if (!parse_size(trim(*content_length), content_length_)) {
                    return fail(errc::kParseError, "invalid Content-Length");
                }
                if (content_length_ > limits_.max_body_bytes) {
                    return fail(errc::kTooLarge, "body exceeds limit");
                }
                framing_ = Framing::ContentLength;
                state_ = content_length_ == 0 ? State::Complete : State::Body;
            } else {
                framing_ = Framing::None;
                state_ = State::Complete;
            }
            return state_;
        }

        if (line.front() == ' ' || line.front() == '\t') {
            return fail(errc::kParseError, "obsolete header line folding is rejected");
        }

        const auto colon = line.find(':');
        if (colon == std::string_view::npos || colon == 0) {
            return fail(errc::kParseError, "malformed header line");
        }
        const auto name = trim(line.substr(0, colon));
        const auto value = trim(line.substr(colon + 1));
        if (name.empty()) return fail(errc::kParseError, "empty header name");
        request_.headers.add(name, value);
    }
}

HttpRequestParser::State HttpRequestParser::parse_body() {
    const std::size_t available = buffer_.size() - cursor_;
    if (available == 0) return state_;

    const std::size_t remaining = content_length_ - request_.body.size();
    const std::size_t take = std::min(available, remaining);
    request_.body.append(buffer_.data() + cursor_, take);
    cursor_ += take;

    if (request_.body.size() >= content_length_) state_ = State::Complete;
    return state_;
}

HttpRequestParser::State HttpRequestParser::parse_chunk_size() {
    std::string_view line;
    if (!consume_line(line)) {
        if (buffer_.size() - cursor_ > limits_.max_header_bytes) {
            return fail(errc::kTooLarge, "chunk header too long");
        }
        return state_;
    }

    const auto semicolon = line.find(';');
    if (semicolon != std::string_view::npos) line = line.substr(0, semicolon);

    if (!parse_hex(trim(line), chunk_remaining_)) {
        return fail(errc::kParseError, "invalid chunk size");
    }
    if (chunk_remaining_ == 0) {
        state_ = State::ChunkTrailer;
    } else {
        if (request_.body.size() + chunk_remaining_ > limits_.max_body_bytes) {
            return fail(errc::kTooLarge, "chunked body exceeds limit");
        }
        state_ = State::ChunkData;
    }
    return state_;
}

HttpRequestParser::State HttpRequestParser::parse_chunk_data() {
    const std::size_t available = buffer_.size() - cursor_;
    if (available == 0) return state_;

    if (chunk_remaining_ > 0) {
        const std::size_t take = std::min(available, chunk_remaining_);
        request_.body.append(buffer_.data() + cursor_, take);
        cursor_ += take;
        chunk_remaining_ -= take;
        if (chunk_remaining_ > 0) return state_;
    }

    // A chunk is followed by CRLF.
    if (buffer_.size() - cursor_ < 2) return state_;
    if (buffer_[cursor_] != '\r' || buffer_[cursor_ + 1] != '\n') {
        return fail(errc::kParseError, "missing CRLF after chunk");
    }
    cursor_ += 2;
    state_ = State::ChunkSize;
    return state_;
}

HttpRequestParser::State HttpRequestParser::parse_chunk_trailer() {
    std::string_view line;
    if (!consume_line(line)) return state_;
    if (line.empty()) {
        state_ = State::Complete;
        return state_;
    }
    // Trailers are accepted and ignored.
    return state_;
}

void HttpRequestParser::reset() {
    buffer_.erase(0, cursor_);
    cursor_ = 0;
    request_ = HttpRequest{};
    error_ = Status{};
    state_ = State::RequestLine;
    framing_ = Framing::None;
    content_length_ = 0;
    chunk_remaining_ = 0;
    headers_done_ = false;

    // A pipelined request may already be buffered.
    if (!buffer_.empty()) {
        feed({});
    }
}

// ------------------------------------------------------------------ response parser

HttpResponseParser::HttpResponseParser(ParseLimits limits) : limits_(limits) {}

HttpResponseParser::State HttpResponseParser::fail(int code, std::string message) {
    state_ = State::Failed;
    error_ = Status::error(code, std::move(message));
    return state_;
}

HttpResponseParser::State HttpResponseParser::feed(std::string_view data, const BodySink& on_body) {
    if (state_ == State::Complete || state_ == State::Failed) return state_;
    if (!data.empty()) saw_any_bytes_ = true;
    buffer_.append(data.data(), data.size());

    for (;;) {
        const State before = state_;
        switch (state_) {
            case State::StatusLine: parse_status_line(); break;
            case State::Headers: parse_headers(); break;
            case State::Body: dispatch_body(on_body); break;
            case State::ChunkSize: {
                // Reuse the request chunk grammar through a tiny inline state.
                std::string_view line;
                const auto newline = buffer_.find('\n', cursor_);
                if (newline == std::string_view::npos) return state_;
                std::string_view raw(buffer_.data() + cursor_, newline - cursor_);
                if (!raw.empty() && raw.back() == '\r') raw.remove_suffix(1);
                cursor_ = newline + 1;
                const auto semicolon = raw.find(';');
                if (semicolon != std::string_view::npos) raw = raw.substr(0, semicolon);
                if (!parse_hex(trim(raw), chunk_remaining_)) {
                    return fail(errc::kParseError, "invalid response chunk size");
                }
                state_ = chunk_remaining_ == 0 ? State::ChunkTrailer : State::ChunkData;
                break;
            }
            case State::ChunkData: {
                const std::size_t available = buffer_.size() - cursor_;
                if (available == 0) return state_;
                const std::size_t take = std::min(available, chunk_remaining_);
                if (take > 0) {
                    if (on_body) on_body(std::string_view(buffer_.data() + cursor_, take));
                    body_bytes_ += take;
                    cursor_ += take;
                    chunk_remaining_ -= take;
                }
                if (chunk_remaining_ > 0) return state_;
                if (buffer_.size() - cursor_ < 2) return state_;
                if (buffer_[cursor_] != '\r' || buffer_[cursor_ + 1] != '\n') {
                    return fail(errc::kParseError, "missing CRLF after response chunk");
                }
                cursor_ += 2;
                state_ = State::ChunkSize;
                break;
            }
            case State::ChunkTrailer: {
                std::string_view line;
                const auto newline = buffer_.find('\n', cursor_);
                if (newline == std::string_view::npos) return state_;
                std::string_view raw(buffer_.data() + cursor_, newline - cursor_);
                cursor_ = newline + 1;
                if (!raw.empty() && raw.back() == '\r') raw.remove_suffix(1);
                if (raw.empty()) state_ = State::Complete;
                break;
            }
            case State::Complete:
            case State::Failed: return state_;
        }

        if (state_ == before) return state_;
        if (state_ == State::Complete || state_ == State::Failed) return state_;
        // Drop consumed bytes eagerly so a long SSE stream does not grow the
        // buffer without bound.
        if (cursor_ > 64 * 1024) {
            buffer_.erase(0, cursor_);
            cursor_ = 0;
        }
    }
}

HttpResponseParser::State HttpResponseParser::parse_status_line() {
    const auto newline = buffer_.find('\n', cursor_);
    if (newline == std::string_view::npos) {
        if (buffer_.size() - cursor_ > limits_.max_header_bytes) {
            return fail(errc::kTooLarge, "status line too long");
        }
        return state_;
    }

    std::string_view line(buffer_.data() + cursor_, newline - cursor_);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    cursor_ = newline + 1;

    if (!line.starts_with("HTTP/1.")) return fail(errc::kParseError, "malformed status line");
    const auto first_space = line.find(' ');
    if (first_space == std::string_view::npos) {
        return fail(errc::kParseError, "malformed status line");
    }
    const auto rest = line.substr(first_space + 1);
    const auto second_space = rest.find(' ');

    std::size_t status = 0;
    const auto code_text = second_space == std::string_view::npos ? rest : rest.substr(0, second_space);
    if (!parse_size(code_text, status) || status < 100 || status > 599) {
        return fail(errc::kParseError, "invalid status code");
    }
    status_ = static_cast<int>(status);
    reason_ = second_space == std::string_view::npos
                  ? std::string(reason_phrase(status_))
                  : std::string(rest.substr(second_space + 1));

    state_ = State::Headers;
    return state_;
}

HttpResponseParser::State HttpResponseParser::parse_headers() {
    for (;;) {
        if (cursor_ > limits_.max_header_bytes) {
            return fail(errc::kTooLarge, "response header section too large");
        }

        const auto newline = buffer_.find('\n', cursor_);
        if (newline == std::string_view::npos) {
            if (buffer_.size() - cursor_ > limits_.max_header_bytes) {
                return fail(errc::kTooLarge, "response header section too large");
            }
            return state_;
        }

        std::string_view line(buffer_.data() + cursor_, newline - cursor_);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        cursor_ = newline + 1;

        if (line.empty()) {
            headers_done_ = true;
            const std::string* transfer_encoding = headers_.find("Transfer-Encoding");
            const std::string* content_length = headers_.find("Content-Length");

            if (status_ == 204 || status_ == 304) {
                state_ = State::Complete;
            } else if (transfer_encoding != nullptr && equals_ci(*transfer_encoding, "chunked")) {
                framing_ = Framing::Chunked;
                state_ = State::ChunkSize;
            } else if (content_length != nullptr) {
                if (!parse_size(trim(*content_length), content_length_)) {
                    return fail(errc::kParseError, "invalid Content-Length in response");
                }
                framing_ = Framing::ContentLength;
                state_ = content_length_ == 0 ? State::Complete : State::Body;
            } else {
                framing_ = Framing::CloseDelimited;
                state_ = State::Body;
            }
            return state_;
        }

        const auto colon = line.find(':');
        if (colon == std::string_view::npos || colon == 0) {
            return fail(errc::kParseError, "malformed response header");
        }
        headers_.add(trim(line.substr(0, colon)), trim(line.substr(colon + 1)));
    }
}

HttpResponseParser::State HttpResponseParser::dispatch_body(const BodySink& on_body) {
    const std::size_t available = buffer_.size() - cursor_;
    if (available == 0) return state_;

    if (framing_ == Framing::CloseDelimited) {
        if (on_body) on_body(std::string_view(buffer_.data() + cursor_, available));
        body_bytes_ += available;
        cursor_ += available;
        return state_;  // stays in Body until finish_eof()
    }

    const std::size_t remaining = content_length_ - body_bytes_;
    const std::size_t take = std::min(available, remaining);
    if (take > 0) {
        if (on_body) on_body(std::string_view(buffer_.data() + cursor_, take));
        body_bytes_ += take;
        cursor_ += take;
    }
    if (body_bytes_ >= content_length_) state_ = State::Complete;
    return state_;
}

HttpResponseParser::State HttpResponseParser::finish_eof() {
    if (state_ == State::Complete || state_ == State::Failed) return state_;
    if (state_ == State::Body && framing_ == Framing::CloseDelimited) {
        state_ = State::Complete;
        return state_;
    }
    if (!saw_any_bytes_) return fail(errc::kClosed, "connection closed before a response arrived");
    return fail(errc::kIoError, "connection closed before the response body was complete");
}

void HttpResponseParser::reset() {
    buffer_.erase(0, cursor_);
    cursor_ = 0;
    state_ = State::StatusLine;
    headers_ = HeaderMap{};
    error_ = Status{};
    status_ = 0;
    reason_.clear();
    framing_ = Framing::None;
    content_length_ = 0;
    body_bytes_ = 0;
    chunk_remaining_ = 0;
    headers_done_ = false;
    saw_any_bytes_ = false;
}

// ------------------------------------------------------------------ serialisation

std::string serialize_request(const HttpRequest& request) {
    std::string out;
    out.reserve(256 + request.body.size());
    out += to_string(request.method);
    out += ' ';
    out += request.target;
    out += ' ';
    out += request.version.empty() ? "HTTP/1.1" : request.version;
    out += "\r\n";
    append_common_headers(out, request.headers);

    if (!request.body.empty() && !request.headers.contains("Content-Length") &&
        !request.headers.contains("Transfer-Encoding")) {
        out += "Content-Length: ";
        out += std::to_string(request.body.size());
        out += "\r\n";
    }
    if (!request.headers.contains("Connection")) {
        out += "Connection: keep-alive\r\n";
    }
    out += "\r\n";
    out += request.body;
    return out;
}

std::string serialize_response_head(const HttpResponse& response) {
    std::string out;
    out.reserve(256);
    out += "HTTP/1.1 ";
    out += std::to_string(response.status);
    out += ' ';
    out += response.reason.empty() ? reason_phrase(response.status) : response.reason;
    out += "\r\n";
    append_common_headers(out, response.headers);
    if (!response.headers.contains("Connection")) {
        out += "Connection: keep-alive\r\n";
    }
    out += "\r\n";
    return out;
}

std::string serialize_response(const HttpResponse& response) {
    HttpResponse copy = response;
    if (!copy.headers.contains("Content-Length") && !copy.headers.contains("Transfer-Encoding")) {
        copy.headers.set("Content-Length", std::to_string(copy.body.size()));
    }
    if (copy.reason.empty()) copy.reason = reason_phrase(copy.status);

    std::string out = serialize_response_head(copy);
    out += copy.body;
    return out;
}

std::string chunk_encode(std::string_view data) {
    std::string out;
    char header[24];
    const int written = std::snprintf(header, sizeof(header), "%zx\r\n", data.size());
    if (written > 0) out.append(header, static_cast<std::size_t>(written));
    out.append(data.data(), data.size());
    out += "\r\n";
    return out;
}

std::string_view chunk_terminator() noexcept { return "0\r\n\r\n"; }

std::string url_decode(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '+') {
            out.push_back(' ');
        } else if (c == '%' && i + 2 < text.size()) {
            const auto hex_value = [](char digit) -> int {
                if (digit >= '0' && digit <= '9') return digit - '0';
                if (digit >= 'a' && digit <= 'f') return digit - 'a' + 10;
                if (digit >= 'A' && digit <= 'F') return digit - 'A' + 10;
                return -1;
            };
            const int high = hex_value(text[i + 1]);
            const int low = hex_value(text[i + 2]);
            if (high >= 0 && low >= 0) {
                out.push_back(static_cast<char>((high << 4) | low));
                i += 2;
            } else {
                out.push_back(c);
            }
        } else {
            out.push_back(c);
        }
    }
    return out;
}

std::string url_encode(std::string_view text) {
    constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        const auto uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc) != 0 || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(c);
        } else {
            out.push_back('%');
            out.push_back(kHex[uc >> 4]);
            out.push_back(kHex[uc & 0x0F]);
        }
    }
    return out;
}

}  // namespace aegis::net
