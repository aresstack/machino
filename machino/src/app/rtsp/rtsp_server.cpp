#include "app/rtsp/rtsp_server.hpp"
#include "app/http/audio_stream.hpp"
#include "app/rtp/rtp_packet.hpp"
#include "app/rtsp/rtp_audio.hpp"
#include "app/rtsp/rtp_jpeg.hpp"
#include <algorithm>
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

// AP30: the largest request this server will buffer before giving up on a
// connection. Same ceiling as the HTTP head - a real RTSP request is a few
// hundred bytes.
static const size_t MAX_RTSP_REQUEST = 8192;
static int64_t now_ms() { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000; }
static const size_t RTP_MTU = 1400;
// A connection that sends no request for this long before it PLAYs is
// reaped: the session timeout the server advertises (timeout=60).
static const int kIdleBeforePlayMs = 60000;

// defined below, used by refuse() above the parser section
static std::string header(const std::string& req, const char* name);

static int64_t mono_us() {
    struct timespec ts{}; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

using lifecycle::ConsumerType;
using lifecycle::DemandHandle;
using lifecycle::unit_name;

// One RTP flow of a session - the video (trackID 0), the microphone (1) or
// the ONVIF backchannel (2). The negotiation and the transport state are the
// same for all three, so they are one struct, not three sets of fields.
struct RtspServer::Track {
    bool        setup = false;
    bool        tcp = true;
    int         rtp_ch = 0, rtcp_ch = 1;      // interleaved channels on the RTSP connection
    int         udp_fd = -1;
    // The RTSP peer's address at the client_port it announced: where the
    // camera's RTP goes, and - for the backchannel - the ONLY source whose
    // datagrams are audio for the speaker.
    sockaddr_in udp_peer{};
    uint16_t    seq = 0;
    uint32_t    ssrc = 0;
    uint32_t    ts = 0;
    void close_socket() { if (udp_fd >= 0) { ::close(udp_fd); udp_fd = -1; } }
};

struct RtspServer::Session {
    int         fd = -1;
    int         unit = -1;                      // bound to a stream on the first DESCRIBE/SETUP/PLAY
    std::string peer;
    std::string session_id;
    Track       video, audio, back;             // trackID 0, 1, 2
    bool        playing = false;
    DemandHandle demand;               // RAII: alive only while PLAYing
    std::shared_ptr<Sink> sink;
    int64_t     pts0_us = -1;
    bool        wait_key = true;
    std::string inbuf;
    int64_t     last_request_ms = 0;   // a connection that never asks for anything is reaped
    RtspAuth::Ctx auth;                // per-connection; dies with the socket
    // The audio track (trackID=1), when the client SETs it UP.
    int         a_rate = 0;            // capture rate of the listener
    std::shared_ptr<Sink> a_sink;      // AudioService listener, only while PLAYing
    uint32_t    a_last_seq = 0;        // of the last frame sent: a gap means the sink dropped frames
    bool        a_seq_valid = false;
    // The ONVIF backchannel (trackID=2): the client talks, the speaker plays.
    bool        bc_refused_logged = false;
    bool        bc_source_logged = false;
    uint32_t    bc_ssrc = 0;           // the one talker this session plays
    bool        bc_ssrc_valid = false;
    int64_t     bc_last_ms = 0;
    // The MJPEG mount (unit JPEG): frames are pulled from the JPEG unit at
    // the configured rate instead of popped from a hub.
    int64_t     jpeg_next_us = 0;
    int64_t     jpeg_t0_us = -1;
    std::string jpeg_last_why;
    bool is_jpeg() const { return unit == lifecycle::UNIT_JPEG; }
};

RtspServer::RtspServer(const RtspConfig& cfg, lifecycle::PipelineManager& pipeline, StreamHub& hub,
                       StreamHub* sub_hub, RtspAuth::CheckFn auth_check,
                       RtspAuth::ClaimFn claimed, bool unsafe)
    : cfg_(cfg), pipeline_(pipeline), hub_(hub), sub_hub_(sub_hub),
      auth_(cfg.auth, std::move(auth_check), nullptr, std::move(claimed), unsafe) {}

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
    // MJPEG over RTP (RFC 2435) from the JPEG unit, where one is configured.
    if (path == "/stream=2" && pipeline_.unit_configured(lifecycle::UNIT_JPEG)) return lifecycle::UNIT_JPEG;
    return -1;                                    // unknown mount
}
StreamHub* RtspServer::hub_for(int unit) const { return unit == lifecycle::UNIT_SUB ? sub_hub_ : &hub_; }
const std::string& RtspServer::path_for(int unit) const {
    static const std::string kJpegPath = "/stream=2";
    return unit == lifecycle::UNIT_JPEG ? kJpegPath : unit == lifecycle::UNIT_SUB ? cfg_.sub_path : cfg_.path;
}

