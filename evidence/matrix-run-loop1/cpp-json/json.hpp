#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <variant>
#include <stdexcept>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <cassert>

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------

struct Value;
using Array  = std::vector<Value>;
using Object = std::unordered_map<std::string, Value>;

// ---------------------------------------------------------------------------
// Value variant  (null | bool | double | string | array | object)
// ---------------------------------------------------------------------------

struct Null {};
inline bool operator==(Null, Null) noexcept { return true; }

struct Value {
    using Variant = std::variant<Null, bool, double, std::string, Array, Object>;
    Variant data;

    // Constructors
    Value()                           : data(Null{})                   {}
    Value(Null)                       : data(Null{})                   {}
    Value(bool v)                     : data(v)                        {}
    Value(double v)                   : data(v)                        {}
    Value(int v)                      : data(static_cast<double>(v))   {}
    Value(long long v)                : data(static_cast<double>(v))   {}
    Value(const char* v)              : data(std::string(v))           {}
    Value(std::string v)              : data(std::move(v))             {}
    Value(Array v)                    : data(std::move(v))             {}
    Value(Object v)                   : data(std::move(v))             {}

    // Type tests
    bool is_null()   const { return std::holds_alternative<Null>(data);        }
    bool is_bool()   const { return std::holds_alternative<bool>(data);        }
    bool is_number() const { return std::holds_alternative<double>(data);      }
    bool is_string() const { return std::holds_alternative<std::string>(data); }
    bool is_array()  const { return std::holds_alternative<Array>(data);       }
    bool is_object() const { return std::holds_alternative<Object>(data);      }

    // Accessors (throw std::bad_variant_access on wrong type)
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
// ParseError
// ---------------------------------------------------------------------------

struct ParseError : std::runtime_error {
    std::size_t offset;
    ParseError(const std::string& msg, std::size_t off)
        : std::runtime_error(msg + " (offset " + std::to_string(off) + ")")
        , offset(off)
    {}
};

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------

class Parser {
public:
    explicit Parser(std::string_view src)
        : src_(src), pos_(0)
    {}

    Value parse() {
        skip_ws();
        Value v = parse_value();
        skip_ws();
        if (pos_ != src_.size()) {
            throw ParseError("unexpected trailing content", pos_);
        }
        return v;
    }

private:
    std::string_view src_;
    std::size_t      pos_;

    // -----------------------------------------------------------------------
    // Whitespace
    // -----------------------------------------------------------------------

