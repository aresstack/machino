#include "app/http/http_server.hpp"
#include "app/http/chrome.hpp"
#include "app/http/netui.hpp"
#include "app/http/devui.hpp"
#include "app/compat/majestic_webui.hpp"
#include "app/webrtc/peer.hpp"
#include "app/http/fmp4.hpp"
#include "app/http/http_parse.hpp"
#include "app/http/websocket.hpp"
#include "app/rtsp/h264_nal.hpp"
#include "core/log.hpp"
#include "core/runtime_stats.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <time.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sstream>
#include <string>
#include <csignal>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <unistd.h>

namespace machino { namespace http {

static const char* MOD = "HTTP";
static const size_t MAX_IN = 16 * 1024;

// A logo upload is the one request whose body legitimately exceeds the 16 KiB
// working buffer: the stock settings page posts raw BGRA pixels. The allowance
// is granted from the REQUEST LINE, so it applies to that one route and no
// other request inherits it - and the service still validates the body against
// the declared w*h*4 afterwards, so this is a buffer bound, not a trust grant.
static size_t input_cap(const std::string& in) {
    static const char OSD_POST[] = "POST /api/v1/osd/image";
    if (in.compare(0, sizeof(OSD_POST) - 1, OSD_POST) == 0)
        return osd::OsdService::MAX_IMAGE_BYTES + 4096;
    // An ONVIF request is a SOAP document; the scanner's own bound is what
    // decides how big one may be, and it refuses anything past it.
    static const char ONVIF_POST[] = "POST /onvif/";
    if (in.compare(0, sizeof(ONVIF_POST) - 1, ONVIF_POST) == 0)
        return onvif::MAX_REQUEST + 4096;
    return MAX_IN;
}

// atoi() on a value outside int range is undefined behaviour, and these values
// come straight off the query string. The service range-checks the result
// anyway, so the consequence was bounded - but the UB should not be there.
static int query_int(const std::string& s, int fallback) {
    if (s.empty()) return fallback;
    errno = 0;
    char* end = nullptr;
    const long v = strtol(s.c_str(), &end, 10);
    if (errno == ERANGE || !end || *end != 0) return fallback;
    if (v < -2147483647L - 1 || v > 2147483647L) return fallback;
    return (int)v;
}

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
    bool ws_logs = false;           // /ws/logs subscriber (shared logread feed)
    // /ws/video: one live MSE feed = one StreamHub consumer with its own
    // demand, exactly like an RTSP session (no second encoder, no JPEG).
    bool ws_video = false;
    int  ws_unit = 0;               // lifecycle unit this viewer watches (main/sub)
    StreamHub* ws_hub = nullptr;    // the hub ws_sink came from (for unsubscribe)
    std::shared_ptr<Sink>    ws_sink;
    lifecycle::DemandHandle  ws_demand;
    bool ws_init_sent = false;
    bool ws_await_key = true;       // never hand the decoder a P-frame without its reference
    std::vector<uint8_t> ws_sps, ws_pps;
    uint32_t ws_seq = 1;
    uint64_t ws_dts = 0;            // 90 kHz decode timeline
    // AP15: the decode timeline. Derived from the capture clock, not summed
    // from per-fragment durations - see fmp4::Timeline for why, and for the
    // host tests that hold it to that.
    fmp4::Timeline ws_timeline;
    int64_t  ws_last_idr_req_ms = 0;
    // /ws/webrtc: the signalling WebSocket owns one PeerSession (UDP socket
    // in the same poll loop) and, like /ws/video, is a StreamHub consumer
    // with its OWN demand; PLI maps onto the existing on-demand IDR.
    bool rtc_ws = false;
    int  rtc_unit = 0;              // lifecycle unit the session streams (main/sub)
    StreamHub* rtc_hub = nullptr;   // the hub rtc_sink came from
    std::unique_ptr<webrtc::PeerSession> rtc;
    std::shared_ptr<Sink>    rtc_sink;
    lifecycle::DemandHandle  rtc_demand;
    // Front-door relay: per-client non-blocking upstream state. The poll loop
    // owns both sockets; no thread ever blocks on the busybox side, so a slow
    // CGI can not starve /ws/video or any other connection. Upstream bytes are
    // STREAMED into the client's bounded out buffer (never stored whole); a
    // full out buffer pauses upstream reads (backpressure) instead of growing.
    enum class Relay { None, Queued, Connecting, Writing, Reading };
    int         relay_fd = -1;
    Relay       relay_state = Relay::None;
    std::string relay_req;          // wire bytes for the upstream
    size_t      relay_off = 0;
    size_t      relay_total = 0;    // bytes already forwarded downstream
    int64_t     relay_idle_deadline_ms = 0;   // refreshed on connect/send/recv progress
    int64_t     relay_abs_deadline_ms = 0;    // hard ceiling, never refreshed
    std::string relay_what;         // "METHOD /path" for logging
    // Downstream keep-alive for relayed replies. The head is held back until
    // it is complete so it can be judged and rewritten; the body then streams
    // as before. Only a head with an exact length lets the browser connection
    // survive - see relay_head_keepalive(). Everything else closes, as it
    // always did.
    bool        keep_alive_wanted = false;    // what the BROWSER asked for
    std::string relay_head;         // partial upstream head, before framing is known
    bool        relay_head_done = false;
    bool        relay_keep = false; // this reply may leave the downstream open
    size_t      relay_body_len = 0; // exact body bytes to expect when relay_keep
    size_t      relay_body_seen = 0;
    // Menu injection: buffer a text/html page body, add Machino's nav links, and
    // send it with a corrected Content-Length. Only entered for a GET whose head
    // says text/html; anything else streams verbatim exactly as before. The head
    // is held in relay_saved_head until the body is buffered and rewritten.
    bool        relay_inject = false;
    bool        relay_get = false;  // only GET pages are buffered for injection
    std::string relay_saved_head;   // original upstream head, kept until injection
    std::string relay_inject_buf;   // the html body, accumulated
};

static const char* MJPEG_BOUNDARY = "machinoframe";

HttpServer::HttpServer(const ServerConfig& cfg, api::ApiService& api, EventBus& bus,
                       StreamHub* hub, lifecycle::PipelineManager* pipeline, StreamHub* sub_hub)
    : cfg_(cfg), api_(api), bus_(bus), hub_(hub), sub_hub_(sub_hub), pipeline_(pipeline) {
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
    for (auto& c : clients_) {
        if (c->sub) bus_.unsubscribe(c->sub);
        if (c->ws_video) RuntimeStats::get().dec(&RuntimeCounters::ws_video_clients);
        if (c->ws_logs)  RuntimeStats::get().dec(&RuntimeCounters::ws_logs_clients);
        if (c->rtc)      RuntimeStats::get().dec(&RuntimeCounters::webrtc_sessions);
        if (c->ws_sink) { StreamHub* h = c->ws_hub ? c->ws_hub : hub_; if (h) { c->ws_sink->close(); h->unsubscribe(c->ws_sink); } }
        if (c->rtc_sink) { StreamHub* h = c->rtc_hub ? c->rtc_hub : hub_; if (h) { c->rtc_sink->close(); h->unsubscribe(c->rtc_sink); } }
        if (c->relay_fd >= 0) close(c->relay_fd);
        close(c->fd);
    }
    clients_.clear();
    // The logread child is NOT touched here: main owns it, it was forked
    // before IMP existed, and it must outlive every HTTP restart.

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

bool HttpServer::sub_available() const {
    return sub_hub_ && pipeline_ && pipeline_->unit_configured(lifecycle::UNIT_SUB);
}

// The stock webui's ?stream= query: absent/0 = main, 1 = sub when it exists,
// anything else fail-closed (never a silent main feed).
int HttpServer::unit_for_stream(const std::string& sv) const {
    if (sv.empty() || sv == "0") return lifecycle::UNIT_MAIN;
    if (sv == "1" && sub_available()) return lifecycle::UNIT_SUB;
    return -1;
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
bool HttpServer::pump_requests(Client& c) {
    bool ok = true;
    while (ok && !c.close_after_flush && c.relay_state == Client::Relay::None &&
           c.in.find("\r\n\r\n") != std::string::npos) {
        const size_t before = c.in.size();
        ok = handle_request(c);
        if (c.in.size() == before) break;          // incomplete body: wait for more
    }
    return ok;
}

bool HttpServer::handle_request(Client& c) {
    size_t consumed = 0; Request req;
    Limits lim;
    lim.max_body = input_cap(c.in);
    Parse p = parse_request(c.in, consumed, req, lim);
    if (p == Parse::Incomplete) return c.in.size() <= input_cap(c.in);
    if (p == Parse::TooLarge) { queue(c, response(413, "application/json", api::ApiService::error("invalid_value", "", "request too large").dump(), false)); c.close_after_flush = true; return true; }
    if (p == Parse::Bad)      { queue(c, response(400, "application/json", api::ApiService::error("invalid_json", "", "malformed HTTP request").dump(), false)); c.close_after_flush = true; return true; }
    c.in.erase(0, consumed); ++c.requests; c.last_activity_ms = now_ms();

    const std::string& path = req.path; const std::string& m = req.method;
    api::Response r;
    if (m == "OPTIONS") { queue(c, response(204, "text/plain", "", req.keep_alive, "Access-Control-Allow-Methods: GET, POST, PUT, PATCH, OPTIONS\r\nAccess-Control-Allow-Headers: Content-Type, If-Match\r\n")); if (!req.keep_alive) c.close_after_flush = true; return true; }

    // AP11: ONVIF. Handled before BOTH gates because it carries its own
    // authentication (WS-Security, or HTTP Basic) and answers in SOAP: a
    // session redirect or a JSON 401 would be unintelligible to an ONVIF
    // client. The service is told the claim state and the unsafe flag and
    // applies them itself, so the policy is the same one, expressed in the
    // protocol the caller speaks.
    if (onvif_ && onvif::OnvifService::is_onvif_path(path)) {
        if (m != "POST") {
            bool ok = queue(c, response(405, "text/plain", "ONVIF expects POST\n", false));
            c.close_after_flush = true;
            return ok;
        }
        onvif::OnvifService::Request oreq;
        oreq.path = path;
        oreq.body = req.body;
        oreq.authorization = req.header("authorization");
        oreq.method = m;
        // The host the client used, so the XAddr and RTSP URLs it gets back
        // are reachable from where it is standing. Port stripped: the service
        // appends the ports it knows.
        oreq.host = req.header("host");
        if (const size_t colon = oreq.host.rfind(':'); colon != std::string::npos &&
            oreq.host.find(']') == std::string::npos)
            oreq.host.erase(colon);
        onvif_->set_unsafe(cfg_.unsafe);
        if (setup_) onvif_->set_claimed(!setup_->unclaimed());
        onvif::OnvifService::Response ores = onvif_->handle(oreq, (int64_t)::time(nullptr));
        LOGD(MOD, "%s: onvif %s -> %d", c.peer.c_str(), path.c_str(), ores.status);
        bool ok = queue(c, response(ores.status, ores.content_type.c_str(), ores.body, false,
                                    ores.extra_headers));
        c.close_after_flush = true;
        return ok;
    }

    // AP10: unclaimed / first-run. Runs BEFORE the session gate, because on an
    // unclaimed camera there is no credential that could satisfy it. Upstream:
    // while root's shadow hash is empty the camera serves NOTHING but the claim
    // flow, and once it is set /setup must stop existing - "an unauthenticated
    // page that sets the root password must not outlive the state that
    // justifies it".
    if (setup_ && !cfg_.unsafe) {
        const bool unclaimed = setup_->unclaimed();
        if (path == "/setup" && m == "POST") {
            SetupOutcome so = setup_->post(req.body);
            std::string cookie;
            if (so.mint_session && gate_) cookie = gate_->mint(now_ms());
            // Never the password, never the body - only what happened.
            LOGI(MOD, "%s: setup -> %d%s", c.peer.c_str(), so.status,
                 so.mint_session ? " (claimed, session minted)" : "");
            bool ok = queue(c, response(so.status, "text/plain", so.body, false, cookie));
            c.close_after_flush = true;
            return ok;
        }
        if (!unclaimed && m == "GET" && path == "/setup.html") {
            bool ok = queue(c, response(404, "text/plain", "Not Found\n", false));
            c.close_after_flush = true;
            return ok;
        }
        if (unclaimed && !SetupGate::is_setup_path(m, path) && !SessionGate::is_local_peer(c.peer)) {
            if (m == "GET" && req.header("accept").find("text/html") != std::string::npos) {
                bool ok = queue(c, response(302, "text/plain", "", false, "Location: /setup.html\r\n"));
                c.close_after_flush = true;
                return ok;
            }
            bool ok = queue(c, response(401, "application/json",
                                        api::ApiService::error("unauthorized", path,
                                                               "this camera has not been set up yet").dump(),
                                        false));
            c.close_after_flush = true;
            return ok;
        }
    }

    // Majestic drop-in session auth (see session.hpp for the exact webui
    // contract). Runs BEFORE every route, native or relayed.
    // /login and /logout stay wired even with authentication off: they are
    // routes the stock UI calls, and relaying them upstream instead would be a
    // different answer, not an absent one. Only the ENFORCEMENT below is
    // skipped when system.unsafe is set.
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
        const bool local = SessionGate::is_local_peer(c.peer);   // camera-local = trusted, like Majestic
        if (!cfg_.unsafe && !local && !SessionGate::is_public(m, path) && !gate_->authed(req.header("cookie"), t)
            && !gate_->authed_basic(req.header("authorization"))) {
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
    } else if (net_api_ && (path == "/machino/net" || path == "/machino/net/")) {
        // Machino's own page, under its own path. The stock WebUI is left
        // byte-identical: an installer that edits p/header.cgi makes an
        // upgrade of the stock UI either revert the change or conflict with
        // it. The menu entry that points here is a separate, explicit step.
        //
        // Served only when the network API is wired -- a page whose every
        // button answers 404 is worse than no page.
        if (m != "GET") { r = api::ApiService::fail(405, "unknown_field", path, "method not allowed"); }
        else {
            const bool ok = queue(c, response(200, "text/html; charset=utf-8",
                                              std::string(machino_net_page(), machino_net_page_len()),
                                              req.keep_alive));
            if (!req.keep_alive) c.close_after_flush = true;
            return ok;
        }
    } else if (path == "/machino/chrome.js") {
        // Die Kopfleiste fuer Machinos eigene Seiten. Bewusst eine eigene
        // Datei statt zweimal inline: beide Seiten brauchen dasselbe, und der
        // Browser kann sie zwischenspeichern. Siehe app/http/chrome.hpp dafuer,
        // warum nur die LINKS der Stock-WebUI uebernommen werden und nicht
        // deren Markup samt CSS.
        //
        // Nicht an net_api_ gebunden: ein Skript, das nichts tut, wenn es
        // nichts zu zeigen gibt, ist harmlos -- ein 404 mitten in einer
        // ausgelieferten Seite dagegen steht in jeder Browserkonsole.
        if (m != "GET") { r = api::ApiService::fail(405, "unknown_field", path, "method not allowed"); }
        else {
            const bool ok = queue(c, response(200, "application/javascript; charset=utf-8",
                                              machino_chrome_js(cfg_.chrome_source),
                                              req.keep_alive));
            if (!req.keep_alive) c.close_after_flush = true;
            return ok;
        }
    } else if (net_api_ && (path == "/machino/devices" || path == "/machino/devices/")) {
        // Die Geraeteseite. Dieselbe Bedingung wie bei /machino/net: nur
        // ausliefern, wenn die API dahinter verdrahtet ist -- eine Seite,
        // deren jeder Knopf 404 antwortet, ist schlimmer als keine Seite.
        if (m != "GET") { r = api::ApiService::fail(405, "unknown_field", path, "method not allowed"); }
        else {
            const bool ok = queue(c, response(200, "text/html; charset=utf-8",
                                              std::string(machino_devices_page(), machino_devices_page_len()),
                                              req.keep_alive));
            if (!req.keep_alive) c.close_after_flush = true;
            return ok;
        }
    } else if (net_api_ && net_api_->handle(m, path, req.body, r)) {
        // Asked first among the /api/v1 routes because it owns two whole
        // prefixes. It returns false for anything outside them, so the chain
        // below is unchanged for every existing path.
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
        Json st = api_.state().body;
        r = (m == "GET") ? api::Response{200, compat::majestic_sources(compat::majestic_config(api_.config().body, st), st)}
                         : api::ApiService::fail(405, "unknown_field", path, "method not allowed");
    } else if (path == "/api/v1/get") {
        // Stock CGI probe (majestic.sh mj_cfg): plain-text value or 404.
        if (m != "GET") { r = api::ApiService::fail(405, "unknown_field", path, "method not allowed"); }
        else {
            const std::string key = SessionGate::form_value(req.query, "key");
            std::string val;
            bool found = !key.empty() &&
                         compat::majestic_get(compat::majestic_config(api_.config().body, api_.state().body), key, val);
            bool ok = queue(c, found ? response(200, "text/plain", val, req.keep_alive)
                                     : response(404, "text/plain", "no such key\n", req.keep_alive));
            if (!req.keep_alive) c.close_after_flush = true;
            return ok;
        }
    } else if (path == "/api/v1/image") {
        // AP10: the live preview the settings page drives while a slider moves.
        // POST with the values in the QUERY string - that is mj-settings.js's
        // shape, not a choice of ours - and sendBeacon on pagehide posts the
        // same way. Nothing is persisted; the Save button is a separate path.
        if (m != "POST") { r = api::ApiService::fail(405, "unknown_field", path, "method not allowed"); }
        else             { r = api_.live_image(req.query); }
    } else if (path == "/api/v1/reset") {
        // Settings-page per-row reset: restore the built-in default; 404 =
        // "this camera has no such setting" (handled by the stock UI).
        if (m != "GET") { r = api::ApiService::fail(405, "unknown_field", path, "method not allowed"); }
        else {
            const std::string key = SessionGate::form_value(req.query, "key");
            compat::MajesticTranslation tr = compat::majestic_reset(key);
            if (!tr.ok)               r = api::ApiService::fail(tr.status, tr.code.c_str(), tr.path, tr.message);
            else if (!tr.unset.empty()) r = api_.unset_config(tr.unset);   // no-default: REMOVE the key (#416)
            else                      r = api_.patch_config(tr.patch.dump(), "");
        }
    } else if (path == "/api/v1/osd") {
        // AP9. The stock settings page reads this for the real overlay
        // rectangles (it counts regions and bytes from them) and the preview
        // overlays read `group`/`streams` to map coordinates. A 404 is a
        // legitimate answer the page handles as a property of the build, so
        // "we cannot say" is never faked with an empty 200.
        if (m != "GET") { r = api::ApiService::fail(405, "unknown_field", path, "method not allowed"); }
        else {
            Json doc;
            if (osd_ && osd_->report(doc)) {
                bool ok = queue(c, response(200, "application/json", doc.dump(), req.keep_alive));
                if (!req.keep_alive) c.close_after_flush = true;
                return ok;
            }
            r = api::ApiService::fail(404, "unknown_field", path, "this build cannot report overlay geometry");
        }
    } else if (path == "/api/v1/osd/image") {
        // GET  -> raw BGRA plus X-Osd-Width/Height/Ref, which is what the
        //         page's canvas reader expects.
        // POST with a body -> upload (w/h/ref in the query).
        // POST with NO body -> remove, which is how the page's staged logo
        //         deletion lands on save (flushLogoBin).
        const std::string ov = SessionGate::form_value(req.query, "overlay");
        const int overlay = query_int(ov, -1);
        if (!osd_) { r = api::ApiService::fail(404, "unknown_field", path, "no overlay store"); }
        else if (m == "GET") {
            osd::ImageInfo info; std::string pixels;
            if (osd_->load_image(overlay, info, pixels)) {
                char hdr[128];
                std::snprintf(hdr, sizeof hdr,
                              "X-Osd-Width: %d\r\nX-Osd-Height: %d\r\nX-Osd-Ref: %d\r\n",
                              info.w, info.h, info.ref);
                bool ok = queue(c, response(200, "application/octet-stream", pixels, req.keep_alive, hdr),
                                osd::OsdService::MAX_IMAGE_BYTES + 4096);
                if (!req.keep_alive) c.close_after_flush = true;
                return ok;
            }
            r = api::ApiService::fail(404, "unknown_field", path, "no picture for this overlay");
        } else if (m == "POST") {
            osd::OsdService::ImageResult res =
                req.body.empty()
                    ? osd_->delete_image(overlay)
                    : osd_->store_image(overlay,
                                        query_int(SessionGate::form_value(req.query, "w"), 0),
                                        query_int(SessionGate::form_value(req.query, "h"), 0),
                                        query_int(SessionGate::form_value(req.query, "ref"), 0),
                                        reinterpret_cast<const uint8_t*>(req.body.data()),
                                        req.body.size());
            if (res.ok()) {
                bool ok = queue(c, response(200, "application/json", std::string("{\"ok\":1}"), req.keep_alive));
                if (!req.keep_alive) c.close_after_flush = true;
                return ok;
            }
            // The page shows the response text verbatim when an upload is
            // refused, so this body is the operator-facing message.
            bool ok = queue(c, response(res.status, "text/plain", res.message + "\n", false));
            c.close_after_flush = true;
            return ok;
        } else {
            r = api::ApiService::fail(405, "unknown_field", path, "method not allowed");
        }
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
    } else if (path == "/ws/video") {
        // The stock webui's Live player (upstream preview.js): WebSocket, one
        // JSON init + fMP4 init segment, then one moof+mdat per frame.
        const std::string wskey = req.header("sec-websocket-key");
        const std::string sv = SessionGate::form_value(req.query, "stream");
        const int unit = unit_for_stream(sv);
        if (m != "GET" || wskey.empty()) { r = api::ApiService::fail(400, "invalid_value", path, "websocket upgrade required"); }
        else if (!hub_ || !pipeline_)    { r = api::ApiService::fail(501, "unavailable", path, "no media wiring"); }
        else if (unit < 0) {
            r = api::ApiService::fail(404, "unknown_field", path, "stream " + sv + " is not available");
        } else {
            StreamHub* h = unit == lifecycle::UNIT_SUB ? sub_hub_ : hub_;
            Result dr;
            lifecycle::DemandHandle d = pipeline_->acquire_unit(unit, lifecycle::ConsumerType::HttpStream, &dr);
            if (!d.active()) { r = api::ApiService::fail(503, "unavailable", path, "pipeline start failed"); }
            else {
                queue(c, ws::handshake_response(wskey));
                c.ws_video = true;
                RuntimeStats::get().inc(&RuntimeCounters::ws_video_clients);
                c.ws_unit = unit;
                c.ws_hub = h;
                c.ws_demand = std::move(d);
                c.ws_sink = h->subscribe();
                c.ws_await_key = true;
                pipeline_->request_idr(unit);
                LOGI(MOD, "%s: /ws/video session started (unit %d)", c.peer.c_str(), unit);
                return true;
            }
        }
    } else if (path == "/ws/webrtc") {
        // The stock webui's preferred Live transport (preview-webrtc.js):
        // this socket only signals; media runs over the session's UDP port.
        const std::string wskey = req.header("sec-websocket-key");
        const std::string sv = SessionGate::form_value(req.query, "stream");
        const int unit = unit_for_stream(sv);
        if (m != "GET" || wskey.empty()) { r = api::ApiService::fail(400, "invalid_value", path, "websocket upgrade required"); }
        else if (!hub_ || !pipeline_)    { r = api::ApiService::fail(501, "unavailable", path, "no media wiring"); }
        else if (unit < 0) {
            r = api::ApiService::fail(404, "unknown_field", path, "stream " + sv + " is not available");
        } else {
            queue(c, ws::handshake_response(wskey));
            c.rtc_ws = true;
            c.rtc_unit = unit;
            LOGI(MOD, "%s: /ws/webrtc signalling open (unit %d)", c.peer.c_str(), unit);
            return true;
        }
    } else if (path == "/ws/upgrade") {
        // AP21: this build does not flash firmware. Answering 404 looked
        // harmless and was not - the stock Update page reports a failed
        // handshake as "Could not start the upgrade. Another session may be in
        // progress, or the camera is unreachable", and BOTH halves of that are
        // false here. It sends an owner hunting for a phantom session on a
        // camera that is answering perfectly.
        //
        // The contract has a channel for exactly this: upstream's update.js
        // matches an enumerated, anchored refusal vocabulary on a TEXT frame
        // and then says "Nothing was written to flash, so the camera is
        // unchanged" - which is the true sentence. So the socket is accepted,
        // the refusal is spoken in the words the page knows, and the reason
        // follows in the log pane underneath it.
        const std::string wskey = req.header("sec-websocket-key");
        if (m != "GET" || wskey.empty()) { r = api::ApiService::fail(400, "invalid_value", path, "websocket upgrade required"); }
        else {
            queue(c, ws::handshake_response(wskey));
            const std::string why = compat::upgrade_refusal();
            queue(c, ws::frame(true, why.data(), why.size()));
            c.close_after_flush = true;
            LOGI(MOD, "%s: /ws/upgrade refused - this build does not flash firmware", c.peer.c_str());
            return true;
        }
    } else if (path == "/ws/logs") {
        // The stock log viewer: one WebSocket, binary frames of raw syslog
        // lines (it splits on newline itself). Source is the system log, which
        // is why the daemon also logs to syslog as "majestic" in drop-in mode.
        const std::string wskey = req.header("sec-websocket-key");
        if (m != "GET" || wskey.empty()) { r = api::ApiService::fail(400, "invalid_value", path, "websocket upgrade required"); }
        else {
            queue(c, ws::handshake_response(wskey));
            // Subscribe only. No fork, no kill, nothing from a request that
            // can reach the media path - see app/log_reader.hpp.
            if (logs_fd() < 0) { c.close_after_flush = true; return true; }   // no reader on this box
            // Count it BEFORE anything can fail below: the two teardown paths
            // decrement on c.ws_logs, so the flag and the gauge have to be set
            // together or the gauge drifts. This increment was missing
            // entirely - the counter had a dec() in both teardown paths and no
            // inc() anywhere, so it read 0 for the life of the process while
            // clients were connected and receiving. It was quoted as evidence
            // that a Logs page was closed when it was not.
            c.ws_logs = true;
            RuntimeStats::get().inc(&RuntimeCounters::ws_logs_clients);
            LOGI(MOD, "%s: /ws/logs subscribed", c.peer.c_str());
            return true;
        }
    } else if (path == "/api/v1/stream.mjpeg" || path == "/stream.mjpeg" || path == "/stream") {
        if (m != "GET") { r = api::ApiService::fail(405, "unknown_field", path, "method not allowed"); }
        // AP24: refuse the same way /snapshot does when there is no JPEG unit.
        // This used to answer 200 and open a multipart stream that could never
        // carry a frame - and MJPEG clients are exempt from the idle timeout,
        // so it also held a client slot open for as long as the viewer waited.
        // A stream that reports success and then produces nothing is the exact
        // shape this project has been removing everywhere else.
        else if (pipeline_ && !pipeline_->unit_configured(lifecycle::UNIT_JPEG)) {
            r = api::ApiService::fail(501, "unavailable", path,
                                      "jpeg not configured on this platform - no MJPEG stream to give");
        }
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

// Start forwarding one request to the internal OpenIPC WebUI (busybox httpd).
// Non-blocking: this only opens the upstream socket and records relay state;
// pump_relay advances it from the poll loop as the fds become ready. The
// downstream connection is closed after the relayed reply: the upstream is
// HTTP/1.0/EOF-delimited, so one request per socket.
bool HttpServer::relay_upstream(Client& c, const Request& req) {
    c.relay_req = forward_request(req, cfg_.upstream_host);
    c.relay_off = 0;
    c.relay_total = 0;
    c.relay_head.clear();
    c.relay_head_done = false;
    c.relay_keep = false;
    c.relay_body_len = 0;
    c.relay_body_seen = 0;
    c.relay_inject = false;
    c.relay_saved_head.clear();
    c.relay_inject_buf.clear();
    c.relay_get = (req.method == "GET");
    c.keep_alive_wanted = req.keep_alive;
    const int64_t now = now_ms();
    c.relay_idle_deadline_ms = now + cfg_.relay_timeout_ms;
    c.relay_abs_deadline_ms  = now + cfg_.relay_max_ms;
    c.relay_what = req.method + " " + req.path;
    // Every in-flight relay is a forked CGI on the busybox side; a browser
    // dashboard fires a dozen fetches at once and the camera has ~43 MiB of
    // userspace. Excess relays wait here until a slot frees (the old blocking
    // relay serialized them to exactly one, which is what kept busybox safe).
    int inflight = 0;
    for (auto& other : clients_) if (other->relay_fd >= 0) ++inflight;
    if (inflight >= cfg_.max_relay_inflight) { c.relay_state = Client::Relay::Queued; return true; }
    return relay_open(c);
}

// Open the upstream socket for a prepared relay (fresh or dequeued).
bool HttpServer::relay_open(Client& c) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        c.relay_state = Client::Relay::None;
        queue(c, response(502, "text/plain", "upstream socket failed\n", false));
        c.close_after_flush = true; return true;
    }
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons((uint16_t)cfg_.upstream_port);
    int rc = -1;
    if (inet_pton(AF_INET, cfg_.upstream_host.c_str(), &a.sin_addr) == 1)
        rc = connect(fd, (sockaddr*)&a, sizeof a);
    if (rc < 0 && errno != EINPROGRESS) {
        LOGW(MOD, "relay: connect %s:%d failed: %s", cfg_.upstream_host.c_str(), cfg_.upstream_port, strerror(errno));
        close(fd);
        c.relay_state = Client::Relay::None;
        queue(c, response(502, "text/plain", "OpenIPC WebUI backend unreachable\n", false));
        c.close_after_flush = true; return true;
    }
    c.relay_fd = fd;
    c.relay_state = (rc == 0) ? Client::Relay::Writing : Client::Relay::Connecting;
    c.relay_idle_deadline_ms = now_ms() + cfg_.relay_timeout_ms;   // waiting in the queue was not upstream inactivity
    return true;
}

// Advance one client's upstream relay; called every poll iteration with the
// upstream fd's revents. Upstream bytes stream straight into the client's out
// buffer; a full out buffer pauses reads until the downstream drains. Bounded
// by an inactivity deadline (refreshed on progress), an absolute ceiling and
// max_relay_bytes. Returns false only when the client itself must be dropped.
bool HttpServer::pump_relay(Client& c, short re, int64_t now) {
    auto done = [&]() {                                            // upstream finished cleanly
        LOGD(MOD, "relay %s -> %zu B%s", c.relay_what.c_str(), c.relay_total,
             c.relay_keep ? " (downstream kept)" : "");
        close(c.relay_fd); c.relay_fd = -1; c.relay_state = Client::Relay::None;
        c.relay_req.clear();
        if (c.relay_keep) {
            // The browser connection survives: reset the per-reply state so the
            // next request on it starts clean. The UPSTREAM socket is still one
            // per request - only the expensive half is reused.
            c.relay_head.clear(); c.relay_head_done = false;
            c.relay_keep = false; c.relay_body_len = 0; c.relay_body_seen = 0;
            c.relay_inject = false; c.relay_saved_head.clear(); c.relay_inject_buf.clear();
            c.relay_total = 0;
            return true;
        }
        c.close_after_flush = true;
        return true;
    };
    auto fail = [&](int status, const char* msg) -> bool {
        LOGW(MOD, "relay: %s: %s", c.relay_what.c_str(), msg);
        close(c.relay_fd); c.relay_fd = -1; c.relay_state = Client::Relay::None;
        c.relay_req.clear();
        // Bytes already streamed can not be unsent: the only honest signal
        // left is cutting the connection, never a truncated 200. Before any
        // body bytes an explicit error response still fits.
        if (c.relay_total > 0) { c.out.clear(); return false; }
        c.close_after_flush = true;
        // AP24: the body names the request too. A bare "backend timed out"
        // cannot be told apart from a wedged camera by whoever reads it, and
        // the log line that carries the path is on the camera, not in front of
        // them.
        return queue(c, response(status, "text/plain",
                                 std::string(msg) + " (" + c.relay_what + ")\n", false));
    };
    auto progress = [&]() {
        c.relay_idle_deadline_ms = now + cfg_.relay_timeout_ms;
        c.last_activity_ms = now;                                  // a streaming download is not an idle client
    };
    if (c.relay_state == Client::Relay::Queued) {
        if (now >= c.relay_idle_deadline_ms) {                     // parked too long behind slow CGIs
            c.relay_state = Client::Relay::None; c.relay_req.clear();
            c.close_after_flush = true;
            return queue(c, response(503, "text/plain", "OpenIPC WebUI backend is busy\n", false));
        }
        int inflight = 0;
        for (auto& other : clients_) if (other->relay_fd >= 0) ++inflight;
        if (inflight >= cfg_.max_relay_inflight) return true;      // keep waiting
        return relay_open(c);
    }
    if (re & POLLNVAL) return fail(502, "OpenIPC WebUI backend failed");
    // AP24: name the request and the bound. A bare "backend timed out" cannot
    // be told apart from a wedged camera, and the case that produces it here
    // is neither: /cgi-bin/j/time.cgi runs `ntpd -n -q -N`, which on a camera
    // with no gateway spends 41 s failing DNS and prints nothing meanwhile -
    // indistinguishable from a hang to an inactivity bound. The reader needs
    // to know WHICH request and HOW LONG before they can judge that.
    if (now >= c.relay_abs_deadline_ms) {
        char m[128];
        snprintf(m, sizeof m, "OpenIPC WebUI backend exceeded the %d ms relay ceiling", cfg_.relay_max_ms);
        return fail(504, m);
    }
    if (now >= c.relay_idle_deadline_ms) {
        char m[128];
        snprintf(m, sizeof m, "OpenIPC WebUI backend sent nothing for %d ms", cfg_.relay_timeout_ms);
        return fail(504, m);
    }
    if (c.relay_state == Client::Relay::Connecting) {
        if (re & (POLLERR | POLLHUP)) return fail(502, "OpenIPC WebUI backend unreachable");
        if (!(re & POLLOUT)) return true;                          // still connecting
        int err = 0; socklen_t el = sizeof err;
        if (getsockopt(c.relay_fd, SOL_SOCKET, SO_ERROR, &err, &el) < 0 || err != 0)
            return fail(502, "OpenIPC WebUI backend unreachable");
        c.relay_state = Client::Relay::Writing;
        progress();
    }
    if (c.relay_state == Client::Relay::Writing) {
        while (c.relay_off < c.relay_req.size()) {
            ssize_t w = send(c.relay_fd, c.relay_req.data() + c.relay_off,
                             c.relay_req.size() - c.relay_off, MSG_NOSIGNAL | MSG_DONTWAIT);
            if (w > 0) { c.relay_off += (size_t)w; progress(); continue; }
            if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return true;
            return fail(502, "upstream write failed");
        }
        c.relay_state = Client::Relay::Reading;
    }
    // Reading: stream toward the downstream buffer while it has room. When it
    // is full we simply stop reading (and the poll loop stops watching the
    // upstream for POLLIN) - TCP itself backpressures busybox; waiting on a
    // slow downstream is not upstream inactivity.
    char buf[8192];
    while (c.out.size() < cfg_.max_out_buffer) {
        ssize_t rd = recv(c.relay_fd, buf, sizeof buf, MSG_DONTWAIT);
        if (rd == 0) {
            // A page that ended while still inside the scan window: inject on
            // what we have (or serve it unchanged if the anchor never appeared)
            // and flush it before finishing.
            if (c.relay_inject) {
                bool did = false;
                std::string merged = http::inject_machino_nav(c.relay_inject_buf, did);
                const std::string& emit = did ? merged : c.relay_inject_buf;
                if (!emit.empty() && !queue(c, emit, cfg_.max_out_buffer + sizeof buf)) return false;
                c.relay_total += emit.size();
                c.relay_inject = false;
                c.relay_inject_buf.clear();
                if (!did)
                    LOGW(MOD, "relay: %s: nav anchor not found - page served unchanged",
                         c.relay_what.c_str());
            }
            // EOF: upstream done (it was asked for Connection: close). If we
            // already promised a Content-Length downstream and got fewer bytes,
            // the upstream truncated: cutting the connection is the only honest
            // signal left, exactly as in fail() - never leave a kept-alive
            // connection desynchronised behind a short body.
            if (c.relay_keep && c.relay_body_seen < c.relay_body_len) {
                LOGW(MOD, "relay: %s: upstream ended %zu B short of its Content-Length",
                     c.relay_what.c_str(), c.relay_body_len - c.relay_body_seen);
                c.relay_keep = false;
            }
            return done();
        }
        if (rd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return true;
            return fail(502, "OpenIPC WebUI backend failed");
        }
        if (c.relay_total + c.relay_head.size() + (size_t)rd > cfg_.max_relay_bytes)
            return fail(502, "upstream response exceeds the relay size limit");

        // Hold the head back until it is complete: whether the downstream may
        // stay open is decided from it, and a half-read head cannot be judged.
        if (!c.relay_head_done) {
            c.relay_head.append(buf, (size_t)rd);
            size_t sep = 0;
            const size_t hend = http::relay_head_end(c.relay_head, sep);
            if (hend == std::string::npos) {
                if (c.relay_head.size() > 8192) return fail(502, "OpenIPC WebUI backend sent an oversized header");
                progress();
                continue;
            }
            std::string head = c.relay_head.substr(0, hend + sep);
            std::string rest = c.relay_head.substr(hend + sep);
            c.relay_head.clear();
            c.relay_head_done = true;

            // Menu injection: a GET whose head says text/html is delivered
            // close-framed with Machino's nav links inserted near the top. The
            // page is NOT buffered whole — the navbar is at the start of <body>,
            // so we scan a bounded window, inject once, and stream the rest
            // verbatim. Nothing under /var/www is touched.
            if (c.relay_get && http::relay_head_is_html(head)) {
                const std::string sh = http::relay_head_stream_close(head);
                if (!queue(c, sh, cfg_.max_out_buffer + sizeof buf)) return false;
                c.relay_total += sh.size();
                c.relay_keep = false;      // close-framed: the socket close is the end
                c.relay_inject = true;
                c.relay_inject_buf = rest; // begin the scan window
                // Try to inject from what we already have; otherwise keep reading.
                if (c.relay_inject) {
                    bool did = false;
                    std::string merged = http::inject_machino_nav(c.relay_inject_buf, did);
                    if (did || c.relay_inject_buf.size() >= cfg_.max_inject_bytes) {
                        const std::string& emit = did ? merged : c.relay_inject_buf;
                        if (!queue(c, emit, cfg_.max_out_buffer + sizeof buf)) return false;
                        c.relay_total += emit.size();
                        c.relay_inject = false;
                        c.relay_inject_buf.clear();
                        if (!did)
                            LOGW(MOD, "relay: %s: nav anchor not found in first %zu B - page served unchanged",
                                 c.relay_what.c_str(), cfg_.max_inject_bytes);
                    }
                }
                progress();
                continue;
            }

            std::string patched;
            c.relay_keep = c.keep_alive_wanted &&
                           http::relay_head_keepalive(head, patched, c.relay_body_len);
            const std::string& send_head = c.relay_keep ? patched : head;
            if (!queue(c, send_head, cfg_.max_out_buffer + sizeof buf)) return false;
            c.relay_total += send_head.size();

            if (!rest.empty()) {
                if (c.relay_keep && rest.size() > c.relay_body_len) rest.resize(c.relay_body_len);
                if (!queue(c, rest, cfg_.max_out_buffer + sizeof buf)) return false;
                c.relay_total += rest.size();
                c.relay_body_seen += rest.size();
            }
            if (c.relay_keep && c.relay_body_seen >= c.relay_body_len) return done();
            progress();
            continue;
        }

        // Still scanning for the navbar anchor: accumulate into the bounded
        // window and inject as soon as it is found (or give up at the window
        // edge and pass the buffer through). These bytes never take the verbatim
        // path below.
        if (c.relay_inject) {
            c.relay_inject_buf.append(buf, (size_t)rd);
            bool did = false;
            std::string merged = http::inject_machino_nav(c.relay_inject_buf, did);
            if (did || c.relay_inject_buf.size() >= cfg_.max_inject_bytes) {
                const std::string& emit = did ? merged : c.relay_inject_buf;
                if (!queue(c, emit, cfg_.max_out_buffer + sizeof buf)) return false;
                c.relay_total += emit.size();
                c.relay_inject = false;
                c.relay_inject_buf.clear();
                if (!did)
                    LOGW(MOD, "relay: %s: nav anchor not in first %zu B - page served unchanged",
                         c.relay_what.c_str(), cfg_.max_inject_bytes);
            }
            progress();
            continue;
        }

        // Body. Verbatim pass-through; cap = threshold + one read so this never trips.
        size_t take = (size_t)rd;
        if (c.relay_keep && c.relay_body_seen + take > c.relay_body_len)
            take = c.relay_body_len - c.relay_body_seen;      // never overrun the declared length
        if (take && !queue(c, std::string(buf, take), cfg_.max_out_buffer + sizeof buf)) return false;
        c.relay_total += take;
        c.relay_body_seen += take;
        if (c.relay_keep && c.relay_body_seen >= c.relay_body_len) return done();
        progress();
    }
    progress();                                                    // paused on backpressure, not idle
    return true;
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

void HttpServer::note_h264_profile(int unit, const std::vector<uint8_t>& sps) {
    if (unit < 0 || unit >= 4 || sps.size() < 4) return;
    char b[8];
    snprintf(b, sizeof b, "%02x%02x%02x", sps[1], sps[2], sps[3]);
    if (h264_profile_[unit] != b) h264_profile_[unit] = b;   // follows a reconfigure
}

namespace {
// A resync is an EPISODE, not a tick: while a socket is behind, the cap is hit
// on every pass through the loop, and counting those would report a hundred
// hiccups where the viewer saw one.
template <class C> void mark_resync(C& c) {
    if (c.ws_await_key) return;
    c.ws_await_key = true;
    RuntimeStats::get().inc(&RuntimeCounters::ws_video_resyncs);
}
} // namespace

// Drain the hub sink into ws frames - bounded per tick, drop-until-key on
// backpressure (old frames are worse than dropped frames; the decoder must
// never see a P-frame whose reference was dropped).
void HttpServer::pump_ws_video(Client& c) {
    if (!c.ws_sink) return;
    const size_t soft_cap = cfg_.ws_out_cap;                  // an IDR burst fits, runaway buffers do not
    for (int i = 0; i < 8; ++i) {
        if (c.out.size() > soft_cap / 2) { mark_resync(c); return; }
        AuPtr au; bool disc = false;
        if (!c.ws_sink->pop(au, 0, &disc)) return;
        if (disc) mark_resync(c);
        if (!au || au->data.empty()) continue;

        if (au->key) {
            std::vector<uint8_t> sps, pps;
            if (h264::extract_params(au->data.data(), au->data.size(), sps, pps) && !sps.empty() && !pps.empty()) {
                note_h264_profile(c.ws_unit, sps);      // the UNREWRITTEN sps: the profile bytes
                // MSE only: state the stream's true reorder/DPB bounds in the
                // avcC SPS so the browser stops holding ~1 s of frames (the
                // in-band SPS is stripped from mdat anyway). RTSP is untouched.
                sps = h264::sps_with_bitstream_restriction(sps);
                if (!c.ws_init_sent || sps != c.ws_sps || pps != c.ws_pps) {
                    c.ws_sps = sps; c.ws_pps = pps;
                    const EffectiveStream es = pipeline_ ? pipeline_->stream_unit(c.ws_unit) : EffectiveStream{};
                    const int w = es.width, h = es.height;
                    const std::string cs = fmp4::codec_string(sps);
                    Json info = Json::object();
                    info.set("type", Json::string("init"));
                    info.set("codec", Json::string("h264"));
                    info.set("codecString", Json::string(cs));
                    info.set("width", Json::integer(w));
                    info.set("height", Json::integer(h));
                    const std::string init_json = info.dump();
                    std::vector<uint8_t> init = fmp4::init_segment(sps, pps, w, h, 90000);
                    queue(c, ws::frame(true, init_json.data(), init_json.size()), soft_cap);
                    if (!queue(c, ws::frame(false, init.data(), init.size()), soft_cap)) { c.close_after_flush = true; return; }
                    c.ws_init_sent = true;
                    LOGI(MOD, "%s: /ws/video init %s %dx%d", c.peer.c_str(), cs.c_str(), w, h);
                }
            }
            c.ws_await_key = false;
        }
        if (!c.ws_init_sent) continue;
        if (c.ws_await_key) continue;                          // resumes at the next key frame

        // The timeline lives in fmp4::Timeline, where a host test can reach
        // it: derived from the capture clock, discontinuities absorbed.
        uint32_t dur = 0;
        c.ws_dts = c.ws_timeline.next(au->pts_us, dur);
        std::vector<uint8_t> sample = fmp4::annexb_to_avcc(au->data.data(), au->data.size());
        if (sample.empty()) continue;
        std::vector<uint8_t> frag;
        // A producer reference time, when the camera actually knows what time
        // it is (AP12 bound): upstream's player reads it to show the true
        // end-to-end lag. A camera with an unset clock stays silent rather
        // than reporting an invented one.
        struct timespec rt;
        if (clock_gettime(CLOCK_REALTIME, &rt) == 0 && rt.tv_sec > 1700000000) {
            const uint64_t ntp = ((uint64_t)(rt.tv_sec + 2208988800ull) << 32)
                               | (uint64_t)((double)rt.tv_nsec * 4.294967296);
            frag = fmp4::prft(1, ntp, c.ws_dts);
        }
        const std::vector<uint8_t> body = fmp4::fragment(c.ws_seq++, c.ws_dts, dur, sample, au->key);
        frag.insert(frag.end(), body.begin(), body.end());
        if (!queue(c, ws::frame(false, frag.data(), frag.size()), soft_cap)) {
            RuntimeStats::get().inc(&RuntimeCounters::ws_video_overruns);
            mark_resync(c);                                    // dropped: wait for the next key
            return;
        }
        RuntimeStats::get().inc(&RuntimeCounters::ws_video_frames);
        RuntimeStats::get().inc(&RuntimeCounters::ws_video_bytes, frag.size());
        RuntimeStats::get().high_water(&RuntimeCounters::ws_video_out_peak, (int)c.out.size());
    }
}

// Client -> server on a /ws/video socket: tiny JSON ({"request":"idr"}),
// ping (answered), close. Returns false to drop the connection.
bool HttpServer::ws_video_input(Client& c) {
    for (;;) {
        size_t used = 0; int op = 0; std::string payload;
        ws::Parse p = ws::parse_frame(c.in, used, op, payload);
        if (p == ws::Parse::Incomplete) return c.in.size() <= MAX_IN;
        if (p == ws::Parse::Bad) return false;
        c.in.erase(0, used);
        if (op == 8) return false;                             // close
        if (op == 9) { queue(c, ws::pong_frame(payload)); continue; }
        if (op == 1 && payload.find("\"idr\"") != std::string::npos && pipeline_) {
            const int64_t t = now_ms();
            if (t - c.ws_last_idr_req_ms >= 1000) {            // the client rate-limits too; belt and braces
                c.ws_last_idr_req_ms = t;
                pipeline_->request_idr(c.ws_unit);
            }
        }
    }
}

// /ws/webrtc signalling: {"req":"offer","data":<sdp>} -> answer/error/busy.
// Trickled candidates are ignored - ICE-lite learns the peer address from its
// authenticated STUN checks. One session per socket, two per camera.
bool HttpServer::rtc_ws_input(Client& c) {
    for (;;) {
        size_t used = 0; int op = 0; std::string payload;
        ws::Parse p = ws::parse_frame(c.in, used, op, payload, 32768);   // an SDP offer is a few KB
        if (p == ws::Parse::Incomplete) return c.in.size() <= MAX_IN;
        if (p == ws::Parse::Bad) return false;
        c.in.erase(0, used);
        if (op == 8) return false;
        if (op == 9) { queue(c, ws::pong_frame(payload)); continue; }
        if (op != 1) continue;
        // A real Chrome SDP offer is ~7.5 KB inside one JSON string; the
        // default JsonLimits (4 KB string / 16 KB total) reject it. Raise the
        // caps for signalling messages, bounded by the WS frame cap (32 KB).
        Json msg; std::string jerr;
        const JsonLimits sig_limits{16, 32768, 65536};
        if (!Json::parse(payload, msg, jerr, sig_limits)) { LOGW(MOD, "webrtc: json parse failed: %s", jerr.c_str()); continue; }
        const Json* req = msg.get("req");
        if (!req || !req->is_string()) { LOGW(MOD, "webrtc: no req field"); continue; }
        auto reply = [&](const char* kind, const std::string& data) {
            Json r = Json::object();
            r.set("reply", Json::string(kind));
            r.set("data", Json::string(data));
            const std::string s = r.dump();
            return queue(c, ws::frame(true, s.data(), s.size()));
        };
        if (req->as_string() != "offer") continue;   // trickled candidates: ICE-lite learns from STUN
        int active = 0;
        for (auto& o : clients_) if (o->rtc) ++active;
        if (c.rtc || active >= 2) { if (!reply("busy", "every session slot is taken")) return false; continue; }
        const Json* data = msg.get("data");
        if (!data || !data->is_string()) { if (!reply("error", "offer carries no sdp")) return false; continue; }
        sockaddr_in la{}; socklen_t ll = sizeof la;
        char ip[INET_ADDRSTRLEN] = "0.0.0.0";
        if (getsockname(c.fd, (sockaddr*)&la, &ll) == 0)
            inet_ntop(AF_INET, &la.sin_addr, ip, sizeof ip);
        std::unique_ptr<webrtc::PeerSession> sess(new webrtc::PeerSession(ip));
        std::string err;
        const std::string answer = sess->on_offer(data->as_string(), err,
                                                  c.rtc_unit >= 0 && c.rtc_unit < 4 ? h264_profile_[c.rtc_unit] : std::string());
        if (answer.empty()) { LOGW(MOD, "webrtc: offer rejected: %s", err.c_str()); if (!reply("error", err)) return false; continue; }
        StreamHub* h = c.rtc_unit == lifecycle::UNIT_SUB ? sub_hub_ : hub_;
        Result dr;
        lifecycle::DemandHandle d = pipeline_->acquire_unit(c.rtc_unit, lifecycle::ConsumerType::HttpStream, &dr);
        if (!d.active()) { if (!reply("error", "pipeline start failed")) return false; continue; }
        c.rtc = std::move(sess);
        RuntimeStats::get().inc(&RuntimeCounters::webrtc_sessions);
        c.rtc_demand = std::move(d);
        c.rtc_hub = h;
        c.rtc_sink = h->subscribe();
        pipeline_->request_idr(c.rtc_unit);
        if (!reply("answer", answer)) return false;
        LOGI(MOD, "%s: webrtc session negotiated (unit %d)", c.peer.c_str(), c.rtc_unit);
    }
}

// Per tick: DTLS timers, PLI -> on-demand IDR, and the AU pump into RTP.
void HttpServer::pump_rtc(Client& c) {
    if (!c.rtc) return;
    c.rtc->tick();
    c.rtc->log_stats();
    if (c.rtc->take_pli() && pipeline_) pipeline_->request_idr(c.rtc_unit);
    if (!c.rtc->media_ready() || !c.rtc_sink) return;
    for (int i = 0; i < 8; ++i) {
        AuPtr au; bool disc = false;
        if (!c.rtc_sink->pop(au, 0, &disc)) return;
        if (!au || au->data.empty()) continue;
        if (au->key) {
            std::vector<uint8_t> sps, pps;
            if (h264::extract_params(au->data.data(), au->data.size(), sps, pps)) note_h264_profile(c.rtc_unit, sps);
        }
        c.rtc->send_au(au->data.data(), au->data.size(), au->pts_us, au->key);
    }
}

bool HttpServer::logs_wanted() const {
    for (const auto& c : clients_) if (c->ws_logs) return true;
    return false;
}

// One "logread -f" for the whole server. fork+exec (no shell) so nothing is
// parsed on our behalf, the read end is non-blocking and joins poll(); the
// child is reaped when the last subscriber goes.


// Forward whole lines only: the viewer splits on newline and keeps a partial
// tail, but sending half a line to every subscriber would interleave badly
// once there is more than one.
// Collect the logread child once it has actually exited. Non-blocking, called
// from the poll loop, so a child that takes a moment to die after SIGTERM is
// still reaped instead of accumulating as a zombie PID.

int HttpServer::logs_fd() const { return log_reader_ ? log_reader_->fd() : -1; }

// Drain the reader's pipe and fan whole lines out to whoever is subscribed.
//
// This runs whether or not anybody is listening. With no subscribers the lines
// are read and dropped, because an undrained pipe blocks `logread` and turns
// the log stream into a dead one. Nothing here forks or signals: a reader that
// dies is marked dead and stays dead for the life of the daemon, since forking
// a replacement could happen while IMP is live - which is the whole defect
// this design exists to avoid.
void HttpServer::logs_pump(short revents) {
    if (logs_fd() < 0) return;
    const auto reader_died = [this](const char* why) {
        LOGW(MOD, "/ws/logs: reader gone (%s) - not restarting it, see log_reader.hpp", why);
        if (log_reader_) log_reader_->mark_dead();
        for (auto& c : clients_) if (c->ws_logs) c->close_after_flush = true;
    };
    if (revents & (POLLERR | POLLNVAL)) { reader_died("poll error"); return; }
    if (!(revents & (POLLIN | POLLHUP))) return;
    char buf[4096];
    for (;;) {
        const ssize_t n = read(logs_fd(), buf, sizeof buf);
        if (n == 0) { reader_died("logread exited"); return; }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return;
            reader_died("read error"); return;
        }
        logs_buf_.append(buf, (size_t)n);
        const size_t nl = logs_buf_.rfind(0x0a);
        if (nl == std::string::npos) {
            if (logs_buf_.size() > 64 * 1024) logs_buf_.clear();   // pathological single line
            continue;
        }
        const std::string chunk = logs_buf_.substr(0, nl + 1);
        logs_buf_.erase(0, nl + 1);
        const std::string frame = ws::frame(false, chunk.data(), chunk.size());  // binary, per the viewer
        for (auto& c : clients_) {
            if (!c->ws_logs) continue;
            // a viewer that cannot keep up is dropped, never buffered without
            // bound - the log stream must not become a memory leak
            if (!queue(*c, frame, cfg_.max_out_buffer)) c->close_after_flush = true;
        }
    }
}

void HttpServer::loop() {
    std::vector<pollfd> pfds;
    // pfds[k+1] belongs to refs[k]: an explicit fd->client map, because
    // clients_ mutates (accept) between building the set and consuming the
    // events - positional indexing would misroute revents.
    struct PollRef { Client* c; int kind; };   // 0 downstream, 1 relay upstream, 2 webrtc udp
    std::vector<PollRef> refs;
    last_telemetry_ms_ = last_heartbeat_ms_ = now_ms();
    while (!quit_) {
        pfds.clear(); refs.clear();
        pfds.push_back({listen_fd_, POLLIN, 0});
        int timeout_ms = 250;
        for (auto& cp : clients_) {
            Client* c = cp.get();
            pfds.push_back({c->fd, (short)(POLLIN | (c->out.empty() ? 0 : POLLOUT)), 0});
            refs.push_back({c, 0});
            if (c->relay_fd >= 0) {                    // upstream rides the same poll; nothing blocks
                short ev = (c->relay_state == Client::Relay::Reading)
                    // full downstream buffer: stop watching for data (TCP
                    // backpressures busybox); errors still wake us.
                    ? (short)(c->out.size() < cfg_.max_out_buffer ? POLLIN : 0)
                    : (short)POLLOUT;
                pfds.push_back({c->relay_fd, ev, 0});
                refs.push_back({c, 1});
            }
            if (c->rtc && c->rtc->fd() >= 0) {         // webrtc media socket: STUN/DTLS/RTCP in
                pfds.push_back({c->rtc->fd(), POLLIN, 0});
                refs.push_back({c, 2});
            }
        }
        for (auto& c : clients_) if (c->mjpeg || c->ws_video || c->rtc) { timeout_ms = 20; break; }   // tick fast enough for the frame rate
        const size_t logs_idx = (logs_fd() >= 0) ? pfds.size() : (size_t)-1;
        if (logs_fd() >= 0) pfds.push_back({logs_fd(), POLLIN, 0});
        int n = poll(pfds.data(), pfds.size(), timeout_ms);
        int64_t t = now_ms();
        if (n > 0 && (pfds[0].revents & POLLIN)) accept_client();   // joins the NEXT poll cycle (not in refs)
        for (size_t i = 0; i < clients_.size(); ++i) {
            Client& c = *clients_[i];
            short re = 0, rre = 0, ure = 0;
            for (size_t k = 0; k < refs.size(); ++k)
                if (refs[k].c == &c) {
                    if (refs[k].kind == 1) rre = pfds[k + 1].revents;
                    else if (refs[k].kind == 2) ure = pfds[k + 1].revents;
                    else re = pfds[k + 1].revents;
                }
            bool ok = true;
            if (re & (POLLHUP | POLLERR | POLLNVAL)) ok = false;
            else if (re & POLLIN) {
                char buf[4096]; ssize_t r = recv(c.fd, buf, sizeof buf, MSG_DONTWAIT);
                if (r == 0) ok = false;
                else if (r < 0) { if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) ok = false; }
                else if (c.ws_video) { c.in.append(buf, (size_t)r); ok = ws_video_input(c); }
                else if (c.rtc_ws)   { c.in.append(buf, (size_t)r); ok = rtc_ws_input(c); }
                else if (c.sse || c.mjpeg || c.ws_logs) { /* ignore input on streaming connections */ }
                else { c.in.append(buf, (size_t)r); if (c.in.size() > input_cap(c.in)) ok = false; else ok = pump_requests(c); }
            }
            if (ok && c.relay_state != Client::Relay::None) {
                ok = pump_relay(c, rre, t);
                // A request that arrived WHILE the relay was in flight is still
                // sitting in c.in, and its POLLIN is long gone - the parse loop
                // above only runs on fresh input. Before downstream keep-alive
                // this could not happen, because every relayed reply closed the
                // connection; now it can, and an unparsed request would hang
                // there until the idle timeout.
                if (ok && c.relay_state == Client::Relay::None &&
                    !c.close_after_flush && !c.ws_video && !c.rtc_ws && !c.sse &&
                    !c.mjpeg && !c.ws_logs)
                    ok = pump_requests(c);
            }
            if (ok && c.sse) {
                drain_events(c);
                if (t - last_heartbeat_ms_ >= 15000) queue(c, ": keepalive\n\n");
            }
            if (ok && c.mjpeg && !c.close_after_flush) push_mjpeg(c);
            if (ok && c.ws_video && !c.close_after_flush) pump_ws_video(c);
            if (ok && c.rtc) {
                if (ure & POLLIN) c.rtc->on_readable();
                pump_rtc(c);
            }
            if (ok) ok = flush(c);
            if (ok && c.close_after_flush && c.out.empty()) ok = false;
            if (ok && !c.sse && !c.mjpeg && !c.ws_video && !c.rtc_ws && !c.ws_logs && t - c.last_activity_ms > cfg_.idle_timeout_ms) ok = false;
            if (!ok) {
                // Close now, erase after the iteration: refs holds pointers
                // into clients_, so the vector must not shift under it.
                if (c.ws_video) RuntimeStats::get().dec(&RuntimeCounters::ws_video_clients);
                if (c.ws_logs)  RuntimeStats::get().dec(&RuntimeCounters::ws_logs_clients);
                if (c.rtc)      RuntimeStats::get().dec(&RuntimeCounters::webrtc_sessions);
                if (c.sub) bus_.unsubscribe(c.sub);
                if (c.ws_sink) { StreamHub* h = c.ws_hub ? c.ws_hub : hub_; if (h) { c.ws_sink->close(); h->unsubscribe(c.ws_sink); } }
                if (c.rtc_sink) { StreamHub* h = c.rtc_hub ? c.rtc_hub : hub_; if (h) { c.rtc_sink->close(); h->unsubscribe(c.rtc_sink); } }
                c.rtc.reset();                          // closes the UDP socket
                // The two DemandHandles (ws_demand, rtc_demand) are NOT
                // released here: they are Client members and their destructors
                // do it when the erase below drops the unique_ptr. That is
                // safe because httpd is declared after the PipelineManager in
                // main() and therefore destroyed before it - release() calls
                // back into the manager.
                if (c.relay_fd >= 0) { close(c.relay_fd); c.relay_fd = -1; }
                close(c.fd); c.fd = -1;
            }
        }
        clients_.erase(std::remove_if(clients_.begin(), clients_.end(),
                                      [](const std::unique_ptr<Client>& p) { return p->fd < 0; }),
                       clients_.end());
        if (logs_idx != (size_t)-1 && logs_idx < pfds.size()) logs_pump(pfds[logs_idx].revents);
        // The reader keeps running with no subscribers on purpose: its pipe must
        // be drained or logread blocks on it. logs_pump discards when nobody
        // is listening.

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
