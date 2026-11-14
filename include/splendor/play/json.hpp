#pragma once

#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

// A JSON value for hosts: requests, logs and opaque agent reports. Objects keep their keys'
// insertion order, so a value dumps as it was built or parsed. Integers stay exact (signed
// or unsigned 64-bit); other numbers are doubles.
namespace splendor::play {

class Json {
public:
    using Array = std::vector<Json>;
    using Object = std::vector<std::pair<std::string, Json>>;

    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool value) : value_(value) {}
    Json(int value) : value_(static_cast<std::int64_t>(value)) {}
    Json(unsigned value) : value_(static_cast<std::uint64_t>(value)) {}
    Json(long value) : value_(static_cast<std::int64_t>(value)) {}
    Json(unsigned long value) : value_(static_cast<std::uint64_t>(value)) {}
    Json(long long value) : value_(static_cast<std::int64_t>(value)) {}
    Json(unsigned long long value) : value_(static_cast<std::uint64_t>(value)) {}
    Json(double value) : value_(value) {}
    Json(const char* value) : value_(std::string(value)) {}
    Json(std::string_view value) : value_(std::string(value)) {}
    Json(std::string value) : value_(std::move(value)) {}
    Json(Array value) : value_(std::move(value)) {}
    Json(Object value) : value_(std::move(value)) {}

    static Json array() { return Json(Array{}); }
    static Json object() { return Json(Object{}); }
    // Rejects anything but one complete JSON value (surrounding whitespace allowed).
    static Json parse(std::string_view text);

    bool is_null() const noexcept { return value_.index() == 0; }
    bool is_bool() const noexcept { return std::holds_alternative<bool>(value_); }
    bool is_number() const noexcept;
    bool is_string() const noexcept { return std::holds_alternative<std::string>(value_); }
    bool is_array() const noexcept { return std::holds_alternative<Array>(value_); }
    bool is_object() const noexcept { return std::holds_alternative<Object>(value_); }

    // Checked reads: each throws `std::invalid_argument` naming `what` on a mismatch.
    bool as_bool(std::string_view what) const;
    // An integer in [0, 2^bits), a positive one when `positive`.
    std::uint64_t as_uint(std::string_view what, unsigned bits = 64, bool positive = false) const;
    double as_double(std::string_view what) const;
    const std::string& as_string(std::string_view what) const;
    const Array& as_array(std::string_view what) const;
    Array& as_array(std::string_view what);
    const Object& as_object(std::string_view what) const;
    Object& as_object(std::string_view what);

    // An object member, or null when absent.
    const Json* find(std::string_view key) const;
    // An object member; throws when absent.
    const Json& at(std::string_view key) const;
    // Sets an object member, keeping its position when it exists.
    Json& set(std::string_view key, Json value);
    // Appends to an array.
    void push(Json value);
    // An object whose keys are exactly `keys`, in any order; throws naming `what`.
    const Object& closed(std::initializer_list<std::string_view> keys, std::string_view what) const;

    std::string dump() const;
    void dump_to(std::string& out) const;
    bool operator==(const Json& other) const;

private:
    std::variant<std::monostate, bool, std::int64_t, std::uint64_t, double, std::string, Array, Object>
        value_;
};

// `value` as a JSON string literal, quotes included.
void append_quoted(std::string& out, std::string_view value);

}  // namespace splendor::play
