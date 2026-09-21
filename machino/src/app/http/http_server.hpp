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
    int         upstream_port = 0;
    int         relay_timeout_ms = 6000;      // upstream INACTIVITY bound: refreshed on connect/send/recv progress
    int         relay_max_ms = 120000;        // absolute safety ceiling per relayed request
    // Each relayed request makes busybox fork a CGI (shell + helpers). On a
    // 128 MiB camera whose userspace is ~43 MiB a browser dashboard firing a
    // dozen fetches at once is a real OOM risk, so excess relays queue.
    int         max_relay_inflight = 3;
    size_t      max_relay_bytes = 8 * 1024 * 1024;   // total bytes forwarded per relayed request
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
    HttpServer(const ServerConfig& cfg, api::ApiService& api, EventBus& bus,
               StreamHub* hub = nullptr, lifecycle::PipelineManager* pipeline = nullptr);
    ~HttpServer();
    Result start();
    void   stop();
    int    port() const { return cfg_.port; }

private:
    struct Client;
    void loop();
    void accept_client();
    bool handle_request(Client& c);
    void drain_events(Client& c);
    void push_mjpeg(Client& c);     // multipart JPEG frames for an /api/v1/stream.mjpeg client
    void pump_ws_video(Client& c);  // fMP4-per-frame over WebSocket (majestic /ws/video)
    bool ws_video_input(Client& c); // client frames: {"request":"idr"}, ping, close
    bool relay_upstream(Client& c, const Request& req); // start (or queue) a non-blocking upstream relay
    bool relay_open(Client& c);                          // open the upstream socket for a prepared relay
    bool pump_relay(Client& c, short re, int64_t now);   // advance it; false drops the client
    bool flush(Client& c);
    bool queue(Client& c, const std::string& data, size_t cap = 0);   // cap 0 = max_out_buffer

    ServerConfig      cfg_;
    api::ApiService&  api_;
    EventBus&         bus_;
    StreamHub*        hub_ = nullptr;
    lifecycle::PipelineManager* pipeline_ = nullptr;
    std::unique_ptr<SessionGate> gate_;   // set when cfg_.session_auth
    int               listen_fd_ = -1;
    std::atomic<bool> quit_{false};
    std::thread       thread_;
    std::vector<std::unique_ptr<Client>> clients_;
    int64_t           last_telemetry_ms_ = 0;
    int64_t           last_heartbeat_ms_ = 0;
};

}} // namespace machino::http
