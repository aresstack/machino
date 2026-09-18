#include "adapters/ingenic/ingenic_platform.hpp"
#include "adapters/ingenic/ingenic_encoder.hpp"
#include "adapters/ingenic/ingenic_framesource.hpp"
#include "core/log.hpp"

#include <imp/imp_common.h>
#include <imp/imp_system.h>
#include <cstring>
#include <unistd.h>

namespace machino { namespace ingenic {

static const char* MOD = "ING_PLAT";

IngenicPlatform::IngenicPlatform(const SensorConfig& sensor, const BoardWiring& wiring)
    : sensor_(sensor), wiring_(wiring) {}

IngenicPlatform::~IngenicPlatform() { tear_down(); }

// Proven bring-up order (M1, T40NN/IMX307, libimp 1.3.1 on the OpenIPC
// 4.4.94 tx-isp): ISP_Open -> AddSensor -> EnableSensor -> System_Init
// (retry) -> EnableTuning. The sensor struct carries the board wiring; the
// kernel copies rst/pwdn gpio into the sensor module and pulses the reset.
Result IngenicPlatform::bring_up() {
    if (stage_ != Stage::Down) return Result::ok();

    memset(&info_, 0, sizeof info_);
    snprintf(info_.name, sizeof info_.name, "%s", sensor_.model.c_str());
    info_.cbus_type = TX_SENSOR_CONTROL_INTERFACE_I2C;
    snprintf(info_.i2c.type, sizeof info_.i2c.type, "%s", sensor_.model.c_str());
    info_.i2c.addr            = wiring_.i2c_addr;
    info_.i2c.i2c_adapter_id  = wiring_.i2c_bus;
    info_.rst_gpio            = wiring_.reset_gpio;
    info_.pwdn_gpio           = wiring_.pwdn_gpio;
    info_.power_gpio          = -1;
    info_.sensor_id           = 0;
    info_.video_interface     = IMPISP_SENSOR_VI_MIPI_CSI0;
    info_.mclk                = (IMPSensorMclk)wiring_.mclk;
    info_.default_boot        = 0;

    int rc = IMP_ISP_Open();
    if (rc < 0) { LOGE(MOD, "IMP_ISP_Open failed (%d)", rc); return Result::error(rc); }
    stage_ = Stage::IspOpen;

    rc = IMP_ISP_AddSensor(IMPVI_MAIN, &info_);
    if (rc < 0) { LOGE(MOD, "IMP_ISP_AddSensor(%s i2c%d/0x%02x mclk%d rst=%d pwdn=%d) failed (%d)",
                       info_.name, wiring_.i2c_bus, wiring_.i2c_addr, wiring_.mclk,
                       wiring_.reset_gpio, wiring_.pwdn_gpio, rc);
                  tear_down(); return Result::error(rc); }
    stage_ = Stage::SensorAdded;

    rc = IMP_ISP_EnableSensor(IMPVI_MAIN, &info_);
    if (rc < 0) { LOGE(MOD, "IMP_ISP_EnableSensor failed (%d)", rc); tear_down(); return Result::error(rc); }
    stage_ = Stage::SensorEnabled;

    int tries = 0;
    while ((rc = IMP_System_Init()) < 0) {
        if (++tries >= 5) { LOGE(MOD, "IMP_System_Init failed (%d) after %d tries", rc, tries); tear_down(); return Result::error(rc); }
        LOGW(MOD, "IMP_System_Init failed (%d) - retry %d", rc, tries);
        usleep(200 * 1000);
    }
    stage_ = Stage::SystemInit;

    rc = IMP_ISP_EnableTuning();
    if (rc < 0) LOGW(MOD, "IMP_ISP_EnableTuning failed (%d) - continuing without tuning", rc);
    else stage_ = Stage::TuningOn;

    LOGI(MOD, "up: %s %dx%d i2c%d/0x%02x mclk%d rst=%d pwdn=%d", info_.name, sensor_.width, sensor_.height,
         wiring_.i2c_bus, wiring_.i2c_addr, wiring_.mclk, wiring_.reset_gpio, wiring_.pwdn_gpio);
    return Result::ok();
}

// Reverse order. Every stage is undone exactly once.
void IngenicPlatform::tear_down() {
    switch (stage_) {
        case Stage::TuningOn:      IMP_ISP_DisableTuning();                 /* fallthrough */
        case Stage::SystemInit:    IMP_System_Exit();                       /* fallthrough */
        case Stage::SensorEnabled: IMP_ISP_DisableSensor(IMPVI_MAIN);       /* fallthrough */
        case Stage::SensorAdded:   IMP_ISP_DelSensor(IMPVI_MAIN, &info_);   /* fallthrough */
        case Stage::IspOpen:       IMP_ISP_Close();                         /* fallthrough */
        case Stage::Down:          break;
    }
    if (stage_ != Stage::Down) LOGI(MOD, "down");
    stage_ = Stage::Down;
}

std::unique_ptr<IFrameSource> IngenicPlatform::create_framesource(int chn, const StreamConfig& sc) {
    return IngenicFrameSource::create(chn, sc, sensor_);
}

std::unique_ptr<IEncoder> IngenicPlatform::create_encoder(int chn, const StreamConfig& sc) {
    return IngenicEncoder::create(chn, sc);
}

Result IngenicPlatform::bind(IFrameSource& fs, IEncoder& enc) {
    IMPCell src = { DEV_ID_FS,  fs.channel(),  0 };
    IMPCell dst = { DEV_ID_ENC, enc.channel(), 0 };   // encoder group == channel
    int rc = IMP_System_Bind(&src, &dst);
    if (rc < 0) { LOGE(MOD, "IMP_System_Bind fs%d->enc%d failed (%d)", fs.channel(), enc.channel(), rc); return Result::error(rc); }
    return Result::ok();
}

Result IngenicPlatform::unbind(IFrameSource& fs, IEncoder& enc) {
    IMPCell src = { DEV_ID_FS,  fs.channel(),  0 };
    IMPCell dst = { DEV_ID_ENC, enc.channel(), 0 };
    int rc = IMP_System_UnBind(&src, &dst);
    if (rc < 0) { LOGW(MOD, "IMP_System_UnBind failed (%d)", rc); return Result::error(rc); }
    return Result::ok();
}

int64_t IngenicPlatform::timestamp_us() { return IMP_System_GetTimeStamp(); }

}} // namespace machino::ingenic
