// Machino core: hardware abstraction types.
//
//   PlatformDescriptor  which SoC family/model an adapter drives
//   SensorDescriptor    what a sensor is (interface, native size, modes)
//   SensorWiring        how a sensor is wired on ONE board (bus, address, clock, pins)
//   BoardProfile        platform + sensor + wiring + default mode for ONE board
//
// The core never contains concrete values (no "t40nn", "imx307", no GPIO
// numbers). Data lives in profiles (src/profiles) or the user configuration.
// All wiring fields are optional: "unset" is a first-class, fail-closed state.
#pragma once
#include <optional>
#include <string>
#include <vector>

namespace machino { namespace hw {

struct PlatformDescriptor {
    std::string vendor;   // e.g. "ingenic"  (adapter selector)
    std::string family;   // e.g. "t40"
    std::string model;    // e.g. "t40nn"
    std::string id() const { return vendor + "-" + model; }
};

struct SensorMode {
    int width  = 0;
    int height = 0;
    int fps    = 0;
    bool operator==(const SensorMode& o) const { return width == o.width && height == o.height && fps == o.fps; }
};

enum class SensorInterface { Unknown, MipiCsi, Dvp };

struct SensorDescriptor {
    std::string     model;                 // e.g. "imx307" (as the vendor driver names it)
    SensorInterface interface = SensorInterface::Unknown;
    int             native_width  = 0;
    int             native_height = 0;
    std::vector<SensorMode> modes;         // verified modes only; index 0 = default

    bool has_mode(const SensorMode& m) const {
        for (const auto& x : modes) if (x == m) return true;
        return false;
    }
    const SensorMode* default_mode() const { return modes.empty() ? nullptr : &modes[0]; }
};

struct SensorWiring {
    std::optional<int> i2c_bus;
    std::optional<int> i2c_addr;
    std::optional<int> mclk;         // vendor clock index
    std::optional<int> reset_gpio;   // -1 == "no such pin" (explicitly none)
    std::optional<int> pwdn_gpio;    // -1 == "no such pin"
};

struct BoardProfile {
    std::string  board_id;           // unique id of this profile
    std::string  platform;           // PlatformDescriptor::id(), e.g. "ingenic-t40nn"
    std::string  sensor;             // SensorDescriptor::model
    SensorWiring wiring;
    std::optional<SensorMode> default_mode;
    bool         hardware_verified = false;
    std::string  notes;
};

}} // namespace machino::hw
