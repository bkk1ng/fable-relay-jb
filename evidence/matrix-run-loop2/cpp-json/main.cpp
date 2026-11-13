// main.cpp — C++17 recursive-descent JSON round-trip demo
// Build:  g++ -std=c++17 -O2 -Wall -Wextra -o json_demo main.cpp
// Run:    ./json_demo
#include "json.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

using json::Array;
using json::Object;
using json::ParseError;
using json::Value;

// ---------------------------------------------------------------------------
// Helpers for structural comparison of two Values
// ---------------------------------------------------------------------------

static bool values_equal(const Value& a, const Value& b);

static bool arrays_equal(const Array& a, const Array& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (!values_equal(a[i], b[i])) return false;
    return true;
}

static bool objects_equal(const Object& a, const Object& b) {
    if (a.size() != b.size()) return false;
    for (const auto& [key, val] : a) {
        const Value* other = b.find(key);
        if (!other) return false;
        if (!values_equal(val, *other)) return false;
    }
    return true;
}

static bool values_equal(const Value& a, const Value& b) {
    if (a.data.index() != b.data.index()) return false;
    if (a.is_null())   return true;
    if (a.is_bool())   return a.get_bool()   == b.get_bool();
    if (a.is_number()) {
        // Treat NaN as unequal (NaN != NaN) and compare with a tiny epsilon
        double x = a.get_number();
        double y = b.get_number();
        if (std::isnan(x) || std::isnan(y)) return false;
        return std::abs(x - y) <= 1e-15 * (std::abs(x) + std::abs(y) + 1.0);
    }
    if (a.is_string()) return a.get_string() == b.get_string();
    if (a.is_array())  return arrays_equal(a.get_array(), b.get_array());
    if (a.is_object()) return objects_equal(a.get_object(), b.get_object());
    return false;
}

// ---------------------------------------------------------------------------
// Pretty-print a value recursively with indentation (for the manual walker)
// ---------------------------------------------------------------------------

static void print_value(const Value& v, int depth, std::string& out);

static void print_indent(int depth, std::string& out) {
    out.append(static_cast<std::size_t>(depth * 2), ' ');
}

static void print_array(const Array& arr, int depth, std::string& out) {
    out += "[\n";
    for (std::size_t i = 0; i < arr.size(); ++i) {
        print_indent(depth + 1, out);
        print_value(arr[i], depth + 1, out);
        if (i + 1 < arr.size()) out += ',';
        out += '\n';
    }
    print_indent(depth, out);
    out += ']';
}

static void print_object(const Object& obj, int depth, std::string& out) {
    out += "{\n";
    bool first = true;
    for (const auto& [key, val] : obj) {
        if (!first) out += ",\n";
        first = false;
        print_indent(depth + 1, out);
        // Reuse serializer's string encoder for correct escaping
        json::detail::serialize_string(key, out);
        out += ": ";
        print_value(val, depth + 1, out);
    }
    out += '\n';
    print_indent(depth, out);
    out += '}';
}

static void print_value(const Value& v, int depth, std::string& out) {
    if (v.is_null())        out += "null";
    else if (v.is_bool())   out += v.get_bool() ? "true" : "false";
    else if (v.is_number()) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.17g", v.get_number());
        out += buf;
    }
    else if (v.is_string())  json::detail::serialize_string(v.get_string(), out);
    else if (v.is_array())   print_array(v.get_array(), depth, out);
    else if (v.is_object())  print_object(v.get_object(), depth, out);
}

// ---------------------------------------------------------------------------
// A small helper to label pass/fail
// ---------------------------------------------------------------------------

static const char* pass_fail(bool ok) {
    return ok ? "PASS" : "FAIL";
}

// ---------------------------------------------------------------------------
// Test: error reporting carries the correct byte offset
// ---------------------------------------------------------------------------

static void test_error_offset() {
    std::printf("\n--- Error offset test ---\n");
    // The 'x' at position 3 should be flagged.
    std::string_view bad = "[1, x]";
    try {
        json::parse(bad);
        std::printf("  FAIL: expected a ParseError\n");
    } catch (const ParseError& e) {
        std::printf("  %s — message: %s\n", pass_fail(e.offset == 4), e.what());
    }

    // Truncated object.
    std::string_view bad2 = "{\"k\":}";
    try {
        json::parse(bad2);
        std::printf("  FAIL: expected a ParseError for truncated object\n");
    } catch (const ParseError& e) {
        std::printf("  %s — offset %zu\n", pass_fail(e.offset == 5), e.offset);
    }
}

// ---------------------------------------------------------------------------
// Test: string escape round-trip
// ---------------------------------------------------------------------------

static void test_string_escapes() {
    std::printf("\n--- String escape test ---\n");

    // Raw JSON with assorted escapes including a surrogate pair (U+1F600).
    std::string_view src =
        R"("tab\there\nnewline\"quote\\backslash\u00e9café\ud83d\ude00")";

    try {
        Value v = json::parse(src);
        if (v.is_string()) {
            const std::string& s = v.get_string();
            // Re-serialize and re-parse to confirm round-trip.
            std::string reser = json::serialize(v);
            Value v2 = json::parse(reser);
            bool ok = (s == v2.get_string());
            std::printf("  %s — string length %zu, round-trips through serializer\n",
                        pass_fail(ok), s.size());
        } else {
            std::printf("  FAIL: parsed value is not a string\n");
        }
    } catch (const ParseError& e) {
        std::printf("  FAIL: parse threw: %s\n", e.what());
    }
}

