// Ingenic adapter: IMP SDK 1.3.1 (T40 family) platform port. Constructed from
// the resolved, abstract hardware description; the only place that turns it
// into IMPSensorInfo. Sole owner of the ISP/sensor/system sessions.
#pragma once
#include "adapters/ingenic/imp_sessions.hpp"
#include "adapters/ingenic/sensor_params.hpp"
#include "ports/iplatform.hpp"
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
    Result bind(IFrameSource& fs, IEncoder& enc) override;
    Result unbind(IFrameSource& fs, IEncoder& enc) override;
    int64_t timestamp_us() override;

    // Conservative defaults this adapter declares to the resolver.
    static hw::PlatformDefaults platform_defaults();

private:
    hw::ResolvedHardware hw_;
    SensorParams  params_;
    bool          params_ok_ = false;
    std::string   params_err_;
    IMPSensorInfo info_{};
    std::unique_ptr<imp::IspSession>    isp_;
    std::unique_ptr<imp::SensorSession> sensor_session_;
    std::unique_ptr<imp::SystemSession> system_;
    std::unique_ptr<imp::TuningSession> tuning_;
    std::unique_ptr<imp::Binding>       binding_;
};

}} // namespace machino::ingenic
