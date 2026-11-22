#include "ai/Sse.h"

#include <charconv>

namespace aegis::ai {

void SseParser::feed(std::string_view chunk, const Callback& callback) {
    line_buffer_.append(chunk.data(), chunk.size());

    std::size_t start = 0;
    while (start < line_buffer_.size()) {
        const auto newline = line_buffer_.find('\n', start);
        if (newline == std::string::npos) break;

        std::string_view line(line_buffer_.data() + start, newline - start);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        handle_line(line, callback);
        start = newline + 1;
    }

    if (start > 0) line_buffer_.erase(0, start);
}

void SseParser::handle_line(std::string_view line, const Callback& callback) {
    if (line.empty()) {
        if (has_data_) {
            if (!current_.data.empty() && current_.data.back() == '\n') current_.data.pop_back();
            ++events_;
            if (callback) callback(current_);
        }
        current_ = SseEvent{};
        has_data_ = false;
        return;
    }

    if (line.front() == ':') return;  // comment / keep-alive

    const auto colon = line.find(':');
    std::string_view field;
    std::string_view value;
    if (colon == std::string_view::npos) {
        field = line;
    } else {
        field = line.substr(0, colon);
        value = line.substr(colon + 1);
        if (!value.empty() && value.front() == ' ') value.remove_prefix(1);
    }

    if (field == "data") {
        current_.data.append(value);
        current_.data.push_back('\n');
        has_data_ = true;
    } else if (field == "event") {
        current_.event.assign(value);
    } else if (field == "id") {
        current_.id.assign(value);
    } else if (field == "retry") {
        int parsed = 0;
        const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
        if (result.ec == std::errc{} && parsed >= 0) {
            current_.retry = parsed;
            current_.has_retry = true;
        }
    }
    // Unknown fields are ignored per the specification.
}

void SseParser::reset() {
    line_buffer_.clear();
    current_ = SseEvent{};
    has_data_ = false;
}

}  // namespace aegis::ai
