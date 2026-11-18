// Offline tests of the http mod (registry + Lua bindings + JSON) through a fake
// StormFabric host: a small Lua stack with per-VM globals, tables with string and integer
// keys, builtin rawset/next for extras->call, a fake runtime and a fake storage.
// Needs tests/test_server.py running.   mod_test.exe [--port 8780]
#include "sw_mod_sdk.h"
#include "../src/http_client.h"
#include "../src/http_json.h"
#include "../src/http_registry.h"

#include <windows.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <vector>

// ---- Fake Lua -------------------------------------------------------------------------

struct Key {
    bool integer = false;
    int64_t i = 0;
    std::string s;
    bool operator<(const Key& o) const {
        if (integer != o.integer) return integer; // integer keys first
        return integer ? i < o.i : s < o.s;
    }
};
static Key SKey(const std::string& s) { Key k; k.s = s; return k; }
static Key IKey(int64_t i) { Key k; k.integer = true; k.i = i; return k; }

struct Value;
using Table = std::map<Key, Value>;
struct Value {
    SwLuaType type = SW_LUA_TYPE_NIL;
    bool b = false;
    double n = 0;
    std::string s;
    std::shared_ptr<Table> t;
    SwLuaCallback fn = nullptr;
    void* userData = nullptr;
    const char* builtin = nullptr; // "rawset" / "next"
};
static Value Nil() { return Value(); }
static Value Num(double n) { Value v; v.type = SW_LUA_TYPE_NUMBER; v.n = n; return v; }
static Value Int(double n) { Value v; v.type = SW_LUA_TYPE_INTEGER; v.n = n; return v; }
static Value Bool(bool b) { Value v; v.type = SW_LUA_TYPE_BOOLEAN; v.b = b; return v; }
static Value Str(const std::string& s) { Value v; v.type = SW_LUA_TYPE_STRING; v.s = s; return v; }
static Value NewTable() { Value v; v.type = SW_LUA_TYPE_TABLE; v.t = std::make_shared<Table>(); return v; }
static Value Builtin(const char* name) { Value v; v.type = SW_LUA_TYPE_FUNCTION; v.builtin = name; return v; }

// Lua key normalisation: a float key with an integer value is that integer.
static bool ToKey(const Value& v, Key* out) {
    if (v.type == SW_LUA_TYPE_STRING) { *out = SKey(v.s); return true; }
    if ((v.type == SW_LUA_TYPE_NUMBER || v.type == SW_LUA_TYPE_INTEGER) && v.n == (double)(int64_t)v.n) {
        *out = IKey((int64_t)v.n);
        return true;
    }
    if (v.type == SW_LUA_TYPE_BOOLEAN) { *out = SKey(v.b ? "\001true" : "\001false"); return true; }
    return false;
}
static Value KeyValue(const Key& k) { return k.integer ? Int((double)k.i) : Str(k.s); }

struct Vm {
    std::vector<Value> stack;
    Table globals; // string keys only
    std::string lib = "http";
    bool callSupported = true;
    int calls = 0;
    int moves = 0;
};

static Value* At(void* L, int index) {
    Vm* vm = (Vm*)L;
    const int size = (int)vm->stack.size();
    const int i = index > 0 ? index - 1 : size + index;
    if (index == 0 || i < 0 || i >= size) return nullptr;
    return &vm->stack[(size_t)i];
}
static void Push(void* L, Value v) { ((Vm*)L)->stack.push_back(std::move(v)); }

// Same rules as the loader: arg* take positive indices only.
static int F_getTop(void* L) { return (int)((Vm*)L)->stack.size(); }
static double F_argNumber(void* L, int i, double d) {
    Value* v = i > 0 ? At(L, i) : nullptr;
    return v && (v->type == SW_LUA_TYPE_NUMBER || v->type == SW_LUA_TYPE_INTEGER) ? v->n : d;
}
static bool F_argBool(void* L, int i, bool d) {
    Value* v = i > 0 ? At(L, i) : nullptr;
    if (!v) return d;
    if (v->type == SW_LUA_TYPE_NIL) return false;
    return v->type == SW_LUA_TYPE_BOOLEAN ? v->b : true;
}
static size_t F_argString(void* L, int i, char* out, size_t cap) {
    Value* v = i > 0 ? At(L, i) : nullptr;
    if (!v || v->type != SW_LUA_TYPE_STRING || !cap) return 0;
    const size_t n = v->s.size() < cap - 1 ? v->s.size() : cap - 1;
    memcpy(out, v->s.data(), n);
    out[n] = 0;
    return n;
}
static size_t g_maxStack = 0;
static bool F_checkStack(void* L, int n) {
    const size_t want = ((Vm*)L)->stack.size() + (size_t)n;
    if (want > g_maxStack) g_maxStack = want;
    return n >= 0 && n <= 4096 && want < 200000;
}
static void F_pushNumber(void* L, double n) { Push(L, Num(n)); }
static void F_pushBool(void* L, bool b) { Push(L, Bool(b)); }

