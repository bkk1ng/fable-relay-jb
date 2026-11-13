#pragma once

// C++17 recursive-descent JSON parser + serializer — no external dependencies.
//
// Quick usage:
//   json::Value v = json::parse(src);          // throws json::ParseError on failure
//   std::string s = json::serialize(v);         // compact
//   std::string p = json::serialize(v, true);   // pretty (2-space indent)

#include <cassert>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace json {

// ─── forward declaration ────────────────────────────────────────────────────

struct Value;

// Ordered key-value sequence preserves insertion order.
using Array  = std::vector<Value>;
using Object = std::vector<std::pair<std::string, Value>>;

// ─── error ──────────────────────────────────────────────────────────────────

struct ParseError : std::runtime_error {
    std::size_t offset;

    ParseError(std::string const& msg, std::size_t off)
        : std::runtime_error(msg + " (at offset " + std::to_string(off) + ")")
        , offset(off)
    {}
};

// ─── value ──────────────────────────────────────────────────────────────────

struct Value {
    // Variant index mirrors the JSON spec ordering: null < bool < number < string < array < object.
    std::variant<std::nullptr_t, bool, double, std::string, Array, Object> v;

    // ── constructors ────────────────────────────────────────────────────────
    Value()                      : v(nullptr)       {}
    Value(std::nullptr_t)        : v(nullptr)       {}
    Value(bool b)                : v(b)             {}
    Value(double d)              : v(d)             {}
    Value(int i)                 : v(double(i))     {}
    Value(long i)                : v(double(i))     {}
    Value(long long i)           : v(double(i))     {}
    Value(std::string s)         : v(std::move(s))  {}
    Value(const char* s)         : v(std::string(s)){}
    Value(Array a)               : v(std::move(a))  {}
    Value(Object o)              : v(std::move(o))  {}

    // ── type queries ────────────────────────────────────────────────────────
    bool is_null()   const { return std::holds_alternative<std::nullptr_t>(v); }
    bool is_bool()   const { return std::holds_alternative<bool>(v);           }
    bool is_number() const { return std::holds_alternative<double>(v);         }
    bool is_string() const { return std::holds_alternative<std::string>(v);    }
    bool is_array()  const { return std::holds_alternative<Array>(v);          }
    bool is_object() const { return std::holds_alternative<Object>(v);         }

    // ── accessors (throw std::bad_variant_access on wrong type) ─────────────
    bool&        as_bool()   { return std::get<bool>(v);        }
    double&      as_number() { return std::get<double>(v);      }
    std::string& as_string() { return std::get<std::string>(v); }
    Array&       as_array()  { return std::get<Array>(v);       }
    Object&      as_object() { return std::get<Object>(v);      }

    bool        const& as_bool()   const { return std::get<bool>(v);        }
    double      const& as_number() const { return std::get<double>(v);      }
    std::string const& as_string() const { return std::get<std::string>(v); }
    Array       const& as_array()  const { return std::get<Array>(v);       }
    Object      const& as_object() const { return std::get<Object>(v);      }
};

// ─── implementation detail ──────────────────────────────────────────────────

namespace detail {

// ── parser ──────────────────────────────────────────────────────────────────

class Parser {
    std::string_view src_;
    std::size_t      pos_ = 0;

    // ── low-level helpers ───────────────────────────────────────────────────

    void skip_ws() noexcept {
        while (pos_ < src_.size()) {
            char c = src_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++pos_;
            else break;
        }
    }

    [[nodiscard]] char peek() const noexcept {
        return pos_ < src_.size() ? src_[pos_] : '\0';
    }

    char consume() {
        if (pos_ >= src_.size())
            throw ParseError("unexpected end of input", pos_);
        return src_[pos_++];
    }

