// Typprüfung der echten Verdrahtung, ohne die Linux-.cpp zu binden.
// Nur Deklarationen: findet Signaturbrüche zwischen den Bausteinen.
#include "adapters/linux/linux_usb_host.hpp"
#include "adapters/linux/sysfs_gpio.hpp"
#include "adapters/linux/wpa_ctrl.hpp"
#include "core/hw/pin_resolver.hpp"
#include "app/api/net_api.hpp"
#include "adapters/linux/linux_ethernet_uplink.hpp"
#include "adapters/linux/wifi_station_uplink.hpp"
#include "adapters/linux/wpa_supplicant_wifi.hpp"
#include "core/net/connectivity.hpp"
#include "core/net/network_txn.hpp"
#include "core/usb/usb_host_service.hpp"
#include "profiles/usb_profiles.hpp"
#include <type_traits>

using namespace machino;

// So wuerde main() es bauen.
static_assert(std::is_constructible<linuxsys::SysfsGpio,
                  const hw::IPinResolver&, std::string, bool>::value,
              "SysfsGpio nimmt Resolver, Wurzel, unexport-Flag");

static_assert(std::is_constructible<linuxsys::LinuxUsbHostBackend,
                  IGpioController&, UsbPowerCapability, std::string, std::string>::value,
              "LinuxUsbHostBackend nimmt GPIO, Boardprofil, sysfs-Pfad, Controllername");

static_assert(std::is_constructible<usb::UsbHostService, IUsbHostBackend&>::value,
              "UsbHostService nimmt ein Backend");

static_assert(std::is_constructible<linuxsys::WpaCtrl, std::string>::value, "WpaCtrl");

// Die Adapter muessen die Ports wirklich erfuellen, nicht nur so aussehen.
static_assert(std::is_base_of<IGpioController, linuxsys::SysfsGpio>::value, "");
static_assert(std::is_base_of<IUsbHostBackend, linuxsys::LinuxUsbHostBackend>::value, "");
static_assert(!std::is_abstract<linuxsys::SysfsGpio>::value,
              "SysfsGpio muss instanziierbar sein - sonst fehlt eine Override");
static_assert(!std::is_abstract<linuxsys::LinuxUsbHostBackend>::value,
              "LinuxUsbHostBackend muss instanziierbar sein");
static_assert(!std::is_abstract<NullUsbHostBackend>::value, "");

// Der Resolver aus dem Boardprofil passt an das GPIO-Backend.
static_assert(std::is_base_of<hw::IPinResolver, hw::BankPinResolver>::value, "");
static_assert(std::is_base_of<hw::IPinResolver, hw::NumericPinResolver>::value, "");

// AP36 Uplinks und WLAN-Adapter. Auch diese .cpp brauchen Linux-Header, also
// wird hier wenigstens festgenagelt, dass sie die Ports wirklich erfuellen und
// dass main() sie so bauen kann.
static_assert(std::is_base_of<net::INetworkUplink, linuxsys::LinuxEthernetUplink>::value, "");
static_assert(std::is_base_of<net::INetworkUplink, linuxsys::WifiStationUplink>::value, "");
static_assert(std::is_base_of<net::IWifiAdapter,   linuxsys::WpaSupplicantWifi>::value, "");
static_assert(!std::is_abstract<linuxsys::LinuxEthernetUplink>::value,
              "LinuxEthernetUplink muss instanziierbar sein - sonst fehlt eine Override");
static_assert(!std::is_abstract<linuxsys::WifiStationUplink>::value, "");
static_assert(!std::is_abstract<linuxsys::WpaSupplicantWifi>::value, "");

static_assert(std::is_constructible<linuxsys::LinuxEthernetUplink,
                  std::string, std::string, std::string>::value,
              "LinuxEthernetUplink nimmt ifname, /sys, /proc");
static_assert(std::is_constructible<linuxsys::LinuxEthernetUplink>::value,
              "und hat Defaults fuer den Normalfall");
static_assert(std::is_constructible<linuxsys::WifiStationUplink,
                  net::IWifiAdapter&, std::string, std::string, std::string>::value,
              "WifiStationUplink nimmt den Adapter plus Pfade");
static_assert(std::is_constructible<linuxsys::WpaSupplicantWifi,
                  std::string, linuxsys::WpaSupplicantWifi::Paths>::value,
              "WpaSupplicantWifi nimmt ifname plus Pfadsatz");
static_assert(std::is_constructible<linuxsys::LinuxNetif, std::string>::value, "");

// HttpServer ruft NetApiService::handle() und set_net_api() auf. http_server.cpp
// braucht arpa/inet.h und laesst sich hier nicht uebersetzen, also wird
// wenigstens die Aufrufstelle hier festgenagelt: aendert sich die Signatur,
// bricht dieser Test und nicht erst der Cross-Build.
static_assert(std::is_same<decltype(std::declval<api::NetApiService&>().handle(
                  std::declval<const std::string&>(), std::declval<const std::string&>(),
                  std::declval<const std::string&>(), std::declval<api::Response&>())),
                  bool>::value,
              "NetApiService::handle(method, path, body, out) -> bool");
static_assert(std::is_same<decltype(std::declval<api::NetApiService&>().tick()), bool>::value,
              "NetApiService::tick() -> bool");
static_assert(std::is_constructible<api::NetApiService, api::NetApiService::Deps>::value,
              "NetApiService nimmt genau ein Deps-Aggregat");

void run_adapter_wiring_tests() { }   // die Zusicherungen greifen beim Uebersetzen
