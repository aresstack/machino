// Ingenic adapter: IMP SDK 1.3.1 (T40 family) platform port. Constructed from
// the resolved, abstract hardware description; the only place that turns it
// into IMPSensorInfo. Sole owner of the ISP/sensor/system sessions.
// M5: live sensor frame rate via ISP tuning (with hardware read-back) and the
// power-control port (read-only clocks, cpufreq probe).
#pragma once
#include "adapters/ingenic/imp_sessions.hpp"
#include "adapters/ingenic/ingenic_jpeg.hpp"
#include "adapters/ingenic/ingenic_image_control.hpp"
#include "adapters/ingenic/ingenic_power_control.hpp"
#include "adapters/ingenic/sensor_params.hpp"
#include "ports/iplatform.hpp"
#include <map>
#include <memory>

namespace machino { namespace ingenic {

class IngenicPlatform final : public IPlatform {
public:
    explicit IngenicPlatform(const hw::ResolvedHardware& hw);
    ~IngenicPlatform() override;

    const char* name() const override { return "ingenic-imp-1.3.1"; }
    CapabilitySet capabilities() const override;
    Result bring_up() override;
    void   tear_down() override;

    std::unique_ptr<IFrameSource> create_framesource(int chn, const EffectiveStream& sc) override;
    std::unique_ptr<IEncoder>     create_encoder(int chn, const EffectiveStream& sc) override;
    std::unique_ptr<IJpegEncoder> create_jpeg(int chn, const JpegParams& p) override;
    Result bind(IFrameSource& fs, IEncoder& enc) override;
    Result unbind(IFrameSource& fs, IEncoder& enc) override;
    int64_t timestamp_us() override;

    Result set_sensor_fps(int fps, int& effective) override;
    Result get_sensor_fps(int& fps) override;
    IPowerControl* power() override { return &power_; }
    IImageControl* image() override { return &image_; }

    static hw::PlatformDefaults platform_defaults();

private:
    hw::ResolvedHardware hw_;
    SensorParams  params_;
    bool          params_ok_ = false;
    std::string   params_err_;
    IMPSensorInfo info_{};
    IngenicPowerControl power_;
    IngenicImageControl image_;
    std::unique_ptr<imp::IspSession>    isp_;
    std::unique_ptr<imp::SensorSession> sensor_session_;
    std::unique_ptr<imp::SystemSession> system_;
    std::unique_ptr<imp::TuningSession> tuning_;
    std::map<int, std::unique_ptr<imp::Binding>> bindings_;   // keyed by encoder channel: one per video unit
};

}} // namespace machino::ingenic
