// Machino core: application configuration. Flat `section.key = value` file.
// Platform-neutral: `platform` selects the adapter, the sensor bus block
// describes *how the board is wired*; the adapter maps it onto vendor structs.
#pragma once
#include <string>

namespace machino {

struct SensorConfig {
    std::string model  = "imx307";
    int width  = 1920;
    int height = 1080;
    int fps    = 20;
};

// Board wiring of the sensor. Defaults are fail-closed: "no such pin" so that
// a board without explicit values never toggles a GPIO by accident.
struct SensorBusConfig {
    int i2c_bus    = 0;
    int i2c_addr   = 0x1a;
    int mclk       = 0;
    int reset_gpio = -1;
    int pwdn_gpio  = -1;
};

enum class RcMode : int { Cbr = 0, Vbr = 1, FixQp = 2 };

struct StreamConfig {
    int    width        = 1920;
    int    height       = 1080;
    int    fps          = 20;
    int    gop          = 40;
    int    bitrate_kbps = 3000;
    int    profile      = 2;      // 0 baseline, 1 main, 2 high
    RcMode rc           = RcMode::Cbr;
    int    qp           = 35;     // FixQp only
    int    buffers      = 2;      // FrameSource video buffers
};

struct RtspConfig {
    int         port = 554;
    std::string path = "/ch0";
};

struct PipelineConfig {
    bool always_on       = false;  // keep the pipeline up without consumers
    int  grace_ms        = 3000;   // tear-down delay after the last consumer left
    int  poll_timeout_ms = 500;    // encoder poll granularity
};

struct LogConfig {
    int  level  = 2;    // 0 err, 1 warn, 2 info, 3 debug
    bool syslog = false;
};

struct AppConfig {
    std::string     platform = "ingenic-t40nn";   // adapter selector
    SensorConfig    sensor;
    SensorBusConfig bus;
    StreamConfig    video;
    RtspConfig      rtsp;
    PipelineConfig  pipeline;
    LogConfig       log;
};

// Loads `path` into `cfg` (fields not present keep their defaults). Returns
// false and fills `err` on I/O errors. Unknown keys / bad values are warnings.
bool load_config(const char* path, AppConfig& cfg, std::string& err);

} // namespace machino
