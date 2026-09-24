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
#include "core/cellular/cellular_config.hpp"
#include "core/cellular/cellular_status.hpp"
#include "core/cellular/ecm_link.hpp"
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

// machino.conf mapping, same shape as the USB one. The order list is one
// comma-separated value rather than indexed keys: it is read and written as a
// whole, and indexed keys leave orphans behind when the list gets shorter.
void policy_to_settings(const net::UplinkPolicy& p,
                        std::vector<std::pair<std::string, std::string>>& out);
bool policy_from_settings(const std::vector<std::pair<std::string, std::string>>& in,
                          net::UplinkPolicy& p, std::string& err);

// ------------------------------------------------------------------ WiFi

Json wifi_capabilities_json(const net::WifiCapabilities& c);
Json wifi_scan_json(const std::vector<net::WifiNetwork>& networks);
Json wifi_status_json(const net::WifiCapabilities& caps, net::WifiMode mode,
                      net::LinkState state, const net::WifiNetwork* connected);

// Both refuse an incomplete request rather than filling in a guess: a WiFi
// connect with no SSID must not silently become "join anything".
bool wifi_station_from_json(const Json& body, net::WifiStationConfig& cfg, std::string& err);
bool wifi_ap_from_json(const Json& body, net::WifiApConfig& cfg, std::string& err);

// -------------------------------------------------------------- Mobilfunk

// Der Status als Dokument. Werte, die das Modem nicht geliefert hat, werden
// null -- nicht 0. Eine Anzeige kann "-" zeigen; aus einer 0 macht sie
// "-0 dBm", und das sieht aus wie eine Messung.
Json cellular_status_json(const cellular::CellularStatus& s);

// Die Konfiguration, OHNE Geheimnisse. Statt PIN und Passwort steht dort, ob
// eines gesetzt ist -- das braucht die Oberflaeche, um "gespeichert" von
// "leer" zu unterscheiden, und mehr braucht sie nicht.
Json cellular_config_json(const cellular::CellularConfig& c);

// Die bekannten Anbieterkonfigurationen als Vorschlaege, mit dem Grund dabei.
Json cellular_presets_json();

// Mobilfunk so, wie die Netzwerkseite ihn braucht: Modemzustand UND Uplink in
// einem Dokument.
//
// Zwei Sichten, die auseinanderzuhalten der ganze Punkt ist. `modem`, `sim`,
// `network` und `radio` beschreiben das Geraet; `dataLink`, `interface`,
// `address` und `internet` beschreiben die Verbindung. Ein Modem kann
// hervorragend eingebucht sein und trotzdem kein Byte transportieren, und wer
// nur eine der beiden Haelften anzeigt, kann diesen Fall nicht benennen.
//
// `state` ist dabei der UPLINK-Zustand, nicht der des Modems -- dieselbe
// Skala wie bei Ethernet und WLAN, damit eine Oberflaeche die drei
// nebeneinander darstellen kann, ohne Mobilfunk gesondert zu behandeln.
Json cellular_network_json(const cellular::CellularStatus& s,
                           const cellular::CellularConfig& c,
                           const cellular::CellularLinkState& link,
                           net::LinkState uplink_state,
                           bool internet);

// Ein unbekanntes Feld ist ein Fehler. Ein leerer String bei password/simPin
// heisst "loeschen"; ein FEHLENDES Feld heisst "nicht anfassen" -- sonst
// loescht jede Teiländerung der Oberflaeche die PIN mit.
bool cellular_config_from_json(const Json& body, cellular::CellularConfig& cfg, std::string& err);

void cellular_config_to_settings(const cellular::CellularConfig& cfg,
                                 std::vector<std::pair<std::string, std::string>>& out);
bool cellular_config_from_settings(const std::vector<std::pair<std::string, std::string>>& in,
                                   cellular::CellularConfig& cfg, std::string& err);

}} // namespace machino::api
