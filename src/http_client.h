// HTTP/HTTPS client core on top of WinHTTP (Windows 8.1+).
// Independent of the StormFabric SDK so it can be tested outside the game.
//
// Thread model: every public method is thread-safe and never blocks on the network.
// A fixed pool of worker threads runs the requests with synchronous WinHTTP calls; one
// monitor thread enforces each request's overall deadline. A request is cancelled by
// closing its WinHTTP handle from another thread, which unblocks the worker.
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace httpc {

// auto: WinHTTP automatic proxy, then a direct connection if that fails for a non-network
// reason. direct: never a proxy. system: the WinHTTP proxy configured with netsh.
enum class Proxy { Auto, Direct, System };

struct Options {
    std::vector<std::string> headers;  // "Name: value", no CR/LF
    std::string contentType;           // empty: guessed from the body (json or text)
    uint32_t timeoutMs = 15000;        // whole request, from the start of its execution
    bool insecure = false;             // https: accept invalid/self-signed certificates
    Proxy proxy = Proxy::Auto;
    bool followRedirects = true;       // never from https to http (WinHTTP policy)
    size_t maxResponseBytes = 1048576; // decoded body limit; 0 = the client's limit
    bool wantHeaders = true;           // false: do not collect response headers
};

// Parses "key=value;key=value". Keys: header (repeatable), content_type, timeout (1..120
// seconds), insecure (0|1), proxy (auto|direct|system), redirects (0|1), max_bytes,
// headers (0|1). Unknown keys and malformed values are errors.
bool ParseOptions(const std::string& text, Options* out, std::string* error);

struct Request {
    std::string method = "GET"; // GET POST PUT PATCH DELETE HEAD (case-insensitive)
    std::string url;            // http:// or https://
    std::string body;
    Options options;
};

struct Result {
    bool ok = false;     // true: a full HTTP response was received (any status, 404 included)
    int status = 0;      // HTTP status when ok
    std::string body;    // response body when ok
    std::vector<std::pair<std::string, std::string>> headers; // lower-case names, duplicates joined by ", "
    std::string error;   // readable text when !ok
    int code = 0;        // when !ok: Win32/WinHTTP error code, 0 for limits
};

enum class JobState { Unknown, Queued, Running, Done };

class Client {
public:
    struct Config {
        int threads = 4;
        size_t maxBodyBytes = 1048576;     // request body limit
        size_t maxResponseBytes = 1048576; // upper bound for Options::maxResponseBytes
        size_t maxJobs = 256;              // queued + running + done but not taken
        std::string userAgent = "StormFabric-HTTP/0.1";
        std::function<void(const std::string&)> log; // diagnostics (any thread), optional
    };

    static std::unique_ptr<Client> Create(const Config& config);
    ~Client(); // cancels everything and joins every thread

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    // Validates and queues a request. Returns its id (never 0, never reused), or 0 with
    // *error set when the request is invalid or the client is full. Network failures are
    // reported later in the Result. `tag` groups requests (the mod uses one per Lua VM).
    uint64_t Submit(const Request& request, uint64_t tag, std::string* error);

    // Cancels a queued or running request; its Result becomes "cancelled". False when the
    // request is unknown or already done.
    bool Cancel(uint64_t id);
    // Cancels and forgets every request of a tag, done or not.
    void DiscardTag(uint64_t tag);

    // Removes and returns a finished request. TakeAny returns the oldest finished request
    // of the tag. False when none is ready.
    bool Take(uint64_t id, Result* out);
    bool TakeAny(uint64_t tag, uint64_t* id, Result* out);

    JobState State(uint64_t id) const;
    bool Owns(uint64_t id, uint64_t tag) const; // the request exists and belongs to the tag
    size_t Count(uint64_t tag) const; // requests of the tag not taken yet
    size_t Count() const;             // all requests not taken yet

    // Number of worker/monitor threads alive in the process (diagnostics/tests).
    static int LiveThreads();

private:
    struct Job;
    Client() = default;
    void Worker();
    void Monitor();
    void Execute(Job& job);
    bool Attempt(Job& job, void* session, Result* result, unsigned long* failure, const char** stage);
    void* Session(Proxy mode);
    void SetHandle(Job& job, void* handle);
    void Finish(const std::shared_ptr<Job>& job, Result result);
    void Abort(Job& job, int reason); // closes the job's handle; caller holds mutex_

    Config config_;
    mutable std::mutex mutex_;
    std::condition_variable workCv_;    // workers: queue not empty or stopping
    std::condition_variable monitorCv_; // monitor: a deadline may have changed or stopping
    bool stop_ = false;
    uint64_t nextId_ = 1;
    std::map<uint64_t, std::shared_ptr<Job>> jobs_; // ordered by id = submission order
    std::deque<uint64_t> queue_;
    std::vector<std::thread> threads_;

    std::mutex sessionMutex_;
    void* sessions_[3] = {nullptr, nullptr, nullptr}; // by Proxy value, created on demand
};

} // namespace httpc
