#include "app/rtsp/rtsp_server.hpp"
#include "app/rtsp/h264_nal.hpp"
#include "core/log.hpp"
#include "core/runtime_stats.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace machino {

static const char* MOD = "RTSP";
static int64_t now_ms() { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000; }
static const size_t RTP_MTU = 1400;

// defined below, used by refuse() above the parser section
static std::string header(const std::string& req, const char* name);

static int64_t mono_us() {
    struct timespec ts{}; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

using lifecycle::ConsumerType;
using lifecycle::DemandHandle;
using lifecycle::unit_name;

struct RtspServer::Session {
    int         fd = -1;
    int         unit = -1;                      // bound to a stream on the first DESCRIBE/SETUP/PLAY
    std::string peer;
    std::string session_id;
    bool        tcp = true;
    int         rtp_ch = 0, rtcp_ch = 1;
    int         udp_fd = -1;
    sockaddr_in udp_dst{};
    bool        playing = false;
    DemandHandle demand;               // RAII: alive only while PLAYing
    std::shared_ptr<Sink> sink;
    uint16_t    rtp_seq = 0;
    uint32_t    ssrc = 0;
    int64_t     pts0_us = -1;
    bool        wait_key = true;
    std::string inbuf;
    RtspAuth::Ctx auth;                // per-connection; dies with the socket
};

RtspServer::RtspServer(const RtspConfig& cfg, lifecycle::PipelineManager& pipeline, StreamHub& hub,
                       StreamHub* sub_hub, RtspAuth::CheckFn auth_check)
    : cfg_(cfg), pipeline_(pipeline), hub_(hub), sub_hub_(sub_hub),
      auth_(cfg.auth, std::move(auth_check)) {}

int RtspServer::unit_from_url(const std::string& url) const {
    // Extract the mount path from the request URL and compare it exactly, so an
    // unknown mount is rejected rather than silently treated as ch0. Accepts a
    // trailing "/trackID=..." control suffix.
    std::string path = url;
    size_t sch = path.find("://");
    if (sch != std::string::npos) { size_t sl = path.find('/', sch + 3); path = (sl == std::string::npos) ? "" : path.substr(sl); }
    size_t tr = path.find("/trackID"); if (tr != std::string::npos) path = path.substr(0, tr);
    if (!path.empty() && path.back() == '/') path.pop_back();
    if (path == cfg_.path) return lifecycle::UNIT_MAIN;
    if (sub_hub_ && !cfg_.sub_path.empty() && path == cfg_.sub_path) return lifecycle::UNIT_SUB;
    // Majestic drop-in aliases: the stock WebUI's "Stream URLs" page hands out
    // rtsp://CAM/stream=0 and /stream=1 - those must play against Machino too.
    if (path == "/stream=0") return lifecycle::UNIT_MAIN;
    if (sub_hub_ && path == "/stream=1") return lifecycle::UNIT_SUB;
    return -1;                                    // unknown mount
}
StreamHub* RtspServer::hub_for(int unit) const { return unit == lifecycle::UNIT_SUB ? sub_hub_ : &hub_; }
const std::string& RtspServer::path_for(int unit) const { return unit == lifecycle::UNIT_SUB ? cfg_.sub_path : cfg_.path; }

RtspServer::~RtspServer() { stop(); }

// Bind a listener on `port` and start the acceptor. Caller holds lifecycle_m_.
Result RtspServer::open_listener(int port) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return Result::error(errno);
    int one = 1; setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_ANY); a.sin_port = htons((uint16_t)port);
    if (bind(fd, (sockaddr*)&a, sizeof a) < 0 || listen(fd, 8) < 0) {
        int e = errno; LOGE(MOD, "bind/listen :%d failed: %s", port, strerror(e));
        close(fd); return Result::error(e);
    }
    listen_fd_ = fd;
    quit_ = false;
    acceptor_ = std::thread([this] { accept_loop(); });
    if (sub_hub_) LOGI(MOD, "listening on :%d paths %s (main) %s (sub) (pipeline stays cold until PLAY)", port, cfg_.path.c_str(), cfg_.sub_path.c_str());
    else          LOGI(MOD, "listening on :%d path %s (pipeline stays cold until PLAY)", port, cfg_.path.c_str());
    return Result::ok();
}

