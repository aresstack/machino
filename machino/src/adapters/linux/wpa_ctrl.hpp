// A minimal wpa_supplicant control-interface client.
//
// Why not just run wpa_cli: this daemon must not fork while the IMP pipeline
// is live. That is not a style preference -- it is the documented trigger of
// an out-of-memory incident on this camera, and the reason log_reader forks
// exactly once at start-up and never from a request. Shelling out for every
// scan would reintroduce it, with the added charm of doing so while a user
// watches the WiFi page.
//
// wpa_supplicant's control interface is a unix DATAGRAM socket that speaks a
// simple line protocol, so talking to it directly costs about a hundred lines
// and no process at all.
#pragma once
#include "core/result.hpp"
#include <string>
#include <vector>

namespace machino { namespace linuxsys {

class WpaCtrl {
public:
    // `iface_path` is the per-interface socket, e.g.
    // /var/run/wpa_supplicant/wlan0. Nothing is opened until first use, so
    // constructing this on a camera without WiFi is free.
    explicit WpaCtrl(std::string iface_path);
    ~WpaCtrl();

    WpaCtrl(const WpaCtrl&) = delete;
    WpaCtrl& operator=(const WpaCtrl&) = delete;

    bool available() const;          // the socket exists

    // One request, one reply. `timeout_ms` bounds the wait; wpa_supplicant
    // answers immediately for everything we ask.
    Result request(const std::string& cmd, std::string& reply, int timeout_ms = 2000);

    // Convenience: returns true when the reply is exactly "OK".
    bool ok_request(const std::string& cmd, int timeout_ms = 2000);

    void close();

private:
    Result ensure_open();

    std::string iface_path_;
    std::string local_path_;   // our end; unlinked on close
    int         fd_ = -1;
};

}} // namespace machino::linuxsys
