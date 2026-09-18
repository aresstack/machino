// Ingenic adapter: how the sensor is wired to the SoC on a concrete board.
// This is the quirk that cost M1: the T40 tx-isp driver takes rst_gpio /
// pwdn_gpio from the AddSensor ioctl and pulses the reset before the chip-id
// probe. Passing -1 skips the pulse -> sensor never wakes -> i2c EIO.
//
// Known boards (verified 2026-09-18 on OpenIPC, kernel 4.4.94):
//   T40NN + IMX307:  i2c1 @0x1a, MCLK1, reset_gpio 91, pwdn_gpio 0
#pragma once
#include "core/config.hpp"

namespace machino { namespace ingenic {

struct BoardWiring {
    int i2c_bus;
    int i2c_addr;
    int mclk;         // IMPSensorMclk index
    int reset_gpio;   // -1 = no such pin
    int pwdn_gpio;    // -1 = no such pin
};

inline BoardWiring wiring_from_config(const SensorBusConfig& b) {
    return BoardWiring{ b.i2c_bus, b.i2c_addr, b.mclk, b.reset_gpio, b.pwdn_gpio };
}

}} // namespace machino::ingenic
