#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "common/Status.h"

namespace aegis::ai {

// Tiny `{{variable}}` template used for system prompts and prompt presets.
//
// Rendering is strict on purpose: a missing variable is an error instead of a
// silently empty string, because a half rendered system prompt is much harder to
// debug than a rejected request.
class PromptTemplate {
public:
    explicit PromptTemplate(std::string text) : text_(std::move(text)) {}

    [[nodiscard]] Result<std::string> render(
        const std::unordered_map<std::string, std::string>& variables) const;

    [[nodiscard]] const std::string& source() const noexcept { return text_; }
    [[nodiscard]] std::vector<std::string> variables() const;

private:
    std::string text_;
};

}  // namespace aegis::ai
