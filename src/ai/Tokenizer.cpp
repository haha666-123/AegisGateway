#include "ai/Tokenizer.h"

#include <cstdint>

namespace aegis::ai {
namespace {

// Decodes one UTF-8 code point and advances `index`. Invalid sequences are
// treated as a single byte so the function never throws or loops forever.
std::uint32_t decode_utf8(std::string_view text, std::size_t& index) {
    const auto byte = static_cast<unsigned char>(text[index]);
    if (byte < 0x80) {
        ++index;
        return byte;
    }

    std::size_t extra = 0;
    std::uint32_t code = 0;
    if ((byte & 0xE0) == 0xC0) {
        extra = 1;
        code = byte & 0x1FU;
    } else if ((byte & 0xF0) == 0xE0) {
        extra = 2;
        code = byte & 0x0FU;
    } else if ((byte & 0xF8) == 0xF0) {
        extra = 3;
        code = byte & 0x07U;
    } else {
        ++index;
        return byte;
    }

    if (index + extra >= text.size()) {
        ++index;
        return byte;
    }
    for (std::size_t i = 1; i <= extra; ++i) {
        const auto next = static_cast<unsigned char>(text[index + i]);
        if ((next & 0xC0) != 0x80) {
            ++index;
            return byte;
        }
        code = (code << 6) | (next & 0x3FU);
    }
    index += extra + 1;
    return code;
}

bool is_word_char(std::uint32_t code) {
    return (code >= 'a' && code <= 'z') || (code >= 'A' && code <= 'Z') ||
           (code >= '0' && code <= '9') || code == '_' || code == '-' || code == '\'';
}

bool is_cjk(std::uint32_t code) {
    return (code >= 0x2E80 && code <= 0x9FFF) ||    // CJK radicals .. unified ideographs
           (code >= 0xAC00 && code <= 0xD7AF) ||    // Hangul syllables
           (code >= 0xF900 && code <= 0xFAFF) ||    // CJK compatibility ideographs
           (code >= 0xFF00 && code <= 0xFFEF) ||    // half/full width forms
           (code >= 0x20000 && code <= 0x2FA1F);    // CJK extension B..F
}

bool is_space(std::uint32_t code) {
    return code == ' ' || code == '\t' || code == '\n' || code == '\r' || code == '\f' ||
           code == 0xA0;
}

}  // namespace

int estimate_tokens(std::string_view text) {
    if (text.empty()) return 0;

    int tokens = 0;
    int word_run = 0;
    const auto flush_word = [&tokens, &word_run] {
        if (word_run > 0) tokens += (word_run + 3) / 4;
        word_run = 0;
    };

    std::size_t index = 0;
    while (index < text.size()) {
        const std::uint32_t code = decode_utf8(text, index);
        if (is_word_char(code)) {
            ++word_run;
        } else if (is_space(code)) {
            flush_word();
        } else if (is_cjk(code)) {
            flush_word();
            ++tokens;
        } else {
            flush_word();
            ++tokens;
        }
    }
    flush_word();
    return tokens == 0 ? 1 : tokens;
}

int estimate_message_tokens(std::string_view role, std::string_view content) {
    return 4 + estimate_tokens(role) + estimate_tokens(content);
}

int estimate_chat_tokens(const std::vector<std::pair<std::string, std::string>>& messages) {
    int total = 3;  // priming overhead of the chat template
    for (const auto& [role, content] : messages) {
        total += estimate_message_tokens(role, content);
    }
    return total;
}

}  // namespace aegis::ai
