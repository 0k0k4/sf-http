// HTTP/HTTPS client core on top of WinHTTP. See http_client.h.
#include "http_client.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <chrono>
#include <cstring>

#pragma comment(lib, "winhttp.lib")

#ifndef WINHTTP_OPTION_DECOMPRESSION
#define WINHTTP_OPTION_DECOMPRESSION 118
#define WINHTTP_DECOMPRESSION_FLAG_GZIP 0x00000001
#define WINHTTP_DECOMPRESSION_FLAG_DEFLATE 0x00000002
#define WINHTTP_DECOMPRESSION_FLAG_ALL (WINHTTP_DECOMPRESSION_FLAG_GZIP | WINHTTP_DECOMPRESSION_FLAG_DEFLATE)
#endif
#ifndef WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL
#define WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL 133
#define WINHTTP_PROTOCOL_FLAG_HTTP2 0x1
#endif

namespace httpc {
namespace {

using Clock = std::chrono::steady_clock;

std::atomic<int> g_liveThreads{0};

struct ThreadCounter {
    ThreadCounter() { ++g_liveThreads; }
    ~ThreadCounter() { --g_liveThreads; }
};

enum AbortReason { kNotAborted = 0, kCancelled = 1, kTimedOut = 2, kShutdown = 3 };

std::wstring Widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), &w[0], n);
    return w;
}

std::string Narrow(const wchar_t* w, size_t len) {
    if (!w || !len) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, (int)len, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, (int)len, &s[0], n, nullptr, nullptr);
    return s;
}

std::string ErrorText(DWORD code) {
    wchar_t* buffer = nullptr;
    DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_IGNORE_INSERTS;
    HMODULE module = nullptr;
    if (code >= WINHTTP_ERROR_BASE && code <= WINHTTP_ERROR_LAST) {
        module = GetModuleHandleW(L"winhttp.dll");
        flags |= FORMAT_MESSAGE_FROM_HMODULE;
    } else {
        flags |= FORMAT_MESSAGE_FROM_SYSTEM;
    }
    const DWORD len = FormatMessageW(flags, module, code, 0, (LPWSTR)&buffer, 0, nullptr);
    std::string text = len ? Narrow(buffer, len) : std::string();
    if (buffer) LocalFree(buffer);
    while (!text.empty() && (text.back() == '\r' || text.back() == '\n' || text.back() == ' ' || text.back() == '.'))
        text.pop_back();
    if (text.empty()) text = "error";
    return text + " (" + std::to_string(code) + ")";
}

bool StartsWithNoCase(const std::string& s, const char* prefix) {
    const size_t n = strlen(prefix);
    return s.size() >= n && _strnicmp(s.c_str(), prefix, n) == 0;
}

bool HasControlChars(const std::string& s) {
    for (unsigned char c : s)
        if (c < 0x20 || c == 0x7f) return true;
    return false;
}

std::string Trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
    return s.substr(b, e - b);
}

std::string Lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

std::string Upper(std::string s) {
    for (char& c : s)
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    return s;
}

bool ValidHeader(const std::string& h) {
    const size_t colon = h.find(':');
    if (colon == std::string::npos || colon == 0) return false;
    for (size_t i = 0; i < colon; ++i) {
        const unsigned char c = (unsigned char)h[i];
        if (c <= 0x20 || c >= 0x7f) return false; // token characters only
    }
    for (size_t i = colon + 1; i < h.size(); ++i) {
        const unsigned char c = (unsigned char)h[i];
        if ((c < 0x20 && c != '\t') || c == 0x7f) return false; // no CR/LF injection
    }
    return true;
}

bool HeaderNamed(const std::string& h, const char* name) {
    const size_t colon = h.find(':');
    return colon != std::string::npos && Lower(Trim(h.substr(0, colon))) == name;
}

bool ParseBool(const std::string& v, bool* out) {
    if (v == "1" || v == "true") { *out = true; return true; }
    if (v == "0" || v == "false") { *out = false; return true; }
    return false;
}

const char* const kMethods[] = {"GET", "POST", "PUT", "PATCH", "DELETE", "HEAD"};

