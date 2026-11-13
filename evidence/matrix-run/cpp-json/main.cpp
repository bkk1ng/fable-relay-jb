// main.cpp — JSON parser/serializer demo
// Requires C++17. Build: c++ -std=c++17 -Wall -Wextra -o json_demo main.cpp

#include "json.hpp"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

// ---------------------------------------------------------------------------
// A sample JSON document that exercises most of the grammar:
//   - nested objects and arrays
//   - all scalar types (null, bool, number, string)
//   - string escapes: \n \t \\ \" \uXXXX (including a surrogate pair)
//   - negative and floating-point numbers
// ---------------------------------------------------------------------------
static const char* SAMPLE = R"({
  "name"      : "JSON \u0026 C++17",
  "version"   : 1.0,
  "stable"    : true,
  "deprecated": false,
  "nothing"   : null,
  "scores"    : [98, -3, 0, 3.14e2],
  "meta" : {
    "author"  : "Alice\tBob",
    "note"    : "line1\nline2",
    "escape"  : "back\\slash and \"quotes\"",
    "emoji"   : "\uD83D\uDE00",
    "empty"   : {}
  },
  "matrix"    : [[1,2],[3,4]],
  "empty_arr" : []
})";

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static void check(bool cond, const char* msg) {
    if (!cond) {
        std::fprintf(stderr, "ASSERTION FAILED: %s\n", msg);
        std::abort();
    }
}

