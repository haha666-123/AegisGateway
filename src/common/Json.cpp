#include "common/Json.h"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <limits>
#include <system_error>

namespace aegis {
namespace {

constexpr int kMaxDepth = 128;

const Json& null_json() {
    static const Json kNull;
    return kNull;
}

class Parser {
public:
    Parser(std::string_view text) : text_(text) {}

    Result<Json> parse() {
        skip_ws();
        auto value = parse_value(0);
        if (!value) return std::unexpected(value.error());
        skip_ws();
        if (pos_ != text_.size()) {
            return fail(errc::kParseError, error_at("trailing characters after top level value"));
        }
        return std::move(*value);
    }

private:
    std::string error_at(std::string_view what) const {
        return std::string(what) + " at offset " + std::to_string(pos_);
    }

    void skip_ws() {
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    bool consume(std::string_view literal) {
        if (text_.substr(pos_, literal.size()) == literal) {
            pos_ += literal.size();
            return true;
        }
        return false;
    }

    Result<Json> parse_value(int depth) {
        if (depth > kMaxDepth) {
            return fail(errc::kParseError, error_at("nesting too deep"));
        }
        if (pos_ >= text_.size()) {
            return fail(errc::kParseError, error_at("unexpected end of input"));
        }

        switch (text_[pos_]) {
            case '{': return parse_object(depth);
            case '[': return parse_array(depth);
            case '"': {
                auto s = parse_string();
                if (!s) return std::unexpected(s.error());
                return Json(std::move(*s));
            }
            case 't':
                if (consume("true")) return Json(true);
                return fail(errc::kParseError, error_at("invalid literal"));
            case 'f':
                if (consume("false")) return Json(false);
                return fail(errc::kParseError, error_at("invalid literal"));
            case 'n':
                if (consume("null")) return Json(nullptr);
                return fail(errc::kParseError, error_at("invalid literal"));
            default: return parse_number();
        }
    }

    Result<Json> parse_object(int depth) {
        ++pos_;  // '{'
        Json object{Json::Object{}};
        skip_ws();
        if (pos_ < text_.size() && text_[pos_] == '}') {
            ++pos_;
            return object;
        }
        for (;;) {
            skip_ws();
            if (pos_ >= text_.size() || text_[pos_] != '"') {
                return fail(errc::kParseError, error_at("expected object key"));
            }
            auto key = parse_string();
            if (!key) return std::unexpected(key.error());

            skip_ws();
            if (pos_ >= text_.size() || text_[pos_] != ':') {
                return fail(errc::kParseError, error_at("expected ':' after object key"));
            }
            ++pos_;

            skip_ws();
            auto value = parse_value(depth + 1);
            if (!value) return std::unexpected(value.error());
            object.set(*key, std::move(*value));

            skip_ws();
            if (pos_ >= text_.size()) {
                return fail(errc::kParseError, error_at("unterminated object"));
            }
            if (text_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (text_[pos_] == '}') {
                ++pos_;
                return object;
            }
            return fail(errc::kParseError, error_at("expected ',' or '}' in object"));
        }
    }

    Result<Json> parse_array(int depth) {
        ++pos_;  // '['
        Json array{Json::Array{}};
        skip_ws();
        if (pos_ < text_.size() && text_[pos_] == ']') {
            ++pos_;
            return array;
        }
        for (;;) {
            skip_ws();
            auto value = parse_value(depth + 1);
            if (!value) return std::unexpected(value.error());
            array.push_back(std::move(*value));

            skip_ws();
            if (pos_ >= text_.size()) {
                return fail(errc::kParseError, error_at("unterminated array"));
            }
            if (text_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (text_[pos_] == ']') {
                ++pos_;
                return array;
            }
            return fail(errc::kParseError, error_at("expected ',' or ']' in array"));
        }
    }

    static void append_utf8(std::string& out, uint32_t cp) {
        if (cp <= 0x7F) {
            out.push_back(static_cast<char>(cp));
        } else if (cp <= 0x7FF) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp <= 0xFFFF) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    Result<uint32_t> parse_hex4() {
        if (pos_ + 4 > text_.size()) {
            return fail(errc::kParseError, error_at("truncated \\u escape"));
        }
        uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text_[pos_ + static_cast<std::size_t>(i)];
            uint32_t digit = 0;
            if (c >= '0' && c <= '9') {
                digit = static_cast<uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                digit = static_cast<uint32_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                digit = static_cast<uint32_t>(c - 'A' + 10);
            } else {
                return fail(errc::kParseError, error_at("invalid \\u escape"));
            }
            value = (value << 4) | digit;
        }
        pos_ += 4;
        return value;
    }

    Result<std::string> parse_string() {
        ++pos_;  // opening quote
        std::string out;
        for (;;) {
            if (pos_ >= text_.size()) {
                return fail(errc::kParseError, error_at("unterminated string"));
            }
            const char c = text_[pos_];
            if (c == '"') {
                ++pos_;
                return out;
            }
            if (c == '\\') {
                ++pos_;
                if (pos_ >= text_.size()) {
                    return fail(errc::kParseError, error_at("truncated escape"));
                }
                const char esc = text_[pos_++];
                switch (esc) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u': {
                        auto cp = parse_hex4();
                        if (!cp) return std::unexpected(cp.error());
                        uint32_t code = *cp;
                        if (code >= 0xD800 && code <= 0xDBFF) {
                            if (pos_ + 2 > text_.size() || text_[pos_] != '\\' ||
                                text_[pos_ + 1] != 'u') {
                                return fail(errc::kParseError,
                                            error_at("unpaired high surrogate"));
                            }
                            pos_ += 2;
                            auto low = parse_hex4();
                            if (!low) return std::unexpected(low.error());
                            if (*low < 0xDC00 || *low > 0xDFFF) {
                                return fail(errc::kParseError,
                                            error_at("invalid low surrogate"));
                            }
                            code = 0x10000 + ((code - 0xD800) << 10) + (*low - 0xDC00);
                        }
                        append_utf8(out, code);
                        break;
                    }
                    default:
                        return fail(errc::kParseError, error_at("invalid escape character"));
                }
                continue;
            }
            if (static_cast<unsigned char>(c) < 0x20) {
                return fail(errc::kParseError, error_at("raw control character in string"));
            }
            out.push_back(c);
            ++pos_;
        }
    }

