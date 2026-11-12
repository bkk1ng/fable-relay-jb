// json.hpp — C++17 recursive-descent JSON parser + serializer, no external deps
// Usage:
//   json::Value v  = json::parse(input_string);        // throws json::ParseError on failure
//   std::string s  = json::serialize(v, pretty, indent);

#pragma once

#include <cassert>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace json {

// ---------------------------------------------------------------------------
// Forward declarations and type aliases
// ---------------------------------------------------------------------------
struct Value;
using Array  = std::vector<Value>;
using Object = std::vector<std::pair<std::string, Value>>;  // ordered, allows duplicates

// ---------------------------------------------------------------------------
// Value — the JSON value type
// ---------------------------------------------------------------------------
struct Value {
    using Variant = std::variant<
        std::nullptr_t,  // 0 — null
        bool,            // 1 — boolean
        double,          // 2 — number
        std::string,     // 3 — string
        Array,           // 4 — array
        Object           // 5 — object
    >;

    Variant data;

    // Constructors
    Value()                        : data(nullptr)           {}
    Value(std::nullptr_t)          : data(nullptr)           {}
    Value(bool b)                  : data(b)                 {}
    Value(int    n)                : data(static_cast<double>(n)) {}
    Value(long   n)                : data(static_cast<double>(n)) {}
    Value(double d)                : data(d)                 {}
    Value(const char* s)           : data(std::string(s))    {}
    Value(std::string s)           : data(std::move(s))      {}
    Value(Array  a)                : data(std::move(a))      {}
    Value(Object o)                : data(std::move(o))      {}

    // Type queries
    bool is_null()   const noexcept { return data.index() == 0; }
    bool is_bool()   const noexcept { return data.index() == 1; }
    bool is_number() const noexcept { return data.index() == 2; }
    bool is_string() const noexcept { return data.index() == 3; }
    bool is_array()  const noexcept { return data.index() == 4; }
    bool is_object() const noexcept { return data.index() == 5; }

    // Typed accessors — throw std::bad_variant_access on wrong type
    bool               as_bool()   const { return std::get<bool>(data);        }
    double             as_number() const { return std::get<double>(data);      }
    const std::string& as_string() const { return std::get<std::string>(data); }
    std::string&       as_string()       { return std::get<std::string>(data); }
    const Array&       as_array()  const { return std::get<Array>(data);       }
    Array&             as_array()        { return std::get<Array>(data);       }
    const Object&      as_object() const { return std::get<Object>(data);      }
    Object&            as_object()       { return std::get<Object>(data);      }
};

// ---------------------------------------------------------------------------
// ParseError — carries the byte offset where parsing failed
// ---------------------------------------------------------------------------
class ParseError : public std::runtime_error {
public:
    std::size_t offset;

    ParseError(std::string msg, std::size_t offset)
        : std::runtime_error(msg + " (offset " + std::to_string(offset) + ")")
        , offset(offset)
    {}
};

// ---------------------------------------------------------------------------
// detail — internal implementation
// ---------------------------------------------------------------------------
namespace detail {

// ---- Parser ----------------------------------------------------------------

class Parser {
    std::string_view src_;
    std::size_t      pos_ = 0;

    // ---- helpers -----------------------------------------------------------

    void skip_ws() noexcept {
        while (pos_ < src_.size()) {
            char c = src_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++pos_;
            else break;
        }
    }

    char peek() const noexcept {
        return pos_ < src_.size() ? src_[pos_] : '\0';
    }

    char consume() {
        if (pos_ >= src_.size())
            throw ParseError("unexpected end of input", pos_);
        return src_[pos_++];
    }

    void expect(char c) {
        char got = consume();
        if (got != c) {
            throw ParseError(
                std::string("expected '") + c + "' got '" + got + "'", pos_ - 1);
        }
    }

    bool starts_with(std::string_view s) const noexcept {
        return src_.substr(pos_).substr(0, s.size()) == s;
    }

    // ---- unicode -----------------------------------------------------------

    uint32_t parse_hex4() {
        uint32_t cp = 0;
        for (int i = 0; i < 4; ++i) {
            if (pos_ >= src_.size())
                throw ParseError("incomplete \\uXXXX escape", pos_);
            char c = src_[pos_++];
            cp <<= 4;
            if      (c >= '0' && c <= '9') cp |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') cp |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') cp |= static_cast<uint32_t>(c - 'A' + 10);
            else throw ParseError("bad hex digit in unicode escape", pos_ - 1);
        }
        return cp;
    }

