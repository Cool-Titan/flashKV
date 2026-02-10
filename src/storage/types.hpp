#pragma once

#include <chrono>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <unordered_set>
#include <variant>

namespace flashkv {

// ===== Value Types =====

struct StringValue {
    std::string content;
    explicit StringValue(const std::string& s) : content(s) {}
};

struct HashValue {
    std::map<std::string, std::string> fields;
    HashValue() = default;
};

struct ListValue {
    std::deque<std::string> elements;
    ListValue() = default;
};

struct SetValue {
    std::unordered_set<std::string> members;
    SetValue() = default;
};

// Union type for all supported data types.
using Value = std::variant<
    std::monostate,  // Empty/deleted
    StringValue,
    HashValue,
    ListValue,
    SetValue
>;

// Human readable type name, used in error messages and the TYPE command.
inline std::string value_type_name(const Value& v) {
    if (std::holds_alternative<StringValue>(v)) return "string";
    if (std::holds_alternative<HashValue>(v))   return "hash";
    if (std::holds_alternative<ListValue>(v))   return "list";
    if (std::holds_alternative<SetValue>(v))    return "set";
    return "none";
}

// Standard Redis wording so clients can pattern-match on it.
inline const char* const WRONGTYPE_ERROR =
    "WRONGTYPE Operation against a key holding the wrong kind of value";

// ===== Metadata =====

struct KeyMetadata {
    Value data;
    std::optional<std::chrono::system_clock::time_point> expiry_time;
    std::chrono::system_clock::time_point last_access_time;

    KeyMetadata()
        : data(std::monostate{}),
          last_access_time(std::chrono::system_clock::now()) {}

    explicit KeyMetadata(const Value& v, std::optional<int> ttl_seconds = std::nullopt)
        : data(v), last_access_time(std::chrono::system_clock::now()) {
        if (ttl_seconds) {
            expiry_time = std::chrono::system_clock::now() +
                          std::chrono::seconds(*ttl_seconds);
        }
    }

    bool is_expired() const {
        if (!expiry_time) return false;
        return std::chrono::system_clock::now() >= *expiry_time;
    }
};

// ===== Result Type (Error Handling) =====
// Result<T> is either the value T or an error message.
//
// The error arm is a one-field wrapper rather than a bare std::string on
// purpose: std::variant<std::string, std::string> is a duplicated alternative,
// which makes std::holds_alternative<std::string> / std::get<std::string>
// ill-formed. Since methods such as set() return Result<std::string>, the
// wrapper is what makes the whole API uniform. Callers only ever touch the
// is_error() / get_value() / get_error() helpers below.

struct ErrorMsg {
    std::string message;
    ErrorMsg() = default;
    explicit ErrorMsg(std::string m) : message(std::move(m)) {}
};

template <typename T>
using Result = std::variant<T, ErrorMsg>;

template <typename T>
bool is_error(const Result<T>& r) {
    return std::holds_alternative<ErrorMsg>(r);
}

template <typename T>
std::string get_error(const Result<T>& r) {
    return std::get<ErrorMsg>(r).message;
}

template <typename T>
T get_value(const Result<T>& r) {
    return std::get<0>(r);  // index, not type: T may itself be std::string
}

// Convenience factory so handlers can `return make_error<int>("...")`.
template <typename T>
Result<T> make_error(const std::string& msg) {
    return ErrorMsg(msg);
}

}  // namespace flashkv