RtspServer::~RtspServer() { stop(); }

// Bind and listen, WITHOUT touching any listener already running. Returns the
// fd or -1. Kept separate from adoption so a re-bind can prove the new port
// works before the working one is given up (AP3.5).
int RtspServer::bind_listener(int port) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) { LOGE(MOD, "socket: %s", strerror(errno)); return -1; }
    int one = 1; setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_ANY); a.sin_port = htons((uint16_t)port);
    if (bind(fd, (sockaddr*)&a, sizeof a) < 0 || listen(fd, 8) < 0) {
        const int e = errno; LOGE(MOD, "bind/listen :%d failed: %s", port, strerror(e));
        close(fd); errno = e; return -1;
    }
    return fd;
}

// Take ownership of an already-bound fd and start accepting on it.
// Caller holds lifecycle_m_ and has ensured no listener is running.
void RtspServer::adopt_listener(int fd, int port) {
    listen_fd_ = fd;
    quit_ = false;
    acceptor_ = std::thread([this] { accept_loop(); });
    if (sub_hub_) LOGI(MOD, "listening on :%d paths %s (main) %s (sub) (pipeline stays cold until PLAY)", port, cfg_.path.c_str(), cfg_.sub_path.c_str());
    else          LOGI(MOD, "listening on :%d path %s (pipeline stays cold until PLAY)", port, cfg_.path.c_str());
}

