#include <string>
#include <string_view>
#include <vector>

#include "TestFramework.h"
#include "net/Http.h"

using namespace aegis;
using namespace aegis::net;

namespace {

HttpRequestParser make_parser() {
    HttpRequestParser parser;
    return parser;
}

std::string sample_post() {
    return "POST /v1/chat/completions?trace=1 HTTP/1.1\r\n"
           "Host: 127.0.0.1:8080\r\n"
           "Content-Type: application/json\r\n"
           "Content-Length: 11\r\n"
           "Connection: keep-alive\r\n"
           "\r\n"
           "{\"a\": true}ignored";
}

}  // namespace

AEGIS_TEST(http_method_helpers) {
    AEGIS_CHECK_EQ(std::string(to_string(Method::Get)), std::string("GET"));
    AEGIS_CHECK_EQ(std::string(to_string(Method::Patch)), std::string("PATCH"));
    AEGIS_CHECK(method_from_string("POST").has_value());
    AEGIS_CHECK(method_from_string("post") == std::nullopt);
    AEGIS_CHECK(method_from_string("BREW") == std::nullopt);
    AEGIS_CHECK_EQ(std::string(reason_phrase(429)), std::string("Too Many Requests"));
    AEGIS_CHECK_EQ(std::string(reason_phrase(599)), std::string("Unknown"));
}

AEGIS_TEST(http_request_line_and_query) {
    auto parser = make_parser();
    const auto state = parser.feed(
        "GET /v1/models?limit=10&after=abc%20def HTTP/1.1\r\nHost: local\r\n\r\n");
    AEGIS_CHECK(state == HttpRequestParser::State::Complete);
    const auto& request = parser.request();
    AEGIS_CHECK(request.method == Method::Get);
    AEGIS_CHECK_EQ(std::string(request.path()), std::string("/v1/models"));
    AEGIS_CHECK_EQ(std::string(request.query()), std::string("limit=10&after=abc%20def"));
    AEGIS_CHECK_EQ(request.query_param("limit"), std::string("10"));
    AEGIS_CHECK_EQ(request.query_param("after"), std::string("abc def"));
    AEGIS_CHECK_EQ(request.query_param("missing"), std::string(""));
    AEGIS_CHECK(request.keep_alive());
    AEGIS_CHECK_EQ(std::string(request.headers.get("host")), std::string("local"));
}

AEGIS_TEST(http_request_split_every_byte) {
    const std::string raw = sample_post();
    for (std::size_t split = 1; split < raw.size(); ++split) {
        HttpRequestParser parser;
        const auto first = raw.substr(0, split);
        const auto second = raw.substr(split);
        auto state = parser.feed(first);
        if (state != HttpRequestParser::State::Complete) state = parser.feed(second);
        AEGIS_CHECK(state == HttpRequestParser::State::Complete);
        AEGIS_CHECK_EQ(parser.request().body, std::string("{\"a\": true}"));
    }
}

AEGIS_TEST(http_request_body_with_content_length) {
    auto parser = make_parser();
    const auto state = parser.feed(sample_post());
    AEGIS_CHECK(state == HttpRequestParser::State::Complete);
    AEGIS_CHECK_EQ(parser.request().method == Method::Post, true);
    AEGIS_CHECK_EQ(parser.request().body.size(), std::size_t{11});
    AEGIS_CHECK_EQ(parser.remaining_buffered(), std::string("ignored").size());
}

AEGIS_TEST(http_request_chunked_body) {
    auto parser = make_parser();
    const std::string raw =
        "POST /upload HTTP/1.1\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "5\r\nhello\r\n"
        "7;ext=1\r\n, world\r\n"
        "0\r\n\r\n";
    const auto state = parser.feed(raw);
    AEGIS_CHECK(state == HttpRequestParser::State::Complete);
    AEGIS_CHECK_EQ(parser.request().body, std::string("hello, world"));
}

AEGIS_TEST(http_request_chunked_split_mid_chunk) {
    const std::string raw =
        "POST /upload HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n"
        "A\r\n0123456789\r\n0\r\n\r\n";
    for (std::size_t split = 1; split < raw.size(); ++split) {
        HttpRequestParser parser;
        parser.feed(std::string_view(raw).substr(0, split));
        parser.feed(std::string_view(raw).substr(split));
        AEGIS_CHECK(parser.complete());
        AEGIS_CHECK_EQ(parser.request().body, std::string("0123456789"));
    }
}

