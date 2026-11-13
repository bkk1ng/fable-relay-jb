#pragma once

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace json {

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
struct Value;
using Null   = std::monostate;
using Array  = std::vector<Value>;
using Object = std::vector<std::pair<std::string, Value>>;

// ---------------------------------------------------------------------------
// Value
// ---------------------------------------------------------------------------
struct Value {
    using variant_type = std::variant<Null, bool, double, std::string, Array, Object>;
    variant_type v;

    // Constructors
    Value()                        : v(Null{})               {}
    explicit Value(std::nullptr_t) : v(Null{})               {}
    explicit Value(bool b)         : v(b)                    {}
    explicit Value(double d)       : v(d)                    {}
    explicit Value(int i)          : v(static_cast<double>(i)) {}
    explicit Value(std::string s)  : v(std::move(s))         {}
    explicit Value(const char* s)  : v(std::string(s))       {}
    explicit Value(Array a)        : v(std::move(a))         {}
    explicit Value(Object o)       : v(std::move(o))         {}

    // Type predicates
    bool is_null()   const noexcept { return std::holds_alternative<Null>(v);        }
    bool is_bool()   const noexcept { return std::holds_alternative<bool>(v);        }
    bool is_number() const noexcept { return std::holds_alternative<double>(v);      }
    bool is_string() const noexcept { return std::holds_alternative<std::string>(v); }
    bool is_array()  const noexcept { return std::holds_alternative<Array>(v);       }
    bool is_object() const noexcept { return std::holds_alternative<Object>(v);      }

    // Mutable accessors — throw std::bad_variant_access on wrong type
    bool&        as_bool()   { return std::get<bool>(v);        }
    double&      as_number() { return std::get<double>(v);      }
    std::string& as_string() { return std::get<std::string>(v); }
    Array&       as_array()  { return std::get<Array>(v);       }
    Object&      as_object() { return std::get<Object>(v);      }

    // Const accessors
    bool               as_bool()   const { return std::get<bool>(v);        }
    double             as_number() const { return std::get<double>(v);      }
    const std::string& as_string() const { return std::get<std::string>(v); }
    const Array&       as_array()  const { return std::get<Array>(v);       }
    const Object&      as_object() const { return std::get<Object>(v);      }
};

// ---------------------------------------------------------------------------
// ParseError
// ---------------------------------------------------------------------------
class ParseError : public std::runtime_error {
public:
    std::size_t offset;

    ParseError(const std::string& msg, std::size_t off)
        : std::runtime_error(msg + " (offset " + std::to_string(off) + ")")
        , offset(off)
    {}
};

// ---------------------------------------------------------------------------
// Parser — recursive descent
// ---------------------------------------------------------------------------
class Parser {
    std::string_view src_;
    std::size_t      pos_ = 0;

    // ---- character-level helpers ------------------------------------------

    void skip_ws() noexcept {
        while (pos_ < src_.size()) {
            char c = src_[pos_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') ++pos_;
            else break;
        }
    }

    char peek() const noexcept {
        return pos_ < src_.size() ? src_[pos_] : '\0';
    }

    char consume() {
        if (pos_ >= src_.size())
            throw ParseError("Unexpected end of input", pos_);
        return src_[pos_++];
    }

    void expect(char expected) {
        char got = consume();
        if (got != expected) {
            throw ParseError(
                std::string("Expected '") + expected + "' but got '" + got + "'",
                pos_ - 1);
        }
    }

    // ---- literal helpers --------------------------------------------------

    void consume_literal(const char* word, std::size_t len) {
        std::size_t start = pos_;
        if (pos_ + len > src_.size() || src_.substr(pos_, len) != std::string_view(word, len))
            throw ParseError(std::string("Expected '") + word + "'", start);
        pos_ += len;
    }

    // ---- decode one \uXXXX escape (pos_ must point past the 'u') ----------
    //      Returns the Unicode codepoint; advances pos_ by 4.
    uint32_t parse_hex4() {
        if (pos_ + 4 > src_.size())
            throw ParseError("Truncated \\u escape", pos_);
        uint32_t cp = 0;
        for (int i = 0; i < 4; ++i, ++pos_) {
            char h = src_[pos_];
            uint32_t nibble;
            if (h >= '0' && h <= '9')      nibble = static_cast<uint32_t>(h - '0');
            else if (h >= 'a' && h <= 'f') nibble = static_cast<uint32_t>(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') nibble = static_cast<uint32_t>(h - 'A' + 10);
            else throw ParseError("Invalid hex digit in \\u escape", pos_);
            cp = (cp << 4) | nibble;
        }
        return cp;
    }

    // Encode a Unicode codepoint as UTF-8 and append to out.
    static void encode_utf8(std::string& out, uint32_t cp) {
        if (cp <= 0x7Fu) {
            out += static_cast<char>(cp);
        } else if (cp <= 0x7FFu) {
            out += static_cast<char>(0xC0u | (cp >> 6));
            out += static_cast<char>(0x80u | (cp & 0x3Fu));
        } else if (cp <= 0xFFFFu) {
            out += static_cast<char>(0xE0u | (cp >> 12));
            out += static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
            out += static_cast<char>(0x80u | (cp & 0x3Fu));
        } else {
            out += static_cast<char>(0xF0u | (cp >> 18));
            out += static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu));
            out += static_cast<char>(0x80u | ((cp >> 6)  & 0x3Fu));
            out += static_cast<char>(0x80u | (cp         & 0x3Fu));
        }
    }

    // ---- recursive descent ------------------------------------------------

    Value parse_value() {
        skip_ws();
        switch (peek()) {
            case '"': return Value(parse_raw_string());
            case '{': return parse_object();
            case '[': return parse_array();
            case 't': consume_literal("true",  4); return Value(true);
            case 'f': consume_literal("false", 5); return Value(false);
            case 'n': consume_literal("null",  4); return Value();
            default:  return parse_number();
        }
    }

    // Parse a JSON string; returns the decoded std::string.
    std::string parse_raw_string() {
        expect('"');
        std::string result;
        result.reserve(16);

        while (true) {
            if (pos_ >= src_.size())
                throw ParseError("Unterminated string", pos_);

            unsigned char c = static_cast<unsigned char>(src_[pos_++]);

            if (c == '"') {
                break;
            }

            if (c < 0x20) {
                throw ParseError("Unescaped control character in string", pos_ - 1);
            }

            if (c != '\\') {
                result += static_cast<char>(c);
                continue;
            }

            // Escape sequence
            if (pos_ >= src_.size())
                throw ParseError("Unterminated escape sequence", pos_);

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
                    uint32_t cp = parse_hex4();
                    // Handle UTF-16 surrogate pairs
                    if (cp >= 0xD800u && cp <= 0xDBFFu) {
                        // High surrogate — expect \uXXXX low surrogate
                        if (pos_ + 6 > src_.size() ||
                            src_[pos_] != '\\' || src_[pos_ + 1] != 'u')
                            throw ParseError("Expected low surrogate after high surrogate", pos_);
                        pos_ += 2;
                        uint32_t low = parse_hex4();
                        if (low < 0xDC00u || low > 0xDFFFu)
                            throw ParseError("Invalid low surrogate value", pos_ - 4);
                        cp = 0x10000u + ((cp - 0xD800u) << 10) + (low - 0xDC00u);
                    } else if (cp >= 0xDC00u && cp <= 0xDFFFu) {
                        throw ParseError("Unexpected low surrogate", pos_ - 4);
                    }
                    encode_utf8(result, cp);
                    break;
                }
                default:
                    throw ParseError(
                        std::string("Unknown escape character: \\") + esc, pos_ - 1);
            }
        }
        return result;
    }

    Value parse_array() {
        expect('[');
        Array arr;
        skip_ws();
        if (peek() == ']') { consume(); return Value(std::move(arr)); }

        while (true) {
            arr.push_back(parse_value());
            skip_ws();
            char c = peek();
            if (c == ']') { consume(); break; }
            if (c != ',')
                throw ParseError("Expected ',' or ']' in array", pos_);
            consume();
        }
        return Value(std::move(arr));
    }

    Value parse_object() {
        expect('{');
        Object obj;
        skip_ws();
        if (peek() == '}') { consume(); return Value(std::move(obj)); }

        while (true) {
            skip_ws();
            if (peek() != '"')
                throw ParseError("Expected string key in object", pos_);
            std::string key = parse_raw_string();
            skip_ws();
            expect(':');
            Value val = parse_value();
            obj.emplace_back(std::move(key), std::move(val));
            skip_ws();
            char c = peek();
            if (c == '}') { consume(); break; }
            if (c != ',')
                throw ParseError("Expected ',' or '}' in object", pos_);
            consume();
        }
        return Value(std::move(obj));
    }

    Value parse_number() {
        std::size_t start = pos_;

        // Optional leading minus
        if (peek() == '-') ++pos_;

        // Integer part
        if (pos_ >= src_.size() ||
            !std::isdigit(static_cast<unsigned char>(src_[pos_])))
            throw ParseError("Expected digit", pos_);

        // Leading zero may only be a bare zero (no 007)
        if (src_[pos_] == '0') {
            ++pos_;
        } else {
            while (pos_ < src_.size() &&
                   std::isdigit(static_cast<unsigned char>(src_[pos_])))
                ++pos_;
        }

        // Optional fractional part
        if (pos_ < src_.size() && src_[pos_] == '.') {
            ++pos_;
            if (pos_ >= src_.size() ||
                !std::isdigit(static_cast<unsigned char>(src_[pos_])))
                throw ParseError("Expected digit after decimal point", pos_);
            while (pos_ < src_.size() &&
                   std::isdigit(static_cast<unsigned char>(src_[pos_])))
                ++pos_;
        }

        // Optional exponent
        if (pos_ < src_.size() && (src_[pos_] == 'e' || src_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < src_.size() && (src_[pos_] == '+' || src_[pos_] == '-'))
                ++pos_;
            if (pos_ >= src_.size() ||
                !std::isdigit(static_cast<unsigned char>(src_[pos_])))
                throw ParseError("Expected digit in exponent", pos_);
            while (pos_ < src_.size() &&
                   std::isdigit(static_cast<unsigned char>(src_[pos_])))
                ++pos_;
        }

        std::string num_str(src_.substr(start, pos_ - start));
        try {
            return Value(std::stod(num_str));
        } catch (...) {
            throw ParseError("Invalid number: " + num_str, start);
        }
    }

