// Application: small HTTP/1.1 + SSE server for /api/v1. One thread, poll()
// over all sockets, non-blocking I/O, bounded per-client buffers, bounded
// client count, idle timeout. Requests are dispatched synchronously to the
// ApiService (which serialises PATCHes). An SSE client owns an EventBus
// subscription; a slow client whose output buffer or subscription overflows
// is disconnected - the media path is never blocked by HTTP.
// API access is never media demand.
#pragma once
#include "app/api/api_service.hpp"
#include "app/http/session.hpp"
#include "app/http/setup.hpp"
#include "app/log_reader.hpp"
#include "app/onvif/onvif_service.hpp"
#include "app/osd/osd_service.hpp"
#include "app/api/net_api.hpp"
#include "core/events.hpp"
#include "core/lifecycle/pipeline_manager.hpp"
#include "core/result.hpp"
#include "core/stream_hub.hpp"
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <sys/types.h>   // pid_t for the pending-reap list

namespace machino { namespace http {

struct Request; // http_parse.hpp

struct ServerConfig {
    std::string bind = "0.0.0.0";
    int         port = 8080;
    int         max_clients = 16;
    int         idle_timeout_ms = 30000;      // non-SSE keep-alive idle
    size_t      max_out_buffer = 64 * 1024;    // per client for API/SSE; overflow -> disconnect
    size_t      max_snapshot_bytes = 4 * 1024 * 1024;  // a full-res JPEG response may exceed the API cap
    // /ws/video output cap: at 3 Mbit/s the pause threshold (cap/2) holds well
    // under a second of video, so a stalled viewer never accumulates stale
    // frames; an init segment plus one IDR fragment still fits comfortably.
    size_t      ws_out_cap = 512 * 1024;
    int         telemetry_interval_ms = 1000; // SSE telemetry rate (only while SSE clients exist)
    int         mjpeg_max_fps = 10;           // /api/v1/stream.mjpeg cap (JPEG snapshot-driven)
    // Front-door relay: any request that is not a native Machino/Majestic route
    // is forwarded to this internal OpenIPC WebUI (busybox httpd). port 0 = off.
    std::string upstream_host = "127.0.0.1";
    // majestic system.unsafe: authentication off for every endpoint, unclaimed
    // cameras included. Upstream calls this the supported way to run a
    // deliberately-open camera.
    bool        unsafe = false;
    int         upstream_port = 0;
    int         relay_timeout_ms = 6000;      // upstream INACTIVITY bound: refreshed on connect/send/recv progress
    int         relay_max_ms = 120000;        // absolute safety ceiling per relayed request
    // Each relayed request makes busybox fork a CGI (shell + helpers). On a
    // 128 MiB camera whose userspace is ~43 MiB a browser dashboard firing a
    // dozen fetches at once is a real OOM risk, so excess relays queue.
    int         max_relay_inflight = 3;
    size_t      max_relay_bytes = 8 * 1024 * 1024;   // total bytes forwarded per relayed request
    // How far into an HTML page we scan for the navbar anchor before giving up
    // and serving it unchanged. The OpenIPC navbar is at the very top of <body>,
    // so a small window suffices; it also bounds what is held in memory and stays
    // under max_out_buffer so the injected prefix fits one queued write.
    size_t      max_inject_bytes = 48 * 1024;
    // Majestic drop-in session auth (POST /login, POST /logout, 401 gating).
    // Active only when both are set; auth_check validates the credentials.
    bool                  session_auth = false;
    SessionGate::CheckFn  auth_check;
};

class HttpServer {
public:
    // hub/pipeline are the /ws/video media wiring (majestic-webui Live): a WS
    // client is a StreamHub consumer with its OWN DemandHandle, exactly like
    // an RTSP session - no second encoder, no JPEG path. Null = route off.
    // `sub_hub` is the substream's AU feed (null = no substream): stream=1 on
    // /ws/video and /ws/webrtc serves from it with UNIT_SUB demand, exactly
    // like RTSP's second mount point.
    HttpServer(const ServerConfig& cfg, api::ApiService& api, EventBus& bus,
               StreamHub* hub = nullptr, lifecycle::PipelineManager* pipeline = nullptr,
               StreamHub* sub_hub = nullptr);
    ~HttpServer();
    // AP9: the OSD surface (/api/v1/osd and /api/v1/osd/image). Null = the
    // routes answer 404, which the stock settings page reads as "this build
    // cannot say" and stops polling.
    void set_osd(osd::OsdService* o) { osd_ = o; }

