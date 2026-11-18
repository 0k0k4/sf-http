#include "http_registry.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace httpc {
namespace {

std::string Trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r')) --e;
    return s.substr(b, e - b);
}

std::string Lower(std::string s) {
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

bool ParseSize(const std::string& v, size_t lo, size_t hi, size_t* out) {
    char* end = nullptr;
    const unsigned long long n = strtoull(v.c_str(), &end, 10);
    if (v.empty() || *end || n < lo || n > hi) return false;
    *out = (size_t)n;
    return true;
}

bool ParseIpv4(const std::string& s, unsigned char out[4]) {
    int part = 0;
    size_t i = 0;
    while (part < 4) {
        if (i >= s.size() || !isdigit((unsigned char)s[i])) return false;
        // "0177" is octal for Windows (127): a leading zero is not a plain decimal part.
        if (s[i] == '0' && i + 1 < s.size() && isdigit((unsigned char)s[i + 1])) return false;
        unsigned v = 0;
        size_t digits = 0;
        while (i < s.size() && isdigit((unsigned char)s[i]) && digits < 4) {
            v = v * 10 + (unsigned)(s[i] - '0');
            ++i;
            ++digits;
        }
        if (v > 255) return false;
        out[part++] = (unsigned char)v;
        if (part < 4) {
            if (i >= s.size() || s[i] != '.') return false;
            ++i;
        }
    }
    return i == s.size();
}

} // namespace

std::string DefaultConfigText() {
    return "# http mod configuration (edit with the game closed)\n"
           "# allow_hosts: comma-separated list: * (any), host, *.domain\n"
           "allow_hosts=*\n"
           "max_inflight=16\n"
           "max_per_vm=8\n"
           "rate_per_minute=60\n"
           "max_response_bytes=1048576\n"
           "max_body_bytes=1048576\n"
           "threads=4\n"
           "idle_timeout=30\n"
           "# local_network: who may reach localhost and private addresses (192.168.x, 10.x...):\n"
           "# none, addons (default: not microcontrollers, which workshop vehicles carry), all\n"
           "local_network=addons\n"
           "log_requests=1\n"
           "# microcontrollers: also give `http` to microcontroller Lua (workshop vehicles\n"
           "# included). 0 = addon scripts only.\n"
           "microcontrollers=1\n";
}

bool ParseConfig(const std::string& text, Limits* out, std::string* error) {
    Limits result = *out;
    size_t pos = 0;
    int lineNo = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        ++lineNo;
        const size_t hash = line.find('#');
        if (hash != std::string::npos) line.resize(hash);
        line = Trim(line);
        if (line.empty()) continue;
        const size_t eq = line.find('=');
        auto bad = [&](const std::string& what) {
            if (error) *error = "line " + std::to_string(lineNo) + ": " + what;
            return false;
        };
        if (eq == std::string::npos) return bad("expected key=value");
        const std::string key = Trim(line.substr(0, eq));
        const std::string value = Trim(line.substr(eq + 1));
        size_t n = 0;
        if (key == "allow_hosts") {
            result.allowHosts.clear();
            size_t p = 0;
            while (p <= value.size()) {
                size_t comma = value.find(',', p);
                if (comma == std::string::npos) comma = value.size();
                const std::string h = Lower(Trim(value.substr(p, comma - p)));
                if (!h.empty()) result.allowHosts.push_back(h);
                p = comma + 1;
            }
        } else if (key == "max_inflight") {
            if (!ParseSize(value, 1, 256, &result.maxInflight)) return bad("max_inflight: 1..256");
        } else if (key == "max_per_vm") {
            if (!ParseSize(value, 1, 256, &result.maxPerVm)) return bad("max_per_vm: 1..256");
        } else if (key == "rate_per_minute") {
            if (!ParseSize(value, 1, 100000, &result.ratePerMinute)) return bad("rate_per_minute: 1..100000");
        } else if (key == "max_response_bytes") {
            if (!ParseSize(value, 1, 1048576, &result.maxResponseBytes)) return bad("max_response_bytes: 1..1048576");
        } else if (key == "max_body_bytes") {
            if (!ParseSize(value, 1, 1048576, &result.maxBodyBytes)) return bad("max_body_bytes: 1..1048576");
        } else if (key == "threads") {
            if (!ParseSize(value, 1, 32, &n)) return bad("threads: 1..32");
            result.threads = (int)n;
        } else if (key == "idle_timeout") {
            char* e = nullptr;
            const double s = strtod(value.c_str(), &e);
            if (value.empty() || *e || !(s >= 1 && s <= 86400)) return bad("idle_timeout: 1..86400 seconds");
            result.idleSeconds = s;
        } else if (key == "local_network") {
            if (value == "none") result.localNetwork = LocalNetwork::None;
            else if (value == "addons") result.localNetwork = LocalNetwork::Addons;
            else if (value == "all") result.localNetwork = LocalNetwork::All;
            else return bad("local_network: none, addons or all");
        } else if (key == "log_requests") {
            if (value == "1") result.logRequests = true;
            else if (value == "0") result.logRequests = false;
            else return bad("log_requests: 0 or 1");
        } else if (key == "microcontrollers") {
            if (value == "1") result.microcontrollers = true;
            else if (value == "0") result.microcontrollers = false;
            else return bad("microcontrollers: 0 or 1");
        } else {
            return bad("unknown key: " + key);
        }
    }
    *out = result;
    return true;
}

