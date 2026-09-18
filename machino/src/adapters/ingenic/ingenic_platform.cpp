#include "adapters/ingenic/ingenic_platform.hpp"
#include "adapters/ingenic/ingenic_encoder.hpp"
#include "adapters/ingenic/ingenic_framesource.hpp"
#include "core/log.hpp"
#include <cstdio>
#include <cstring>

namespace machino { namespace ingenic {

static const char* MOD = "ING_PLAT";

IngenicPlatform::IngenicPlatform(const SensorConfig& sensor, const BoardWiring& wiring)
    : sensor_(sensor), wiring_(wiring) {}

IngenicPlatform::~IngenicPlatform() { tear_down(); }

// Proven bring-up (M1/M2 on T40NN/IMX307, libimp 1.3.1, OpenIPC 4.4.94 tx-isp):
// ISP_Open -> AddSensor+EnableSensor -> System_Init (retry) -> EnableTuning.
// Each step is an RAII session held in a local until every step succeeded;
// an early return destroys the locals in reverse order - no goto chain.
Result IngenicPlatform::bring_up() {
    if (isp_) return Result::ok();

    memset(&info_, 0, sizeof info_);
    snprintf(info_.name, sizeof info_.name, "%s", sensor_.model.c_str());
    info_.cbus_type = TX_SENSOR_CONTROL_INTERFACE_I2C;
    snprintf(info_.i2c.type, sizeof info_.i2c.type, "%s", sensor_.model.c_str());
    info_.i2c.addr           = wiring_.i2c_addr;
    info_.i2c.i2c_adapter_id = wiring_.i2c_bus;
    info_.rst_gpio           = wiring_.reset_gpio;
    info_.pwdn_gpio          = wiring_.pwdn_gpio;
    info_.power_gpio         = -1;
    info_.sensor_id          = 0;
    info_.video_interface    = IMPISP_SENSOR_VI_MIPI_CSI0;
    info_.mclk               = (IMPSensorMclk)wiring_.mclk;
    info_.default_boot       = 0;

    auto isp = std::make_unique<imp::IspSession>();
    if (!isp->ok()) return Result::error(isp->rc());

    auto sensor = std::make_unique<imp::SensorSession>(info_);
    if (!sensor->ok()) return Result::error(sensor->rc());          // isp rolls back

    auto system = std::make_unique<imp::SystemSession>(5, 200);
    if (!system->ok()) return Result::error(system->rc());          // sensor, isp roll back

    auto tuning = std::make_unique<imp::TuningSession>();           // non-fatal

    isp_ = std::move(isp); sensor_session_ = std::move(sensor);
    system_ = std::move(system); tuning_ = std::move(tuning);
    LOGI(MOD, "up: %s %dx%d i2c%d/0x%02x mclk%d rst=%d pwdn=%d tuning=%d", info_.name, sensor_.width,
         sensor_.height, wiring_.i2c_bus, wiring_.i2c_addr, wiring_.mclk, wiring_.reset_gpio,
         wiring_.pwdn_gpio, (int)tuning_->ok());
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

std::unique_ptr<IFrameSource> IngenicPlatform::create_framesource(int chn, const StreamConfig& sc) {
    return IngenicFrameSource::create(chn, sc, sensor_);
}

std::unique_ptr<IEncoder> IngenicPlatform::create_encoder(int chn, const StreamConfig& sc) {
    return IngenicEncoder::create(chn, sc);
}

Result IngenicPlatform::bind(IFrameSource& fs, IEncoder& enc) {
    if (binding_) return Result::busy();
    IMPCell src = { DEV_ID_FS,  fs.channel(),  0 };
    IMPCell dst = { DEV_ID_ENC, enc.channel(), 0 };   // encoder group == channel
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
