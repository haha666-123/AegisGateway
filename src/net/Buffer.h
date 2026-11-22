#pragma once

#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace aegis::net {

// Growable byte buffer with a read cursor. Consuming data only advances the
// cursor; compaction happens lazily when the read cursor is large enough to be
// worth the memmove. This keeps `parse -> consume -> parse` loops cheap, which
// is what the HTTP/SSE parsers do on every socket read.
class Buffer {
public:
    Buffer() = default;
    explicit Buffer(std::size_t reserve) { storage_.reserve(reserve); }

    void append(const char* data, std::size_t length) {
        if (length == 0) return;
        compact_if_needed(length);
        const auto* bytes = reinterpret_cast<const char*>(data);
        storage_.insert(storage_.end(), bytes, bytes + length);
    }

    void append(std::string_view text) { append(text.data(), text.size()); }

    [[nodiscard]] std::string_view view() const noexcept {
        return std::string_view(storage_.data() + read_index_, readable());
    }

    [[nodiscard]] const char* data() const noexcept { return storage_.data() + read_index_; }

    [[nodiscard]] std::size_t readable() const noexcept { return storage_.size() - read_index_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return storage_.capacity(); }
    [[nodiscard]] bool empty() const noexcept { return readable() == 0; }

    void consume(std::size_t count) noexcept {
        read_index_ += count > readable() ? readable() : count;
        if (read_index_ == storage_.size()) clear();
    }

    void clear() noexcept {
        storage_.clear();
        read_index_ = 0;
    }

    [[nodiscard]] std::string take_all() {
        std::string out(data(), readable());
        clear();
        return out;
    }

    [[nodiscard]] std::size_t find(std::string_view needle, std::size_t from = 0) const noexcept {
        if (needle.empty() || from >= readable()) return std::string_view::npos;
        const auto pos = view().find(needle, from);
        return pos;
    }

private:
    void compact_if_needed(std::size_t incoming) {
        if (read_index_ == 0) return;
        const bool worth_compacting =
            read_index_ >= 4096 && (read_index_ >= storage_.size() / 2 || incoming > capacity());
        if (!worth_compacting) return;
        storage_.erase(storage_.begin(), storage_.begin() + static_cast<std::ptrdiff_t>(read_index_));
        read_index_ = 0;
    }

    std::vector<char> storage_;
    std::size_t read_index_{0};
};

}  // namespace aegis::net