static SwLuaStatus X_pushString(const SwModHost*, void* L, const char* b, size_t n) {
    if ((!b && n) || n > SW_LUA_MAX_STRING_BYTES) return SW_LUA_INVALID_ARGUMENT;
    Push(L, Str(std::string(b ? b : "", n)));
    return SW_LUA_OK;
}
static SwLuaStatus X_pushNil(const SwModHost*, void* L) { Push(L, Nil()); return SW_LUA_OK; }
static SwLuaStatus X_pushInteger(const SwModHost*, void* L, int64_t v) { Push(L, Int((double)v)); return SW_LUA_OK; }
static SwLuaStatus X_createTable(const SwModHost*, void* L, uint32_t slots) {
    if (slots > SW_LUA_MAX_ARRAY_SLOTS) return SW_LUA_INVALID_ARGUMENT;
    Push(L, NewTable());
    return SW_LUA_OK;
}
static bool ValidKey(const char* k) { return k && *k && strlen(k) <= 255; }
static SwLuaStatus X_getField(const SwModHost*, void* L, int i, const char* k) {
    Value* v = At(L, i);
    if (!v) return SW_LUA_OUT_OF_RANGE;
    if (!ValidKey(k)) return SW_LUA_INVALID_ARGUMENT;
    if (v->type != SW_LUA_TYPE_TABLE) return SW_LUA_TYPE_MISMATCH;
    auto it = v->t->find(SKey(k));
    Push(L, it == v->t->end() ? Nil() : it->second);
    return SW_LUA_OK;
}
static SwLuaStatus X_setField(const SwModHost*, void* L, int i, const char* k) {
    Value* v = At(L, i);
    Vm* vm = (Vm*)L;
    if (!v || vm->stack.empty()) return SW_LUA_OUT_OF_RANGE;
    if (!ValidKey(k)) return SW_LUA_INVALID_ARGUMENT;
    if (v->type != SW_LUA_TYPE_TABLE) return SW_LUA_TYPE_MISMATCH;
    std::shared_ptr<Table> t = v->t; // v may be the value being popped
    Value value = vm->stack.back();
    vm->stack.pop_back();
    if (value.type == SW_LUA_TYPE_NIL) t->erase(SKey(k));
    else (*t)[SKey(k)] = value;
    return SW_LUA_OK;
}
static SwLuaStatus X_getType(const SwModHost*, void* L, int i, SwLuaType* t) {
    Value* v = At(L, i);
    *t = v ? v->type : SW_LUA_TYPE_NONE;
    return v ? SW_LUA_OK : SW_LUA_OUT_OF_RANGE;
}
static SwLuaStatus X_pop(const SwModHost*, void* L, int n) {
    Vm* vm = (Vm*)L;
    if (n < 0 || n > (int)vm->stack.size()) return SW_LUA_OUT_OF_RANGE;
    vm->stack.resize(vm->stack.size() - (size_t)n);
    return SW_LUA_OK;
}
static SwLuaStatus X_raiseError(const SwModHost*, void*, const char*, size_t) { return SW_LUA_UNSUPPORTED; }
static SwLuaStatus X_copyString(const SwModHost*, void* L, int i, char* out, size_t cap, size_t* len) {
    Value* v = At(L, i);
    if (!v) return SW_LUA_OUT_OF_RANGE;
    if (v->type != SW_LUA_TYPE_STRING) return SW_LUA_TYPE_MISMATCH;
    *len = v->s.size();
    if (cap) {
        const size_t n = v->s.size() < cap - 1 ? v->s.size() : cap - 1;
        memcpy(out, v->s.data(), n);
        out[n] = 0;
    }
    return v->s.size() < cap ? SW_LUA_OK : SW_LUA_BUFFER_TOO_SMALL;
}
static SwLuaStatus X_getGlobal(const SwModHost*, void* L, const char* name) {
    Vm* vm = (Vm*)L;
    auto it = vm->globals.find(SKey(name));
    Push(L, it == vm->globals.end() ? Nil() : it->second);
    return SW_LUA_OK;
}
static void CopyError(const char* text, char* out, size_t cap) {
    if (out && cap) {
        strncpy_s(out, cap, text, _TRUNCATE);
    }
}
// Runs the builtin functions rawset and next, like lua_pcall with a fixed result count.
static SwLuaStatus X_call(const SwModHost*, void* L, int nargs, int nresults, char* err, size_t cap) {
    Vm* vm = (Vm*)L;
    if (!vm->callSupported) return SW_LUA_UNSUPPORTED;
    if (nargs < 0 || nresults < 0 || (int)vm->stack.size() < nargs + 1) return SW_LUA_OUT_OF_RANGE;
    const size_t fnIndex = vm->stack.size() - (size_t)nargs - 1;
    Value fn = vm->stack[fnIndex];
    if (fn.type != SW_LUA_TYPE_FUNCTION) return SW_LUA_TYPE_MISMATCH;
    std::vector<Value> args(vm->stack.begin() + (long long)fnIndex + 1, vm->stack.end());
    vm->stack.resize(fnIndex);
    ++vm->calls;
    std::vector<Value> results;
    auto fail = [&](const char* text) {
        CopyError(text, err, cap);
        return SW_LUA_CALL_FAILED;
    };
    if (fn.builtin && !strcmp(fn.builtin, "rawset")) {
        if (args.size() < 3 || args[0].type != SW_LUA_TYPE_TABLE) return fail("bad argument #1 to 'rawset'");
        Key k;
        if (!ToKey(args[1], &k)) return fail("invalid key");
        if (args[2].type == SW_LUA_TYPE_NIL) args[0].t->erase(k);
        else (*args[0].t)[k] = args[2];
        results.push_back(args[0]);
    } else if (fn.builtin && !strcmp(fn.builtin, "table.pack")) {
        Value packed = NewTable();
        for (size_t i = 0; i < args.size(); ++i)
            if (args[i].type != SW_LUA_TYPE_NIL) (*packed.t)[IKey((int64_t)i + 1)] = args[i];
        (*packed.t)[SKey("n")] = Int((double)args.size());
        results.push_back(packed);
    } else if (fn.builtin && !strcmp(fn.builtin, "table.move")) {
        // table.move(a1, f, e, t [, a2])
        if (args.size() < 4 || args[0].type != SW_LUA_TYPE_TABLE) return fail("bad argument #1 to 'move'");
        Value dest = args.size() >= 5 ? args[4] : args[0];
        if (dest.type != SW_LUA_TYPE_TABLE) return fail("bad argument #5 to 'move'");
        const int64_t f = (int64_t)args[1].n, e = (int64_t)args[2].n, t = (int64_t)args[3].n;
        for (int64_t i = f; i <= e; ++i) {
            auto it = args[0].t->find(IKey(i));
            if (it == args[0].t->end()) dest.t->erase(IKey(t + i - f));
            else (*dest.t)[IKey(t + i - f)] = it->second;
        }
        results.push_back(dest);
        ++vm->moves;
    } else if (fn.builtin && !strcmp(fn.builtin, "next")) {
        if (args.empty() || args[0].type != SW_LUA_TYPE_TABLE) return fail("bad argument #1 to 'next'");
        Table& t = *args[0].t;
        Table::iterator it;
        if (args.size() < 2 || args[1].type == SW_LUA_TYPE_NIL) {
            it = t.begin();
        } else {
            Key k;
            if (!ToKey(args[1], &k) || t.find(k) == t.end()) return fail("invalid key to 'next'");
            it = std::next(t.find(k));
        }
        if (it == t.end()) results.push_back(Nil());
        else { results.push_back(KeyValue(it->first)); results.push_back(it->second); }
    } else {
        return fail("attempt to call an unknown function");
    }
    results.resize((size_t)nresults);
    for (Value& v : results) vm->stack.push_back(v);
    return SW_LUA_OK;
}
static SwLuaStatus X_pushValue(const SwModHost*, void* L, int i) {
    Value* v = At(L, i);
    if (!v) return SW_LUA_OUT_OF_RANGE;
    Value copy = *v;
    Push(L, copy);
    return SW_LUA_OK;
}