    void skip_ws() noexcept {
        while (pos_ < src_.size()) {
            char c = src_[pos_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    // -----------------------------------------------------------------------
    // Dispatch
    // -----------------------------------------------------------------------

    Value parse_value() {
        if (pos_ >= src_.size()) {
            throw ParseError("unexpected end of input", pos_);
        }
        char c = src_[pos_];
        switch (c) {
            case '"': return parse_string_value();
            case '{': return parse_object();
            case '[': return parse_array();
            case 't': return parse_literal("true",  Value(true));
            case 'f': return parse_literal("false", Value(false));
            case 'n': return parse_literal("null",  Value(Null{}));
            default:
                if (c == '-' || (c >= '0' && c <= '9')) {
                    return parse_number();
                }
                throw ParseError(std::string("unexpected character '") + c + "'", pos_);
        }
    }

    // -----------------------------------------------------------------------
    // Literals
    // -----------------------------------------------------------------------

    Value parse_literal(std::string_view token, Value result) {
        if (src_.substr(pos_, token.size()) != token) {
            throw ParseError("invalid literal near '" + std::string(token) + "'", pos_);
        }
        pos_ += token.size();
        return result;
    }

    // -----------------------------------------------------------------------
    // Number
    // -----------------------------------------------------------------------

    Value parse_number() {
        std::size_t start = pos_;

        if (src_[pos_] == '-') ++pos_;

        if (pos_ >= src_.size()) {
            throw ParseError("incomplete number", start);
        }

        if (src_[pos_] == '0') {
            ++pos_;
        } else if (src_[pos_] >= '1' && src_[pos_] <= '9') {
            while (pos_ < src_.size() && src_[pos_] >= '0' && src_[pos_] <= '9') ++pos_;
        } else {
            throw ParseError("invalid number", start);
        }

        // Fractional part
        if (pos_ < src_.size() && src_[pos_] == '.') {
            ++pos_;
            if (pos_ >= src_.size() || src_[pos_] < '0' || src_[pos_] > '9') {
                throw ParseError("digit expected after '.'", pos_);
            }
            while (pos_ < src_.size() && src_[pos_] >= '0' && src_[pos_] <= '9') ++pos_;
        }

        // Exponent
        if (pos_ < src_.size() && (src_[pos_] == 'e' || src_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < src_.size() && (src_[pos_] == '+' || src_[pos_] == '-')) ++pos_;
            if (pos_ >= src_.size() || src_[pos_] < '0' || src_[pos_] > '9') {
                throw ParseError("digit expected in exponent", pos_);
            }
            while (pos_ < src_.size() && src_[pos_] >= '0' && src_[pos_] <= '9') ++pos_;
        }

        std::string raw(src_.substr(start, pos_ - start));
        try {
            double v = std::stod(raw);
            return Value(v);
        } catch (...) {
            throw ParseError("number out of range: " + raw, start);
        }
    }

    // -----------------------------------------------------------------------
    // String helpers
    // -----------------------------------------------------------------------

    // Decode 4 hex digits into a code point; returns UINT32_MAX on failure.
    static uint32_t parse_hex4(std::string_view s, std::size_t pos) {
        if (pos + 4 > s.size()) return UINT32_MAX;
        uint32_t cp = 0;
        for (int i = 0; i < 4; ++i) {
            char c = s[pos + i];
            uint32_t nibble;
            if (c >= '0' && c <= '9')      nibble = static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') nibble = static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') nibble = static_cast<uint32_t>(c - 'A' + 10);
            else return UINT32_MAX;
            cp = (cp << 4) | nibble;
        }
        return cp;
    }

    // Encode a Unicode code point to UTF-8 and append to out.
    static void encode_utf8(uint32_t cp, std::string& out) {
        if (cp <= 0x7F) {
            out += static_cast<char>(cp);
        } else if (cp <= 0x7FF) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp <= 0xFFFF) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp <= 0x10FFFF) {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
        // Invalid code points are silently dropped.
    }

    // Parse a JSON string starting at the current '"'.
    std::string parse_string_raw() {
        assert(src_[pos_] == '"');
        std::size_t str_start = pos_;
        ++pos_; // consume opening '"'

        std::string result;
        result.reserve(16);

        while (pos_ < src_.size()) {
            char c = src_[pos_];

            if (c == '"') {
                ++pos_; // consume closing '"'
                return result;
            }

            if (static_cast<unsigned char>(c) < 0x20) {
                throw ParseError("unescaped control character in string", pos_);
            }

            if (c != '\\') {
                result += c;
                ++pos_;
                continue;
            }

            // Escape sequence
            ++pos_;
            if (pos_ >= src_.size()) {
                throw ParseError("unterminated escape sequence", str_start);
            }
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
                    uint32_t cp = parse_hex4(src_, pos_);
                    if (cp == UINT32_MAX) {
                        throw ParseError("invalid \\uXXXX escape", pos_);
                    }
                    pos_ += 4;

                    // Handle UTF-16 surrogate pair
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        // High surrogate — expect \uXXXX low surrogate
                        if (pos_ + 1 < src_.size() &&
                            src_[pos_] == '\\' && src_[pos_ + 1] == 'u') {
                            pos_ += 2;
                            uint32_t low = parse_hex4(src_, pos_);
                            if (low == UINT32_MAX) {
                                throw ParseError("invalid low surrogate", pos_);
                            }
                            if (low >= 0xDC00 && low <= 0xDFFF) {
                                pos_ += 4;
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                            } else {
                                throw ParseError("expected low surrogate after high surrogate", pos_);
                            }
                        } else {
                            throw ParseError("lone high surrogate", pos_);
                        }
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        throw ParseError("lone low surrogate", pos_);
                    }

                    encode_utf8(cp, result);
                    break;
                }
                default:
                    throw ParseError(std::string("invalid escape '\\") + esc + "'", pos_ - 1);
            }
        }
        throw ParseError("unterminated string", str_start);
    }

    Value parse_string_value() {
        return Value(parse_string_raw());
    }

    // -----------------------------------------------------------------------
    // Array
    // -----------------------------------------------------------------------

    Value parse_array() {
        assert(src_[pos_] == '[');
        std::size_t arr_start = pos_;
        ++pos_; // consume '['
        skip_ws();

        Array arr;

        if (pos_ < src_.size() && src_[pos_] == ']') {
            ++pos_;
            return Value(std::move(arr));
        }

        while (true) {
            skip_ws();
            if (pos_ >= src_.size()) {
                throw ParseError("unterminated array", arr_start);
            }
            arr.push_back(parse_value());
            skip_ws();
            if (pos_ >= src_.size()) {
                throw ParseError("unterminated array", arr_start);
            }
            char c = src_[pos_];
            if (c == ']') { ++pos_; break; }
            if (c == ',') { ++pos_; continue; }
            throw ParseError("expected ',' or ']' in array", pos_);
        }

        return Value(std::move(arr));
    }

    // -----------------------------------------------------------------------
    // Object
    // -----------------------------------------------------------------------

    Value parse_object() {
        assert(src_[pos_] == '{');
        std::size_t obj_start = pos_;
        ++pos_; // consume '{'
        skip_ws();

        Object obj;

        if (pos_ < src_.size() && src_[pos_] == '}') {
            ++pos_;
            return Value(std::move(obj));
        }

        while (true) {
            skip_ws();
            if (pos_ >= src_.size()) {
                throw ParseError("unterminated object", obj_start);
            }
            if (src_[pos_] != '"') {
                throw ParseError("expected string key in object", pos_);
            }
            std::string key = parse_string_raw();
            skip_ws();
            if (pos_ >= src_.size() || src_[pos_] != ':') {
                throw ParseError("expected ':' after key", pos_);
            }
            ++pos_; // consume ':'
            skip_ws();
            Value val = parse_value();
            obj.emplace(std::move(key), std::move(val));

            skip_ws();
            if (pos_ >= src_.size()) {
                throw ParseError("unterminated object", obj_start);
            }
            char c = src_[pos_];
            if (c == '}') { ++pos_; break; }
            if (c == ',') { ++pos_; continue; }
            throw ParseError("expected ',' or '}' in object", pos_);
        }

        return Value(std::move(obj));
    }
};

