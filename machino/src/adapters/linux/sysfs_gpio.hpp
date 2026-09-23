// Linux GPIO through /sys/class/gpio, addressed by logical pin name.
//
// Naming convention: "P<letter><index>" -- port letter A..F, index 0..31, so
// the global number is (letter - 'A') * 32 + index. That is the numbering the
// Ingenic pinctrl uses (PB18 -> 50) and it is shared by several SoC families;
// a platform that numbers differently supplies its own IGpioController rather
// than teaching this one exceptions.
//
// Why sysfs and not the character device: this kernel is 4.4, where
// /dev/gpiochip* has no line-request ABI worth using, and sysfs is exactly
// what the vendor's own firmware uses. It also means a human can reproduce
// every write with echo, which matters when the next person debugs this with
// a multimeter.
#pragma once
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
    explicit SysfsGpio(std::string root = "/sys/class/gpio", bool unexport_on_close = false);
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

    std::string     root_;
    bool            unexport_on_close_;
    mutable std::mutex m_;
    std::set<int>   exported_;   // only pins WE exported get unexported again
};

}} // namespace machino::linuxsys