// Close the listener and end every session. Each session's DemandHandle
// releases as its thread unwinds, so the pipeline winds down through the
// normal grace path - disabling RTSP must not leave the encoder running.
// Caller holds lifecycle_m_.
void RtspServer::close_listener() {
    if (listen_fd_ < 0) return;
    quit_ = true;
    shutdown(listen_fd_, SHUT_RDWR); close(listen_fd_); listen_fd_ = -1;
    if (acceptor_.joinable()) acceptor_.join();          // no reaping runs after this
    std::vector<std::unique_ptr<Client>> all;
    {
        std::lock_guard<std::mutex> lk(clients_m_);
        all.swap(clients_);
        // the fd is read under the same lock the client thread clears it under,
        // so a number that was already closed (and possibly handed out again by
        // the kernel) is never shut down here
        for (auto& c : all) if (c->fd >= 0) shutdown(c->fd, SHUT_RDWR);
    }
    for (auto& c : all) if (c->th.joinable()) c->th.join();
}

Result RtspServer::start() {
    std::lock_guard<std::mutex> lk(lifecycle_m_);
    if (!cfg_.enabled) { LOGI(MOD, "disabled (rtsp.enabled=false): no listener"); return Result::ok(); }
    return open_listener(cfg_.port);
}

void RtspServer::stop() {
    std::lock_guard<std::mutex> lk(lifecycle_m_);
    if (listen_fd_ < 0) return;
    close_listener();
    LOGI(MOD, "stopped");
}

bool RtspServer::listening() const { return listen_fd_ >= 0; }

power::ApplyResult RtspServer::set_enabled(bool on) {
    using power::ApplyResult;
    std::lock_guard<std::mutex> lk(lifecycle_m_);
    if (on == cfg_.enabled && (on == (listen_fd_ >= 0)))
        return ApplyResult::applied(ApplyMode::Live, on ? 1 : 0, on ? 1 : 0, on ? "already listening" : "already disabled");
    if (!on) {
        close_listener();
        cfg_.enabled = false;
        LOGI(MOD, "disabled at runtime: listener closed, sessions ended");
        return ApplyResult::applied(ApplyMode::Live, 0, 0, "listener closed; sessions ended");
    }
    Result r = open_listener(cfg_.port);
    if (!r) return ApplyResult::rejected(ApplyMode::Live, 1, std::string("cannot bind port ") + std::to_string(cfg_.port));
    cfg_.enabled = true;
    return ApplyResult::applied(ApplyMode::Live, 1, 1, "listener bound");
}

// Atomic re-bind: the old listener only stays closed if the new port binds.
// If it does not, the previous port is restored so RTSP is never left dead.
power::ApplyResult RtspServer::set_port(int port) {
    using power::ApplyResult;
    if (port < 1 || port > 65535)
        return ApplyResult::rejected(ApplyMode::Live, port, "port must be in 1..65535");
    std::lock_guard<std::mutex> lk(lifecycle_m_);
    if (port == cfg_.port) return ApplyResult::applied(ApplyMode::Live, port, port, "unchanged");
    if (!cfg_.enabled || listen_fd_ < 0) {          // nothing bound: just record it
        cfg_.port = port;
        return ApplyResult::applied(ApplyMode::Live, port, port, "stored; rtsp is disabled");
    }
    const int old_port = cfg_.port;
    close_listener();
    if (open_listener(port)) { cfg_.port = port; return ApplyResult::applied(ApplyMode::Live, port, port, "re-bound"); }
    // roll back to the port we know worked; if even that fails we are honest
    // about it rather than pretending the change succeeded
    if (open_listener(old_port))
        return ApplyResult::rejected(ApplyMode::Live, port, "cannot bind port " + std::to_string(port) + "; kept " + std::to_string(old_port));
    cfg_.enabled = false;
    LOGE(MOD, "re-bind to :%d failed AND :%d could not be restored - rtsp is now down", port, old_port);
    return ApplyResult::rejected(ApplyMode::Live, port, "cannot bind " + std::to_string(port) + "; restoring " + std::to_string(old_port) + " also failed - rtsp disabled");
}

