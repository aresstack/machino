#include "app/http/http_server.hpp"
#include "app/compat/majestic_webui.hpp"
#include "app/http/http_parse.hpp"
#include "core/log.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace machino { namespace http {

static const char* MOD = "HTTP";
static const size_t MAX_IN = 16 * 1024;

static int64_t now_ms() { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000; }

// Sample the Linux side the majestic-webui Dashboard reads via /metrics. Every
// source is best-effort: a file that is missing (e.g. no thermal zone on this
// SoC) simply leaves its have_* flag false and the metric is omitted.
static compat::LinuxSample read_linux_sample() {
    compat::LinuxSample s;
    s.now_unix = (double)time(nullptr);
    if (FILE* f = fopen("/proc/uptime", "r")) {
        double up = 0; if (fscanf(f, "%lf", &up) == 1) { s.have_boot = true; s.boot_unix = s.now_unix - up; }
        fclose(f);
    }
    if (s.have_boot) {                        // process start -> app_boot (field 22 of /proc/self/stat, jiffies)
        if (FILE* f = fopen("/proc/self/stat", "r")) {
            std::string line; int ch; while ((ch = fgetc(f)) != EOF) line.push_back((char)ch); fclose(f);
            size_t rp = line.rfind(')');       // comm can contain spaces/parens; skip past it
            if (rp != std::string::npos) {
                std::istringstream rest(line.substr(rp + 1));
                std::string tok; int field = 2; unsigned long long starttime = 0;
                while (rest >> tok) { if (++field == 22) { starttime = strtoull(tok.c_str(), nullptr, 10); break; } }
                if (starttime) { s.have_app_boot = true; s.app_boot_unix = s.boot_unix + (double)starttime / 100.0; }
            }
        }
    }
    if (FILE* f = fopen("/proc/loadavg", "r")) {
        if (fscanf(f, "%lf %lf %lf", &s.load1, &s.load5, &s.load15) == 3) s.have_load = true;
        fclose(f);
    }
    if (FILE* f = fopen("/proc/meminfo", "r")) {
        char line[256]; unsigned long long kb;
        while (fgets(line, sizeof line, f)) {
            if      (sscanf(line, "MemTotal: %llu kB", &kb) == 1)        s.mem_total = kb * 1024ull;
            else if (sscanf(line, "MemFree: %llu kB", &kb) == 1)         s.mem_free = kb * 1024ull;
            else if (sscanf(line, "MemAvailable: %llu kB", &kb) == 1)    s.mem_avail = kb * 1024ull;
            else if (sscanf(line, "SReclaimable: %llu kB", &kb) == 1)    s.mem_sreclaim = kb * 1024ull;
            else if (sscanf(line, "Active(file): %llu kB", &kb) == 1)    s.mem_active_file = kb * 1024ull;
            else if (sscanf(line, "Inactive(file): %llu kB", &kb) == 1)  s.mem_inactive_file = kb * 1024ull;
        }
        fclose(f);
        s.have_mem = s.mem_total > 0;
    }
    if (FILE* f = fopen("/proc/stat", "r")) {
        char line[256];
        while (fgets(line, sizeof line, f)) {
            if (strncmp(line, "cpu", 3) != 0 || line[3] == ' ') continue;   // skip the aggregate "cpu " line
            compat::LinuxSample::Cpu c;
            unsigned long long u=0,n=0,sy=0,id=0,io=0,ir=0,so=0,st=0;
            if (sscanf(line, "cpu%d %llu %llu %llu %llu %llu %llu %llu %llu",
                       &c.index, &u, &n, &sy, &id, &io, &ir, &so, &st) >= 5) {
                c.user=u; c.nice=n; c.system=sy; c.idle=id; c.iowait=io; c.irq=ir; c.softirq=so; c.steal=st;
                s.cpus.push_back(c);
            }
        }
        fclose(f);
    }
    if (FILE* f = fopen("/proc/net/dev", "r")) {
        char line[512];
        while (fgets(line, sizeof line, f)) {
            char* colon = strchr(line, ':'); if (!colon) continue;
            *colon = 0; char dev[64];
            if (sscanf(line, " %63s", dev) != 1 || strcmp(dev, "lo") == 0) continue;
            unsigned long long fld[16] = {0};
            int got = sscanf(colon + 1, "%llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
                             &fld[0],&fld[1],&fld[2],&fld[3],&fld[4],&fld[5],&fld[6],&fld[7],
                             &fld[8],&fld[9],&fld[10],&fld[11],&fld[12],&fld[13],&fld[14],&fld[15]);
            if (got >= 9) { compat::LinuxSample::Net nn; nn.dev = dev; nn.rx = fld[0]; nn.tx = fld[8]; s.nets.push_back(nn); }
        }
        fclose(f);
    }
    if (FILE* f = fopen("/sys/class/thermal/thermal_zone0/temp", "r")) {
        long milli = 0; if (fscanf(f, "%ld", &milli) == 1 && milli > 0) { s.have_temp = true; s.temp_c = (double)milli / 1000.0; }
        fclose(f);
    }
    return s;
}

