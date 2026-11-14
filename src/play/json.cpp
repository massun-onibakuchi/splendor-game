#include "splendor/play/json.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <system_error>

namespace splendor::play {
namespace {

[[noreturn]] void mismatch(const std::string_view what, const char* expected) {
    throw std::invalid_argument(std::string(what) + " must be " + expected);
}

class Parser {
public:
    explicit Parser(const std::string_view text) : text_(text) {}

    Json document() {
        Json value = parse_value(0);
        skip();
        if (at_ != text_.size()) fail("trailing characters");
        return value;
    }

private:
    static constexpr int kMaxDepth = 64;

    [[noreturn]] void fail(const char* reason) const {
        throw std::invalid_argument(
            std::string("invalid JSON at byte ") + std::to_string(at_) + ": " + reason);
    }

    void skip() {
        while (at_ < text_.size()
               && (text_[at_] == ' ' || text_[at_] == '\t' || text_[at_] == '\n' || text_[at_] == '\r')) {
            ++at_;
        }
    }

    char peek() {
        skip();
        if (at_ >= text_.size()) fail("unexpected end");
        return text_[at_];
    }

    void expect(const char c) {
        if (peek() != c) fail("unexpected character");
        ++at_;
    }

    bool literal(const std::string_view word) {
        if (text_.substr(at_, word.size()) != word) return false;
        at_ += word.size();
        return true;
    }

    Json parse_value(const int depth) {
        if (depth > kMaxDepth) fail("nesting too deep");
        const char c = peek();
        if (c == '{') return parse_object(depth);
        if (c == '[') return parse_array(depth);
        if (c == '"') return Json(parse_string());
        if (literal("true")) return Json(true);
        if (literal("false")) return Json(false);
        if (literal("null")) return Json();
        return parse_number();
    }

    Json parse_object(const int depth) {
        expect('{');
        Json::Object object;
        if (peek() == '}') {
            ++at_;
            return Json(std::move(object));
        }
        for (;;) {
            if (peek() != '"') fail("expected a key");
            std::string key = parse_string();
            for (const auto& [existing, value] : object) {
                if (existing == key) fail("duplicate key");
            }
            expect(':');
            object.emplace_back(std::move(key), parse_value(depth + 1));
            const char next = peek();
            ++at_;
            if (next == '}') return Json(std::move(object));
            if (next != ',') fail("expected ',' or '}'");
        }
    }

    Json parse_array(const int depth) {
        expect('[');
        Json::Array array;
        if (peek() == ']') {
            ++at_;
            return Json(std::move(array));
        }
        for (;;) {
            array.push_back(parse_value(depth + 1));
            const char next = peek();
            ++at_;
            if (next == ']') return Json(std::move(array));
            if (next != ',') fail("expected ',' or ']'");
        }
    }

    unsigned hex4() {
        if (at_ + 4 > text_.size()) fail("truncated escape");
        unsigned value = 0;
        for (int index = 0; index < 4; ++index) {
            const char h = text_[at_++];
            value <<= 4;
            if (h >= '0' && h <= '9') value |= static_cast<unsigned>(h - '0');
            else if (h >= 'a' && h <= 'f') value |= static_cast<unsigned>(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') value |= static_cast<unsigned>(h - 'A' + 10);
            else fail("invalid escape");
        }
        return value;
    }

    static void append_utf8(std::string& out, const unsigned code) {
        if (code < 0x80) {
            out.push_back(static_cast<char>(code));
        } else if (code < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else if (code < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (code >> 18)));
            out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
    }

    std::string parse_string() {
        expect('"');
        std::string out;
        for (;;) {
            if (at_ >= text_.size()) fail("unterminated string");
            const char c = text_[at_++];
            if (c == '"') return out;
            if (static_cast<unsigned char>(c) < 0x20) fail("control character in string");
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (at_ >= text_.size()) fail("truncated escape");
            switch (const char e = text_[at_++]) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    unsigned code = hex4();
                    if (code >= 0xD800 && code <= 0xDBFF) {
                        if (!literal("\\u")) fail("unpaired surrogate");
                        const unsigned low = hex4();
                        if (low < 0xDC00 || low > 0xDFFF) fail("unpaired surrogate");
                        code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                    } else if (code >= 0xDC00 && code <= 0xDFFF) {
                        fail("unpaired surrogate");
                    }
                    append_utf8(out, code);
                    break;
                }
                default:
                    static_cast<void>(e);
                    fail("invalid escape");
            }
        }
    }