// Errors that a different proxy mode cannot fix.
bool Definitive(DWORD err) {
    return err == ERROR_WINHTTP_CANNOT_CONNECT || err == ERROR_WINHTTP_NAME_NOT_RESOLVED ||
           err == ERROR_WINHTTP_TIMEOUT || err == ERROR_WINHTTP_SECURE_FAILURE ||
           err == ERROR_WINHTTP_OPERATION_CANCELLED || err == ERROR_WINHTTP_REDIRECT_FAILED ||
           err == ERROR_WINHTTP_CONNECTION_ERROR || err == ERROR_WINHTTP_INVALID_SERVER_RESPONSE;
}

// Raw CRLF header block -> lower-case names, duplicates joined. Skips the status line.
void ParseHeaders(const std::string& raw, std::vector<std::pair<std::string, std::string>>* out) {
    size_t pos = raw.find("\r\n");
    if (pos == std::string::npos) return;
    pos += 2;
    while (pos < raw.size()) {
        size_t end = raw.find("\r\n", pos);
        if (end == std::string::npos) end = raw.size();
        const std::string line = raw.substr(pos, end - pos);
        pos = end + 2;
        const size_t colon = line.find(':');
        if (colon == std::string::npos || colon == 0) continue;
        const std::string name = Lower(Trim(line.substr(0, colon)));
        const std::string value = Trim(line.substr(colon + 1));
        if (name.empty()) continue;
        auto it = std::find_if(out->begin(), out->end(), [&](const auto& p) { return p.first == name; });
        if (it == out->end()) out->emplace_back(name, value);
        else it->second += ", " + value;
    }
}

} // namespace

bool ParseOptions(const std::string& text, Options* out, std::string* error) {
    Options result = *out;
    auto fail = [&](const std::string& message) {
        if (error) *error = message;
        return false;
    };
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t end = text.find(';', pos);
        if (end == std::string::npos) end = text.size();
        const std::string item = text.substr(pos, end - pos);
        pos = end + 1;
        if (!Trim(item).empty()) {
            const size_t eq = item.find('=');
            if (eq == std::string::npos) return fail("option without '=': " + Trim(item));
            const std::string key = Trim(item.substr(0, eq));
            const std::string value = Trim(item.substr(eq + 1));
            if (key == "header") {
                if (!ValidHeader(value)) return fail("invalid header: " + value);
                result.headers.push_back(value);
            } else if (key == "content_type") {
                if (value.empty() || HasControlChars(value)) return fail("invalid content_type");
                result.contentType = value;
            } else if (key == "insecure") {
                if (!ParseBool(value, &result.insecure)) return fail("insecure must be 0 or 1");
            } else if (key == "redirects") {
                if (!ParseBool(value, &result.followRedirects)) return fail("redirects must be 0 or 1");
            } else if (key == "headers") {
                if (!ParseBool(value, &result.wantHeaders)) return fail("headers must be 0 or 1");
            } else if (key == "proxy") {
                if (value == "auto") result.proxy = Proxy::Auto;
                else if (value == "direct") result.proxy = Proxy::Direct;
                else if (value == "system") result.proxy = Proxy::System;
                else return fail("proxy must be auto, direct or system");
            } else if (key == "timeout") {
                char* endp = nullptr;
                const double seconds = strtod(value.c_str(), &endp);
                if (value.empty() || *endp || !(seconds >= 1 && seconds <= 120))
                    return fail("timeout must be 1..120 seconds");
                result.timeoutMs = (uint32_t)(seconds * 1000);
            } else if (key == "max_bytes") {
                char* endp = nullptr;
                const unsigned long long n = strtoull(value.c_str(), &endp, 10);
                if (value.empty() || *endp || n == 0 || n > (1ull << 30)) return fail("max_bytes must be a positive number");
                result.maxResponseBytes = (size_t)n;
            } else {
                return fail("unknown option: " + key);
            }
        }
        if (end == text.size()) break;
    }
    *out = result;
    return true;
}

struct Client::Job {
    uint64_t id = 0;
    uint64_t tag = 0;
    Request request;
    std::string method;   // upper case
    bool secure = false;
    std::wstring host, path;
    uint16_t port = 0;
    JobState state = JobState::Queued;
    Clock::time_point deadline;
    HINTERNET handle = nullptr; // current request handle, guarded by Client::mutex_
    int abort = kNotAborted;    // guarded by Client::mutex_
    Result result;
};

