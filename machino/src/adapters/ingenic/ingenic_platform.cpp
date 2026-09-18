#include "adapters/ingenic/ingenic_platform.hpp"
#include "adapters/ingenic/ingenic_encoder.hpp"
#include "adapters/ingenic/ingenic_framesource.hpp"
#include "core/log.hpp"
#include <cstdio>
#include <cstring>

namespace machino { namespace ingenic {

static const char* MOD = "ING_PLAT";

hw::PlatformDefaults IngenicPlatform::platform_defaults() {
    hw::PlatformDefaults d;
    // No GPIO defaults on purpose. No mclk default either: the clock index is
    // board wiring on T40 (this board uses MCLK1, others may not).
    return d;
}

IngenicPlatform::IngenicPlatform(const hw::ResolvedHardware& hw) : hw_(hw) {
    params_ok_ = sensor_params_from(hw_, params_, params_err_);
    if (!params_ok_) LOGE(MOD, "sensor parameters unusable: %s", params_err_.c_str());
}

IngenicPlatform::~IngenicPlatform() { tear_down(); }

// What has actually been established on this platform (M1/M2 hardware runs).
// Everything not exercised stays Unknown - unknown is not "unsupported".
CapabilitySet IngenicPlatform::capabilities() const {
    CapabilitySet c;
    c.video.h264            = Cap::Supported;    // proven: H.264 High 1080p20 via RTSP
    c.video.h265            = Cap::Unknown;      // SDK offers it, not exercised yet
    c.video.max_streams     = -1;                // unknown until measured
    c.sensor.configurable_fps = Cap::Unknown;    // IMP_ISP_Tuning_SetSensorFPS not wired yet
    c.isp.available         = Cap::Supported;
    c.encoder.hardware      = Cap::Supported;
    c.power.isp_clock_control     = Cap::Unknown;
    c.power.encoder_clock_control = Cap::Unknown;
    c.power.cpu_frequency_control = Cap::Unknown;
    c.ai.available          = Cap::Unknown;
    return c;
}

// Proven bring-up (M1/M2/M3 on T40NN/IMX307 board A, libimp 1.3.1, OpenIPC
// 4.4.94 tx-isp): ISP_Open -> AddSensor+EnableSensor -> System_Init (retry)
// -> EnableTuning. RAII sessions in locals; early return rolls back.
Result IngenicPlatform::bring_up() {
    if (isp_) return Result::ok();
    if (!params_ok_) { LOGE(MOD, "refusing bring-up: %s", params_err_.c_str()); return Result::unsupported(); }

    memset(&info_, 0, sizeof info_);
    snprintf(info_.name, sizeof info_.name, "%s", params_.name.c_str());
    info_.cbus_type = TX_SENSOR_CONTROL_INTERFACE_I2C;
    snprintf(info_.i2c.type, sizeof info_.i2c.type, "%s", params_.name.c_str());
    info_.i2c.addr           = params_.i2c_addr;
    info_.i2c.i2c_adapter_id = params_.i2c_bus;
    info_.rst_gpio           = params_.reset_gpio;      // -1 == vendor driver leaves the pin alone
    info_.pwdn_gpio          = params_.pwdn_gpio;
    info_.power_gpio         = params_.power_gpio;
    info_.sensor_id          = 0;
    info_.video_interface    = params_.mipi ? IMPISP_SENSOR_VI_MIPI_CSI0 : IMPISP_SENSOR_VI_DVP;
    info_.mclk               = (IMPSensorMclk)params_.mclk;
    info_.default_boot       = 0;

    auto isp = std::make_unique<imp::IspSession>();
    if (!isp->ok()) return Result::error(isp->rc());
    auto sensor = std::make_unique<imp::SensorSession>(info_);
    if (!sensor->ok()) return Result::error(sensor->rc());
    auto system = std::make_unique<imp::SystemSession>(5, 200);
    if (!system->ok()) return Result::error(system->rc());
    auto tuning = std::make_unique<imp::TuningSession>();

    isp_ = std::move(isp); sensor_session_ = std::move(sensor);
    system_ = std::move(system); tuning_ = std::move(tuning);
    LOGI(MOD, "up: %s i2c%d/0x%02x mclk%d rst=%d pwdn=%d %s tuning=%d", info_.name, params_.i2c_bus,
         params_.i2c_addr, params_.mclk, params_.reset_gpio, params_.pwdn_gpio,
         params_.mipi ? "mipi" : "dvp", (int)tuning_->ok());
    return Result::ok();
}

void IngenicPlatform::tear_down() {
    if (!isp_) return;
    binding_.reset();
    tuning_.reset();
    system_.reset();
    sensor_session_.reset();
    isp_.reset();
    LOGI(MOD, "down");
}

std::unique_ptr<IFrameSource> IngenicPlatform::create_framesource(int chn, const EffectiveStream& sc) {
    return IngenicFrameSource::create(chn, sc);
}

std::unique_ptr<IEncoder> IngenicPlatform::create_encoder(int chn, const EffectiveStream& sc) {
    return IngenicEncoder::create(chn, sc);
}

Result IngenicPlatform::bind(IFrameSource& fs, IEncoder& enc) {
    if (binding_) return Result::busy();
    IMPCell src = { DEV_ID_FS,  fs.channel(),  0 };
    IMPCell dst = { DEV_ID_ENC, enc.channel(), 0 };
    auto b = std::make_unique<imp::Binding>(src, dst);
    if (!b->ok()) return Result::error(b->rc());
    binding_ = std::move(b);
    return Result::ok();
}

Result IngenicPlatform::unbind(IFrameSource&, IEncoder&) {
    binding_.reset();
    return Result::ok();
}

int64_t IngenicPlatform::timestamp_us() { return IMP_System_GetTimeStamp(); }

}} // namespace machino::ingenic
