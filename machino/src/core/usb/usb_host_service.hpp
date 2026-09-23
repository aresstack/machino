// Application policy for the USB host: what the user asked for, what the board
// can actually do, and the arbitration between the two.
//
// This service is platform-neutral by construction. It knows modes and logical
// pin names; it never knows a GPIO number, a register or a vendor. Everything
// hardware-shaped arrives through IUsbHostBackend.
//
// It is deliberately NOT part of the media lifecycle. A USB device appearing,
// failing or vanishing must not be able to touch IMP, the pipeline or WebRTC --
// the camera keeps streaming whatever the port does. The only coupling is that
// both live in the same process today; if USB later grows storage, modem and
// WiFi management, this service is the piece that gets lifted out.
#pragma once
#include "ports/iusb_host.hpp"
#include <mutex>
#include <string>
#include <vector>

namespace machino { namespace usb {

struct UsbConfig {
    bool         enabled = false;
    UsbPowerMode mode = UsbPowerMode::BoardDefault;
    std::string  pin;                 // empty = use the board default
    bool         active_high = true;
    bool         enable_at_boot = true;
    // Escape hatch for a board we do not have a profile for. Without it only
    // pins the backend lists as wired-for-this-purpose are accepted, because
    // the rest of the GPIO space is the sensor reset, the PHY reset and the
    // flash -- we nearly drove one of those by accident while finding PB18.
    bool         expert = false;
};

// What the service resolved the request into, after consulting the backend.
struct UsbResolved {
    UsbPowerMode mode = UsbPowerMode::None;   // BoardDefault is resolved away
    std::string  pin;                         // concrete, after defaulting
    bool         active_high = true;
    bool         drives_power = false;        // false for AlwaysOn/None
};

struct UsbStatus {
    bool                   enabled = false;
    bool                   host_active = false;
    bool                   power_known = false;
    bool                   power_on = false;
    UsbResolved            resolved;
    UsbCapabilities        caps;
    std::vector<UsbDevice> devices;
};

class UsbHostService {
public:
    explicit UsbHostService(IUsbHostBackend& backend);

    UsbHostService(const UsbHostService&) = delete;
    UsbHostService& operator=(const UsbHostService&) = delete;

    // Pure decision function, exposed so it can be tested without hardware and
    // so the API can reject a bad PATCH before anything is applied.
    // Returns false and fills `err` with a stable machine-ish reason.
    static bool resolve(const UsbConfig& cfg, const UsbCapabilities& caps,
                        UsbResolved& out, std::string& err);

    // Validate, then apply. On failure nothing is changed and `err` explains.
    Result apply(const UsbConfig& cfg, std::string& err);

    // Apply the stored configuration; used once at start-up when
    // enable_at_boot is set. Separate from apply() so the boot path cannot
    // silently turn a port on that the user disabled.
    //
    // `applied` distinguishes "did nothing because the user said not to" from
    // "did it". Returning Ok while also filling `err` -- as an earlier version
    // did -- makes a caller that logs any non-empty err report a success as a
    // problem.
    Result apply_at_boot(std::string& err, bool* applied = nullptr);

    UsbConfig       config() const;
    UsbCapabilities capabilities() const;
    UsbStatus       status() const;

private:
    IUsbHostBackend& backend_;
    mutable std::mutex m_;
    UsbConfig   cfg_;
    UsbResolved resolved_;
};

}} // namespace machino::usb
