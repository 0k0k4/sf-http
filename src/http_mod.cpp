// StormFabric mod: HTTP/HTTPS client for addon and microcontroller scripts.
// Global table `http` in addon/mission VMs and microcontroller VMs. Polling API: a Lua
// function cannot be retained by a native mod, so scripts call http.poll() from onTick.
// No game address: only host->api and the Windows API.
#include "sw_mod_sdk.h"
#include "http_json.h"
#include "http_registry.h"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <string>
#include <vector>

#define HTTP_MOD_VERSION "0.1.0"

namespace {

using httpc::json::Value;

const SwModHost* g_host = nullptr;
httpc::Registry* g_registry = nullptr; // heap: never destroyed inside DllMain
std::atomic<uint64_t> g_nextOwner{1};
std::atomic<int> g_addonVms{0};
bool g_hasGetGlobal = false;
bool g_hasCall = false;
bool g_hasPushValue = false;

const char kConfigKey[] = "http.cfg";
const char kOwnerField[] = "__owner";
const char kTableName[] = "http";
int kMicrocontrollerTag = 0; // its address marks the microcontroller copy of the functions

constexpr size_t kMaxUrlBytes = 8192;
constexpr size_t kMaxOptionsBytes = 16384;
constexpr size_t kMaxJsonBytes = 1048576;
constexpr size_t kMaxJsonElements = 100000;

void Log(const std::string& message) {
    if (g_host) g_host->log(g_host->modId, message.c_str());
}

const SwLuaApi* Lua() { return g_host->api->lua; }
const SwLuaExtrasApi* X() { return g_host->api->lua->extras; }
bool IsMicrocontroller(void* userData) { return userData == &kMicrocontrollerTag; }

SwLuaType TypeAt(void* L, int index) {
    SwLuaType t = SW_LUA_TYPE_NONE;
    if (X()->getType(g_host, L, index, &t) != SW_LUA_OK) return SW_LUA_TYPE_NONE;
    return t;
}
bool IsType(void* L, int index, SwLuaType type) { return TypeAt(L, index) == type; }
bool IsNumber(void* L, int index) {
    const SwLuaType t = TypeAt(L, index);
    return t == SW_LUA_TYPE_NUMBER || t == SW_LUA_TYPE_INTEGER;
}
bool IsAbsent(void* L, int index) { return index > Lua()->getTop(L) || IsType(L, index, SW_LUA_TYPE_NIL); }

// Copies a Lua string byte for byte (binary and embedded NUL preserved).
bool ArgBytes(void* L, int index, std::string* out, size_t maxBytes) {
    if (!IsType(L, index, SW_LUA_TYPE_STRING)) return false;
    char probe[1];
    size_t length = 0;
    const SwLuaStatus s = X()->copyString(g_host, L, index, probe, sizeof(probe), &length);
    if ((s != SW_LUA_OK && s != SW_LUA_BUFFER_TOO_SMALL) || length > maxBytes) return false;
    out->assign(length + 1, '\0');
    if (X()->copyString(g_host, L, index, &(*out)[0], out->size(), &length) != SW_LUA_OK) return false;
    out->resize(length);
    return true;
}

// Positive integer id, or 0.
uint64_t ArgId(void* L, int index) {
    if (!IsNumber(L, index)) return 0;
    const double v = Lua()->argNumber(L, index, 0);
    if (!(v >= 1 && v <= 9007199254740992.0) || v != (double)(uint64_t)v) return 0;
    return (uint64_t)v;
}

bool PushString(void* L, const std::string& s) { return X()->pushString(g_host, L, s.data(), s.size()) == SW_LUA_OK; }
bool PushInt(void* L, int64_t v) { return X()->pushInteger(g_host, L, v) == SW_LUA_OK; }

// Drops everything above `top`.
void PopTo(void* L, int top) {
    const int extra = Lua()->getTop(L) - top;
    if (extra > 0) X()->pop(g_host, L, extra);
}

// nil, message: the usual Lua failure pair. Returns the number of values pushed.
int PushFailure(void* L, const std::string& message) {
    if (!Lua()->checkStack(L, 2) || X()->pushNil(g_host, L) != SW_LUA_OK) return 0;
    return PushString(L, message) ? 2 : 1;
}

// Identifies the calling script VM without keeping its state: each VM has its own `http`
// table, which receives a numeric token on first use. 0 when unavailable.
uint64_t Owner(void* L) {
    const SwLuaExtrasApi* x = X();
    if (!g_hasGetGlobal || !Lua()->checkStack(L, 3)) return 0;
    const int top = Lua()->getTop(L);
    uint64_t owner = 0;
    if (x->getGlobal(g_host, L, kTableName) == SW_LUA_OK && IsType(L, -1, SW_LUA_TYPE_TABLE)) {
        if (x->getField(g_host, L, -1, kOwnerField) == SW_LUA_OK && IsNumber(L, -1)) {
            // argNumber only accepts positive indices (negative ones return the default).
            const double v = Lua()->argNumber(L, Lua()->getTop(L), 0);
            if (v >= 1) owner = (uint64_t)v;
        }
        PopTo(L, top + 1); // keep the table only
        if (!owner) {
            const uint64_t fresh = g_nextOwner++;
            if (x->pushInteger(g_host, L, (int64_t)fresh) == SW_LUA_OK && x->setField(g_host, L, -2, kOwnerField) == SW_LUA_OK)
                owner = fresh;
        }
    }
    PopTo(L, top);
    return owner;
}

// ---- Requests ---------------------------------------------------------------------------

int Submit(void* L, void* userData, const std::string& method, int urlIndex, int bodyIndex, int optionsIndex) {
    std::string url, body, options, error;
    const size_t maxBody = g_registry->GetLimits().maxBodyBytes;
    if (!ArgBytes(L, urlIndex, &url, kMaxUrlBytes)) return PushFailure(L, "url must be a string");
    if (bodyIndex && !IsAbsent(L, bodyIndex) && !ArgBytes(L, bodyIndex, &body, maxBody))
        return PushFailure(L, "body must be a string of at most " + std::to_string(maxBody) + " bytes");
    if (optionsIndex && !IsAbsent(L, optionsIndex) && !ArgBytes(L, optionsIndex, &options, kMaxOptionsBytes))
        return PushFailure(L, "options must be a string");
    const uint64_t id =
        g_registry->Submit(Owner(L), IsMicrocontroller(userData), method, url, body, options, &error);
    if (!id) return PushFailure(L, error);
    if (!Lua()->checkStack(L, 1)) return 0;
    return PushInt(L, (int64_t)id) ? 1 : 0;
}

// http.request(method, url [, body [, options]]) -> id | nil, error
int LuaRequest(void* L, void* userData) {
    std::string method;
    if (!ArgBytes(L, 1, &method, 16)) return PushFailure(L, "method must be a string");
    return Submit(L, userData, method, 2, 3, 4);
}
// http.get(url [, options]) -> id | nil, error
int LuaGet(void* L, void* userData) { return Submit(L, userData, "GET", 1, 0, 2); }
// http.post(url, body [, options]) -> id | nil, error
int LuaPost(void* L, void* userData) { return Submit(L, userData, "POST", 1, 2, 3); }

// Response headers as a table with lower-case names.
bool PushHeaders(void* L, const httpc::Result& r) {
    if (X()->createTable(g_host, L, 0) != SW_LUA_OK) return false;
    for (const auto& h : r.headers) {
        if (h.first.empty() || h.first.size() > 255) continue;
        if (!Lua()->checkStack(L, 1) || !PushString(L, h.second)) return false;
        if (X()->setField(g_host, L, -2, h.first.c_str()) != SW_LUA_OK) {
            X()->pop(g_host, L, 1);
            return false;
        }
    }
    return true;
}

// http.poll([id]) -> id, "response", status, body, headers | id, "error", message, code | nothing
int LuaPoll(void* L, void*) {
    uint64_t id = 0;
    if (!IsAbsent(L, 1)) {
        id = ArgId(L, 1);
        if (!id) return 0;
    }
    uint64_t from = 0;
    httpc::Result r;
    if (!g_registry->Poll(Owner(L), id, &from, &r)) return 0;
    const int top = Lua()->getTop(L);
    // The result is already taken: on a failed push, return what was pushed (balanced).
    if (!Lua()->checkStack(L, 6) || !PushInt(L, (int64_t)from)) return 0;
    if (r.ok) {
        if (!PushString(L, "response")) return 1;
        if (!PushInt(L, r.status)) return 2;
        if (!PushString(L, r.body)) return 3;
        if (r.headers.empty()) return 4;
        if (!PushHeaders(L, r)) {
            PopTo(L, top + 4);
            return 4;
        }
        return 5;
    }
    if (!PushString(L, "error")) return 1;
    if (!PushString(L, r.error)) return 2;
    return PushInt(L, r.code) ? 4 : 3;
}

const char* StateName(httpc::JobState s) {
    switch (s) {
    case httpc::JobState::Queued: return "queued";
    case httpc::JobState::Running: return "running";
    case httpc::JobState::Done: return "done";
    default: return "unknown";
    }
}

// http.state(id) -> "queued" | "running" | "done" | "unknown"
int LuaState(void* L, void*) {
    const uint64_t id = ArgId(L, 1);
    const httpc::JobState s = id ? g_registry->State(Owner(L), id) : httpc::JobState::Unknown;
    if (!Lua()->checkStack(L, 1)) return 0;
    return PushString(L, StateName(s)) ? 1 : 0;
}

// http.cancel(id) -> bool
int LuaCancel(void* L, void*) {
    const uint64_t id = ArgId(L, 1);
    const bool ok = id && g_registry->Cancel(Owner(L), id);
    if (!Lua()->checkStack(L, 1)) return 0;
    Lua()->pushBool(L, ok);
    return 1;
}

// http.pending() -> number of requests of this script not returned by poll yet
int LuaPending(void* L, void*) {
    const size_t n = g_registry->Pending(Owner(L));
    if (!Lua()->checkStack(L, 1)) return 0;
    return PushInt(L, (int64_t)n) ? 1 : 0;
}

// http.version() -> string
int LuaVersion(void* L, void*) {
    if (!Lua()->checkStack(L, 1)) return 0;
    return PushString(L, HTTP_MOD_VERSION) ? 1 : 0;
}

// http.urlencode(text) -> text
int LuaUrlEncode(void* L, void*) {
    std::string text;
    if (!ArgBytes(L, 1, &text, kMaxUrlBytes)) return PushFailure(L, "urlencode: text must be a string");
    if (!Lua()->checkStack(L, 1)) return 0;
    return PushString(L, httpc::json::UrlEncode(text)) ? 1 : 0;
}

// ---- JSON -------------------------------------------------------------------------------

bool WriteJson(const Value& v, std::string* out) {
    switch (v.type) {
    case Value::Type::Null: *out += "null"; return true;
    case Value::Type::Bool: *out += v.boolean ? "true" : "false"; return true;
    case Value::Type::Number: {
        std::string n;
        if (!httpc::json::FormatNumber(v.number, &n)) return false;
        *out += n;
        return true;
    }
    case Value::Type::String: *out += httpc::json::Quote(v.string); return true;
    case Value::Type::Array:
        out->push_back('[');
        for (size_t i = 0; i < v.items.size(); ++i) {
            if (i) out->push_back(',');
            if (!WriteJson(v.items[i], out)) return false;
        }
        out->push_back(']');
        return true;
    default:
        out->push_back('{');
        for (size_t i = 0; i < v.fields.size(); ++i) {
            if (i) out->push_back(',');
            *out += httpc::json::Quote(v.fields[i].first);
            out->push_back(':');
            if (!WriteJson(v.fields[i].second, out)) return false;
        }
        out->push_back('}');
        return true;
    }
}

size_t CountElements(const Value& v) {
    size_t n = 1;
    for (const Value& item : v.items) n += CountElements(item);
    for (const auto& f : v.fields) n += CountElements(f.second);
    return n;
}

bool PushJson(void* L, const Value& v, std::string* error);

bool HasGlobalFunction(void* L, const char* name) {
    if (!g_hasGetGlobal || !Lua()->checkStack(L, 1)) return false;
    const int top = Lua()->getTop(L);
    const bool found = X()->getGlobal(g_host, L, name) == SW_LUA_OK && IsType(L, -1, SW_LUA_TYPE_FUNCTION);
    PopTo(L, top);
    return found;
}

// table[key] = value through rawset(table, key, value): integer keys, and string keys that
// setField cannot take (empty, over 255 bytes, embedded NUL). Needs call + pushValue.
bool RawSet(void* L, int table, bool integerKey, int64_t index, const std::string& key, const Value& value,
            std::string* error) {
    if (!g_hasCall || !g_hasPushValue) {
        *error = integerKey ? "JSON arrays need game build 1.15.23 and a StormFabric loader with pushValue"
                            : "this JSON key needs game build 1.15.23 and a StormFabric loader with pushValue";
        return false;
    }
    const int top = Lua()->getTop(L);
    if (!Lua()->checkStack(L, 4) || X()->getGlobal(g_host, L, "rawset") != SW_LUA_OK ||
        !IsType(L, -1, SW_LUA_TYPE_FUNCTION)) {
        PopTo(L, top);
        *error = "rawset is not available in this Lua VM";
        return false;
    }
    if (X()->pushValue(g_host, L, table) != SW_LUA_OK || !(integerKey ? PushInt(L, index) : PushString(L, key)) ||
        !PushJson(L, value, error)) {
        PopTo(L, top);
        if (error->empty()) *error = "out of Lua stack";
        return false;
    }
    char message[256];
    const SwLuaStatus s = X()->call(g_host, L, 3, 0, message, sizeof(message));
    if (s != SW_LUA_OK) {
        PopTo(L, top);
        *error = s == SW_LUA_CALL_FAILED ? std::string("rawset failed: ") + message
                                         : "JSON arrays need game build 1.15.23 (Lua calls unsupported here)";
        if (s == SW_LUA_UNSUPPORTED) g_hasCall = false;
        return false;
    }
    return true;
}

// Fills the array table at `table` with table.move(table.pack(v1..vk), 1, k, offset, t),
// k values per call. Stormworks keeps the `table` library in every VM but removes rawset
// from microcontrollers (and addons): rawset is only the fallback.
bool FillArray(void* L, int table, const std::vector<Value>& items, std::string* error) {
    if (!g_hasCall || !g_hasPushValue) {
        *error = "JSON arrays need game build 1.15.23 and a StormFabric loader with pushValue";
        return false;
    }
    constexpr size_t kChunk = 200; // extras->call takes at most 255 arguments
    const int top = Lua()->getTop(L);
    bool haveTableLib = false;
    if (Lua()->checkStack(L, 3) && X()->getGlobal(g_host, L, "table") == SW_LUA_OK && IsType(L, -1, SW_LUA_TYPE_TABLE)) {
        haveTableLib = X()->getField(g_host, L, -1, "pack") == SW_LUA_OK && IsType(L, -1, SW_LUA_TYPE_FUNCTION);
        X()->pop(g_host, L, 1);
        haveTableLib = haveTableLib && X()->getField(g_host, L, -1, "move") == SW_LUA_OK && IsType(L, -1, SW_LUA_TYPE_FUNCTION);
        X()->pop(g_host, L, 1);
    }
    if (!haveTableLib) {
        PopTo(L, top);
        for (size_t i = 0; i < items.size(); ++i) {
            if (items[i].type == Value::Type::Null) continue; // nil leaves a hole, as in any Lua JSON
            if (!RawSet(L, table, true, (int64_t)i + 1, std::string(), items[i], error)) return false;
        }
        return true;
    }
    const int lib = Lua()->getTop(L);
    for (size_t offset = 0; offset < items.size(); offset += kChunk) {
        const size_t k = (std::min)(kChunk, items.size() - offset);
        if (!Lua()->checkStack(L, (int)k + 8) || X()->getField(g_host, L, lib, "move") != SW_LUA_OK ||
            X()->getField(g_host, L, lib, "pack") != SW_LUA_OK) {
            PopTo(L, top);
            *error = "out of Lua stack";
            return false;
        }
        for (size_t i = 0; i < k; ++i) {
            if (!PushJson(L, items[offset + i], error)) { // null -> nil keeps the index
                PopTo(L, top);
                if (error->empty()) *error = "out of Lua stack";
                return false;
            }
        }
        char message[256];
        SwLuaStatus s = X()->call(g_host, L, (int)k, 1, message, sizeof(message)); // -> move, packed
        if (s == SW_LUA_OK) {
            if (!PushInt(L, 1) || !PushInt(L, (int64_t)k) || !PushInt(L, (int64_t)offset + 1) ||
                X()->pushValue(g_host, L, table) != SW_LUA_OK) {
                PopTo(L, top);
                *error = "out of Lua stack";
                return false;
            }
            s = X()->call(g_host, L, 5, 0, message, sizeof(message));
        }
        if (s != SW_LUA_OK) {
            PopTo(L, top);
            if (s == SW_LUA_UNSUPPORTED) g_hasCall = false;
            *error = s == SW_LUA_CALL_FAILED ? std::string("table.move failed: ") + message
                                             : "JSON arrays need game build 1.15.23 (Lua calls unsupported here)";
            return false;
        }
    }
    PopTo(L, top);
    return true;
}

// Pushes a JSON value as Lua: objects and arrays become tables, null becomes nil.
bool PushJson(void* L, const Value& v, std::string* error) {
    if (!Lua()->checkStack(L, 2)) {
        *error = "out of Lua stack";
        return false;
    }
    switch (v.type) {
    case Value::Type::Null: return X()->pushNil(g_host, L) == SW_LUA_OK;
    case Value::Type::Bool: Lua()->pushBool(L, v.boolean); return true;
    case Value::Type::Number:
        if (v.integer) return PushInt(L, (int64_t)v.number);
        Lua()->pushNumber(L, v.number);
        return true;
    case Value::Type::String: return PushString(L, v.string);
    case Value::Type::Array: {
        const uint32_t slots = (uint32_t)(std::min<size_t>)(v.items.size(), SW_LUA_MAX_ARRAY_SLOTS);
        if (X()->createTable(g_host, L, slots) != SW_LUA_OK) return false;
        return v.items.empty() || FillArray(L, Lua()->getTop(L), v.items, error);
    }
    default: {
        if (X()->createTable(g_host, L, 0) != SW_LUA_OK) return false;
        const int table = Lua()->getTop(L);
        for (const auto& f : v.fields) {
            if (f.second.type == Value::Type::Null) continue;
            const std::string& key = f.first;
            if (!key.empty() && key.size() <= 255 && key.find('\0') == std::string::npos) {
                if (!PushJson(L, f.second, error)) return false;
                if (X()->setField(g_host, L, table, key.c_str()) != SW_LUA_OK) {
                    *error = "cannot set field " + key;
                    return false;
                }
            } else if (HasGlobalFunction(L, "rawset")) {
                if (!RawSet(L, table, false, 0, key, f.second, error)) return false;
            } // else: no rawset in this VM (microcontrollers): the odd key is skipped
        }
        return true;
    }
    }
}

bool ParseJsonArg(void* L, int index, Value* out, std::string* error) {
    std::string text;
    if (!ArgBytes(L, index, &text, kMaxJsonBytes)) {
        *error = "text must be a string of at most 1 MiB";
        return false;
    }
    if (!httpc::json::Parse(text, out, error)) return false;
    if (CountElements(*out) > kMaxJsonElements) {
        *error = "JSON has more than " + std::to_string(kMaxJsonElements) + " values";
        return false;
    }
    return true;
}

// http.json_decode(text) -> value | nil, error
int LuaJsonDecode(void* L, void*) {
    Value v;
    std::string error;
    if (!ParseJsonArg(L, 1, &v, &error)) return PushFailure(L, "json_decode: " + error);
    const int top = Lua()->getTop(L);
    if (!PushJson(L, v, &error)) {
        PopTo(L, top);
        return PushFailure(L, "json_decode: " + (error.empty() ? std::string("out of Lua memory") : error));
    }
    return 1;
}

// http.field(text, path) -> value | nil. "a.b.1.c": object keys and 1-based indexes.
// Strings, numbers and booleans come back as Lua values; objects and arrays as JSON text.
// Works without Lua calls (both game builds).
int LuaField(void* L, void*) {
    Value v;
    std::string error, path;
    if (!ParseJsonArg(L, 1, &v, &error)) return PushFailure(L, "field: " + error);
    if (!ArgBytes(L, 2, &path, 1024)) return PushFailure(L, "field: path must be a string");
    const Value* found = httpc::json::Find(v, path);
    if (!found || found->type == Value::Type::Null) return 0;
    if (found->type == Value::Type::Array || found->type == Value::Type::Object) {
        std::string text;
        if (!WriteJson(*found, &text) || !Lua()->checkStack(L, 1)) return 0;
        return PushString(L, text) ? 1 : 0;
    }
    const int top = Lua()->getTop(L);
    if (!PushJson(L, *found, &error)) {
        PopTo(L, top);
        return 0;
    }
    return 1;
}

struct Encoder {
    void* L;
    std::string out;
    std::string error;
    size_t elements = 0;