static SwLuaExtrasApi g_extras = {sizeof(SwLuaExtrasApi), X_pushString, X_pushNil, X_pushInteger, X_createTable,
                                  X_getField, X_setField, X_getType, X_pop, X_raiseError, X_copyString,
                                  X_getGlobal, X_call, X_pushValue};

static std::vector<SwLuaFunction> g_library, g_mcLibrary;
static std::string g_libraryName, g_mcLibraryName;
static bool F_registerAddonLibrary(const SwModHost*, const char* name, const SwLuaFunction* f, size_t n) {
    g_libraryName = name;
    g_library.assign(f, f + n);
    return true;
}
static bool F_registerLibrary(const SwModHost*, const char* name, const SwLuaFunction* f, size_t n) {
    g_mcLibraryName = name;
    g_mcLibrary.assign(f, f + n);
    return true;
}
static bool F_enableQol(const SwModHost*) { return true; }
static void F_disableQol(const SwModHost*) {}

static SwLuaApi g_lua = {sizeof(SwLuaApi), F_enableQol, F_disableQol, F_registerLibrary, F_getTop, F_argNumber,
                         F_argBool, F_argString, F_checkStack, F_pushNumber, F_pushBool, nullptr, &g_extras,
                         F_registerAddonLibrary};

// ---- Fake runtime / storage / host ---------------------------------------------------

static uint64_t g_caps = SW_CAP_LUA_ADDON | SW_CAP_MAIN_THREAD_TASKS | SW_CAP_MOD_STORAGE;
static SwEventCallback g_eventCb = nullptr;
static SwTaskCallback g_taskCb = nullptr;
static uint64_t R_caps() { return g_caps; }
static SwResult R_subscribe(const SwModHost*, uint64_t, SwEventCallback cb, void*, SwHandle* h) {
    g_eventCb = cb;
    *h = 1;
    return SW_OK;
}
static SwResult R_unsubscribe(const SwModHost*, SwHandle) { return SW_OK; }
static SwResult R_run(const SwModHost*, SwTaskCallback, void*, SwHandle*) { return SW_OK; }
static SwResult R_schedule(const SwModHost*, double, double, SwTaskCallback cb, void*, SwHandle* h) {
    g_taskCb = cb;
    *h = 2;
    return SW_OK;
}
static SwResult R_cancel(const SwModHost*, SwHandle) { return SW_OK; }
static double R_time() { return 0; }
static bool R_isGame() { return true; }
static SwResult R_simTime(double*) { return SW_UNAVAILABLE; }
static SwRuntimeApi g_runtime = {offsetof(SwRuntimeApi, subscribeSimulationTick), R_caps, R_subscribe, R_unsubscribe,
                                 R_run, R_schedule, R_cancel, R_time, R_isGame, R_simTime};

static std::map<std::string, std::string> g_storage;
static SwStorageStatus S_read(const SwModHost*, const char* key, uint8_t* buf, size_t cap, size_t* size) {
    auto it = g_storage.find(key);
    if (it == g_storage.end()) { *size = 0; return SW_STORAGE_NOT_FOUND; }
    *size = it->second.size();
    if (cap < it->second.size()) return SW_STORAGE_BUFFER_TOO_SMALL;
    memcpy(buf, it->second.data(), it->second.size());
    return SW_STORAGE_OK;
}
static SwStorageStatus S_write(const SwModHost*, const char* key, const uint8_t* b, size_t n) {
    g_storage[key].assign((const char*)b, n);
    return SW_STORAGE_OK;
}
static SwStorageApi g_storageApi = {offsetof(SwStorageApi, readData), S_read, S_write, nullptr, nullptr};

static std::vector<std::string> g_logs;
static bool g_quiet = false;
static void H_log(const char*, const char* m) {
    if (!g_quiet) printf("      log: %s\n", m);
    g_logs.push_back(m);
}

static SwEngineApi g_api = {SW_ENGINE_API_VERSION, sizeof(SwEngineApi), nullptr, &g_lua, nullptr, &g_runtime,
                            nullptr, nullptr, &g_storageApi, nullptr, nullptr};
static SwModHost g_hostStruct = {SW_MOD_SDK_VERSION, nullptr, "http", "HTTP", "test", "test", ".",
                                 nullptr, nullptr, H_log, &g_api};

extern "C" bool SwModInit(const SwModHost* host);
extern "C" void SwModUnload(void);

// A new addon VM (or microcontroller VM): the library table registered for that kind of
// VM, plus next and table.pack/move. Like the game, microcontroller VMs have no rawset.
static std::unique_ptr<Vm> NewVm(bool mc = false) {
    auto vm = std::make_unique<Vm>();
    const auto& functions = mc ? g_mcLibrary : g_library;
    vm->lib = mc ? g_mcLibraryName : g_libraryName;
    Value table = NewTable();
    for (const SwLuaFunction& f : functions) {
        Value fn;
        fn.type = SW_LUA_TYPE_FUNCTION;
        fn.fn = f.callback;
        fn.userData = f.userData;
        (*table.t)[SKey(f.name)] = fn;
    }
    vm->globals[SKey(vm->lib)] = table;
    if (!mc) vm->globals[SKey("rawset")] = Builtin("rawset");
    vm->globals[SKey("next")] = Builtin("next");
    Value tableLib = NewTable();
    (*tableLib.t)[SKey("pack")] = Builtin("table.pack");
    (*tableLib.t)[SKey("move")] = Builtin("table.move");
    vm->globals[SKey("table")] = tableLib;
    return vm;
}

// ---- Test helpers ---------------------------------------------------------------------

static int g_failed = 0, g_passed = 0, g_port = 8780;
static bool g_ok = true;
#define CHECK(cond, ...)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("    FAIL line %d: %s -- ", __LINE__, #cond);            \
            printf(__VA_ARGS__);                                             \
            printf("\n");                                                    \
            g_ok = false;                                                    \
        }                                                                    \
    } while (0)