int Client::LiveThreads() { return g_liveThreads.load(); }

std::unique_ptr<Client> Client::Create(const Config& config) {
    std::unique_ptr<Client> c(new Client());
    c->config_ = config;
    c->config_.threads = std::max(1, std::min(config.threads, 32));
    for (int i = 0; i < c->config_.threads; ++i) c->threads_.emplace_back(&Client::Worker, c.get());
    c->threads_.emplace_back(&Client::Monitor, c.get());
    return c;
}

Client::~Client() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
        for (auto& kv : jobs_) Abort(*kv.second, kShutdown);
        workCv_.notify_all();
        monitorCv_.notify_all();
    }
    for (std::thread& t : threads_)
        if (t.joinable()) t.join();
    for (void*& s : sessions_)
        if (s) { WinHttpCloseHandle(s); s = nullptr; }
}

void Client::Abort(Job& job, int reason) {
    if (job.abort == kNotAborted) job.abort = reason;
    if (job.handle) {
        WinHttpCloseHandle(job.handle); // unblocks the worker's current WinHTTP call
        job.handle = nullptr;
    }
}

uint64_t Client::Submit(const Request& request, uint64_t tag, std::string* error) {
    auto fail = [&](const std::string& text) -> uint64_t {
        if (error) *error = text;
        return 0;
    };
    const std::string method = Upper(request.method);
    if (std::find_if(std::begin(kMethods), std::end(kMethods), [&](const char* m) { return method == m; }) ==
        std::end(kMethods))
        return fail("method must be GET, POST, PUT, PATCH, DELETE or HEAD");
    const std::string& url = request.url;
    if (url.size() > 8192 || HasControlChars(url)) return fail("invalid url");
    bool secure;
    if (StartsWithNoCase(url, "http://")) secure = false;
    else if (StartsWithNoCase(url, "https://")) secure = true;
    else return fail("url must start with http:// or https://");
    for (const std::string& h : request.options.headers)
        if (!ValidHeader(h)) return fail("invalid header: " + h);
    if (HasControlChars(request.options.contentType)) return fail("invalid content_type");
    if (request.body.size() > config_.maxBodyBytes)
        return fail("body exceeds " + std::to_string(config_.maxBodyBytes) + " bytes");
    if (method == "HEAD" && !request.body.empty()) return fail("HEAD cannot have a body");

    const std::wstring wide = Widen(url);
    if (wide.empty()) return fail("url is not valid UTF-8");
    URL_COMPONENTS parts = {};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = (DWORD)-1;
    parts.dwUrlPathLength = (DWORD)-1;
    parts.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(wide.c_str(), (DWORD)wide.size(), 0, &parts) || !parts.dwHostNameLength)
        return fail("invalid url");

    auto job = std::make_shared<Job>();
    job->tag = tag;
    job->request = request;
    job->method = method;
    job->secure = secure;
    job->host.assign(parts.lpszHostName, parts.dwHostNameLength);
    job->port = parts.nPort;
    job->path.assign(parts.lpszUrlPath ? parts.lpszUrlPath : L"", parts.dwUrlPathLength);
    if (parts.lpszExtraInfo && parts.dwExtraInfoLength) {
        std::wstring extra(parts.lpszExtraInfo, parts.dwExtraInfoLength);
        const size_t hash = extra.find(L'#'); // fragments are never sent
        if (hash != std::wstring::npos) extra.resize(hash);
        job->path += extra;
    }
    if (job->path.empty()) job->path = L"/";
    size_t& limit = job->request.options.maxResponseBytes;
    if (limit == 0 || limit > config_.maxResponseBytes) limit = config_.maxResponseBytes;

    std::lock_guard<std::mutex> lock(mutex_);
    if (stop_) return fail("client stopped");
    if (jobs_.size() >= config_.maxJobs) return fail("too many requests");
    job->id = nextId_++;
    jobs_[job->id] = job;
    queue_.push_back(job->id);
    workCv_.notify_one();
    return job->id;
}

