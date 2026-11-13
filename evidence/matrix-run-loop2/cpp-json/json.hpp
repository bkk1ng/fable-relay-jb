// json.hpp — C++17 recursive-descent JSON parser + serializer, no external deps
#pragma once

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace json {

// ---------------------------------------------------------------------------
// Forward declarations for recursive value types
// ---------------------------------------------------------------------------
struct Array;
struct Object;

// ---------------------------------------------------------------------------
// Value
//   Variant over: null | bool | double | string | Array | Object
//   Array and Object are heap-allocated via shared_ptr so that Value has a
//   fixed, known size before Array/Object are fully defined.
// ---------------------------------------------------------------------------
struct Value {
    using Null      = std::nullptr_t;
    using Bool      = bool;
    using Number    = double;
    using String    = std::string;
    using ArrayPtr  = std::shared_ptr<Array>;
    using ObjectPtr = std::shared_ptr<Object>;

    std::variant<Null, Bool, Number, String, ArrayPtr, ObjectPtr> data;

    // --- constructors -------------------------------------------------------
    Value() noexcept : data(nullptr) {}
    /* implicit */ Value(std::nullptr_t) noexcept : data(nullptr) {}
    // 'explicit' on bool prevents silent int→bool coercion.
    explicit Value(bool b) noexcept : data(b) {}
    /* implicit */ Value(double d) noexcept : data(d) {}
    /* implicit */ Value(std::string s) noexcept : data(std::move(s)) {}
    /* implicit */ Value(const char* s) : data(std::string(s)) {}
    /* implicit */ Value(ArrayPtr a) noexcept : data(std::move(a)) {}
    /* implicit */ Value(ObjectPtr o) noexcept : data(std::move(o)) {}

    // --- type queries -------------------------------------------------------
    bool is_null()   const noexcept { return std::holds_alternative<Null>(data); }
    bool is_bool()   const noexcept { return std::holds_alternative<Bool>(data); }
    bool is_number() const noexcept { return std::holds_alternative<Number>(data); }
    bool is_string() const noexcept { return std::holds_alternative<String>(data); }
    bool is_array()  const noexcept { return std::holds_alternative<ArrayPtr>(data); }
    bool is_object() const noexcept { return std::holds_alternative<ObjectPtr>(data); }

    // --- typed accessors (throw std::bad_variant_access on wrong type) ------
    Bool          get_bool()   const { return std::get<Bool>(data); }
    Number        get_number() const { return std::get<Number>(data); }
    const String& get_string() const { return std::get<String>(data); }
    String&       get_string()       { return std::get<String>(data); }
    const Array&  get_array()  const { return *std::get<ArrayPtr>(data); }
    Array&        get_array()        { return *std::get<ArrayPtr>(data); }
    const Object& get_object() const { return *std::get<ObjectPtr>(data); }
    Object&       get_object()       { return *std::get<ObjectPtr>(data); }
};

// ---------------------------------------------------------------------------
// Array — ordered list of Values
// ---------------------------------------------------------------------------
struct Array {
    std::vector<Value> items;

    std::size_t  size()  const noexcept { return items.size(); }
    bool         empty() const noexcept { return items.empty(); }

    Value&       operator[](std::size_t i)       { return items[i]; }
    const Value& operator[](std::size_t i) const { return items[i]; }

    void push_back(Value v) { items.push_back(std::move(v)); }

    auto begin()       { return items.begin(); }
    auto end()         { return items.end(); }
    auto begin() const { return items.begin(); }
    auto end()   const { return items.end(); }
};

// ---------------------------------------------------------------------------
// Object — ordered list of key/value pairs (preserves insertion order)
// ---------------------------------------------------------------------------
struct Object {
    std::vector<std::pair<std::string, Value>> items;

    std::size_t size()  const noexcept { return items.size(); }
    bool        empty() const noexcept { return items.empty(); }

    // Returns nullptr when key is absent.
    Value* find(std::string_view key) noexcept {
        for (auto& [k, v] : items)
            if (k == key) return &v;
        return nullptr;
    }
    const Value* find(std::string_view key) const noexcept {
        for (const auto& [k, v] : items)
            if (k == key) return &v;
        return nullptr;
    }

    void insert(std::string key, Value val) {
        items.emplace_back(std::move(key), std::move(val));
    }

    auto begin()       { return items.begin(); }
    auto end()         { return items.end(); }
    auto begin() const { return items.begin(); }
    auto end()   const { return items.end(); }
};

// ---------------------------------------------------------------------------
// ParseError — carries the byte offset into the input
// ---------------------------------------------------------------------------
class ParseError : public std::runtime_error {
public:
    std::size_t offset; ///< Zero-based byte offset in the source string

    ParseError(std::string msg, std::size_t off)
        : std::runtime_error(std::move(msg) + " (offset " + std::to_string(off) + ")")
        , offset(off)
    {}
};

// ===========================================================================
// Implementation (header-only)
// ===========================================================================
namespace detail {

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------
class Parser {
public:
    explicit Parser(std::string_view src) noexcept
        : src_(src), pos_(0)
    {}

    Value parse() {
        skip_ws();
        Value v = parse_value();
        skip_ws();
        if (pos_ != src_.size())
            error("unexpected trailing content");
        return v;
    }

private:
    std::string_view src_;
    std::size_t      pos_;

    // ---- error helper ------------------------------------------------------
    [[noreturn]] void error(const char* msg) const {
        throw ParseError(msg, pos_);
    }

    // ---- character helpers -------------------------------------------------
    bool at_end() const noexcept { return pos_ >= src_.size(); }

    char peek() const noexcept {
        return at_end() ? '\0' : src_[pos_];
    }

    char advance() {
        if (at_end()) error("unexpected end of input");
        return src_[pos_++];
    }

    void expect(char ch) {
        char got = advance();
        if (got != ch) error("unexpected character");
    }

    bool match(char ch) noexcept {
        if (peek() == ch) { ++pos_; return true; }
        return false;
    }

    void skip_ws() noexcept {
        while (!at_end()) {
            char c = src_[pos_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
                ++pos_;
            else
                break;
        }
    }

    // ---- literal helpers ---------------------------------------------------
    void consume_literal(const char* lit) {
        for (std::size_t i = 1; lit[i]; ++i) { // skip lit[0], already peeked
            if (at_end() || src_[pos_] != lit[i])
                error("invalid literal");
            ++pos_;
        }
    }

    // ---- Unicode -----------------------------------------------------------
    // Parse four hex digits and return the code unit.
    std::uint16_t parse_hex4() {
        std::uint16_t cp = 0;
        for (int i = 0; i < 4; ++i) {
            char c = advance();
            cp <<= 4;
            if      (c >= '0' && c <= '9') cp |= static_cast<std::uint16_t>(c - '0');
            else if (c >= 'a' && c <= 'f') cp |= static_cast<std::uint16_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') cp |= static_cast<std::uint16_t>(c - 'A' + 10);
            else error("invalid hex digit in \\uXXXX");
        }
        return cp;
    }

    // Encode a Unicode code point (U+0000 .. U+10FFFF) as UTF-8.
    static void encode_utf8(std::uint32_t cp, std::string& out) {
        if (cp <= 0x7F) {
            out += static_cast<char>(cp);
        } else if (cp <= 0x7FF) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp <= 0xFFFF) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    // ---- String parsing ----------------------------------------------------
    std::string parse_string_raw() {
        expect('"');
        std::string out;
        out.reserve(32);

        while (true) {
            if (at_end()) error("unterminated string");
            char c = advance();

            if (c == '"') break;

            if (c == '\\') {
                // Escape sequence
                char esc = advance();
                switch (esc) {
                    case '"':  out += '"';  break;
                    case '\\': out += '\\'; break;
                    case '/':  out += '/';  break;
                    case 'b':  out += '\b'; break;
                    case 'f':  out += '\f'; break;
                    case 'n':  out += '\n'; break;
                    case 'r':  out += '\r'; break;
                    case 't':  out += '\t'; break;
                    case 'u': {
                        std::uint16_t hi = parse_hex4();
                        // Handle surrogate pair
                        if (hi >= 0xD800 && hi <= 0xDBFF) {
                            // Expect \uDC00–\uDFFF
                            if (at_end() || src_[pos_] != '\\')
                                error("missing low surrogate");
                            ++pos_;
                            if (at_end() || src_[pos_] != 'u')
                                error("missing low surrogate");
                            ++pos_;
                            std::uint16_t lo = parse_hex4();
                            if (lo < 0xDC00 || lo > 0xDFFF)
                                error("invalid low surrogate");
                            std::uint32_t cp = 0x10000u
                                + (static_cast<std::uint32_t>(hi - 0xD800) << 10)
                                + static_cast<std::uint32_t>(lo - 0xDC00);
                            encode_utf8(cp, out);
                        } else if (hi >= 0xDC00 && hi <= 0xDFFF) {
                            error("unexpected low surrogate");
                        } else {
                            encode_utf8(static_cast<std::uint32_t>(hi), out);
                        }
                        break;
                    }
                    default:
                        error("invalid escape character");
                }
            } else if (static_cast<unsigned char>(c) < 0x20) {
                // Control characters must be escaped in JSON
                error("unescaped control character in string");
            } else {
                out += c;
            }
        }
        return out;
    }

    // ---- Number parsing ----------------------------------------------------
    Value parse_number() {
        std::size_t start = pos_;
        // Optional minus
        if (peek() == '-') ++pos_;

        if (at_end()) error("unexpected end of number");

        if (peek() == '0') {
            ++pos_;
            // Leading zero — next char must not be a digit
            if (!at_end() && src_[pos_] >= '0' && src_[pos_] <= '9')
                error("leading zeros are not allowed");
        } else if (peek() >= '1' && peek() <= '9') {
            while (!at_end() && src_[pos_] >= '0' && src_[pos_] <= '9')
                ++pos_;
        } else {
            error("invalid number");
        }

        // Optional fractional part
        if (!at_end() && src_[pos_] == '.') {
            ++pos_;
            if (at_end() || src_[pos_] < '0' || src_[pos_] > '9')
                error("digit expected after decimal point");
            while (!at_end() && src_[pos_] >= '0' && src_[pos_] <= '9')
                ++pos_;
        }

        // Optional exponent
        if (!at_end() && (src_[pos_] == 'e' || src_[pos_] == 'E')) {
            ++pos_;
            if (!at_end() && (src_[pos_] == '+' || src_[pos_] == '-'))
                ++pos_;
            if (at_end() || src_[pos_] < '0' || src_[pos_] > '9')
                error("digit expected in exponent");
            while (!at_end() && src_[pos_] >= '0' && src_[pos_] <= '9')
                ++pos_;
        }

        // Convert the substring to double
        std::string token(src_.substr(start, pos_ - start));
        char* endptr = nullptr;
        double d = std::strtod(token.c_str(), &endptr);
        if (endptr != token.c_str() + token.size())
            error("malformed number");
        return Value(d);
    }

    // ---- Array parsing -----------------------------------------------------
    Value parse_array() {
        expect('[');
        auto arr = std::make_shared<Array>();
        skip_ws();
        if (match(']')) return Value(std::move(arr)); // empty array

        while (true) {
            skip_ws();
            arr->push_back(parse_value());
            skip_ws();
            char c = peek();
            if (c == ']') { advance(); break; }
            if (c == ',') { advance(); continue; }
            error("expected ',' or ']' in array");
        }
        return Value(std::move(arr));
    }

    // ---- Object parsing ----------------------------------------------------
    Value parse_object() {
        expect('{');
        auto obj = std::make_shared<Object>();
        skip_ws();
        if (match('}')) return Value(std::move(obj)); // empty object

        while (true) {
            skip_ws();
            if (peek() != '"') error("expected string key in object");
            std::string key = parse_string_raw();
            skip_ws();
            expect(':');
            skip_ws();
            Value val = parse_value();
            obj->insert(std::move(key), std::move(val));
            skip_ws();
            char c = peek();
            if (c == '}') { advance(); break; }
            if (c == ',') { advance(); continue; }
            error("expected ',' or '}' in object");
        }
        return Value(std::move(obj));
    }

    // ---- Dispatch ----------------------------------------------------------
    Value parse_value() {
        skip_ws();
        if (at_end()) error("unexpected end of input");

        char c = peek();
        switch (c) {
            case 'n': ++pos_; consume_literal("null");  return Value(nullptr);
            case 't': ++pos_; consume_literal("true");  return Value(true);
            case 'f': ++pos_; consume_literal("false"); return Value(false);
            case '"': return Value(parse_string_raw());
            case '[': return parse_array();
            case '{': return parse_object();
            default:
                if (c == '-' || (c >= '0' && c <= '9'))
                    return parse_number();
                error("unexpected character");
        }
    }
}; // class Parser

// ---------------------------------------------------------------------------
// Serializer
// ---------------------------------------------------------------------------

// Emit a string as a JSON-encoded "quoted string".
inline void serialize_string(const std::string& s, std::string& out) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    // Encode as \uXXXX
                    char buf[7];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
                break;
        }
    }
    out += '"';
}