// Calls http.<name>(args...) and returns its results. Checks the stack contract:
// arguments untouched, exactly `returned` values pushed on top.
static std::vector<Value> Call(Vm& vm, const char* name, std::vector<Value> args) {
    Value& table = vm.globals[SKey(vm.lib)];
    auto it = table.t ? table.t->find(SKey(name)) : Table::iterator();
    if (!table.t || it == table.t->end()) {
        CHECK(false, "no function %s", name);
        return {};
    }
    vm.stack = args;
    const int n = it->second.fn(&vm, it->second.userData);
    CHECK(n >= 0 && vm.stack.size() == args.size() + (size_t)n, "%s: stack %zu args %zu results %d", name,
          vm.stack.size(), args.size(), n);
    for (size_t i = 0; i < args.size() && i < vm.stack.size(); ++i)
        CHECK(vm.stack[i].type == args[i].type, "%s: argument %zu modified", name, i + 1);
    std::vector<Value> out;
    if (n > 0 && vm.stack.size() >= (size_t)n) out.assign(vm.stack.end() - n, vm.stack.end());
    vm.stack.clear();
    return out;
}

static std::string Url(const char* path = "/echo") { return "http://127.0.0.1:" + std::to_string(g_port) + path; }

// Polls until a result arrives for the VM (optionally for one id).
static std::vector<Value> Wait(Vm& vm, double id = 0, double seconds = 10) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds((int)(seconds * 1000));
    while (std::chrono::steady_clock::now() < end) {
        auto r = Call(vm, "poll", id ? std::vector<Value>{Num(id)} : std::vector<Value>{});
        if (!r.empty()) return r;
        Sleep(5);
    }
    return {};
}

static bool Init(const char* config) {
    g_storage.clear();
    if (config) g_storage["http.cfg"] = config;
    g_logs.clear();
    return SwModInit(&g_hostStruct);
}

static void Run(const char* name, void (*fn)()) {
    printf("[ %s ]\n", name);
    g_ok = true;
    fn();
    printf("    %s\n", g_ok ? "ok" : "FAILED");
    (g_ok ? g_passed : g_failed)++;
}

static bool Logged(const char* needle) {
    for (const std::string& l : g_logs)
        if (l.find(needle) != std::string::npos) return true;
    return false;
}

static void ResetLibraries() {
    g_library.clear();
    g_libraryName.clear();
    g_mcLibrary.clear();
    g_mcLibraryName.clear();
}

static const Value* Field(const Value& t, const char* key) {
    if (t.type != SW_LUA_TYPE_TABLE) return nullptr;
    auto it = t.t->find(SKey(key));
    return it == t.t->end() ? nullptr : &it->second;
}
static const Value* Index(const Value& t, int64_t i) {
    if (t.type != SW_LUA_TYPE_TABLE) return nullptr;
    auto it = t.t->find(IKey(i));
    return it == t.t->end() ? nullptr : &it->second;
}

// ---- Tests --------------------------------------------------------------------------------

static void TestHelpers() {
    using namespace httpc;
    CHECK(UrlHost("https://User:pw@Example.COM:8443/x?y") == "example.com", "%s", UrlHost("https://User:pw@Example.COM:8443/x?y").c_str());
    CHECK(UrlHost("http://[::1]:80/") == "[::1]" && UrlHost("http://host.") == "host", "ipv6 / trailing dot");
    CHECK(SafeUrl("https://u:secret@h.com:8/path?token=x") == "https://h.com:8", "%s", SafeUrl("https://u:secret@h.com:8/path?token=x").c_str());
    CHECK(HostAllowed({"*.example.com"}, "api.example.com") && !HostAllowed({"*.example.com"}, "example.com") &&
              !HostAllowed({"*.example.com"}, "badexample.com") && !HostAllowed({}, "a"), "allow_hosts");
    for (const char* h : {"localhost", "a.localhost", "127.0.0.1", "127.1.2.3", "10.0.0.5", "192.168.1.1", "172.16.0.1",
                          "172.31.255.255", "169.254.1.1", "0.0.0.0", "100.64.0.1", "[::1]", "[fd00::1]", "[fe80::1]",
                          "[::ffff:192.168.0.1]", "2130706433", "0x7f.1", "0177.0.0.1"})
        CHECK(IsLocalHost(h), "local: %s", h);
    for (const char* h : {"example.com", "8.8.8.8", "172.32.0.1", "192.169.0.1", "[2001:db8::1]", "1.1.1.1", "deadbeef.com",
                          "3dmodels.example", "[::ffff:8.8.8.8]"})
        CHECK(!IsLocalHost(h), "public: %s", h);
    Limits l;
    std::string err;
    CHECK(ParseConfig(DefaultConfigText(), &l, &err), "defaults parse: %s", err.c_str());
    CHECK(ParseConfig("allow_hosts= A.com , *.b.org\nmax_per_vm=2 # c\n\nidle_timeout=1.5\r\nlocal_network=all\n", &l, &err) &&
              l.allowHosts.size() == 2 && l.allowHosts[0] == "a.com" && l.maxPerVm == 2 && l.idleSeconds == 1.5 &&
              l.localNetwork == LocalNetwork::All, "custom parse: %s", err.c_str());
    CHECK(!ParseConfig("max_per_vm=0", &l, &err) && !ParseConfig("nope=1", &l, &err) &&
              !ParseConfig("max_response_bytes=2000000", &l, &err) && !ParseConfig("junk", &l, &err) &&
              !ParseConfig("local_network=lan", &l, &err), "bad configs");

    json::Value v;
    CHECK(json::Parse(" {\"a\": [1, 2.5, -3e2, true, null, \"x\\u00e9\\ud83d\\ude00\"], \"b\": {}} ", &v, &err), "parse: %s", err.c_str());
    const json::Value* a = json::Find(v, "a");
    CHECK(a && a->items.size() == 6 && a->items[0].integer && !a->items[1].integer && a->items[2].number == -300 &&
              a->items[3].boolean && a->items[4].type == json::Value::Type::Null &&
              a->items[5].string == "x\xc3\xa9\xf0\x9f\x98\x80", "parsed values");
    CHECK(json::Find(v, "a.2")->number == 2.5 && !json::Find(v, "a.7") && !json::Find(v, "c") && !json::Find(v, "a.0"), "find");
    for (const char* bad : {"", "{", "[1,]", "{\"a\" 1}", "01", "1.", "\"\\x\"", "[1] x", "tru", "\"a\nb\""})
        CHECK(!json::Parse(bad, &v, &err), "invalid json accepted: %s", bad);
    std::string deep(65, '['), closing(65, ']');
    CHECK(!json::Parse(deep + closing, &v, &err), "depth limit");
    CHECK(json::Parse(deep.substr(1) + closing.substr(1), &v, &err), "64 levels accepted: %s", err.c_str());
    CHECK(json::Quote("a\"\\\n\x01\xff") == "\"a\\\"\\\\\\n\\u0001\xEF\xBF\xBD\"", "quote %s", json::Quote("a\"\\\n\x01\xff").c_str());
    std::string n;
    CHECK(json::FormatNumber(3, &n) && n == "3" && json::FormatNumber(0.1, &n) && n == "0.1" &&
              json::FormatNumber(-0.0, &n) && n == "0" && !json::FormatNumber(1.0 / 0.0 * 0.0, &n), "numbers");
    CHECK(json::UrlEncode("a b&c=d/é~") == "a%20b%26c%3Dd%2F%C3%A9~", "%s", json::UrlEncode("a b&c=d/é~").c_str());
}

