#include <string>

#include "TestFramework.h"
#include "common/Json.h"

using namespace aegis;

AEGIS_TEST(json_parse_primitives) {
    auto value = parse_json("true");
    AEGIS_CHECK(value.has_value());
    AEGIS_CHECK(value->is_bool());
    AEGIS_CHECK(value->as_bool());

    AEGIS_CHECK(parse_json("false")->as_bool() == false);
    AEGIS_CHECK(parse_json("null")->is_null());
    AEGIS_CHECK_EQ(parse_json("42")->as_int64(), int64_t{42});
    AEGIS_CHECK_EQ(parse_json("-17")->as_int64(), int64_t{-17});
    AEGIS_CHECK_EQ(parse_json("3.5")->as_double(), 3.5);
    AEGIS_CHECK_EQ(parse_json("1e3")->as_double(), 1000.0);
    AEGIS_CHECK_EQ(parse_json("\"hello\"")->as_string(), std::string_view("hello"));
    AEGIS_CHECK_EQ(parse_json("  \t\r\n 7 \n")->as_int64(), int64_t{7});
}

AEGIS_TEST(json_parse_string_escapes) {
    const auto value = parse_json(R"("a\"b\\c\/d\n\t")");
    AEGIS_CHECK(value.has_value());
    AEGIS_CHECK_EQ(value->as_string(), std::string_view("a\"b\\c/d\n\t"));

    const auto unicode = parse_json(R"("\u4f60\u597d")");
    AEGIS_CHECK_EQ(unicode->as_string(), std::string_view("你好"));

    const auto emoji = parse_json(R"("\ud83d\ude00")");
    AEGIS_CHECK(emoji.has_value());
    AEGIS_CHECK_EQ(emoji->as_string(), std::string_view("\xF0\x9F\x98\x80"));
}

AEGIS_TEST(json_parse_nested_structures) {
    const auto doc = parse_json(R"({
        "model": "gpt-4o-mini",
        "stream": true,
        "messages": [
            {"role": "system", "content": "be brief"},
            {"role": "user", "content": "hi"}
        ],
        "params": {"temperature": 0.2, "max_tokens": 128, "top_p": null}
    })");

    AEGIS_CHECK(doc.has_value());
    AEGIS_CHECK(doc->is_object());
    AEGIS_CHECK_EQ(doc->size(), std::size_t{4});
    AEGIS_CHECK_EQ((*doc)["model"].as_string(), std::string_view("gpt-4o-mini"));
    AEGIS_CHECK((*doc)["stream"].as_bool());
    AEGIS_CHECK_EQ((*doc)["messages"].size(), std::size_t{2});
    AEGIS_CHECK_EQ((*doc)["messages"][1]["role"].as_string(), std::string_view("user"));
    AEGIS_CHECK_EQ((*doc)["params"]["max_tokens"].as_int64(), int64_t{128});
    AEGIS_CHECK((*doc)["params"]["top_p"].is_null());

    // insertion order is preserved
    const auto keys = doc->keys();
    AEGIS_CHECK_EQ(keys.size(), std::size_t{4});
    AEGIS_CHECK_EQ(keys[0], std::string("model"));
    AEGIS_CHECK_EQ(keys[3], std::string("params"));
}

AEGIS_TEST(json_parse_errors) {
    AEGIS_CHECK(!parse_json("").has_value());
    AEGIS_CHECK(!parse_json("{").has_value());
    AEGIS_CHECK(!parse_json("{\"a\": }").has_value());
    AEGIS_CHECK(!parse_json("[1,2").has_value());
    AEGIS_CHECK(!parse_json("tru").has_value());
    AEGIS_CHECK(!parse_json("\"unterminated").has_value());
    AEGIS_CHECK(!parse_json("\"bad\\qescape\"").has_value());
    AEGIS_CHECK(!parse_json("{} trailing").has_value());
    AEGIS_CHECK(!parse_json("+1").has_value());
    AEGIS_CHECK(!parse_json("1 2").has_value());
    AEGIS_CHECK(!parse_json("[1,2,]").has_value());

    std::string deep(200, '[');
    deep.append(200, ']');
    AEGIS_CHECK(!parse_json(deep).has_value());

    const auto error = parse_json("{\"a\": 1");
    AEGIS_CHECK(!error.has_value());
    AEGIS_CHECK(error.error().code() == errc::kParseError);
    AEGIS_CHECK(error.error().message().find("offset") != std::string::npos);
}

