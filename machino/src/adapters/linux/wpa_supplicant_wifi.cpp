#include "adapters/linux/wpa_supplicant_wifi.hpp"

#include "core/log.hpp"
#include "core/net/wpa_parse.hpp"

#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>

#define MOD "WIFI"

namespace machino { namespace linuxsys {

namespace {

bool file_exists(const std::string& path)
{
    struct stat st;
    return ::stat(path.c_str(), &st) == 0;
}

bool dir_exists(const std::string& path)
{
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

} // namespace

WpaSupplicantWifi::WpaSupplicantWifi(std::string ifname, WpaPaths paths)
    : ifname_(std::move(ifname)), paths_(std::move(paths)),
      ctrl_(paths_.ctrl_dir + "/" + ifname_) {}

bool WpaSupplicantWifi::have_binary(const char* name) const
{
    for (const std::string& dir : paths_.bin_dirs)
        if (file_exists(dir + "/" + name)) return true;
    return false;
}

bool WpaSupplicantWifi::driver_bound() const
{
    // /sys/class/net/<if>/wireless exists only when the interface is a
    // wireless device with a driver attached. Present but with no
    // wpa_supplicant running is a real and common state on this image, and it
    // is why "the radio is there" and "we can use it" are separate answers.
    return dir_exists(paths_.sys_root + "/class/net/" + ifname_ + "/wireless") ||
           dir_exists(paths_.sys_root + "/class/net/" + ifname_ + "/phy80211");
}

net::WifiCapabilities WpaSupplicantWifi::capabilities() const
{
    net::WifiCapabilities c;
    c.ifname = ifname_;
    c.present = driver_bound();

    // What the DRIVER can do. Without nl80211/iw this image cannot ask it
    // directly, so: station and scan are what every chip we support does and
    // what wpa_supplicant needs anyway. Concurrent STA+AP is NOT claimed --
    // claiming a capability we have not verified produces a control that
    // fails at the radio, which is worse than a greyed-out one with a reason.
    c.driver_station = c.present;
    c.driver_scan    = c.present;
    c.driver_concurrent_sta_ap = false;

    c.wpa_supplicant_available = have_binary("wpa_supplicant") && ctrl_.available();

    // The AP half is the hostapd adapter's to answer, because "can this board
    // run an access point" is a question about a different daemon and a
    // different config file. No adapter wired means no AP support in this
    // build, which is a legitimate configuration -- so it is reported as one
    // rather than as a failure.
    if (ap_) {
        ap_->describe(c);
    } else {
        c.driver_ap = false;
        c.hostapd_available = false;
        c.dhcp_server_available = false;
        c.ap_unavailable_reason = c.present
            ? "this build has no access point support"
            : "no WiFi radio is bound on " + ifname_;
    }

    if (c.present) {
        // Read once, not built from a string we chose: the name here is the
        // one the kernel reports, so a support log says aic8800 or 8189fs
        // rather than whatever we assumed.
        char buf[128] = {0};
        const std::string link = paths_.sys_root + "/class/net/" + ifname_ + "/device/driver";
        const ssize_t n = ::readlink(link.c_str(), buf, sizeof(buf) - 1);
        if (n > 0) {
            std::string s(buf, (size_t)n);
            const size_t slash = s.rfind('/');
            c.driver = (slash == std::string::npos) ? s : s.substr(slash + 1);
        }
    }
    return c;
}

Result WpaSupplicantWifi::do_scan(std::vector<net::WifiNetwork>& out)
{
    // SCAN returns OK at once and finishes later, so SCAN_RESULTS right after
    // it serves the PREVIOUS scan. That is not a bug to paper over with a
    // sleep -- a sleep in the API thread stalls the HTTP loop. The port
    // documents that a scan may be served from cache; the UI polls.
    ctrl_.ok_request("SCAN");

    std::string reply;
    Result rc = ctrl_.request("SCAN_RESULTS", reply, 3000);
    if (!rc.is_ok()) return rc;

    out.clear();
    for (const net::WpaScanEntry& e : net::parse_scan_results(reply)) {
        net::WifiNetwork n;
        n.ssid = e.ssid;
        n.bssid = e.bssid;
        n.channel = net::channel_for_frequency(e.frequency_mhz);
        n.rssi_dbm = e.signal_dbm;
        n.security = (net::WifiSecurity)net::security_from_flags(e.flags);
        out.push_back(n);
    }
    return Result::ok();
}

Result WpaSupplicantWifi::scan(std::vector<net::WifiNetwork>& out)
{
    std::lock_guard<std::mutex> g(m_);
    if (!ctrl_.available()) return Result::unsupported();
    return do_scan(out);
}

Result WpaSupplicantWifi::start_station(const net::WifiStationConfig& cfg)
{
    std::lock_guard<std::mutex> g(m_);
    if (!ctrl_.available()) return Result::unsupported();
    if (cfg.ssid.empty())   return Result::error();

    // The passphrase is turned into a command fragment BEFORE anything is
    // changed. If it cannot be quoted safely the configuration stays exactly
    // as it was, rather than being half torn down.
    std::string psk_arg;
    const bool open = cfg.passphrase.empty();
    if (!open && !net::wpa_quote(cfg.passphrase, psk_arg)) {
        LOGW(MOD, "the passphrase contains a character that cannot be sent to wpa_supplicant");
        return Result::error();
    }

    std::string reply;
    // One network at a time. Leaving the old one enabled would let
    // wpa_supplicant fall back to it, and the user would see a successful
    // "connected" on the wrong SSID.
    ctrl_.ok_request("REMOVE_NETWORK all");

    if (!ctrl_.request("ADD_NETWORK", reply).is_ok()) return Result::error();
    while (!reply.empty() && (reply.back() == '\n' || reply.back() == '\r')) reply.pop_back();
    if (reply.empty() || reply == "FAIL") return Result::error();
    const std::string id = reply;
    for (char c : id) if (c < '0' || c > '9') return Result::error();

    auto set = [&](const std::string& kv) {
        return ctrl_.ok_request("SET_NETWORK " + id + " " + kv);
    };

    // Hex, not a quoted string: an SSID is arbitrary bytes by specification
    // and may contain quotes, backslashes or invalid UTF-8.
    if (!set("ssid " + net::wpa_hex(cfg.ssid))) { ctrl_.ok_request("REMOVE_NETWORK " + id); return Result::error(); }

    // scan_ssid makes a hidden network reachable; it costs a probe request per
    // scan and nothing else.
    set("scan_ssid 1");

    bool ok;
    if (open) {
        ok = set("key_mgmt NONE");
    } else if (cfg.passphrase.size() == 64) {
        // Already the raw PSK. Passing it as a quoted passphrase would make
        // wpa_supplicant derive a second PSK from the hex text.
        bool hex = true;
        for (char c : cfg.passphrase)
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) { hex = false; break; }
        ok = hex ? set("psk " + cfg.passphrase) : set("psk " + psk_arg);
    } else {
        ok = set("psk " + psk_arg);
    }
    if (!ok) { ctrl_.ok_request("REMOVE_NETWORK " + id); return Result::error(); }