    bool Fail(const std::string& e) {
        if (error.empty()) error = e;
        return false;
    }

    bool Number(int index) {
        const bool integer = IsType(L, index, SW_LUA_TYPE_INTEGER);
        const double v = Lua()->argNumber(L, index, 0);
        std::string n;
        if (integer && v >= -9.2e18 && v <= 9.2e18) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%lld", (long long)v);
            n = buf;
        } else if (!httpc::json::FormatNumber(v, &n)) {
            return Fail("NaN and infinity cannot be encoded");
        }
        out += n;
        return true;
    }

    // Value at absolute index `index`.
    bool Value(int index, int depth) {
        if (++elements > kMaxJsonElements) return Fail("more than " + std::to_string(kMaxJsonElements) + " values");
        if (out.size() > kMaxJsonBytes) return Fail("result exceeds 1 MiB");
        switch (TypeAt(L, index)) {
        case SW_LUA_TYPE_NIL: out += "null"; return true;
        case SW_LUA_TYPE_BOOLEAN: out += Lua()->argBool(L, index, false) ? "true" : "false"; return true;
        case SW_LUA_TYPE_NUMBER:
        case SW_LUA_TYPE_INTEGER: return Number(index);
        case SW_LUA_TYPE_STRING: {
            std::string s;
            if (!ArgBytes(L, index, &s, kMaxJsonBytes)) return Fail("string too long");
            out += httpc::json::Quote(s);
            return true;
        }
        case SW_LUA_TYPE_TABLE: return Table(index, depth);
        default: return Fail("cannot encode a function, userdata or thread");
        }
    }

