// Port: the SoC media platform. Brings the sensor/ISP up and down, creates
// frame-source and encoder channels and wires them, reports what it can do,
// and (M5) exposes the live sensor-rate control and the power-control port.
// The core never includes a vendor header.
#pragma once
#include "core/capabilities.hpp"
#include "core/config.hpp"
#include "core/result.hpp"
#include "ports/iencoder.hpp"
#include "ports/iframesource.hpp"
#include "ports/iimage_control.hpp"
#include "ports/ipower_control.hpp"
#include <cstdint>
#include <memory>

namespace machino {

class IPlatform {
public:
    virtual ~IPlatform() = default;
    virtual const char* name() const = 0;
    virtual CapabilitySet capabilities() const = 0;

    virtual Result bring_up() = 0;
    virtual void   tear_down() = 0;

    virtual std::unique_ptr<IFrameSource> create_framesource(int chn, const EffectiveStream& sc) = 0;
    virtual std::unique_ptr<IEncoder>     create_encoder(int chn, const EffectiveStream& sc) = 0;

    virtual Result bind(IFrameSource& fs, IEncoder& enc)   = 0;
    virtual Result unbind(IFrameSource& fs, IEncoder& enc) = 0;

    virtual int64_t timestamp_us() = 0;

    // Live sensor frame rate (only while brought up). `effective` is read back
    // from the hardware when the platform can do that. Default: unsupported.
    virtual Result set_sensor_fps(int fps, int& effective) { (void)fps; effective = -1; return Result::unsupported(); }
    virtual Result get_sensor_fps(int& fps) { fps = -1; return Result::unsupported(); }

    // Power/performance control, or nullptr when the adapter has none.
    virtual IPowerControl* power() { return nullptr; }
    // Image (ISP) control, or nullptr when the adapter has none.
    virtual IImageControl* image() { return nullptr; }
};

} // namespace machino
