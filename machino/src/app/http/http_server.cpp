#include "app/http/http_server.hpp"
#include "app/compat/majestic_webui.hpp"
#include "app/http/http_parse.hpp"
#include "core/log.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace machino { namespace http {

static const char* MOD = "HTTP";
static const size_t MAX_IN = 16 * 1024;

static int64_t now_ms() { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000; }

struct HttpServer::Client {
    int fd = -1;
    std::string peer;
    std::string in, out;
    bool sse = false;
    bool close_after_flush = false;
    int64_t last_activity_ms = 0;
    std::shared_ptr<Subscription> sub;
    unsigned requests = 0;
};

HttpServer::HttpServer(const ServerConfig& cfg, api::ApiService& api, EventBus& bus) : cfg_(cfg), api_(api), bus_(bus) {}
HttpServer::~HttpServer() { stop(); }

Result HttpServer::start() {
    listen_fd_ = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (listen_fd_ < 0) return Result::error(errno);
    int one = 1; setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons((uint16_t)cfg_.port);
    if (inet_pton(AF_INET, cfg_.bind.c_str(), &a.sin_addr) != 1) { close(listen_fd_); listen_fd_ = -1; LOGE(MOD, "invalid bind address '%s'", cfg_.bind.c_str()); return Result::error(EINVAL); }
    if (bind(listen_fd_, (sockaddr*)&a, sizeof a) < 0 || listen(listen_fd_, 8) < 0) {
        int e = errno; LOGE(MOD, "bind/listen %s:%d failed: %s", cfg_.bind.c_str(), cfg_.port, strerror(e));
        close(listen_fd_); listen_fd_ = -1; return Result::error(e);
    }
    quit_ = false;
    thread_ = std::thread([this] { loop(); });
    LOGI(MOD, "API listening on http://%s:%d/api/v1 (max %d clients; reads are not media demand)", cfg_.bind.c_str(), cfg_.port, cfg_.max_clients);
    return Result::ok();
}

void HttpServer::stop() {
    if (listen_fd_ < 0) return;
    quit_ = true;
    if (thread_.joinable()) thread_.join();
    for (auto& c : clients_) { if (c->sub) bus_.unsubscribe(c->sub); close(c->fd); }
    clients_.clear();
    close(listen_fd_); listen_fd_ = -1;
    LOGI(MOD, "stopped");
}