    // Parse exactly 4 hex digits for a \uXXXX escape.
    uint32_t parse_hex4() {
        uint32_t val = 0;
        for (int i = 0; i < 4; ++i) {
            if (pos_ >= src_.size())
                throw ParseError("truncated \\uXXXX escape", pos_);
            char c = src_[pos_++];
            val <<= 4;
            if      (c >= '0' && c <= '9') val |= uint32_t(c - '0');
            else if (c >= 'a' && c <= 'f') val |= uint32_t(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') val |= uint32_t(c - 'A' + 10);
            else throw ParseError("invalid hex digit in \\u escape", pos_ - 1);
        }
        return val;
    }

    // Encode a Unicode code point as UTF-8 into out.
    static void encode_utf8(uint32_t cp, std::string& out) {
        if (cp <= 0x7F) {
            out += char(cp);
        } else if (cp <= 0x7FF) {
            out += char(0xC0 | (cp >> 6));
            out += char(0x80 | (cp & 0x3F));
        } else if (cp <= 0xFFFF) {
            out += char(0xE0 | (cp >> 12));
            out += char(0x80 | ((cp >> 6) & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        } else if (cp <= 0x10FFFF) {
            out += char(0xF0 | (cp >> 18));
            out += char(0x80 | ((cp >> 12) & 0x3F));
            out += char(0x80 | ((cp >> 6)  & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        }
        // code points > 0x10FFFF are caught before we reach here
    }

    // ── string ──────────────────────────────────────────────────────────────

    // Called after the opening '"' has been consumed.
    std::string parse_string_content() {
        std::string out;
        out.reserve(16);
        while (true) {
            if (pos_ >= src_.size())
                throw ParseError("unterminated string", pos_);

            unsigned char c = static_cast<unsigned char>(src_[pos_++]);

            if (c == '"') {
                break;
            } else if (c == '\\') {
                // escape sequence
                if (pos_ >= src_.size())
                    throw ParseError("truncated escape sequence", pos_);
                char esc = src_[pos_++];
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
                        uint32_t cp = parse_hex4();
                        // Handle surrogate pairs (UTF-16 encoded supplementary planes).
                        if (cp >= 0xD800 && cp <= 0xDBFF) {
                            // High surrogate — must be followed by \uDCxx...\uDFxx.
                            if (pos_ + 1 < src_.size() &&
                                src_[pos_] == '\\' && src_[pos_ + 1] == 'u')
                            {
                                pos_ += 2;
                                uint32_t low = parse_hex4();
                                if (low >= 0xDC00 && low <= 0xDFFF) {
                                    cp = 0x10000u + ((cp - 0xD800u) << 10) + (low - 0xDC00u);
                                } else {
                                    throw ParseError("invalid low surrogate in pair", pos_ - 4);
                                }
                            } else {
                                throw ParseError("missing low surrogate after high surrogate", pos_);
                            }
                        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                            throw ParseError("unexpected low surrogate", pos_ - 4);
                        }
                        encode_utf8(cp, out);
                        break;
                    }
                    default:
                        throw ParseError(
                            std::string("invalid escape character '\\") + esc + "'",
                            pos_ - 1);
                }
            } else if (c < 0x20) {
                // RFC 8259 §7: unescaped control characters are not allowed.
                throw ParseError("unescaped control character in string", pos_ - 1);
            } else {
                out += char(c);
            }
        }
        return out;
    }

    // ── literals ────────────────────────────────────────────────────────────

    Value parse_true() {
        if (src_.size() - pos_ < 4 || src_.substr(pos_, 4) != "true")
            throw ParseError("invalid literal (expected 'true')", pos_);
        pos_ += 4;
        return Value(true);
    }

    Value parse_false() {
        if (src_.size() - pos_ < 5 || src_.substr(pos_, 5) != "false")
            throw ParseError("invalid literal (expected 'false')", pos_);
        pos_ += 5;
        return Value(false);
    }

    Value parse_null() {
        if (src_.size() - pos_ < 4 || src_.substr(pos_, 4) != "null")
            throw ParseError("invalid literal (expected 'null')", pos_);
        pos_ += 4;
        return Value(nullptr);
    }

    // ── number ──────────────────────────────────────────────────────────────

    Value parse_number() {
        std::size_t start = pos_;

        // Optional minus sign.
        if (peek() == '-') ++pos_;

        if (pos_ >= src_.size())
            throw ParseError("invalid number: digit expected", pos_);

        // Integer part.
        if (src_[pos_] == '0') {
            ++pos_;
        } else if (src_[pos_] >= '1' && src_[pos_] <= '9') {
            while (pos_ < src_.size() && src_[pos_] >= '0' && src_[pos_] <= '9')
                ++pos_;
        } else {
            throw ParseError("invalid number: digit expected", pos_);
        }

        // Optional fractional part.
        if (pos_ < src_.size() && src_[pos_] == '.') {
            ++pos_;
            if (pos_ >= src_.size() || src_[pos_] < '0' || src_[pos_] > '9')
                throw ParseError("invalid number: digit expected after '.'", pos_);
            while (pos_ < src_.size() && src_[pos_] >= '0' && src_[pos_] <= '9')
                ++pos_;
        }

        // Optional exponent.
        if (pos_ < src_.size() && (src_[pos_] == 'e' || src_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < src_.size() && (src_[pos_] == '+' || src_[pos_] == '-'))
                ++pos_;
            if (pos_ >= src_.size() || src_[pos_] < '0' || src_[pos_] > '9')
                throw ParseError("invalid number: digit expected in exponent", pos_);
            while (pos_ < src_.size() && src_[pos_] >= '0' && src_[pos_] <= '9')
                ++pos_;
        }

        // Convert the captured span to double via null-terminated strtod.
        std::string buf(src_.data() + start, pos_ - start);
        char* end_ptr = nullptr;
        double val = std::strtod(buf.c_str(), &end_ptr);
        if (end_ptr != buf.c_str() + buf.size())
            throw ParseError("invalid number '" + buf + "'", start);

        return Value(val);
    }

    // ── composite ───────────────────────────────────────────────────────────

    Value parse_array() {
        ++pos_; // consume '['
        skip_ws();

        Array arr;
        if (peek() == ']') { ++pos_; return Value(std::move(arr)); }

        for (;;) {
            arr.push_back(parse_value());
            skip_ws();
            char c = peek();
            if (c == ']') { ++pos_; break; }
            if (c != ',')
                throw ParseError("expected ',' or ']' in array", pos_);
            ++pos_;
        }
        return Value(std::move(arr));
    }

    Value parse_object() {
        ++pos_; // consume '{'
        skip_ws();

        Object obj;
        if (peek() == '}') { ++pos_; return Value(std::move(obj)); }

        for (;;) {
            skip_ws();
            if (peek() != '"')
                throw ParseError("expected '\"' for object key", pos_);
            ++pos_;
            std::string key = parse_string_content();

            skip_ws();
            if (peek() != ':')
                throw ParseError("expected ':' in object", pos_);
            ++pos_;

            Value val = parse_value();
            obj.push_back({ std::move(key), std::move(val) });

            skip_ws();
            char c = peek();
            if (c == '}') { ++pos_; break; }
            if (c != ',')
                throw ParseError("expected ',' or '}' in object", pos_);
            ++pos_;
        }
        return Value(std::move(obj));
    }

    // ── dispatch ────────────────────────────────────────────────────────────

    Value parse_value() {
        skip_ws();
        if (pos_ >= src_.size())
            throw ParseError("unexpected end of input", pos_);

        switch (peek()) {
            case '"':  ++pos_; return Value(parse_string_content());
            case '[':  return parse_array();
            case '{':  return parse_object();
            case 't':  return parse_true();
            case 'f':  return parse_false();
            case 'n':  return parse_null();
            case '-':
            case '0': case '1': case '2': case '3': case '4':
            case '5': case '6': case '7': case '8': case '9':
                return parse_number();
            default:
                throw ParseError(
                    std::string("unexpected character '") + peek() + "'", pos_);
        }
    }

public:
    explicit Parser(std::string_view s) : src_(s) {}

    Value parse() {
        Value root = parse_value();
        skip_ws();
        if (pos_ != src_.size())
            throw ParseError("unexpected trailing content", pos_);
        return root;
    }
};

// ── serializer ──────────────────────────────────────────────────────────────

class Serializer {
    std::string out_;
    bool        pretty_;
    int         indent_width_;
    int         depth_ = 0;

    void write_indent() {
        if (pretty_) {
            out_ += '\n';
            out_.append(std::size_t(depth_ * indent_width_), ' ');
        }
    }

    // Write a JSON string with all mandatory escapes applied.
    void write_string(std::string const& s) {
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
                        // Control character: produce \u00XX.
                        char buf[8];
                        std::snprintf(buf, sizeof(buf), "\\u%04x", unsigned(c));
                        out_ += buf;
                    } else {
                        out_ += char(c);
                    }
            }
        }
        out_ += '"';
    }

