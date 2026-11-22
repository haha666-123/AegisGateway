#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aegis::ai {

// Token accounting used for rate limiting and usage reporting.
//
// The gateway deliberately ships no BPE vocabulary (that would mean vendoring a
// multi-megabyte table). Instead it uses a documented heuristic that tracks real
// tokenizers closely enough for budgeting:
//
//   * a run of ASCII word characters costs 1 token per 4 characters
//   * a CJK / Hangul / Kana code point costs 1 token
//   * punctuation and symbols cost 1 token each
//   * every chat message adds 4 tokens of framing overhead
//
// Tests pin the behaviour so a change is always visible.
[[nodiscard]] int estimate_tokens(std::string_view text);

[[nodiscard]] int estimate_message_tokens(std::string_view role, std::string_view content);

[[nodiscard]] int estimate_chat_tokens(
    const std::vector<std::pair<std::string, std::string>>& messages);

}  // namespace aegis::ai