static void TestInitChecks() {
    ResetLibraries();
    g_caps = SW_CAP_MAIN_THREAD_TASKS | SW_CAP_MOD_STORAGE; // older build: microcontrollers only
    CHECK(Init(nullptr), "init without SW_CAP_LUA_ADDON");
    CHECK(g_library.empty() && g_mcLibrary.size() == 12, "mc only (%zu, %zu)", g_library.size(), g_mcLibrary.size());
    CHECK(Logged("libraries: addon=no microcontroller=yes"), "libraries logged");
    SwModUnload();
    ResetLibraries();
    CHECK(!Init("microcontrollers=0\n"), "nothing to register: refused");
    g_caps = SW_CAP_LUA_ADDON | SW_CAP_MAIN_THREAD_TASKS | SW_CAP_MOD_STORAGE;

    const uint32_t saved = g_extras.structSize;
    g_extras.structSize = offsetof(SwLuaExtrasApi, copyString);
    CHECK(!Init(nullptr), "refused without extras->copyString");
    g_extras.structSize = offsetof(SwLuaExtrasApi, pushValue);
    CHECK(Init(nullptr), "init without pushValue");
    CHECK(Logged("pushValue unavailable"), "older loader logged");
    SwModUnload();
    g_extras.structSize = saved;

    ResetLibraries();
    CHECK(Init(nullptr), "init");
    CHECK(g_libraryName == "http" && g_library.size() == 12 && g_mcLibraryName == "http" && g_mcLibrary.size() == 12,
          "libraries %s %zu / %s %zu", g_libraryName.c_str(), g_library.size(), g_mcLibraryName.c_str(), g_mcLibrary.size());
    CHECK(g_library[0].userData == nullptr && g_mcLibrary[0].userData != nullptr, "microcontroller copy is tagged");
    CHECK(g_storage.count("http.cfg") && g_storage["http.cfg"] == httpc::DefaultConfigText(), "default config written");
    CHECK(g_eventCb && g_taskCb, "subscribed + scheduled");
    CHECK(Logged("http 0.1.0 ready"), "ready logged");
    SwModUnload();

    CHECK(Init("max_per_vm=oops\n"), "init with a bad config");
    CHECK(Logged("http.cfg ignored"), "bad config logged");
    SwModUnload();
}

static void TestRequests() {
    CHECK(Init(nullptr), "init");
    auto vm = NewVm();
    auto r = Call(*vm, "version", {});
    CHECK(r.size() == 1 && r[0].s == "0.1.0", "version");
    r = Call(*vm, "get", {Num(42)});
    CHECK(r.size() == 2 && r[0].type == SW_LUA_TYPE_NIL && r[1].s.find("url") != std::string::npos, "get(42)");
    r = Call(*vm, "get", {Str("ftp://x/")});
    CHECK(r.size() == 2 && r[0].type == SW_LUA_TYPE_NIL && r[1].s.find("http://") != std::string::npos, "get(ftp)");
    r = Call(*vm, "get", {Str(Url()), Str("bogus=1")});
    CHECK(r.size() == 2 && r[1].s.find("unknown option") != std::string::npos, "bad options");
    r = Call(*vm, "request", {Str("BREW"), Str(Url())});
    CHECK(r.size() == 2 && r[1].s.find("method") != std::string::npos, "bad method");
    r = Call(*vm, "post", {Str(Url()), Num(5)});
    CHECK(r.size() == 2 && r[1].s.find("body") != std::string::npos, "post(url, 5)");
    CHECK(Call(*vm, "poll", {}).empty(), "poll nothing");
    CHECK(Call(*vm, "poll", {Num(-1)}).empty(), "poll bad id");
    r = Call(*vm, "state", {Num(999)});
    CHECK(r.size() == 1 && r[0].s == "unknown", "state unknown");
    r = Call(*vm, "pending", {});
    CHECK(r.size() == 1 && r[0].n == 0, "pending 0");

    r = Call(*vm, "get", {Str(Url("/echo?q=1")), Str("header=X-Test: lua")});
    CHECK(r.size() == 1 && r[0].type == SW_LUA_TYPE_INTEGER && r[0].n >= 1, "get returns an integer id");
    const double id = r.empty() ? 0 : r[0].n;
    CHECK(Logged("GET http://127.0.0.1:"), "request logged");
    CHECK(Call(*vm, "pending", {})[0].n == 1, "pending 1");
    r = Wait(*vm);
    CHECK(r.size() == 5 && r[0].n == id && r[1].s == "response" && r[2].n == 200 && r[2].type == SW_LUA_TYPE_INTEGER,
          "response (%zu values)", r.size());
    if (r.size() == 5) {
        CHECK(r[3].s.find("\"method\": \"GET\"") != std::string::npos, "body");
        const Value* h = Field(r[4], "x-test-echo");
        CHECK(h && h->s == "lua", "headers table");
    }
    CHECK(Call(*vm, "state", {Num(id)})[0].s == "unknown", "released after poll");

    r = Call(*vm, "post", {Str(Url()), Str("{\"n\":1}"), Str("headers=0")});
    const double postId = r.size() == 1 ? r[0].n : 0;
    r = Wait(*vm, postId);
    CHECK(r.size() == 4 && r[1].s == "response" && r[3].s.find("application/json") != std::string::npos,
          "post without headers table (%zu values)", r.size());

    r = Call(*vm, "request", {Str("delete"), Str(Url()), Nil(), Str("timeout=5")});
    r = Wait(*vm, r.size() == 1 ? r[0].n : 0);
    CHECK(r.size() == 5 && r[4].t && Field(r[4], "x-echo-method")->s == "DELETE", "request(delete)");

    r = Call(*vm, "get", {Str(Url("/status/404"))});
    r = Wait(*vm);
    CHECK(r.size() >= 3 && r[1].s == "response" && r[2].n == 404, "404 is a response");

    r = Call(*vm, "get", {Str("http://127.0.0.1:1/"), Str("proxy=direct")});
    r = Wait(*vm);
    CHECK(r.size() == 4 && r[1].s == "error" && !r[2].s.empty() && r[3].n != 0, "network error");

    r = Call(*vm, "get", {Str(Url("/slow?ms=3000"))});
    const double slow = r.size() == 1 ? r[0].n : 0;
    Sleep(100);
    CHECK(Call(*vm, "state", {Num(slow)})[0].s == "running", "running");
    CHECK(Call(*vm, "cancel", {Num(slow)})[0].b, "cancel");
    r = Wait(*vm, slow);
    CHECK(r.size() == 4 && r[2].s == "cancelled", "cancelled");
    CHECK(!Call(*vm, "cancel", {Num(slow)})[0].b, "cancel twice");

    r = Call(*vm, "urlencode", {Str("a b&é")});
    CHECK(r.size() == 1 && r[0].s == "a%20b%26%C3%A9", "urlencode");
    SwModUnload();
}