    struct Entry {
        bool integerKey = false;
        double number = 0;
        std::string key; // string key, or the number formatted
        std::string json;
    };

    // Iterates with next(t, k). Keys stay on the stack (no lua_remove): one slot per entry,
    // popped when the table is done.
    bool Table(int index, int depth) {
        if (depth >= httpc::json::kMaxDepth) return Fail("nesting deeper than 64 (or a cycle)");
        if (!g_hasCall || !g_hasPushValue)
            return Fail("tables need game build 1.15.23 and a StormFabric loader with pushValue");
        const int base = Lua()->getTop(L);
        std::vector<Entry> entries;
        bool ok = true;
        for (;;) {
            if (!Lua()->checkStack(L, 4)) { ok = Fail("out of Lua stack"); break; }
            if (X()->getGlobal(g_host, L, "next") != SW_LUA_OK || !IsType(L, -1, SW_LUA_TYPE_FUNCTION)) {
                ok = Fail("next is not available in this Lua VM");
                break;
            }
            const bool first = Lua()->getTop(L) == base + 1;
            if (X()->pushValue(g_host, L, index) != SW_LUA_OK ||
                (first ? X()->pushNil(g_host, L) : X()->pushValue(g_host, L, -3)) != SW_LUA_OK) {
                ok = Fail("out of Lua stack");
                break;
            }
            char message[256];
            const SwLuaStatus s = X()->call(g_host, L, 2, 2, message, sizeof(message));
            if (s != SW_LUA_OK) {
                if (s == SW_LUA_UNSUPPORTED) g_hasCall = false;
                ok = Fail(s == SW_LUA_CALL_FAILED ? std::string("next failed: ") + message
                                                  : "tables need game build 1.15.23");
                break;
            }
            // Stack: base.. [previous keys] key value
            const int valueIndex = Lua()->getTop(L), keyIndex = valueIndex - 1;
            if (IsType(L, keyIndex, SW_LUA_TYPE_NIL)) break;
            Entry e;
            const SwLuaType kt = TypeAt(L, keyIndex);
            if (kt == SW_LUA_TYPE_STRING) {
                if (!ArgBytes(L, keyIndex, &e.key, kMaxJsonBytes)) { ok = Fail("key too long"); break; }
            } else if (kt == SW_LUA_TYPE_NUMBER || kt == SW_LUA_TYPE_INTEGER) {
                e.number = Lua()->argNumber(L, keyIndex, 0);
                e.integerKey = e.number >= 1 && e.number <= 9007199254740992.0 && e.number == (double)(int64_t)e.number;
                if (!httpc::json::FormatNumber(e.number, &e.key)) { ok = Fail("NaN key"); break; }
            } else {
                ok = Fail("table keys must be strings or numbers");
                break;
            }
            std::string saved;
            saved.swap(out);
            const bool valueOk = Value(valueIndex, depth + 1);
            e.json.swap(out);
            out.swap(saved);
            if (!valueOk) { ok = false; break; }
            entries.push_back(std::move(e));
            X()->pop(g_host, L, 1); // the value; the key stays for the next call
        }
        PopTo(L, base);
        if (!ok) return false;

        bool isArray = true;
        for (const Entry& e : entries)
            if (!e.integerKey) { isArray = false; break; }
        if (isArray) {
            std::vector<const Entry*> byIndex(entries.size(), nullptr);
            for (const Entry& e : entries) {
                const double k = e.number;
                if (k > (double)entries.size() || byIndex[(size_t)k - 1]) { isArray = false; break; }
                byIndex[(size_t)k - 1] = &e;
            }
            if (isArray) { // keys exactly 1..n; an empty table is an array
                out.push_back('[');
                for (size_t i = 0; i < byIndex.size(); ++i) {
                    if (i) out.push_back(',');
                    out += byIndex[i]->json;
                }
                out.push_back(']');
                return true;
            }
        }
        out.push_back('{');
        for (size_t i = 0; i < entries.size(); ++i) {
            if (i) out.push_back(',');
            out += httpc::json::Quote(entries[i].key);
            out.push_back(':');
            out += entries[i].json;
        }
        out.push_back('}');
        return true;
    }
};

