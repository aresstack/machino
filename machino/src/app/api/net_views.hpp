// JSON views and config mapping for the USB and connectivity surfaces.
//
// Kept out of ApiService and out of the services themselves: the services are
// transport-independent by design, and ApiService is already large. These are
// pure functions over the status structs, which also means the whole API shape
// is testable on the host without a camera, an HTTP server or a socket.
//
// Two rules the tests enforce:
//   * a passphrase never appears in any document produced here
//   * the documents report CAPABILITIES, so a UI can grey out what a board
//     cannot do instead of offering a control that fails
#pragma once
#include "core/json.hpp"
#include "core/net/connectivity.hpp"
#include "core/usb/usb_host_service.hpp"
#include <string>
#include <vector>

namespace machino { namespace api {

// ------------------------------------------------------------------ USB

Json usb_status_json(const usb::UsbStatus& s);
Json usb_capabilities_json(const UsbCapabilities& c);
Json usb_devices_json(const std::vector<UsbDevice>& devices);

// Parse a PATCH body onto an existing config. Returns false with a reason on
// an unknown field or a bad value; the caller then changes nothing.
bool usb_config_from_json(const Json& body, usb::UsbConfig& cfg, std::string& err);
Json usb_config_json(const usb::UsbConfig& cfg);

// machino.conf mapping. Keys are flat and prefixed, like the rest of the file.
void usb_config_to_settings(const usb::UsbConfig& cfg,
                            std::vector<std::pair<std::string, std::string>>& out);
bool usb_config_from_settings(const std::vector<std::pair<std::string, std::string>>& in,
                              usb::UsbConfig& cfg, std::string& err);

// --------------------------------------------------------- connectivity

Json uplink_status_json(const net::UplinkStatus& u);
Json network_json(const std::vector<net::UplinkStatus>& uplinks,
                  const net::UplinkPolicy& policy,
                  const std::string& active_id);
Json policy_json(const net::UplinkPolicy& p);
bool policy_from_json(const Json& body, net::UplinkPolicy& p, std::string& err);

// ------------------------------------------------------------------ WiFi

Json wifi_capabilities_json(const net::WifiCapabilities& c);
Json wifi_scan_json(const std::vector<net::WifiNetwork>& networks);
Json wifi_status_json(const net::WifiCapabilities& caps, net::WifiMode mode,
                      net::LinkState state, const net::WifiNetwork* connected);

// Both refuse an incomplete request rather than filling in a guess: a WiFi
// connect with no SSID must not silently become "join anything".
bool wifi_station_from_json(const Json& body, net::WifiStationConfig& cfg, std::string& err);
bool wifi_ap_from_json(const Json& body, net::WifiApConfig& cfg, std::string& err);

}} // namespace machino::api