    // AP10: the unclaimed / first-run gate. Null = this build has no claim
    // concept and behaves exactly as before.
    void set_setup(SetupGate* s) { setup_ = s; }

    // AP11: the ONVIF surface under /onvif/. Null = the paths are not native
    // and fall through to the relay, exactly as before.
    void set_onvif(onvif::OnvifService* o) { onvif_ = o; }

    // The one logread child, forked in main() before any IMP initialisation.
    // Null = no log streaming; /ws/logs then accepts and closes at once.
    void set_log_reader(LogReader* r) { log_reader_ = r; }

    // AP35/AP36: the USB and connectivity surfaces. Null = none of
    // /api/v1/usb* or /api/v1/network* exists, which is what a build without a
    // USB backend or a radio should look like to a client.
    void set_net_api(api::NetApiService* n) { net_api_ = n; }

    Result start();
    void   stop();
    int    port() const { return cfg_.port; }

private:
    struct Client;
    osd::OsdService* osd_ = nullptr;
    SetupGate*       setup_ = nullptr;
    onvif::OnvifService* onvif_ = nullptr;
    api::NetApiService*  net_api_ = nullptr;
    void loop();
    void accept_client();
    // Parse and serve every complete request already buffered in c.in. Stops at
    // a relay going in flight (no pipelining behind a relayed reply) and at a
    // connection marked for close.
    bool pump_requests(Client& c);
    bool handle_request(Client& c);
    void drain_events(Client& c);
    void push_mjpeg(Client& c);     // multipart JPEG frames for an /api/v1/stream.mjpeg client
    void pump_ws_video(Client& c);  // fMP4-per-frame over WebSocket (majestic /ws/video)
    bool ws_video_input(Client& c); // client frames: {"request":"idr"}, ping, close
    bool rtc_ws_input(Client& c);   // /ws/webrtc signalling: offer -> answer/busy/error
    // /ws/logs: ONE shared "logread -f" child feeds every subscriber, its pipe
    // rides the same poll() so nothing blocks the media path. Started with the
    // first subscriber, reaped with the last.
    // The logread child is NOT started or stopped here any more: it is forked
    // once in main() before IMP can exist, and /ws/logs only adds and removes
    // subscribers. See app/log_reader.hpp for why that matters on this camera.
    void logs_pump(short revents);
    bool logs_wanted() const;
    int  logs_fd() const;
    void pump_rtc(Client& c);       // webrtc per tick: DTLS timers, PLI->IDR, AU->RTP
    // AP16: what this camera actually emits, per unit, learned from the SPS of
    // any key frame that happens to pass through - never by waiting for one.
    // The RTSP side may block up to 3 s for an IDR to answer DESCRIBE; the
    // poll loop may not, and an SDP answer that arrives late is worse than an
    // answer that follows the browser's own codec preference.
    // Written and read only from the poll loop, like every other Client-facing
    // member here - no lock, and none needed.
    void note_h264_profile(int unit, const std::vector<uint8_t>& sps);
    std::string h264_profile_[4];   // profile-level-id, "" until first seen
    bool relay_upstream(Client& c, const Request& req); // start (or queue) a non-blocking upstream relay
    bool relay_open(Client& c);                          // open the upstream socket for a prepared relay
    bool pump_relay(Client& c, short re, int64_t now);   // advance it; false drops the client
    bool flush(Client& c);
    bool queue(Client& c, const std::string& data, size_t cap = 0);   // cap 0 = max_out_buffer

    ServerConfig      cfg_;
    api::ApiService&  api_;
    EventBus&         bus_;
    StreamHub*        hub_ = nullptr;
    StreamHub*        sub_hub_ = nullptr;
    lifecycle::PipelineManager* pipeline_ = nullptr;
    bool sub_available() const;                       // substream configured and fed
    // -1 = not served; else the lifecycle unit for a ?stream= query value
    int  unit_for_stream(const std::string& sv) const;
    std::unique_ptr<SessionGate> gate_;   // set when cfg_.session_auth
    int               listen_fd_ = -1;
    LogReader*        log_reader_ = nullptr;   // owned by main, forked before IMP

    std::string       logs_buf_;          // partial line carried between reads
    std::atomic<bool> quit_{false};
    std::thread       thread_;
    std::vector<std::unique_ptr<Client>> clients_;
    int64_t           last_telemetry_ms_ = 0;
    int64_t           last_heartbeat_ms_ = 0;
};

}} // namespace machino::http
