// Ingenic adapter: translation of the abstract hardware description into the
// plain parameters the IMP sensor registration needs. This header has no IMP
// dependency so the conversion is unit-testable on the host; the adapter
// copies the result into IMPSensorInfo (see ingenic_platform.cpp).
#pragma once
#include "core/hw/resolve.hpp"
#include <string>

namespace machino { namespace ingenic {

struct SensorParams {
    std::string name;          // vendor driver name == sensor model
    int i2c_bus    = 0;
    int i2c_addr   = 0;
    int mclk       = 0;        // IMPSensorMclk index
    int reset_gpio = -1;       // -1 -> the vendor driver skips the pin
    int pwdn_gpio  = -1;
    int power_gpio = -1;       // never used ("invalid now" in the SDK)
    bool mipi      = true;     // video_interface: MIPI CSI0 vs DVP
};

// Returns false with `err` when the description cannot be mapped (unknown
// interface, missing bus/address).
bool sensor_params_from(const hw::ResolvedHardware& hw, SensorParams& out, std::string& err);

}} // namespace machino::ingenic
