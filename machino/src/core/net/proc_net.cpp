#include "core/net/proc_net.hpp"

#include <cstdio>
#include <cstdlib>

namespace machino { namespace net {

namespace {

std::vector<std::string> split_lines(const std::string& text)
{
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos <= text.size()) {
        const size_t nl = text.find('\n', pos);
        std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        while (!line.empty() && (line.back() == '\r')) line.pop_back();
        out.push_back(line);
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return out;
}

// Whitespace-separated fields. /proc pads with spaces and tabs in different
// places on different kernels, so neither may be assumed.
std::vector<std::string> fields(const std::string& line)
{
    std::vector<std::string> out;
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
        const size_t start = i;
        while (i < line.size() && line[i] != ' ' && line[i] != '\t') ++i;
        if (i > start) out.push_back(line.substr(start, i - start));
    }
    return out;
}

bool hex32(const std::string& s, uint32_t& out)
{
    if (s.empty() || s.size() > 8) return false;
    uint32_t v = 0;
    for (char c : s) {
        int d;
        if      (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return false;
        v = (v << 4) | (uint32_t)d;
    }
    out = v;
    return true;
}

bool dec_int(const std::string& s, long& out)
{
    if (s.empty()) return false;
    size_t i = (s[0] == '-') ? 1 : 0;
    if (i >= s.size()) return false;
    for (; i < s.size(); ++i) if (s[i] < '0' || s[i] > '9') return false;
    out = std::strtol(s.c_str(), nullptr, 10);
    return true;
}

// The /proc/net/route columns are the address in HOST byte order of the
// network-order value, so on a little-endian machine the first octet is the
// low byte. Both this SoC and every host that runs the tests are little
// endian, and the file format does not change with the reader.
std::string le_hex_to_dotted(uint32_t v)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
                  (unsigned)(v & 0xff), (unsigned)((v >> 8) & 0xff),
                  (unsigned)((v >> 16) & 0xff), (unsigned)((v >> 24) & 0xff));
    return buf;
}

} // namespace

std::vector<DefaultRoute> parse_proc_net_route(const std::string& text)
{
    std::vector<DefaultRoute> out;
    const std::vector<std::string> lines = split_lines(text);
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::vector<std::string> f = fields(lines[i]);
        // Iface Destination Gateway Flags RefCnt Use Metric Mask ...
        if (f.size() < 8) continue;
        if (f[0] == "Iface") continue;                 // header

        uint32_t dest = 0, gw = 0, mask = 0;
        if (!hex32(f[1], dest) || !hex32(f[2], gw) || !hex32(f[7], mask)) continue;
        if (dest != 0 || mask != 0) continue;          // not a default route

        long metric = 0;
        if (!dec_int(f[6], metric)) continue;

        DefaultRoute r;
        r.ifname = f[0];
        r.gateway = (gw == 0) ? std::string() : le_hex_to_dotted(gw);
        r.metric = (int)metric;
        out.push_back(r);
    }
    return out;
}

bool best_default_route(const std::string& text, DefaultRoute& out)
{
    const std::vector<DefaultRoute> all = parse_proc_net_route(text);
    if (all.empty()) return false;
    size_t best = 0;
    for (size_t i = 1; i < all.size(); ++i)
        if (all[i].metric < all[best].metric) best = i;
    out = all[best];
    return true;
}

std::vector<std::string> parse_resolv_conf(const std::string& text)
{
    std::vector<std::string> out;
    for (const std::string& line : split_lines(text)) {
        const std::vector<std::string> f = fields(line);
        if (f.size() < 2) continue;
        if (f[0].empty() || f[0][0] == '#' || f[0][0] == ';') continue;
        if (f[0] != "nameserver") continue;
        out.push_back(f[1]);
    }
    return out;
}

bool parse_proc_net_dev(const std::string& text, const std::string& ifname,
                        uint64_t& rx_out, uint64_t& tx_out)
{
    const std::string want = ifname + ":";
    for (const std::string& line : split_lines(text)) {
        // The interface name and its first counter can be jammed together
        // when the name is long ("wwan0:12345678"), so the colon is split on
        // before the fields are taken.
        const size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string name = line.substr(0, colon);
        size_t s = 0, e = name.size();
        while (s < e && (name[s] == ' ' || name[s] == '\t')) ++s;
        while (e > s && (name[e - 1] == ' ' || name[e - 1] == '\t')) --e;
        name = name.substr(s, e - s);
        if (name != ifname) continue;

        const std::vector<std::string> f = fields(line.substr(colon + 1));
        // rx: bytes packets errs drop fifo frame compressed multicast
        // tx: bytes packets errs drop fifo colls carrier compressed
        if (f.size() < 16) return false;
        rx_out = std::strtoull(f[0].c_str(), nullptr, 10);
        tx_out = std::strtoull(f[8].c_str(), nullptr, 10);
        return true;
    }
    (void)want;
    return false;
}

int netmask_to_prefix(const std::string& mask)
{
    if (mask.empty()) return -1;

    // Bare prefix form.
    if (mask.find('.') == std::string::npos) {
        long v = 0;
        if (!dec_int(mask, v) || v < 0 || v > 32) return -1;
        return (int)v;
    }

    unsigned a = 0, b = 0, c = 0, d = 0;
    int consumed = 0;
    if (std::sscanf(mask.c_str(), "%u.%u.%u.%u%n", &a, &b, &c, &d, &consumed) != 4) return -1;
    if (consumed != (int)mask.size()) return -1;
    if (a > 255 || b > 255 || c > 255 || d > 255) return -1;

    const uint32_t v = (a << 24) | (b << 16) | (c << 8) | d;
    // Contiguous check: the complement plus one must be a power of two.
    // 255.255.0.255 is not a netmask, and Linux will not accept it either.
    int prefix = 0;
    uint32_t bit = 0x80000000u;
    while (prefix < 32 && (v & bit)) { ++prefix; bit >>= 1; }
    for (int i = prefix; i < 32; ++i)
        if (v & (0x80000000u >> i)) return -1;
    return prefix;
}

std::string prefix_to_netmask(int prefix)
{
    if (prefix < 0 || prefix > 32) return std::string();
    const uint32_t v = (prefix == 0) ? 0u : (0xffffffffu << (32 - prefix));
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
                  (unsigned)((v >> 24) & 0xff), (unsigned)((v >> 16) & 0xff),
                  (unsigned)((v >> 8) & 0xff), (unsigned)(v & 0xff));
    return buf;
}

}} // namespace machino::net
