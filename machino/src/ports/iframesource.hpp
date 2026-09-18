// Port: a raw video channel (sensor/ISP output at a given geometry and rate).
#pragma once
#include "core/result.hpp"

namespace machino {

class IFrameSource {
public:
    virtual ~IFrameSource() = default;
    virtual Result enable()  = 0;
    virtual Result disable() = 0;
    virtual int    channel() const = 0;
};

} // namespace machino
