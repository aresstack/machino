#include "core/net/wpa_parse.hpp"
#include "ports/inetwork.hpp"

#include <cstdlib>

namespace machino { namespace net {

std::vector<WpaScanEntry> parse_scan_results(const std::string& text)
{
    std::vector<WpaScanEntry> out;
    size_t pos = 0;
    bool first = true;

    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        if (nl == std::string::npos) pos = text.size() + 1; else pos = nl + 1;

        if (first) { first = false; continue; }      // "bssid / frequency / ..." header
        if (line.empty()) continue;
        while (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;

        // bssid \t frequency \t signal \t flags \t ssid
        // The SSID is last on purpose: it may contain spaces, and splitting on
        // tabs keeps that intact. It may also be empty (hidden network).
        std::string f[5];
        size_t fi = 0, start = 0;
        for (size_t i = 0; i <= line.size() && fi < 5; ++i) {
            if (i == line.size() || line[i] == '\t') {
                f[fi++] = line.substr(start, i - start);
                start = i + 1;
            }
        }
        if (fi < 4) continue;                        // malformed: drop it

        WpaScanEntry e;
        e.bssid = f[0];
        e.frequency_mhz = std::atoi(f[1].c_str());
        e.signal_dbm = std::atoi(f[2].c_str());
        e.flags = f[3];
        e.ssid = (fi >= 5) ? f[4] : std::string();
        if (e.bssid.size() != 17) continue;          // "aa:bb:cc:dd:ee:ff"
        out.push_back(e);
    }
    return out;
}

int channel_for_frequency(int mhz)
{
    if (mhz == 2484) return 14;                                  // Japan, 802.11b only
    if (mhz >= 2412 && mhz <= 2472 && (mhz - 2412) % 5 == 0) return 1 + (mhz - 2412) / 5;
    if (mhz >= 5180 && mhz <= 5825 && (mhz - 5000) % 5 == 0) return (mhz - 5000) / 5;
    return 0;
}

int security_from_flags(const std::string& flags)
{
    const bool sae  = flags.find("SAE") != std::string::npos;
    const bool wpa2 = flags.find("WPA2") != std::string::npos;
    const bool wpa1 = flags.find("[WPA-") != std::string::npos || flags.find("[WPA]") != std::string::npos;
    const bool wep  = flags.find("WEP") != std::string::npos;

    if (sae && wpa2)  return (int)WifiSecurity::Wpa2Wpa3;
    if (sae)          return (int)WifiSecurity::Wpa3;
    if (wpa2)         return (int)WifiSecurity::Wpa2;
    // WPA1 gets its own value. Reporting it as WPA2 would tell the user their
    // network is something it is not, and hide that they are on TKIP.
    if (wpa1)         return (int)WifiSecurity::Wpa;
    if (wep)          return (int)WifiSecurity::Wep;
    return (int)WifiSecurity::Open;
}

bool wpa_status_field(const std::string& status, const std::string& key, std::string& out)
{
    const std::string needle = key + "=";
    size_t pos = 0;
    while (pos < status.size()) {
        size_t nl = status.find('\n', pos);
        std::string line = status.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = (nl == std::string::npos) ? status.size() : nl + 1;
        while (!line.empty() && line.back() == '\r') line.pop_back();

        if (line.compare(0, needle.size(), needle) == 0) {
            out = line.substr(needle.size());
            return true;
        }
    }
    return false;
}

std::string wpa_hex(const std::string& raw)
{
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(raw.size() * 2);
    for (unsigned char c : raw) {
        out += kHex[(c >> 4) & 0xf];
        out += kHex[c & 0xf];
    }
    return out;
}

bool wpa_quote(const std::string& raw, std::string& out)
{
    std::string s = "\"";
    for (unsigned char c : raw) {
        // Refuse, do not strip. A passphrase quietly shortened by one
        // character produces a camera that cannot associate, and nothing
        // anywhere says why.
        if (c < 0x20 || c == 0x7f) return false;
        if (c == '"' || c == '\\') s += '\\';
        s += (char)c;
    }
    s += '"';
    out = s;
    return true;
}

}} // namespace machino::net
