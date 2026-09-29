#include "app/http/http_server.hpp"
#include "app/http/audio_stream.hpp"
#include "app/http/ogg.hpp"
#include "app/http/stills.hpp"
#include "app/http/hls.hpp"
#include "core/audio/audio_encoder.hpp"
#include "app/compat/majestic_webui.hpp"
#include "app/webrtc/peer.hpp"
#include "app/http/fmp4.hpp"
#include "app/http/http_parse.hpp"
#include "app/http/websocket.hpp"
#include "app/rtsp/h264_nal.hpp"
#include "core/diag.hpp"
#include "core/log.hpp"
#include "core/runtime_stats.hpp"

#include <algorithm>
#include <atomic>
#include <arpa/inet.h>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <time.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sstream>
#include <string>
#include <thread>
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
// An /audio.* client whose output buffer has been over its bound for this long
// is not reading at all (see pump_audio).
static const int kAudioStallMs = 10000;
// Still captures (/image.yuv420, /image.heif) running at once, see stills_in_flight.
static const size_t kMaxStillJobs = 2;

static size_t input_cap(const std::string& in) {
    static const char OSD_POST[] = "POST /api/v1/osd/image";
    if (in.compare(0, sizeof(OSD_POST) - 1, OSD_POST) == 0)
        return osd::OsdService::MAX_IMAGE_BYTES + 4096;
    // An ONVIF request is a SOAP document; the scanner's own bound is what
    // decides how big one may be, and it refuses anything past it.
    static const char ONVIF_POST[] = "POST /onvif/";
    if (in.compare(0, sizeof(ONVIF_POST) - 1, ONVIF_POST) == 0)
        return onvif::MAX_REQUEST + 4096;
    // The AI page's model-bundle upload: a .tgz (model .bin + manifest) relayed
    // to the busybox CGI, which streams it to disk. machino buffers the request
    // body once before relaying, so this bound is also the RAM ceiling for the
    // upload - 8 MB, matching the CGI's own limit. A deliberate admin action on
    // an otherwise idle camera; models are a few MB.
    // The speaker clip: raw samples, bounded by what the speaker queue takes.
    static const char PLAY[] = "POST /play_audio";
    if (in.compare(0, sizeof(PLAY) - 1, PLAY) == 0)
        return kMaxPlayBodyBytes + 4096;
    static const char AI_UPLOAD[] = "POST /cgi-bin/machino-ai-upload.cgi";
    if (in.compare(0, sizeof(AI_UPLOAD) - 1, AI_UPLOAD) == 0)
        return 8 * 1024 * 1024 + 4096;
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
    // /ws/upgrade: this connection runs one sysupgrade child and streams its
    // stdout+stderr to the page as text frames. One start per socket.
    bool ws_upgrade = false;
    bool upgrade_started = false;   // a start frame has been consumed
    int  upgrade_fd = -1;           // read end of the child's merged stdout/stderr
    pid_t upgrade_pid = -1;
    bool upgrade_saw_flash = false; // saw the point-of-no-return marker
    bool upgrade_saw_reboot = false;
    std::string upgrade_win;        // rolling tail for markers that straddle reads
    int64_t upgrade_last_ping_ms = 0;
    // /ws/video: one live MSE feed = one StreamHub consumer with its own
    // demand, exactly like an RTSP session (no second encoder, no JPEG).
    bool ws_video = false;
    bool ws_raw = false;            // /video.mp4: the same fMP4, unframed over plain HTTP
    int64_t hls_wait_until = 0;     // a playlist request parked until the first segment exists
    std::string hls_prefix;
    bool hls_keep = false;
    int  ws_unit = 0;               // lifecycle unit this viewer watches (main/sub)
    StreamHub* ws_hub = nullptr;    // the hub ws_sink came from (for unsubscribe)
    std::shared_ptr<Sink>    ws_sink;
    lifecycle::DemandHandle  ws_demand;
    // /audio.*: one AudioService listener. The body is the raw sample stream,
    // no framing; a full output buffer drops frames instead of growing.
    AudioFormat audio_fmt = AudioFormat::None;
    int         audio_rate = 0;     // capture rate the listener was opened at
    std::shared_ptr<Sink> audio_sink;
    int64_t     audio_full_since_ms = 0;   // the output buffer has been over its bound since
    uint32_t    audio_last_seq = 0;        // capture sequence of the last frame seen
    bool        audio_seq_valid = false;
    // /audio.m4a and /audio.opus: the encoder and its container state.
    std::unique_ptr<audio::AudioEncoder> audio_enc;
    std::unique_ptr<ogg::OpusWriter>     audio_ogg;
    uint32_t    audio_seq = 1;
    uint64_t    audio_dt = 0;
    // /ws/video&audio=: the microphone as a second track of the same MSE stream.
    std::shared_ptr<Sink>                ws_audio_sink;
    std::unique_ptr<audio::AudioEncoder> ws_audio_enc;
    uint64_t    ws_audio_dt = 0;
    bool        ws_audio_started = false;
    uint32_t    ws_audio_last_seq = 0;
    bool        ws_audio_seq_valid = false;
    int64_t     ws_audio_skew_us = 0;      // the video timeline's skew the audio was anchored under
    // A still being captured for this client (see StillJob).
    std::shared_ptr<StillJob> still;
    // Waiting for something that is not the socket: a parked request whose
    // answer comes from the HLS segmenter or a capture thread. Input that
    // arrives meanwhile is buffered and parsed after the answer.
    bool parked() const { return hls_wait_until != 0 || (bool)still; }
    // A socket this server streams INTO rather than converses on: nothing
    // is read from it, the idle timeout does not apply, the relay never
    // takes it over. One predicate, not six hand-kept lists.
    bool streaming() const { return sse || mjpeg || ws_video || rtc_ws || ws_logs || (bool)audio_sink; }
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
    // The session's audio: the microphone out (an AudioService listener) and
    // talkback in (queued on the speaker). Both only when the answer says so.
    std::shared_ptr<Sink>    rtc_audio_sink;
    int                      rtc_audio_rate = 0;
    uint32_t                 rtc_audio_last_seq = 0;    // Seq-Gap-Kompensation (wie audio_last_seq)
    bool                     rtc_audio_seq_valid = false;
    bool                     rtc_talk_refused = false;   // log a refused talkback once, not per packet
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
    // Karten-Injektion (network.cgi): der Anker liegt tief in der Seite, also
    // wird der GANZE Body gepuffert und erst am Upstream-EOF transformiert.
    // Ohne dieses Flag gilt das bewaehrte begrenzte Fenster fuer die Navbar.
    bool        relay_inject_cards = false;
    // Dashboard mit Live-Vorschau (webui.dashboard_preview=live): wie die
    // Karten eine Ganzseiten-Transformation am EOF, weil der Anker (das
    // dashboard.js-Tag) am Seitenende steht.
    bool        relay_inject_preview = false;
    int         relay_preview_stream = 0;
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
struct HttpServer::HlsLive {
    lifecycle::DemandHandle demand;
    std::shared_ptr<Sink>   sink;
    hls::Segmenter          seg;
    int64_t                 last_req_ms = 0;
    int                     w = 0, h = 0;
};

// The capture for /image.yuv420 or /image.heif, on its own thread. It talks
// only to the pipeline manager and the stream hubs (both outlive the server
// and are thread-safe, exactly as they are for the RTSP threads) and writes
// the answer here; the poll loop hands it to the client when `done`.
struct HttpServer::StillJob {
    std::thread       th;
    std::atomic<bool> done{false};
    bool        keep_alive = false;
    std::string path;
    int         status = 500;
    std::string ctype, extra, error;
    std::vector<uint8_t> body;
};

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
    for (auto& c : clients_) release_client(*c);
    clients_.clear();
    reap_stills(true);                          // every capture thread joined before the pipeline may go
    hls_stop();
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

bool HttpServer::queue_bytes(Client& c, const std::string& head, const uint8_t* body, size_t n, size_t cap) {
    if (c.out.size() + head.size() + n > cap) return false;
    c.out += head;
    c.out.append(reinterpret_cast<const char*>(body), n);
    return true;
}

// Give back everything a client holds and close its socket. Called from the
// poll loop's close path and from stop(); nothing else frees these.
void HttpServer::release_client(Client& c) {
    if (c.ws_video) RuntimeStats::get().dec(&RuntimeCounters::ws_video_clients);
    if (c.ws_logs)  RuntimeStats::get().dec(&RuntimeCounters::ws_logs_clients);
    if (c.rtc)      RuntimeStats::get().dec(&RuntimeCounters::webrtc_sessions);
    if (c.sub) { bus_.unsubscribe(c.sub); c.sub.reset(); }
    if (c.ws_sink)  { StreamHub* h = c.ws_hub  ? c.ws_hub  : hub_; if (h) { c.ws_sink->close();  h->unsubscribe(c.ws_sink); }  c.ws_sink.reset(); }
    if (c.rtc_sink) { StreamHub* h = c.rtc_hub ? c.rtc_hub : hub_; if (h) { c.rtc_sink->close(); h->unsubscribe(c.rtc_sink); } c.rtc_sink.reset(); }
    if (c.audio_sink && audio_)     { audio_->unlisten(c.audio_sink);     c.audio_sink.reset(); }
    if (c.rtc_audio_sink && audio_) { audio_->unlisten(c.rtc_audio_sink); c.rtc_audio_sink.reset(); }
    if (c.ws_audio_sink && audio_)  { audio_->unlisten(c.ws_audio_sink);  c.ws_audio_sink.reset(); }
    c.rtc.reset();                              // closes the UDP socket
    // The two DemandHandles (ws_demand, rtc_demand) are NOT released here:
    // they are Client members and their destructors do it when the Client is
    // dropped. That is safe because httpd is declared after the
    // PipelineManager in main() and therefore destroyed before it -
    // release() calls back into the manager.
    if (c.still) { still_orphans_.push_back(std::move(c.still)); }   // joined once its capture is done
    if (c.relay_fd >= 0) { close(c.relay_fd); c.relay_fd = -1; }
    // Drop our read end of the upgrade child, but never kill it: sysupgrade
    // past the flash point is meant to outlive us ("Protected: flashing
    // continues"); the child finishes and reboots the box.
    if (c.upgrade_fd >= 0) {
        close(c.upgrade_fd); c.upgrade_fd = -1;
        if (c.upgrade_pid > 0) waitpid(c.upgrade_pid, nullptr, WNOHANG);
    }
    if (c.fd >= 0) { close(c.fd); c.fd = -1; }
}

// Captures running right now, for clients still here and for ones that left.
// Each holds a frame (3 MB at 1080p) and a thread; two at a time is what a
// dashboard click plus one tool asks for, more is a loop or a flood.
size_t HttpServer::stills_in_flight() const {
    size_t n = still_orphans_.size();
    for (const auto& c : clients_) if (c->still && !c->still->done.load(std::memory_order_acquire)) ++n;
    return n;
}

// Orphaned capture threads: join the finished ones (`wait`: all of them).
void HttpServer::reap_stills(bool wait) {
    for (auto it = still_orphans_.begin(); it != still_orphans_.end();) {
        if (wait || (*it)->done.load(std::memory_order_acquire)) {
            if ((*it)->th.joinable()) (*it)->th.join();
            it = still_orphans_.erase(it);
        } else ++it;
    }
}

// The capture thread is done: its answer goes out, the parked request is
// over and whatever the client sent meanwhile is parsed.
void HttpServer::finish_still(Client& c) {
    std::shared_ptr<StillJob> job = std::move(c.still);
    if (job->th.joinable()) job->th.join();
    if (job->status == 200) {
        const std::string head = response_head(200, job->ctype, job->body.size(), job->keep_alive, job->extra);
        if (!queue_bytes(c, head, job->body.data(), job->body.size(), job->body.size() + head.size() + cfg_.max_out_buffer)) { c.close_after_flush = true; return; }
    } else {
        queue(c, response(job->status, "application/json",
                          api::ApiService::fail(job->status, job->status == 400 || job->status == 416 ? "invalid_value" : "unavailable", job->path, job->error).body.dump(),
                          job->keep_alive));
    }
    if (!job->keep_alive) c.close_after_flush = true;
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
    // A handler that turned the connection into a stream (or parked it) ends
    // the conversation: a second request pipelined behind it would otherwise
    // replace the sink the first one holds without giving it back.
    while (ok && !c.close_after_flush && c.relay_state == Client::Relay::None && !c.parked() && !c.streaming() &&
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
    diag::set_http("dispatch");   // stall marker: inside request handling

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
    } else if (path == "/machino/net" || path == "/machino/net/" ||
               path == "/machino/devices" || path == "/machino/devices/") {
        // Die Machino-Seiten sind seit dem UI-Umbau ECHTE OpenIPC-Seiten:
        // haserl-CGIs im Webroot (von Machino installiert, von seinem
        // Uninstall entfernt), gerendert von derselben Pipeline wie network.cgi
        // -- Head, Navbar, Theme und main.js stehen damit im ERSTEN HTML, und
        // relative Links haben denselben Basiskontext wie ueberall.
        //
        // Ein erster Anlauf hat stattdessen die Kopfleiste clientseitig aus
        // einer geholten Seite uebernommen. Das war der falsche Weg: die Seite
        // sprang beim Einfuegen, relative Links liefen unter /machino/ ins
        // Leere, und deren main.js starb an einem globalen Bezeichnerkonflikt.
        // Diese Pfade bleiben nur als Weiterleitung fuer Lesezeichen und
        // aeltere Menue-Injektionen bestehen.
        const bool dev = path.compare(0, 16, "/machino/devices") == 0;
        const char* to = dev ? "/cgi-bin/machino-devices.cgi" : "/cgi-bin/machino-usb.cgi";
        std::string h = "HTTP/1.1 302 Found\r\nLocation: ";
        h += to;
        h += "\r\nContent-Length: 0\r\n";
        h += req.keep_alive ? "Connection: keep-alive\r\n\r\n"
                            : "Connection: close\r\n\r\n";
        const bool ok = queue(c, h);
        if (!req.keep_alive) c.close_after_flush = true;
        return ok;
    } else if (net_api_ && net_api_->handle(m, path, req.body, r)) {
        // Asked first among the /api/v1 routes because it owns two whole
        // prefixes. It returns false for anything outside them, so the chain
        // below is unchanged for every existing path.
    } else if (path == "/api/v1" || path == "/api/v1/") { r = (m == "GET") ? api_.discovery() : api::ApiService::fail(405, "unknown_field", path, "method not allowed"); }
    else if (path == "/api/v1/capabilities") { r = (m == "GET") ? api_.capabilities() : api::ApiService::fail(405, "unknown_field", path, "method not allowed"); }
    else if (path == "/api/v1/state")        { r = (m == "GET") ? api_.state() : api::ApiService::fail(405, "unknown_field", path, "method not allowed"); }
    else if (path == "/api/v1/telemetry")    { r = (m == "GET") ? api_.telemetry() : api::ApiService::fail(405, "unknown_field", path, "method not allowed"); }
    else if (path == "/api/v1/ai/detectors") { r = (m == "GET") ? api_.ai_detectors() : api::ApiService::fail(405, "unknown_field", path, "method not allowed"); }
    else if (path.rfind("/night/", 0) == 0) {
        // W2: majestics Day/Night-Flaeche (Stream-URLs-Seite dokumentiert
        // sie woertlich). GET, Antwort = nacktes JSON-Boolean.
        r = (m == "GET") ? api_.night_action(path.substr(7))
                         : api::ApiService::fail(405, "unknown_field", path, "method not allowed");
    }
    else if (path == "/api/v1/gpio") {
        r = (m == "GET") ? api_.gpio_map()
                         : api::ApiService::fail(405, "unknown_field", path, "method not allowed");
    }
    else if (path == "/metrics/night") {
        // majestic-Vertrag: value=<name>_enabled -> "0"/"1" als text/plain.
        const std::string v = SessionGate::form_value(req.query, "value");
        std::string val;
        if (m == "GET" && api_.night_metric(v, val)) {
            bool ok = queue(c, response(200, "text/plain", val, req.keep_alive));
            if (!req.keep_alive) c.close_after_flush = true;
            return ok;
        }
        r = api::ApiService::fail(404, "not_found", path, "no such gauge");
    }
    else if (path == "/api/v1/ipsec")            { r = (m == "GET") ? api_.ipsec_get() : api::ApiService::fail(405, "unknown_field", path, "method not allowed"); }
    else if (path == "/api/v1/ipsec/config")     { r = (m == "PUT") ? api_.ipsec_put_config(req.body) : api::ApiService::fail(405, "unknown_field", path, "method not allowed"); }
    else if (path == "/api/v1/ipsec/connect")    { r = (m == "POST") ? api_.ipsec_connect() : api::ApiService::fail(405, "unknown_field", path, "method not allowed"); }
    else if (path == "/api/v1/ipsec/disconnect") { r = (m == "POST") ? api_.ipsec_disconnect() : api::ApiService::fail(405, "unknown_field", path, "method not allowed"); }
    else if (path == "/api/v1/ipsec/status")     { r = (m == "GET") ? api_.ipsec_status() : api::ApiService::fail(405, "unknown_field", path, "method not allowed"); }
    else if (path == "/api/v1/audio/tone")       { r = (m == "POST") ? api_.audio_tone(req.body)    : api::ApiService::fail(405, "unknown_field", path, "method not allowed"); }
    else if (path == "/api/v1/audio/monitor")    { r = (m == "POST") ? api_.audio_monitor(req.body) : api::ApiService::fail(405, "unknown_field", path, "method not allowed"); }
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
        diag::set_http("api_config");   // stall marker: the route the hardlock reproduced on
        if (m == "GET") r = api_.config();
        else if (m == "POST") {
            compat::MajesticTranslation t = compat::majestic_post_to_native(req.body);
            r = t.ok ? api_.patch_config(t.patch.dump(), "")
                     : api::ApiService::fail(t.status, t.code.c_str(), t.path, t.message);
        } else if (m == "PATCH" || m == "PUT") r = api_.patch_config(req.body, req.header("if-match"));
        else r = api::ApiService::fail(405, "unknown_field", path, "method not allowed");
    } else if (path == "/ws/video") {
        diag::set_http("media:ws_video");   // stall marker: acquire/IMP path
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
                start_fmp4_viewer(c, unit, h, std::move(d), SessionGate::form_value(req.query, "audio"));
                pipeline_->request_idr(unit);
                LOGI(MOD, "%s: /ws/video session started (unit %d)", c.peer.c_str(), unit);
                return true;
            }
        }
    } else if (path == "/hls") {
        // majestic's HLS viewer page. Native HLS where the browser has it
        // (Safari, iOS, Android, recent Chrome); elsewhere hls.js, fetched by
        // the browser - the camera serves only the page and the stream.
        static const char kPage[] =
            "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>HLS</title>"
            "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
            "<style>html,body{margin:0;background:#000;height:100%;color:#ccc;font:14px sans-serif}"
            "video{display:block;width:100%;height:100%;object-fit:contain}"
            "#n{position:absolute;top:8px;left:8px}</style></head><body>"
            "<video id=\"v\" autoplay muted playsinline controls></video><div id=\"n\"></div><script>"
            "var v=document.getElementById('v'),u='/hls/index.m3u8';"
            "function note(t){document.getElementById('n').textContent=t}"
            "if(v.canPlayType('application/vnd.apple.mpegurl')){v.src=u}else{"
            "var s=document.createElement('script');"
            "s.src='https://cdn.jsdelivr.net/npm/hls.js@1/dist/hls.min.js';"
            "s.onload=function(){if(window.Hls&&Hls.isSupported()){var h=new Hls({liveSyncDurationCount:2});"
            "h.loadSource(u);h.attachMedia(v)}else note('This browser cannot play HLS.')};"
            "s.onerror=function(){note('No native HLS here and hls.js could not be loaded: open '+location.origin+u+' in VLC.')};"
            "document.head.appendChild(s)}</script></body></html>\n";
        bool ok = queue(c, response(m == "GET" ? 200 : 405, "text/html; charset=utf-8",
                                    m == "GET" ? kPage : "", req.keep_alive));
        if (!req.keep_alive) c.close_after_flush = true;
        return ok;
    } else if (path == "/video.m3u8" || path == "/hls.m3u8" ||
               (path.rfind("/hls/", 0) == 0 && path.size() > 10 && path.compare(path.size() - 5, 5, ".m3u8") == 0)) {
        std::string err;
        if (m != "GET") r = api::ApiService::fail(405, "unknown_field", path, "method not allowed");
        else if (!hls_touch(err)) r = api::ApiService::fail(503, "unavailable", path, err);
        else {
            c.hls_prefix = path.rfind("/hls/", 0) == 0 ? "" : "hls/";
            c.hls_keep = req.keep_alive;
            if (hls_answer_playlist(c)) return true;
            c.hls_wait_until = now_ms() + 10000;      // the first segment is a GOP away
            return true;
        }
    } else if (path.rfind("/hls/", 0) == 0) {
        std::string err; uint64_t seq = 0; unsigned gen = 0;
        const std::string name = path.substr(5);
        std::vector<uint8_t> data;
        const bool is_init = hls::Segmenter::parse_init_name(name, gen);
        if (m != "GET") r = api::ApiService::fail(405, "unknown_field", path, "method not allowed");
        else if (!is_init && !hls::Segmenter::parse_segment_name(name, seq)) r = api::ApiService::fail(404, "unknown_field", path, "no such HLS resource");
        else if (!hls_touch(err)) r = api::ApiService::fail(503, "unavailable", path, err);
        else if (is_init && gen != hls_->seg.init_generation()) r = api::ApiService::fail(404, "unknown_field", path, "that init segment is gone - reload the playlist");
        else if (is_init ? (data = hls_->seg.init()).empty() : !hls_->seg.segment(seq, data))
            r = api::ApiService::fail(404, "unknown_field", path, is_init ? "no key frame yet" : "segment no longer held");
        else {
            const std::string head = response_head(200, is_init ? "video/mp4" : "video/iso.segment", data.size(), req.keep_alive);
            bool ok = queue_bytes(c, head, data.data(), data.size(), data.size() + head.size() + cfg_.max_out_buffer);
            if (!req.keep_alive) c.close_after_flush = true;
            return ok;
        }
    } else if (path == "/video.mp4") {
        // majestic's progressive MP4: the /ws/video fragments (ftyp+moov, then
        // moof+mdat per frame) straight over HTTP, close-framed. VLC, ffplay,
        // a <video> element and `curl > file.mp4` all read it. Audio (AAC,
        // what every MP4 player decodes) rides along when the microphone is on
        // and the build has the encoder; ?audio=opus picks Opus instead.
        const std::string sv = SessionGate::form_value(req.query, "stream");
        const int unit = unit_for_stream(sv);
        if (m != "GET")                  { r = api::ApiService::fail(405, "unknown_field", path, "method not allowed"); }
        else if (!hub_ || !pipeline_)    { r = api::ApiService::fail(501, "unavailable", path, "no media wiring"); }
        else if (unit < 0) {
            r = api::ApiService::fail(404, "unknown_field", path, "stream " + sv + " is not available");
        } else {
            StreamHub* h = unit == lifecycle::UNIT_SUB ? sub_hub_ : hub_;
            Result dr;
            lifecycle::DemandHandle d = pipeline_->acquire_unit(unit, lifecycle::ConsumerType::HttpStream, &dr);
            if (!d.active()) { r = api::ApiService::fail(503, "unavailable", path, "pipeline start failed"); }
            else {
                queue(c, "HTTP/1.1 200 OK\r\nContent-Type: video/mp4\r\nCache-Control: no-store\r\n"
                         "Access-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n");
                c.ws_raw = true;
                const std::string a = SessionGate::form_value(req.query, "audio");
                start_fmp4_viewer(c, unit, h, std::move(d), a.empty() ? std::string("mp4a.40.2") : (a == "none" ? std::string() : a));
                pipeline_->request_idr(unit);
                LOGI(MOD, "%s: /video.mp4 started (unit %d)", c.peer.c_str(), unit);
                return true;
            }
        }
    } else if (path == "/ws/webrtc") {
        diag::set_http("media:ws_webrtc");   // stall marker: acquire/IMP path
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
        // The stock Update page's flash channel. update.js opens this socket and
        // sends ONE JSON start frame {source, kernel, rootfs, reset, force}; the
        // camera runs sysupgrade and streams its stdout+stderr back verbatim, so
        // every marker the page watches for (Protected/Kernel updated/RootFS
        // updated/Unconditional reboot/"<reason> Aborting.") comes straight from
        // sysupgrade. Two invariants machino adds on top (upgrade_plan + a host
        // test hold them): --web is ALWAYS passed (else sysupgrade SIGQUITs this
        // daemon and the log dies), and overlay wipe (reset/-n) is NEVER passed
        // (it would erase machino and the AI model). majestic stays disabled
        // across the update because the overlay whiteout on S95majestic is
        // preserved - a normal rootfs flash keeps the overlay.
        const std::string wskey = req.header("sec-websocket-key");
        if (m != "GET" || wskey.empty()) { r = api::ApiService::fail(400, "invalid_value", path, "websocket upgrade required"); }
        else {
            queue(c, ws::handshake_response(wskey));
            c.ws_upgrade = true;                 // now awaiting the JSON start frame
            c.upgrade_last_ping_ms = now_ms();
            LOGI(MOD, "%s: /ws/upgrade open - awaiting start", c.peer.c_str());
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
    } else if (path == "/mjpeg.html") {
        // majestic's viewer page for the MJPEG stream. One <img>; when JPEG is
        // off, the image itself answers 501 and the browser shows it broken,
        // which is the truth.
        static const char kPage[] =
            "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>MJPEG</title>"
            "<style>html,body{margin:0;background:#000;height:100%}"
            "img{display:block;margin:auto;max-width:100%;max-height:100%}</style></head>"
            "<body><img src=\"/mjpeg\" alt=\"MJPEG stream\"></body></html>\n";
        bool ok = queue(c, response(m == "GET" ? 200 : 405, "text/html; charset=utf-8",
                                    m == "GET" ? kPage : "", req.keep_alive));
        if (!req.keep_alive) c.close_after_flush = true;
        return ok;
    } else if (path == "/image.yuv420") {
        // The stock page's detail still (preview-still.js): one uncompressed
        // frame of the MAIN channel, optionally cropped, described in headers.
        // The capture waits for a frame (right after a cold start: up to
        // 2 s), so it runs on its own thread and the request is parked; the
        // poll loop that pumps every live stream never waits for it.
        stills::Crop want; const bool has_crop = !SessionGate::form_value(req.query, "crop").empty();
        const std::string cs = SessionGate::form_value(req.query, "crop");
        if (m != "GET") r = api::ApiService::fail(405, "unknown_field", path, "method not allowed");
        else if (!pipeline_) r = api::ApiService::fail(501, "unavailable", path, "no media wiring");
        else if (has_crop && !stills::parse_crop(cs, want)) r = api::ApiService::fail(400, "invalid_value", path, "crop is XxYxWxH");
        else if (stills_in_flight() >= kMaxStillJobs) r = api::ApiService::fail(503, "unavailable", path, "another still is being captured");
        else {
            auto job = std::make_shared<StillJob>();
            job->keep_alive = req.keep_alive; job->path = path;
            lifecycle::PipelineManager* pl = pipeline_;
            job->th = std::thread([job, pl, want, has_crop] {
                std::vector<uint8_t> frame; int fw = 0, fh = 0;
                const Result sr = pl->snap_nv12(lifecycle::UNIT_MAIN, frame, fw, fh, job->error, 2000);
                if (!sr) job->status = sr.status == Status::Unsupported ? 501 : 503;
                else {
                    stills::Crop got{0, 0, fw, fh};
                    if (has_crop) {
                        std::vector<uint8_t> cut;
                        if (!stills::nv12_crop(frame.data(), fw, fh, want, cut, got)) {
                            job->status = 416; job->error = "crop lies outside the " + std::to_string(fw) + "x" + std::to_string(fh) + " frame";
                        } else { job->body.swap(cut); job->status = 200; }
                    } else { job->body.swap(frame); job->status = 200; }
                    if (job->status == 200) {
                        job->ctype = "application/octet-stream";
                        job->extra = stills::yuv_headers(got.w, got.h) +
                                     "Access-Control-Expose-Headers: X-Frame-Width, X-Frame-Height, X-Pixel-Format, X-Stride-Luma, X-Stride-Chroma\r\n";
                    }
                }
                job->done.store(true, std::memory_order_release);
            });
            c.still = job;
            return true;
        }
    } else if (path == "/image.heif") {
        // One IDR of the main stream as a HEIF image item. H.264 in HEIF
        // ('avci'), because H.264 is what the encoder makes. The key frame
        // is up to a GOP away: its own thread, the request parked meanwhile.
        if (m != "GET") r = api::ApiService::fail(405, "unknown_field", path, "method not allowed");
        else if (!hub_ || !pipeline_) r = api::ApiService::fail(501, "unavailable", path, "no media wiring");
        else if (stills_in_flight() >= kMaxStillJobs) r = api::ApiService::fail(503, "unavailable", path, "another still is being captured");
        else {
            auto job = std::make_shared<StillJob>();
            job->keep_alive = req.keep_alive; job->path = path;
            job->th = std::thread([this, job] {
                AuPtr au;
                if (!grab_idr(lifecycle::UNIT_MAIN, au, 2000, job->error)) job->status = 503;
                else {
                    std::vector<uint8_t> sps, pps;
                    h264::extract_params(au->data.data(), au->data.size(), sps, pps);
                    const std::vector<uint8_t> sample = fmp4::annexb_to_avcc(au->data.data(), au->data.size());
                    const EffectiveStream es = pipeline_->stream_unit(lifecycle::UNIT_MAIN);
                    if (sps.empty() || pps.empty() || sample.empty()) { job->status = 503; job->error = "key frame without parameter sets"; }
                    else { job->body = fmp4::heif_avc_still(sps, pps, es.width, es.height, sample); job->ctype = "image/heif"; job->status = 200; }
                }
                job->done.store(true, std::memory_order_release);
            });
            c.still = job;
            return true;
        }
    } else if (path == "/api/v1/stream.mjpeg" || path == "/stream.mjpeg" || path == "/stream" || path == "/mjpeg") {
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
    } else if (audio_format_for_path(path) != AudioFormat::None) {
        if (m != "GET") { r = api::ApiService::fail(405, "unknown_field", path, "method not allowed"); }
        else if (!audio_) { r = api::ApiService::fail(501, "unavailable", path, "this build has no audio path"); }
        else {
            std::string why;
            const AudioFormat f = audio_format_for_path(path);
            std::unique_ptr<audio::AudioEncoder> enc;
            if (*audio_codec_for(f)) enc = audio::make_encoder(audio_codec_for(f), audio_->sample_rate());
            std::shared_ptr<Sink> s;
            if (*audio_codec_for(f) && !enc)
                why = audio::codecs_built() ? "the encoder refused this sample rate"
                                            : "this build has no AAC/Opus encoder (built without CODECS)";
            else s = audio_->listen(why);
            if (!s) { r = api::ApiService::fail(*audio_codec_for(f) && !enc ? 501 : 503, "unavailable", path, why); }
            else {
                c.audio_fmt = f;
                c.audio_rate = audio_->sample_rate();
                c.audio_sink = std::move(s);
                queue(c, audio_stream_headers(f, c.audio_rate));
                if (f == AudioFormat::Aac) {
                    fmp4::AudioTrack t;
                    t.codec = fmp4::AudioTrack::Aac; t.track_id = 1;
                    t.sample_rate = enc->sample_rate(); t.asc = enc->config();
                    const std::vector<uint8_t> init = fmp4::audio_init_segment(t);
                    c.out.append(reinterpret_cast<const char*>(init.data()), init.size());
                } else if (f == AudioFormat::Opus) {
                    c.audio_ogg.reset(new ogg::OpusWriter((uint32_t)now_ms() ^ 0x4f707573u, enc->sample_rate(), enc->pre_skip()));
                    c.out += c.audio_ogg->headers();
                }
                c.audio_enc = std::move(enc);
                LOGI(MOD, "%s: audio stream started (%s, %d Hz on the wire)", c.peer.c_str(),
                     audio_format_name(f), audio_wire_rate(f, c.audio_rate));
                return true;
            }
        }
    } else if (path == "/play_audio") {
        // majestic's speaker endpoint. Plain-text answers: the stock settings
        // page shows the body of a refusal verbatim ("Rejects with what the
        // camera said"), so it must be a sentence, not a JSON envelope.
        int status = 200; std::string text = "OK\n";
        if (m != "POST") { status = 405; text = "POST the samples to play\n"; }
        else if (!audio_ || !audio_->output_available()) { status = 501; text = "this camera has no audio output\n"; }
        else {
            std::vector<int16_t> pcm; int rate = 0; std::string err;
            if (!play_body_to_pcm(req.body, audio_->config().srate, pcm, rate, err)) { status = 400; text = err + "\n"; }
            else {
                const size_t n = pcm.size();
                const Result pr = audio_->play(std::move(pcm), rate, err);
                if (!pr) {
                    status = pr.status == Status::Unsupported ? 501 : pr.status == Status::Busy ? 503 : 400;
                    text = err + "\n";
                } else {
                    LOGI(MOD, "%s: play_audio queued %zu samples at %d Hz", c.peer.c_str(), n, rate);
                }
            }
        }
        bool ok = queue(c, response(status, "text/plain; charset=utf-8", text, req.keep_alive));
        if (!req.keep_alive) c.close_after_flush = true;
        return ok;
    } else if (path == "/snapshot" || path == "/snapshot.jpg" || path == "/api/v1/snapshot" ||
               path == "/image.jpg") {
        diag::set_http("media:snapshot");   // stall marker: JPEG/IMP path
        // W3: /image.jpg ist majestics Name fuer dasselbe Standbild (Dashboard
        // pollt es, die Kameraseite holt Stills mit ?t=/?session= -- die Query
        // ist Cache-Busting und wird ignoriert). Mit jpeg.enabled=false
        // antwortet der Pfad ehrlich 501 (T40NN-JPEG-Wedge, Default aus).
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
    // Nur fuer die eine bekannte Seite; alles andere behaelt den schnellen
    // Fenster-Pfad. Der Query-Teil ist egal (req.path ist ohne Query).
    c.relay_inject_cards = (c.relay_get && req.path == "/cgi-bin/network.cgi");
    c.relay_inject_preview = false;
    if (c.relay_get && req.path == "/cgi-bin/dashboard.cgi" && api_.dashboard_preview() == "live") {
        c.relay_inject_preview = true;
        c.relay_preview_stream = sub_available() ? 1 : 0;
    }
    c.keep_alive_wanted = req.keep_alive;
    const int64_t now = now_ms();
    c.relay_idle_deadline_ms = now + cfg_.relay_timeout_ms;
    c.relay_abs_deadline_ms  = now + cfg_.relay_max_ms;
    c.relay_what = req.method + " " + req.path;
    diag::set_http("relay");   // stall marker: forwarding to busybox on :85
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
            c.relay_inject = false; c.relay_inject_cards = false; c.relay_inject_preview = false;
            c.relay_saved_head.clear(); c.relay_inject_buf.clear();
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
                std::string emit = did ? std::move(merged) : c.relay_inject_buf;
                if (c.relay_inject_cards) {
                    // Erst hier, am EOF, ist der tiefe Anker sicher im Puffer.
                    bool didCards = false;
                    std::string withCards = http::inject_machino_network_cards(emit, didCards);
                    if (didCards) emit = std::move(withCards);
                    else
                        LOGW(MOD, "relay: %s: card anchor not found - page served without the machino card",
                             c.relay_what.c_str());
                }
                if (c.relay_inject_preview) {
                    bool didPrev = false;
                    std::string withPrev = http::inject_machino_dashboard_preview(emit, c.relay_preview_stream, didPrev);
                    if (didPrev) emit = std::move(withPrev);
                    else
                        LOGW(MOD, "relay: %s: dashboard.js tag not found - page served without the live preview",
                             c.relay_what.c_str());
                }
                if (!emit.empty() &&
                    !queue(c, emit, cfg_.max_page_transform_bytes + cfg_.max_out_buffer + sizeof buf))
                    return false;
                c.relay_total += emit.size();
                c.relay_inject = false;
                c.relay_inject_cards = false;
                c.relay_inject_preview = false;
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
                // Im Karten-Modus wird NIE vorzeitig emittiert: der zweite
                // Anker liegt tief in der Seite, alles laeuft bis zum EOF auf.
                if (c.relay_inject && !c.relay_inject_cards && !c.relay_inject_preview) {
                    bool did = false;
                    // Erst emittieren, wenn die GANZE Navbar im Fenster liegt:
                    // die Services-Eintraege haengen am zweiten Anker.
                    std::string merged = http::inject_machino_nav(c.relay_inject_buf, did);
                    if ((did && http::relay_nav_complete(c.relay_inject_buf)) ||
                        c.relay_inject_buf.size() >= cfg_.max_inject_bytes) {
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
            if (c.relay_inject_cards || c.relay_inject_preview) {
                // Ganzseiten-Pufferung. Die Notbremse gibt die Seite
                // UNVERAENDERT weiter, statt sie abzuschneiden.
                if (c.relay_inject_buf.size() > cfg_.max_page_transform_bytes) {
                    LOGW(MOD, "relay: %s: page exceeds %zu B - served unchanged, no card",
                         c.relay_what.c_str(), cfg_.max_page_transform_bytes);
                    if (!queue(c, c.relay_inject_buf,
                               cfg_.max_page_transform_bytes + cfg_.max_out_buffer + sizeof buf))
                        return false;
                    c.relay_total += c.relay_inject_buf.size();
                    c.relay_inject = false;
                    c.relay_inject_cards = false;
                    c.relay_inject_preview = false;
                    c.relay_inject_buf.clear();
                }
                progress();
                continue;
            }
            bool did = false;
            std::string merged = http::inject_machino_nav(c.relay_inject_buf, did);
            if ((did && http::relay_nav_complete(c.relay_inject_buf)) ||
                c.relay_inject_buf.size() >= cfg_.max_inject_bytes) {
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

// Shared by /ws/video and /video.mp4: subscribe the client to a unit's hub
// and, when asked (`audio_want`: codec names in the viewer's order), to the
// microphone through the first encoder this build has.
void HttpServer::start_fmp4_viewer(Client& c, int unit, StreamHub* h, lifecycle::DemandHandle d, const std::string& audio_want) {
    c.ws_video = true;
    RuntimeStats::get().inc(&RuntimeCounters::ws_video_clients);
    c.ws_unit = unit;
    c.ws_hub = h;
    c.ws_demand = std::move(d);
    c.ws_sink = h->subscribe();
    c.ws_await_key = true;
    // &audio=opus,mp4a.40.2: the codecs this browser can decode, in its
    // order. The first one this build can encode wins; with the microphone
    // off (or no encoder) the init names no audio codec and the player drops
    // to muted by itself.
    if (!audio_want.empty() && audio_ && audio_->config().enabled) {
        size_t at = 0;
        while (at <= audio_want.size() && !c.ws_audio_enc) {
            const size_t comma = audio_want.find(',', at);
            const std::string codec = audio_want.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
            c.ws_audio_enc = audio::make_encoder(codec, audio_->sample_rate());
            if (comma == std::string::npos) break;
            at = comma + 1;
        }
        if (c.ws_audio_enc) {
            std::string why;
            c.ws_audio_sink = audio_->listen(why);
            if (!c.ws_audio_sink) { c.ws_audio_enc.reset(); LOGW(MOD, "%s: fMP4 audio refused: %s", c.peer.c_str(), why.c_str()); }
        }
    }
}

// A binary fMP4 piece for this viewer: a WebSocket frame on /ws/video, the
// bytes themselves on /video.mp4.
bool HttpServer::queue_fmp4(Client& c, const std::vector<uint8_t>& b, size_t cap) {
    if (c.ws_raw) return queue(c, std::string(reinterpret_cast<const char*>(b.data()), b.size()), cap);
    return queue(c, ws::frame(false, b.data(), b.size()), cap);
}

// ---- HLS ---------------------------------------------------------------------
// Any HLS request keeps the segmenter alive; the first one starts it.
bool HttpServer::hls_touch(std::string& err) {
    if (!hub_ || !pipeline_) { err = "no media wiring"; return false; }
    if (!hls_) {
        Result dr;
        lifecycle::DemandHandle d = pipeline_->acquire_unit(lifecycle::UNIT_MAIN, lifecycle::ConsumerType::HttpStream, &dr);
        if (!d.active()) { err = "pipeline start failed"; return false; }
        hls_.reset(new HlsLive);
        hls_->demand = std::move(d);
        hls_->sink = hub_->subscribe();
        const EffectiveStream es = pipeline_->stream_unit(lifecycle::UNIT_MAIN);
        hls_->w = es.width; hls_->h = es.height;
        pipeline_->request_idr(lifecycle::UNIT_MAIN);
        LOGI(MOD, "HLS segmenter started (%dx%d)", es.width, es.height);
    }
    hls_->last_req_ms = now_ms();
    return true;
}

void HttpServer::hls_stop() {
    if (!hls_) return;
    if (hls_->sink && hub_) { hls_->sink->close(); hub_->unsubscribe(hls_->sink); }
    hls_.reset();                                   // the demand handle releases the unit
}

// Drains the main stream into the segmenter. A player fetches the playlist
// every target duration; 30 s without any request means nobody is watching.
void HttpServer::hls_pump(int64_t t) {
    if (!hls_) return;
    if (t - hls_->last_req_ms > 30000) { LOGI(MOD, "HLS segmenter stopped (no player)"); hls_stop(); return; }
    for (int i = 0; i < 64; ++i) {
        AuPtr au; bool disc = false;
        if (!hls_->sink->pop(au, 0, &disc)) return;
        if (!au || au->data.empty()) continue;
        if (au->key) {                                  // a reconfigured stream: the next init must say the new size
            const EffectiveStream es = pipeline_->stream_unit(lifecycle::UNIT_MAIN);
            if (es.width > 0 && es.height > 0) { hls_->w = es.width; hls_->h = es.height; }
        }
        hls_->seg.feed(au->data.data(), au->data.size(), au->key, au->pts_us, hls_->w, hls_->h, disc);
    }
}

bool HttpServer::hls_answer_playlist(Client& c) {
    if (!hls_ || !hls_->seg.ready()) return false;
    c.hls_wait_until = 0;
    queue(c, response(200, "application/vnd.apple.mpegurl", hls_->seg.playlist(c.hls_prefix), c.hls_keep));
    if (!c.hls_keep) c.close_after_flush = true;
    return true;
}

// One key frame of a unit, synchronously: demand + a private sink + an IDR
// request, then the first key AU. Bounded, since it runs inside the poll
// loop like the JPEG snapshot does.
bool HttpServer::grab_idr(int unit, AuPtr& out, int timeout_ms, std::string& err) {
    StreamHub* h = unit == lifecycle::UNIT_SUB ? sub_hub_ : hub_;
    if (!h || !pipeline_) { err = "no media wiring"; return false; }
    Result dr;
    lifecycle::DemandHandle d = pipeline_->acquire_unit(unit, lifecycle::ConsumerType::Snapshot, &dr);
    if (!d.active()) { err = "pipeline start failed"; return false; }
    std::shared_ptr<Sink> sink = h->subscribe();
    pipeline_->request_idr(unit);
    const int64_t until = now_ms() + timeout_ms;
    bool got = false;
    for (int64_t t = now_ms(); t < until && !got; t = now_ms()) {
        AuPtr au;
        if (!sink->pop(au, (int)(until - t))) continue;
        if (au && au->key && !au->data.empty()) { out = au; got = true; }
    }
    sink->close();
    h->unsubscribe(sink);
    if (!got) err = "no key frame within " + std::to_string(timeout_ms) + " ms";
    return got;
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
                    fmp4::AudioTrack at;
                    if (c.ws_audio_enc && c.ws_audio_sink) {
                        at.codec = std::string(c.ws_audio_enc->codec()) == "opus" ? fmp4::AudioTrack::Opus : fmp4::AudioTrack::Aac;
                        at.track_id = 2;
                        at.sample_rate = c.ws_audio_enc->sample_rate();
                        at.asc = c.ws_audio_enc->config();
                        at.pre_skip = c.ws_audio_enc->pre_skip();
                        info.set("audioCodec", Json::string(c.ws_audio_enc->codec()));
                        info.set("mime", Json::string("video/mp4; codecs=\"" + cs + ", " + c.ws_audio_enc->codec() + "\""));
                    }
                    const std::string init_json = info.dump();
                    std::vector<uint8_t> init = fmp4::init_segment(sps, pps, w, h, 90000,
                                                                   at.codec == fmp4::AudioTrack::None ? nullptr : &at);
                    if (!c.ws_raw) queue(c, ws::frame(true, init_json.data(), init_json.size()), soft_cap);
                    else if (c.ws_init_sent) { mark_resync(c); c.close_after_flush = true; return; }   // a file cannot change its moov mid-stream
                    if (!queue_fmp4(c, init, soft_cap)) { c.close_after_flush = true; return; }
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
        if (!queue_fmp4(c, frag, soft_cap)) {
            RuntimeStats::get().inc(&RuntimeCounters::ws_video_overruns);
            mark_resync(c);                                    // dropped: wait for the next key
            return;
        }
        RuntimeStats::get().inc(&RuntimeCounters::ws_video_frames);
        RuntimeStats::get().inc(&RuntimeCounters::ws_video_bytes, frag.size());
        RuntimeStats::get().high_water(&RuntimeCounters::ws_video_out_peak, (int)c.out.size());
    }
}

bool HttpServer::pump_audio(Client& c) {
    if (!c.audio_sink) return true;
    if (c.audio_sink->closed()) { c.close_after_flush = true; return true; }   // audio switched off / shutdown
    // Two seconds of the wire format. A player that falls behind loses NEW
    // frames past that - old audio is worse than a gap, and two seconds is
    // the most a listener is ever behind live. One that stops reading
    // altogether is dropped after kAudioStallMs: a streaming socket has no
    // idle timeout, so nothing else would ever free its slot.
    const size_t cap = audio_stream_bytes(c.audio_fmt, c.audio_rate, 2);
    const int64_t t = now_ms();
    for (int i = 0; i < 16; ++i) {
        AuPtr au;
        if (!c.audio_sink->pop(au, 0)) break;
        if (!au || au->data.empty()) continue;
        const bool room = c.out.size() <= cap;
        if (room) c.audio_full_since_ms = 0; else if (!c.audio_full_since_ms) c.audio_full_since_ms = t;
        // Frames the sink dropped for us (a slow client) took their time too:
        // the container clock skips them, or everything after a drop would
        // play early.
        const uint32_t missing = (c.audio_seq_valid && au->seq > c.audio_last_seq + 1) ? au->seq - c.audio_last_seq - 1 : 0;
        c.audio_last_seq = au->seq; c.audio_seq_valid = true;
        if (!c.audio_enc) {
            if (room) audio_encode(c.audio_fmt, c.audio_rate, au->data.data(), au->data.size(), c.out);
            continue;
        }
        const uint32_t ts = c.audio_enc->timescale();
        const uint32_t frame_ticks = (uint32_t)((uint64_t)(au->data.size() / 2) * ts / (uint64_t)(c.audio_rate > 0 ? c.audio_rate : 8000));
        if (missing) {
            if (c.audio_fmt == AudioFormat::Opus) c.audio_ogg->skip(missing * frame_ticks);
            else c.audio_dt += (uint64_t)missing * frame_ticks;
        }
        // Compressed: always ENCODE (the encoder's state must see every
        // frame); what does not fit is skipped in the container's clock, so
        // the page sequence (Ogg) and the decode time (fMP4) stay honest.
        std::vector<audio::EncodedFrame> frames;
        c.audio_enc->encode(reinterpret_cast<const int16_t*>(au->data.data()), au->data.size() / 2, frames);
        for (const auto& fr : frames) {
            if (c.audio_fmt == AudioFormat::Opus) {
                if (room) c.out += c.audio_ogg->packet(fr.data, fr.duration);
                else      c.audio_ogg->skip(fr.duration);
            } else {
                if (room) {
                    const std::vector<uint8_t> frag = fmp4::fragment(c.audio_seq++, c.audio_dt, fr.duration, fr.data, true, 1);
                    c.out.append(reinterpret_cast<const char*>(frag.data()), frag.size());
                }
                c.audio_dt += fr.duration;
            }
        }
    }
    if (c.audio_full_since_ms && t - c.audio_full_since_ms > kAudioStallMs) {
        LOGW(MOD, "%s: audio client stopped reading for %d s - dropping", c.peer.c_str(), kAudioStallMs / 1000);
        return false;
    }
    return true;
}

// /ws/video&audio=: the microphone into the SAME MSE stream, as track 2. Its
// timeline starts at the video's decode time when the first audio frame is
// sent and then advances by the codec's own frame durations.
void HttpServer::pump_ws_audio(Client& c) {
    if (!c.ws_audio_sink || !c.ws_audio_enc) return;
    if (c.ws_audio_sink->closed()) {                       // microphone switched off: the video goes on
        if (audio_) audio_->unlisten(c.ws_audio_sink);
        c.ws_audio_sink.reset();
        return;
    }
    const size_t soft_cap = cfg_.ws_out_cap;
    for (int i = 0; i < 8; ++i) {
        AuPtr au;
        if (!c.ws_audio_sink->pop(au, 0)) return;
        if (!au || au->data.empty()) continue;
        const uint32_t missing = (c.ws_audio_seq_valid && au->seq > c.ws_audio_last_seq + 1) ? au->seq - c.ws_audio_last_seq - 1 : 0;
        c.ws_audio_last_seq = au->seq; c.ws_audio_seq_valid = true;
        std::vector<audio::EncodedFrame> frames;
        c.ws_audio_enc->encode(reinterpret_cast<const int16_t*>(au->data.data()), au->data.size() / 2, frames);
        if (!c.ws_init_sent) continue;                     // nothing to attach audio to yet
        const uint32_t ts = c.ws_audio_enc->timescale();
        // The video timeline absorbs a stall (>= 1 s without frames) into
        // its skew instead of advancing; the audio must follow it there, or
        // the two tracks drift apart by every stall. A skew change re-anchors
        // the audio on the video's decode time.
        if (c.ws_audio_started && c.ws_timeline.skew_us != c.ws_audio_skew_us) c.ws_audio_started = false;
        if (!c.ws_audio_started) {
            if (c.ws_await_key) continue;                   // anchor on a frame the player really has
            c.ws_audio_dt = c.ws_dts * ts / 90000; c.ws_audio_started = true;
            c.ws_audio_skew_us = c.ws_timeline.skew_us;
        } else if (missing) {
            // Frames the sink dropped (a slow socket) still took their time:
            // the audio clock skips them, or the track would run ahead of
            // the video by every drop. (Not on the anchoring frame: the
            // dropped ones are OLDER than the anchor.)
            c.ws_audio_dt += (uint64_t)missing * ((uint64_t)(au->data.size() / 2) * ts / (uint64_t)(c.ws_audio_enc->sample_rate() > 0 ? c.ws_audio_enc->sample_rate() : 8000));
        }
        for (const auto& fr : frames) {
            // The video's backpressure rule (audio is dropped first), and
            // while the video resyncs nothing is sent: the clock keeps
            // running either way, and no fragment is built for the bin.
            const bool send = !c.ws_await_key && c.out.size() <= soft_cap / 2;
            if (send) {
                const std::vector<uint8_t> frag = fmp4::fragment(c.ws_seq++, c.ws_audio_dt, fr.duration, fr.data, true, 2);
                queue_fmp4(c, frag, soft_cap);
            }
            c.ws_audio_dt += fr.duration;
        }
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

// Fork sysupgrade with its stdout+stderr merged into one non-blocking pipe we
// read from the poll loop. No shell: argv is executed directly, so the
// validated source token can not be a command. Returns false only if the fork
// machinery itself failed (the caller then speaks "cannot start sysupgrade").
bool HttpServer::spawn_upgrade(Client& c, const std::vector<std::string>& argv) {
    int fds[2];
    if (pipe(fds) != 0) return false;
    const pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return false; }
    if (pid == 0) {
        // Child: stdout+stderr -> pipe write end, detach from our controlling
        // session so a socket close can not deliver a signal to the flasher.
        dup2(fds[1], 1);
        dup2(fds[1], 2);
        close(fds[0]); close(fds[1]);
        setsid();
        std::vector<char*> a;
        a.reserve(argv.size() + 1);
        for (const auto& s : argv) a.push_back(const_cast<char*>(s.c_str()));
        a.push_back(nullptr);
        execv(a[0], a.data());
        // execv only returns on failure; say so on the same stream the page reads.
        const char* msg = "ERROR: cannot start sysupgrade\n";
        ssize_t wr = write(2, msg, strlen(msg)); (void)wr;
        _exit(127);
    }
    close(fds[1]);
    fcntl(fds[0], F_SETFL, O_NONBLOCK);
    c.upgrade_fd = fds[0];
    c.upgrade_pid = pid;
    return true;
}

// Client -> server on a /ws/upgrade socket: ONE JSON start frame, then pings
// and close. The start frame maps to a sysupgrade argv (compat::upgrade_plan)
// or a refusal; a second start frame is ignored (one flash per socket).
bool HttpServer::ws_upgrade_input(Client& c) {
    for (;;) {
        size_t used = 0; int op = 0; std::string payload;
        ws::Parse p = ws::parse_frame(c.in, used, op, payload, 8192);
        if (p == ws::Parse::Incomplete) return c.in.size() <= MAX_IN;
        if (p == ws::Parse::Bad) return false;
        c.in.erase(0, used);
        if (op == 8) return false;                             // close
        if (op == 9) { queue(c, ws::pong_frame(payload)); continue; }
        if (op != 1) continue;
        if (c.upgrade_started) continue;                       // one start per socket
        c.upgrade_started = true;

        const compat::UpgradePlan plan = compat::upgrade_plan(payload);
        if (plan.argv.empty()) {
            queue(c, ws::frame(true, plan.refusal.data(), plan.refusal.size()));
            c.close_after_flush = true;
            LOGI(MOD, "%s: /ws/upgrade refused start", c.peer.c_str());
            return true;
        }
        std::string cmd;
        for (const auto& s : plan.argv) { cmd += s; cmd += ' '; }
        if (!spawn_upgrade(c, plan.argv)) {
            const std::string why = "ERROR: cannot start sysupgrade\n\n"
                                    "The camera could not launch sysupgrade.";
            queue(c, ws::frame(true, why.data(), why.size()));
            c.close_after_flush = true;
            LOGW(MOD, "%s: /ws/upgrade spawn failed", c.peer.c_str());
            return true;
        }
        c.upgrade_last_ping_ms = now_ms();
        LOGI(MOD, "%s: /ws/upgrade started: %s(pid %d)", c.peer.c_str(), cmd.c_str(), (int)c.upgrade_pid);
    }
}

// Drain the upgrade child's pipe into text frames, keep the socket warm during
// the quiet download/time-sync phases, and close honestly when the child ends.
void HttpServer::pump_upgrade(Client& c, short revents) {
    if (c.upgrade_fd < 0) {
        // No child yet: ping the idle awaiting/starting socket so a NAT or the
        // browser does not drop it before the first frame.
        const int64_t t = now_ms();
        if (t - c.upgrade_last_ping_ms >= 10000) {
            c.upgrade_last_ping_ms = t;
            queue(c, ws::ping_frame());
        }
        return;
    }
    bool eof = false;
    if (revents & (POLLIN | POLLHUP | POLLERR)) {
        char buf[4096];
        for (;;) {
            ssize_t n = read(c.upgrade_fd, buf, sizeof buf);
            if (n > 0) {
                queue(c, ws::frame(true, buf, (size_t)n), cfg_.max_out_buffer);
                // Track the point of no return and the reboot announcement over a
                // rolling window so a marker split across two reads still counts.
                c.upgrade_win.append(buf, (size_t)n);
                if (c.upgrade_win.size() > 512) c.upgrade_win.erase(0, c.upgrade_win.size() - 512);
                if (!c.upgrade_saw_flash &&
                    (c.upgrade_win.find("Protected: flashing") != std::string::npos ||
                     c.upgrade_win.find("Flashing from RAM") != std::string::npos ||
                     c.upgrade_win.find("Stopping web server before flashing") != std::string::npos))
                    c.upgrade_saw_flash = true;
                if (!c.upgrade_saw_reboot &&
                    (c.upgrade_win.find("Unconditional reboot") != std::string::npos ||
                     c.upgrade_win.find("Rebooting now") != std::string::npos))
                    c.upgrade_saw_reboot = true;
                continue;
            }
            if (n == 0) { eof = true; break; }
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            eof = true; break;   // EIO etc.: the pipe is gone
        }
    }
    if (!eof) {
        const int64_t t = now_ms();
        if (t - c.upgrade_last_ping_ms >= 10000) {
            c.upgrade_last_ping_ms = t;
            queue(c, ws::ping_frame());  // keep the quiet phases alive
        }
        return;
    }
    // The child closed its stdout. Reap it and decide the epilogue.
    int status = 0;
    const pid_t r = (c.upgrade_pid > 0) ? waitpid(c.upgrade_pid, &status, WNOHANG) : 0;
    close(c.upgrade_fd); c.upgrade_fd = -1;
    if (c.upgrade_saw_reboot || c.upgrade_saw_flash) {
        // The flash reached the point of no return; the box is rebooting (or
        // sysupgrade said "flashing continues" and detached). Say nothing more -
        // the page already saw the markers and now polls for the camera to
        // return. Emitting "Upgrade did not complete" here would be a lie.
        LOGI(MOD, "%s: /ws/upgrade child ended after flash; awaiting reboot", c.peer.c_str());
    } else {
        // No flash markers were seen, so nothing was written. Only add the
        // "did not complete" ending on a GENUINE failure exit: a clean exit
        // (Same version / --no_reboot success) already said its piece, and the
        // die() path printed "<reason> Aborting." which the page keys on. A
        // WNOHANG that has not reaped yet (r == 0) is treated as "unknown, say
        // nothing" rather than a false failure.
        const bool failed = (r > 0) && (!WIFEXITED(status) || WEXITSTATUS(status) != 0);
        if (failed) {
            const std::string why = "\nUpgrade did not complete: sysupgrade exited without flashing.\n";
            queue(c, ws::frame(true, why.data(), why.size()));
        }
    }
    c.upgrade_pid = -1;
    c.close_after_flush = true;
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
        // Audio both ways, each only where it is switched on: the microphone
        // out (audio.enabled), talkback in (audio.outputEnabled). With either
        // off the answer says so in its direction, which is exactly what the
        // stock player reads ("a camera with audio.outputEnabled off answers
        // sendonly").
        const bool a_send = audio_ && audio_->available() && audio_->config().enabled;
        const bool a_recv = audio_ && audio_->output_available() && audio_->config().output_enabled;
        const std::string answer = sess->on_offer(data->as_string(), err,
                                                  c.rtc_unit >= 0 && c.rtc_unit < 4 ? h264_profile_[c.rtc_unit] : std::string(),
                                                  a_send, a_recv);
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
        if (c.rtc->audio_sending()) {
            std::string why;
            c.rtc_audio_sink = audio_->listen(why);
            c.rtc_audio_rate = audio_->sample_rate();
            if (!c.rtc_audio_sink) LOGW(MOD, "%s: webrtc audio: microphone refused (%s) - video only", c.peer.c_str(), why.c_str());
        }
        if (!reply("answer", answer)) return false;
        LOGI(MOD, "%s: webrtc session negotiated (unit %d)", c.peer.c_str(), c.rtc_unit);
    }
}

// Per tick: DTLS timers, PLI -> on-demand IDR, and the AU pump into RTP.
void HttpServer::pump_rtc(Client& c) {
    if (!c.rtc) return;
    if (c.rtc->stranded() && !c.close_after_flush) {
        // The media socket cannot reach the browser any more (source address
        // gone). Closing the signalling socket is what makes the page notice
        // now instead of after its own minutes-long timeout.
        LOGW(MOD, "%s: webrtc media path is gone (sends fail) - closing the session so the client reconnects",
             c.peer.c_str());
        c.close_after_flush = true;
        c.out.clear();
        return;
    }
    c.rtc->tick();
    c.rtc->log_stats();
    if (c.rtc->take_pli() && pipeline_) pipeline_->request_idr(c.rtc_unit);
    if (!c.rtc->media_ready() || !c.rtc_sink) return;
    // Talkback: whatever arrived since the last tick goes to the speaker
    // queue (8 kHz clips of a tick's length). A refusal (speaker switched off
    // meanwhile) drops the audio; the video session goes on.
    {
        std::vector<int16_t> talk;
        if (audio_ && c.rtc->take_audio_in(talk)) {
            std::string why;
            if (!audio_->play(std::move(talk), 8000, why)) {
                if (!c.rtc_talk_refused) LOGW(MOD, "%s: talkback dropped: %s", c.peer.c_str(), why.c_str());
                c.rtc_talk_refused = true;
            } else c.rtc_talk_refused = false;
        }
    }
    // The microphone out, as the negotiated G.711.
    if (c.rtc_audio_sink) {
        for (int k = 0; k < 4; ++k) {
            AuPtr a;
            if (!c.rtc_audio_sink->pop(a, 0)) break;
            if (!a || a->data.empty()) continue;
            // Sink-verworfene Frames ueberspringen den RTP-Takt (wie RTSP und
            // die HTTP-Audio-Pfade), sonst driftet WebRTC-Audio bei Congestion.
            const uint32_t missing = (c.rtc_audio_seq_valid && a->seq > c.rtc_audio_last_seq + 1)
                                         ? a->seq - c.rtc_audio_last_seq - 1 : 0;
            c.rtc_audio_last_seq = a->seq; c.rtc_audio_seq_valid = true;
            std::string g711;
            audio_encode(c.rtc->audio_is_pcma() ? AudioFormat::Alaw : AudioFormat::Ulaw, c.rtc_audio_rate,
                         a->data.data(), a->data.size(), g711);
            // Ein 40-ms-Frame @8 kHz = 320 Samples; die Luecke ist missing davon.
            c.rtc->send_audio(reinterpret_cast<const uint8_t*>(g711.data()), g711.size(),
                              missing * (uint32_t)g711.size());
        }
        if (c.rtc_audio_sink->closed()) { audio_->unlisten(c.rtc_audio_sink); c.rtc_audio_sink.reset(); }
    }
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

void HttpServer::drop_clients_on_vanished_addresses() {
    // The addresses that exist right now. A client bound to anything else is
    // stranded: its packets leave with a source the network no longer routes.
    std::vector<uint32_t> present;
    struct ifaddrs* ifa = nullptr;
    if (getifaddrs(&ifa) != 0) return;            // cannot tell: touch nothing
    for (struct ifaddrs* p = ifa; p; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
        present.push_back(((sockaddr_in*)p->ifa_addr)->sin_addr.s_addr);
    }
    freeifaddrs(ifa);
    if (present.empty()) return;                   // mid-reconfiguration: touch nothing

    int dropped = 0;
    for (auto& cp : clients_) {
        Client& c = *cp;
        if (c.fd < 0 || c.close_after_flush) continue;
        sockaddr_in la{}; socklen_t ll = sizeof la;
        if (getsockname(c.fd, (sockaddr*)&la, &ll) != 0) continue;
        if (std::find(present.begin(), present.end(), la.sin_addr.s_addr) != present.end()) continue;
        c.close_after_flush = true;
        c.out.clear();
        ++dropped;
    }
    if (dropped)
        LOGI(MOD, "address change: closed %d client(s) bound to an address that no longer exists", dropped);
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
            if (c->upgrade_fd >= 0) {                   // sysupgrade child stdout: stream it out
                pfds.push_back({c->upgrade_fd, POLLIN, 0});
                refs.push_back({c, 3});
            }
        }
        for (auto& c : clients_) if (c->mjpeg || c->ws_video || c->rtc || c->audio_sink || c->still) { timeout_ms = 20; break; }   // tick fast enough for the frame rate (a still: delivered within a tick of its capture)
        if (hls_) timeout_ms = 20;
        const size_t logs_idx = (logs_fd() >= 0) ? pfds.size() : (size_t)-1;
        if (logs_fd() >= 0) pfds.push_back({logs_fd(), POLLIN, 0});
        diag::set_http("poll");   // stall marker: HTTP thread waiting for I/O
        int n = poll(pfds.data(), pfds.size(), timeout_ms);
        int64_t t = now_ms();
        if (n > 0 && (pfds[0].revents & POLLIN)) accept_client();   // joins the NEXT poll cycle (not in refs)
        hls_pump(t);
        {
            const unsigned e = addr_epoch_.load(std::memory_order_acquire);
            if (e != seen_addr_epoch_) { seen_addr_epoch_ = e; drop_clients_on_vanished_addresses(); }
        }
        reap_stills(false);
        for (size_t i = 0; i < clients_.size(); ++i) {
            Client& c = *clients_[i];
            short re = 0, rre = 0, ure = 0, uge = 0;
            for (size_t k = 0; k < refs.size(); ++k)
                if (refs[k].c == &c) {
                    if (refs[k].kind == 1) rre = pfds[k + 1].revents;
                    else if (refs[k].kind == 2) ure = pfds[k + 1].revents;
                    else if (refs[k].kind == 3) uge = pfds[k + 1].revents;
                    else re = pfds[k + 1].revents;
                }
            bool ok = true;
            if (re & (POLLHUP | POLLERR | POLLNVAL)) ok = false;
            else if (re & POLLIN) {
                char buf[4096]; ssize_t r = recv(c.fd, buf, sizeof buf, MSG_DONTWAIT);
                if (r == 0) ok = false;
                else if (r < 0) { if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) ok = false; }
                else if (c.ws_video && c.ws_raw) { /* /video.mp4: nothing to read */ }
                else if (c.ws_video) { c.in.append(buf, (size_t)r); ok = ws_video_input(c); }
                else if (c.rtc_ws)   { c.in.append(buf, (size_t)r); ok = rtc_ws_input(c); }
                else if (c.ws_upgrade) { c.in.append(buf, (size_t)r); ok = ws_upgrade_input(c); }
                else if (c.streaming()) { /* ignore input on streaming connections */ }
                else if (c.parked()) { c.in.append(buf, (size_t)r); if (c.in.size() > input_cap(c.in)) ok = false; }   // pipelined: parsed after the answer
                else { c.in.append(buf, (size_t)r); if (c.in.size() > input_cap(c.in)) ok = false; else ok = pump_requests(c); }
            }
            if (ok && c.still && c.still->done.load(std::memory_order_acquire)) {
                finish_still(c);
                if (!c.close_after_flush) ok = pump_requests(c);
            }
            if (ok && c.hls_wait_until) {
                if (hls_answer_playlist(c)) ok = pump_requests(c);
                else if (t > c.hls_wait_until) {
                    c.hls_wait_until = 0;
                    queue(c, response(503, "application/json", "{\"error\":\"no HLS segment within 10 s\"}", c.hls_keep));
                    if (!c.hls_keep) c.close_after_flush = true;
                    else ok = pump_requests(c);
                }
            }
            if (ok && c.relay_state != Client::Relay::None) {
                ok = pump_relay(c, rre, t);
                // A request that arrived WHILE the relay was in flight is still
                // sitting in c.in, and its POLLIN is long gone - the parse loop
                // above only runs on fresh input. Before downstream keep-alive
                // this could not happen, because every relayed reply closed the
                // connection; now it can, and an unparsed request would hang
                // there until the idle timeout.
                if (ok && c.relay_state == Client::Relay::None && !c.close_after_flush && !c.streaming())
                    ok = pump_requests(c);
            }
            if (ok && c.sse) {
                drain_events(c);
                if (t - last_heartbeat_ms_ >= 15000) queue(c, ": keepalive\n\n");
            }
            if (ok && c.mjpeg && !c.close_after_flush) push_mjpeg(c);
            if (ok && c.ws_video && !c.close_after_flush) { pump_ws_video(c); pump_ws_audio(c); }
            if (ok && c.audio_sink && !c.close_after_flush) ok = pump_audio(c);
            if (ok && c.rtc) {
                if (ure & POLLIN) c.rtc->on_readable();
                pump_rtc(c);
            }
            if (ok && c.ws_upgrade && !c.close_after_flush) pump_upgrade(c, uge);
            if (ok) ok = flush(c);
            if (ok && c.close_after_flush && c.out.empty()) ok = false;
            // A running upgrade is exempt from the idle timeout: sysupgrade's
            // download and time-sync phases are silent for minutes and no recv
            // refreshes last_activity_ms. A socket still only AWAITING its start
            // frame is not exempt, so an abandoned handshake is still reaped.
            const bool upgrade_running = c.ws_upgrade && c.upgrade_started;
            if (ok && !c.streaming() && !upgrade_running && t - c.last_activity_ms > cfg_.idle_timeout_ms) ok = false;
            if (!ok) {
                // Close now, erase after the iteration: refs holds pointers
                // into clients_, so the vector must not shift under it.
                release_client(c);
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
