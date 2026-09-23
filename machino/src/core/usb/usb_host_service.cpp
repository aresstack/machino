#include "core/usb/usb_host_service.hpp"

#include <algorithm>

namespace machino {

const char* usb_power_mode_name(UsbPowerMode m)
{
    switch (m) {
        case UsbPowerMode::BoardDefault: return "board-default";
        case UsbPowerMode::Gpio:         return "gpio";
        case UsbPowerMode::AlwaysOn:     return "always-on";
        case UsbPowerMode::None:         return "none";
    }
    return "board-default";
}

bool usb_power_mode_parse(const std::string& s, UsbPowerMode& out)
{
    if (s == "board-default") { out = UsbPowerMode::BoardDefault; return true; }
    if (s == "gpio")          { out = UsbPowerMode::Gpio;         return true; }
    if (s == "always-on")     { out = UsbPowerMode::AlwaysOn;     return true; }
    if (s == "none")          { out = UsbPowerMode::None;         return true; }
    return false;
}

namespace usb {

namespace {

bool pin_is_listed(const UsbPowerCapability& p, const std::string& pin)
{
    if (!pin.empty() && pin == p.default_pin) return true;
    return std::find(p.allowed_pins.begin(), p.allowed_pins.end(), pin) != p.allowed_pins.end();
}

} // namespace

UsbHostService::UsbHostService(IUsbHostBackend& backend) : backend_(backend) {}

bool UsbHostService::resolve(const UsbConfig& cfg, const UsbCapabilities& caps,
                             UsbResolved& out, std::string& err)
{
    out = UsbResolved{};

    if (!caps.host_supported) {
        // Disabling something the board does not have is not an error; asking
        // for it is.
        if (!cfg.enabled) { out.mode = UsbPowerMode::None; return true; }
        err = "this board has no USB host";
        return false;
    }

    UsbPowerMode mode = cfg.mode;
    if (mode == UsbPowerMode::BoardDefault) {
        mode = caps.power.switchable ? UsbPowerMode::Gpio : UsbPowerMode::AlwaysOn;
    }

    switch (mode) {
        case UsbPowerMode::None:
            out.mode = UsbPowerMode::None;
            out.drives_power = false;
            return true;

        case UsbPowerMode::AlwaysOn:
            // Only honest when the board really cannot switch. Claiming it on a
            // switchable board would leave the port dark and look like a bug.
            if (caps.power.switchable && cfg.mode == UsbPowerMode::AlwaysOn) {
                err = "this board switches port power; always-on would leave it off";
                return false;
            }
            out.mode = UsbPowerMode::AlwaysOn;
            out.drives_power = false;
            return true;

        case UsbPowerMode::Gpio: {
            if (!caps.power.switchable) {
                err = "this board cannot switch port power from software";
                return false;
            }
            std::string pin = cfg.pin.empty() ? caps.power.default_pin : cfg.pin;
            if (pin.empty()) {
                err = "no power pin configured and the board declares no default";
                return false;
            }
            if (!cfg.expert && !pin_is_listed(caps.power, pin)) {
                err = "pin " + pin + " is not one the board wires for USB power "
                      "(set expert to override; the rest of the GPIO space belongs "
                      "to the sensor, the PHY and the flash)";
                return false;
            }
            out.mode = UsbPowerMode::Gpio;
            out.pin = pin;
            out.active_high = cfg.pin.empty() ? caps.power.default_active_high : cfg.active_high;
            out.drives_power = true;
            return true;
        }

        case UsbPowerMode::BoardDefault:
            break;   // resolved above
    }

    err = "unsupported power mode";
    return false;
}

Result UsbHostService::apply(const UsbConfig& cfg, std::string& err)
{
    const UsbCapabilities caps = backend_.capabilities();

    UsbResolved r;
    if (!resolve(cfg, caps, r, err)) return Result::error();

    // The backend is told about EVERY mode, not just Gpio. Skipping the other
    // modes would mean a switch from gpio to none never reaches it, so it
    // would keep the pin asserted and the port would stay powered while the
    // mode says otherwise. Backends treat AlwaysOn/None as "stop driving".
    Result rc = backend_.set_power(r.mode, r.pin, r.active_high, cfg.enabled);

    if (!rc.is_ok()) {
        err = "the platform refused to apply USB power";
        return rc;
    }

    std::lock_guard<std::mutex> g(m_);
    cfg_ = cfg;
    resolved_ = r;
    return Result::ok();
}

Result UsbHostService::apply_at_boot(std::string& err)
{
    UsbConfig c;
    {
        std::lock_guard<std::mutex> g(m_);
        c = cfg_;
    }
    if (!c.enable_at_boot) {
        err = "enable_at_boot is off";
        return Result::ok();     // not a failure: the user asked for this
    }
    return apply(c, err);
}

UsbConfig UsbHostService::config() const
{
    std::lock_guard<std::mutex> g(m_);
    return cfg_;
}

UsbCapabilities UsbHostService::capabilities() const
{
    return backend_.capabilities();
}

UsbStatus UsbHostService::status() const
{
    UsbStatus s;
    s.caps = backend_.capabilities();
    s.host_active = backend_.host_active();
    s.devices = backend_.devices();
    {
        std::lock_guard<std::mutex> g(m_);
        s.enabled = cfg_.enabled;
        s.resolved = resolved_;
    }
    bool on = false;
    if (backend_.power_state(on)) { s.power_known = true; s.power_on = on; }
    return s;
}

}} // namespace machino::usb
