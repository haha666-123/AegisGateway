#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

namespace aegis::ai {

struct SseEvent {
    std::string event{"message"};
    std::string id;
    std::string data;
    int retry{0};
    bool has_retry{false};
};

// Incremental Server-Sent Events parser (W3C "event stream" format).
//
// Upstream chunks arrive on arbitrary byte boundaries - a single `data:` line is
// routinely split across two TCP segments - so the parser only dispatches on a
// complete blank line and buffers everything else.
class SseParser {
public:
    using Callback = std::function<void(const SseEvent&)>;

    void feed(std::string_view chunk, const Callback& callback);
    void reset();

    [[nodiscard]] std::size_t events_dispatched() const noexcept { return events_; }
    [[nodiscard]] std::size_t buffered_bytes() const noexcept { return line_buffer_.size(); }

private:
    void handle_line(std::string_view line, const Callback& callback);

    std::string line_buffer_;
    SseEvent current_;
    bool has_data_{false};
    std::size_t events_{0};
};

}  // namespace aegis::ai