std::string UrlHost(const std::string& url) {
    const size_t scheme = url.find("://");
    if (scheme == std::string::npos) return std::string();
    const size_t end = url.find_first_of("/?#", scheme + 3);
    std::string authority = url.substr(scheme + 3, end == std::string::npos ? std::string::npos : end - scheme - 3);
    const size_t at = authority.rfind('@');
    if (at != std::string::npos) authority = authority.substr(at + 1);
    if (!authority.empty() && authority[0] == '[') {
        const size_t close = authority.find(']');
        return Lower(close == std::string::npos ? authority : authority.substr(0, close + 1));
    }
    const size_t colon = authority.find(':');
    if (colon != std::string::npos) authority.resize(colon);
    while (!authority.empty() && authority.back() == '.') authority.pop_back(); // "host." == "host"
    return Lower(authority);
}

bool HostAllowed(const std::vector<std::string>& patterns, const std::string& host) {
    if (host.empty()) return false;
    for (const std::string& p : patterns) {
        if (p == "*" || p == host) return true;
        if (p.size() > 2 && p[0] == '*' && p[1] == '.') {
            const std::string suffix = p.substr(1); // ".domain"
            if (host.size() > suffix.size() && host.compare(host.size() - suffix.size(), suffix.size(), suffix) == 0)
                return true;
        }
    }
    return false;
}

bool IsLocalHost(const std::string& host) {
    if (host == "localhost" || (host.size() > 10 && host.compare(host.size() - 10, 10, ".localhost") == 0))
        return true;
    unsigned char a[4];
    if (ParseIpv4(host, a)) {
        return a[0] == 0 || a[0] == 10 || a[0] == 127 || (a[0] == 169 && a[1] == 254) ||
               (a[0] == 172 && a[1] >= 16 && a[1] <= 31) || (a[0] == 192 && a[1] == 168) ||
               (a[0] == 100 && a[1] >= 64 && a[1] <= 127); // carrier-grade NAT
    }
    if (!host.empty() && host[0] == '[') {
        const std::string h = host.substr(1, host.size() - 2);
        if (h == "::1" || h == "::") return true;
        if (h.size() >= 2 && (h.compare(0, 2, "fc") == 0 || h.compare(0, 2, "fd") == 0)) return true; // ULA
        if (h.size() >= 4 && h.compare(0, 4, "fe80") == 0) return true;                               // link-local
        if (h.compare(0, 7, "::ffff:") == 0) {                                                       // mapped IPv4
            unsigned char m[4];
            return ParseIpv4(h.substr(7), m) ? IsLocalHost(h.substr(7)) : true;
        }
        return false;
    }
    // Other numeric forms WinHTTP may accept (decimal "2130706433", hex "0x7f.1"...) are
    // refused rather than guessed.
    // A host starting with a digit and made only of [0-9a-fx.] is such a form.
    if (host.empty() || !isdigit((unsigned char)host[0])) return false;
    return host.find_first_not_of("0123456789abcdefx.") == std::string::npos;
}

std::string SafeUrl(const std::string& url) {
    const size_t scheme = url.find("://");
    if (scheme == std::string::npos) return "?";
    const size_t end = url.find_first_of("/?#", scheme + 3);
    std::string authority = url.substr(scheme + 3, end == std::string::npos ? std::string::npos : end - scheme - 3);
    const size_t at = authority.rfind('@');
    if (at != std::string::npos) authority = authority.substr(at + 1);
    return url.substr(0, scheme + 3) + authority;
}

Registry::Registry(Clock clock, Logger log) : clock_(std::move(clock)), log_(std::move(log)) {
    Configure(Limits());
}

Registry::~Registry() {
    std::unique_ptr<Client> client;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        client = std::move(client_);
    }
    client.reset(); // joins the threads outside the lock
}

double Registry::Now() const {
    if (clock_) return clock_();
    return (double)GetTickCount64() / 1000.0;
}

void Registry::Configure(const Limits& limits) {
    Client::Config cfg;
    cfg.threads = limits.threads;
    cfg.maxBodyBytes = limits.maxBodyBytes;
    cfg.maxResponseBytes = limits.maxResponseBytes;
    cfg.maxJobs = limits.maxInflight;
    if (log_) {
        Logger log = log_;
        cfg.log = [log](const std::string& m) { log(m); };
    }
    std::unique_ptr<Client> fresh = Client::Create(cfg);
    std::unique_ptr<Client> old;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        limits_ = limits;
        old = std::move(client_);
        client_ = std::move(fresh);
        lastTouched_.clear();
        starts_.clear();
    }
    old.reset();
}

