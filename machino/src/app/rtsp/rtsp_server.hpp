// Application: minimal RTSP/1.0 server for one H.264 stream.
//   OPTIONS / DESCRIBE / SETUP / PLAY / TEARDOWN (+ GET_PARAMETER keepalive)
//   RTP/AVP over interleaved TCP or UDP unicast; RFC 6184 single-NAL + FU-A.
//
// Demand semantics (M4): a connected socket is NOT demand. PLAY takes an RAII
// DemandHandle; TEARDOWN, disconnect, a dead socket (send stalls) or the
// session object going out of scope release it. DESCRIBE takes a scoped
// demand only when no SPS/PPS are cached yet.
//
// M8: two mount points on one port - cfg.path (main, unit 0) and cfg.sub_path
// (sub, unit 1) when a substream hub is wired. The request URL picks the unit;
// each stream has its own hub, demand unit, and cached SPS/PPS.
#pragma once
#include "core/config.hpp"
#include "core/lifecycle/pipeline_manager.hpp"
#include "core/stream_hub.hpp"
#include "app/rtsp/rtsp_auth.hpp"
#include "ports/rtsp_control.hpp"
#include "ports/stream_server.hpp"
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace machino {

class RtspServer final : public IStreamServer, public IRtspControl {
public:
    // `auth_check` is the same system-account validator the WebUI session gate
    // uses (crypt(3) against /etc/shadow) - the stock UI states RTSP
    // authenticates as root with the WebUI password, so there is no separate
    // RTSP account. Null = no credential source, auth stays off.
    // `claimed` reports whether the camera has been set up; while it says no,
    // RTSP answers 401 to everything, because upstream is explicit that an
    // unclaimed camera streams nothing. `unsafe` is majestic system.unsafe and
    // outranks both. Defaults keep the previous behaviour exactly.
    RtspServer(const RtspConfig& cfg, lifecycle::PipelineManager& pipeline, StreamHub& hub,
               StreamHub* sub_hub = nullptr, RtspAuth::CheckFn auth_check = nullptr,
               RtspAuth::ClaimFn claimed = nullptr, bool unsafe = false);
    ~RtspServer() override;
    Result start() override;
    void   stop() override;

    // IRtspControl: live reconfiguration, no daemon restart. Both serialise on
    // lifecycle_m_ so an API thread can never race the accept loop teardown.
    power::ApplyResult set_enabled(bool on) override;
    power::ApplyResult set_port(int port) override;
    bool listening() const;            // for tests/diagnostics

private:
    struct Session;
    // One connected client. `done` is set by its own thread as the very last
    // action, which is what lets the accept loop join and free the slot while
    // the server keeps running.
    struct Client {
        std::thread       th;
        int               fd = -1;          // -1 once the client thread closed it
        std::atomic<bool> done{false};
    };
    void accept_loop();
    Result open_listener(int port);    // bind+listen+acceptor (lifecycle_m_ held)
    void   close_listener();           // stop acceptor, drop sessions (lifecycle_m_ held)
    void reap_finished();
    void refuse(int fd, const std::string& peer);
    void client_loop(Client* c, std::string peer);
    bool handle_request(Session& s, const std::string& req);
    int  unit_from_url(const std::string& url) const;      // cfg.path -> main, cfg.sub_path -> sub
    StreamHub* hub_for(int unit) const;
    const std::string& path_for(int unit) const;
    bool obtain_params(int unit, std::vector<uint8_t>& sps, std::vector<uint8_t>& pps);
    bool send_au(Session& s, const AccessUnit& au);
    bool send_rtp(Session& s, const uint8_t* payload, size_t len, uint32_t ts, bool marker);

    RtspConfig  cfg_;
    lifecycle::PipelineManager& pipeline_;
    StreamHub&  hub_;                     // main stream (unit 0)
    StreamHub*  sub_hub_ = nullptr;       // substream (unit 1), or null when not configured
    int         listen_fd_ = -1;
    std::atomic<bool> quit_{false};
    std::thread acceptor_;
    std::mutex  clients_m_;
    std::vector<std::unique_ptr<Client>> clients_;
    RtspAuth    auth_;
    std::mutex  lifecycle_m_;            // serialises start/stop/set_enabled/set_port
    std::mutex  params_m_;
    std::vector<uint8_t> sps_[2], pps_[2];   // cached per unit (main, sub)
};

} // namespace machino
