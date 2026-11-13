#include "json.hpp"
#include <iostream>
#include <string>
#include <cassert>
#include <limits>

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

static void print_separator(const char* label) {
    std::cout << "\n--- " << label << " ---\n";
}

// ---------------------------------------------------------------------------
// Round-trip test: parse src, serialize compact, parse again, serialize again.
// The two compact forms must match.
// ---------------------------------------------------------------------------

static bool round_trip(const std::string& src, bool verbose = false) {
    Value first   = parse(src);
    std::string compact1 = serialize(first);
    Value second  = parse(compact1);
    std::string compact2 = serialize(second);

    bool ok = (compact1 == compact2);
    if (verbose) {
        std::cout << "  original  : " << src      << '\n';
        std::cout << "  compact 1 : " << compact1 << '\n';
        std::cout << "  compact 2 : " << compact2 << '\n';
        std::cout << "  round-trip: " << (ok ? "PASS" : "FAIL") << '\n';
    }
    return ok;
}

int main() {
    // -----------------------------------------------------------------------
    // 1. Sample document — exercises every Value type and string escapes
    // -----------------------------------------------------------------------
    const std::string sample = R"({
    "project": "json.hpp demo",
    "version": 1,
    "stable": true,
    "ratio": 3.14159265358979,
    "nothing": null,
    "tags": ["parser", "serializer", "C++17"],
    "nested": {
        "depth": 2,
        "matrix": [[1, 0], [0, 1]]
    },
    "escapes": "tab:\there\nnewline, quote:\"hi\", solidus:\/end",
    "unicode": "\u0048\u0065\u006C\u006C\u006F \u4e16\u754c",
    "surrogate_pair": "\uD83D\uDE00",
    "empty_obj": {},
    "empty_arr": []
})";

    print_separator("1. Sample document — pretty-printed");
    Value doc = parse(sample);
    std::cout << serialize_pretty(doc) << '\n';

    // -----------------------------------------------------------------------
    // 2. Round-trip the sample document
    // -----------------------------------------------------------------------
    print_separator("2. Round-trip verification");
    bool ok = round_trip(sample, /*verbose=*/true);
    assert(ok && "round-trip failed on sample document");

    // -----------------------------------------------------------------------
    // 3. Individual value types
    // -----------------------------------------------------------------------
    print_separator("3. Individual value types");

    auto show = [](const char* label, const Value& v) {
        std::cout << "  " << label << " -> " << serialize(v) << '\n';
    };

    show("null",          Value{});
    show("true",          Value{true});
    show("false",         Value{false});
    show("integer 42",    Value{42});
    show("float  2.5",    Value{2.5});
    show("large int",     Value{1000000LL});
    show("string",        Value{"hello \"world\""});
    show("empty string",  Value{""});

    Array arr;
    arr.push_back(Value{1});
    arr.push_back(Value{"two"});
    arr.push_back(Value{false});
    arr.push_back(Value{});
    show("array",         Value{arr});

    Object obj;
    obj["x"] = Value{10};
    obj["y"] = Value{20};
    show("object",        Value{obj});

    // -----------------------------------------------------------------------
    // 4. String escape round-trips
    // -----------------------------------------------------------------------
    print_separator("4. String escape round-trips");

    auto str_rt = [](const char* desc, const std::string& json_str) {
        // json_str must be a full JSON string literal, e.g. "\"hi\\n\""
        Value v = parse(json_str);
        std::string back = serialize(v);
        bool ok = round_trip(json_str);
        std::cout << "  " << desc << "\n"
                  << "    parsed  : " << v.as_string() << '\n'
                  << "    re-ser  : " << back << '\n'
                  << "    ok      : " << (ok ? "yes" : "NO") << '\n';
    };

    str_rt("backslash + quote",  R"("back\\slash and \"quote\"")");
    str_rt("control chars",      R"("bell:\u0007 tab:\t newline:\n cr:\r")");
    str_rt("BMP codepoint",      R"("\u00e9l\u00e8ve")");   // élève
    str_rt("surrogate pair 😀",  R"("\uD83D\uDE00")");

    // -----------------------------------------------------------------------
    // 5. Error handling
    // -----------------------------------------------------------------------
    print_separator("5. Error handling");

    auto expect_error = [](const char* desc, const std::string& bad) {
        try {
            parse(bad);
            std::cout << "  " << desc << " -> ERROR: no exception thrown!\n";
        } catch (const ParseError& e) {
            std::cout << "  " << desc << " -> caught: " << e.what() << '\n';
        }
    };

    expect_error("trailing garbage",    R"(42 oops)");
    expect_error("bad literal",         R"(treu)");
    expect_error("unterminated string", R"("hello)");
    expect_error("bad escape",          R"("bad \q escape")");
    expect_error("lone high surrogate", R"("\uD800")");
    expect_error("bad number",          R"(1.)");
    expect_error("missing colon",       R"({"key" "val"})");
    expect_error("missing comma array", R"([1 2 3])");
    expect_error("unexpected char",     R"(@)");

    // -----------------------------------------------------------------------
    // 6. Compact vs pretty serializer output
    // -----------------------------------------------------------------------
    print_separator("6. Compact vs pretty");

    const std::string small = R"({"a":1,"b":[true,null]})";
    Value sv = parse(small);
    std::cout << "  compact : " << serialize(sv)           << '\n';
    std::cout << "  pretty  :\n" << serialize_pretty(sv, 2) << '\n';

    // -----------------------------------------------------------------------
    // 7. NaN / Inf serialization safety
    // -----------------------------------------------------------------------
    print_separator("7. NaN/Inf -> null convention");
    show("NaN",  Value{std::numeric_limits<double>::quiet_NaN()});
    show("Inf",  Value{std::numeric_limits<double>::infinity()});
    show("-Inf", Value{-std::numeric_limits<double>::infinity()});

    // -----------------------------------------------------------------------
    // Done
    // -----------------------------------------------------------------------
    print_separator("done");
    std::cout << "All assertions passed.\n";
    return 0;
}