public:
    explicit Parser(std::string_view src) : src_(src) {}

    Value parse() {
        Value val = parse_value();
        skip_ws();
        if (pos_ != src_.size())
            throw ParseError("Unexpected trailing content", pos_);
        return val;
    }
};

// ---------------------------------------------------------------------------
// Serializer
// ---------------------------------------------------------------------------
class Serializer {
    bool pretty_;
    int  indent_size_;

    void newline_indent(std::string& out, int depth) const {
        out += '\n';
        out.append(static_cast<std::size_t>(depth * indent_size_), ' ');
    }

    static void serialize_string(std::string& out, const std::string& s) {
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
                        char buf[7];
                        std::snprintf(buf, sizeof(buf), "\\u%04x",
                                      static_cast<unsigned>(c));
                        out += buf;
                    } else {
                        out += static_cast<char>(c);
                    }
            }
        }
        out += '"';
    }

    void serialize_value(std::string& out, const Value& val, int depth) const {
        std::visit([&](const auto& node) {
            using T = std::decay_t<decltype(node)>;

            if constexpr (std::is_same_v<T, Null>) {
                out += "null";

            } else if constexpr (std::is_same_v<T, bool>) {
                out += node ? "true" : "false";

            } else if constexpr (std::is_same_v<T, double>) {
                // Use shortest round-trip representation via setprecision(17).
                std::ostringstream oss;
                oss << std::setprecision(17) << node;
                out += oss.str();

            } else if constexpr (std::is_same_v<T, std::string>) {
                serialize_string(out, node);

            } else if constexpr (std::is_same_v<T, Array>) {
                out += '[';
                for (std::size_t i = 0; i < node.size(); ++i) {
                    if (i > 0) out += ',';
                    if (pretty_) newline_indent(out, depth + 1);
                    serialize_value(out, node[i], depth + 1);
                }
                if (!node.empty() && pretty_) newline_indent(out, depth);
                out += ']';

            } else if constexpr (std::is_same_v<T, Object>) {
                out += '{';
                for (std::size_t i = 0; i < node.size(); ++i) {
                    if (i > 0) out += ',';
                    if (pretty_) newline_indent(out, depth + 1);
                    serialize_string(out, node[i].first);
                    out += pretty_ ? ": " : ":";
                    serialize_value(out, node[i].second, depth + 1);
                }
                if (!node.empty() && pretty_) newline_indent(out, depth);
                out += '}';
            }
        }, val.v);
    }

public:
    explicit Serializer(bool pretty = false, int indent_size = 2)
        : pretty_(pretty), indent_size_(indent_size) {}

    std::string serialize(const Value& val) const {
        std::string out;
        out.reserve(256);
        serialize_value(out, val, 0);
        return out;
    }
};

// ---------------------------------------------------------------------------
// Convenience free functions
// ---------------------------------------------------------------------------

/// Parse JSON text; throws ParseError on failure.
inline Value parse(std::string_view src) {
    return Parser(src).parse();
}

/// Serialize a Value to JSON text.
/// @param pretty  Enable indented output.
/// @param indent  Spaces per indentation level (used only when pretty == true).
inline std::string serialize(const Value& val,
                             bool         pretty = false,
                             int          indent = 2) {
    return Serializer(pretty, indent).serialize(val);
}

} // namespace json
