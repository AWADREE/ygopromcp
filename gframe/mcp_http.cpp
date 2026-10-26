#include "mcp.h"

#ifdef _WIN32
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <bcrypt.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace ygo {
namespace {
using json = nlohmann::json;
constexpr size_t kHeaderLimit = 16384;
constexpr size_t kBodyLimit = 1048576;
constexpr unsigned kWorkers = 4;
// Three GUI jobs may wait for a prompt; one worker remains available for
// cancellation notifications, status errors and quick rejections.
constexpr unsigned kMaxActiveRequests = kWorkers - 1;
std::mutex g_http_mutex;
SOCKET g_listener = INVALID_SOCKET;
std::array<SOCKET, kWorkers> g_clients{};
std::vector<std::thread> g_workers;
std::atomic<bool> g_http_running{false};
std::atomic<bool> g_initialized{false};
std::atomic<bool> g_initializing{false};
std::atomic<unsigned> g_socket_count{0};
std::atomic<unsigned> g_active_requests{0};
std::string g_endpoint;
std::string g_session_id;
std::string g_failure;
uint16_t g_port = 0;

std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}
std::string Trim(const std::string& s) {
    const auto first = s.find_first_not_of(" \t");
    if (first == std::string::npos) return "";
    const auto last = s.find_last_not_of(" \t");
    return s.substr(first, last - first + 1);
}
std::string RpcError(const json& id, int code, const char* message) {
    return json{{"jsonrpc", "2.0"}, {"id", id},
                {"error", {{"code", code}, {"message", message}}}}.dump();
}
bool SendAll(SOCKET s, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        const int n = send(s, data.data() + sent,
                           static_cast<int>(std::min<size_t>(data.size() - sent, 16384)), 0);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}
void Reply(SOCKET s, int status, const char* phrase, const std::string& body = "",
           const std::string& extra = "") {
    std::ostringstream out;
    out << "HTTP/1.1 " << status << ' ' << phrase << "\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Connection: close\r\n"
        << "Cache-Control: no-store\r\n";
    if (!body.empty()) out << "Content-Type: application/json; charset=utf-8\r\n";
    out << extra << "\r\n" << body;
    SendAll(s, out.str());
}
bool ConnectionAlive(SOCKET s) {
    if (!g_http_running) return false;
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(s, &readfds);
    timeval zero{};
    const int ready = select(0, &readfds, nullptr, nullptr, &zero);
    if (ready < 0) return false;
    if (ready == 0) return true;
    char c;
    // Any additional bytes after a complete, length-delimited request are
    // unsupported pipelining. EOF is a cancelled request.
    (void)recv(s, &c, 1, MSG_PEEK);
    return false;
}
bool Token(std::string& out) {
    unsigned char bytes[24]{};
    if (BCryptGenRandom(nullptr, bytes, sizeof(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        return false;
    static const char hex[] = "0123456789abcdef";
    out.clear();
    for (unsigned char b : bytes) { out.push_back(hex[b >> 4]); out.push_back(hex[b & 15]); }
    return true;
}
void HandleClient(SOCKET s) {
    DWORD timeout = 5000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    std::string wire;
    char chunk[4096];
    size_t end = std::string::npos;
    while (g_http_running && (end = wire.find("\r\n\r\n")) == std::string::npos) {
        if (wire.size() > kHeaderLimit) { Reply(s, 431, "Request Header Fields Too Large"); return; }
        const int n = recv(s, chunk, sizeof(chunk), 0);
        if (n <= 0) return;
        wire.append(chunk, static_cast<size_t>(n));
    }
    if (!g_http_running) return;
    if (end > kHeaderLimit) { Reply(s, 431, "Request Header Fields Too Large"); return; }
    std::istringstream lines(wire.substr(0, end));
    std::string first;
    if (!std::getline(lines, first)) { Reply(s, 400, "Bad Request"); return; }
    if (!first.empty() && first.back() == '\r') first.pop_back();
    std::istringstream request_line(first);
    std::string verb, path, version, trailing;
    if (!(request_line >> verb >> path >> version) || (request_line >> trailing) ||
        version != "HTTP/1.1") { Reply(s, 400, "Bad Request"); return; }
    std::map<std::string, std::string> headers;
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto colon = line.find(':');
        if (colon == std::string::npos) { Reply(s, 400, "Bad Request"); return; }
        auto key = Lower(Trim(line.substr(0, colon)));
        if (key.empty() || headers.count(key)) { Reply(s, 400, "Bad Request"); return; }
        headers.emplace(std::move(key), Trim(line.substr(colon + 1)));
    }
    const std::string expected_host = "127.0.0.1:" + std::to_string(g_port);
    if (verb == "GET") {
        if (!headers.count("host") || Lower(headers["host"]) != expected_host ||
            headers.count("origin")) { Reply(s, 403, "Forbidden"); return; }
        if (path != g_endpoint.substr(g_endpoint.find('/', 7))) {
            Reply(s, 404, "Not Found"); return;
        }
        Reply(s, 405, "Method Not Allowed", "", "Allow: POST\r\n"); return;
    }
    if (verb != "POST") { Reply(s, 405, "Method Not Allowed", "", "Allow: POST\r\n"); return; }
    if (!headers.count("content-type") ||
        Lower(headers["content-type"]).find("application/json") != 0 ||
        !headers.count("accept") ||
        headers["accept"].find("application/json") == std::string::npos ||
        headers["accept"].find("text/event-stream") == std::string::npos ||
        !headers.count("content-length")) { Reply(s, 400, "Bad Request"); return; }
    size_t length = 0;
    try {
        const auto& value = headers["content-length"];
        if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos)
            throw 1;
        length = static_cast<size_t>(std::stoull(value));
    } catch (...) { Reply(s, 400, "Bad Request"); return; }
    if (length == 0 || length > kBodyLimit || wire.size() - end - 4 > length) {
        Reply(s, 413, "Payload Too Large"); return;
    }
    std::string body = wire.substr(end + 4);
    while (g_http_running && body.size() < length) {
        const int n = recv(s, chunk, static_cast<int>(std::min<size_t>(sizeof(chunk), length - body.size())), 0);
        if (n <= 0) return;
        body.append(chunk, static_cast<size_t>(n));
    }
    if (!g_http_running) return;
    // Consume the bounded body before rejecting an authenticated POST. On
    // Windows, closing a socket with unread request bytes can reset it before
    // the client receives the intended 403/404 response.
    if (!headers.count("host") || Lower(headers["host"]) != expected_host ||
        headers.count("origin")) { Reply(s, 403, "Forbidden"); return; }
    if (path != g_endpoint.substr(g_endpoint.find('/', 7))) {
        Reply(s, 404, "Not Found"); return;
    }
    json req;
    try { req = json::parse(body); }
    catch (...) { Reply(s, 400, "Bad Request", RpcError(nullptr, -32700, "Parse error")); return; }
    if (!req.is_object() || !req.contains("method") || !req["method"].is_string()) {
        Reply(s, 400, "Bad Request", RpcError(nullptr, -32600, "Invalid Request")); return;
    }
    const std::string method = req["method"].get<std::string>();
    if (method != "initialize" &&
        (!headers.count("mcp-protocol-version") ||
         headers["mcp-protocol-version"] != "2025-11-25")) {
        Reply(s, 400, "Bad Request", RpcError(req.value("id", json(nullptr)), -32600,
                                                "MCP-Protocol-Version must be 2025-11-25"));
        return;
    }
    if (method == "initialize") {
        if (!req.contains("params") || !req["params"].is_object() ||
            !req["params"].contains("protocolVersion") ||
            !req["params"]["protocolVersion"].is_string()) {
            Reply(s, 400, "Bad Request", RpcError(req.value("id", json(nullptr)), -32600,
                                                    "Invalid initialize parameters"));
            return;
        }
    } else {
        if (!g_initialized) {
            Reply(s, 400, "Bad Request", RpcError(req.value("id", json(nullptr)), -32000,
                                                    "Initialize first"));
            return;
        }
        // The temporary URL admits one native MCP client session. A second
        // client cannot use that session's request IDs or cancel its jobs.
        if (!headers.count("mcp-session-id") || headers["mcp-session-id"] != g_session_id) {
            Reply(s, 403, "Forbidden", RpcError(req.value("id", json(nullptr)), -32000,
                                                  "MCP session is not active"));
            return;
        }
    }
    if (!req.contains("id")) {
        if (method == "notifications/cancelled" && req.contains("params") &&
            req["params"].is_object() && req["params"].contains("requestId")) {
            MCP_CancelHttpRequest(req["params"]["requestId"].dump());
        } else if (method != "notifications/initialized") {
            Reply(s, 400, "Bad Request"); return;
        }
        Reply(s, 202, "Accepted");
        return;
    }
    const bool initializing = method == "initialize";
    if (initializing && (g_initialized || g_initializing.exchange(true))) {
        Reply(s, 409, "Conflict", RpcError(req["id"], -32000,
                                           "Another MCP client session is active"));
        return;
    }
    const unsigned previous = g_active_requests.fetch_add(1);
    if (previous >= kMaxActiveRequests) {
        --g_active_requests;
        if (initializing) g_initializing = false;
        Reply(s, 503, "Service Unavailable", RpcError(req["id"], -32000,
                                                     "Local MCP request capacity is full"));
        return;
    }
    struct ActiveRequestScope { ~ActiveRequestScope() { --g_active_requests; } } active_scope;
    const std::string answer = MCP_InvokeHttp(body, [s] { return ConnectionAlive(s); });
    std::string session_header;
    if (initializing) {
        try {
            if (g_http_running && !answer.empty() && json::parse(answer).contains("result")) {
                g_initialized = true;
                session_header = "Mcp-Session-Id: " + g_session_id + "\r\n";
            }
        } catch (...) {}
        g_initializing = false;
    }
    if (!g_http_running || answer.empty()) return;
    Reply(s, 200, "OK", answer, session_header);
}
void FailListener(int code) {
    if (!g_http_running.exchange(false)) return;
    {
        std::lock_guard<std::mutex> lk(g_http_mutex);
        g_failure = "Local MCP listener stopped unexpectedly (socket error " +
                    std::to_string(code) + ")";
        if (g_listener != INVALID_SOCKET) { closesocket(g_listener); g_listener = INVALID_SOCKET; }
        for (SOCKET client : g_clients) if (client != INVALID_SOCKET) shutdown(client, SD_BOTH);
    }
    MCP_CancelAllHttpRequests();
}
void Worker(unsigned index) {
    while (g_http_running) {
        SOCKET listener;
        { std::lock_guard<std::mutex> lk(g_http_mutex); listener = g_listener; }
        if (listener == INVALID_SOCKET) break;
        SOCKET s = accept(listener, nullptr, nullptr);
        if (s == INVALID_SOCKET) {
            if (g_http_running) FailListener(WSAGetLastError());
            break;
        }
        { std::lock_guard<std::mutex> lk(g_http_mutex); g_clients[index] = s; }
        ++g_socket_count;
        HandleClient(s);
        { std::lock_guard<std::mutex> lk(g_http_mutex); g_clients[index] = INVALID_SOCKET; }
        --g_socket_count;
        shutdown(s, SD_BOTH);
        closesocket(s);
    }
}
} // namespace

bool MCP_HTTP_Start(uint16_t port, std::string& error) {
    if (!MCP_BridgeIsReady()) { error = "Game MCP bridge is unavailable"; return false; }
    if (g_http_running) { error = "MCP is already listening"; return false; }
    if (!g_workers.empty()) { error = "Previous MCP listener is still stopping"; return false; }
    if (port == 0) { error = "Enter a port from 1 to 65535"; return false; }
    std::string token, session;
    if (!Token(token) || !Token(session)) {
        error = "Could not create temporary MCP access tokens"; return false;
    }
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) { error = "Could not create loopback socket"; return false; }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR ||
        listen(s, static_cast<int>(kWorkers)) == SOCKET_ERROR) {
        const int code = WSAGetLastError();
        closesocket(s);
        error = "Could not listen on 127.0.0.1:" + std::to_string(port) +
                " (socket error " + std::to_string(code) + ")";
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(g_http_mutex);
        g_listener = s;
        g_clients.fill(INVALID_SOCKET);
        g_port = port;
        g_endpoint = "http://127.0.0.1:" + std::to_string(port) + "/mcp/" + token;
        g_session_id = session;
        g_failure.clear();
    }
    g_initialized = false;
    g_initializing = false;
    g_active_requests = 0;
    g_http_running = true;
    try {
        for (unsigned i = 0; i < kWorkers; ++i) g_workers.emplace_back([i] { Worker(i); });
    } catch (...) {
        MCP_HTTP_Stop();
        error = "Could not start local MCP worker threads";
        return false;
    }
    error.clear();
    return true;
}
void MCP_HTTP_Stop() {
    g_http_running = false;
    bool listener_open;
    {
        std::lock_guard<std::mutex> lk(g_http_mutex);
        listener_open = g_listener != INVALID_SOCKET;
    }
    if (g_workers.empty() && !listener_open) return;
    MCP_CancelAllHttpRequests();
    {
        std::lock_guard<std::mutex> lk(g_http_mutex);
        if (g_listener != INVALID_SOCKET) { closesocket(g_listener); g_listener = INVALID_SOCKET; }
        for (SOCKET s : g_clients) if (s != INVALID_SOCKET) shutdown(s, SD_BOTH);
    }
    for (auto& worker : g_workers) if (worker.joinable()) worker.join();
    g_workers.clear();
    std::lock_guard<std::mutex> lk(g_http_mutex);
    g_endpoint.clear();
    g_session_id.clear();
    g_port = 0;
    g_initialized = false;
    g_initializing = false;
}
bool MCP_HTTP_IsRunning() { return g_http_running; }
unsigned MCP_HTTP_ActiveRequestCount() { return g_active_requests; }
std::string MCP_HTTP_Endpoint() {
    std::lock_guard<std::mutex> lk(g_http_mutex);
    return g_endpoint;
}
std::string MCP_HTTP_TakeFailure() {
    std::lock_guard<std::mutex> lk(g_http_mutex);
    std::string error;
    error.swap(g_failure);
    return error;
}
#ifdef YGOPROMCP_HTTP_PROBE
void MCP_HTTP_ProbeBreakListener() {
    std::lock_guard<std::mutex> lk(g_http_mutex);
    if (g_listener != INVALID_SOCKET) {
        closesocket(g_listener);
        g_listener = INVALID_SOCKET;
    }
}
#endif
} // namespace ygo

#else
namespace ygo {
bool MCP_HTTP_Start(uint16_t, std::string& error) {
    error = "Local HTTP MCP is not available on this build"; return false;
}
void MCP_HTTP_Stop() {}
bool MCP_HTTP_IsRunning() { return false; }
unsigned MCP_HTTP_ActiveRequestCount() { return 0; }
std::string MCP_HTTP_Endpoint() { return {}; }
std::string MCP_HTTP_TakeFailure() { return {}; }
#ifdef YGOPROMCP_HTTP_PROBE
void MCP_HTTP_ProbeBreakListener() {}
#endif
} // namespace ygo
#endif