    if (!ctrl_.ok_request("ENABLE_NETWORK " + id) ||
        !ctrl_.ok_request("SELECT_NETWORK " + id)) {
        ctrl_.ok_request("REMOVE_NETWORK " + id);
        return Result::error();
    }

    // SAVE_CONFIG is NOT called. The staged-change transaction owns
    // persistence; writing wpa_supplicant.conf here would make a change
    // survive the rollback that is supposed to undo it.
    mode_ = net::WifiMode::Station;
    LOGI(MOD, "station configured on %s (ssid %zu bytes, %s)",
         ifname_.c_str(), cfg.ssid.size(), open ? "open" : "with a key");
    return Result::ok();
}

Result WpaSupplicantWifi::start_ap(const net::WifiApConfig& cfg)
{
    std::lock_guard<std::mutex> g(m_);
    // hostapd is a different daemon with a different config file. Without one
    // wired, pretending wpa_supplicant could do it would produce a "success"
    // and no access point.
    if (!ap_) return Result::unsupported();

    // Get wpa_supplicant off the radio first. DISCONNECT alone is not enough:
    // it drops the current association but leaves the networks enabled, so the
    // supplicant starts scanning and re-associating within seconds and fights
    // hostapd for the PHY. The symptom is an access point that comes up and
    // immediately drops, which looks like a driver fault.
    //
    // This is NOT an assumption that the chip can do STA and AP at once --
    // driver_concurrent_sta_ap is false and nothing here claims otherwise. It
    // is the opposite: assume it cannot, and clear the way. Whether hostapd
    // then actually got the radio is not assumed either; HostapdAp::start()
    // reads the running SSID back and fails if it does not match.
    ctrl_.ok_request("DISABLE_NETWORK all");
    ctrl_.ok_request("DISCONNECT");

    std::string err;
    Result rc = ap_->start(cfg, err);
    if (!rc.is_ok()) {
        LOGW(MOD, "access point not started: %s", err.c_str());
        return rc;
    }
    mode_ = net::WifiMode::AccessPoint;
    return Result::ok();
}

Result WpaSupplicantWifi::stop()
{
    std::lock_guard<std::mutex> g(m_);
    if (mode_ == net::WifiMode::AccessPoint) {
        Result rc = ap_ ? ap_->stop() : Result::unsupported();
        mode_ = net::WifiMode::Station;
        return rc;
    }
    if (!ctrl_.available()) return Result::unsupported();
    ctrl_.ok_request("DISCONNECT");
    return Result::ok();
}

net::WifiMode WpaSupplicantWifi::mode() const
{
    std::lock_guard<std::mutex> g(m_);
    return mode_;
}

net::LinkState WpaSupplicantWifi::state() const
{
    std::lock_guard<std::mutex> g(m_);
    // In AP mode wpa_supplicant's STATUS describes a station that is not
    // associating, which would read as "down" and make the status page cry
    // wolf about a radio that is working exactly as asked.
    if (mode_ == net::WifiMode::AccessPoint)
        return (ap_ && ap_->running()) ? net::LinkState::Connected : net::LinkState::Failed;

    if (!ctrl_.available()) return net::LinkState::Absent;

    std::string reply;
    if (!ctrl_.request("STATUS", reply).is_ok()) return net::LinkState::Absent;

    std::string s;
    if (!net::wpa_status_field(reply, "wpa_state", s)) return net::LinkState::Absent;

    if (s == "COMPLETED")    return net::LinkState::Connected;
    if (s == "DISCONNECTED") return net::LinkState::Down;
    if (s == "INACTIVE")     return net::LinkState::Down;
    if (s == "INTERFACE_DISABLED") return net::LinkState::Absent;
    // SCANNING, AUTHENTICATING, ASSOCIATING, ASSOCIATED, 4WAY_HANDSHAKE,
    // GROUP_HANDSHAKE: all on the way somewhere.
    return net::LinkState::Connecting;
}

bool WpaSupplicantWifi::station_status(net::WifiNetwork& out) const
{
    std::lock_guard<std::mutex> g(m_);
    // In AP mode there is no network we are a station on. Answering with a
    // stale one from before the switch would put an SSID on the status page
    // that the camera is not connected to.
    if (mode_ == net::WifiMode::AccessPoint) return false;
    if (!ctrl_.available()) return false;

    std::string reply;
    if (!ctrl_.request("STATUS", reply).is_ok()) return false;

    std::string state;
    if (!net::wpa_status_field(reply, "wpa_state", state) || state != "COMPLETED") return false;

    net::WifiNetwork n;
    net::wpa_status_field(reply, "ssid", n.ssid);
    net::wpa_status_field(reply, "bssid", n.bssid);

    std::string freq;
    if (net::wpa_status_field(reply, "freq", freq))
        n.channel = net::channel_for_frequency((int)std::strtol(freq.c_str(), nullptr, 10));

    // STATUS carries no RSSI; SIGNAL_POLL does, and it is cheap.
    std::string sig;
    if (ctrl_.request("SIGNAL_POLL", sig).is_ok()) {
        std::string rssi;
        if (net::wpa_status_field(sig, "RSSI", rssi))
            n.rssi_dbm = (int)std::strtol(rssi.c_str(), nullptr, 10);
    }

    // The key is deliberately not reported. STATUS does not carry it and we do
    // not go looking: nothing on this path may hand a passphrase back out.
    out = n;
    return true;
}

}} // namespace machino::linuxsys