// Join and drop the clients that have finished. Without this every
// connect/disconnect cycle leaves a joinable std::thread behind: the list grows
// for the lifetime of the daemon and each entry keeps its thread stack, which a
// camera that reconnects all day notices long before a restart would clean up.
void RtspServer::reap_finished() {
    std::vector<std::unique_ptr<Client>> done;
    {
        std::lock_guard<std::mutex> lk(clients_m_);
        for (auto it = clients_.begin(); it != clients_.end();) {
            if ((*it)->done.load(std::memory_order_acquire)) { done.push_back(std::move(*it)); it = clients_.erase(it); }
            else ++it;
        }
    }
    for (auto& c : done) if (c->th.joinable()) c->th.join();   // never while holding clients_m_
}

// Over the configured limit. RTSP can only answer a request, so we take
// whatever already arrived in one non-blocking read - a flood of connections
// must not stall the accept loop - and answer that CSeq. Nothing yet: CSeq 0,
// which at least makes the refusal visible instead of a bare reset.
void RtspServer::refuse(int fd, const std::string& peer) {
    char buf[1024];
    ssize_t n = recv(fd, buf, sizeof buf - 1, MSG_DONTWAIT);
    std::string cseq = "0";
    if (n > 0) { buf[n] = 0; std::string got = header(std::string(buf, (size_t)n), "CSeq"); if (!got.empty()) cseq = got; }
    std::string resp = "RTSP/1.0 453 Not Enough Bandwidth\r\nCSeq: " + cseq + "\r\n\r\n";
    send(fd, resp.data(), resp.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
    close(fd);
    LOGW(MOD, "client %s refused: rtsp.max_clients = %d already connected", peer.c_str(), cfg_.max_clients);
}

void RtspServer::accept_loop() {
    while (!quit_) {
        reap_finished();                    // free the slots of clients that left
        pollfd p{listen_fd_, POLLIN, 0};
        if (poll(&p, 1, 250) <= 0) continue;
        sockaddr_in ca{}; socklen_t cl = sizeof ca;
        int fd = accept4(listen_fd_, (sockaddr*)&ca, &cl, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (fd < 0) continue;
        char ip[INET_ADDRSTRLEN]; inet_ntop(AF_INET, &ca.sin_addr, ip, sizeof ip);
        std::string peer = std::string(ip) + ":" + std::to_string(ntohs(ca.sin_port));
        size_t live; { std::lock_guard<std::mutex> lk(clients_m_); live = clients_.size(); }
        if ((int)live >= cfg_.max_clients) { refuse(fd, peer); continue; }
        int one = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        int snd = cfg_.send_buffer_bytes; setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &snd, sizeof snd);
        auto c = std::make_unique<Client>(); c->fd = fd;
        Client* slot = c.get();
        // start the thread before publishing the slot: stop() must never find a
        // Client whose thread has not been created yet and skip its join
        slot->th = std::thread([this, slot, peer] { client_loop(slot, peer); });
        std::lock_guard<std::mutex> lk(clients_m_);
        clients_.push_back(std::move(c));
    }
}

// Non-blocking send with a bounded stall: a client that stops reading for
// SEND_STALL_MS is treated as dead (false) so its demand gets released.
static bool send_all(int fd, const void* p, size_t n, int stall_limit_ms) {
    const uint8_t* b = (const uint8_t*)p; int stalled = 0;
    while (n) {
        ssize_t w = send(fd, b, n, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (w < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                pollfd pf{fd, POLLOUT, 0};
                if (poll(&pf, 1, 50) <= 0) { stalled += 50; if (stalled >= stall_limit_ms) return false; }
                continue;
            }
            return false;
        }
        b += w; n -= (size_t)w; stalled = 0;
    }
    return true;
}

void RtspServer::client_loop(Client* c, std::string peer) {
    const int fd = c->fd;
    Session s; s.fd = fd; s.peer = peer;
    struct timeval tv; gettimeofday(&tv, nullptr);
    s.ssrc = (uint32_t)(tv.tv_sec ^ (tv.tv_usec << 8) ^ (uint32_t)fd);
    s.session_id = std::to_string((unsigned long)(s.ssrc ^ 0x5a5a5a5aUL));
    LOGI(MOD, "client %s connected", peer.c_str());

    bool alive = true; char buf[2048];
    while (alive && !quit_) {
        pollfd p{fd, POLLIN, 0};
        int pr = poll(&p, 1, s.playing ? 0 : 200);
        if (pr > 0) {
            if (p.revents & (POLLHUP | POLLERR)) break;
            ssize_t n = recv(fd, buf, sizeof buf, 0);
            if (n == 0) break;
            if (n < 0) { if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) break; }
            else {
                s.inbuf.append(buf, (size_t)n);
                size_t end;
                while ((end = s.inbuf.find("\r\n\r\n")) != std::string::npos) {
                    std::string req = s.inbuf.substr(0, end + 4);
                    s.inbuf.erase(0, end + 4);
                    if (!handle_request(s, req)) { alive = false; break; }
                }
            }
        }
        if (alive && s.playing && s.sink) {
            AuPtr au; bool discontinuity = false;
            if (s.sink->pop(au, 20, &discontinuity)) {
                if (discontinuity) { if (StreamHub* h = hub_for(s.unit)) h->record_discontinuity(); s.wait_key = true; }
                if (s.wait_key && !au->key) continue;
                s.wait_key = false;
                if (!send_au(s, *au)) { LOGW(MOD, "%s: send stalled/failed - dropping client", peer.c_str()); alive = false; }
                else if (au->fetched_us > 0) { if (StreamHub* h = hub_for(s.unit)) h->record_out_to_send(mono_us() - au->fetched_us); }
            }
        }
    }
    if (s.sink) { StreamHub* h = hub_for(s.unit); if (h) h->unsubscribe(s.sink); }
    if (s.playing) RuntimeStats::get().dec(&RuntimeCounters::rtsp_sessions);
    s.demand.release();                                     // explicit for readability; the dtor would do it too
    if (s.udp_fd >= 0) close(s.udp_fd);
    // clear the fd before closing it: stop() must not shut down a number the
    // kernel may already have handed to a completely different socket
    { std::lock_guard<std::mutex> lk(clients_m_); c->fd = -1; }
    close(fd);
    LOGI(MOD, "client %s closed", peer.c_str());
    c->done.store(true, std::memory_order_release);     // last touch: the slot may be freed right after
}