// http.json_encode(value) -> text | nil, error
int LuaJsonEncode(void* L, void*) {
    if (Lua()->getTop(L) < 1) return PushFailure(L, "json_encode: value expected");
    Encoder e{L};
    const int top = Lua()->getTop(L);
    const bool ok = e.Value(1, 0);
    PopTo(L, top);
    if (!ok) return PushFailure(L, "json_encode: " + e.error);
    if (e.out.size() > kMaxJsonBytes) return PushFailure(L, "json_encode: result exceeds 1 MiB");
    if (!Lua()->checkStack(L, 1)) return 0;
    return PushString(L, e.out) ? 1 : 0;
}

// ---- Registration -------------------------------------------------------------------------

#define HTTP_FUNCTIONS(tag)                                                                    \
    {"request", &LuaRequest, tag}, {"get", &LuaGet, tag}, {"post", &LuaPost, tag},             \
    {"poll", &LuaPoll, tag}, {"state", &LuaState, tag}, {"cancel", &LuaCancel, tag},           \
    {"pending", &LuaPending, tag}, {"version", &LuaVersion, tag},                              \
    {"json_decode", &LuaJsonDecode, tag}, {"json_encode", &LuaJsonEncode, tag},                \
    {"field", &LuaField, tag}, {"urlencode", &LuaUrlEncode, tag}

