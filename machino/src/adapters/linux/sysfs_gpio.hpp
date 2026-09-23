// Linux GPIO through /sys/class/gpio.
//
// This class knows sysfs and nothing else. How a logical pin name becomes a
// number is NOT its business -- an earlier version hardcoded the Ingenic
// "P<letter><index> = bank*32 + index" convention here, which quietly made a
// supposedly generic Linux backend SoC-specific. The board profile now picks
// an hw::IPinResolver and hands it in.
//
// Why sysfs and not the character device: this kernel is 4.4, where
// /dev/gpiochip* has no line-request ABI worth using, and sysfs is exactly
// what the vendor's own firmware uses. It also means a human can reproduce
// every write with echo, which matters when the next person debugs this with
// a multimeter.
#pragma once
#include "core/hw/pin_resolver.hpp"
#include "ports/igpio.hpp"
#include <mutex>
#include <set>
#include <string>

namespace machino { namespace linuxsys {

class SysfsGpio : public IGpioController {
public:
    // `root` is injectable so a test can point it at a temp tree.
    //
    // unexport_on_close defaults to FALSE on purpose. Unexporting returns the
    // pin to its reset state, and on this board that means the USB load switch
    // turns off: restarting the media daemon would cut power to the WiFi
    // dongle the camera is reachable through. A pin left exported costs
    // nothing; a network interface that disappears on every restart costs a
    // site visit.
    // `resolver` is borrowed and must outlive this object.
    explicit SysfsGpio(const hw::IPinResolver& resolver,
                       std::string root = "/sys/class/gpio",
                       bool unexport_on_close = false);
    ~SysfsGpio() override;

    bool   available() const override;
    bool   resolve(const std::string& name, int& number_out) const override;
    bool   holder_of(const std::string& name, GpioPinInfo& info) const override;
    Result configure_output(const std::string& name, bool initial_level) override;
    Result write(const std::string& name, bool level) override;
    Result read(const std::string& name, bool& level_out) const override;
    void   release(const std::string& name) override;

private:
    std::string dir_for(int n) const;

    const hw::IPinResolver& resolver_;
    std::string     root_;
    bool            unexport_on_close_;
    mutable std::mutex m_;
    std::set<int>   exported_;   // only pins WE exported get unexported again
};

}} // namespace machino::linuxsys