    Result<Json> parse_number() {
        const std::size_t start = pos_;
        if (pos_ < text_.size() && (text_[pos_] == '-' || text_[pos_] == '+')) {
            if (text_[pos_] == '+') {
                return fail(errc::kParseError, error_at("leading '+' is not valid JSON"));
            }
            ++pos_;
        }
        bool is_integral = true;
        bool digits = false;
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (c >= '0' && c <= '9') {
                digits = true;
                ++pos_;
            } else if (c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') {
                is_integral = false;
                ++pos_;
            } else {
                break;
            }
        }
        if (!digits) {
            return fail(errc::kParseError, error_at("invalid number"));
        }

        const std::string_view token = text_.substr(start, pos_ - start);
        if (is_integral) {
            int64_t value = 0;
            const auto* begin = token.data();
            const auto* end = token.data() + token.size();
            const auto res = std::from_chars(begin, end, value);
            if (res.ec == std::errc{} && res.ptr == end) return Json(value);
            is_integral = false;  // overflowed int64, fall through to double
        }

        double value = 0.0;
        const auto* begin = token.data();
        const auto* end = token.data() + token.size();
        const auto res = std::from_chars(begin, end, value);
        if (res.ec != std::errc{} || res.ptr != end || !std::isfinite(value)) {
            return fail(errc::kParseError, error_at("invalid number value"));
        }
        return Json(value);
    }

    std::string_view text_;
    std::size_t pos_{0};
};

void escape_string_to(std::string& out, std::string_view text) {
    out.push_back('"');
    for (const char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: {
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                    out += buf;
                } else {
                    out.push_back(c);
                }
            }
        }
    }
    out.push_back('"');
}

void append_double(std::string& out, double value) {
    if (!std::isfinite(value)) {
        out += "null";
        return;
    }
    char buf[40];
    const auto res = std::to_chars(buf, buf + sizeof(buf), value);
    if (res.ec != std::errc{}) {
        out += "null";
        return;
    }
    out.append(buf, res.ptr);
}