const SwLuaFunction kAddonFunctions[] = {HTTP_FUNCTIONS(nullptr)};
const SwLuaFunction kMicrocontrollerFunctions[] = {HTTP_FUNCTIONS(&kMicrocontrollerTag)};
constexpr size_t kFunctionCount = sizeof(kAddonFunctions) / sizeof(kAddonFunctions[0]);

void LoadConfig(const SwModHost* host) {
    const SwStorageApi* storage = host->api->storage;
    httpc::Limits limits;
    if (!storage || storage->structSize < offsetof(SwStorageApi, writeConfig) + sizeof(storage->writeConfig) ||
        !(host->api->runtime->capabilities() & SW_CAP_MOD_STORAGE)) {
        Log("storage unavailable: default configuration");
        g_registry->Configure(limits);
        return;
    }
    std::vector<uint8_t> buffer(64 * 1024);
    size_t size = 0;
    const SwStorageStatus s = storage->readConfig(host, kConfigKey, buffer.data(), buffer.size(), &size);
    if (s == SW_STORAGE_NOT_FOUND) {
        const std::string text = httpc::DefaultConfigText();
        storage->writeConfig(host, kConfigKey, (const uint8_t*)text.data(), text.size());
        Log("wrote default http.cfg");
    } else if (s == SW_STORAGE_OK) {
        std::string error;
        if (!httpc::ParseConfig(std::string((const char*)buffer.data(), size), &limits, &error)) {
            Log("http.cfg ignored: " + error);
            limits = httpc::Limits();
        }
    } else {
        Log("http.cfg unreadable (status " + std::to_string((int)s) + "): defaults");
    }
    g_registry->Configure(limits);
    std::string hosts;
    for (const std::string& h : limits.allowHosts) hosts += (hosts.empty() ? "" : ",") + h;
    static const char* const kLocal[] = {"none", "addons", "all"};
    Log("config: allow_hosts=" + hosts + " max_inflight=" + std::to_string(limits.maxInflight) +
        " max_per_vm=" + std::to_string(limits.maxPerVm) + " rate_per_minute=" + std::to_string(limits.ratePerMinute) +
        " local_network=" + kLocal[(int)limits.localNetwork] + " microcontrollers=" + (limits.microcontrollers ? "1" : "0"));
}

