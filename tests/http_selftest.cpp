// Offline self-test of http_client (no game). Needs tests/test_server.py running.
//   http_selftest.exe [--port 8780] [--tls-port 8781] [--online]
#include "../src/http_client.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace httpc;
using Clock = std::chrono::steady_clock;

static int g_port = 8780, g_tlsPort = 8781;
static int g_failed = 0, g_passed = 0;

#define CHECK(cond, what)                                                        \
    do {                                                                         \
        if (!(cond)) {                                                           \
            printf("    FAIL %s (line %d)\n", what, __LINE__);                   \
            return false;                                                        \
        }                                                                        \
    } while (0)

static std::string Http(const char* path) { return "http://127.0.0.1:" + std::to_string(g_port) + path; }
static std::string Https(const char* path) { return "https://localhost:" + std::to_string(g_tlsPort) + path; }

static double Seconds(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

static std::string Header(const Result& r, const char* name) {
    for (const auto& h : r.headers)
        if (h.first == name) return h.second;
    return std::string();
}

// Submits and waits for the result (up to 30 s).
static bool Run1(Client& c, const std::string& method, const std::string& url, const std::string& body,
                 const std::string& options, Result* out, std::string* submitError = nullptr) {
    Request req;
    req.method = method;
    req.url = url;
    req.body = body;
    std::string error;
    if (!ParseOptions(options, &req.options, &error)) {
        printf("    options: %s\n", error.c_str());
        return false;
    }
    const uint64_t id = c.Submit(req, 1, &error);
    if (!id) {
        if (submitError) *submitError = error;
        return false;
    }
    const auto start = Clock::now();
    while (!c.Take(id, out)) {
        if (Seconds(start) > 30) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

static std::unique_ptr<Client> NewClient(int threads = 4) {
    Client::Config cfg;
    cfg.threads = threads;
    cfg.log = [](const std::string& s) { printf("    log: %s\n", s.c_str()); };
    return Client::Create(cfg);
}

static bool TestGet() {
    auto c = NewClient();
    Result r;
    CHECK(Run1(*c, "GET", Http("/echo?x=1"), "", "header=X-Test: hello;proxy=direct", &r), "request");
    CHECK(r.ok && r.status == 200, "status 200");
    CHECK(r.body.find("\"method\": \"GET\"") != std::string::npos, "method in body");
    CHECK(r.body.find("/echo?x=1") != std::string::npos, "query string sent");
    CHECK(Header(r, "x-test-echo") == "hello", "custom header sent and echoed");
    CHECK(Header(r, "content-type") == "application/json", "response header lower-cased");
    return true;
}

static bool TestMethods() {
    auto c = NewClient();
    Result r;
    CHECK(Run1(*c, "post", Http("/echo"), "{\"a\":1}", "", &r), "post");
    CHECK(r.ok && r.body.find("\"body\": \"{\\\"a\\\":1}\"") != std::string::npos, "post body echoed");
    CHECK(r.body.find("\"content-type\": \"application/json\"") != std::string::npos, "json content type guessed");
    CHECK(Run1(*c, "POST", Http("/echo"), "plain", "", &r), "post text");
    CHECK(r.body.find("text/plain; charset=utf-8") != std::string::npos, "text content type guessed");
    CHECK(Run1(*c, "POST", Http("/echo"), "a=1", "content_type=application/x-www-form-urlencoded", &r), "post form");
    CHECK(r.body.find("application/x-www-form-urlencoded") != std::string::npos, "explicit content type");
    CHECK(Run1(*c, "POST", Http("/echo"), "x", "header=Content-Type: text/csv", &r), "post header ct");
    CHECK(r.body.find("text/csv") != std::string::npos && r.body.find("text/plain") == std::string::npos,
          "content-type header wins");
    for (const char* m : {"PUT", "PATCH", "DELETE"}) {
        CHECK(Run1(*c, m, Http("/echo"), "data", "", &r), m);
        CHECK(r.ok && Header(r, "x-echo-method") == m, "method reached the server");
    }
    CHECK(Run1(*c, "HEAD", Http("/bytes/1000"), "", "", &r), "head");
    CHECK(r.ok && r.status == 200 && r.body.empty() && Header(r, "content-length") == "1000", "head: no body");
    return true;
}

static bool TestStatus() {
    auto c = NewClient();
    Result r;
    CHECK(Run1(*c, "GET", Http("/status/404"), "", "", &r), "404");
    CHECK(r.ok && r.status == 404, "404 is a response, not an error");
    CHECK(Run1(*c, "GET", Http("/status/500"), "", "", &r), "500");
    CHECK(r.ok && r.status == 500, "500 is a response");
    return true;
}

static bool TestRedirects() {
    auto c = NewClient();
    Result r;
    const std::string to = "/redirect?to=" + Http("/echo");
    CHECK(Run1(*c, "GET", Http(to.c_str()), "", "", &r), "redirect");
    CHECK(r.ok && r.status == 200 && Header(r, "x-echo-method") == "GET", "redirect followed");
    CHECK(Run1(*c, "GET", Http(to.c_str()), "", "redirects=0", &r), "redirect off");
    CHECK(r.ok && r.status == 302 && !Header(r, "location").empty(), "redirect not followed");
    CHECK(Run1(*c, "GET", Http("/loop"), "", "", &r), "loop");
    CHECK(!r.ok && !r.error.empty(), "redirect loop is an error");
    printf("    loop: %s\n", r.error.c_str());
    const std::string down = "/redirect?to=" + Http("/echo");
    CHECK(Run1(*c, "GET", Https(down.c_str()), "", "insecure=1", &r), "https->http");
    CHECK(!(r.ok && r.status == 200), "https -> http redirect not followed");
    printf("    https->http: ok=%d status=%d %s\n", r.ok, r.status, r.error.c_str());
    return true;
}

static bool TestTls() {
    auto c = NewClient();
    Result r;
    CHECK(Run1(*c, "GET", Https("/echo"), "", "", &r), "self-signed");
    CHECK(!r.ok && r.code != 0, "self-signed refused");
    printf("    refused: %s\n", r.error.c_str());
    CHECK(Run1(*c, "GET", Https("/echo"), "", "insecure=1", &r), "insecure");
    CHECK(r.ok && r.status == 200, "insecure accepted");
    return true;
}

static bool TestEncodings() {
    auto c = NewClient();
    Result r;
    CHECK(Run1(*c, "GET", Http("/chunked"), "", "", &r), "chunked");
    CHECK(r.ok && r.body == "part0 part1 part2 part3 part4 part5 part6 part7 part8 part9 ", "chunked assembled");
    CHECK(Run1(*c, "GET", Http("/gzip"), "", "", &r), "gzip");
    std::string expected;
    for (int i = 0; i < 200; ++i) expected += "hello gzip ";
    CHECK(r.ok && r.body == expected, "gzip decoded");
    CHECK(Run1(*c, "GET", Http("/multi"), "", "", &r), "multi");
    CHECK(Header(r, "x-multi") == "one, two" && Header(r, "set-cookie") == "a=1, b=2", "duplicate headers joined");
    CHECK(Run1(*c, "GET", Http("/multi"), "", "headers=0", &r), "no headers");
    CHECK(r.ok && r.headers.empty(), "headers=0");
    return true;
}

static bool TestLimits() {
    auto c = NewClient();
    Result r;
    CHECK(Run1(*c, "GET", Http("/bytes/1048576"), "", "", &r), "1 MiB");
    CHECK(r.ok && r.body.size() == 1048576 && r.body[26] == 'a', "1 MiB accepted");
    CHECK(Run1(*c, "GET", Http("/bytes/1048577"), "", "", &r), "1 MiB + 1");
    CHECK(!r.ok && r.error.find("too large") != std::string::npos, "1 MiB + 1 refused");
    CHECK(Run1(*c, "GET", Http("/bytes/2000"), "", "max_bytes=1000", &r), "max_bytes");
    CHECK(!r.ok && r.error.find("too large") != std::string::npos, "max_bytes applied");
    CHECK(Run1(*c, "GET", Http("/bytes/1000"), "", "max_bytes=99999999", &r), "max_bytes clamp");
    CHECK(r.ok, "max_bytes above the client limit is clamped");
    std::string error;
    CHECK(!Run1(*c, "POST", Http("/echo"), std::string(1048577, 'x'), "", &r, &error), "big body");
    CHECK(error.find("body exceeds") != std::string::npos, "body limit at submit");
    CHECK(Run1(*c, "POST", Http("/bytes/10"), std::string(1048576, 'x'), "", &r), "1 MiB body");
    CHECK(r.ok && r.status == 200, "1 MiB body sent");
    return true;
}

static bool TestTimeouts() {
    auto c = NewClient();
    Result r;
    auto start = Clock::now();
    CHECK(Run1(*c, "GET", Http("/slow?ms=5000"), "", "timeout=1", &r), "slow");
    double t = Seconds(start);
    CHECK(!r.ok && r.error.find("timeout") != std::string::npos, "slow server times out");
    CHECK(t < 2.5, "timeout near 1 s");
    printf("    slow: %.2f s, %s\n", t, r.error.c_str());
    start = Clock::now();
    CHECK(Run1(*c, "GET", Http("/drip?count=100&ms=300"), "", "timeout=2", &r), "drip");
    t = Seconds(start);
    CHECK(!r.ok && r.error.find("timeout") != std::string::npos, "dripping server times out");
    CHECK(t < 3.5, "overall deadline near 2 s");
    printf("    drip: %.2f s\n", t);
    start = Clock::now();
    CHECK(Run1(*c, "GET", "http://10.255.255.1/", "", "timeout=1;proxy=direct", &r), "black hole");
    t = Seconds(start);
    CHECK(!r.ok && t < 2.5, "unreachable host times out");
    printf("    black hole: %.2f s, %s\n", t, r.error.c_str());
    return true;
}

static bool TestCancel() {
    auto c = NewClient(1);
    Request req;
    req.url = Http("/slow?ms=5000");
    std::string error;
    const uint64_t running = c->Submit(req, 7, &error);
    const uint64_t queued = c->Submit(req, 7, &error);
    CHECK(running && queued, "submit");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(c->State(running) == JobState::Running, "first running");
    CHECK(c->State(queued) == JobState::Queued, "second queued (1 thread)");
    CHECK(c->Cancel(queued), "cancel queued");
    CHECK(c->State(queued) == JobState::Done, "queued cancel is immediate");
    const auto start = Clock::now();
    CHECK(c->Cancel(running), "cancel running");
    Result r;
    while (!c->Take(running, &r) && Seconds(start) < 5) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(Seconds(start) < 1, "running cancel is quick");
    CHECK(!r.ok && r.error == "cancelled", "running cancelled");
    uint64_t id = 0;
    CHECK(c->TakeAny(7, &id, &r) && id == queued && r.error == "cancelled", "queued cancelled");
    CHECK(!c->Cancel(queued), "cancel unknown");
    CHECK(c->Count() == 0, "nothing left");
    return true;
}

static bool TestNetworkErrors() {
    auto c = NewClient();
    Result r;
    CHECK(Run1(*c, "GET", "http://127.0.0.1:1/", "", "proxy=direct", &r), "refused");
    CHECK(!r.ok && r.code != 0, "connection refused is an error");
    printf("    refused: %s\n", r.error.c_str());
    CHECK(Run1(*c, "GET", "http://does-not-exist.invalid/", "", "", &r), "dns");
    CHECK(!r.ok && r.code != 0, "dns failure is an error");
    printf("    dns: %s\n", r.error.c_str());
    CHECK(Run1(*c, "GET", Http("/truncate"), "", "", &r), "truncate");
    CHECK(!r.ok, "truncated body is an error");
    printf("    truncated: %s\n", r.error.c_str());
    return true;
}

static bool TestValidation() {
    auto c = NewClient();
    Request req;
    std::string error;
    req.url = "ftp://x/";
    CHECK(!c->Submit(req, 1, &error) && error.find("http://") != std::string::npos, "scheme refused");
    req.url = Http("/echo");
    req.method = "CONNECT";
    CHECK(!c->Submit(req, 1, &error), "method refused");
    req.method = "GET";
    req.options.headers = {"X-A: 1\r\nX-B: 2"};
    CHECK(!c->Submit(req, 1, &error), "header injection refused");
    req.options.headers.clear();
    req.url = "http://127.0.0.1/\r\nx";
    CHECK(!c->Submit(req, 1, &error), "control chars in url refused");
    req.url = Http("/echo");
    req.method = "HEAD";
    req.body = "x";
    CHECK(!c->Submit(req, 1, &error), "HEAD with body refused");

    Options o;
    CHECK(ParseOptions("timeout=5; header=A: b ;header=C: d;insecure=1;redirects=0;proxy=direct;max_bytes=10;headers=0", &o, &error), "parse ok");
    CHECK(o.timeoutMs == 5000 && o.headers.size() == 2 && o.headers[0] == "A: b" && o.insecure && !o.followRedirects &&
          o.proxy == Proxy::Direct && o.maxResponseBytes == 10 && !o.wantHeaders, "parsed values");
    Options p;
    CHECK(!ParseOptions("timeout=0", &p, &error), "timeout range");
    CHECK(!ParseOptions("bogus=1", &p, &error), "unknown key");
    CHECK(!ParseOptions("insecure", &p, &error), "missing =");
    CHECK(ParseOptions("", &p, &error) && ParseOptions(";;", &p, &error), "empty options");
    return true;
}

static bool TestParallel() {
    auto c = NewClient(4);
    Request req;
    req.url = Http("/slow?ms=500");
    std::string error;
    std::vector<uint64_t> ids;
    const auto start = Clock::now();
    for (int i = 0; i < 16; ++i) ids.push_back(c->Submit(req, 3, &error));
    CHECK(c->Count(3) == 16, "16 queued");
    size_t done = 0;
    Result r;
    uint64_t id;
    while (done < ids.size() && Seconds(start) < 10) {
        if (c->TakeAny(3, &id, &r)) {
            CHECK(r.ok && r.body == "slow", "parallel result");
            ++done;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    const double t = Seconds(start);
    CHECK(done == 16, "all done");
    CHECK(t > 1.8 && t < 4.0, "4 at a time (about 2 s)");
    printf("    16 x 0.5 s with 4 threads: %.2f s\n", t);
    return true;
}

static bool TestDestroyBusy() {
    const auto start = Clock::now();
    {
        auto c = NewClient(4);
        Request req;
        req.url = Http("/slow?ms=10000");
        std::string error;
        for (int i = 0; i < 10; ++i) CHECK(c->Submit(req, 1, &error), "submit");
        req.url = Http("/drip?count=1000&ms=100");
        CHECK(c->Submit(req, 2, &error), "submit drip");
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        c->DiscardTag(2);
        CHECK(c->Count(2) == 0, "discarded");
    }
    CHECK(Seconds(start) < 2, "destroy with running requests is quick");
    CHECK(Client::LiveThreads() == 0, "no thread left");
    return true;
}

static bool TestOnline() {
    auto c = NewClient();
    Result r;
    CHECK(Run1(*c, "GET", "https://httpbin.org/get?stormfabric=1", "", "timeout=30", &r), "get");
    printf("    httpbin get: ok=%d status=%d %s\n", r.ok, r.status, r.ok ? "" : r.error.c_str());
    CHECK(r.ok && r.status == 200 && r.body.find("stormfabric") != std::string::npos, "httpbin get");
    CHECK(Run1(*c, "POST", "https://httpbin.org/post", "{\"from\":\"stormfabric\"}", "timeout=30", &r), "post");
    CHECK(r.ok && r.status == 200 && r.body.find("\"from\": \"stormfabric\"") != std::string::npos, "httpbin post json");
    CHECK(Run1(*c, "GET", "https://example.com/", "", "timeout=30", &r), "example.com");
    CHECK(r.ok && r.status == 200 && r.body.find("Example Domain") != std::string::npos, "example.com");
    return true;
}

static void Run(const char* name, bool (*fn)()) {
    printf("%s\n", name);
    fflush(stdout);
    if (fn()) {
        ++g_passed;
        printf("    ok\n");
    } else {
        ++g_failed;
    }
    if (Client::LiveThreads() != 0) {
        printf("    FAIL %d thread(s) left\n", Client::LiveThreads());
        ++g_failed;
    }
}

int main(int argc, char** argv) {
    bool online = false;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) g_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--tls-port") && i + 1 < argc) g_tlsPort = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--online")) online = true;
    }
    Run("get + headers", TestGet);
    Run("methods + bodies", TestMethods);
    Run("http status", TestStatus);
    Run("redirects", TestRedirects);
    Run("tls", TestTls);
    Run("chunked/gzip/headers", TestEncodings);
    Run("size limits", TestLimits);
    Run("timeouts", TestTimeouts);
    Run("cancel", TestCancel);
    Run("network errors", TestNetworkErrors);
    Run("validation + options", TestValidation);
    Run("parallel pool", TestParallel);
    Run("destroy while busy", TestDestroyBusy);
    if (online) Run("online (httpbin.org, example.com)", TestOnline);
    printf("\n%d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