// ---------------------------------------------------------------------------
// Public parse entry point
// ---------------------------------------------------------------------------

inline Value parse(std::string_view src) {
    return Parser(src).parse();
}

// ---------------------------------------------------------------------------
// Serializer
// ---------------------------------------------------------------------------

class Serializer {
public:
    struct Options {
        bool   pretty        = false;
        int    indent_size   = 4;    // spaces per level (pretty mode)
        char   indent_char   = ' ';
    };

    explicit Serializer(Options opts = {}) : opts_(opts) {}

    std::string serialize(const Value& v) {
        depth_ = 0;
        out_.clear();
        write_value(v);
        return out_;
    }

private:
    Options     opts_;
    std::string out_;
    int         depth_ = 0;

    void write_value(const Value& v) {
        if (v.is_null())   { out_ += "null"; return; }
        if (v.is_bool())   { out_ += v.as_bool() ? "true" : "false"; return; }
        if (v.is_number()) { write_number(v.as_number()); return; }
        if (v.is_string()) { write_string(v.as_string()); return; }
        if (v.is_array())  { write_array(v.as_array()); return; }
        if (v.is_object()) { write_object(v.as_object()); return; }
    }

    // -----------------------------------------------------------------------
    // Number serialization: integers are emitted without a decimal point.
    // -----------------------------------------------------------------------
    void write_number(double d) {
        if (std::isnan(d) || std::isinf(d)) {
            // JSON does not allow NaN/Inf; emit null per common convention.
            out_ += "null";
            return;
        }
        // If the value is an exact integer representable in int64, emit it cleanly.
        if (d == std::trunc(d) && d >= -1e15 && d <= 1e15) {
            long long iv = static_cast<long long>(d);
            out_ += std::to_string(iv);
            return;
        }
        // General case: use the shortest round-trip representation.
        std::ostringstream oss;
        oss << std::setprecision(17) << d;
        out_ += oss.str();
    }

    // -----------------------------------------------------------------------
    // String serialization with proper escaping
    // -----------------------------------------------------------------------
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
                        // Control character: emit \uXXXX
                        char buf[7];
                        std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                        out_ += buf;
                    } else {
                        out_ += static_cast<char>(c);
                    }
            }
        }
        out_ += '"';
    }

    // -----------------------------------------------------------------------
    // Indentation helpers
    // -----------------------------------------------------------------------
    void newline_indent() {
        if (!opts_.pretty) return;
        out_ += '\n';
        int spaces = depth_ * opts_.indent_size;
        out_.append(spaces, opts_.indent_char);
    }

    // -----------------------------------------------------------------------
    // Array serialization
    // -----------------------------------------------------------------------
    void write_array(const Array& arr) {
        if (arr.empty()) { out_ += "[]"; return; }
        out_ += '[';
        ++depth_;
        for (std::size_t i = 0; i < arr.size(); ++i) {
            newline_indent();
            write_value(arr[i]);
            if (i + 1 < arr.size()) out_ += ',';
        }
        --depth_;
        newline_indent();
        out_ += ']';
    }

    // -----------------------------------------------------------------------
    // Object serialization (sorted keys for deterministic output)
    // -----------------------------------------------------------------------
    void write_object(const Object& obj) {
        if (obj.empty()) { out_ += "{}"; return; }

        // Sort keys for reproducible output.
        std::vector<const std::string*> keys;
        keys.reserve(obj.size());
        for (const auto& [k, _] : obj) keys.push_back(&k);
        std::sort(keys.begin(), keys.end(),
                  [](const std::string* a, const std::string* b) { return *a < *b; });

        out_ += '{';
        ++depth_;
        for (std::size_t i = 0; i < keys.size(); ++i) {
            newline_indent();
            write_string(*keys[i]);
            out_ += opts_.pretty ? ": " : ":";
            write_value(obj.at(*keys[i]));
            if (i + 1 < keys.size()) out_ += ',';
        }
        --depth_;
        newline_indent();
        out_ += '}';
    }
};

// ---------------------------------------------------------------------------
// Public serialize entry points
// ---------------------------------------------------------------------------

inline std::string serialize(const Value& v, bool pretty = false) {
    Serializer::Options opts;
    opts.pretty = pretty;
    return Serializer(opts).serialize(v);
}

inline std::string serialize_pretty(const Value& v, int indent = 4) {
    Serializer::Options opts;
    opts.pretty      = true;
    opts.indent_size = indent;
    return Serializer(opts).serialize(v);
}