// indent_w: spaces per level, -1 means compact
inline void serialize_value(const Value& v, int indent_w, int depth, std::string& out) {
    auto nl_indent = [&](int d) {
        if (indent_w < 0) return;
        out += '\n';
        out.append(static_cast<std::size_t>(d * indent_w), ' ');
    };

    if (v.is_null()) {
        out += "null";
    } else if (v.is_bool()) {
        out += v.get_bool() ? "true" : "false";
    } else if (v.is_number()) {
        double d = v.get_number();
        // Use enough precision for a lossless round-trip (17 significant digits).
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.17g", d);
        // Ensure the output is not mistaken for an integer when read back.
        // If there's no '.' or 'e'/'E', append ".0" is not strictly needed for
        // JSON (bare integer-shaped doubles are valid), so we leave it as-is;
        // strtod will recover the exact value from %.17g.
        out += buf;
    } else if (v.is_string()) {
        serialize_string(v.get_string(), out);
    } else if (v.is_array()) {
        const Array& arr = v.get_array();
        out += '[';
        for (std::size_t i = 0; i < arr.size(); ++i) {
            if (i > 0) out += ',';
            nl_indent(depth + 1);
            serialize_value(arr[i], indent_w, depth + 1, out);
        }
        if (!arr.empty()) nl_indent(depth);
        out += ']';
    } else if (v.is_object()) {
        const Object& obj = v.get_object();
        out += '{';
        bool first = true;
        for (const auto& [key, val] : obj) {
            if (!first) out += ',';
            first = false;
            nl_indent(depth + 1);
            serialize_string(key, out);
            out += ':';
            if (indent_w >= 0) out += ' ';
            serialize_value(val, indent_w, depth + 1, out);
        }
        if (!obj.empty()) nl_indent(depth);
        out += '}';
    }
}

} // namespace detail

// ===========================================================================
// Public API
// ===========================================================================

/// Parse a JSON text.  Throws ParseError on any syntax or structural error.
inline Value parse(std::string_view input) {
    return detail::Parser(input).parse();
}

/// Serialize a Value to a JSON string.
/// \param indent   Number of spaces per indentation level.
///                 Pass -1 (default) for compact single-line output.
///                 Pass 0 or more for pretty-printed multi-line output.
inline std::string serialize(const Value& v, int indent = -1) {
    std::string out;
    out.reserve(256);
    detail::serialize_value(v, indent, 0, out);
    return out;
}

} // namespace json
