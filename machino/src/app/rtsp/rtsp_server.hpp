// Application: minimal RTSP/1.0 server for one H.264 stream.
//   OPTIONS / DESCRIBE / SETUP / PLAY / TEARDOWN (+ GET_PARAMETER keepalive)
//   RTP/AVP over interleaved TCP or UDP unicast; RFC 6184 single-NAL + FU-A.
// Each session is a pipeline consumer: PLAY -> acquire(), TEARDOWN or
// disconnect -> release(). DESCRIBE takes a short-lived reference to obtain
// SPS/PPS for the SDP when none are cached yet.
#pragma once
#include "core/config.hpp"
#include "core/pipeline.hpp"
#include "core/stream_hub.hpp"
#include "ports/stream_server.hpp"
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace machino {

class RtspServer final : public IStreamServer {
public:
    RtspServer(const RtspConfig& cfg, Pipeline& pipeline, StreamHub& hub);
    ~RtspServer() override;
    Result start() override;
    void   stop() override;

private:
    struct Session;
    void accept_loop();
    void client_loop(int fd, std::string peer);
    bool handle_request(Session& s, const std::string& req);
    bool obtain_params(std::vector<uint8_t>& sps, std::vector<uint8_t>& pps);
    void send_au(Session& s, const AccessUnit& au);
    bool send_rtp(Session& s, const uint8_t* payload, size_t len, uint32_t ts, bool marker);

    RtspConfig  cfg_;
    Pipeline&   pipeline_;
    StreamHub&  hub_;
    int         listen_fd_ = -1;
    std::atomic<bool> quit_{false};
    std::thread acceptor_;
    std::mutex  clients_m_;
    std::vector<std::thread> clients_;
    std::vector<int>         client_fds_;
    std::mutex  params_m_;
    std::vector<uint8_t> sps_, pps_;
};

} // namespace machino
