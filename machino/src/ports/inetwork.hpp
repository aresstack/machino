// Ports: network uplinks and the WiFi radio, platform-neutral.
//
// The point of this layer is that nothing above it may ask "is this WiFi or
// LTE". An uplink is a way to reach the internet with a state, a metric and a
// cost; the video path does not consult it at all -- it just uses the IP stack
// and lets routing decide. That is what makes "stream over LTE" a routing
// change rather than an encoder change.
//
// Deliberately absent here: SSIDs in the uplink type, AT commands, wpa
//_supplicant, hostapd, ttyUSB paths, AIC8800, EC200A. Those belong to one
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

enum class WifiMode : int { Station = 0, AccessPoint };

enum class WifiSecurity : int { Open = 0, Wpa2, Wpa3, Wpa2Wpa3, Wep };

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

struct WifiCapabilities {
    bool present = false;          // a radio exists (driver bound, interface up)
    bool station = false;
    bool access_point = false;     // false when the platform has no hostapd
    bool scanning = false;
    std::string ifname;
    std::string driver;
    std::string ap_unavailable_reason;   // shown in the UI instead of a dead control
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