struct HttpServer::Client {
    int fd = -1;
    std::string peer;
    std::string in, out;
    bool sse = false;
    bool mjpeg = false;             // multipart/x-mixed-replace JPEG stream
    int64_t next_frame_ms = 0;      // mjpeg: earliest time for the next frame
    std::string last_jpeg;          // mjpeg: last frame sent, to skip duplicates (snapshot cache)
    bool close_after_flush = false;
    int64_t last_activity_ms = 0;
    std::shared_ptr<Subscription> sub;
    unsigned requests = 0;
};

static const char* MJPEG_BOUNDARY = "machinoframe";

HttpServer::HttpServer(const ServerConfig& cfg, api::ApiService& api, EventBus& bus) : cfg_(cfg), api_(api), bus_(bus) {
    if (cfg_.session_auth && cfg_.auth_check)
        gate_.reset(new SessionGate(cfg_.auth_check));
}
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

bool HttpServer::queue(Client& c, const std::string& data, size_t cap) {
    size_t limit = cap ? cap : cfg_.max_out_buffer;
    if (c.out.size() + data.size() > limit) { LOGW(MOD, "%s: output buffer overflow (%zu B, cap %zu) - dropping slow client", c.peer.c_str(), c.out.size(), limit); return false; }
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

    // Majestic drop-in session auth (see session.hpp for the exact webui
    // contract). Runs BEFORE every route, native or relayed.
    if (gate_) {
        const int64_t t = now_ms();
        if (path == "/login" && m == "POST") {
            SessionGate::LoginResult lr = gate_->login(req.body, t);
            LOGI(MOD, "%s: login %s", c.peer.c_str(), lr.status == 200 ? "ok" : "REJECTED");
            bool ok = queue(c, response(lr.status, "text/plain",
                                        lr.status == 200 ? "OK" : "Forbidden", req.keep_alive, lr.set_cookie));
            if (!req.keep_alive) c.close_after_flush = true;
            return ok;
        }
        if (path == "/logout" && m == "POST") {
            gate_->logout(req.header("cookie"));
            bool ok = queue(c, response(200, "text/plain", "OK", req.keep_alive, SessionGate::clear_cookie()));
            if (!req.keep_alive) c.close_after_flush = true;
            return ok;
        }
        if (!SessionGate::is_public(m, path) && !gate_->authed(req.header("cookie"), t)) {
            // Top-level navigation -> the login page; fetch()/assets -> 401
            // WITHOUT WWW-Authenticate (never the browser's Basic popup;
            // main.js redirects to /login.html on 401 itself).
            if (m == "GET" && req.header("accept").find("text/html") != std::string::npos) {
                bool ok = queue(c, response(302, "text/plain", "",
                                            false, "Location: /login.html?next=" + path + "\r\n"));
                c.close_after_flush = true;
                return ok;
            }
            bool ok = queue(c, response(401, "application/json",
                                        api::ApiService::error("unauthorized", path, "sign in required").dump(),
                                        req.keep_alive));
            if (!req.keep_alive) c.close_after_flush = true;
            return ok;
        }
    }
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
    } else if (path == "/api/v1/sources") {
        r = (m == "GET") ? api::Response{200, compat::majestic_sources(compat::majestic_config(api_.config().body, api_.state().body))}
                         : api::ApiService::fail(405, "unknown_field", path, "method not allowed");
    } else if (path == "/metrics") {
        if (m != "GET") { r = api::ApiService::fail(405, "unknown_field", path, "method not allowed"); }
        else {
            // majestic-webui heartbeat: node-exporter text from Linux + Machino
            // telemetry. This is what clears "Camera is not responding".
            std::string body = compat::majestic_metrics(api_.telemetry().body, api_.state().body, read_linux_sample());
            bool ok = queue(c, response(200, "text/plain; version=0.0.4", body, req.keep_alive));
            if (!req.keep_alive) c.close_after_flush = true;
            return ok;
        }
    } else if (path == "/api/v1/config") {
        if (m == "GET") r = api_.config();
        else if (m == "POST") {
            compat::MajesticTranslation t = compat::majestic_post_to_native(req.body);
            r = t.ok ? api_.patch_config(t.patch.dump(), "")
                     : api::ApiService::fail(t.status, t.code.c_str(), t.path, t.message);
        } else if (m == "PATCH" || m == "PUT") r = api_.patch_config(req.body, req.header("if-match"));
        else r = api::ApiService::fail(405, "unknown_field", path, "method not allowed");
    } else if (path == "/api/v1/stream.mjpeg" || path == "/stream.mjpeg" || path == "/stream") {
        if (m != "GET") { r = api::ApiService::fail(405, "unknown_field", path, "method not allowed"); }
        else {
            c.mjpeg = true; c.next_frame_ms = 0;                 // first frame as soon as possible
            queue(c, mjpeg_headers(MJPEG_BOUNDARY));
            LOGI(MOD, "%s: MJPEG stream started", c.peer.c_str());
            return true;
        }
    } else if (path == "/snapshot" || path == "/snapshot.jpg" || path == "/api/v1/snapshot") {
        if (m != "GET") { r = api::ApiService::fail(405, "unknown_field", path, "method not allowed"); }
        else {
            std::vector<uint8_t> jpg; std::string serr;
            // 2s bound: one-shot request, but still inside the single poll
            // loop - never let it hang the server for the full default wait.
            Result sr = api_.snapshot(jpg, serr, 2000);
            if (sr) {
                std::string body(reinterpret_cast<const char*>(jpg.data()), jpg.size());
                bool ok = queue(c, response(200, "image/jpeg", body, req.keep_alive), cfg_.max_snapshot_bytes);
                if (!req.keep_alive) c.close_after_flush = true;
                return ok;
            }
            int code = sr.status == Status::Unsupported ? 501 : 503;
            r = api::ApiService::fail(code, "unavailable", path, serr.empty() ? "snapshot failed" : serr);
        }
    } else if (cfg_.upstream_port > 0) {
        // Front-door: not a native route -> hand it to the internal OpenIPC
        // WebUI (busybox httpd). The stock UI never learns Machino exists.
        return relay_upstream(c, req);
    } else r = api::ApiService::fail(404, "unknown_field", path, "unknown endpoint");

    std::string body = r.body.dump();
    LOGD(MOD, "%s %s -> %d (%zu B)", m.c_str(), path.c_str(), r.status, body.size());
    if (!queue(c, response(r.status, "application/json", body, req.keep_alive))) return false;
    if (!req.keep_alive) c.close_after_flush = true;
    return true;
}