static std::string header(const std::string& req, const char* name) {
    std::string key = std::string(name) + ":";
    size_t p = req.find(key); if (p == std::string::npos) return "";
    p += key.size(); while (p < req.size() && req[p] == ' ') ++p;
    size_t e = req.find("\r\n", p);
    return req.substr(p, e == std::string::npos ? std::string::npos : e - p);
}

// SPS/PPS for the SDP: from the cache, or via a scoped demand (the pipeline
// may start for it; the handle is released before returning - no leak).
bool RtspServer::obtain_params(int unit, std::vector<uint8_t>& sps, std::vector<uint8_t>& pps) {
    StreamHub* h = hub_for(unit);
    if (!h) return false;
    { std::lock_guard<std::mutex> lk(params_m_); if (!sps_[unit].empty() && !pps_[unit].empty()) { sps = sps_[unit]; pps = pps_[unit]; return true; } }
    DemandHandle d = pipeline_.acquire_unit(unit, ConsumerType::Rtsp);
    if (!d.active()) return false;
    auto sink = h->subscribe();
    pipeline_.request_idr(unit);
    bool ok = false;
    for (int i = 0; i < 150 && !ok && !quit_; ++i) {      // <= ~3 s
        AuPtr au; if (!sink->pop(au, 20)) continue;
        if (au->key && h264::extract_params(au->data.data(), au->data.size(), sps, pps)) ok = true;
    }
    h->unsubscribe(sink);
    if (ok) { std::lock_guard<std::mutex> lk(params_m_); sps_[unit] = sps; pps_[unit] = pps; }
    return ok;                                              // d released here
}

