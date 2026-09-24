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

// What the single USB port is used for.
//
// Not a bitmask and not two flags: the port carries one device. An enum makes
// "WiFi and cellular at the same time" unrepresentable rather than merely
// discouraged.
enum class UsbFunction : int { Off = 0, Wifi, Cellular };

const char* usb_function_name(UsbFunction f);

// Anything that is not one of the three names is REFUSED, not defaulted.
// A hand-edited machino.conf saying usb.mode=wlan must not silently become
// Off -- that reads as "the setting did not take" and sends the owner looking
// in the wrong place. The caller reports the bad value and keeps what it had.
bool usb_function_parse(const std::string& s, UsbFunction& out);

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

    // What the USB port is used for. ONE setting, because there is one port.
    //
    // This replaced two independent checkboxes (usb.wifi.enabled and
    // cellular.enabled), and the reason is not tidiness. Both of them switched
    // on a driver stack for the same physical connector, so "both true" was a
    // state the configuration could express and the hardware could not. Which
    // of the two won would then have been decided by init script ordering --
    // a fact nobody reads before ticking a box.
    //
    // It is also a resource decision. The aic8800 driver is the component that
    // pushed this camera into OOM twice during bring-up: it asks for 847
    // order-3 blocks where about 20 are free. A camera carrying a 4G modem on
    // that port must not pay any of that, and Off must pay neither.
    //
    // Off means off at the source: the boot helper loads no module, raises no
    // rail and starts no daemon. It is read before machino exists, straight
    // out of machino.conf, which is why a change takes effect at the NEXT BOOT
    // rather than at once. "Restart required" is the honest thing to say;
    // pretending it is live would be the lie, and swapping kernel modules
    // under a running IMP pipeline is how this camera hardlocks.
    UsbFunction  function = UsbFunction::Off;
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

    // The stored function, and the one the boot helper actually acted on.
    //
    // They differ exactly between a change and the reboot that makes it real,
    // and that gap is the whole reason the UI has something to say. Deriving
    // "reboot required" from anything else -- a dirty flag, a timestamp --
    // would survive the reboot and keep nagging.
    UsbFunction            function = UsbFunction::Off;
    UsbFunction            boot_function = UsbFunction::Off;
    bool                   reboot_required = false;
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

    // Adopt a stored configuration WITHOUT touching the port.
    //
    // Start-up reads the file and then lets apply_at_boot() decide whether the
    // port actually comes up -- which is the entire point of enable_at_boot.
    // Going through apply() to load it would power the port on regardless of
    // what the user asked for, and there would be no way to say "remember this
    // setting but leave the port alone".
    //
    // Still validated: a hand-edited file that names a pin the board does not
    // wire is rejected here rather than at the first apply.
    bool load_config(const UsbConfig& cfg, std::string& err);

    // Apply the stored configuration; used once at start-up when
    // enable_at_boot is set. Separate from apply() so the boot path cannot
    // silently turn a port on that the user disabled.
    //
    // `applied` distinguishes "did nothing because the user said not to" from
    // "did it". Returning Ok while also filling `err` -- as an earlier version
    // did -- makes a caller that logs any non-empty err report a success as a
    // problem.
    Result apply_at_boot(std::string& err, bool* applied = nullptr);

    // What the boot helper actually started, read once at start-up from the
    // marker it leaves behind. Unknown means "we could not find out", and that
    // is reported as Off rather than as agreement -- claiming the running mode
    // matches the stored one when nobody knows would hide a needed reboot.
    void set_boot_function(UsbFunction f);

    UsbConfig       config() const;
    UsbCapabilities capabilities() const;
    UsbStatus       status() const;

private:
    IUsbHostBackend& backend_;
    mutable std::mutex m_;
    UsbConfig   cfg_;
    UsbResolved resolved_;
    UsbFunction boot_function_ = UsbFunction::Off;
};

}} // namespace machino::usb