static void TestOwners() {
    CHECK(Init(nullptr), "init");
    auto a = NewVm(), b = NewVm();
    auto r = Call(*a, "get", {Str(Url())});
    const double id = r.size() == 1 ? r[0].n : 0;
    Sleep(300);
    CHECK(Call(*b, "poll", {}).empty() && Call(*b, "poll", {Num(id)}).empty(), "other VM sees nothing");
    CHECK(Call(*b, "state", {Num(id)})[0].s == "unknown" && !Call(*b, "cancel", {Num(id)})[0].b, "other VM cannot touch it");
    const double token = Field(a->globals[SKey("http")], "__owner")->n;
    r = Wait(*a, id);
    CHECK(r.size() == 5, "owner gets it");
    CHECK(Field(a->globals[SKey("http")], "__owner")->n == token, "owner token stable");
    SwModUnload();
}

static void TestMicrocontroller() {
    CHECK(Init(nullptr), "init");
    auto mc = NewVm(true);
    auto r = Call(*mc, "get", {Str(Url())});
    CHECK(r.size() == 2 && r[1].s.find("local_network") != std::string::npos, "local address refused for microcontrollers");
    r = Call(*mc, "get", {Str("http://localhost:1/")});
    CHECK(r.size() == 2, "localhost refused for microcontrollers");
    CHECK(Logged("(local_network)"), "refusal logged");
    auto addon = NewVm();
    r = Call(*addon, "get", {Str(Url())});
    CHECK(r.size() == 1, "addon may reach localhost");
    SwModUnload();

    CHECK(Init("local_network=all\n"), "init local_network=all");
    mc = NewVm(true);
    r = Call(*mc, "get", {Str(Url())});
    CHECK(r.size() == 1, "allowed with local_network=all");
    CHECK(Logged("(microcontroller)"), "microcontroller request logged");
    r = Wait(*mc);
    CHECK(r.size() == 5 && r[2].n == 200, "microcontroller response");
    SwModUnload();

    CHECK(Init("local_network=none\n"), "init local_network=none");
    addon = NewVm();
    CHECK(Call(*addon, "get", {Str(Url())}).size() == 2, "none: addon refused too");
    SwModUnload();
}

static void TestLimits() {
    CHECK(Init("max_per_vm=2\nrate_per_minute=3\n"), "init");
    auto vm = NewVm();
    CHECK(Call(*vm, "get", {Str(Url("/slow?ms=300"))}).size() == 1, "1");
    CHECK(Call(*vm, "get", {Str(Url("/slow?ms=300"))}).size() == 1, "2");
    auto r = Call(*vm, "get", {Str(Url())});
    CHECK(r.size() == 2 && r[1].s.find("too many pending") != std::string::npos, "max_per_vm");
    Wait(*vm);
    Wait(*vm);
    CHECK(Call(*vm, "get", {Str(Url())}).size() == 1, "3");
    Wait(*vm);
    r = Call(*vm, "get", {Str(Url())});
    CHECK(r.size() == 2 && r[1].s.find("rate limit") != std::string::npos, "rate_per_minute");
    auto other = NewVm();
    CHECK(Call(*other, "get", {Str(Url())}).size() == 1, "rate is per VM");
    SwModUnload();

    CHECK(Init("allow_hosts=example.com\n"), "init allow_hosts");
    vm = NewVm();
    r = Call(*vm, "get", {Str(Url())});
    CHECK(r.size() == 2 && r[1].s.find("not allowed") != std::string::npos, "allow_hosts");
    SwModUnload();

    CHECK(Init("max_body_bytes=10\nmax_response_bytes=100\n"), "init sizes");
    vm = NewVm();
    r = Call(*vm, "post", {Str(Url()), Str(std::string(11, 'x'))});
    CHECK(r.size() == 2 && r[1].s.find("body") != std::string::npos, "max_body_bytes");
    r = Call(*vm, "get", {Str(Url("/bytes/101"))});
    r = Wait(*vm);
    CHECK(r.size() == 4 && r[2].s.find("too large") != std::string::npos, "max_response_bytes");
    SwModUnload();
}

