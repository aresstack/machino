#include "core/hw/resolve.hpp"
#include "core/log.hpp"

namespace machino { namespace hw {

static const char* MOD = "HW";

const char* source_name(Source s) {
    switch (s) {
        case Source::None:            return "none";
        case Source::PlatformDefault: return "platform-default";
        case Source::BoardProfile:    return "board-profile";
        case Source::UserConfig:      return "user-config";
    }
    return "?";
}

// user > board > platform default. Records a conflict line when user and
// board disagree (the user value wins, but it is logged).
static void pick(const char* name, const std::optional<int>& user, const std::optional<int>& board,
                 const std::optional<int>& def, Resolved<int>& out, std::string& conflicts) {
    if (user)  { out.value = *user;  out.source = Source::UserConfig; }
    else if (board) { out.value = *board; out.source = Source::BoardProfile; }
    else if (def)   { out.value = *def;   out.source = Source::PlatformDefault; }
    else { out.source = Source::None; }
    if (user && board && *user != *board)
        conflicts += std::string(name) + "=" + std::to_string(*user) + " [user-config] overrides " +
                     std::to_string(*board) + " [board-profile]\n";
}

bool resolve_hardware(const UserHardwareConfig& user, const Registry& reg,
                      const PlatformDefaults& defaults, ResolvedHardware& out, std::string& err) {
    ResolvedHardware r;
    const BoardProfile* board = nullptr;
    if (!user.board_id.empty()) {
        board = reg.board(user.board_id);
        if (!board) { err = "unknown board profile '" + user.board_id + "'"; return false; }
        r.board_id = board->board_id;
        r.board_verified = board->hardware_verified;
    }

    // platform: user > board
    std::string platform_id = !user.platform.empty() ? user.platform : (board ? board->platform : "");
    if (platform_id.empty()) { err = "no platform (set 'platform' or select a board profile)"; return false; }
    const PlatformDescriptor* pd = reg.platform(platform_id);
    if (!pd) { err = "unknown platform '" + platform_id + "'"; return false; }
    r.platform = *pd;
    if (board && !user.platform.empty() && user.platform != board->platform)
        r.conflicts += "platform=" + user.platform + " [user-config] overrides " + board->platform + " [board-profile]\n";

    // sensor: user > board
    std::string sensor_model = !user.sensor.empty() ? user.sensor : (board ? board->sensor : "");
    if (sensor_model.empty()) { err = "no sensor (set 'sensor.model' or select a board profile)"; return false; }
    const SensorDescriptor* sd = reg.sensor(sensor_model);
    if (!sd) { err = "unknown sensor '" + sensor_model + "'"; return false; }
    r.sensor = *sd;
    if (board && !user.sensor.empty() && user.sensor != board->sensor)
        r.conflicts += "sensor=" + user.sensor + " [user-config] overrides " + board->sensor + " [board-profile]\n";

    // A board profile describes one specific board: these pins, this i2c
    // address and these presets belong to *that* sensor on *that* platform.
    // Once the config points at a different platform or a different sensor the
    // profile no longer describes the hardware in front of us, so none of it may
    // be inherited - a reset pin above all, which would then be driven on a board
    // that never wired it that way. Drop the profile and let the fail-closed
    // checks below demand explicit values instead.
    bool board_applies = board != nullptr;
    if (board_applies && !user.platform.empty() && user.platform != board->platform) board_applies = false;
    if (board_applies && !user.sensor.empty()   && user.sensor   != board->sensor)   board_applies = false;
    if (board && !board_applies) {
        r.board_verified = false;
        r.conflicts += "board profile '" + board->board_id + "' no longer describes this hardware "
                       "(platform/sensor overridden) - wiring and presets are ignored, set them explicitly\n";
    }

    // wiring
    SensorWiring bw = board_applies ? board->wiring : SensorWiring{};
    pick("i2c_bus",    user.wiring.i2c_bus,    bw.i2c_bus,    std::nullopt,  r.i2c_bus,    r.conflicts);
    pick("i2c_addr",   user.wiring.i2c_addr,   bw.i2c_addr,   std::nullopt,  r.i2c_addr,   r.conflicts);
    pick("mclk",       user.wiring.mclk,       bw.mclk,       defaults.mclk, r.mclk,       r.conflicts);
    pick("reset_gpio", user.wiring.reset_gpio, bw.reset_gpio, std::nullopt,  r.reset_gpio, r.conflicts);
    pick("pwdn_gpio",  user.wiring.pwdn_gpio,  bw.pwdn_gpio,  std::nullopt,  r.pwdn_gpio,  r.conflicts);

    // fail closed: no bus/address/clock -> unsupported (never invented)
    if (!r.i2c_bus.set())  { err = "sensor i2c_bus unknown (no user value, no board profile) - refusing to guess"; return false; }
    if (!r.i2c_addr.set()) { err = "sensor i2c_addr unknown (no user value, no board profile) - refusing to guess"; return false; }
    if (!r.mclk.set())     { err = "sensor mclk unknown (no user value, no board profile, no platform default)"; return false; }
    // GPIOs: unset -> "none" (-1): the adapter must never touch a pin then.
    if (!r.reset_gpio.set()) { r.reset_gpio.value = -1; }
    if (!r.pwdn_gpio.set())  { r.pwdn_gpio.value  = -1; }

    // mode: user > board default > sensor default; must be a verified sensor mode
    if (user.mode)                    { r.mode.value = *user.mode;            r.mode.source = Source::UserConfig; }
    else if (board_applies && board->default_mode) { r.mode.value = *board->default_mode; r.mode.source = Source::BoardProfile; }
    else if (sd->default_mode())      { r.mode.value = *sd->default_mode();   r.mode.source = Source::PlatformDefault; }
    else { err = "no sensor mode known for '" + sensor_model + "'"; return false; }
    r.allow_unverified_mode = user.allow_unverified_mode;
    r.presets = board_applies ? board->presets : BoardPresets{};
    if (!sd->has_mode(r.mode.value)) {
        std::string what = "mode " + std::to_string(r.mode.value.width) + "x" + std::to_string(r.mode.value.height) + "@" +
              std::to_string(r.mode.value.fps) + " is not a verified mode of sensor '" + sensor_model + "'";
        if (!user.allow_unverified_mode) { err = what; return false; }
        r.mode_verified = false;
        r.conflicts += what + " (allowed by sensor.allow_unverified_mode)\n";
    }
    if (user.mode && board_applies && board->default_mode && !(*user.mode == *board->default_mode))
        r.conflicts += "mode [user-config] overrides board default\n";

    out = r;
    return true;
}

void log_resolved_hardware(const ResolvedHardware& hw) {
    LOGI(MOD, "Platform: %s %s (%s)", hw.platform.vendor.c_str(), hw.platform.model.c_str(), hw.platform.family.c_str());
    LOGI(MOD, "Board: %s%s", hw.board_id.empty() ? "(none - explicit user wiring)" : hw.board_id.c_str(),
         hw.board_verified ? " [hardware verified]" : "");
    LOGI(MOD, "Sensor: %s (%s, native %dx%d)", hw.sensor.model.c_str(),
         hw.sensor.interface == SensorInterface::MipiCsi ? "mipi-csi" : hw.sensor.interface == SensorInterface::Dvp ? "dvp" : "unknown-if",
         hw.sensor.native_width, hw.sensor.native_height);
    LOGI(MOD, "Mode: %dx%d@%d", hw.mode.value.width, hw.mode.value.height, hw.mode.value.fps);
    LOGI(MOD, "I2C: bus=%d addr=0x%02x", hw.i2c_bus.value, hw.i2c_addr.value);
    LOGI(MOD, "MCLK: %d", hw.mclk.value);
    if (hw.reset_gpio.value >= 0) LOGI(MOD, "Reset GPIO: %d", hw.reset_gpio.value); else LOGI(MOD, "Reset GPIO: none (not touched)");
    if (hw.pwdn_gpio.value  >= 0) LOGI(MOD, "PWDN GPIO: %d",  hw.pwdn_gpio.value);  else LOGI(MOD, "PWDN GPIO: none (not touched)");
    LOGD(MOD, "i2c_bus=%d [%s] i2c_addr=0x%02x [%s] mclk=%d [%s] reset_gpio=%d [%s] pwdn_gpio=%d [%s] mode=%dx%d@%d [%s]",
         hw.i2c_bus.value, source_name(hw.i2c_bus.source), hw.i2c_addr.value, source_name(hw.i2c_addr.source),
         hw.mclk.value, source_name(hw.mclk.source), hw.reset_gpio.value, source_name(hw.reset_gpio.source),
         hw.pwdn_gpio.value, source_name(hw.pwdn_gpio.source), hw.mode.value.width, hw.mode.value.height,
         hw.mode.value.fps, source_name(hw.mode.source));
    if (!hw.conflicts.empty()) {
        size_t p = 0;
        while (p < hw.conflicts.size()) {
            size_t nl = hw.conflicts.find('\n', p);
            std::string line = hw.conflicts.substr(p, nl == std::string::npos ? std::string::npos : nl - p);
            if (!line.empty()) LOGW(MOD, "conflict: %s", line.c_str());
            p = (nl == std::string::npos) ? hw.conflicts.size() : nl + 1;
        }
    }
}

}} // namespace machino::hw
