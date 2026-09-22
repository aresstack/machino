// WS-Discovery responder: UDP 3702, multicast 239.255.255.250.
//
// Deliberately self-contained - its own socket and its own thread, like
// RtspServer - rather than another fd in the HTTP poll loop. That loop carries
// the live media path for /ws/video and /ws/webrtc, and a discovery responder
// is not worth any risk to it. Nothing here starts unless ONVIF is enabled.
//
// All message logic is in discovery.hpp and host-tested; this file is only the
// socket, and it is Linux-only (gated by the MIPS cross-build in CI).
#pragma once
#include "app/onvif/discovery.hpp"
#include "core/result.hpp"
#include <atomic>
#include <functional>
#include <string>
#include <thread>

namespace machino { namespace onvif {

class DiscoveryServer {
public:
    // `scopes` is the space-separated onvif:// scope list, `uuid_seed`
    // something durable enough that the endpoint reference survives a restart.
    // `http_port` is where the device service answers.
    DiscoveryServer(std::string uuid_seed, std::string scopes, int http_port);
    ~DiscoveryServer();

    Result start();
    void   stop();
    bool   running() const { return running_.load(); }

    // Diagnostics: how many probes were answered and how many were ignored
    // because they asked for something this camera is not.
    // 32-bit on purpose: MIPS32 has no lock-free 64-bit atomics, and a probe
    // counter that would need more than four billion is not a real concern.
    unsigned answered() const { return answered_.load(); }
    unsigned ignored() const  { return ignored_.load(); }

private:
    void loop();
    void announce(bool alive);       // Hello (true) / Bye (false), multicast
    // The local address this camera has towards `peer`, so the XAddrs it hands
    // back are reachable from where the client is standing. Determined per
    // reply rather than configured, which is the only thing that is right on a
    // camera with more than one interface.
    std::string local_address_for(const void* peer_sockaddr, unsigned peer_len) const;
    Announcement announcement_for(const std::string& local_ip) const;

    std::string uuid_, scopes_;
    int         http_port_;
    int         fd_ = -1;
    int         wake_[2] = {-1, -1};      // self-pipe, so stop() never waits for a timeout
    std::thread th_;
    std::atomic<bool> running_{false};
    std::atomic<unsigned> answered_{0}, ignored_{0};
    uint64_t    msg_counter_ = 0;
    // Reflection bound: a UDP source address cannot be verified, so every
    // reply is a packet a third party can be made to receive. Loop thread only.
    static const unsigned MAX_REPLIES_PER_SEC = 20;
    int64_t     rate_window_ = 0;
    unsigned    rate_count_ = 0;
};

}} // namespace machino::onvif