    static void utf8_encode(uint32_t cp, std::string& out) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >>  6) & 0x3F));
            out += static_cast<char>(0x80 | ( cp        & 0x3F));
        }
    }

    // ---- parsers -----------------------------------------------------------

    std::string parse_string() {
        expect('"');
        std::string result;
        result.reserve(32);

        while (true) {
            if (pos_ >= src_.size())
                throw ParseError("unterminated string", pos_);

            unsigned char c = static_cast<unsigned char>(src_[pos_++]);

            if (c == '"') {
                break;
            } else if (c == '\\') {
                if (pos_ >= src_.size())
                    throw ParseError("unterminated escape sequence", pos_);
                char esc = src_[pos_++];
                switch (esc) {
                    case '"':  result += '"';  break;
                    case '\\': result += '\\'; break;
                    case '/':  result += '/';  break;
                    case 'b':  result += '\b'; break;
                    case 'f':  result += '\f'; break;
                    case 'n':  result += '\n'; break;
                    case 'r':  result += '\r'; break;
                    case 't':  result += '\t'; break;
                    case 'u': {
                        uint32_t hi = parse_hex4();
                        // Surrogate pair: high surrogate D800–DBFF must be followed by
                        // a low surrogate DC00–DFFF via another \uXXXX sequence.
                        if (hi >= 0xD800 && hi <= 0xDBFF) {
                            if (pos_ + 1 < src_.size() &&
                                src_[pos_] == '\\' && src_[pos_ + 1] == 'u') {
                                pos_ += 2;
                                uint32_t lo = parse_hex4();
                                if (lo < 0xDC00 || lo > 0xDFFF)
                                    throw ParseError("invalid low surrogate", pos_ - 4);
                                uint32_t cp = 0x10000 + ((hi - 0xD800) << 10) + (lo - 0xDC00);
                                utf8_encode(cp, result);
                            } else {
                                throw ParseError("missing low surrogate after high surrogate", pos_);
                            }
                        } else if (hi >= 0xDC00 && hi <= 0xDFFF) {
                            throw ParseError("unexpected low surrogate", pos_ - 4);
                        } else {
                            utf8_encode(hi, result);
                        }
                        break;
                    }
                    default:
                        throw ParseError(
                            std::string("invalid escape character '\\") + esc + "'", pos_ - 1);
                }
            } else if (c < 0x20) {
                throw ParseError("unescaped control character in string", pos_ - 1);
            } else {
                result += static_cast<char>(c);
            }
        }
        return result;
    }

    Value parse_number() {
        std::size_t start = pos_;

        if (pos_ < src_.size() && src_[pos_] == '-') ++pos_;

        if (pos_ >= src_.size())
            throw ParseError("expected digit", pos_);

        if (src_[pos_] == '0') {
            ++pos_;
        } else if (src_[pos_] >= '1' && src_[pos_] <= '9') {
            while (pos_ < src_.size() && src_[pos_] >= '0' && src_[pos_] <= '9')
                ++pos_;
        } else {
            throw ParseError("invalid number", pos_);
        }

        // Fractional part
        if (pos_ < src_.size() && src_[pos_] == '.') {
            ++pos_;
            if (pos_ >= src_.size() || src_[pos_] < '0' || src_[pos_] > '9')
                throw ParseError("expected digit after '.'", pos_);
            while (pos_ < src_.size() && src_[pos_] >= '0' && src_[pos_] <= '9')
                ++pos_;
        }

        // Exponent part
        if (pos_ < src_.size() && (src_[pos_] == 'e' || src_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < src_.size() && (src_[pos_] == '+' || src_[pos_] == '-'))
                ++pos_;
            if (pos_ >= src_.size() || src_[pos_] < '0' || src_[pos_] > '9')
                throw ParseError("expected digit in exponent", pos_);
            while (pos_ < src_.size() && src_[pos_] >= '0' && src_[pos_] <= '9')
                ++pos_;
        }

        std::string token(src_.substr(start, pos_ - start));
        char* endptr = nullptr;
        errno = 0;
        double d = std::strtod(token.c_str(), &endptr);
        if (errno != 0 || endptr != token.c_str() + token.size())
            throw ParseError("number parse failed: " + token, start);
        return Value(d);
    }

    Value parse_literal(std::string_view text, Value result) {
        if (!starts_with(text))
            throw ParseError(
                "invalid literal near '" + std::string(src_.substr(pos_, text.size())) + "'",
                pos_);
        pos_ += text.size();
        return result;
    }

    Value parse_array() {
        expect('[');
        Array arr;
        skip_ws();
        if (peek() == ']') { ++pos_; return Value(std::move(arr)); }

        while (true) {
            arr.push_back(parse_value());
            skip_ws();
            char c = peek();
            if (c == ']') { ++pos_; break; }
            if (c != ',') throw ParseError("expected ',' or ']' in array", pos_);
            ++pos_;  // consume ','
        }
        return Value(std::move(arr));
    }

    Value parse_object() {
        expect('{');
        Object obj;
        skip_ws();
        if (peek() == '}') { ++pos_; return Value(std::move(obj)); }

        while (true) {
            skip_ws();
            if (peek() != '"')
                throw ParseError("expected string key in object", pos_);
            std::string key = parse_string();
            skip_ws();
            expect(':');
            Value val = parse_value();
            obj.emplace_back(std::move(key), std::move(val));
            skip_ws();
            char c = peek();
            if (c == '}') { ++pos_; break; }
            if (c != ',') throw ParseError("expected ',' or '}' in object", pos_);
            ++pos_;  // consume ','
        }
        return Value(std::move(obj));
    }

    Value parse_value() {
        skip_ws();
        if (pos_ >= src_.size())
            throw ParseError("unexpected end of input", pos_);

        switch (peek()) {
            case '"': return Value(parse_string());
            case '[': return parse_array();
            case '{': return parse_object();
            case 't': return parse_literal("true",  Value(true));
            case 'f': return parse_literal("false", Value(false));
            case 'n': return parse_literal("null",  Value(nullptr));
            default:
                if (peek() == '-' || (peek() >= '0' && peek() <= '9'))
                    return parse_number();
                throw ParseError(
                    std::string("unexpected character '") + peek() + "'", pos_);
        }
    }