// ---------------------------------------------------------------------------
// Test: number round-trip for tricky IEEE-754 values
// ---------------------------------------------------------------------------

static void test_numbers() {
    std::printf("\n--- Number round-trip test ---\n");

    const double samples[] = {
        0.0,
        -0.0,
        1.0,
        -1.0,
        3.141592653589793,
        1e100,
        -1e-100,
        9007199254740993.0,          // exceeds 2^53
        0.0001,
        123456789.987654321,
    };

    bool all_ok = true;
    for (double original : samples) {
        Value v(original);
        std::string ser = json::serialize(v);
        try {
            Value v2 = json::parse(ser);
            double got = v2.get_number();
            // Exact round-trip for %.17g
            bool exact = (std::abs(got - original) == 0.0) ||
                         (std::isnan(got) && std::isnan(original));
            std::printf("  %s — %.17g -> \"%s\" -> %.17g\n",
                        pass_fail(exact), original, ser.c_str(), got);
            if (!exact) all_ok = false;
        } catch (const ParseError& e) {
            std::printf("  FAIL: parse threw on \"%s\": %s\n", ser.c_str(), e.what());
            all_ok = false;
        }
    }
    std::printf("  overall: %s\n", pass_fail(all_ok));
}

// ---------------------------------------------------------------------------
// Main: parse a sample document, serialize (compact + pretty), round-trip
// ---------------------------------------------------------------------------

int main() {
    // A representative JSON document exercising every type.
    std::string_view sample = R"({
        "version": "1.0.0",
        "debug": true,
        "ratio": null,
        "count": 42,
        "price": 19.99,
        "scientific": 6.02214076e23,
        "unicode": "caf\u00e9 \u00e9moji \ud83d\ude00",
        "escaped": "line1\nline2\ttabbed",
        "tags": ["alpha", "beta", "gamma"],
        "nested": {
            "enabled": false,
            "level": 3,
            "items": [1, 2, 3, {"deep": true}]
        },
        "empty_array": [],
        "empty_object": {}
    })";

    std::printf("=== JSON Recursive-Descent Round-Trip Demo ===\n\n");

    // ---- Parse -------------------------------------------------------------
    Value doc;
    try {
        doc = json::parse(sample);
    } catch (const ParseError& e) {
        std::fprintf(stderr, "Parse error: %s\n", e.what());
        return EXIT_FAILURE;
    }

    std::printf("Parsed successfully. Top-level is %s.\n",
                doc.is_object() ? "object" : "something else");

    // ---- Inspect a few fields ---------------------------------------------
    if (doc.is_object()) {
        const Object& root = doc.get_object();
        if (const Value* v = root.find("version"))
            std::printf("  version  = %s\n", v->get_string().c_str());
        if (const Value* v = root.find("debug"))
            std::printf("  debug    = %s\n", v->get_bool() ? "true" : "false");
        if (const Value* v = root.find("price"))
            std::printf("  price    = %.17g\n", v->get_number());
        if (const Value* v = root.find("ratio"))
            std::printf("  ratio    = %s\n", v->is_null() ? "null" : "?");
        if (const Value* v = root.find("tags"); v && v->is_array())
            std::printf("  tags     = %zu items\n", v->get_array().size());
    }

    // ---- Compact serialization --------------------------------------------
    std::string compact = json::serialize(doc, -1);
    std::printf("\n--- Compact serialization (%zu bytes) ---\n%s\n",
                compact.size(), compact.c_str());

    // ---- Pretty serialization (indent = 2) --------------------------------
    std::string pretty = json::serialize(doc, 2);
    std::printf("\n--- Pretty serialization (indent=2) ---\n%s\n", pretty.c_str());

    // ---- Pretty serialization via manual walker ---------------------------
    std::string manual;
    manual.reserve(256);
    print_value(doc, 0, manual);
    std::printf("\n--- Manual pretty walker ---\n%s\n", manual.c_str());

    // ---- Round-trip verification ------------------------------------------
    std::printf("\n--- Round-trip verification ---\n");

    Value doc_re;
    try {
        doc_re = json::parse(compact);
    } catch (const ParseError& e) {
        std::fprintf(stderr, "Re-parse (compact) failed: %s\n", e.what());
        return EXIT_FAILURE;
    }
    bool compact_ok = values_equal(doc, doc_re);
    std::printf("  compact  parse -> serialize -> parse: %s\n", pass_fail(compact_ok));

    Value doc_re_pretty;
    try {
        doc_re_pretty = json::parse(pretty);
    } catch (const ParseError& e) {
        std::fprintf(stderr, "Re-parse (pretty) failed: %s\n", e.what());
        return EXIT_FAILURE;
    }
    bool pretty_ok = values_equal(doc, doc_re_pretty);
    std::printf("  pretty   parse -> serialize -> parse: %s\n", pass_fail(pretty_ok));

    // ---- Bonus tests -------------------------------------------------------
    test_string_escapes();
    test_numbers();
    test_error_offset();

    std::printf("\n=== Done ===\n");
    return EXIT_SUCCESS;
}
