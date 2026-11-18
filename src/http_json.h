// Small JSON reader/writer helpers (RFC 8259), independent of the SDK.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace httpc {
namespace json {

constexpr int kMaxDepth = 64;

struct Value {
    enum class Type { Null, Bool, Number, String, Array, Object } type = Type::Null;
    bool boolean = false;
    double number = 0;
    bool integer = false; // number written without fraction/exponent and exactly representable
    std::string string;
    std::vector<Value> items;                          // Array
    std::vector<std::pair<std::string, Value>> fields; // Object, in document order
};

// Parses a complete document (surrounding whitespace allowed). False with *error set
// ("offset N: ...") on invalid input or nesting deeper than kMaxDepth.
bool Parse(const std::string& text, Value* out, std::string* error);

// Follows "a.b.1.c": object keys, or 1-based array indexes. Null when missing.
const Value* Find(const Value& root, const std::string& path);

// JSON string literal with quotes. Bytes that are not valid UTF-8 are replaced by U+FFFD.
std::string Quote(const std::string& s);
// Shortest text that reads back to the same double; false for NaN/infinity.
bool FormatNumber(double v, std::string* out);

// application/x-www-form-urlencoded component encoding (RFC 3986 unreserved kept).
std::string UrlEncode(const std::string& s);

} // namespace json
} // namespace httpc
