#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "ai/Sse.h"
#include "ai/Tokenizer.h"
#include "common/Status.h"
#include "net/HttpClient.h"

namespace aegis::ai {

// Minimal OpenAI compatible client: POST {base}/chat/completions.
//
// Only plain HTTP is supported. Deployments terminate TLS in front of the
// gateway (or run a local vLLM/Ollama endpoint); see the README for the
// rationale. Streaming responses are parsed as SSE and forwarded token by token
// through `on_delta`, so the gateway never buffers a whole completion.
class OpenAiClient {
public:
    struct Message {
        std::string role{"user"};
        std::string content;
    };

    struct Request {
        std::string model;
        std::vector<Message> messages;
        double temperature{-1.0};
        int max_tokens{0};
        bool stream{false};
        bool include_usage{true};
    };

    struct Usage {
        int prompt_tokens{0};
        int completion_tokens{0};
    };

    struct Outcome {
        int status{200};
        std::string id;
        std::string model;
        std::string content;
        Usage usage;
        // Full upstream JSON body for non streaming responses, so the response
        // cache can replay byte identical answers.
        std::string raw_body;
        // Upstream request id for pause()/resume() based back pressure.
        std::uint64_t request_id{0};
    };

    using DeltaHandler = std::function<void(std::string_view delta)>;
    using RawHandler = std::function<void(std::string_view chunk)>;
    using CompletionHandler = std::function<void(Result<Outcome>)>;

    OpenAiClient(net::HttpClient& http, std::string base_url, std::string api_key);

    // `on_raw` receives the upstream body bytes verbatim (before parsing), which
    // is what the gateway uses to forward SSE frames without re-serialising them.
    void complete(const Request& request, DeltaHandler on_delta, CompletionHandler on_done,
                  RawHandler on_raw = {},
                  std::function<void(std::uint64_t request_id)> on_started = {});

    [[nodiscard]] const std::string& base_url() const noexcept { return base_url_; }
    [[nodiscard]] const Status& configuration_error() const noexcept { return url_error_; }

private:
    struct State;

    static void handle_body(State& state, std::string_view chunk);
    static void handle_event(State& state, const SseEvent& event);
    static Outcome parse_non_stream(State& state);

    net::HttpClient& http_;
    std::string base_url_;
    std::string api_key_;
    std::string host_;
    std::string prefix_;
    std::uint16_t port_{80};
    Status url_error_;
};

}  // namespace aegis::ai
