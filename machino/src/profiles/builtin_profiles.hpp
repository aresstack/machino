// Built-in hardware data: platforms, sensors and board profiles known to this
// build. This is DATA outside the core; the core only sees the registry.
//
// Board profiles describe ONE board each. Wiring (bus, address, clock, pins)
// is board wiring - a profile never claims to cover every camera with the
// same SoC and sensor.
#pragma once
#include "core/hw/registry.hpp"

namespace machino { namespace profiles {

void register_builtin(hw::Registry& reg);

}} // namespace machino::profiles