bool RtspServer::handle_request(Session& s, const std::string& req) {
    std::string method = req.substr(0, req.find(' '));
    std::string cseq = header(req, "CSeq"); if (cseq.empty()) cseq = "0";
    LOGD(MOD, "%s %s", s.peer.c_str(), method.c_str());
    // The request line is "METHOD url RTSP/1.0"; the url selects main vs sub.
    // A stream request for the substream mount when it is not configured is a
    // 404, not a silent fall-through to the main stream.
    if (method == "DESCRIBE" || method == "SETUP" || method == "PLAY") {
        size_t a = req.find(' '), b = (a == std::string::npos) ? std::string::npos : req.find(' ', a + 1);
        std::string url = (a != std::string::npos && b != std::string::npos) ? req.substr(a + 1, b - a - 1) : "";
        int ru = unit_from_url(url);
        auto quick = [&](const char* st) {
            std::string r = std::string("RTSP/1.0 ") + st + "\r\nCSeq: " + cseq + "\r\n\r\n";
            send_all(s.fd, r.data(), r.size(), cfg_.send_stall_ms);
        };
        if (ru < 0) { quick("404 Not Found"); return false; }              // unknown mount
        if (s.unit >= 0 && s.unit != ru) { quick("455 Method Not Valid in This State"); return true; }  // one session, one stream
        s.unit = ru;

        // Authorise BEFORE any path that can take demand or subscribe a sink:
        // an unauthenticated client must never start the sensor. Identical for
        // main and sub - the mount point does not change the rule.
        if (auth_.required()) {
            const RtspAuth::Verdict v = auth_.check(s.auth, method, url, header(req, "Authorization"), now_ms());
            if (v != RtspAuth::Verdict::Ok) {
                const std::string ch = auth_.challenge(s.auth, now_ms(), v == RtspAuth::Verdict::Stale);
                std::string extra;
                size_t at = 0;
                while (at <= ch.size()) {                       // one header line per offered scheme
                    const size_t nl = ch.find('\n', at);
                    const std::string one = ch.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
                    if (!one.empty()) extra += "WWW-Authenticate: " + one + "\r\n";
                    if (nl == std::string::npos) break;
                    at = nl + 1;
                }
                // never log the credential, only that it was refused
                LOGW(MOD, "%s %s %s: unauthorised", s.peer.c_str(), method.c_str(), unit_name(s.unit));
                std::string r = "RTSP/1.0 401 Unauthorized\r\nCSeq: " + cseq + "\r\n" + extra + "\r\n";
                send_all(s.fd, r.data(), r.size(), cfg_.send_stall_ms);
                return true;                                     // keep the connection for the retry
            }
        }
    }

    auto reply = [&](const char* status, const std::string& extra, const std::string& body) {
        std::string out = std::string("RTSP/1.0 ") + status + "\r\nCSeq: " + cseq + "\r\nServer: machino/" MACHINO_VERSION "\r\n" + extra;
        if (!body.empty()) out += "Content-Type: application/sdp\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
        out += "\r\n"; out += body;
        return send_all(s.fd, out.data(), out.size(), cfg_.send_stall_ms);
    };

    if (method == "OPTIONS")
        return reply("200 OK", "Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN, GET_PARAMETER\r\n", "");

    if (method == "DESCRIBE") {
        std::vector<uint8_t> sps, pps;
        if (!obtain_params(s.unit, sps, pps)) { LOGW(MOD, "DESCRIBE %s: no SPS/PPS available", unit_name(s.unit)); return reply("503 Service Unavailable", "", ""); }
        char plid[8]; unsigned p1 = sps.size() > 3 ? sps[1] : 0, p2 = sps.size() > 3 ? sps[2] : 0, p3 = sps.size() > 3 ? sps[3] : 0;
        snprintf(plid, sizeof plid, "%02X%02X%02X", p1, p2, p3);
        std::string body = "v=0\r\no=- 0 0 IN IP4 0.0.0.0\r\ns=Machino\r\nt=0 0\r\na=control:*\r\n"
               "m=video 0 RTP/AVP 96\r\nc=IN IP4 0.0.0.0\r\na=rtpmap:96 H264/90000\r\n"
               "a=fmtp:96 packetization-mode=1;profile-level-id=" + std::string(plid) +
               ";sprop-parameter-sets=" + h264::base64(sps.data(), sps.size()) + "," + h264::base64(pps.data(), pps.size()) +
               "\r\na=control:trackID=0\r\n";
        return reply("200 OK", "", body);
    }

    if (method == "SETUP") {
        std::string tr = header(req, "Transport");
        if (tr.find("RTP/AVP/TCP") != std::string::npos || tr.find("interleaved") != std::string::npos) {
            s.tcp = true;
            size_t p = tr.find("interleaved=");
            if (p != std::string::npos) { s.rtp_ch = atoi(tr.c_str() + p + 12); s.rtcp_ch = s.rtp_ch + 1; }
            char t[128]; snprintf(t, sizeof t, "Transport: RTP/AVP/TCP;unicast;interleaved=%d-%d\r\nSession: %s;timeout=60\r\n", s.rtp_ch, s.rtcp_ch, s.session_id.c_str());
            return reply("200 OK", t, "");
        }
        size_t p = tr.find("client_port=");
        if (p == std::string::npos) return reply("461 Unsupported Transport", "", "");
        int cport = atoi(tr.c_str() + p + 12);
        s.tcp = false;
        s.udp_fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        int snd = cfg_.send_buffer_bytes; setsockopt(s.udp_fd, SOL_SOCKET, SO_SNDBUF, &snd, sizeof snd);
        sockaddr_in la{}; la.sin_family = AF_INET; la.sin_addr.s_addr = htonl(INADDR_ANY); la.sin_port = 0;
        if (bind(s.udp_fd, (sockaddr*)&la, sizeof la) < 0) return reply("500 Internal Server Error", "", "");
        socklen_t ll = sizeof la; getsockname(s.udp_fd, (sockaddr*)&la, &ll);
        sockaddr_in pa{}; socklen_t pl = sizeof pa; getpeername(s.fd, (sockaddr*)&pa, &pl);
        s.udp_dst = pa; s.udp_dst.sin_port = htons((uint16_t)cport);
        char t[160]; snprintf(t, sizeof t, "Transport: RTP/AVP;unicast;client_port=%d-%d;server_port=%d-%d\r\nSession: %s;timeout=60\r\n",
                              cport, cport + 1, ntohs(la.sin_port), ntohs(la.sin_port) + 1, s.session_id.c_str());
        return reply("200 OK", t, "");
    }

    if (method == "PLAY") {
        if (!s.playing) {
            StreamHub* h = hub_for(s.unit);
            if (!h) return reply("404 Not Found", "", "");
            Result r; s.demand = pipeline_.acquire_unit(s.unit, ConsumerType::Rtsp, &r);
            if (!s.demand.active()) { LOGW(MOD, "%s PLAY %s: pipeline unavailable (%s)", s.peer.c_str(), unit_name(s.unit), status_name(r.status)); return reply("503 Service Unavailable", "", ""); }
            s.sink = h->subscribe();   // bounded profile depth; a stalled client drops its own frames only
            s.playing = true; s.wait_key = true; s.pts0_us = -1;
            RuntimeStats::get().inc(&RuntimeCounters::rtsp_sessions);
            pipeline_.request_idr(s.unit);
            LOGI(MOD, "%s PLAY %s (%s)", s.peer.c_str(), unit_name(s.unit), s.tcp ? "tcp-interleaved" : "udp");
        }
        return reply("200 OK", "Session: " + s.session_id + "\r\nRange: npt=0.000-\r\nRTP-Info: url=" + path_for(s.unit) + "/trackID=0;seq=" + std::to_string(s.rtp_seq) + "\r\n", "");
    }

    if (method == "TEARDOWN") {
        reply("200 OK", "Session: " + s.session_id + "\r\n", "");
        return false;   // close connection -> client_loop releases the demand
    }

    if (method == "GET_PARAMETER" || method == "SET_PARAMETER")
        return reply("200 OK", "Session: " + s.session_id + "\r\n", "");

    return reply("405 Method Not Allowed", "", "");
}