AEGIS_TEST(http_request_limits) {
    ParseLimits limits;
    limits.max_body_bytes = 8;
    HttpRequestParser parser(limits);
    const auto state = parser.feed(
        "POST /x HTTP/1.1\r\nContent-Length: 9\r\n\r\n012345678");
    AEGIS_CHECK(state == HttpRequestParser::State::Failed);
    AEGIS_CHECK_EQ(parser.error().code(), errc::kTooLarge);

    ParseLimits header_limits;
    header_limits.max_header_bytes = 32;
    HttpRequestParser header_parser(header_limits);
    std::string big_header = "GET / HTTP/1.1\r\nX-Long: ";
    big_header.append(200, 'a');
    big_header += "\r\n\r\n";
    const auto header_state = header_parser.feed(big_header);
    AEGIS_CHECK(header_state == HttpRequestParser::State::Failed);
    AEGIS_CHECK_EQ(header_parser.error().code(), errc::kTooLarge);
}

AEGIS_TEST(http_request_rejects_malformed) {
    const std::vector<std::string> bad_requests{
        "GET\r\n\r\n",
        "GET / HTTP/9.9\r\n\r\n",
        "GET / HTTP/1.1\r\nBadHeader\r\n\r\n",
        "GET relative HTTP/1.1\r\n\r\n",
        "GET / HTTP/1.1\r\n Folded: yes\r\n\r\n",
        "GET / HTTP/1.1\r\nContent-Length: abc\r\n\r\n",
        "POST / HTTP/1.1\r\nTransfer-Encoding: gzip\r\n\r\n",
        "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\nZZ\r\n",
        "BREW / HTTP/1.1\r\n\r\n",
    };
    for (const auto& raw : bad_requests) {
        HttpRequestParser parser;
        const auto state = parser.feed(raw);
        AEGIS_CHECK(state == HttpRequestParser::State::Failed);
        AEGIS_CHECK(!parser.error().ok());
    }
}

AEGIS_TEST(http_request_pipelining) {
    HttpRequestParser parser;
    const std::string raw =
        "GET /one HTTP/1.1\r\n\r\n"
        "GET /two HTTP/1.1\r\n\r\n";
    AEGIS_CHECK(parser.feed(raw) == HttpRequestParser::State::Complete);
    AEGIS_CHECK_EQ(std::string(parser.request().path()), std::string("/one"));

    parser.reset();
    AEGIS_CHECK(parser.complete());
    AEGIS_CHECK_EQ(std::string(parser.request().path()), std::string("/two"));
    AEGIS_CHECK_EQ(parser.remaining_buffered(), std::size_t{0});
}

AEGIS_TEST(http_request_keep_alive_rules) {
    HttpRequestParser parser;
    parser.feed("GET / HTTP/1.0\r\n\r\n");
    AEGIS_CHECK(parser.complete());
    AEGIS_CHECK(!parser.request().keep_alive());

    HttpRequestParser close_parser;
    close_parser.feed("GET / HTTP/1.1\r\nConnection: close\r\n\r\n");
    AEGIS_CHECK(!close_parser.request().keep_alive());
}

AEGIS_TEST(http_response_content_length) {
    HttpResponseParser parser;
    std::string body;
    const auto sink = [&body](std::string_view chunk) { body.append(chunk); };
    const auto state = parser.feed(
        "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 5\r\n\r\nhello", sink);
    AEGIS_CHECK(state == HttpResponseParser::State::Complete);
    AEGIS_CHECK_EQ(parser.status(), 200);
    AEGIS_CHECK_EQ(std::string(parser.reason()), std::string("OK"));
    AEGIS_CHECK_EQ(body, std::string("hello"));
    AEGIS_CHECK_EQ(parser.body_bytes(), std::size_t{5});
}

