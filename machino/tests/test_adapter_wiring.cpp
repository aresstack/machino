// Typprüfung der echten Verdrahtung, ohne die Linux-.cpp zu binden.
// Nur Deklarationen: findet Signaturbrüche zwischen den Bausteinen.
#include "adapters/linux/linux_usb_host.hpp"
#include "adapters/linux/sysfs_gpio.hpp"
#include "adapters/linux/wpa_ctrl.hpp"
#include "core/hw/pin_resolver.hpp"
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

void run_adapter_wiring_tests() { }   // die Zusicherungen greifen beim Uebersetzen