// Forward one request to the internal OpenIPC WebUI (busybox httpd) and queue
// its verbatim response. Blocking, but bounded by relay_timeout_ms and
// max_relay_bytes so a hung/oversized upstream can not wedge the poll loop
// (same discipline as the JPEG path). The connection is closed afterwards:
// the upstream reply is HTTP/1.0/EOF-delimited, so one request per socket.
bool HttpServer::relay_upstream(Client& c, const Request& req) {
    c.close_after_flush = true;
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) { queue(c, response(502, "text/plain", "upstream socket failed\n", false)); return true; }
    struct timeval tv; tv.tv_sec = cfg_.relay_timeout_ms / 1000; tv.tv_usec = (cfg_.relay_timeout_ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons((uint16_t)cfg_.upstream_port);
    if (inet_pton(AF_INET, cfg_.upstream_host.c_str(), &a.sin_addr) != 1 ||
        connect(fd, (sockaddr*)&a, sizeof a) < 0) {
        LOGW(MOD, "relay: connect %s:%d failed: %s", cfg_.upstream_host.c_str(), cfg_.upstream_port, strerror(errno));
        close(fd);
        queue(c, response(502, "text/plain", "OpenIPC WebUI backend unreachable\n", false));
        return true;
    }
    const std::string wire = forward_request(req, cfg_.upstream_host);
    for (size_t off = 0; off < wire.size(); ) {
        ssize_t w = send(fd, wire.data() + off, wire.size() - off, MSG_NOSIGNAL);
        if (w <= 0) { close(fd); queue(c, response(502, "text/plain", "upstream write failed\n", false)); return true; }
        off += (size_t)w;
    }
    std::string resp; char buf[8192];
    for (;;) {
        ssize_t rd = recv(fd, buf, sizeof buf, 0);
        if (rd == 0) break;                  // EOF: upstream done (Connection: close)
        if (rd < 0) { LOGW(MOD, "relay: read %s: %s", req.path.c_str(), strerror(errno));
            if (resp.empty()) { close(fd); queue(c, response(504, "text/plain", "OpenIPC WebUI backend timed out\n", false)); return true; }
            break; }
        resp.append(buf, (size_t)rd);
        if (resp.size() > cfg_.max_relay_bytes) { LOGW(MOD, "relay: %s response too large", req.path.c_str()); break; }
    }
    close(fd);
    LOGD(MOD, "relay %s %s -> %zu B", req.method.c_str(), req.path.c_str(), resp.size());
    // The upstream reply is a complete HTTP response already; forward verbatim.
    return queue(c, resp, cfg_.max_relay_bytes + 4096);
}

