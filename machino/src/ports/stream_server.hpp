// Port: a network stream server (RTSP today; others later). The application
// only starts/stops it; the server is a pipeline consumer through
// Pipeline::acquire()/release() and reads frames from the StreamHub.
#pragma once
#include "core/result.hpp"

namespace machino {

class IStreamServer {
public:
    virtual ~IStreamServer() = default;
    virtual Result start() = 0;
    virtual void   stop()  = 0;
};

} // namespace machino