    Json parse_number() {
        const std::size_t begin = at_;
        if (at_ < text_.size() && text_[at_] == '-') ++at_;
        const std::size_t digits = at_;
        while (at_ < text_.size() && text_[at_] >= '0' && text_[at_] <= '9') ++at_;
        if (at_ == digits) fail("expected a value");
        if (text_[digits] == '0' && at_ - digits > 1) fail("leading zero");
        bool integral = true;
        if (at_ < text_.size() && text_[at_] == '.') {
            integral = false;
            const std::size_t fraction = ++at_;
            while (at_ < text_.size() && text_[at_] >= '0' && text_[at_] <= '9') ++at_;
            if (at_ == fraction) fail("expected digits");
        }
        if (at_ < text_.size() && (text_[at_] == 'e' || text_[at_] == 'E')) {
            integral = false;
            ++at_;
            if (at_ < text_.size() && (text_[at_] == '+' || text_[at_] == '-')) ++at_;
            const std::size_t exponent = at_;
            while (at_ < text_.size() && text_[at_] >= '0' && text_[at_] <= '9') ++at_;
            if (at_ == exponent) fail("expected digits");
        }
        const char* first = text_.data() + begin;
        const char* last = text_.data() + at_;
        if (integral) {
            if (*first == '-') {
                std::int64_t value{};
                if (std::from_chars(first, last, value).ec == std::errc{}) return Json(value);
            } else {
                std::uint64_t value{};
                if (std::from_chars(first, last, value).ec == std::errc{}) return Json(value);
            }
        }
        // `strtod` rather than floating `from_chars`, which older macOS deployment targets lack.
        const std::string literal_text(first, last);
        char* end = nullptr;
        const double value = std::strtod(literal_text.c_str(), &end);
        if (end != literal_text.c_str() + literal_text.size() || !std::isfinite(value)) {
            fail("invalid number");
        }
        return Json(value);
    }

    std::string_view text_;
    std::size_t at_{};
};

}  // namespace

Json Json::parse(const std::string_view text) { return Parser(text).document(); }

bool Json::is_number() const noexcept {
    return std::holds_alternative<std::int64_t>(value_) || std::holds_alternative<std::uint64_t>(value_)
        || std::holds_alternative<double>(value_);
}

bool Json::as_bool(const std::string_view what) const {
    if (!is_bool()) mismatch(what, "boolean");
    return std::get<bool>(value_);
}

std::uint64_t Json::as_uint(const std::string_view what, const unsigned bits, const bool positive) const {
    std::uint64_t value{};
    if (const auto* u = std::get_if<std::uint64_t>(&value_)) {
        value = *u;
    } else if (const auto* i = std::get_if<std::int64_t>(&value_); i && *i >= 0) {
        value = static_cast<std::uint64_t>(*i);
    } else {
        mismatch(what, "a nonnegative integer");
    }
    if ((bits < 64 && value >> bits != 0) || (positive && value == 0)) {
        throw std::invalid_argument(
            std::string(what) + " is outside uint" + std::to_string(bits) + (positive ? " > 0" : ""));
    }
    return value;
}

double Json::as_double(const std::string_view what) const {
    if (const auto* d = std::get_if<double>(&value_)) return *d;
    if (const auto* i = std::get_if<std::int64_t>(&value_)) return static_cast<double>(*i);
    if (const auto* u = std::get_if<std::uint64_t>(&value_)) return static_cast<double>(*u);
    mismatch(what, "a number");
}

const std::string& Json::as_string(const std::string_view what) const {
    if (!is_string()) mismatch(what, "a string");
    return std::get<std::string>(value_);
}

const Json::Array& Json::as_array(const std::string_view what) const {
    if (!is_array()) mismatch(what, "an array");
    return std::get<Array>(value_);
}

Json::Array& Json::as_array(const std::string_view what) {
    if (!is_array()) mismatch(what, "an array");
    return std::get<Array>(value_);
}

const Json::Object& Json::as_object(const std::string_view what) const {
    if (!is_object()) mismatch(what, "an object");
    return std::get<Object>(value_);
}

Json::Object& Json::as_object(const std::string_view what) {
    if (!is_object()) mismatch(what, "an object");
    return std::get<Object>(value_);
}

