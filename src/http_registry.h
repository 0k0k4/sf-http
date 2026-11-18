// Request registry: configuration, per-owner (Lua VM) quotas and isolation, host rules,
// collection of requests whose VM is gone. Independent of the StormFabric SDK.
// Thread-safe; never blocks on the network.
#pragma once
#include "http_client.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace httpc {

// Who may reach loopback and private network addresses (127.x, 10.x, 192.168.x, ...).
enum class LocalNetwork { None, Addons, All };

struct Limits {
    std::vector<std::string> allowHosts{"*"}; // "*", "host", "*.domain"
    size_t maxInflight = 16;          // requests not yet returned, all scripts together
    size_t maxPerVm = 8;              // requests not yet returned, per script VM
    size_t ratePerMinute = 60;        // requests started per VM and per minute
    size_t maxResponseBytes = 1048576;
    size_t maxBodyBytes = 1048576;
    int threads = 4;
    double idleSeconds = 30;          // a VM that made no call for this long loses its requests
    LocalNetwork localNetwork = LocalNetwork::Addons;
    bool logRequests = true;
    bool microcontrollers = true;     // also expose the library to microcontroller Lua
};

// "key=value" lines; '#' starts a comment. Unknown keys are errors.
bool ParseConfig(const std::string& text, Limits* out, std::string* error);
std::string DefaultConfigText();

// "https://user@Host:1234/path" -> "host" (lower case, IPv6 brackets kept).
std::string UrlHost(const std::string& url);
bool HostAllowed(const std::vector<std::string>& patterns, const std::string& host);
// Literal loopback/private/link-local address, or "localhost" / "*.localhost".
bool IsLocalHost(const std::string& host);
// Scheme + host + port only: paths and queries may carry tokens.
std::string SafeUrl(const std::string& url);

class Registry {
public:
    using Clock = std::function<double()>; // seconds, monotonic
    using Logger = std::function<void(const std::string&)>;

    explicit Registry(Clock clock = nullptr, Logger log = nullptr);
    ~Registry();

    // Applies the limits and (re)creates the client: pending requests are dropped.
    void Configure(const Limits& limits);
    Limits GetLimits() const;

    // Owner: opaque token of the calling VM (0 = unknown, shared). Returns the id (> 0),
    // or 0 with *error set.
    uint64_t Submit(uint64_t owner, bool microcontroller, const std::string& method, const std::string& url,
                    const std::string& body, const std::string& optionsText, std::string* error);
    // Next finished request of the owner (id == 0) or that request (id != 0). The request
    // is released once returned.
    bool Poll(uint64_t owner, uint64_t id, uint64_t* outId, Result* out);
    JobState State(uint64_t owner, uint64_t id);
    bool Cancel(uint64_t owner, uint64_t id);
    size_t Pending(uint64_t owner);
    void Touch(uint64_t owner);

    // Drops the requests of owners idle for longer than idleSeconds. Returns the number
    // of owners released. Call periodically (any thread).
    size_t Sweep();
    void CloseOwner(uint64_t owner);
    void CloseAll();
    size_t Count() const;

private:
    double Now() const;
    void TouchLocked(uint64_t owner, double now);

    Clock clock_;
    Logger log_;
    mutable std::mutex mutex_;
    Limits limits_;
    std::unique_ptr<Client> client_;
    std::map<uint64_t, double> lastTouched_;            // by owner
    std::map<uint64_t, std::deque<double>> starts_;     // by owner: start times in the last minute
};

} // namespace httpc