void OnSweep(void*) {
    if (g_registry) g_registry->Sweep();
}

// No addon VM left = the world was closed: drop every request.
void OnAddonVm(const SwEvent* event, void*) {
    if (!event || !g_registry) return;
    if (event->type == SW_EVENT_ADDON_LUA_CREATED) {
        ++g_addonVms;
    } else if (event->type == SW_EVENT_ADDON_LUA_DESTROYED) {
        int n = g_addonVms.load();
        while (n > 0 && !g_addonVms.compare_exchange_weak(n, n - 1)) {}
        if (n == 1 && g_registry->Count()) {
            Log("last addon VM destroyed: dropping requests");
            g_registry->CloseAll();
        }
    }
}

// Windows binds a DLL request by base name to an already loaded module: a mod DLL named
// like a System32 DLL (winhttp.dll...) would replace it for the whole process. Returns
// the clashing name.
std::string ShadowedSystemDll() {
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&ShadowedSystemDll), &self))
        return std::string();
    wchar_t path[MAX_PATH], system[MAX_PATH];
    const DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
    const UINT s = GetSystemDirectoryW(system, MAX_PATH);
    if (!n || n >= MAX_PATH || !s || s >= MAX_PATH) return std::string();
    const wchar_t* name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    const std::wstring candidate = std::wstring(system) + L"\\" + name;
    if (GetFileAttributesW(candidate.c_str()) == INVALID_FILE_ATTRIBUTES) return std::string();
    char narrow[MAX_PATH];
    WideCharToMultiByte(CP_UTF8, 0, name, -1, narrow, sizeof(narrow), nullptr, nullptr);
    return narrow;
}

} // namespace