static void TestJsonDecode() {
    CHECK(Init(nullptr), "init");
    auto vm = NewVm();
    auto r = Call(*vm, "json_decode", {Str("{\"name\":\"Ana\",\"age\":31,\"pi\":3.5,\"ok\":true,\"none\":null,"
                                           "\"list\":[10,\"b\",null,{\"x\":1}],\"\":\"empty key\"}")});
    CHECK(r.size() == 1 && r[0].type == SW_LUA_TYPE_TABLE, "decoded a table");
    if (r.size() == 1 && r[0].t) {
        const Value& t = r[0];
        CHECK(Field(t, "name")->s == "Ana" && Field(t, "age")->type == SW_LUA_TYPE_INTEGER && Field(t, "age")->n == 31 &&
                  Field(t, "pi")->type == SW_LUA_TYPE_NUMBER && Field(t, "pi")->n == 3.5 && Field(t, "ok")->b, "scalars");
        CHECK(!Field(t, "none"), "null is nil");
        const Value* list = Field(t, "list");
        CHECK(list && Index(*list, 1)->n == 10 && Index(*list, 2)->s == "b" && !Index(*list, 3) &&
                  Field(*Index(*list, 4), "x")->n == 1, "array");
        auto empty = t.t->find(SKey(""));
        CHECK(empty != t.t->end() && empty->second.s == "empty key", "empty key through rawset");
    }
    r = Call(*vm, "json_decode", {Str("[1,2")});
    CHECK(r.size() == 2 && r[0].type == SW_LUA_TYPE_NIL && r[1].s.find("json_decode") != std::string::npos, "invalid: %s",
          r.size() > 1 ? r[1].s.c_str() : "");
    r = Call(*vm, "json_decode", {Str("\"just a string\"")});
    CHECK(r.size() == 1 && r[0].s == "just a string", "scalar document");
    r = Call(*vm, "json_decode", {Str("null")});
    CHECK(r.size() == 1 && r[0].type == SW_LUA_TYPE_NIL, "null document");
    std::string big = "[";
    for (int i = 0; i < 5000; ++i) big += (i ? ",[" : "[") + std::to_string(i) + "]";
    big += "]";
    g_maxStack = 0;
    r = Call(*vm, "json_decode", {Str(big)});
    CHECK(r.size() == 1 && Index(*Index(r[0], 5000), 1)->n == 4999, "5000 nested arrays");
    CHECK(g_maxStack < 400, "decode stack stays bounded (%zu)", g_maxStack);

    // As in the game, a microcontroller VM has no rawset: arrays go through table.pack/move.
    auto mc = NewVm(true);
    std::string list = "{\"\":1,\"v\":[";
    for (int i = 1; i <= 450; ++i) list += (i > 1 ? "," : "") + (i == 3 ? std::string("null") : std::to_string(i));
    list += "]}";
    mc->moves = 0;
    r = Call(*mc, "json_decode", {Str(list)});
    CHECK(r.size() == 1 && r[0].t, "decoded without rawset: %s", r.size() > 1 ? r[1].s.c_str() : "");
    if (r.size() == 1 && r[0].t) {
        const Value* v = Field(r[0], "v");
        CHECK(v && Index(*v, 1)->n == 1 && !Index(*v, 3) && Index(*v, 4)->n == 4 && Index(*v, 450)->n == 450 &&
                  Index(*v, 201)->n == 201 && !Field(*v, "n"), "450 values, hole kept, no 'n' field");
        CHECK(r[0].t->find(SKey("")) == r[0].t->end(), "empty key skipped without rawset");
        CHECK(mc->moves == 3, "3 chunks of 200 (%d)", mc->moves);
    }
    // No table library either: rawset fallback (addon VM).
    auto bare = NewVm();
    bare->globals.erase(SKey("table"));
    r = Call(*bare, "json_decode", {Str("[1,null,[2,3]]")});
    CHECK(r.size() == 1 && Index(r[0], 1)->n == 1 && !Index(r[0], 2) && Index(*Index(r[0], 3), 2)->n == 3, "rawset fallback");
    // Neither: a clear error.
    mc->globals.erase(SKey("table"));
    r = Call(*mc, "json_decode", {Str("[1]")});
    CHECK(r.size() == 2 && r[1].s.find("rawset") != std::string::npos, "no setter: %s", r.size() > 1 ? r[1].s.c_str() : "");

    // Older build (call unsupported) or older loader (no pushValue): objects only.
    vm->callSupported = false;
    r = Call(*vm, "json_decode", {Str("{\"a\":{\"b\":\"c\"}}")});
    CHECK(r.size() == 1 && Field(*Field(r[0], "a"), "b")->s == "c", "objects without call");
    r = Call(*vm, "json_decode", {Str("{\"a\":[1]}")});
    CHECK(r.size() == 2 && r[1].s.find("1.15.23") != std::string::npos, "arrays without call: %s", r.size() > 1 ? r[1].s.c_str() : "");
    r = Call(*vm, "field", {Str("{\"a\":[{\"b\":7}]}"), Str("a.1.b")});
    CHECK(r.size() == 1 && r[0].n == 7, "field works without call");
    SwModUnload();
}

static void TestJsonEncode() {
    CHECK(Init(nullptr), "init");
    auto vm = NewVm();
    auto encode = [&](const Value& v) {
        auto r = Call(*vm, "json_encode", {v});
        return r.size() == 1 ? r[0].s : (r.size() == 2 ? "ERR " + r[1].s : std::string("?"));
    };
    CHECK(encode(Nil()) == "null" && encode(Bool(true)) == "true" && encode(Int(42)) == "42" && encode(Num(2.5)) == "2.5" &&
              encode(Num(3)) == "3" && encode(Str("a\"\n")) == "\"a\\\"\\n\"", "scalars");
    Value arr = NewTable();
    (*arr.t)[IKey(1)] = Int(1);
    (*arr.t)[IKey(2)] = Str("two");
    (*arr.t)[IKey(3)] = Bool(false);
    CHECK(encode(arr) == "[1,\"two\",false]", "array: %s", encode(arr).c_str());
    CHECK(encode(NewTable()) == "[]", "empty table");
    Value obj = NewTable();
    (*obj.t)[SKey("content")] = Str("hello");
    (*obj.t)[SKey("list")] = arr;
    Value inner = NewTable();
    (*inner.t)[SKey("x")] = Num(0.5);
    (*obj.t)[SKey("inner")] = inner;
    CHECK(encode(obj) == "{\"content\":\"hello\",\"inner\":{\"x\":0.5},\"list\":[1,\"two\",false]}", "object: %s",
          encode(obj).c_str());
    Value sparse = NewTable();
    (*sparse.t)[IKey(1)] = Int(1);
    (*sparse.t)[IKey(3)] = Int(3);
    CHECK(encode(sparse) == "{\"1\":1,\"3\":3}", "sparse array -> object: %s", encode(sparse).c_str());
    Value cycle = NewTable();
    (*cycle.t)[SKey("self")] = cycle;
    CHECK(encode(cycle).find("ERR") == 0, "cycle refused");
    cycle.t->clear(); // break the shared_ptr cycle
    Value fn = Builtin("next");
    CHECK(encode(fn).find("ERR") == 0, "function refused");
    Value nan = NewTable();
    (*nan.t)[SKey("n")] = Num(std::numeric_limits<double>::quiet_NaN());
    CHECK(encode(nan).find("ERR") == 0, "NaN refused");

    // Round trip through decode.
    auto r = Call(*vm, "json_decode", {Str(encode(obj))});
    CHECK(r.size() == 1 && Field(r[0], "content")->s == "hello" && Index(*Field(r[0], "list"), 2)->s == "two", "round trip");

    Value big = NewTable();
    for (int i = 1; i <= 3000; ++i) (*big.t)[IKey(i)] = Int(i);
    const std::string text = encode(big);
    CHECK(text.size() > 10000 && text.compare(text.size() - 6, 6, ",3000]") == 0, "3000 elements");

    vm->callSupported = false;
    CHECK(encode(obj).find("ERR") == 0 && encode(Int(1)) == "1", "tables need call; scalars do not");
    SwModUnload();

    // Older loader: no pushValue.
    const uint32_t saved = g_extras.structSize;
    g_extras.structSize = offsetof(SwLuaExtrasApi, pushValue);
    CHECK(Init(nullptr), "init without pushValue");
    vm = NewVm();
    CHECK(encode(obj).find("pushValue") != std::string::npos, "encode explains pushValue: %s", encode(obj).c_str());
    SwModUnload();
    g_extras.structSize = saved;
}

