// Generating a hostapd.conf from a WifiApConfig.
//
// Pure text, no file I/O, no radio -- so every rule below is testable on the
// host, which matters more here than anywhere else in the networking code:
//
//   * hostapd.conf is a `key=value` file with no quoting and no escaping. An
//     SSID or a passphrase containing a newline would inject arbitrary
//     directives, and "ignore_broadcast_ssid=0\nwpa=0" turns a secured access
//     point into an open one. This refuses such a value rather than stripping
//     the character, because a silently shortened passphrase produces an AP
//     nobody can join and no explanation anywhere.
//
//   * hostapd fails to START on an invalid value rather than falling back, so
//     every field is range-checked here. A camera whose access point silently
//     never came up is the worst outcome: it is the recovery path.
//
//   * WPA3/SAE needs a hostapd built with CONFIG_SAE. We cannot see that from
//     here, so an SAE request is reported as unsupported by the caller rather
//     than written into a config that will not start.
#pragma once
#include "ports/inetwork.hpp"
#include <string>

namespace machino { namespace net {

// Generates the file. Returns false with a reason and leaves `out` untouched.
//
// `driver` is hostapd's driver name ("nl80211" everywhere that matters here);
// it is a parameter because a wrong one is the single most common reason
// hostapd refuses to start and hardcoding it would hide that.
bool hostapd_conf_from(const WifiApConfig& cfg, const std::string& ifname,
                       const std::string& driver, std::string& out, std::string& err);

// The udhcpd config for the AP's own pool. Same reasoning: an access point
// whose clients never get an address looks exactly like a broken camera, so
// the pool is generated next to the AP config and validated with it.
bool udhcpd_conf_from(const WifiApConfig& cfg, const std::string& ifname,
                      std::string& out, std::string& err);

// True when the value is safe to put on the right-hand side of a key=value
// line: no newline, no carriage return, no other control character, no NUL.
bool hostapd_value_is_safe(const std::string& v);

// 2.4 GHz -> "g", 5 GHz -> "a", and the channel must belong to the band it
// claims. 0 means "let hostapd choose", which needs a band anyway.
bool hostapd_band_for_channel(int channel, std::string& hw_mode_out);

}} // namespace machino::net