SW_MOD_EXPORT bool SwModInit(const SwModHost* host) {
    if (!host || host->sdkVersion < 2 || !host->api || host->api->abiVersion != SW_ENGINE_API_VERSION ||
        !host->api->lua || !host->api->runtime) return false;
    const SwLuaApi* lua = host->api->lua;
    const SwRuntimeApi* runtime = host->api->runtime;
    // Byte-exact strings need extras->pushString/copyString; the rest is optional.
    if (lua->structSize < offsetof(SwLuaApi, extras) + sizeof(lua->extras) || !lua->extras ||
        lua->extras->structSize < offsetof(SwLuaExtrasApi, copyString) + sizeof(lua->extras->copyString)) {
        host->log(host->modId, "Lua extras API missing: disabled");
        return false;
    }
    const std::string shadowed = ShadowedSystemDll();
    if (!shadowed.empty()) {
        host->log(host->modId, (shadowed + " has the name of a Windows DLL and would replace it in the game: "
                                "rename the mod DLL. Disabled.").c_str());
        return false;
    }
    const SwLuaExtrasApi* x = lua->extras;
    const uint64_t caps = runtime->capabilities();
    g_host = host;
    g_hasGetGlobal = x->structSize >= offsetof(SwLuaExtrasApi, getGlobal) + sizeof(x->getGlobal) && x->getGlobal;
    g_hasCall = x->structSize >= offsetof(SwLuaExtrasApi, call) + sizeof(x->call) && x->call;
    g_hasPushValue = x->structSize >= offsetof(SwLuaExtrasApi, pushValue) + sizeof(x->pushValue) && x->pushValue;
    if (!g_hasGetGlobal) Log("extras->getGlobal unavailable: scripts share one request owner");
    if (!g_hasPushValue) Log("extras->pushValue unavailable (older loader): json_encode and JSON arrays disabled");
    g_registry = new httpc::Registry(nullptr, &Log);
    LoadConfig(host);

    // Addon/mission scripts (script.lua): newer reviewed build only.
    bool addon = false;
    if (lua->structSize >= offsetof(SwLuaApi, registerAddonLibrary) + sizeof(lua->registerAddonLibrary) &&
        (caps & SW_CAP_LUA_ADDON)) {
        addon = lua->registerAddonLibrary(host, kTableName, kAddonFunctions, kFunctionCount);
        if (!addon) Log("registerAddonLibrary(http) failed");
    } else {
        Log("addon Lua VMs unavailable on this game build: no addon library");
    }
    // Microcontroller Lua: both reviewed builds; can be turned off in http.cfg.
    bool mc = false;
    if (g_registry->GetLimits().microcontrollers) {
        mc = lua->registerLibrary(host, kTableName, kMicrocontrollerFunctions, kFunctionCount);
        if (!mc) Log("registerLibrary(http) failed: no microcontroller library");
    } else {
        Log("microcontroller library disabled (microcontrollers=0)");
    }
    if (!addon && !mc) {
        Log("no Lua library registered: disabled");
        delete g_registry;
        g_registry = nullptr;
        g_host = nullptr;
        return false;
    }
    Log(std::string("libraries: addon=") + (addon ? "yes" : "no") + " microcontroller=" + (mc ? "yes" : "no"));
    SwHandle handle = 0;
    if (runtime->subscribe(host, SW_EVENT_MASK(SW_EVENT_ADDON_LUA_CREATED) | SW_EVENT_MASK(SW_EVENT_ADDON_LUA_DESTROYED),
                           &OnAddonVm, nullptr, &handle) != SW_OK)
        Log("addon VM events unavailable: relying on idle_timeout");
    if (!(caps & SW_CAP_MAIN_THREAD_TASKS) || runtime->schedule(host, 1.0, 1.0, &OnSweep, nullptr, &handle) != SW_OK)
        Log("scheduled sweep unavailable: idle requests are released on the next request");
    Log("http " HTTP_MOD_VERSION " ready");
    return true;
}

SW_MOD_EXPORT void SwModUnload(void) {
    // Callbacks are already revoked: no Lua call or task can reach the registry now.
    if (g_registry) {
        g_registry->CloseAll();
        delete g_registry;
        g_registry = nullptr;
    }
    g_host = nullptr;
}
