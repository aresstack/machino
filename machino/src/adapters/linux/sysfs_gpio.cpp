#include "adapters/linux/sysfs_gpio.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace machino { namespace linuxsys {

namespace {

bool read_file(const std::string& path, std::string& out)
{
    std::ifstream f(path.c_str());
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) out.pop_back();
    return true;
}

bool write_file(const std::string& path, const std::string& text)
{
    std::ofstream f(path.c_str());
    if (!f) return false;
    f << text;
    f.flush();
    return f.good();
}

bool exists(const std::string& path)
{
    struct stat st;
    return ::stat(path.c_str(), &st) == 0;
}

} // namespace

SysfsGpio::SysfsGpio(std::string root) : root_(std::move(root)) {}

SysfsGpio::~SysfsGpio()
{
    // Give back only what we took. A pin some driver owns was never ours.
    std::lock_guard<std::mutex> g(m_);
    for (int n : exported_) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%d", n);
        write_file(root_ + "/unexport", buf);
    }
    exported_.clear();
}

bool SysfsGpio::available() const
{
    return exists(root_ + "/export");
}

bool SysfsGpio::resolve(const std::string& name, int& number_out) const
{
    // P<letter><digits>, nothing else. A malformed name must not silently
    // become pin 0 -- that is a real pin.
    if (name.size() < 3 || (name[0] != 'P' && name[0] != 'p')) return false;
    char letter = name[1];
    if (letter >= 'a' && letter <= 'z') letter = (char)(letter - 'a' + 'A');
    if (letter < 'A' || letter > 'F') return false;

    int idx = 0;
    size_t i = 2;
    if (i >= name.size()) return false;
    for (; i < name.size(); ++i) {
        if (name[i] < '0' || name[i] > '9') return false;
        idx = idx * 10 + (name[i] - '0');
        if (idx > 31) return false;
    }
    number_out = (letter - 'A') * 32 + idx;
    return true;
}

std::string SysfsGpio::dir_for(int n) const
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "/gpio%d", n);
    return root_ + buf;
}

bool SysfsGpio::holder_of(const std::string& name, GpioPinInfo& info) const
{
    int n = -1;
    if (!resolve(name, n)) return false;

    info = GpioPinInfo{};
    info.name = name;
    info.number = n;

    // debugfs names the claiming driver; it is the only place that does.
    // Absent debugfs we simply cannot say, and we must not guess "free".
    std::string dbg;
    if (read_file("/sys/kernel/debug/gpio", dbg)) {
        char needle[24];
        std::snprintf(needle, sizeof(needle), "gpio-%d ", n);
        size_t p = dbg.find(needle);
        if (p != std::string::npos) {
            size_t eol = dbg.find('\n', p);
            std::string line = dbg.substr(p, eol == std::string::npos ? std::string::npos : eol - p);
            size_t bar = line.find('|');
            if (bar != std::string::npos) {
                size_t close = line.find(')', bar);
                if (close != std::string::npos) {
                    std::string h = line.substr(bar + 1, close - bar - 1);
                    while (!h.empty() && h.back() == ' ') h.pop_back();
                    while (!h.empty() && h.front() == ' ') h.erase(h.begin());
                    if (!h.empty()) { info.claimed = true; info.holder = h; }
                }
            }
            if (line.find(" out ") != std::string::npos) info.direction = "out";
            else if (line.find(" in ") != std::string::npos) info.direction = "in";
        }
    }

    std::string d;
    if (read_file(dir_for(n) + "/direction", d) && !d.empty()) info.direction = d;
    return info.claimed;
}

Result SysfsGpio::configure_output(const std::string& name, bool initial_level)
{
    int n = -1;
    if (!resolve(name, n)) return Result::unsupported();
    if (!available()) return Result::unsupported();

    GpioPinInfo info;
    if (holder_of(name, info)) {
        // Someone else's pin. Refuse; the alternative is taking the Ethernet
        // PHY reset away from its driver.
        return Result::busy();
    }

    std::lock_guard<std::mutex> g(m_);
    const std::string d = dir_for(n);
    if (!exists(d)) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%d", n);
        if (!write_file(root_ + "/export", buf)) return Result::error(errno);
        exported_.insert(n);
    }
    // direction=high/low sets both at once and avoids a glitch through the
    // default level on the way to the wanted one.
    if (!write_file(d + "/direction", initial_level ? "high" : "low")) return Result::error(errno);
    return Result::ok();
}

Result SysfsGpio::write(const std::string& name, bool level)
{
    int n = -1;
    if (!resolve(name, n)) return Result::unsupported();
    std::lock_guard<std::mutex> g(m_);
    if (!write_file(dir_for(n) + "/value", level ? "1" : "0")) return Result::error(errno);
    return Result::ok();
}

Result SysfsGpio::read(const std::string& name, bool& level_out) const
{
    int n = -1;
    if (!resolve(name, n)) return Result::unsupported();
    std::string v;
    if (!read_file(dir_for(n) + "/value", v)) return Result::error(errno);
    level_out = (!v.empty() && v[0] == '1');
    return Result::ok();
}

void SysfsGpio::release(const std::string& name)
{
    int n = -1;
    if (!resolve(name, n)) return;
    std::lock_guard<std::mutex> g(m_);
    auto it = exported_.find(n);
    if (it == exported_.end()) return;      // not ours
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d", n);
    write_file(root_ + "/unexport", buf);
    exported_.erase(it);
}

}} // namespace machino::linuxsys