bool RtspServer::send_rtp(Session& s, const uint8_t* payload, size_t len, uint32_t ts, bool marker) {
    uint8_t pkt[4 + 12 + RTP_MTU]; size_t off = 0;
    if (s.tcp) { pkt[0] = '$'; pkt[1] = (uint8_t)s.rtp_ch; pkt[2] = (uint8_t)((12 + len) >> 8); pkt[3] = (uint8_t)(12 + len); off = 4; }
    uint8_t* h = pkt + off;
    h[0] = 0x80; h[1] = (uint8_t)(96 | (marker ? 0x80 : 0));
    h[2] = (uint8_t)(s.rtp_seq >> 8); h[3] = (uint8_t)s.rtp_seq; ++s.rtp_seq;
    h[4] = (uint8_t)(ts >> 24); h[5] = (uint8_t)(ts >> 16); h[6] = (uint8_t)(ts >> 8); h[7] = (uint8_t)ts;
    h[8] = (uint8_t)(s.ssrc >> 24); h[9] = (uint8_t)(s.ssrc >> 16); h[10] = (uint8_t)(s.ssrc >> 8); h[11] = (uint8_t)s.ssrc;
    memcpy(h + 12, payload, len);
    size_t total = off + 12 + len;
    if (s.tcp) return send_all(s.fd, pkt, total, cfg_.send_stall_ms);
    return sendto(s.udp_fd, pkt, total, MSG_NOSIGNAL, (sockaddr*)&s.udp_dst, sizeof s.udp_dst) == (ssize_t)total;
}