const Json* Json::find(const std::string_view key) const {
    const auto* object = std::get_if<Object>(&value_);
    if (!object) return nullptr;
    for (const auto& [name, value] : *object) {
        if (name == key) return &value;
    }
    return nullptr;
}

const Json& Json::at(const std::string_view key) const {
    if (const Json* value = find(key)) return *value;
    throw std::invalid_argument("missing field " + std::string(key));
}

Json& Json::set(const std::string_view key, Json value) {
    Object& object = as_object("a JSON value with members");
    for (auto& [name, existing] : object) {
        if (name == key) {
            existing = std::move(value);
            return existing;
        }
    }
    return object.emplace_back(std::string(key), std::move(value)).second;
}

void Json::push(Json value) { as_array("a JSON value with elements").push_back(std::move(value)); }

const Json::Object& Json::closed(
    const std::initializer_list<std::string_view> keys, const std::string_view what) const {
    const Object& object = as_object(what);
    bool matches = object.size() == keys.size();
    for (const std::string_view key : keys) matches = matches && find(key) != nullptr;
    if (!matches) throw std::invalid_argument(std::string(what) + " fields do not match its contract");
    return object;
}

void append_quoted(std::string& out, const std::string_view value) {
    static constexpr char kHex[] = "0123456789abcdef";
    out.push_back('"');
    for (const char c : value) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    out += "\\u00";
                    out.push_back(kHex[(c >> 4) & 0xF]);
                    out.push_back(kHex[c & 0xF]);
                } else {
                    out.push_back(c);
                }
        }
    }
    out.push_back('"');
}

void Json::dump_to(std::string& out) const {
    std::visit(
        [&out](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, std::monostate>) {
                out += "null";
            } else if constexpr (std::is_same_v<T, bool>) {
                out += value ? "true" : "false";
            } else if constexpr (std::is_same_v<T, std::int64_t> || std::is_same_v<T, std::uint64_t>) {
                out += std::to_string(value);
            } else if constexpr (std::is_same_v<T, double>) {
                // The shortest of 15-17 significant digits that reads back exactly.
                char buffer[32];
                for (int precision = 15; precision <= 17; ++precision) {
                    std::snprintf(buffer, sizeof buffer, "%.*g", precision, value);
                    if (std::strtod(buffer, nullptr) == value) break;
                }
                const std::string_view text(buffer);
                out += text;
                // A double stays a double when read back.
                if (text.find_first_of(".eEn") == std::string_view::npos) out += ".0";
            } else if constexpr (std::is_same_v<T, std::string>) {
                append_quoted(out, value);
            } else if constexpr (std::is_same_v<T, Array>) {
                out.push_back('[');
                for (std::size_t index = 0; index < value.size(); ++index) {
                    if (index) out.push_back(',');
                    value[index].dump_to(out);
                }
                out.push_back(']');
            } else {
                out.push_back('{');
                for (std::size_t index = 0; index < value.size(); ++index) {
                    if (index) out.push_back(',');
                    append_quoted(out, value[index].first);
                    out.push_back(':');
                    value[index].second.dump_to(out);
                }
                out.push_back('}');
            }
        },
        value_);
}

std::string Json::dump() const {
    std::string out;
    dump_to(out);
    return out;
}

// Logged JSON equality: objects compare as unordered members, numbers by value.
bool Json::operator==(const Json& other) const {
    if (is_number() && other.is_number()) {
        const auto* left_double = std::get_if<double>(&value_);
        const auto* right_double = std::get_if<double>(&other.value_);
        if (left_double || right_double) return as_double("a number") == other.as_double("a number");
        // Two integers: equal when both are nonnegative and equal as unsigned, or both signed.
        const auto* left_signed = std::get_if<std::int64_t>(&value_);
        const auto* right_signed = std::get_if<std::int64_t>(&other.value_);
        if (left_signed && right_signed) return *left_signed == *right_signed;
        if ((left_signed && *left_signed < 0) || (right_signed && *right_signed < 0)) return false;
        return as_uint("an integer") == other.as_uint("an integer");
    }
    if (value_.index() != other.value_.index()) return false;
    if (const auto* object = std::get_if<Object>(&value_)) {
        const Object& theirs = std::get<Object>(other.value_);
        if (object->size() != theirs.size()) return false;
        return std::all_of(object->begin(), object->end(), [&other](const auto& member) {
            const Json* match = other.find(member.first);
            return match && *match == member.second;
        });
    }
    return value_ == other.value_;
}

}  // namespace splendor::play