Limits Registry::GetLimits() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return limits_;
}

void Registry::TouchLocked(uint64_t owner, double now) { lastTouched_[owner] = now; }

void Registry::Touch(uint64_t owner) {
    std::lock_guard<std::mutex> lock(mutex_);
    TouchLocked(owner, Now());
}

uint64_t Registry::Submit(uint64_t owner, bool microcontroller, const std::string& method, const std::string& url,
                          const std::string& body, const std::string& optionsText, std::string* error) {
    Sweep();
    auto fail = [&](const std::string& text) -> uint64_t {
        if (error) *error = text;
        return 0;
    };
    Request request;
    request.method = method;
    request.url = url;
    request.body = body;
    if (!ParseOptions(optionsText, &request.options, error)) return 0;
    const std::string host = UrlHost(url);

    std::lock_guard<std::mutex> lock(mutex_);
    const double now = Now();
    TouchLocked(owner, now);
    if (!HostAllowed(limits_.allowHosts, host)) {
        if (log_) log_("refused " + SafeUrl(url) + " (allow_hosts)");
        return fail("host not allowed by http.cfg: " + host);
    }
    if (IsLocalHost(host) && (limits_.localNetwork == LocalNetwork::None ||
                              (limits_.localNetwork == LocalNetwork::Addons && microcontroller))) {
        if (log_) log_(std::string("refused ") + SafeUrl(url) + " (local_network)");
        return fail("local network address not allowed by http.cfg (local_network): " + host);
    }
    if (client_->Count(owner) >= limits_.maxPerVm)
        return fail("too many pending requests for this script (max " + std::to_string(limits_.maxPerVm) +
                    "): call http.poll()");
    std::deque<double>& starts = starts_[owner];
    while (!starts.empty() && now - starts.front() >= 60) starts.pop_front();
    if (starts.size() >= limits_.ratePerMinute)
        return fail("rate limit: " + std::to_string(limits_.ratePerMinute) + " requests per minute");
    const uint64_t id = client_->Submit(request, owner, error);
    if (!id) return 0;
    starts.push_back(now);
    if (limits_.logRequests && log_)
        log_("#" + std::to_string(id) + " " + request.method + " " + SafeUrl(url) +
             (microcontroller ? " (microcontroller)" : "") + (request.options.insecure ? " (insecure)" : ""));
    return id;
}

bool Registry::Poll(uint64_t owner, uint64_t id, uint64_t* outId, Result* out) {
    std::lock_guard<std::mutex> lock(mutex_);
    TouchLocked(owner, Now());
    if (id) {
        if (!client_->Owns(id, owner) || !client_->Take(id, out)) return false;
        if (outId) *outId = id;
        return true;
    }
    uint64_t taken = 0;
    if (!client_->TakeAny(owner, &taken, out)) return false;
    if (outId) *outId = taken;
    return true;
}

JobState Registry::State(uint64_t owner, uint64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    TouchLocked(owner, Now());
    return client_->Owns(id, owner) ? client_->State(id) : JobState::Unknown;
}

bool Registry::Cancel(uint64_t owner, uint64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    TouchLocked(owner, Now());
    return client_->Owns(id, owner) && client_->Cancel(id);
}

size_t Registry::Pending(uint64_t owner) {
    std::lock_guard<std::mutex> lock(mutex_);
    TouchLocked(owner, Now());
    return client_->Count(owner);
}

size_t Registry::Sweep() {
    std::vector<uint64_t> idle;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const double now = Now();
        for (auto it = lastTouched_.begin(); it != lastTouched_.end();) {
            if (now - it->second > limits_.idleSeconds) {
                idle.push_back(it->first);
                if (client_->Count(it->first) && log_)
                    log_("released " + std::to_string(client_->Count(it->first)) +
                         " request(s) of an idle script (owner " + std::to_string(it->first) + ")");
                client_->DiscardTag(it->first);
                starts_.erase(it->first);
                it = lastTouched_.erase(it);
            } else {
                ++it;
            }
        }
    }
    return idle.size();
}

void Registry::CloseOwner(uint64_t owner) {
    std::lock_guard<std::mutex> lock(mutex_);
    client_->DiscardTag(owner);
    lastTouched_.erase(owner);
    starts_.erase(owner);
}

void Registry::CloseAll() {
    std::lock_guard<std::mutex> lock(mutex_);
    const size_t n = client_->Count();
    if (n && log_) log_("dropping " + std::to_string(n) + " request(s)");
    for (const auto& kv : lastTouched_) client_->DiscardTag(kv.first);
    client_->DiscardTag(0);
    lastTouched_.clear();
    starts_.clear();
}

size_t Registry::Count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return client_->Count();
}

} // namespace httpc