void HttpServer::accept_client() {
    sockaddr_in ca{}; socklen_t cl = sizeof ca;
    int fd = accept4(listen_fd_, (sockaddr*)&ca, &cl, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (fd < 0) return;
    if ((int)clients_.size() >= cfg_.max_clients) {
        std::string r = response(503, "application/json", api::ApiService::error("unavailable", "", "too many clients").dump(), false);
        send(fd, r.data(), r.size(), MSG_NOSIGNAL | MSG_DONTWAIT); close(fd); return;
    }
    int one = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    auto c = std::make_unique<Client>();
    c->fd = fd; c->last_activity_ms = now_ms();
    char ip[INET_ADDRSTRLEN]; inet_ntop(AF_INET, &ca.sin_addr, ip, sizeof ip);
    c->peer = std::string(ip) + ":" + std::to_string(ntohs(ca.sin_port));
    clients_.push_back(std::move(c));
}

bool HttpServer::queue(Client& c, const std::string& data) {
    if (c.out.size() + data.size() > cfg_.max_out_buffer) { LOGW(MOD, "%s: output buffer overflow (%zu B) - dropping slow client", c.peer.c_str(), c.out.size()); return false; }
    c.out += data;
    return true;
}

bool HttpServer::flush(Client& c) {
    while (!c.out.empty()) {
        ssize_t w = send(c.fd, c.out.data(), c.out.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
        if (w < 0) { if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return true; return false; }
        if (w == 0) return false;
        c.out.erase(0, (size_t)w);
    }
    return true;
}

// Dispatches one complete request; returns false to close the connection.
bool HttpServer::handle_request(Client& c) {
    size_t consumed = 0; Request req;
    Parse p = parse_request(c.in, consumed, req);
    if (p == Parse::Incomplete) return c.in.size() <= MAX_IN;
    if (p == Parse::TooLarge) { queue(c, response(413, "application/json", api::ApiService::error("invalid_value", "", "request too large").dump(), false)); c.close_after_flush = true; return true; }
    if (p == Parse::Bad)      { queue(c, response(400, "application/json", api::ApiService::error("invalid_json", "", "malformed HTTP request").dump(), false)); c.close_after_flush = true; return true; }
    c.in.erase(0, consumed); ++c.requests; c.last_activity_ms = now_ms();

    const std::string& path = req.path; const std::string& m = req.method;
    api::Response r;
    if (m == "OPTIONS") { queue(c, response(204, "text/plain", "", req.keep_alive, "Access-Control-Allow-Methods: GET, POST, PUT, PATCH, OPTIONS\r\nAccess-Control-Allow-Headers: Content-Type, If-Match\r\n")); if (!req.keep_alive) c.close_after_flush = true; return true; }
    if (path == "/api/v1/events") {
        if (m != "GET") { r = api::ApiService::fail(405, "unknown_field", path, "method not allowed"); }
        else {
            c.sse = true; c.sub = bus_.subscribe(64);
            queue(c, sse_headers());
            queue(c, sse_event("state", api_.state().body.dump()));         // initial snapshot
            LOGI(MOD, "%s: SSE subscribed (%zu subscribers)", c.peer.c_str(), bus_.subscribers());
            return true;
        }
    } else if (path == "/api/v1" || path == "/api/v1/") { r = (m == "GET") ? api_.discovery() : api::ApiService::fail(405, "unknown_field", path, "method not allowed"); }
    else if (path == "/api/v1/capabilities") { r = (m == "GET") ? api_.capabilities() : api::ApiService::fail(405, "unknown_field", path, "method not allowed"); }
    else if (path == "/api/v1/state")        { r = (m == "GET") ? api_.state() : api::ApiService::fail(405, "unknown_field", path, "method not allowed"); }
    else if (path == "/api/v1/telemetry")    { r = (m == "GET") ? api_.telemetry() : api::ApiService::fail(405, "unknown_field", path, "method not allowed"); }
    else if (path == "/api/v1/config.schema.json") {
        r = (m == "GET") ? api::Response{200, compat::majestic_schema(api_.capabilities().body)}
                         : api::ApiService::fail(405, "unknown_field", path, "method not allowed");
    } else if (path == "/api/v1/config.json") {
        r = (m == "GET") ? api::Response{200, compat::majestic_config(api_.config().body, api_.state().body)}
                         : api::ApiService::fail(405, "unknown_field", path, "method not allowed");
    } else if (path == "/api/v1/config") {
        if (m == "GET") r = api_.config();
        else if (m == "POST") {
            compat::MajesticTranslation t = compat::majestic_post_to_native(req.body);
            r = t.ok ? api_.patch_config(t.patch.dump(), "")
                     : api::ApiService::fail(t.status, t.code.c_str(), t.path, t.message);
        } else if (m == "PATCH" || m == "PUT") r = api_.patch_config(req.body, req.header("if-match"));
        else r = api::ApiService::fail(405, "unknown_field", path, "method not allowed");
    } else r = api::ApiService::fail(404, "unknown_field", path, "unknown endpoint");

    std::string body = r.body.dump();
    LOGD(MOD, "%s %s -> %d (%zu B)", m.c_str(), path.c_str(), r.status, body.size());
    if (!queue(c, response(r.status, "application/json", body, req.keep_alive))) return false;
    if (!req.keep_alive) c.close_after_flush = true;
    return true;
}

void HttpServer::drain_events(Client& c) {
    if (!c.sub) return;
    if (c.sub->overflowed()) { LOGW(MOD, "%s: SSE subscription overflowed - dropping slow client", c.peer.c_str()); c.close_after_flush = true; c.out.clear(); return; }
    Event e;
    while (c.sub->pop(e)) if (!queue(c, sse_event(e.type, e.data))) { c.close_after_flush = true; c.out.clear(); return; }
}

void HttpServer::loop() {
    std::vector<pollfd> pfds;
    last_telemetry_ms_ = last_heartbeat_ms_ = now_ms();
    while (!quit_) {
        pfds.clear();
        pfds.push_back({listen_fd_, POLLIN, 0});
        for (auto& c : clients_) pfds.push_back({c->fd, (short)(POLLIN | (c->out.empty() ? 0 : POLLOUT)), 0});
        int n = poll(pfds.data(), pfds.size(), 250);
        int64_t t = now_ms();
        if (n > 0 && (pfds[0].revents & POLLIN)) accept_client();
        for (size_t i = 0; i < clients_.size(); ++i) {
            Client& c = *clients_[i];
            short re = (i + 1 < pfds.size()) ? pfds[i + 1].revents : 0;
            bool ok = true;
            if (re & (POLLHUP | POLLERR | POLLNVAL)) ok = false;
            else if (re & POLLIN) {
                char buf[4096]; ssize_t r = recv(c.fd, buf, sizeof buf, MSG_DONTWAIT);
                if (r == 0) ok = false;
                else if (r < 0) { if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) ok = false; }
                else if (c.sse) { /* ignore input on SSE connections */ }
                else { c.in.append(buf, (size_t)r); if (c.in.size() > MAX_IN) ok = false; else while (ok && !c.close_after_flush && c.in.find("\r\n\r\n") != std::string::npos) { size_t before = c.in.size(); ok = handle_request(c); if (c.in.size() == before) break; } }
            }
            if (ok && c.sse) {
                drain_events(c);
                if (t - last_heartbeat_ms_ >= 15000) queue(c, ": keepalive\n\n");
            }
            if (ok) ok = flush(c);
            if (ok && c.close_after_flush && c.out.empty()) ok = false;
            if (ok && !c.sse && t - c.last_activity_ms > cfg_.idle_timeout_ms) ok = false;
            if (!ok) {
                if (c.sub) bus_.unsubscribe(c.sub);
                close(c.fd);
                clients_.erase(clients_.begin() + (long)i); --i;
            }
        }
        if (t - last_heartbeat_ms_ >= 15000) last_heartbeat_ms_ = t;
        // 1 Hz telemetry only while somebody listens (cheap otherwise)
        bool any_sse = false; for (auto& c : clients_) if (c->sse) { any_sse = true; break; }
        if (any_sse && t - last_telemetry_ms_ >= cfg_.telemetry_interval_ms) {
            last_telemetry_ms_ = t;
            bus_.publish("telemetry", api_.telemetry_json().dump());
        }
    }
}

}} // namespace machino::http
