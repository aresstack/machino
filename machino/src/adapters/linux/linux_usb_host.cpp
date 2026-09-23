#include "adapters/linux/linux_usb_host.hpp"

#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <sys/stat.h>

namespace machino { namespace linuxsys {

namespace {

bool slurp(const std::string& path, std::string& out)
{
    std::ifstream f(path.c_str());
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) out.pop_back();
    return true;
}

std::string slurp_or(const std::string& path, const char* dflt = "")
{
    std::string s;
    return slurp(path, s) ? s : std::string(dflt);
}

bool is_dir(const std::string& p)
{
    struct stat st;
    return ::stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// "1-1" is a device, "1-1:1.0" an interface, "usb1" a root hub. We report
// devices only; the root hub is not something a user plugged in.
bool is_device_dir(const std::string& name)
{
    if (name.find(':') != std::string::npos) return false;
    if (name.compare(0, 3, "usb") == 0) return false;
    return name.find('-') != std::string::npos;
}

} // namespace

LinuxUsbHostBackend::LinuxUsbHostBackend(IGpioController& gpio,
                                         UsbPowerCapability board_power,
                                         std::string sysfs_usb,
                                         std::string controller_name)
    : gpio_(gpio), board_power_(std::move(board_power)),
      sysfs_usb_(std::move(sysfs_usb)), controller_(std::move(controller_name))
{
    // A board profile may claim a switchable rail while the platform has no
    // usable GPIO. Downgrade honestly instead of failing later.
    if (board_power_.switchable && !gpio_.available()) board_power_.switchable = false;
}

UsbCapabilities LinuxUsbHostBackend::capabilities() const
{
    UsbCapabilities c;
    c.host_supported = is_dir(sysfs_usb_);
    c.controller = controller_;
    if (c.host_supported) {
        // Root hub speed is the honest ceiling for this port.
        std::string s = slurp_or(sysfs_usb_ + "/usb1/speed");
        if (s == "480") c.max_speed = "high";
        else if (s == "12") c.max_speed = "full";
        else if (!s.empty()) c.max_speed = s;
    }
    c.power = board_power_;
    return c;
}

bool LinuxUsbHostBackend::host_active() const
{
    // A root hub exists exactly when the controller came up in host mode.
    return is_dir(sysfs_usb_ + "/usb1");
}

Result LinuxUsbHostBackend::set_power(UsbPowerMode mode, const std::string& pin,
                                      bool active_high, bool on)
{
    if (mode != UsbPowerMode::Gpio) {
        // Not an error: AlwaysOn and None are legitimate. But if we were
        // driving a pin, stop honestly -- leaving it asserted would keep the
        // port powered while the mode says "none", and the status page would
        // then disagree with the multimeter.
        std::string pin_to_drop;
        bool active_high = true;
        {
            std::lock_guard<std::mutex> g(m_);
            pin_to_drop = driven_pin_;
            active_high = driven_active_high_;
            driven_pin_.clear();
            driven_known_ = false;
            driven_on_ = false;
        }
        if (!pin_to_drop.empty()) {
            gpio_.write(pin_to_drop, active_high ? false : true);   // de-assert
            gpio_.release(pin_to_drop);
        }
        return Result::ok();
    }
    if (!board_power_.switchable) return Result::unsupported();
    if (pin.empty()) return Result::error();

    const bool level = active_high ? on : !on;

    std::lock_guard<std::mutex> g(m_);
    if (driven_pin_ != pin) {
        if (!driven_pin_.empty()) gpio_.release(driven_pin_);
        Result rc = gpio_.configure_output(pin, level);
        if (!rc.is_ok()) return rc;
        driven_pin_ = pin;
    } else {
        Result rc = gpio_.write(pin, level);
        if (!rc.is_ok()) return rc;
    }
    driven_active_high_ = active_high;
    driven_on_ = on;
    driven_known_ = true;
    return Result::ok();
}

bool LinuxUsbHostBackend::power_state(bool& on_out) const
{
    std::string pin;
    bool active_high = true;
    {
        std::lock_guard<std::mutex> g(m_);
        if (!driven_known_ || driven_pin_.empty()) {
            if (!board_power_.switchable) { on_out = true; return true; }  // hard-wired rail
            return false;
        }
        pin = driven_pin_;
        active_high = driven_active_high_;
    }
    bool level = false;
    if (!gpio_.read(pin, level).is_ok()) return false;
    on_out = active_high ? level : !level;
    return true;
}

std::vector<UsbDevice> LinuxUsbHostBackend::devices() const
{
    std::vector<UsbDevice> out;
    DIR* d = ::opendir(sysfs_usb_.c_str());
    if (!d) return out;

    struct dirent* e;
    while ((e = ::readdir(d)) != nullptr) {
        const std::string name = e->d_name;
        if (!is_device_dir(name)) continue;

        const std::string base = sysfs_usb_ + "/" + name;
        std::string vid;
        if (!slurp(base + "/idVendor", vid)) continue;   // not a real device node

        UsbDevice dev;
        dev.path = name;
        dev.vid = vid;
        dev.pid = slurp_or(base + "/idProduct");
        dev.manufacturer = slurp_or(base + "/manufacturer");
        dev.product = slurp_or(base + "/product");
        dev.serial = slurp_or(base + "/serial");
        dev.speed = slurp_or(base + "/speed");
        const std::string mp = slurp_or(base + "/bMaxPower");   // "500mA"
        dev.max_power_ma = std::atoi(mp.c_str());

        // Interfaces are siblings named "<dev>:<config>.<n>".
        DIR* d2 = ::opendir(sysfs_usb_.c_str());
        if (d2) {
            struct dirent* e2;
            const std::string prefix = name + ":";
            while ((e2 = ::readdir(d2)) != nullptr) {
                const std::string n2 = e2->d_name;
                if (n2.compare(0, prefix.size(), prefix) != 0) continue;
                const std::string ib = sysfs_usb_ + "/" + n2;
                UsbInterface i;
                i.cls = slurp_or(ib + "/bInterfaceClass");
                i.subclass = slurp_or(ib + "/bInterfaceSubClass");
                i.protocol = slurp_or(ib + "/bInterfaceProtocol");
                char buf[256];
                ssize_t n = ::readlink((ib + "/driver").c_str(), buf, sizeof(buf) - 1);
                if (n > 0) {
                    buf[n] = 0;
                    const char* slash = std::strrchr(buf, '/');
                    i.driver = slash ? slash + 1 : buf;
                }
                dev.interfaces.push_back(i);
            }
            ::closedir(d2);
        }
        out.push_back(dev);
    }
    ::closedir(d);
    return out;
}

}} // namespace machino::linuxsys
