#include "ai/OpenAiClient.h"

#include <memory>
#include <utility>

#include "common/Json.h"
#include "common/Log.h"
#include "common/Metrics.h"

namespace aegis::ai {
namespace {

struct ParsedUrl {
    std::string host;
    std::uint16_t port{80};
    std::string prefix;
};

Result<ParsedUrl> parse_http_url(std::string_view url) {
    if (url.starts_with("https://")) {
        return fail(errc::kInvalidArgument,
                    "https upstreams are not supported: terminate TLS in front of the gateway");
    }
    if (!url.starts_with("http://")) {
        return fail(errc::kInvalidArgument, "upstream base_url must start with http://");
    }

    std::string_view rest = url.substr(7);
    const auto slash = rest.find('/');
    const std::string_view authority = slash == std::string_view::npos ? rest : rest.substr(0, slash);
    std::string prefix = slash == std::string_view::npos ? std::string{}
                                                         : std::string(rest.substr(slash));
    while (!prefix.empty() && prefix.back() == '/') prefix.pop_back();

    if (authority.empty()) {
        return fail(errc::kInvalidArgument, "upstream base_url has an empty host");
    }

    ParsedUrl parsed;
    parsed.host.assign(authority);
    const auto colon = authority.rfind(':');
    if (colon != std::string_view::npos && authority.find(':') == colon) {
        parsed.host.assign(authority.substr(0, colon));
        try {
            const int port = std::stoi(std::string(authority.substr(colon + 1)));
            if (port <= 0 || port > 65535) {
                return fail(errc::kInvalidArgument, "upstream port out of range");
            }
            parsed.port = static_cast<std::uint16_t>(port);
        } catch (...) {
            return fail(errc::kInvalidArgument, "upstream port is not a number");
        }
    }
    parsed.prefix = std::move(prefix);
    return parsed;
}

std::string error_message_from(const std::string& raw) {
    const auto parsed = parse_json(raw);
    if (parsed && parsed->contains("error")) {
        const auto& error = (*parsed)["error"];
        if (error.is_object() && error["message"].is_string()) {
            return std::string(error["message"].as_string());
        }
        if (error.is_string()) return std::string(error.as_string());
    }
    constexpr std::size_t kMaxSnippet = 200;
    return raw.size() > kMaxSnippet ? raw.substr(0, kMaxSnippet) : raw;
}

}  // namespace

struct OpenAiClient::State {
    bool stream{false};
    int http_status{0};
    std::string raw_body;
    std::string content;
    std::string id;
    std::string model;
    Usage usage;
    SseParser sse;
    DeltaHandler on_delta;
    RawHandler on_raw;
    std::uint64_t request_id{0};
};

OpenAiClient::OpenAiClient(net::HttpClient& http, std::string base_url, std::string api_key)
    : http_(http), base_url_(std::move(base_url)), api_key_(std::move(api_key)) {
    auto parsed = parse_http_url(base_url_);
    if (!parsed) {
        url_error_ = parsed.error();
        return;
    }
    host_ = parsed->host;
    port_ = parsed->port;
    prefix_ = parsed->prefix;
}

void OpenAiClient::handle_event(State& state, const SseEvent& event) {
    if (event.data == "[DONE]") return;

    const auto parsed = parse_json(event.data);
    if (!parsed) {
        AEGIS_DEBUG("upstream sent a non JSON SSE frame: {}", sanitize_for_log(event.data));
        return;
    }
    const Json& document = *parsed;

    if (document["id"].is_string()) state.id = std::string(document["id"].as_string());
    if (document["model"].is_string()) state.model = std::string(document["model"].as_string());

    const Json& choices = document["choices"];
    if (choices.is_array() && choices.size() > 0) {
        const Json& delta = choices[0]["delta"];
        if (delta.is_object() && delta["content"].is_string()) {
            const std::string_view piece = delta["content"].as_string();
            if (!piece.empty()) {
                state.content.append(piece);
                if (state.on_delta) state.on_delta(piece);
            }
        }
    }

    const Json& usage = document["usage"];
    if (usage.is_object()) {
        state.usage.prompt_tokens = usage["prompt_tokens"].as_int(state.usage.prompt_tokens);
        state.usage.completion_tokens =
            usage["completion_tokens"].as_int(state.usage.completion_tokens);
    }
}

void OpenAiClient::handle_body(State& state, std::string_view chunk) {
    if (state.http_status >= 400) {
        state.raw_body.append(chunk);
        return;
    }
    if (state.on_raw) state.on_raw(chunk);
    if (state.stream) {
        state.sse.feed(chunk, [&state](const SseEvent& event) { handle_event(state, event); });
        Metrics::instance().inc(Counter::StreamEventsTotal);
        return;
    }
    state.raw_body.append(chunk);
}

OpenAiClient::Outcome OpenAiClient::parse_non_stream(State& state) {
    Outcome outcome;
    outcome.status = state.http_status;
    outcome.id = state.id;
    outcome.model = state.model;

    outcome.raw_body = state.raw_body;
    const auto parsed = parse_json(state.raw_body);
    if (!parsed) {
        outcome.content = state.raw_body;
        return outcome;
    }
    const Json& document = *parsed;
    if (document["id"].is_string()) outcome.id = std::string(document["id"].as_string());
    if (document["model"].is_string()) outcome.model = std::string(document["model"].as_string());

    const Json& choices = document["choices"];
    if (choices.is_array() && choices.size() > 0) {
        const Json& message = choices[0]["message"];
        if (message.is_object() && message["content"].is_string()) {
            outcome.content = std::string(message["content"].as_string());
        } else if (choices[0]["text"].is_string()) {
            outcome.content = std::string(choices[0]["text"].as_string());
        }
    }

    const Json& usage = document["usage"];
    if (usage.is_object()) {
        outcome.usage.prompt_tokens = usage["prompt_tokens"].as_int();
        outcome.usage.completion_tokens = usage["completion_tokens"].as_int();
    }
    return outcome;
}

void OpenAiClient::complete(const Request& request, DeltaHandler on_delta,
                            CompletionHandler on_done, RawHandler on_raw,
                            std::function<void(std::uint64_t)> on_started) {
    if (!url_error_.ok()) {
        if (on_done) on_done(std::unexpected(url_error_));
        return;
    }
    if (request.model.empty()) {
        if (on_done) on_done(fail(errc::kInvalidArgument, "request has no model"));
        return;
    }

    auto state = std::make_shared<State>();
    state->stream = request.stream;
    state->on_delta = std::move(on_delta);
    state->on_raw = std::move(on_raw);

    // Build the upstream payload.
    Json body = Json::make_object();
    body.set("model", request.model);
    Json messages = Json::make_array();
    for (const auto& message : request.messages) {
        Json item = Json::make_object();
        item.set("role", message.role);
        item.set("content", message.content);
        messages.push_back(std::move(item));
    }
    body.set("messages", std::move(messages));
    if (request.temperature >= 0.0) body.set("temperature", request.temperature);
    if (request.max_tokens > 0) body.set("max_tokens", request.max_tokens);
    body.set("stream", request.stream);
    if (request.stream && request.include_usage) {
        Json options = Json::make_object();
        options.set("include_usage", true);
        body.set("stream_options", std::move(options));
    }

    net::HttpClient::Request http_request;
    http_request.host = host_;
    http_request.port = port_;
    http_request.method = net::Method::Post;
    http_request.target = prefix_ + "/chat/completions";
    http_request.headers.set("Content-Type", "application/json");
    http_request.headers.set("Accept", request.stream ? "text/event-stream" : "application/json");
    if (!api_key_.empty()) {
        http_request.headers.set("Authorization", "Bearer " + api_key_);
    }
    http_request.body = body.dump();

    auto* state_ptr = state.get();

    net::HttpClient::Callbacks callbacks;
    callbacks.on_headers = [state_ptr](int status, const net::HeaderMap&) {
        state_ptr->http_status = status;
    };
    callbacks.on_body = [state_ptr](std::string_view chunk) { handle_body(*state_ptr, chunk); };
    callbacks.on_complete = [state, on_done = std::move(on_done)](const Status& status) mutable {
        if (!status.ok()) {
            if (on_done) on_done(std::unexpected(status));
            return;
        }
        if (state->http_status >= 400) {
            const auto message = error_message_from(state->raw_body);
            if (on_done) {
                on_done(fail(errc::kUpstreamError,
                             "upstream HTTP " + std::to_string(state->http_status) + ": " +
                                 message));
            }
            return;
        }

        Outcome outcome;
        outcome.request_id = state->request_id;
        if (state->stream) {
            outcome.status = state->http_status;
            outcome.id = state->id;
            outcome.model = state->model;
            outcome.content = state->content;
            outcome.usage = state->usage;
        } else {
            outcome = parse_non_stream(*state);
        }
        if (on_done) on_done(std::move(outcome));
    };

    state->request_id = http_.send(std::move(http_request), std::move(callbacks));
    if (on_started) on_started(state->request_id);
}

}  // namespace aegis::ai