// Bind and adopt in one step, for the paths that have no listener to protect.
Result RtspServer::open_listener(int port) {
    const int fd = bind_listener(port);
    if (fd < 0) return Result::error(errno ? errno : -1);
    adopt_listener(fd, port);
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
    // Prove the new port works BEFORE giving up the one that does. Two
    // different ports never collide, so this needs no window in which RTSP is
    // down - and it removes the failure mode the previous close-then-open had,
    // where a failed re-bind could also fail to restore the old port and leave
    // the camera with no RTSP at all.
    const int fd = bind_listener(port);
    if (fd < 0)
        return ApplyResult::rejected(ApplyMode::Live, port,
            "cannot bind port " + std::to_string(port) + "; kept " + std::to_string(old_port));
    close_listener();                 // ends existing sessions; their demand releases
    adopt_listener(fd, port);
    cfg_.port = port;
    return ApplyResult::applied(ApplyMode::Live, port, port, "re-bound");
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
    s.video.ssrc = (uint32_t)(tv.tv_sec ^ (tv.tv_usec << 8) ^ (uint32_t)fd);
    s.session_id = std::to_string((unsigned long)(s.video.ssrc ^ 0x5a5a5a5aUL));
    s.audio.ssrc = s.video.ssrc ^ 0xa5a5a5a5u;
    s.audio.rtp_ch = 2; s.audio.rtcp_ch = 3;
    s.back.rtp_ch = 4;  s.back.rtcp_ch = 5;
    s.last_request_ms = now_ms();
    LOGI(MOD, "client %s connected", peer.c_str());

    bool alive = true; char buf[2048];
    while (alive && !quit_) {
        pollfd p{fd, POLLIN, 0};
        int wait = s.playing ? 0 : 200;
        if (s.playing && s.is_jpeg()) {              // no hub to block on: sleep until the next frame is due
            const int64_t left = (s.jpeg_next_us - mono_us()) / 1000;
            wait = left > 0 ? (int)std::min<int64_t>(left, 200) : 0;
            if (s.a_sink && wait > 40) wait = 40;    // the microphone delivers every 40 ms
        }
        int pr = poll(&p, 1, wait);
        if (pr > 0) {
            if (p.revents & (POLLHUP | POLLERR)) break;
            ssize_t n = recv(fd, buf, sizeof buf, 0);
            if (n == 0) break;
            if (n < 0) { if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) break; }
            else {
                s.inbuf.append(buf, (size_t)n);
                // Interleaved binary frames ('$', RFC 2326 10.12) share this
                // socket with the text requests: RTCP receiver reports from
                // every TCP client, and the backchannel's RTP. They are taken
                // off the front before any request parsing; before this, a
                // client's RTCP piled up as "a request that never ends" and
                // the client was dropped at MAX_RTSP_REQUEST.
                for (;;) {
                    int ich = -1; std::string idata;
                    const rtsp::Interleaved st = rtsp::take_interleaved(s.inbuf, ich, idata);
                    if (st == rtsp::Interleaved::Frame) {
                        if (s.back.setup && s.back.tcp && ich == s.back.rtp_ch) on_backchannel(s, idata.data(), idata.size());
                        continue;                            // anything else on the wire is RTCP: ignored
                    }
                    if (st == rtsp::Interleaved::TooLarge) {
                        LOGW(MOD, "%s: interleaved frame over %zu bytes - dropping", s.peer.c_str(), rtsp::kMaxInterleaved);
                        alive = false; break;
                    }
                    if (st == rtsp::Interleaved::Partial) break;
                    const size_t rend = s.inbuf.find("\r\n\r\n");
                    if (rend == std::string::npos) break;
                    // A frame after this request is picked up on the next pass.
                    std::string req = s.inbuf.substr(0, rend + 4);
                    s.inbuf.erase(0, rend + 4);
                    s.last_request_ms = now_ms();
                    if (!handle_request(s, req)) { alive = false; break; }
                }
                if (!alive) break;
                // AP30: bound it. This loop only drains inbuf when it finds a
                // blank line, so a client that connects and sends bytes
                // without ever ending a request grew it without limit - before
                // any authentication, because auth lives inside
                // handle_request, which is never reached. On a camera with
                // 42 MB of RAM and an OOM already in its history, one socket
                // was enough. The HTTP side has had MAX_IN for this reason;
                // RTSP had nothing.
                //
                // 8 KiB is the same ceiling as the HTTP head: a real DESCRIBE
                // or SETUP with Authorization and Transport is a few hundred
                // bytes, so this refuses only what was never a request. An
                // interleaved frame in progress is bounded separately, by
                // kMaxInterleaved (take_interleaved refuses a longer one), so
                // the '$' prefix is no way around the ceiling.
                const size_t bound = (!s.inbuf.empty() && s.inbuf[0] == '$') ? 4 + rtsp::kMaxInterleaved : MAX_RTSP_REQUEST;
                if (s.inbuf.size() > bound) {
                    LOGW(MOD, "%s: %s exceeded %zu bytes without ending - dropping", s.peer.c_str(),
                         s.inbuf[0] == '$' ? "interleaved frame" : "request", bound);
                    break;
                }
            }
        }
        // A connection that has not asked for anything within the session
        // timeout the server itself advertises is not a client: without
        // this, a socket parked before PLAY holds its thread for ever.
        if (!s.playing && now_ms() - s.last_request_ms > kIdleBeforePlayMs) {
            LOGI(MOD, "%s: no request within %d s - closing", s.peer.c_str(), kIdleBeforePlayMs / 1000);
            break;
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
        if (alive && s.playing && s.is_jpeg() && mono_us() >= s.jpeg_next_us) {
            if (!send_jpeg(s)) { LOGW(MOD, "%s: send stalled/failed - dropping client", peer.c_str()); alive = false; }
        }
        if (alive && s.back.setup && !s.back.tcp && s.back.udp_fd >= 0) pump_backchannel_udp(s);
        if (alive && s.playing && s.a_sink) {
            // Everything the microphone delivered since the last pass. The
            // pass is paced by the video pop (20 ms) or the JPEG timer (40 ms
            // with audio), and a bounded drain must cover more than one pass:
            // the old ceiling of four frames per 200 ms JPEG pass lost a
            // fifth of the audio for good.
            for (int k = 0; k < 64 && alive; ++k) {
                AuPtr a;
                if (!s.a_sink->pop(a, 0)) break;
                if (!a || a->data.empty()) continue;
                std::string pcma;
                http::audio_encode(http::AudioFormat::Alaw, s.a_rate, a->data.data(), a->data.size(), pcma);
                // Frames the sink dropped for a slow client still took their
                // time: the RTP clock skips them, or the receiver would play
                // everything after the gap early and drift from the video.
                if (s.a_seq_valid && a->seq > s.a_last_seq + 1) s.audio.ts += (uint32_t)((a->seq - s.a_last_seq - 1) * pcma.size());
                s.a_last_seq = a->seq; s.a_seq_valid = true;
                if (!send_audio(s, pcma)) { LOGW(MOD, "%s: audio send stalled/failed - dropping client", peer.c_str()); alive = false; }
            }
            if (s.a_sink && s.a_sink->closed()) {           // microphone switched off: the video goes on
                if (audio_) audio_->unlisten(s.a_sink);
                s.a_sink.reset();
                LOGI(MOD, "%s: audio track ended (microphone off)", peer.c_str());
            }
        }
    }
    if (s.a_sink && audio_) audio_->unlisten(s.a_sink);
    s.audio.close_socket(); s.back.close_socket();
    if (s.sink) { StreamHub* h = hub_for(s.unit); if (h) h->unsubscribe(s.sink); }
    if (s.playing) RuntimeStats::get().dec(&RuntimeCounters::rtsp_sessions);
    s.demand.release();                                     // explicit for readability; the dtor would do it too
    s.video.close_socket();
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

    if (method == "DESCRIBE" && s.unit == lifecycle::UNIT_JPEG) {
        // One capture up front: a JPEG unit whose frames RFC 2435 cannot carry
        // (too large, unusual sampling) is refused here, not after PLAY.
        std::vector<uint8_t> jpg; std::string err, why; rtsp::JpegFrame jf;
        const Result r = pipeline_.snapshot(jpg, err, 3000);
        if (!r) { LOGW(MOD, "DESCRIBE jpeg: %s", err.c_str()); return reply("503 Service Unavailable", "", ""); }
        if (!rtsp::parse_jpeg(jpg.data(), jpg.size(), jf, why)) { LOGW(MOD, "DESCRIBE jpeg: %s", why.c_str()); return reply("415 Unsupported Media Type", "", ""); }
        std::string body = "v=0\r\no=- 0 0 IN IP4 0.0.0.0\r\ns=Machino\r\nt=0 0\r\na=control:*\r\n" + rtsp::sdp_jpeg_section();
        if (audio_offered()) body += rtsp::sdp_audio_section();
        return reply("200 OK", "", body);
    }

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
        if (audio_offered()) body += rtsp::sdp_audio_section();
        // ONVIF talkback: only for a client that asks for it (Require header)
        // and only while the speaker is switched on.
        if (rtsp::wants_backchannel(header(req, "Require")) && backchannel_offered())
            body += rtsp::sdp_backchannel_section();
        return reply("200 OK", "", body);
    }

    if (method == "SETUP") {
        const std::string tr = header(req, "Transport");
        size_t ua = req.find(' '), ub = (ua == std::string::npos) ? std::string::npos : req.find(' ', ua + 1);
        const std::string surl = (ua != std::string::npos && ub != std::string::npos) ? req.substr(ua + 1, ub - ua - 1) : "";
        const int track = rtsp::track_from_url(surl);
        Track* t = nullptr;
        if      (track == rtsp::kBackchannelTrack) { if (!backchannel_offered()) return reply("404 Not Found", "", ""); t = &s.back; }
        else if (track == rtsp::kAudioTrack)       { if (!audio_offered())       return reply("404 Not Found", "", ""); t = &s.audio; }
        else                                       t = &s.video;
        std::string th;
        const char* st = setup_transport(s, *t, tr, th);
        return reply(st, th, "");
    }

    if (method == "PLAY") {
        if (!s.playing && s.is_jpeg()) {
            // No demand handle: every frame is a snapshot, which takes and
            // drops JPEG demand itself; the unit's grace keeps it warm.
            s.playing = true; s.jpeg_next_us = 0; s.jpeg_t0_us = -1;
            RuntimeStats::get().inc(&RuntimeCounters::rtsp_sessions);
            LOGI(MOD, "%s PLAY jpeg (%s)", s.peer.c_str(), s.video.tcp ? "tcp-interleaved" : "udp");
        } else if (!s.playing) {
            StreamHub* h = hub_for(s.unit);
            if (!h) return reply("404 Not Found", "", "");
            Result r; s.demand = pipeline_.acquire_unit(s.unit, ConsumerType::Rtsp, &r);
            if (!s.demand.active()) { LOGW(MOD, "%s PLAY %s: pipeline unavailable (%s)", s.peer.c_str(), unit_name(s.unit), status_name(r.status)); return reply("503 Service Unavailable", "", ""); }
            s.sink = h->subscribe();   // bounded profile depth; a stalled client drops its own frames only
            s.playing = true; s.wait_key = true; s.pts0_us = -1;
            RuntimeStats::get().inc(&RuntimeCounters::rtsp_sessions);
            pipeline_.request_idr(s.unit);
            LOGI(MOD, "%s PLAY %s (%s)", s.peer.c_str(), unit_name(s.unit), s.video.tcp ? "tcp-interleaved" : "udp");
        }
        if (s.audio.setup && audio_ && !s.a_sink) {
            // The microphone is a listener like any other. When it cannot
            // open, the session still plays - video without sound beats a
            // refused PLAY.
            std::string why;
            s.a_sink = audio_->listen(why);
            s.a_rate = audio_->sample_rate();
            s.a_seq_valid = false;
            if (s.a_sink) LOGI(MOD, "%s PLAY audio (PCMA, %s)", s.peer.c_str(), s.audio.tcp ? "tcp-interleaved" : "udp");
            else          LOGW(MOD, "%s PLAY audio refused: %s - video only", s.peer.c_str(), why.c_str());
        }
        std::string info = "RTP-Info: url=" + path_for(s.unit) + "/trackID=0;seq=" + std::to_string(s.video.seq);
        if (s.audio.setup) info += ",url=" + path_for(s.unit) + "/trackID=" + std::to_string(rtsp::kAudioTrack) + ";seq=" + std::to_string(s.audio.seq);
        return reply("200 OK", "Session: " + s.session_id + "\r\nRange: npt=0.000-\r\n" + info + "\r\n", "");
    }

    if (method == "TEARDOWN") {
        reply("200 OK", "Session: " + s.session_id + "\r\n", "");
        return false;   // close connection -> client_loop releases the demand
    }

    if (method == "GET_PARAMETER" || method == "SET_PARAMETER")
        return reply("200 OK", "Session: " + s.session_id + "\r\n", "");

    return reply("405 Method Not Allowed", "", "");
}

