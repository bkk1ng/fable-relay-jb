#include "json.hpp"

#include <cassert>
#include <cstdio>
#include <string>

// ─── helpers ────────────────────────────────────────────────────────────────

static void check(bool cond, const char* msg) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", msg);
        std::exit(1);
    }
}

// ─── round-trip test ────────────────────────────────────────────────────────

static void test_roundtrip(const std::string& src, const char* label) {
    // Parse then re-serialize compact, then parse again.
    json::Value v1 = json::parse(src);
    std::string compact = json::serialize(v1);
    json::Value v2 = json::parse(compact);
    std::string compact2 = json::serialize(v2);

    check(compact == compact2, label);
    std::printf("  round-trip OK  : %s\n", label);
}

// ─── error-reporting test ───────────────────────────────────────────────────

static void test_error(const std::string& bad, const char* label) {
    bool threw = false;
    try {
        json::parse(bad);
    } catch (const json::ParseError& e) {
        threw = true;
        std::printf("  parse error OK : %-30s  -> %s\n", label, e.what());
    }
    check(threw, label);
}

// ─── main ────────────────────────────────────────────────────────────────────

int main() {
    // ── sample document ─────────────────────────────────────────────────────
    //
    // Exercises every JSON type, nested structures, all string escapes, and
    // numeric edge cases.

    const std::string sample = R"({
  "title": "JSON round-trip demo",
  "version": 1,
  "pi": 3.141592653589793,
  "big": 1.0e308,
  "neg": -42,
  "flag": true,
  "nothing": null,
  "escapes": "tab:\there\nnewline, quote:\"end\"",
  "unicode": "\u0041\u006E\u0074\u0068\u0072\u006F\u0070\u0069\u0063",
  "surrogate_pair": "\uD83D\uDE00",
  "empty_array": [],
  "empty_object": {},
  "matrix": [
    [1, 0, 0],
    [0, 1, 0],
    [0, 0, 1]
  ],
  "person": {
    "name": "Ada Lovelace",
    "born": 1815,
    "alive": false,
    "notes": null,
    "languages": ["C++", "Python", "Rust"]
  }
})";

    std::puts("=== parsing sample document ===");
    json::Value root = json::parse(sample);

    // Basic type checks on what we parsed.
    check(root.is_object(),                            "root is object");
    check(root.as_object().size() == 15,               "root has 15 keys");

    // Key lookups.
    auto& obj = root.as_object();
    auto get = [&](const std::string& k) -> const json::Value& {
        for (auto& kv : obj)
            if (kv.first == k) return kv.second;
        throw std::runtime_error("key not found: " + k);
    };

    check(get("title").is_string(),                    "title is string");
    check(get("title").as_string() == "JSON round-trip demo",
                                                       "title value");
    check(get("version").is_number(),                  "version is number");
    check(get("version").as_number() == 1.0,           "version value");
    check(get("flag").is_bool(),                       "flag is bool");
    check(get("flag").as_bool() == true,               "flag value");
    check(get("nothing").is_null(),                    "nothing is null");
    check(get("empty_array").is_array(),               "empty_array");
    check(get("empty_array").as_array().empty(),       "empty_array size");
    check(get("empty_object").is_object(),             "empty_object");

    // Nested object.
    auto& person = get("person").as_object();
    auto get_p = [&](const std::string& k) -> const json::Value& {
        for (auto& kv : person)
            if (kv.first == k) return kv.second;
        throw std::runtime_error("key not found in person: " + k);
    };
    check(get_p("name").as_string() == "Ada Lovelace", "person.name");
    check(get_p("born").as_number() == 1815.0,         "person.born");
    check(get_p("alive").as_bool() == false,           "person.alive");
    check(get_p("notes").is_null(),                    "person.notes");
    check(get_p("languages").as_array().size() == 3,   "person.languages size");

    // Unicode escape round-trip: \u0041...\u0063 → "Anthropic"
    check(get("unicode").as_string() == "Anthropic",   "unicode escape");

    // Surrogate-pair escape → U+1F600 GRINNING FACE, decoded to UTF-8.
    check(!get("surrogate_pair").as_string().empty(),  "surrogate pair decoded");

    std::puts("  all checks passed");

    // ── pretty-print ─────────────────────────────────────────────────────────
    std::puts("\n=== pretty-printed output ===");
    std::string pretty = json::serialize(root, /*pretty=*/true, /*indent=*/2);
    std::puts(pretty.c_str());

    // ── compact serialize ────────────────────────────────────────────────────
    std::puts("\n=== compact output ===");
    std::string compact = json::serialize(root, false);
    std::printf("%s\n", compact.c_str());

    // ── round-trip tests ─────────────────────────────────────────────────────
    std::puts("\n=== round-trip tests ===");
    test_roundtrip(sample,          "sample document");
    test_roundtrip("null",          "null");
    test_roundtrip("true",          "true");
    test_roundtrip("false",         "false");
    test_roundtrip("0",             "zero");
    test_roundtrip("-0.0",          "negative zero");
    test_roundtrip("1.0e308",       "large exponent");
    test_roundtrip("\"\"",          "empty string");
    test_roundtrip("\"\\n\\t\\r\"", "whitespace escapes");
    test_roundtrip("[]",            "empty array");
    test_roundtrip("{}",            "empty object");
    test_roundtrip("[1,[2,[3]]]",   "nested arrays");

    // ── error tests ──────────────────────────────────────────────────────────
    std::puts("\n=== error tests ===");
    test_error("",                   "empty input");
    test_error("{",                  "unclosed object");
    test_error("[1,2,",              "unclosed array");
    test_error("{\"a\":}",           "missing value");
    test_error("\"abc",             "unterminated string");
    test_error("\"\\q\"",            "bad escape");
    test_error("1.2.3",             "malformed number");
    test_error("truu",              "bad literal");
    test_error("nul",               "truncated null");
    test_error("[1 2]",             "missing comma");
    test_error("{} extra",          "trailing content");

    std::puts("\n=== all tests passed ===");
    return 0;
}
