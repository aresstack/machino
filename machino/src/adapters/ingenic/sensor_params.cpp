#include "adapters/ingenic/sensor_params.hpp"

namespace machino { namespace ingenic {

bool sensor_params_from(const hw::ResolvedHardware& hw, SensorParams& out, std::string& err) {
    if (hw.sensor.model.empty()) { err = "no sensor model"; return false; }
    if (!hw.i2c_bus.set() || !hw.i2c_addr.set()) { err = "i2c bus/address not resolved"; return false; }
    if (!hw.mclk.set()) { err = "mclk not resolved"; return false; }
    SensorParams p;
    p.name       = hw.sensor.model;
    p.i2c_bus    = hw.i2c_bus.value;
    p.i2c_addr   = hw.i2c_addr.value;
    p.mclk       = hw.mclk.value;
    // A pin that nobody configured stays -1: the vendor driver then never
    // requests/toggles it. Never substitute a "plausible" number here.
    p.reset_gpio = hw.reset_gpio.set() ? hw.reset_gpio.value : -1;
    p.pwdn_gpio  = hw.pwdn_gpio.set()  ? hw.pwdn_gpio.value  : -1;
    p.power_gpio = -1;
    switch (hw.sensor.interface) {
        case hw::SensorInterface::MipiCsi: p.mipi = true;  break;
        case hw::SensorInterface::Dvp:     p.mipi = false; break;
        case hw::SensorInterface::Unknown: err = "sensor interface unknown (mipi-csi or dvp required)"; return false;
    }
    out = p;
    return true;
}

}} // namespace machino::ingenic