public:
    explicit Parser(std::string_view src) : src_(src) {}

    Value parse() {
        Value v = parse_value();
        skip_ws();
        if (pos_ != src_.size())
            throw ParseError("trailing content after JSON value", pos_);
        return v;
    }
};

// ---- Serializer ------------------------------------------------------------

class Serializer {
    bool        pretty_;
    int         indent_width_;
    int         depth_ = 0;
    std::string out_;

    void indent() {
        out_ += '\n';
        out_.append(static_cast<std::size_t>(depth_ * indent_width_), ' ');
    }

    void write_string(const std::string& s) {
        out_ += '"';
        for (unsigned char c : s) {
            switch (c) {
                case '"':  out_ += "\\\""; break;
                case '\\': out_ += "\\\\"; break;
                case '\b': out_ += "\\b";  break;
                case '\f': out_ += "\\f";  break;
                case '\n': out_ += "\\n";  break;
                case '\r': out_ += "\\r";  break;
                case '\t': out_ += "\\t";  break;
                default:
                    if (c < 0x20) {
                        char buf[8];
                        std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                        out_ += buf;
                    } else {
                        out_ += static_cast<char>(c);
                    }
            }
        }
        out_ += '"';
    }

    void write_number(double d) {
        if (std::isnan(d) || std::isinf(d))
            throw std::runtime_error("cannot serialize NaN or Infinity as JSON");

        char buf[32];
        // %.17g gives a round-trippable representation while stripping
        // redundant trailing zeros (unlike fixed-precision %f).
        int n = std::snprintf(buf, sizeof(buf), "%.17g", d);
        out_.append(buf, static_cast<std::size_t>(n));
    }

    void write(const Value& v) {
        std::visit([&](const auto& x) { write_one(x); }, v.data);
    }

    void write_one(std::nullptr_t)      { out_ += "null"; }
    void write_one(bool b)              { out_ += b ? "true" : "false"; }
    void write_one(double d)            { write_number(d); }
    void write_one(const std::string& s){ write_string(s); }

    void write_one(const Array& arr) {
        if (arr.empty()) { out_ += "[]"; return; }
        out_ += '[';
        ++depth_;
        for (std::size_t i = 0; i < arr.size(); ++i) {
            if (pretty_) indent();
            write(arr[i]);
            if (i + 1 < arr.size()) out_ += ',';
        }
        --depth_;
        if (pretty_) indent();
        out_ += ']';
    }

    void write_one(const Object& obj) {
        if (obj.empty()) { out_ += "{}"; return; }
        out_ += '{';
        ++depth_;
        for (std::size_t i = 0; i < obj.size(); ++i) {
            if (pretty_) indent();
            write_string(obj[i].first);
            out_ += ':';
            if (pretty_) out_ += ' ';
            write(obj[i].second);
            if (i + 1 < obj.size()) out_ += ',';
        }
        --depth_;
        if (pretty_) indent();
        out_ += '}';
    }

public:
    Serializer(bool pretty, int indent_width)
        : pretty_(pretty), indent_width_(indent_width) {}

    std::string serialize(const Value& v) {
        out_.clear();
        depth_ = 0;
        write(v);
        return out_;
    }
};

}  // namespace detail

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

/// Parse JSON text. Throws ParseError on any syntax error.
inline Value parse(std::string_view input) {
    return detail::Parser(input).parse();
}

/// Serialize a Value to a JSON string.
/// pretty=false → compact (no whitespace)
/// pretty=true  → indented, with indent_width spaces per level
inline std::string serialize(const Value& v, bool pretty = false, int indent_width = 2) {
    return detail::Serializer(pretty, indent_width).serialize(v);
}

}  // namespace json