bool Client::Cancel(uint64_t id) {
    std::shared_ptr<Job> queued;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = jobs_.find(id);
        if (it == jobs_.end() || it->second->state == JobState::Done) return false;
        Job& job = *it->second;
        if (job.state == JobState::Running) {
            Abort(job, kCancelled);
            return true;
        }
        queue_.erase(std::remove(queue_.begin(), queue_.end(), id), queue_.end());
        job.abort = kCancelled;
        queued = it->second;
    }
    Result r;
    r.error = "cancelled";
    r.code = ERROR_WINHTTP_OPERATION_CANCELLED;
    Finish(queued, std::move(r));
    return true;
}

void Client::DiscardTag(uint64_t tag) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = jobs_.begin(); it != jobs_.end();) {
        Job& job = *it->second;
        if (job.tag != tag) { ++it; continue; }
        if (job.state == JobState::Queued)
            queue_.erase(std::remove(queue_.begin(), queue_.end(), job.id), queue_.end());
        Abort(job, kCancelled); // a running worker still holds the shared_ptr
        it = jobs_.erase(it);
    }
}

bool Client::Take(uint64_t id, Result* out) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = jobs_.find(id);
    if (it == jobs_.end() || it->second->state != JobState::Done) return false;
    *out = std::move(it->second->result);
    jobs_.erase(it);
    return true;
}

bool Client::TakeAny(uint64_t tag, uint64_t* id, Result* out) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = jobs_.begin(); it != jobs_.end(); ++it) {
        if (it->second->tag != tag || it->second->state != JobState::Done) continue;
        *id = it->first;
        *out = std::move(it->second->result);
        jobs_.erase(it);
        return true;
    }
    return false;
}

JobState Client::State(uint64_t id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = jobs_.find(id);
    return it == jobs_.end() ? JobState::Unknown : it->second->state;
}

bool Client::Owns(uint64_t id, uint64_t tag) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = jobs_.find(id);
    return it != jobs_.end() && it->second->tag == tag;
}

size_t Client::Count(uint64_t tag) const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t n = 0;
    for (const auto& kv : jobs_)
        if (kv.second->tag == tag) ++n;
    return n;
}

size_t Client::Count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return jobs_.size();
}

void Client::Finish(const std::shared_ptr<Job>& job, Result result) {
    std::lock_guard<std::mutex> lock(mutex_);
    job->result = std::move(result);
    job->state = JobState::Done;
}

void Client::Worker() {
    ThreadCounter counter;
    for (;;) {
        std::shared_ptr<Job> job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            workCv_.wait(lock, [&] { return stop_ || !queue_.empty(); });
            if (stop_) return;
            const uint64_t id = queue_.front();
            queue_.pop_front();
            auto it = jobs_.find(id);
            if (it == jobs_.end()) continue;
            job = it->second;
            job->state = JobState::Running;
            job->deadline = Clock::now() + std::chrono::milliseconds(job->request.options.timeoutMs);
            monitorCv_.notify_all();
        }
        Execute(*job);
    }
}

void Client::Monitor() {
    ThreadCounter counter;
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stop_) {
        const Clock::time_point now = Clock::now();
        Clock::time_point next = now + std::chrono::seconds(1);
        for (auto& kv : jobs_) {
            Job& job = *kv.second;
            if (job.state != JobState::Running || job.abort != kNotAborted) continue;
            if (job.deadline <= now) Abort(job, kTimedOut);
            else next = std::min(next, job.deadline);
        }
        monitorCv_.wait_until(lock, next);
    }
}

