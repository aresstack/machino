// Port: a USB host controller and its power path, platform-neutral.
//
// The core asks "is there a host, can its port power be switched, what is
// plugged in". It never learns which register, which GPIO or which vendor
// quirk produces the answer. The first real backend (Ingenic T40 on a Vanhua
// board) switches a load switch through one GPIO; an ESP32-P4 backend would
// call a board callback; a board with a hard-wired 5 V rail reports
// AlwaysOn and refuses to pretend it can switch anything.
//
// Deliberately NOT in this port: modems, WiFi chips, mass storage. Those are
// device-class integrations that sit ON TOP of an enumerated device. Mixing
// them in here is how a generic abstraction turns into a pile of special
// cases for whatever hardware happened to arrive first.
#pragma once
#include "core/result.hpp"
#include <string>
#include <vector>

namespace machino {

enum class UsbPowerMode : int {
    BoardDefault = 0,  // whatever the backend says the board does
    Gpio,              // switch a named pin
    AlwaysOn,          // rail is not switchable; it is simply on
    None,              // do not drive power at all
};

const char* usb_power_mode_name(UsbPowerMode m);
bool        usb_power_mode_parse(const std::string& s, UsbPowerMode& out);

struct UsbPowerCapability {
    bool        switchable = false;      // can this board switch port power?
    std::string default_pin;             // logical name, e.g. "PB18"
    bool        default_active_high = true;
    int         voltage_mv = 0;          // 0 = unknown. 3300 on the Vanhua board
    // Pins the board vendor actually wired for this purpose. Anything outside
    // needs the expert flag, because the rest of the GPIO space belongs to the
    // sensor, the PHY and the flash.
    std::vector<std::string> allowed_pins;
};

struct UsbCapabilities {
    bool        host_supported = false;
    std::string controller;     // "dwc2", "" when unknown
    std::string max_speed;      // "high", "full", ""
    UsbPowerCapability power;
};

struct UsbInterface {
    std::string cls, subclass, protocol;   // hex strings as the kernel reports them
    std::string driver;                    // bound driver, "" when none
};

struct UsbDevice {
    std::string path;          // backend-local identity, e.g. "1-1"
    std::string vid, pid;      // lowercase hex, no prefix
    std::string manufacturer, product, serial;
    std::string speed;         // "480", "12", ...
    int         max_power_ma = 0;
    std::vector<UsbInterface> interfaces;
};

class IUsbHostBackend {
public:
    virtual ~IUsbHostBackend() = default;

    virtual UsbCapabilities capabilities() const = 0;

    // Whether the controller is actually in host mode right now.
    virtual bool host_active() const = 0;

    // Apply the power decision the service made. `pin` is empty for modes that
    // do not need one. Must be idempotent and must fail rather than half-apply.
    virtual Result set_power(UsbPowerMode mode, const std::string& pin,
                             bool active_high, bool on) = 0;

    // Best-effort readback. False when the platform cannot tell.
    virtual bool power_state(bool& on_out) const = 0;

    virtual std::vector<UsbDevice> devices() const = 0;
};

// A backend for platforms with no USB host at all. Everything reports
// unsupported, nothing pretends. Used on hosts, in tests, and on boards whose
// profile says there is no port.
class NullUsbHostBackend : public IUsbHostBackend {
public:
    UsbCapabilities capabilities() const override { return UsbCapabilities{}; }
    bool host_active() const override { return false; }
    Result set_power(UsbPowerMode, const std::string&, bool, bool) override { return Result::unsupported(); }
    bool power_state(bool&) const override { return false; }
    std::vector<UsbDevice> devices() const override { return {}; }
};

} // namespace machino
