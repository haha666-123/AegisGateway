#include "ai/Prompt.h"

#include <cstddef>

#include "common/Util.h"

namespace aegis::ai {

Result<std::string> PromptTemplate::render(
    const std::unordered_map<std::string, std::string>& variables) const {
    std::string out;
    out.reserve(text_.size() + 32);

    std::size_t cursor = 0;
    while (cursor < text_.size()) {
        const auto open = text_.find("{{", cursor);
        if (open == std::string::npos) {
            out.append(text_, cursor, std::string::npos);
            break;
        }
        out.append(text_, cursor, open - cursor);

        const auto close = text_.find("}}", open + 2);
        if (close == std::string::npos) {
            return fail(errc::kParseError, "unterminated '{{' in prompt template");
        }

        const auto raw_name = std::string_view(text_).substr(open + 2, close - open - 2);
        const auto name = std::string(trim(raw_name));
        if (name.empty()) {
            return fail(errc::kParseError, "empty placeholder in prompt template");
        }

        const auto it = variables.find(name);
        if (it == variables.end()) {
            return fail(errc::kInvalidArgument, "missing template variable: " + name);
        }
        out += it->second;
        cursor = close + 2;
    }
    return out;
}

std::vector<std::string> PromptTemplate::variables() const {
    std::vector<std::string> names;
    std::size_t cursor = 0;
    while (true) {
        const auto open = text_.find("{{", cursor);
        if (open == std::string::npos) break;
        const auto close = text_.find("}}", open + 2);
        if (close == std::string::npos) break;

        const auto raw_name = std::string_view(text_).substr(open + 2, close - open - 2);
        const auto name = std::string(trim(raw_name));
        if (!name.empty()) {
            bool duplicate = false;
            for (const auto& existing : names) {
                if (existing == name) duplicate = true;
            }
            if (!duplicate) names.push_back(name);
        }
        cursor = close + 2;
    }
    return names;
}

}  // namespace aegis::ai