void* Client::Session(Proxy mode) {
    std::lock_guard<std::mutex> lock(sessionMutex_);
    void*& slot = sessions_[(int)mode];
    if (slot) return slot;
    const DWORD access = mode == Proxy::Direct   ? WINHTTP_ACCESS_TYPE_NO_PROXY
                         : mode == Proxy::System ? WINHTTP_ACCESS_TYPE_DEFAULT_PROXY
                                                 : WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY;
    const std::wstring agent = Widen(config_.userAgent);
    HINTERNET session = WinHttpOpen(agent.c_str(), access, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return nullptr;
    DWORD decompression = WINHTTP_DECOMPRESSION_FLAG_ALL;
    WinHttpSetOption(session, WINHTTP_OPTION_DECOMPRESSION, &decompression, sizeof(decompression)); // Win 8.1+
    DWORD http2 = WINHTTP_PROTOCOL_FLAG_HTTP2;
    WinHttpSetOption(session, WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL, &http2, sizeof(http2)); // Win 10 1607+
    slot = session;
    return slot;
}

// Stores the request handle so Cancel/Monitor can close it. When the job was aborted in
// the meantime, the handle is closed at once and the next WinHTTP call fails.
void Client::SetHandle(Job& job, void* handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (job.abort != kNotAborted) {
        if (handle) WinHttpCloseHandle(handle);
        job.handle = nullptr;
        return;
    }
    job.handle = handle;
}

// One attempt with one session. Returns true when a complete response (or a limit error)
// is in *result; false with *failure/*stage set for a WinHTTP failure.
bool Client::Attempt(Job& job, void* session, Result* result, unsigned long* failure, const char** stage) {
    const Options& o = job.request.options;
    HINTERNET connect = nullptr;
    auto handle = [&]() -> HINTERNET {
        std::lock_guard<std::mutex> lock(mutex_);
        return job.handle;
    };
    auto failed = [&](const char* what) -> bool {
        *failure = GetLastError();
        if (!*failure) *failure = ERROR_INTERNAL_ERROR;
        *stage = what;
        return false;
    };
    struct Cleanup {
        Client* self; Job* job; HINTERNET* connect;
        ~Cleanup() {
            {
                std::lock_guard<std::mutex> lock(self->mutex_);
                if (job->handle) { WinHttpCloseHandle(job->handle); job->handle = nullptr; }
            }
            if (*connect) WinHttpCloseHandle(*connect);
        }
    } cleanup{this, &job, &connect};

    connect = WinHttpConnect(session, job.host.c_str(), job.port, 0);
    if (!connect) return failed("connect");
    const std::wstring method = Widen(job.method);
    HINTERNET request = WinHttpOpenRequest(connect, method.c_str(), job.path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, job.secure ? WINHTTP_FLAG_SECURE : 0);
    if (!request) return failed("open request");
    SetHandle(job, request);
    if (!(request = handle())) { SetLastError(ERROR_WINHTTP_OPERATION_CANCELLED); return failed("cancelled"); }

    const int t = (int)o.timeoutMs;
    WinHttpSetTimeouts(request, t, t, t, t);
    if (job.secure && o.insecure) {
        DWORD flags = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                      SECURITY_FLAG_IGNORE_CERT_CN_INVALID | SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
        WinHttpSetOption(request, WINHTTP_OPTION_SECURITY_FLAGS, &flags, sizeof(flags));
    }
    if (!o.followRedirects) {
        DWORD feature = WINHTTP_DISABLE_REDIRECTS;
        WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE, &feature, sizeof(feature));
    }

    std::string headers;
    bool hasContentType = false;
    for (const std::string& h : o.headers) {
        headers += h + "\r\n";
        if (HeaderNamed(h, "content-type")) hasContentType = true;
    }
    const std::string& body = job.request.body;
    if (!hasContentType && (!body.empty() || !o.contentType.empty())) {
        std::string type = o.contentType;
        if (type.empty()) {
            const std::string start = Trim(body.substr(0, 64));
            type = (!start.empty() && (start[0] == '{' || start[0] == '[')) ? "application/json"
                                                                             : "text/plain; charset=utf-8";
        }
        headers += "Content-Type: " + type + "\r\n";
    }
    if (!headers.empty()) {
        const std::wstring w = Widen(headers);
        if (w.empty()) { SetLastError(ERROR_INVALID_PARAMETER); return failed("request headers"); }
        if (!WinHttpAddRequestHeaders(request, w.c_str(), (DWORD)w.size(), WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE))
            return failed("request headers");
    }
    if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data(), (DWORD)body.size(),
                            (DWORD)body.size(), 0))
        return failed("send request");
    if (!WinHttpReceiveResponse(request, nullptr)) return failed("receive response");

    DWORD status = 0, size = sizeof(status);
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX))
        return failed("query status");

    Result r;
    r.status = (int)status;
    if (o.wantHeaders) {
        DWORD bytes = 0;
        WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
                            WINHTTP_NO_OUTPUT_BUFFER, &bytes, WINHTTP_NO_HEADER_INDEX);
        if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && bytes) {
            std::wstring raw(bytes / sizeof(wchar_t) + 1, L'\0');
            if (WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, &raw[0],
                                    &bytes, WINHTTP_NO_HEADER_INDEX))
                ParseHeaders(Narrow(raw.c_str(), bytes / sizeof(wchar_t)), &r.headers);
        }
    }

    if (job.method != "HEAD") {
        std::vector<char> chunk(64 * 1024);
        for (;;) {
            DWORD read = 0;
            if (!WinHttpReadData(request, chunk.data(), (DWORD)chunk.size(), &read)) return failed("read body");
            if (!read) break;
            if (r.body.size() + read > o.maxResponseBytes) {
                result->ok = false;
                result->code = 0;
                result->error = "response too large (limit " + std::to_string(o.maxResponseBytes) + " bytes)";
                return true;
            }
            r.body.append(chunk.data(), read);
        }
        // WinHTTP ends the read without an error when the server closes the connection
        // before the announced length. Content-Length is the encoded size, so the check is
        // only possible without Content-Encoding.
        DWORD length = 0, lengthSize = sizeof(length), encodingSize = 0;
        const bool hasLength = WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                                                   WINHTTP_HEADER_NAME_BY_INDEX, &length, &lengthSize,
                                                   WINHTTP_NO_HEADER_INDEX) != FALSE;
        WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_ENCODING, WINHTTP_HEADER_NAME_BY_INDEX,
                            WINHTTP_NO_OUTPUT_BUFFER, &encodingSize, WINHTTP_NO_HEADER_INDEX);
        const bool encoded = GetLastError() == ERROR_INSUFFICIENT_BUFFER && encodingSize > 0;
        if (hasLength && !encoded && r.body.size() < length) {
            result->ok = false;
            result->code = ERROR_WINHTTP_CONNECTION_ERROR;
            result->error = "connection closed after " + std::to_string(r.body.size()) + " of " +
                            std::to_string(length) + " bytes";
            return true;
        }
    }
    r.ok = true;
    *result = std::move(r);
    return true;
}