void append_int(std::string& out, int64_t value) {
    char buf[24];
    const auto res = std::to_chars(buf, buf + sizeof(buf), value);
    out.append(buf, res.ptr);
}

}  // namespace

Json::Json(double value) {
    if (std::isfinite(value) && value == std::floor(value) &&
        value >= -9.2233720368547758e18 && value <= 9.2233720368547758e18) {
        value_ = Number{value, static_cast<int64_t>(value), true};
    } else {
        value_ = Number{value, 0, false};
    }
}

Json::Json(int64_t value) : value_(Number{static_cast<double>(value), value, true}) {}

Json::Type Json::type() const noexcept {
    switch (value_.index()) {
        case 0: return Type::Null;
        case 1: return Type::Bool;
        case 2: return Type::Number;
        case 3: return Type::String;
        case 4: return Type::Array;
        case 5: return Type::Object;
        default: return Type::Null;
    }
}

bool Json::as_bool(bool fallback) const noexcept {
    const auto* value = std::get_if<bool>(&value_);
    return value == nullptr ? fallback : *value;
}

double Json::as_double(double fallback) const noexcept {
    const auto* value = std::get_if<Number>(&value_);
    if (value == nullptr) return fallback;
    return value->integral ? static_cast<double>(value->i) : value->d;
}

int64_t Json::as_int64(int64_t fallback) const noexcept {
    const auto* value = std::get_if<Number>(&value_);
    if (value == nullptr) return fallback;
    if (value->integral) return value->i;
    if (!std::isfinite(value->d)) return fallback;
    return static_cast<int64_t>(std::llround(value->d));
}

int Json::as_int(int fallback) const noexcept {
    const int64_t value = as_int64(fallback);
    if (value > std::numeric_limits<int>::max() || value < std::numeric_limits<int>::min()) {
        return fallback;
    }
    return static_cast<int>(value);
}

std::string_view Json::as_string(std::string_view fallback) const noexcept {
    const auto* value = std::get_if<std::string>(&value_);
    return value == nullptr ? fallback : std::string_view(*value);
}

std::string& Json::mutable_string() {
    if (!is_string()) value_ = std::string{};
    return std::get<std::string>(value_);
}

std::size_t Json::size() const noexcept {
    if (const auto* array = std::get_if<Array>(&value_)) return array->size();
    if (const auto* object = std::get_if<Object>(&value_)) return object->size();
    return 0;
}

const Json& Json::operator[](std::size_t index) const {
    const auto* array = std::get_if<Array>(&value_);
    if (array == nullptr || index >= array->size()) return null_json();
    return (*array)[index];
}

Json& Json::operator[](std::size_t index) {
    if (!is_array()) value_ = Array{};
    auto& array = std::get<Array>(value_);
    if (index >= array.size()) array.resize(index + 1);
    return array[index];
}

void Json::push_back(Json value) {
    if (!is_array()) value_ = Array{};
    std::get<Array>(value_).push_back(std::move(value));
}

const Json::Array& Json::elements() const { return std::get<Array>(value_); }

bool Json::contains(std::string_view key) const {
    const auto* object = std::get_if<Object>(&value_);
    if (object == nullptr) return false;
    for (const auto& [name, value] : *object) {
        if (name == key) return true;
    }
    return false;
}

const Json& Json::operator[](std::string_view key) const {
    const auto* object = std::get_if<Object>(&value_);
    if (object == nullptr) return null_json();
    for (const auto& [name, value] : *object) {
        if (name == key) return value;
    }
    return null_json();
}

Json& Json::operator[](std::string_view key) {
    if (!is_object()) value_ = Object{};
    auto& object = std::get<Object>(value_);
    for (auto& [name, value] : object) {
        if (name == key) return value;
    }
    object.emplace_back(std::string(key), Json{});
    return object.back().second;
}