AEGIS_TEST(json_dump_roundtrip) {
    const std::string original =
        R"({"a":1,"b":[true,false,null],"c":"x\ny","d":{"e":2.25},"f":[],"g":{}})";
    const auto parsed = parse_json(original);
    AEGIS_CHECK(parsed.has_value());
    const std::string dumped = parsed->dump();
    AEGIS_CHECK_EQ(dumped, original);

    const auto reparsed = parse_json(dumped);
    AEGIS_CHECK(reparsed.has_value());
    AEGIS_CHECK(*reparsed == *parsed);
}

AEGIS_TEST(json_dump_escapes_control_characters) {
    Json value(std::string("a\x01"));
    AEGIS_CHECK_EQ(value.dump(), std::string("\"a\\u0001\""));

    Json text(std::string("tab\there"));
    AEGIS_CHECK_EQ(text.dump(), std::string("\"tab\\there\""));

    Json quote(std::string("say \"hi\""));
    AEGIS_CHECK_EQ(quote.dump(), std::string("\"say \\\"hi\\\"\""));
}

AEGIS_TEST(json_numbers_keep_integer_form) {
    AEGIS_CHECK_EQ(Json(42).dump(), std::string("42"));
    AEGIS_CHECK_EQ(Json(int64_t{-7}).dump(), std::string("-7"));
    AEGIS_CHECK_EQ(Json(1.5).dump(), std::string("1.5"));
    AEGIS_CHECK_EQ(Json(2.0).dump(), std::string("2"));

    const int64_t big = 9007199254740993LL;  // not representable as double
    AEGIS_CHECK_EQ(Json(big).dump(), std::string("9007199254740993"));
    AEGIS_CHECK_EQ(parse_json("9007199254740993")->as_int64(), big);
}

AEGIS_TEST(json_object_helpers) {
    Json object = Json::make_object();
    object.set("x", 1);
    object.set("y", Json::make_array());
    object["y"].push_back("a");
    object["y"].push_back("b");

    AEGIS_CHECK(object.contains("x"));
    AEGIS_CHECK(!object.contains("z"));
    AEGIS_CHECK_EQ(object["y"].size(), std::size_t{2});
    AEGIS_CHECK_EQ(object["y"][0].as_string(), std::string_view("a"));

    // missing keys behave like null
    AEGIS_CHECK(object["missing"].is_null());
    AEGIS_CHECK_EQ(object["missing"].as_int(9), 9);

    object.set("x", 5);  // overwrite keeps position
    AEGIS_CHECK_EQ(object.keys()[0], std::string("x"));
    AEGIS_CHECK_EQ(object["x"].as_int(), 5);

    AEGIS_CHECK(object.erase("x"));
    AEGIS_CHECK(!object.erase("x"));
    AEGIS_CHECK_EQ(object.keys()[0], std::string("y"));
}

AEGIS_TEST(json_array_helpers) {
    Json array = Json::make_array();
    AEGIS_CHECK(array.is_array());
    AEGIS_CHECK(array.empty());
    for (int i = 0; i < 3; ++i) array.push_back(i * i);
    AEGIS_CHECK_EQ(array.size(), std::size_t{3});
    AEGIS_CHECK_EQ(array[2].as_int(), 4);
    AEGIS_CHECK_EQ(array.elements().size(), std::size_t{3});
    const Json& const_array = array;
    AEGIS_CHECK(const_array[99].is_null());
    AEGIS_CHECK_EQ(array.dump(), std::string("[0,1,4]"));
}

AEGIS_TEST(json_pretty_dump_is_stable) {
    const auto doc = parse_json(R"({"a":[1,2],"b":{"c":true}})");
    const auto pretty = doc->dump_pretty(2);
    AEGIS_CHECK(pretty.find("\n  \"a\": [") != std::string::npos);
    AEGIS_CHECK(pretty.find("\n    \"c\": true") != std::string::npos);
    AEGIS_CHECK(parse_json(pretty)->dump() == doc->dump());
}

AEGIS_TEST(json_accessor_fallbacks) {
    const auto value = parse_json(R"({"s":"text","n":3.5})");
    AEGIS_CHECK_EQ((*value)["missing"].as_string("fallback"), std::string_view("fallback"));
    AEGIS_CHECK_EQ((*value)["s"].as_int64(11), int64_t{11});
    AEGIS_CHECK_EQ((*value)["n"].as_int64(), int64_t{4});  // rounds
    AEGIS_CHECK_EQ((*value)["n"].as_double(), 3.5);
    AEGIS_CHECK_EQ((*value)["s"].as_bool(true), true);
}
