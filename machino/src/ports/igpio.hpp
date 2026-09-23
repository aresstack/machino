// Port: a general-purpose IO controller, addressed by LOGICAL pin name.
//
// The core never learns a Linux GPIO number. It says "PB18"; the platform
// backend decides what that means -- on Ingenic T40 it is global GPIO 50, on
// another SoC it is something else entirely, and on a board without GPIO the
// controller simply reports the pin as unknown.
//
// Why a name and not a number: the number is a property of one kernel's
// gpiolib numbering, not of the board. Putting it in the config or in the API
// would bake a platform detail into the contract, and the first non-Ingenic
// target would have to break it.
//
// Safety is part of this port, not an afterthought. A camera SoC routes the
// sensor reset, the Ethernet PHY reset, the SPI flash and I2C through the same
// GPIO space; writing the wrong pin can take the board off the network or stop
// the video. `holder_of` lets a caller refuse a pin that a driver has already
// claimed instead of finding out by breaking something.
#pragma once
#include "core/result.hpp"
#include <string>
#include <vector>

namespace machino {

struct GpioPinInfo {
    std::string name;      // logical, e.g. "PB18"
    int         number;    // platform-native id; -1 when the backend has none
    bool        claimed;   // some driver holds it
    std::string holder;    // who, when the platform can say
    std::string direction; // "in", "out" or "" when unknown
};

class IGpioController {
public:
    virtual ~IGpioController() = default;

    // Does this platform expose GPIO at all? A backend that returns false must
    // make every other call fail with Unsupported rather than pretend.
    virtual bool available() const = 0;

    // "PB18" -> platform id. False when the name is not valid on this platform.
    virtual bool resolve(const std::string& name, int& number_out) const = 0;

    // Who owns the pin right now, as far as the platform can tell. Returns
    // false when the pin is free or the platform cannot answer; `info` is
    // filled in either way when the pin resolves.
    virtual bool holder_of(const std::string& name, GpioPinInfo& info) const = 0;

    // Take the pin as an output with a defined initial level. Must fail with
    // Busy when another driver holds it -- never steal.
    virtual Result configure_output(const std::string& name, bool initial_level) = 0;

    virtual Result write(const std::string& name, bool level) = 0;
    virtual Result read(const std::string& name, bool& level_out) const = 0;

    // Give the pin back. Idempotent.
    virtual void release(const std::string& name) = 0;
};

} // namespace machino
