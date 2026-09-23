// Board-specific USB power wiring.
//
// This is the one place a pin name belongs. The USB service, the connectivity
// layer and the Linux backend must all stay free of it: they take a
// UsbPowerCapability and a pin resolver and do what they are told.
//
// The values here are MEASURED on the device, not read from a datasheet:
//
//   2026-09-23, Vanhua T40NN board
//     /sys/class/gpio/gpio50 driven high  ->  USB VCC rises from 0 V to 3.3 V
//     GPIO 50 = PB18 under the Ingenic bank mapping (bank 1 * 32 + 18)
//     the switch is an A1SHB P-MOSFET; a transistor between PB18 and its gate
//     inverts the sense, so HIGH turns the rail ON
//     the vendor firmware does exactly this in its own appinstall script
//
// Note what is NOT here: ingenic,drvvbus-gpio = <&gpb 27>. That property comes
// from Ingenic's reference board (shark.dts) and OpenIPC inherited it; on this
// board PB27 is not connected to the switch. Driving it changes nothing, which
// is why the vendor disabled the property in the stock device tree.
#pragma once
#include "core/hw/pin_resolver.hpp"
#include "ports/iusb_host.hpp"
#include <string>

namespace machino { namespace profiles {

// The pin naming scheme this SoC family uses. Owned here so the GPIO backend
// does not have to know it.
const hw::IPinResolver& ingenic_pin_resolver();

// Returns false for a board we have no measurements for -- in which case the
// USB service reports "not switchable" rather than guessing a pin.
bool usb_power_for_board(const std::string& board_id, UsbPowerCapability& out);

}} // namespace machino::profiles
