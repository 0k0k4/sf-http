#include "http_json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace httpc {
namespace json {
namespace {

struct Parser {
    const std::string& s;
    size_t i = 0;
    std::string error;

    explicit Parser(const std::string& text) : s(text) {}

    bool Fail(const std::string& what) {
        if (error.empty()) error = "offset " + std::to_string(i) + ": " + what;
        return false;
    }
    void Space() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
    }
    bool Literal(const char* word) {
        const size_t n = strlen(word);
        if (s.compare(i, n, word) != 0) return Fail("invalid literal");
        i += n;
        return true;
    }
    static void PutUtf8(uint32_t cp, std::string* out) {
        if (cp < 0x80) {
            out->push_back((char)cp);
        } else if (cp < 0x800) {
            out->push_back((char)(0xC0 | (cp >> 6)));
            out->push_back((char)(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out->push_back((char)(0xE0 | (cp >> 12)));
            out->push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            out->push_back((char)(0x80 | (cp & 0x3F)));
        } else {
            out->push_back((char)(0xF0 | (cp >> 18)));
            out->push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
            out->push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            out->push_back((char)(0x80 | (cp & 0x3F)));
        }
    }
    bool Hex4(uint32_t* out) {
        if (i + 4 > s.size()) return Fail("truncated \\u escape");
        uint32_t v = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = s[i++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= (uint32_t)(c - '0');
            else if (c >= 'a' && c <= 'f') v |= (uint32_t)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= (uint32_t)(c - 'A' + 10);
            else return Fail("invalid \\u escape");
        }
        *out = v;
        return true;
    }
    bool String(std::string* out) {
        ++i; // opening quote
        for (;;) {
            if (i >= s.size()) return Fail("unterminated string");
            const unsigned char c = (unsigned char)s[i];
            if (c == '"') { ++i; return true; }
            if (c < 0x20) return Fail("control character in string");
            if (c != '\\') { out->push_back((char)c); ++i; continue; }
            if (++i >= s.size()) return Fail("unterminated escape");
            const char e = s[i++];
            switch (e) {
            case '"': out->push_back('"'); break;
            case '\\': out->push_back('\\'); break;
            case '/': out->push_back('/'); break;
            case 'b': out->push_back('\b'); break;
            case 'f': out->push_back('\f'); break;
            case 'n': out->push_back('\n'); break;
            case 'r': out->push_back('\r'); break;
            case 't': out->push_back('\t'); break;
            case 'u': {
                uint32_t cp;
                if (!Hex4(&cp)) return false;
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    uint32_t lo;
                    if (i + 1 < s.size() && s[i] == '\\' && s[i + 1] == 'u') {
                        i += 2;
                        if (!Hex4(&lo)) return false;
                        if (lo >= 0xDC00 && lo <= 0xDFFF) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        else { PutUtf8(0xFFFD, out); cp = lo >= 0xD800 && lo <= 0xDFFF ? 0xFFFD : lo; }
                    } else {
                        cp = 0xFFFD;
                    }
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    cp = 0xFFFD;
                }
                PutUtf8(cp, out);
                break;
            }
            default: return Fail("invalid escape");
            }
        }
    }
    bool Number(Value* out) {
        const size_t start = i;
        bool integer = true;
        if (s[i] == '-') ++i;
        if (i >= s.size()) return Fail("invalid number");
        if (s[i] == '0') ++i;
        else if (s[i] >= '1' && s[i] <= '9') while (i < s.size() && isdigit((unsigned char)s[i])) ++i;
        else return Fail("invalid number");
        if (i < s.size() && s[i] == '.') {
            integer = false;
            ++i;
            if (i >= s.size() || !isdigit((unsigned char)s[i])) return Fail("invalid number");
            while (i < s.size() && isdigit((unsigned char)s[i])) ++i;
        }
        if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
            integer = false;
            ++i;
            if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
            if (i >= s.size() || !isdigit((unsigned char)s[i])) return Fail("invalid number");
            while (i < s.size() && isdigit((unsigned char)s[i])) ++i;
        }
        const std::string text = s.substr(start, i - start);
        out->type = Value::Type::Number;
        out->number = strtod(text.c_str(), nullptr);
        out->integer = integer && std::fabs(out->number) <= 9007199254740992.0;
        return true;
    }
    bool Parse(Value* out, int depth) {
        if (depth >= kMaxDepth) return Fail("nesting deeper than " + std::to_string(kMaxDepth));
        Space();
        if (i >= s.size()) return Fail("unexpected end");
        const char c = s[i];
        if (c == '{') {
            out->type = Value::Type::Object;
            ++i;
            Space();
            if (i < s.size() && s[i] == '}') { ++i; return true; }
            for (;;) {
                Space();
                if (i >= s.size() || s[i] != '"') return Fail("expected a key");
                std::string key;
                if (!String(&key)) return false;
                Space();
                if (i >= s.size() || s[i] != ':') return Fail("expected ':'");
                ++i;
                Value v;
                if (!Parse(&v, depth + 1)) return false;
                bool replaced = false; // duplicate key: the last one wins
                for (auto& f : out->fields)
                    if (f.first == key) { f.second = std::move(v); replaced = true; break; }
                if (!replaced) out->fields.emplace_back(std::move(key), std::move(v));
                Space();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == '}') { ++i; return true; }
                return Fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            out->type = Value::Type::Array;
            ++i;
            Space();
            if (i < s.size() && s[i] == ']') { ++i; return true; }
            for (;;) {
                Value v;
                if (!Parse(&v, depth + 1)) return false;
                out->items.push_back(std::move(v));
                Space();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == ']') { ++i; return true; }
                return Fail("expected ',' or ']'");
            }
        }
        if (c == '"') {
            out->type = Value::Type::String;
            return String(&out->string);
        }
        if (c == 't') { out->type = Value::Type::Bool; out->boolean = true; return Literal("true"); }
        if (c == 'f') { out->type = Value::Type::Bool; out->boolean = false; return Literal("false"); }
        if (c == 'n') { out->type = Value::Type::Null; return Literal("null"); }
        if (c == '-' || (c >= '0' && c <= '9')) return Number(out);
        return Fail("unexpected character");
    }
};

