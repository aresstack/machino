#include "profiles/usb_profiles.hpp"

namespace machino { namespace profiles {

const hw::IPinResolver& ingenic_pin_resolver()
{
    // Six banks of 32: PA0..PF31, so PB18 = 1 * 32 + 18 = 50.
    static const hw::BankPinResolver r(32, 6);
    return r;
}

bool night_defaults_for_board(const std::string& board_id, NightDefaults& out)
{
    // Stock-Belegung dieses Boards, aus ovfs_boardsystem disassembliert
    // (HalPerip_Init: base 96 + idx 22/23 -> PD22/PD23; InitIrLight:
    // 32+17 -> PB17 als Eingang = Lichtsensor). Andere T40-Boards erben
    // das NICHT durch den SoC -- nur durch eigene Messung/Analyse.
    if (board_id == "t40nn-imx307-board-a") {
        out.ircut_pin1 = "PD22";
        out.ircut_pin2 = "PD23";
        out.light_sensor_pin = "PB17";
        return true;
    }
    return false;
}

bool usb_power_for_board(const std::string& board_id, UsbPowerCapability& out)
{
    // The board this was measured on. Other T40NN cameras may well wire the
    // switch to a different pin, so they do not inherit this by being a T40.
    if (board_id == "t40nn-imx307-board-a") {
        UsbPowerCapability p;
        p.switchable = true;
        p.default_pin = "PB18";
        p.default_active_high = true;   // measured: high -> 3.3 V at the connector
        p.voltage_mv = 3300;            // measured: 3.3 V, NOT 5 V
        p.allowed_pins = {"PB18"};      // everything else needs the expert flag
        out = p;
        return true;
    }

    // Unknown board: say "cannot switch" rather than offering a pin we have
    // never measured. Guessing here would hand a user a control that drives an
    // arbitrary GPIO on their camera.
    return false;
}

}} // namespace machino::profiles