void HttpServer::drain_events(Client& c) {
    if (!c.sub) return;
    if (c.sub->overflowed()) { LOGW(MOD, "%s: SSE subscription overflowed - dropping slow client", c.peer.c_str()); c.close_after_flush = true; c.out.clear(); return; }
    Event e;
    while (c.sub->pop(e)) if (!queue(c, sse_event(e.type, e.data))) { c.close_after_flush = true; c.out.clear(); return; }
}

// One JPEG frame per rate-limited tick, and only when the previous frame has
// fully flushed (backpressure): a slow browser drops frames instead of growing
// the buffer, and the media path is never blocked. No jpeg -> end the stream.
void HttpServer::push_mjpeg(Client& c) {
    const int64_t t = now_ms();
    const int interval = cfg_.mjpeg_max_fps > 0 ? 1000 / cfg_.mjpeg_max_fps : 100;
    if (t < c.next_frame_ms || !c.out.empty()) return;
    std::vector<uint8_t> jpg; std::string err;
    // SHORT capture bound: this runs in the single poll loop, so a frame that
    // is not ready within 300ms must not stall every other client - skip this
    // tick and try again on the next one. Only real failures end the stream.
    Result sr = api_.snapshot(jpg, err, 300);
    if (sr.status == Status::Timeout) { c.next_frame_ms = t + interval; return; }
    if (!sr) {
        LOGW(MOD, "%s: MJPEG ending - no JPEG (%s)", c.peer.c_str(), err.empty() ? "unavailable" : err.c_str());
        c.close_after_flush = true;
        return;
    }
    c.next_frame_ms = t + interval;
    // Skip unchanged frames: the snapshot cache (cache_ms) hands out the same
    // JPEG to calls within its window, so this avoids re-sending duplicates.
    std::string data(reinterpret_cast<const char*>(jpg.data()), jpg.size());
    if (data == c.last_jpeg) return;
    c.last_jpeg.swap(data);
    if (!queue(c, mjpeg_frame(MJPEG_BOUNDARY, jpg.data(), jpg.size()), cfg_.max_snapshot_bytes + 256)) {
        c.close_after_flush = true; c.out.clear(); return;
    }
}

void HttpServer::loop() {
    std::vector<pollfd> pfds;
    last_telemetry_ms_ = last_heartbeat_ms_ = now_ms();
    while (!quit_) {
        pfds.clear();
        pfds.push_back({listen_fd_, POLLIN, 0});
        int timeout_ms = 250;
        for (auto& c : clients_) pfds.push_back({c->fd, (short)(POLLIN | (c->out.empty() ? 0 : POLLOUT)), 0});
        for (auto& c : clients_) if (c->mjpeg) { timeout_ms = 40; break; }   // tick fast enough for the frame rate
        int n = poll(pfds.data(), pfds.size(), timeout_ms);
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
                else if (c.sse || c.mjpeg) { /* ignore input on streaming connections */ }
                else { c.in.append(buf, (size_t)r); if (c.in.size() > MAX_IN) ok = false; else while (ok && !c.close_after_flush && c.in.find("\r\n\r\n") != std::string::npos) { size_t before = c.in.size(); ok = handle_request(c); if (c.in.size() == before) break; } }
            }
            if (ok && c.sse) {
                drain_events(c);
                if (t - last_heartbeat_ms_ >= 15000) queue(c, ": keepalive\n\n");
            }
            if (ok && c.mjpeg && !c.close_after_flush) push_mjpeg(c);
            if (ok) ok = flush(c);
            if (ok && c.close_after_flush && c.out.empty()) ok = false;
            if (ok && !c.sse && !c.mjpeg && t - c.last_activity_ms > cfg_.idle_timeout_ms) ok = false;
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