Json& Json::set(std::string_view key, Json value) {
    if (!is_object()) value_ = Object{};
    auto& object = std::get<Object>(value_);
    for (auto& [name, existing] : object) {
        if (name == key) {
            existing = std::move(value);
            return existing;
        }
    }
    object.emplace_back(std::string(key), std::move(value));
    return object.back().second;
}

bool Json::erase(std::string_view key) {
    auto* object = std::get_if<Object>(&value_);
    if (object == nullptr) return false;
    for (auto it = object->begin(); it != object->end(); ++it) {
        if (it->first == key) {
            object->erase(it);
            return true;
        }
    }
    return false;
}

const Json::Object& Json::members() const { return std::get<Object>(value_); }

std::vector<std::string> Json::keys() const {
    std::vector<std::string> out;
    if (const auto* object = std::get_if<Object>(&value_)) {
        out.reserve(object->size());
        for (const auto& [name, value] : *object) out.push_back(name);
    }
    return out;
}

void Json::dump_to(std::string& out, int indent, int depth) const {
    const bool pretty = indent > 0;
    switch (type()) {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += as_bool() ? "true" : "false"; break;
        case Type::Number: {
            const auto& number = std::get<Number>(value_);
            if (number.integral) {
                append_int(out, number.i);
            } else {
                append_double(out, number.d);
            }
            break;
        }
        case Type::String: escape_string_to(out, as_string()); break;
        case Type::Array: {
            const auto& array = std::get<Array>(value_);
            if (array.empty()) {
                out += "[]";
                break;
            }
            out.push_back('[');
            for (std::size_t i = 0; i < array.size(); ++i) {
                if (i > 0) out.push_back(',');
                if (pretty) {
                    out.push_back('\n');
                    out.append(static_cast<std::size_t>(indent) * (depth + 1), ' ');
                }
                array[i].dump_to(out, indent, depth + 1);
            }
            if (pretty) {
                out.push_back('\n');
                out.append(static_cast<std::size_t>(indent) * depth, ' ');
            }
            out.push_back(']');
            break;
        }
        case Type::Object: {
            const auto& object = std::get<Object>(value_);
            if (object.empty()) {
                out += "{}";
                break;
            }
            out.push_back('{');
            for (std::size_t i = 0; i < object.size(); ++i) {
                if (i > 0) out.push_back(',');
                if (pretty) {
                    out.push_back('\n');
                    out.append(static_cast<std::size_t>(indent) * (depth + 1), ' ');
                }
                escape_string_to(out, object[i].first);
                out.push_back(':');
                if (pretty) out.push_back(' ');
                object[i].second.dump_to(out, indent, depth + 1);
            }
            if (pretty) {
                out.push_back('\n');
                out.append(static_cast<std::size_t>(indent) * depth, ' ');
            }
            out.push_back('}');
            break;
        }
    }
}

std::string Json::dump() const {
    std::string out;
    out.reserve(64);
    dump_to(out, 0, 0);
    return out;
}

std::string Json::dump_pretty(int indent) const {
    std::string out;
    out.reserve(256);
    dump_to(out, indent, 0);
    return out;
}

bool operator==(const Json& lhs, const Json& rhs) {
    if (lhs.type() != rhs.type()) return false;
    switch (lhs.type()) {
        case Json::Type::Null: return true;
        case Json::Type::Bool: return lhs.as_bool() == rhs.as_bool();
        case Json::Type::Number: {
            const auto& a = std::get<Json::Number>(lhs.value_);
            const auto& b = std::get<Json::Number>(rhs.value_);
            if (a.integral && b.integral) return a.i == b.i;
            return lhs.as_double() == rhs.as_double();
        }
        case Json::Type::String: return lhs.as_string() == rhs.as_string();
        case Json::Type::Array: return lhs.elements() == rhs.elements();
        case Json::Type::Object: {
            const auto& a = lhs.members();
            const auto& b = rhs.members();
            if (a.size() != b.size()) return false;
            for (const auto& [key, value] : a) {
                if (!rhs.contains(key)) return false;
                if (!(rhs[key] == value)) return false;
            }
            return true;
        }
    }
    return false;
}

Result<Json> parse_json(std::string_view text) {
    Parser parser(text);
    return parser.parse();
}

}  // namespace aegis
