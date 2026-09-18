#include "profiles/builtin_profiles.hpp"

namespace machino { namespace profiles {

void register_builtin(hw::Registry& reg) {
    // ---- platforms ---------------------------------------------------------
    reg.add_platform(hw::PlatformDescriptor{"ingenic", "t40", "t40nn"});   // id "ingenic-t40nn"
    reg.add_platform(hw::PlatformDescriptor{"ingenic", "t40", "t40n"});    // id "ingenic-t40n" (untested)

    // ---- sensors -----------------------------------------------------------
    // Only modes that were actually run on hardware are listed. Add a mode
    // when it has been verified, not from a datasheet.
    hw::SensorDescriptor imx307;
    imx307.model         = "imx307";
    imx307.interface     = hw::SensorInterface::MipiCsi;
    imx307.native_width  = 1920;
    imx307.native_height = 1080;
    imx307.modes         = { hw::SensorMode{1920, 1080, 20} };   // mode 0: verified 2026-09-18 (M1/M2)
    reg.add_sensor(imx307);

    // ---- board profiles ----------------------------------------------------
    // Board A: the T40NN + IMX307 camera used for M1/M2. HARDWARE VERIFIED.
    // These pin numbers are this board's wiring. Another T40NN/IMX307 camera
    // may be wired differently - do not assume 91/0 elsewhere.
    hw::BoardProfile a;
    a.board_id          = "t40nn-imx307-board-a";
    a.platform          = "ingenic-t40nn";
    a.sensor            = "imx307";
    a.wiring.i2c_bus    = 1;
    a.wiring.i2c_addr   = 0x1a;
    a.wiring.mclk       = 1;
    a.wiring.reset_gpio = 91;
    a.wiring.pwdn_gpio  = 0;
    a.default_mode      = hw::SensorMode{1920, 1080, 20};
    a.hardware_verified = true;
    a.notes             = "OpenIPC 4.4.94 tx-isp, libimp 1.3.1; verified H.264/RTSP 2026-09-18 (M1 timps, M2/M3 machino)";
    reg.add_board(a);
}

}} // namespace machino::profiles