    // Serialize a double with enough precision to round-trip exactly.
    // %.17g produces 17 significant digits in shortest-necessary form.
    void write_number(double d) {
        if (std::isinf(d) || std::isnan(d))
            throw std::runtime_error("JSON does not support Inf or NaN");
        char buf[64];
        int n = std::snprintf(buf, sizeof(buf), "%.17g", d);
        out_.append(buf, std::size_t(n));
    }

    void write_value(Value const& val) {
        std::visit([this](auto const& x) {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, std::nullptr_t>) {
                out_ += "null";
            } else if constexpr (std::is_same_v<T, bool>) {
                out_ += (x ? "true" : "false");
            } else if constexpr (std::is_same_v<T, double>) {
                write_number(x);
            } else if constexpr (std::is_same_v<T, std::string>) {
                write_string(x);
            } else if constexpr (std::is_same_v<T, Array>) {
                write_array(x);
            } else if constexpr (std::is_same_v<T, Object>) {
                write_object(x);
            }
        }, val.v);
    }

    void write_array(Array const& arr) {
        out_ += '[';
        ++depth_;
        for (std::size_t i = 0; i < arr.size(); ++i) {
            if (i > 0) out_ += ',';
            write_indent();
            write_value(arr[i]);
        }
        --depth_;
        if (!arr.empty() && pretty_) write_indent();
        out_ += ']';
    }

    void write_object(Object const& obj) {
        out_ += '{';
        ++depth_;
        for (std::size_t i = 0; i < obj.size(); ++i) {
            if (i > 0) out_ += ',';
            write_indent();
            write_string(obj[i].first);
            out_ += (pretty_ ? ": " : ":");
            write_value(obj[i].second);
        }
        --depth_;
        if (!obj.empty() && pretty_) write_indent();
        out_ += '}';
    }

public:
    explicit Serializer(bool pretty = false, int indent_width = 2)
        : pretty_(pretty), indent_width_(indent_width) {}

    std::string run(Value const& root) {
        write_value(root);
        return std::move(out_);
    }
};

} // namespace detail

// ─── public API ─────────────────────────────────────────────────────────────

/// Parse a JSON string.  Throws json::ParseError on any syntax error.
inline Value parse(std::string_view src) {
    return detail::Parser(src).parse();
}

/// Serialize a Value to a JSON string.
/// @param pretty      true for indented output, false (default) for compact.
/// @param indent_size number of spaces per indent level when pretty == true.
inline std::string serialize(Value const& v,
                             bool pretty      = false,
                             int  indent_size = 2)
{
    return detail::Serializer(pretty, indent_size).run(v);
}

} // namespace json
