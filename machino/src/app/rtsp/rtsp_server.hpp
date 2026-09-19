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
#include "ports/stream_server.hpp"
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace machino {

class RtspServer final : public IStreamServer {
public:
    RtspServer(const RtspConfig& cfg, lifecycle::PipelineManager& pipeline, StreamHub& hub,
               StreamHub* sub_hub = nullptr);
    ~RtspServer() override;
    Result start() override;
    void   stop() override;

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
    std::mutex  params_m_;
    std::vector<uint8_t> sps_[2], pps_[2];   // cached per unit (main, sub)
};

} // namespace machino