// The Transport negotiation every track shares (RFC 2326 12.39): RTP/AVP/TCP
// interleaved on this connection, or RTP/AVP unicast UDP to the client's
// port. Fills the reply headers; returns the status line.
const char* RtspServer::setup_transport(Session& s, Track& t, const std::string& tr, std::string& headers) {
    char line[200];
    if (tr.find("RTP/AVP/TCP") != std::string::npos || tr.find("interleaved") != std::string::npos) {
        int rtp = 0, rtcp = 0;
        if (rtsp::interleaved_channels(tr, rtp, rtcp)) { t.rtp_ch = rtp; t.rtcp_ch = rtcp; }
        t.setup = true; t.tcp = true;
        t.close_socket();                                        // a re-SETUP that switched transports
        snprintf(line, sizeof line, "Transport: RTP/AVP/TCP;unicast;interleaved=%d-%d\r\nSession: %s;timeout=60\r\n",
                 t.rtp_ch, t.rtcp_ch, s.session_id.c_str());
        headers = line;
        return "200 OK";
    }
    const size_t p = tr.find("client_port=");
    if (p == std::string::npos) return "461 Unsupported Transport";
    const int cport = atoi(tr.c_str() + p + 12);
    if (cport <= 0 || cport > 65535) return "461 Unsupported Transport";
    t.close_socket();                                            // a second SETUP gets a fresh socket, never a leaked one
    t.udp_fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (t.udp_fd < 0) return "500 Internal Server Error";
    int snd = cfg_.send_buffer_bytes; setsockopt(t.udp_fd, SOL_SOCKET, SO_SNDBUF, &snd, sizeof snd);
    sockaddr_in la{}; la.sin_family = AF_INET; la.sin_addr.s_addr = htonl(INADDR_ANY); la.sin_port = 0;
    if (bind(t.udp_fd, (sockaddr*)&la, sizeof la) < 0) { t.close_socket(); return "500 Internal Server Error"; }
    socklen_t ll = sizeof la; getsockname(t.udp_fd, (sockaddr*)&la, &ll);
    // The peer is the RTSP connection's address - the one that authenticated -
    // at the port the client named. Never a port on some other host.
    sockaddr_in pa{}; socklen_t pl = sizeof pa; getpeername(s.fd, (sockaddr*)&pa, &pl);
    t.udp_peer = pa; t.udp_peer.sin_port = htons((uint16_t)cport);
    t.setup = true; t.tcp = false;
    snprintf(line, sizeof line, "Transport: RTP/AVP;unicast;client_port=%d-%d;server_port=%d-%d\r\nSession: %s;timeout=60\r\n",
             cport, cport + 1, ntohs(la.sin_port), ntohs(la.sin_port) + 1, s.session_id.c_str());
    headers = line;
    return "200 OK";
}

