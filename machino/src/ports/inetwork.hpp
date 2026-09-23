// Ports: network uplinks and the WiFi radio, platform-neutral.
//
// The point of this layer is that nothing above it may ask "is this WiFi or
// LTE". An uplink is a way to reach the internet with a state, a metric and a
// cost; the video path does not consult it at all -- it just uses the IP stack
// and lets routing decide. That is what makes "stream over LTE" a routing
// change rather than an encoder change.
//
// Deliberately absent here: SSIDs in the uplink type, AT commands,
// wpa_supplicant, hostapd, ttyUSB paths, AIC8800, EC200A. Those belong to one
// adapter implementation each.
#pragma once
#include "core/result.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace machino { namespace net {

enum class UplinkType : int { Ethernet = 0, Wifi, Cellular };

enum class LinkState : int {
    Absent = 0,     // no such hardware / no driver
    Down,           // hardware there, not carrying
    Connecting,
    Connected,      // link up and addressed
    Failed,
};

const char* uplink_type_name(UplinkType t);
const char* link_state_name(LinkState s);

struct NetworkInfo {
    std::string ifname;        // "eth0", "wlan0", "wwan0"
    std::string ipv4;          // "" when unaddressed
    std::string netmask;
    std::string gateway;
    std::string dns;
    bool        dhcp = true;
};

struct UplinkMetrics {
    bool     carrier = false;
    int      rssi_dbm = 0;      // 0 = not applicable (Ethernet)
    int      link_mbit = 0;
    uint64_t rx_bytes = 0, tx_bytes = 0;
};

class INetworkUplink {
public:
    virtual ~INetworkUplink() = default;

    // A stable identity, unique within the process, e.g. "wlan0" or "lte1".
    // The TYPE is not an identity: a board can carry two modems or two radios,
    // and a preference list written as "cellular" could then not say which one.
    // Ids survive an interface going away and coming back; ifname may not.
    virtual std::string id() const = 0;

    virtual UplinkType    type() const = 0;
    virtual LinkState     state() const = 0;
    virtual NetworkInfo   info() const = 0;
    virtual UplinkMetrics metrics() const = 0;

    virtual Result connect() = 0;
    virtual Result disconnect() = 0;

    // Does traffic actually reach the outside through this uplink? Kept
    // separate from "connected" because a camera on a WLAN with no uplink is
    // connected and useless, and failover has to tell those apart.
    virtual bool has_internet() const = 0;
};

// ---------------------------------------------------------------- WiFi
//
// An access point is NOT an uplink. A camera serving its own WLAN so a phone
// can reach the setup page is working exactly as intended, and it has no
// internet by design. Treating that as a failed uplink would make the status
// page cry wolf and could make failover tear the AP down.
//
//   WifiAdapter
//     Station mode     -> feeds an INetworkUplink
//     AccessPoint mode -> feeds a local access service, never an uplink
//
// Handing internet from another uplink to AP clients is routing plus NAT and
// a separate feature; it does not change what an AP is.

enum class WifiMode : int { Station = 0, AccessPoint };

// Wpa is WPA1/TKIP, and it has its own value for a reason: it is neither WPA2
// nor open. Folding it into Wpa2 -- which an earlier version did -- tells the
// user their network is something it is not, and hides that they are on a
// cipher that has been broken for years. The UI can then say "WPA (legacy)"
// and mean it.
enum class WifiSecurity : int { Open = 0, Wpa, Wpa2, Wpa3, Wpa2Wpa3, Wep };

const char* wifi_mode_name(WifiMode m);
bool        wifi_mode_parse(const std::string& s, WifiMode& out);
const char* wifi_security_name(WifiSecurity s);
bool        wifi_security_parse(const std::string& s, WifiSecurity& out);

struct WifiNetwork {
    std::string  ssid;
    std::string  bssid;
    int          channel = 0;
    int          rssi_dbm = 0;
    WifiSecurity security = WifiSecurity::Open;
};

struct WifiStationConfig {
    std::string  ssid;
    std::string  passphrase;   // never logged, never returned by the API
    bool         dhcp = true;
    std::string  static_ip, netmask, gateway, dns;
};

struct WifiApConfig {
    std::string  ssid;
    std::string  passphrase;
    WifiSecurity security = WifiSecurity::Wpa2;
    int          channel = 0;          // 0 = auto
    std::string  ipv4 = "192.168.4.1";
    std::string  dhcp_start = "192.168.4.20";
    std::string  dhcp_end   = "192.168.4.100";
    bool         dhcp_server = true;
};

// Two independent questions, kept apart on purpose.
//
// What the RADIO AND ITS DRIVER can do is one thing; what USERSPACE TOOLING is
// installed is another. Installing hostapd does not make a chip AP-capable,
// and an AP-capable chip is useless without it. An earlier version collapsed
// both into one flag, which would have produced a UI that promises AP mode as
// soon as a binary appears -- and then fails at the radio.
struct WifiCapabilities {
    bool present = false;              // a radio exists, driver bound

    // driver / hardware
    bool driver_station = false;
    bool driver_ap = false;            // needs nl80211/iw to answer honestly
    bool driver_scan = false;
    bool driver_concurrent_sta_ap = false;

    // userspace tooling present on this image
    bool wpa_supplicant_available = false;
    bool hostapd_available = false;
    bool dhcp_server_available = false;

    std::string ifname;
    std::string driver;

    // Both sides must agree before a mode is offered.
    bool station_usable() const { return present && driver_station && wpa_supplicant_available; }
    bool ap_usable()      const { return present && driver_ap && hostapd_available; }
    bool scan_usable()    const { return present && driver_scan; }

    // Filled by the adapter with the concrete reason, so the UI can say
    // "hostapd is not in this image" instead of greying out a control.
    std::string ap_unavailable_reason;
};

class IWifiAdapter {
public:
    virtual ~IWifiAdapter() = default;

    virtual WifiCapabilities capabilities() const = 0;

    // Synchronous from the caller's view; an implementation may cache.
    virtual Result scan(std::vector<WifiNetwork>& out) = 0;

    virtual Result start_station(const WifiStationConfig& cfg) = 0;
    virtual Result start_ap(const WifiApConfig& cfg) = 0;
    virtual Result stop() = 0;

    virtual WifiMode  mode() const = 0;
    virtual LinkState state() const = 0;
    virtual bool      station_status(WifiNetwork& out) const = 0;   // what we are on
};

}} // namespace machino::net