// Length of the valid UTF-8 sequence at s[i], or 0.
size_t Utf8Length(const std::string& s, size_t i) {
    const unsigned char c = (unsigned char)s[i];
    size_t n;
    uint32_t cp;
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) { n = 2; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { n = 3; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { n = 4; cp = c & 0x07; }
    else return 0;
    if (i + n > s.size()) return 0;
    for (size_t k = 1; k < n; ++k) {
        const unsigned char d = (unsigned char)s[i + k];
        if ((d & 0xC0) != 0x80) return 0;
        cp = (cp << 6) | (d & 0x3F);
    }
    if ((n == 2 && cp < 0x80) || (n == 3 && cp < 0x800) || (n == 4 && (cp < 0x10000 || cp > 0x10FFFF)) ||
        (cp >= 0xD800 && cp <= 0xDFFF))
        return 0;
    return n;
}

} // namespace

bool Parse(const std::string& text, Value* out, std::string* error) {
    Parser p(text);
    Value v;
    if (!p.Parse(&v, 0)) {
        if (error) *error = p.error;
        return false;
    }
    p.Space();
    if (p.i != text.size()) {
        p.Fail("trailing characters");
        if (error) *error = p.error;
        return false;
    }
    *out = std::move(v);
    return true;
}

const Value* Find(const Value& root, const std::string& path) {
    const Value* v = &root;
    size_t pos = 0;
    if (path.empty()) return v;
    while (pos <= path.size()) {
        size_t dot = path.find('.', pos);
        if (dot == std::string::npos) dot = path.size();
        const std::string part = path.substr(pos, dot - pos);
        pos = dot + 1;
        if (v->type == Value::Type::Object) {
            const Value* next = nullptr;
            for (const auto& f : v->fields)
                if (f.first == part) { next = &f.second; break; }
            if (!next) return nullptr;
            v = next;
        } else if (v->type == Value::Type::Array) {
            char* end = nullptr;
            const unsigned long long n = strtoull(part.c_str(), &end, 10);
            if (part.empty() || *end || n < 1 || n > v->items.size()) return nullptr;
            v = &v->items[(size_t)n - 1];
        } else {
            return nullptr;
        }
        if (dot == path.size()) break;
    }
    return v;
}

std::string Quote(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = (unsigned char)s[i];
        const size_t n = Utf8Length(s, i);
        if (!n) { out += "\xEF\xBF\xBD"; ++i; continue; }
        if (n > 1) { out.append(s, i, n); i += n; continue; }
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20 || c == 0x7f) {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out.push_back((char)c);
            }
        }
        ++i;
    }
    out.push_back('"');
    return out;
}

bool FormatNumber(double v, std::string* out) {
    if (!std::isfinite(v)) return false;
    if (v == std::floor(v) && std::fabs(v) < 1e15) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.0f", v);
        *out = std::strcmp(buf, "-0") == 0 ? "0" : buf;
        return true;
    }
    char buf[40];
    for (int precision = 15; precision <= 17; ++precision) {
        snprintf(buf, sizeof(buf), "%.*g", precision, v);
        if (strtod(buf, nullptr) == v) break;
    }
    *out = buf;
    return true;
}

std::string UrlEncode(const std::string& s) {
    static const char kHex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back((char)c);
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 15]);
        }
    }
    return out;
}

} // namespace json
} // namespace httpc
