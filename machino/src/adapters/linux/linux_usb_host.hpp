// A USB host backend for Linux: enumeration from sysfs, power through an
// injected IGpioController.
//
// It carries no board knowledge. What the board can do -- whether port power
// is switchable, on which pin, at what polarity and voltage -- is handed in at
// construction from the resolved board profile. That is the difference between
// "the T40 backend" and "the Linux backend that a T40 profile configures": the
// second one also serves the next camera.
#pragma once
#include "ports/igpio.hpp"
#include "ports/iusb_host.hpp"
#include <mutex>
#include <string>

namespace machino { namespace linuxsys {

class LinuxUsbHostBackend : public IUsbHostBackend {
public:
    // `gpio` may be a controller that reports unavailable; then power is
    // simply not switchable and the service will say so instead of failing
    // at the first write.
    LinuxUsbHostBackend(IGpioController& gpio,
                        UsbPowerCapability board_power,
                        std::string sysfs_usb = "/sys/bus/usb/devices",
                        std::string controller_name = "");

    UsbCapabilities capabilities() const override;
    bool            host_active() const override;
    Result          set_power(UsbPowerMode mode, const std::string& pin,
                              bool active_high, bool on) override;
    bool            power_state(bool& on_out) const override;
    std::vector<UsbDevice> devices() const override;

private:
    IGpioController&   gpio_;
    UsbPowerCapability board_power_;
    std::string        sysfs_usb_;
    std::string        controller_;

    std::string        first_root_hub() const;

    mutable std::mutex m_;
    std::string        driven_pin_;      // the pin we currently hold, if any
    bool               driven_active_high_ = true;
    bool               driven_on_ = false;
    bool               driven_known_ = false;
};

}} // namespace machino::linuxsys