AEGIS_TEST(http_response_chunked_streaming) {
    HttpResponseParser parser;
    std::vector<std::string> chunks;
    const auto sink = [&chunks](std::string_view chunk) { chunks.emplace_back(chunk); };
    const std::string raw =
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Type: text/event-stream\r\n\r\n"
        "6\r\ndata: \r\n"
        "5\r\nhello\r\n"
        "0\r\n\r\n";
    const auto state = parser.feed(raw, sink);
    AEGIS_CHECK(state == HttpResponseParser::State::Complete);
    AEGIS_CHECK(chunks.size() >= 2);
    std::string joined;
    for (const auto& chunk : chunks) joined += chunk;
    AEGIS_CHECK_EQ(joined, std::string("data: hello"));
}

AEGIS_TEST(http_response_close_delimited) {
    HttpResponseParser parser;
    std::string body;
    const auto sink = [&body](std::string_view chunk) { body.append(chunk); };
    parser.feed("HTTP/1.1 200 OK\r\nConnection: close\r\n\r\npart1", sink);
    AEGIS_CHECK(!parser.complete());
    parser.feed("-part2", sink);
    AEGIS_CHECK(parser.finish_eof() == HttpResponseParser::State::Complete);
    AEGIS_CHECK_EQ(body, std::string("part1-part2"));
}

AEGIS_TEST(http_response_no_body_status) {
    HttpResponseParser parser;
    const auto sink = [](std::string_view) {};
    const auto state = parser.feed("HTTP/1.1 204 No Content\r\nX: y\r\n\r\n", sink);
    AEGIS_CHECK(state == HttpResponseParser::State::Complete);
    AEGIS_CHECK_EQ(parser.body_bytes(), std::size_t{0});
}

AEGIS_TEST(http_response_truncated_body_fails) {
    HttpResponseParser parser;
    const auto sink = [](std::string_view) {};
    parser.feed("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nshort", sink);
    AEGIS_CHECK(parser.finish_eof() == HttpResponseParser::State::Failed);
    AEGIS_CHECK_EQ(parser.error().code(), errc::kIoError);
}

AEGIS_TEST(http_serialize_roundtrip) {
    HttpResponse response = HttpResponse::json(200, R"({"ok":true})");
    const std::string text = serialize_response(response);
    AEGIS_CHECK(text.starts_with("HTTP/1.1 200 OK\r\n"));
    AEGIS_CHECK(text.find("Content-Length: 11\r\n") != std::string::npos);
    AEGIS_CHECK(text.ends_with(R"({"ok":true})"));

    HttpResponseParser parser;
    std::string body;
    parser.feed(text, [&body](std::string_view chunk) { body.append(chunk); });
    AEGIS_CHECK(parser.complete());
    AEGIS_CHECK_EQ(parser.status(), 200);
    AEGIS_CHECK_EQ(body, std::string(R"({"ok":true})"));

    HttpRequest request;
    request.method = Method::Post;
    request.target = "/v1/chat/completions";
    request.body = R"({"model":"x"})";
    request.headers.set("Content-Type", "application/json");
    const std::string request_text = serialize_request(request);
    AEGIS_CHECK(request_text.starts_with("POST /v1/chat/completions HTTP/1.1\r\n"));
    AEGIS_CHECK(request_text.find("Content-Length: 13\r\n") != std::string::npos);
}

AEGIS_TEST(http_chunk_encoding) {
    AEGIS_CHECK_EQ(chunk_encode("hello"), std::string("5\r\nhello\r\n"));
    AEGIS_CHECK_EQ(chunk_encode(""), std::string("0\r\n\r\n"));
    AEGIS_CHECK_EQ(std::string(chunk_terminator()), std::string("0\r\n\r\n"));

    const std::string head = serialize_response_head(HttpResponse::json(200, ""));
    AEGIS_CHECK(head.find("Content-Length") == std::string::npos);
    AEGIS_CHECK(head.ends_with("\r\n\r\n"));
}

AEGIS_TEST(http_url_codec) {
    AEGIS_CHECK_EQ(url_decode("a%20b"), std::string("a b"));
    AEGIS_CHECK_EQ(url_decode("a+b"), std::string("a b"));
    AEGIS_CHECK_EQ(url_decode("100%"), std::string("100%"));
    AEGIS_CHECK_EQ(url_decode("%zz"), std::string("%zz"));
    AEGIS_CHECK_EQ(url_encode("a b/c"), std::string("a%20b%2Fc"));
    AEGIS_CHECK_EQ(url_encode("safe-._~"), std::string("safe-._~"));
}
