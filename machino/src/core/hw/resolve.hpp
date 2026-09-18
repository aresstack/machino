// Machino core: resolve the effective hardware description with a strict,
// logged precedence:
//
//     explicit user config  >  board profile  >  safe platform defaults  >  fail closed
//
// Rules
//   * GPIO numbers are never guessed. If neither user nor board set a pin,
//     the effective value is "none" (-1) and no GPIO is ever touched.
//   * i2c bus/address and the sensor model have no safe default: missing ->
//     the resolution fails (unsupported), it does not invent values.
//   * Every effective value carries its source so the startup log can show
//     "reset_gpio=91 [board-profile]" and conflicts are reported.
#pragma once
#include "core/hw/descriptors.hpp"
#include "core/hw/registry.hpp"
#include <string>

namespace machino { namespace hw {

enum class Source { None, PlatformDefault, BoardProfile, UserConfig };
const char* source_name(Source s);

template <typename T>
struct Resolved {
    T      value{};
    Source source = Source::None;
    bool   set() const { return source != Source::None; }
};

// What the user explicitly configured (all optional).
struct UserHardwareConfig {
    std::string  board_id;        // selects a registered board profile ("" = none)
    std::string  platform;        // overrides/sets the platform id
    std::string  sensor;          // overrides/sets the sensor model
    SensorWiring wiring;          // explicit wiring overrides
    std::optional<SensorMode> mode;
};

// Conservative per-platform defaults an adapter may declare. Deliberately
// has no GPIO entries: pins are board wiring, never a platform default.
struct PlatformDefaults {
    std::optional<int> mclk;
};

struct ResolvedHardware {
    PlatformDescriptor platform;
    std::string        board_id;          // "" when no profile was used
    bool               board_verified = false;
    SensorDescriptor   sensor;
    Resolved<int>      i2c_bus, i2c_addr, mclk, reset_gpio, pwdn_gpio;
    Resolved<SensorMode> mode;
    std::string        conflicts;         // human-readable list, "" when none
};

// Returns false with `err` when the result would be unsafe/unsupported.
bool resolve_hardware(const UserHardwareConfig& user, const Registry& reg,
                      const PlatformDefaults& defaults, ResolvedHardware& out, std::string& err);

// Compact startup summary (INFO) + per-value provenance (DEBUG).
void log_resolved_hardware(const ResolvedHardware& hw);

}} // namespace machino::hw
