// Port: runtime control of the RTSP listener, so the config API can change
// rtsp.enabled / rtsp.port without a daemon restart. Kept behind an interface
// because the API layer must not depend on the socket implementation (and the
// host tests drive a fake).
//
// Semantics both implementations must honour:
//   set_enabled(false) closes the listener AND ends running sessions; their
//   DemandHandles release, so the pipeline winds down through the normal grace
//   path. set_enabled(true) re-binds without a process restart.
//   set_port(n) re-binds atomically: on failure the previous working listener
//   stays up and the call is rejected - never a half-dead RTSP.
#pragma once
#include "core/power/apply.hpp"

namespace machino {

class IRtspControl {
public:
    virtual ~IRtspControl() = default;
    virtual power::ApplyResult set_enabled(bool on) = 0;
    virtual power::ApplyResult set_port(int port) = 0;
};

} // namespace machino
