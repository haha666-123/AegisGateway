#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "common/Status.h"

namespace aegis {

// A small, dependency free JSON value used for the gateway configuration, for
// OpenAI compatible request/response payloads and for the unit tests.
//
// Design notes:
//   * objects keep insertion order (vector of pairs) so dumps are deterministic
//     and diff friendly, which matters for recorded fixtures in tests;
//   * numbers remember whether they arrived as integers so token counters are
//     never printed as 17.0;
//   * parse() reports byte offsets on error, which makes broken upstream
//     payloads much easier to debug.
class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    struct Number {
        double d{0.0};
        int64_t i{0};
        bool integral{false};
    };

    using Array = std::vector<Json>;
    using Object = std::vector<std::pair<std::string, Json>>;

    Json() = default;
    Json(std::nullptr_t) noexcept {}
    Json(bool value) : value_(value) {}
    Json(double value);
    Json(int value) : Json(static_cast<int64_t>(value)) {}
    Json(int64_t value);
    Json(const char* value) : value_(std::string(value == nullptr ? "" : value)) {}
    Json(std::string value) : value_(std::move(value)) {}
    Json(std::string_view value) : value_(std::string(value)) {}
    explicit Json(Array value) : value_(std::move(value)) {}
    explicit Json(Object value) : value_(std::move(value)) {}

    static Json make_array() { return Json{Array{}}; }
    static Json make_object() { return Json{Object{}}; }

    [[nodiscard]] Type type() const noexcept;
    [[nodiscard]] bool is_null() const noexcept { return type() == Type::Null; }
    [[nodiscard]] bool is_bool() const noexcept { return type() == Type::Bool; }
    [[nodiscard]] bool is_number() const noexcept { return type() == Type::Number; }
    [[nodiscard]] bool is_string() const noexcept { return type() == Type::String; }
    [[nodiscard]] bool is_array() const noexcept { return type() == Type::Array; }
    [[nodiscard]] bool is_object() const noexcept { return type() == Type::Object; }

    [[nodiscard]] bool as_bool(bool fallback = false) const noexcept;
    [[nodiscard]] double as_double(double fallback = 0.0) const noexcept;
    [[nodiscard]] int64_t as_int64(int64_t fallback = 0) const noexcept;
    [[nodiscard]] int as_int(int fallback = 0) const noexcept;
    [[nodiscard]] std::string_view as_string(std::string_view fallback = {}) const noexcept;
    [[nodiscard]] std::string& mutable_string();

    // ----- arrays
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool empty() const noexcept { return size() == 0; }
    const Json& operator[](std::size_t index) const;
    Json& operator[](std::size_t index);
    void push_back(Json value);
    [[nodiscard]] const Array& elements() const;

    // ----- objects
    [[nodiscard]] bool contains(std::string_view key) const;
    const Json& operator[](std::string_view key) const;
    Json& operator[](std::string_view key);
    Json& set(std::string_view key, Json value);
    bool erase(std::string_view key);
    [[nodiscard]] const Object& members() const;
    [[nodiscard]] std::vector<std::string> keys() const;

    [[nodiscard]] std::string dump() const;
    [[nodiscard]] std::string dump_pretty(int indent = 2) const;

    friend bool operator==(const Json& lhs, const Json& rhs);
    friend bool operator!=(const Json& lhs, const Json& rhs) { return !(lhs == rhs); }

private:
    std::variant<std::monostate, bool, Number, std::string, Array, Object> value_;

    void dump_to(std::string& out, int indent, int depth) const;
};

// Declared outside the class so Result<Json> is only instantiated once Json is
// a complete type.
Result<Json> parse_json(std::string_view text);

}  // namespace aegis