// One RTP packet on a track: interleaved on the connection, or a datagram to
// the track's peer. The track's own sequence number advances.
bool RtspServer::send_rtp(Session& s, Track& t, uint8_t pt, const uint8_t* payload, size_t len, uint32_t ts, bool marker) {
    uint8_t pkt[4 + rtp::kHeaderBytes + RTP_MTU]; size_t off = 0;
    if (t.tcp) { pkt[0] = '$'; pkt[1] = (uint8_t)t.rtp_ch; pkt[2] = (uint8_t)((rtp::kHeaderBytes + len) >> 8); pkt[3] = (uint8_t)(rtp::kHeaderBytes + len); off = 4; }
    rtp::write_header(pkt + off, pt, marker, t.seq++, ts, t.ssrc);
    memcpy(pkt + off + rtp::kHeaderBytes, payload, len);
    const size_t total = off + rtp::kHeaderBytes + len;
    if (t.tcp) return send_all(s.fd, pkt, total, cfg_.send_stall_ms);
    return sendto(t.udp_fd, pkt, total, MSG_NOSIGNAL, (sockaddr*)&t.udp_peer, sizeof t.udp_peer) == (ssize_t)total;
}

bool RtspServer::backchannel_offered() const {
    return audio_ && audio_->output_available() && audio_->config().output_enabled;
}