static void TestField() {
    CHECK(Init(nullptr), "init");
    auto vm = NewVm();
    const Value doc = Str("{\"current\":{\"temp\":12.5,\"code\":3,\"name\":\"Paris\",\"rain\":false},\"days\":[{\"t\":1},{\"t\":2}]}");
    auto r = Call(*vm, "field", {doc, Str("current.temp")});
    CHECK(r.size() == 1 && r[0].n == 12.5, "number");
    r = Call(*vm, "field", {doc, Str("current.name")});
    CHECK(r.size() == 1 && r[0].s == "Paris", "string");
    r = Call(*vm, "field", {doc, Str("current.rain")});
    CHECK(r.size() == 1 && r[0].type == SW_LUA_TYPE_BOOLEAN && !r[0].b, "boolean");
    r = Call(*vm, "field", {doc, Str("days.2.t")});
    CHECK(r.size() == 1 && r[0].n == 2, "array index");
    r = Call(*vm, "field", {doc, Str("days")});
    CHECK(r.size() == 1 && r[0].s == "[{\"t\":1},{\"t\":2}]", "sub-tree as JSON text: %s", r.empty() ? "" : r[0].s.c_str());
    CHECK(Call(*vm, "field", {doc, Str("current.nope")}).empty(), "missing -> nothing");
    r = Call(*vm, "field", {Str("{oops"), Str("a")});
    CHECK(r.size() == 2 && r[0].type == SW_LUA_TYPE_NIL, "invalid json");
    SwModUnload();
}

static void TestIdleAndWorldEnd() {
    CHECK(Init("idle_timeout=1\n"), "init");
    auto vm = NewVm();
    auto r = Call(*vm, "get", {Str(Url("/slow?ms=200"))});
    const double id = r.size() == 1 ? r[0].n : 0;
    Sleep(1600);
    g_taskCb(nullptr); // the loader's 1 s sweep task
    CHECK(Call(*vm, "state", {Num(id)})[0].s == "unknown", "idle VM's request released");
    CHECK(Logged("idle script"), "idle release logged");

    SwEvent created = {sizeof(SwEvent), SW_EVENT_ADDON_LUA_CREATED};
    SwEvent destroyed = {sizeof(SwEvent), SW_EVENT_ADDON_LUA_DESTROYED};
    g_eventCb(&created, nullptr);
    g_eventCb(&created, nullptr);
    r = Call(*vm, "get", {Str(Url("/slow?ms=2000"))});
    const double id2 = r.size() == 1 ? r[0].n : 0;
    g_eventCb(&destroyed, nullptr);
    CHECK(Call(*vm, "state", {Num(id2)})[0].s != "unknown", "one VM left: kept");
    g_eventCb(&destroyed, nullptr);
    CHECK(Call(*vm, "state", {Num(id2)})[0].s == "unknown", "no VM left: dropped");
    CHECK(Logged("last addon VM destroyed"), "world end logged");
    g_eventCb(&destroyed, nullptr); // unbalanced (mod loaded mid-game): ignored
    SwModUnload();
}

static void TestUnloadWithTraffic() {
    CHECK(Init(nullptr), "init");
    auto a = NewVm(), b = NewVm();
    for (int i = 0; i < 8; ++i) Call(*a, "get", {Str(Url("/slow?ms=10000"))});
    for (int i = 0; i < 4; ++i) Call(*b, "get", {Str(Url("/drip?count=1000&ms=50"))});
    Call(*b, "get", {Str("http://10.255.255.1/"), Str("proxy=direct")});
    Sleep(300);
    const auto start = std::chrono::steady_clock::now();
    SwModUnload();
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    CHECK(ms < 3000, "unload took %.0f ms", ms);
    CHECK(httpc::Client::LiveThreads() == 0, "threads %d", httpc::Client::LiveThreads());
    for (int cycle = 0; cycle < 5; ++cycle) { // repeated load/unload
        g_quiet = true;
        CHECK(Init(nullptr), "init %d", cycle);
        auto vm = NewVm();
        Call(*vm, "get", {Str(Url("/slow?ms=5000"))});
        SwModUnload();
        g_quiet = false;
    }
    CHECK(httpc::Client::LiveThreads() == 0, "threads after cycles %d", httpc::Client::LiveThreads());
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i)
        if (!strcmp(argv[i], "--port") && i + 1 < argc) g_port = atoi(argv[++i]);
    Run("helpers + json core", TestHelpers);
    Run("init checks", TestInitChecks);
    Run("requests", TestRequests);
    Run("owner isolation", TestOwners);
    Run("microcontroller library", TestMicrocontroller);
    Run("limits", TestLimits);
    Run("json_decode", TestJsonDecode);
    Run("json_encode", TestJsonEncode);
    Run("field", TestField);
    Run("idle sweep and world end", TestIdleAndWorldEnd);
    Run("unload with traffic", TestUnloadWithTraffic);
    printf("\nlive threads at exit: %d\n%d passed, %d failed\n", httpc::Client::LiveThreads(), g_passed, g_failed);
    return g_failed || httpc::Client::LiveThreads() ? 1 : 0;
}