bool RtspServer::send_au(Session& s, const AccessUnit& au) {
    if (s.pts0_us < 0) s.pts0_us = au.pts_us;
    uint32_t ts = (uint32_t)((au.pts_us - s.pts0_us) * 90 / 1000);
    h264::Nal nal[32]; size_t c = h264::split(au.data.data(), au.data.size(), nal, 32);
    for (size_t i = 0; i < c; ++i) {
        bool last = (i + 1 == c);
        const uint8_t* p = nal[i].p; size_t n = nal[i].len;
        if (nal[i].type == 7 || nal[i].type == 8) {
            std::lock_guard<std::mutex> lk(params_m_);
            (nal[i].type == 7 ? sps_[s.unit] : pps_[s.unit]).assign(p, p + n);
        }
        if (n <= RTP_MTU) { if (!send_rtp(s, p, n, ts, last)) return false; continue; }
        uint8_t hdr = p[0]; uint8_t fu_ind = (uint8_t)((hdr & 0xe0) | 28);
        size_t pos = 1; bool first = true;
        uint8_t buf[RTP_MTU];
        while (pos < n) {
            size_t chunk = n - pos; if (chunk > RTP_MTU - 2) chunk = RTP_MTU - 2;
            bool end = (pos + chunk == n);
            buf[0] = fu_ind;
            buf[1] = (uint8_t)((first ? 0x80 : 0) | (end ? 0x40 : 0) | (hdr & 0x1f));
            memcpy(buf + 2, p + pos, chunk);
            if (!send_rtp(s, buf, chunk + 2, ts, last && end)) return false;
            pos += chunk; first = false;
        }
    }
    return true;
}

} // namespace machino
