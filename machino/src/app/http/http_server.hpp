// Application: small HTTP/1.1 + SSE server for /api/v1. One thread, poll()
// over all sockets, non-blocking I/O, bounded per-client buffers, bounded
// client count, idle timeout. Requests are dispatched synchronously to the
// ApiService (which serialises PATCHes). An SSE client owns an EventBus
// subscription; a slow client whose output buffer or subscription overflows
// is disconnected - the media path is never blocked by HTTP.
// API access is never media demand.
#pragma once
#include "app/api/api_service.hpp"
#include "core/events.hpp"
#include "core/result.hpp"
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace machino { namespace http {

struct ServerConfig {
    std::string bind = "0.0.0.0";
    int         port = 8080;
    int         max_clients = 16;
    int         idle_timeout_ms = 30000;      // non-SSE keep-alive idle
    size_t      max_out_buffer = 64 * 1024;    // per client for API/SSE; overflow -> disconnect
    size_t      max_snapshot_bytes = 4 * 1024 * 1024;  // a full-res JPEG response may exceed the API cap
    int         telemetry_interval_ms = 1000; // SSE telemetry rate (only while SSE clients exist)
    int         mjpeg_max_fps = 10;           // /api/v1/stream.mjpeg cap (JPEG snapshot-driven)
};

class HttpServer {
public:
    HttpServer(const ServerConfig& cfg, api::ApiService& api, EventBus& bus);
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
    bool flush(Client& c);
    bool queue(Client& c, const std::string& data, size_t cap = 0);   // cap 0 = max_out_buffer

    ServerConfig      cfg_;
    api::ApiService&  api_;
    EventBus&         bus_;
    int               listen_fd_ = -1;
    std::atomic<bool> quit_{false};
    std::thread       thread_;
    std::vector<std::unique_ptr<Client>> clients_;
    int64_t           last_telemetry_ms_ = 0;
    int64_t           last_heartbeat_ms_ = 0;
};

}} // namespace machino::http
