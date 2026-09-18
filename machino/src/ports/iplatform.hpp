// Port: the SoC media platform. Brings the sensor/ISP up and down, creates
// frame-source and encoder channels and wires them, and reports what it can
// do. The core never includes a vendor header; the adapter is constructed
// from the resolved, abstract hardware description.
#pragma once
#include "core/capabilities.hpp"
#include "core/config.hpp"
#include "core/result.hpp"
#include "ports/iencoder.hpp"
#include "ports/iframesource.hpp"
#include <cstdint>
#include <memory>

namespace machino {

class IPlatform {
public:
    virtual ~IPlatform() = default;
    virtual const char* name() const = 0;
    virtual CapabilitySet capabilities() const = 0;

    // Sensor + ISP + system init. Idempotent; tear_down() undoes it fully.
    virtual Result bring_up() = 0;
    virtual void   tear_down() = 0;

    virtual std::unique_ptr<IFrameSource> create_framesource(int chn, const EffectiveStream& sc) = 0;
    virtual std::unique_ptr<IEncoder>     create_encoder(int chn, const EffectiveStream& sc) = 0;

    virtual Result bind(IFrameSource& fs, IEncoder& enc)   = 0;
    virtual Result unbind(IFrameSource& fs, IEncoder& enc) = 0;

    virtual int64_t timestamp_us() = 0;
};

} // namespace machino