// The UDP backchannel port is reachable by every host on the LAN, and the
// RTSP authentication covered the control connection, not this port. Only
// datagrams from the RTSP peer's address, at the port it named in SETUP, are
// audio for the speaker; anything else is dropped and noted once.
void RtspServer::pump_backchannel_udp(Session& s) {
    uint8_t ub[1600];
    for (int k = 0; k < 16; ++k) {
        sockaddr_in from{}; socklen_t fl = sizeof from;
        const ssize_t un = recvfrom(s.back.udp_fd, ub, sizeof ub, MSG_DONTWAIT, (sockaddr*)&from, &fl);
        if (un <= 0) break;
        if (from.sin_addr.s_addr != s.back.udp_peer.sin_addr.s_addr || from.sin_port != s.back.udp_peer.sin_port) {
            if (!s.bc_source_logged) {
                char ip[INET_ADDRSTRLEN]; inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);
                LOGW(MOD, "%s: backchannel datagram from %s:%d ignored - not the RTSP peer at its client_port",
                     s.peer.c_str(), ip, ntohs(from.sin_port));
                s.bc_source_logged = true;
            }
            continue;
        }
        on_backchannel(s, reinterpret_cast<const char*>(ub), (size_t)un);
    }
}

void RtspServer::on_backchannel(Session& s, const char* p, size_t n) {
    if (!audio_) return;
    rtp::Header h;
    const uint8_t* u = reinterpret_cast<const uint8_t*>(p);
    if (!rtp::parse(u, n, h) || h.payload_len == 0) return;
    // UDP: one talker per session - the SSRC first heard is the one played,
    // a second source is ignored until the first has been silent for two
    // seconds (a client that restarted its sender). Interleaved on the
    // connection the source IS the authenticated client: no lock needed,
    // and a sender restart must not be muted.
    if (!s.back.tcp) {
        const int64_t t = now_ms();
        if (s.bc_ssrc_valid && h.ssrc != s.bc_ssrc && t - s.bc_last_ms < 2000) return;
        s.bc_ssrc = h.ssrc; s.bc_ssrc_valid = true; s.bc_last_ms = t;
    }
    std::vector<int16_t> pcm;
    if (!rtp::decode_g711(h.pt, u + h.payload_at, h.payload_len, pcm)) return;   // only PCMU/PCMA are offered
    std::string why;
    if (!audio_->play(std::move(pcm), 8000, why)) {
        if (!s.bc_refused_logged) LOGW(MOD, "%s: backchannel audio dropped: %s", s.peer.c_str(), why.c_str());
        s.bc_refused_logged = true;
    } else s.bc_refused_logged = false;
}