void Client::Execute(Job& job) {
    std::vector<Proxy> modes;
    switch (job.request.options.proxy) {
    case Proxy::Direct: modes = {Proxy::Direct}; break;
    case Proxy::System: modes = {Proxy::System}; break;
    default: modes = {Proxy::Auto, Proxy::Direct}; break;
    }
    static const char* const kModeNames[] = {"auto proxy", "direct", "system proxy"};

    Result result;
    bool done = false;
    unsigned long failure = 0;
    const char* stage = "";
    std::string history;
    for (size_t i = 0; i < modes.size() && !done; ++i) {
        void* session = Session(modes[i]);
        if (!session) {
            failure = GetLastError();
            stage = "WinHttpOpen";
        } else {
            done = Attempt(job, session, &result, &failure, &stage);
        }
        if (done) break;
        int abort;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            abort = job.abort;
        }
        const std::string line = std::string(kModeNames[(int)modes[i]]) + " " + stage + ": " + ErrorText(failure);
        history += (history.empty() ? "" : "; ") + line;
        if (abort != kNotAborted || Definitive(failure)) break;
        if (config_.log && i + 1 < modes.size()) config_.log("attempt failed, retrying: " + line);
    }

    int abort;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        abort = job.abort;
    }
    const std::string timeoutText = "timeout after " + std::to_string(job.request.options.timeoutMs / 1000) + "." +
                                    std::to_string(job.request.options.timeoutMs % 1000 / 100) + " s";
    if (abort == kTimedOut || (!done && failure == ERROR_WINHTTP_TIMEOUT)) {
        result = Result();
        result.error = timeoutText;
        result.code = ERROR_WINHTTP_TIMEOUT;
    } else if (abort != kNotAborted) {
        result = Result();
        result.error = "cancelled";
        result.code = ERROR_WINHTTP_OPERATION_CANCELLED;
    } else if (!done) {
        result = Result();
        result.code = (int)failure;
        result.error = history;
    }
    // A job discarded with DiscardTag is no longer in jobs_; finishing it is harmless.
    std::shared_ptr<Job> self;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = jobs_.find(job.id);
        if (it != jobs_.end()) self = it->second;
    }
    if (self) Finish(self, std::move(result));
}

} // namespace httpc
