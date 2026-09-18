// Ingenic adapter: IMP SDK 1.3.1 (T40/T40NN) platform port. Sole owner of the
// ISP/sensor/system sessions; frame-source and encoder channels are handed
// out as RAII objects. Bring-up order == member order; tear-down is the
// reverse and fully automatic.
#pragma once
#include "adapters/ingenic/board_wiring.hpp"
#include "adapters/ingenic/imp_sessions.hpp"
#include "ports/iplatform.hpp"
#include <memory>

namespace machino { namespace ingenic {

class IngenicPlatform final : public IPlatform {
public:
    IngenicPlatform(const SensorConfig& sensor, const BoardWiring& wiring);
    ~IngenicPlatform() override;

    const char* name() const override { return "ingenic-imp-1.3.1"; }
    Result bring_up() override;
    void   tear_down() override;

    std::unique_ptr<IFrameSource> create_framesource(int chn, const StreamConfig& sc) override;
    std::unique_ptr<IEncoder>     create_encoder(int chn, const StreamConfig& sc) override;
    Result bind(IFrameSource& fs, IEncoder& enc) override;
    Result unbind(IFrameSource& fs, IEncoder& enc) override;
    int64_t timestamp_us() override;

private:
    SensorConfig  sensor_;
    BoardWiring   wiring_;
    IMPSensorInfo info_{};
    // bring-up order; destroyed in reverse (tuning, system, sensor, isp)
    std::unique_ptr<imp::IspSession>    isp_;
    std::unique_ptr<imp::SensorSession> sensor_session_;
    std::unique_ptr<imp::SystemSession> system_;
    std::unique_ptr<imp::TuningSession> tuning_;
    std::unique_ptr<imp::Binding>       binding_;
};

}} // namespace machino::ingenic