bool RtspServer::audio_offered() const {
    if (!audio_ || !audio_->available()) return false;
    return audio_->config().enabled;
}

bool RtspServer::send_audio(Session& s, const std::string& pcma) {
    size_t sent = 0;
    while (sent < pcma.size()) {
        const size_t len = std::min(pcma.size() - sent, (size_t)RTP_MTU);
        if (!send_rtp(s, s.audio, rtp::kPayloadPcma, reinterpret_cast<const uint8_t*>(pcma.data()) + sent, len, s.audio.ts, false)) return false;
        s.audio.ts += (uint32_t)len;                            // one A-law byte per 8 kHz sample
        sent += len;
    }
    return true;
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
        if (n <= RTP_MTU) { if (!send_rtp(s, s.video, 96, p, n, ts, last)) return false; continue; }
        uint8_t hdr = p[0]; uint8_t fu_ind = (uint8_t)((hdr & 0xe0) | 28);
        size_t pos = 1; bool first = true;
        uint8_t buf[RTP_MTU];
        while (pos < n) {
            size_t chunk = n - pos; if (chunk > RTP_MTU - 2) chunk = RTP_MTU - 2;
            bool end = (pos + chunk == n);
            buf[0] = fu_ind;
            buf[1] = (uint8_t)((first ? 0x80 : 0) | (end ? 0x40 : 0) | (hdr & 0x1f));
            memcpy(buf + 2, p + pos, chunk);
            if (!send_rtp(s, s.video, 96, buf, chunk + 2, ts, last && end)) return false;
            pos += chunk; first = false;
        }
    }
    return true;
}

// One MJPEG frame: a snapshot of the JPEG unit (shared with /image.jpg and
// /mjpeg through the snapshot cache), packetised per RFC 2435. The rate is
// the configured MJPEG cap; a failed capture is skipped, not fatal.
bool RtspServer::send_jpeg(Session& s) {
    const int fps = cfg_.mjpeg_fps > 0 ? cfg_.mjpeg_fps : 5;
    const int64_t now = mono_us();
    s.jpeg_next_us = (s.jpeg_next_us == 0 ? now : s.jpeg_next_us) + 1000000 / fps;
    if (s.jpeg_next_us < now) s.jpeg_next_us = now + 1000000 / fps;     // fell behind: do not burst
    std::vector<uint8_t> jpg; std::string err, why; rtsp::JpegFrame jf;
    // A frame no older than this period: the snapshot cache (300 ms) is
    // longer than the frame period at 5 fps, and the cached image would
    // otherwise be sent twice.
    if (!pipeline_.snapshot(jpg, err, 1000, 1000 / fps - 20)) {
        if (err != s.jpeg_last_why) LOGW(MOD, "%s: jpeg frame skipped: %s", s.peer.c_str(), err.c_str());
        s.jpeg_last_why = err;
        return true;
    }
    if (!rtsp::parse_jpeg(jpg.data(), jpg.size(), jf, why)) {
        if (why != s.jpeg_last_why) LOGW(MOD, "%s: jpeg frame not packetisable: %s", s.peer.c_str(), why.c_str());
        s.jpeg_last_why = why;
        return true;
    }
    s.jpeg_last_why.clear();
    if (s.jpeg_t0_us < 0) s.jpeg_t0_us = now;
    const uint32_t ts = (uint32_t)((now - s.jpeg_t0_us) * 90 / 1000);
    const std::vector<std::vector<uint8_t>> pk = rtsp::jpeg_rtp_payloads(jf, RTP_MTU);
    for (size_t i = 0; i < pk.size(); ++i)
        if (!send_rtp(s, s.video, rtsp::kPayloadJpeg, pk[i].data(), pk[i].size(), ts, i + 1 == pk.size())) return false;
    return true;
}

} // namespace machino