// Deep equality for json::Value — good enough for round-trip verification.
static bool values_equal(const json::Value& a, const json::Value& b) {
    if (a.v.index() != b.v.index()) return false;

    if (a.is_null())   return true;
    if (a.is_bool())   return a.as_bool()   == b.as_bool();
    if (a.is_number()) return a.as_number() == b.as_number();
    if (a.is_string()) return a.as_string() == b.as_string();

    if (a.is_array()) {
        const auto& aa = a.as_array();
        const auto& ba = b.as_array();
        if (aa.size() != ba.size()) return false;
        for (std::size_t i = 0; i < aa.size(); ++i)
            if (!values_equal(aa[i], ba[i])) return false;
        return true;
    }

    // Object
    const auto& ao = a.as_object();
    const auto& bo = b.as_object();
    if (ao.size() != bo.size()) return false;
    for (std::size_t i = 0; i < ao.size(); ++i) {
        if (ao[i].first != bo[i].first) return false;
        if (!values_equal(ao[i].second, bo[i].second)) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main() {
    std::puts("=== JSON parser / serializer demo ===\n");

    // -----------------------------------------------------------------------
    // 1. Parse the sample document
    // -----------------------------------------------------------------------
    std::puts("-- Parse --");
    json::Value root;
    try {
        root = json::parse(SAMPLE);
    } catch (const json::ParseError& e) {
        std::fprintf(stderr, "Parse error: %s\n", e.what());
        return 1;
    }

    check(root.is_object(),                            "root is object");

    const auto& obj = root.as_object();

    // Find helpers — object is ordered (insertion order preserved).
    auto find = [&](const json::Object& o, const char* key) -> const json::Value* {
        for (const auto& kv : o)
            if (kv.first == key) return &kv.second;
        return nullptr;
    };

    // Spot-check scalars
    {
        const json::Value* v = find(obj, "name");
        check(v && v->is_string(), "name is string");
        check(v->as_string() == "JSON & C++17",        // \u0026 -> '&'
              "name value (\\u0026 decoded)");
        std::printf("  name     = \"%s\"\n", v->as_string().c_str());
    }
    {
        const json::Value* v = find(obj, "version");
        check(v && v->is_number(), "version is number");
        check(v->as_number() == 1.0, "version == 1.0");
        std::printf("  version  = %g\n", v->as_number());
    }
    {
        const json::Value* v = find(obj, "stable");
        check(v && v->is_bool() && v->as_bool(), "stable == true");
        std::printf("  stable   = %s\n", v->as_bool() ? "true" : "false");
    }
    {
        const json::Value* v = find(obj, "deprecated");
        check(v && v->is_bool() && !v->as_bool(), "deprecated == false");
        std::printf("  deprecated = false\n");
    }
    {
        const json::Value* v = find(obj, "nothing");
        check(v && v->is_null(), "nothing is null");
        std::printf("  nothing  = null\n");
    }
    {
        const json::Value* v = find(obj, "scores");
        check(v && v->is_array(), "scores is array");
        check(v->as_array().size() == 4, "scores has 4 elements");
        check(v->as_array()[0].as_number() == 98.0,     "scores[0]");
        check(v->as_array()[1].as_number() == -3.0,     "scores[1] negative");
        check(v->as_array()[3].as_number() == 314.0,    "scores[3] 3.14e2");
        std::printf("  scores   = [98, -3, 0, 314]\n");
    }

    // Check nested object & string escapes
    {
        const json::Value* meta = find(obj, "meta");
        check(meta && meta->is_object(), "meta is object");

        const json::Value* note = find(meta->as_object(), "note");
        check(note && note->is_string(), "meta.note is string");
        // The \n in the JSON string should be a real newline in the C++ string.
        check(note->as_string() == "line1\nline2", "meta.note newline");
        std::printf("  meta.note (len=%zu, contains newline)\n",
                    note->as_string().size());

        const json::Value* esc = find(meta->as_object(), "escape");
        check(esc && esc->is_string(), "meta.escape is string");
        check(esc->as_string() == R"(back\slash and "quotes")",
              "meta.escape backslash/quotes");

        // Surrogate pair \uD83D\uDE00 -> U+1F600 GRINNING FACE -> UTF-8: F0 9F 98 80
        const json::Value* emoji = find(meta->as_object(), "emoji");
        check(emoji && emoji->is_string(), "meta.emoji is string");
        const std::string& es = emoji->as_string();
        check(es.size() == 4, "emoji UTF-8 is 4 bytes");
        check(static_cast<unsigned char>(es[0]) == 0xF0 &&
              static_cast<unsigned char>(es[1]) == 0x9F &&
              static_cast<unsigned char>(es[2]) == 0x98 &&
              static_cast<unsigned char>(es[3]) == 0x80,
              "emoji UTF-8 bytes");
        std::printf("  meta.emoji UTF-8 bytes: F0 9F 98 80 (U+1F600)\n");
    }

    // -----------------------------------------------------------------------
    // 2. Serialize — compact
    // -----------------------------------------------------------------------
    std::puts("\n-- Compact serialization --");
    std::string compact = json::serialize(root, false);
    std::printf("  length: %zu bytes\n", compact.size());
    std::printf("  first 80 chars: %.80s\n", compact.c_str());

    // -----------------------------------------------------------------------
    // 3. Serialize — pretty
    // -----------------------------------------------------------------------
    std::puts("\n-- Pretty serialization --");
    std::string pretty = json::serialize(root, true, 2);
    std::printf("%s\n", pretty.c_str());

    // -----------------------------------------------------------------------
    // 4. Round-trip: parse the compact output and compare with original
    // -----------------------------------------------------------------------
    std::puts("-- Round-trip (parse compact, compare) --");
    json::Value round_tripped;
    try {
        round_tripped = json::parse(compact);
    } catch (const json::ParseError& e) {
        std::fprintf(stderr, "Round-trip parse error: %s\n", e.what());
        return 1;
    }
    check(values_equal(root, round_tripped), "round-trip deep equality");
    std::puts("  OK: original == parse(serialize(original))");

    // -----------------------------------------------------------------------
    // 5. Error-reporting test: bad input, check offset
    // -----------------------------------------------------------------------
    std::puts("\n-- Error reporting --");
    const char* bad = R"({"key": tru})";
    try {
        json::parse(bad);
        check(false, "should have thrown");
    } catch (const json::ParseError& e) {
        std::printf("  Caught: %s\n", e.what());
        check(e.offset > 0, "error offset > 0");
    }

    const char* bad2 = R"({"x": "unterminated)";
    try {
        json::parse(bad2);
        check(false, "should have thrown");
    } catch (const json::ParseError& e) {
        std::printf("  Caught: %s\n", e.what());
    }

    const char* bad3 = R"([1, 2, 3} ])";
    try {
        json::parse(bad3);
        check(false, "should have thrown");
    } catch (const json::ParseError& e) {
        std::printf("  Caught: %s\n", e.what());
    }

    // -----------------------------------------------------------------------
    // 6. Programmatic Value construction + serialization
    // -----------------------------------------------------------------------
    std::puts("\n-- Programmatic construction --");
    {
        json::Object person;
        person.emplace_back("name",   json::Value("Carol"));
        person.emplace_back("age",    json::Value(29));
        person.emplace_back("active", json::Value(true));
        person.emplace_back("score",  json::Value(9.5));
        person.emplace_back("notes",  json::Value());   // null

        json::Array tags;
        tags.push_back(json::Value("admin"));
        tags.push_back(json::Value("user"));
        person.emplace_back("tags", json::Value(std::move(tags)));

        json::Value v(std::move(person));
        std::string out = json::serialize(v, true);
        std::printf("%s\n", out.c_str());

        // Parse it back and verify types
        json::Value back = json::parse(out);
        check(back.is_object(), "back is object");
        const json::Value* name = nullptr;
        for (auto& kv : back.as_object())
            if (kv.first == "name") name = &kv.second;
        check(name && name->as_string() == "Carol", "back.name == Carol");
        std::puts("  Programmatic round-trip OK");
    }

    std::puts("\nAll checks passed.");
    return 0;
}
